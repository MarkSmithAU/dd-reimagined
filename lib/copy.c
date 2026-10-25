/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later

    Follows the structure of GNU dd's dd_copy(): with bs= each input block
    is written as it was read; otherwise input is gathered into obs sized
    output blocks.
*/
#include "fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef enum step { STEP_OK, STEP_FAIL, STEP_CANCEL } step;

#define HASH_PATH  1040                 /* a .hashes path */
#define VBUF_SIZE  (1u << 20)           /* verifying and checking read this much at a time */
#define VALIGN     65536u               /* offsets of unbuffered reads of a file */
#define SPARSE_GAP (64u << 10)          /* sparse reading reads across smaller free gaps */
#define XZ_RESERVE ((uint64_t)4 << 30)  /* free memory compressing leaves to the rest, */
#define XZ_RESERVE_SWAP ((uint64_t)2 << 30)  /* or this, when there is a page file to fall back on */

typedef struct job {
    const dd_options   *opt;
    const dd_callbacks *cb;
    dd_cancel          *cancel;
    dd_stats           *st;
    dd_endpoint         in, out;
    DWORD               ibs, obs;
    bool                direct;        /* bs= given: no reblocking */
    uint8_t            *ibuf, *obuf, *sbuf;
    DWORD               oc;            /* bytes waiting in obuf */
    int                 saved_char;    /* conv=swab carry, -1 if none */
    bool                warn_partial;
    DWORD               prev_nread;
    LARGE_INTEGER       freq, start;
    double              last_progress;
    const dd_layout    *layout;        /* fit: the input's partition table */
    dd__gpt_fix         gpt;           /* fit: GPT rebuilt for the output */
    bool                have_gpt;
    int64_t             gpt_file_end;  /* fit to a file: its final length, else 0 */
    bool                gpt_inline;    /* fit to a compressed stream or a hashed file:
                                          patch as it passes */
    int64_t             clear_off, clear_len;  /* fit, an MBR image to a disk: the disk's
                                          end, where an old backup GPT may be */

    /* the input's .hashes file, checked as the input is read */
    dd__sha256         *in_hash;       /* the data read (uncompressed) */
    dd__sha256         *in_raw_hash;   /* an .xz input's file */
    const uint8_t      *in_want, *in_raw_want;  /* in in_list; NULL = not checked */
    wchar_t             in_hashes[HASH_PATH];
    dd__hash_list       in_list;

    /* the output's own .hashes file, and verify */
    dd__sha256         *out_hash;      /* the data written (uncompressed) */
    dd__sha256         *out_raw_hash;  /* a compressed output's file */
    bool                make_hashes;
    bool                hashes_shared;  /* out_hashes is the input's hash file too */
    bool                out_is_file;   /* a file opened by dd (compression makes it a stream) */
    wchar_t             out_hashes[HASH_PATH];
    int64_t             out_start;     /* where the data begins in the output */
    int64_t             out_bytes;
    bool                out_final;     /* out_digest and out_raw_digest are set */
    uint8_t             out_digest[32], out_raw_digest[32];

    uint8_t            *vbuf;          /* VBUF_SIZE, for verify and checking */
    LARGE_INTEGER       phase_start;

    uint64_t            xz_budget;     /* memory compressing and decompressing may use */
    bool                xz_budget_floor; /* no more than xz_reserve was free */
    uint64_t            xz_free;       /* free memory when dd started */
    uint64_t            xz_reserve;    /* of it, kept back for everything else */

    dd__fsmap          *fsmap;         /* sparse_read: the input's used space */
    bool                sparse_stream; /* a compressed input, read in order: its
                                          sparse map's free space is zeroed */
    uint8_t             map_sha[32];   /* the .xz file the sparse map is for */
    const wchar_t      *raw_want_from; /* where in_raw_want comes from: the hash
                                          file, or the sparse map */
    wchar_t             map_path[HASH_PATH];
} job;

void dd_cancel_init(dd_cancel *c)
{
    c->requested = 0;
    InitializeSRWLock(&c->lock);
    c->thread = NULL;
}

void dd_cancel_request(dd_cancel *c)
{
    InterlockedExchange(&c->requested, 1);
    AcquireSRWLockShared(&c->lock);
    if (c->thread)
        CancelSynchronousIo(c->thread);   /* wakes a read blocked on a pipe */
    ReleaseSRWLockShared(&c->lock);
}

static void cancel_attach(dd_cancel *c)
{
    HANDLE t;
    if (!c)
        return;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t,
                         0, FALSE, DUPLICATE_SAME_ACCESS))
        t = NULL;
    AcquireSRWLockExclusive(&c->lock);
    c->thread = t;
    ReleaseSRWLockExclusive(&c->lock);
}

static void cancel_detach(dd_cancel *c)
{
    HANDLE t;
    if (!c)
        return;
    AcquireSRWLockExclusive(&c->lock);
    t = c->thread;
    c->thread = NULL;
    ReleaseSRWLockExclusive(&c->lock);
    if (t)
        CloseHandle(t);
}

static bool cancelled(const job *j)
{
    return j->cancel && j->cancel->requested;
}

static double elapsed(const job *j)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - j->start.QuadPart) / (double)j->freq.QuadPart;
}

static double phase_elapsed(const job *j)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - j->phase_start.QuadPart) / (double)j->freq.QuadPart;
}

/* The .xz side of the copy: the output's when compressing, else the input's */
static int64_t xz_bytes(const job *j)
{
    if (j->out.xz)
        return dd__xz_file_bytes(j->out.xz);
    if (j->in.xz)
        return dd__xz_file_bytes(j->in.xz);
    return -1;
}

static void progress_tick(job *j)
{
    double now;
    if (!j->cb || !j->cb->progress || !j->cb->progress_interval_ms)
        return;
    now = elapsed(j);
    if (now - j->last_progress >= j->cb->progress_interval_ms / 1000.0) {
        j->last_progress = now;
        if (j->st->phase == DD_PHASE_COPY) {
            j->st->elapsed = now;
            j->st->xz_bytes = xz_bytes(j);
            j->st->xz_done = j->out.xz ? dd__xz_compressed_in(j->out.xz) : -1;
        } else
            j->st->phase_elapsed = phase_elapsed(j);
        j->cb->progress(j->cb->ctx, j->st);
    }
}

/* While the encoder compresses what it still holds */
static void finish_tick(void *ctx)
{
    progress_tick(ctx);
}

/* Verifying or checking: the copy's own statistics stay as they were. */
static void start_phase(job *j, dd_phase phase, int64_t total)
{
    j->st->phase = phase;
    j->st->phase_done = 0;
    j->st->phase_total = total;
    j->st->phase_elapsed = 0;
    QueryPerformanceCounter(&j->phase_start);
}

static void warn(const job *j, const wchar_t *fmt, const wchar_t *name)
{
    if (j->opt->status != DD_STATUS_NONE)
        dd__msg(j->cb, DD_MSG_WARNING, fmt, name);
}

static void report(const job *j, const wchar_t *what, const dd_endpoint *ep, DWORD e)
{
    wchar_t m[256];
    dd_errstr(e, m, 256);
    if (e == ERROR_ACCESS_DENIED && ep->device_path)
        wcsncat_s(m, 256, L" (raw device access needs an elevated prompt)", _TRUNCATE);
    dd__msg(j->cb, DD_MSG_ERROR, L"%ls '%ls': %ls", what, ep->name, m);
}

static bool mul64(int64_t a, int64_t b, int64_t *r)
{
    if (a < 0 || b < 0 || (a != 0 && b > INT64_MAX / a))
        return false;
    *r = a * b;
    return true;
}

static bool all_zero(const uint8_t *p, DWORD n)
{
    return n == 0 || (p[0] == 0 && memcmp(p, p + 1, n - 1) == 0);
}

/* ---- reading ----------------------------------------------------------- */

/* Reads [a, b) of the input to dst.  At an error, or if the input ends
   early, *reached is set to where the input stopped. */
static DWORD read_span(job *j, uint8_t *dst, int64_t a, int64_t b, int64_t *reached)
{
    DWORD e = 0, n;

    j->in.pos = a;
    while (j->in.pos < b) {
        e = ep_read(&j->in, dst + (j->in.pos - a), (DWORD)(b - j->in.pos), &n);
        if (e || n == 0)
            break;
    }
    if (j->in.pos < b)
        *reached = j->in.pos;
    return e;
}

/* sparse_read: reads only the parts of [pos, pos + len) that are in use,
   and zeros the rest.  A free gap shorter than SPARSE_GAP is read along
   with its neighbours, as one larger read is faster than two small ones,
   and zeroed afterwards: what comes out does not depend on how it was read. */
static DWORD sparse_read(job *j, uint8_t *buf, DWORD len, DWORD *got)
{
    dd_endpoint *in = &j->in;
    int64_t start = in->pos, end, reached, p, a = -1, b = -1;
    DWORD e = 0;
    bool used;

    *got = 0;
    if (start >= in->size)
        return 0;
    if ((int64_t)len > in->size - start)
        len = (DWORD)(in->size - start);
    end = reached = start + len;
    for (p = start; p < end && reached == end; ) {
        int64_t n = dd__fsmap_run(j->fsmap, p, end - p, &used);
        if (used) {
            if (a < 0)
                a = p;
            b = p + n;
        } else if (a >= 0 && n >= SPARSE_GAP) {
            e = read_span(j, buf + (a - start), a, b, &reached);
            a = -1;
        }
        p += n;
    }
    if (a >= 0 && reached == end)
        e = read_span(j, buf + (a - start), a, b, &reached);

    for (p = start; p < reached; ) {
        int64_t n = dd__fsmap_run(j->fsmap, p, reached - p, &used);
        if (!used) {
            memset(buf + (p - start), 0, (size_t)n);
            j->st->bytes_unused += n;
        }
        p += n;
    }
    in->pos = reached;
    *got = (DWORD)(reached - start);
    return e;
}

static DWORD in_read(job *j, uint8_t *buf, DWORD len, DWORD *got)
{
    return j->fsmap && !j->sparse_stream ? sparse_read(j, buf, len, got) : ep_read(&j->in, buf, len, got);
}

/* A compressed input is read in order: what its sparse map says is free
   is zeroed after reading (and after hashing, so its hashes still check). */
static void zero_free(job *j, uint8_t *buf, int64_t at, DWORD n)
{
    int64_t p = at, end = at + n;

    while (p < end) {
        bool used;
        int64_t k = dd__fsmap_run(j->fsmap, p, end - p, &used);
        if (!used) {
            memset(buf + (p - at), 0, (size_t)k);
            j->st->bytes_unused += k;
        }
        p += k;
    }
}

static DWORD read_block(job *j, uint8_t *buf, DWORD len, DWORD *got)
{
    int64_t at = j->in.pos;
    DWORD e = 0;

    if (j->opt->iflags & DD_FLAG_FULLBLOCK) {
        DWORD total = 0, n;
        while (total < len) {
            e = in_read(j, buf + total, len - total, &n);
            total += n;     /* an error can follow a part that was read */
            if (e || n == 0)
                break;
        }
        *got = total;
    } else {
        e = in_read(j, buf, len, got);
    }
    dd__sha256_update(j->in_hash, buf, *got);
    if (j->sparse_stream)
        zero_free(j, buf, at, *got);

    if (!e && *got > 0 && *got < len && j->warn_partial) {
        if (j->prev_nread > 0 && j->prev_nread < len) {
            if (j->opt->status != DD_STATUS_NONE)
                dd__msg(j->cb, DD_MSG_WARNING,
                        L"warning: partial read (%lu byte%ls); suggest iflag=fullblock",
                        j->prev_nread, j->prev_nread == 1 ? L"" : L"s");
            j->warn_partial = false;
        }
    }
    j->prev_nread = *got;
    return e;
}

/* ---- writing ----------------------------------------------------------- */

/* A compressed output cannot go back to rewrite the GPT headers after the
   data, so with fit the rebuilt protective MBR and primary header replace
   sectors 0 and 1 as they pass. */
static void patch_gpt_inline(job *j, uint8_t *buf, DWORD n)
{
    const dd__gpt_fix *f = &j->gpt;
    int64_t at = j->st->bytes_written, end = 2 * (int64_t)f->ss, i;

    for (i = at; i < end && i < at + n; i++)
        buf[i - at] = i < f->ss ? f->mbr[i] : f->primary[i - f->ss];
}

/* Hashes the output as it will finally read.  When fit rebuilds the GPT on
   a disk afterwards (write_gpt), sectors 0 and 1 are hashed as they will
   end up. */
static void hash_out(job *j, const uint8_t *buf, DWORD n)
{
    int64_t at = j->st->bytes_written, end = 2 * (int64_t)j->gpt.ss;

    if (!j->out_hash)
        return;
    if (j->have_gpt && !j->gpt_inline && at < end && n > 0) {
        uint8_t head[2 * 4096];
        DWORD k = end - at < (int64_t)n ? (DWORD)(end - at) : n, i;

        for (i = 0; i < k; i++) {
            int64_t p = at + i;
            head[i] = p < j->gpt.ss ? j->gpt.mbr[p] : j->gpt.primary[p - j->gpt.ss];
        }
        dd__sha256_update(j->out_hash, head, k);
        buf += k;
        n -= k;
    }
    dd__sha256_update(j->out_hash, buf, n);
}

static DWORD out_write(job *j, const uint8_t *buf, DWORD n, DWORD *put)
{
    if (j->gpt_inline && j->st->bytes_written < 2 * (int64_t)j->gpt.ss)
        patch_gpt_inline(j, (uint8_t *)buf, n);     /* buf is one of our own buffers */
    hash_out(j, buf, n);
    if ((j->opt->conv & DD_CONV_SPARSE) && ep_seekable(&j->out) && all_zero(buf, n)) {
        j->out.pos += n;
        *put = n;
        return 0;
    }
    return ep_write(&j->out, buf, n, put);
}

static step write_failed(job *j, DWORD e)
{
    if (e == ERROR_OPERATION_ABORTED && cancelled(j))
        return STEP_CANCEL;
    report(j, L"error writing", &j->out, e ? e : ERROR_WRITE_FAULT);
    return STEP_FAIL;
}

static step write_obuf(job *j)
{
    DWORD put = 0;
    DWORD e = out_write(j, j->obuf, j->obs, &put);

    j->st->bytes_written += put;
    if (e || put != j->obs) {
        if (put)
            j->st->w_partial++;
        return write_failed(j, e);
    }
    j->st->w_full++;
    return STEP_OK;
}

static step copy_simple(job *j, const uint8_t *src, DWORD n)
{
    while (n) {
        DWORD k = j->obs - j->oc;
        if (k > n)
            k = n;
        memcpy(j->obuf + j->oc, src, k);
        j->oc += k;
        src += k;
        n -= k;
        if (j->oc == j->obs) {
            step s = write_obuf(j);
            if (s != STEP_OK)
                return s;
            j->oc = 0;
        }
    }
    return STEP_OK;
}

/* conv=swab over a stream: an odd byte at the end of a block pairs with
   the first byte of the next one, and is written alone at the very end. */
static DWORD swab_into(job *j, const uint8_t *src, DWORD n)
{
    uint8_t *d = j->sbuf;
    DWORD m = 0, i = 0;

    if (j->saved_char >= 0 && n > 0) {
        d[m++] = src[0];
        d[m++] = (uint8_t)j->saved_char;
        j->saved_char = -1;
        i = 1;
    }
    for (; i + 1 < n; i += 2) {
        d[m++] = src[i + 1];
        d[m++] = src[i];
    }
    if (i < n)
        j->saved_char = src[i];
    return m;
}

static void convert_case(job *j, uint8_t *p, DWORD n)
{
    DWORD i;
    if (j->opt->conv & DD_CONV_UCASE) {
        for (i = 0; i < n; i++)
            if (p[i] >= 'a' && p[i] <= 'z')
                p[i] -= 'a' - 'A';
    } else if (j->opt->conv & DD_CONV_LCASE) {
        for (i = 0; i < n; i++)
            if (p[i] >= 'A' && p[i] <= 'Z')
                p[i] += 'a' - 'A';
    }
}

/* ---- setup ------------------------------------------------------------- */

static const wchar_t *filter_description(dd_filter f)
{
    switch (f) {
    case DD_FILTER_REMOVABLE: return L"whole removable disk";
    case DD_FILTER_FIXED:     return L"whole fixed disk";
    case DD_FILTER_DISK:      return L"whole disk";
    case DD_FILTER_PARTITION: return L"partition, volume or drive";
    case DD_FILTER_FILE:      return L"regular file";
    default:                  return L"";
    }
}

static bool filter_match(const dd_endpoint *ep, dd_filter f)
{
    bool disk = ep->kind == EP_BLOCK && ep->whole_disk;
    switch (f) {
    case DD_FILTER_NONE:      return true;
    case DD_FILTER_REMOVABLE: return disk && ep->removable;
    case DD_FILTER_FIXED:     return disk && !ep->removable;
    case DD_FILTER_DISK:      return disk;
    case DD_FILTER_PARTITION: return ep->kind == EP_BLOCK && !ep->whole_disk;
    case DD_FILTER_FILE:      return ep->kind == EP_FILE;
    }
    return false;
}

/* Rejects an output that cannot match before opening it, so a filter
   never leaves a newly created file behind. */
static bool filter_precheck(const job *j)
{
    const wchar_t *p = j->opt->output;
    bool dev = false;
    wchar_t *norm;

    if (j->opt->filter == DD_FILTER_NONE)
        return true;
    if (p && wcscmp(p, L"-") != 0 && _wcsnicmp(p, L"/dev/", 5) != 0) {
        norm = dd__normalize_path(p);
        dev = norm && dd__is_device_path(norm);
        free(norm);
    }
    return j->opt->filter == DD_FILTER_FILE ? !dev : dev;
}

static void filter_refused(const job *j)
{
    const wchar_t *name = j->opt->output_name ? j->opt->output_name
                          : j->opt->output && wcscmp(j->opt->output, L"-") != 0
                          ? j->opt->output : L"standard output";
    dd__msg(j->cb, DD_MSG_ERROR, L"'%ls' is not a %ls; refusing to write to it (--filter=%ls)",
            name, filter_description(j->opt->filter), dd_filter_name(j->opt->filter));
}

static uint8_t *alloc_buffer(DWORD n)
{
    return VirtualAlloc(NULL, n ? n : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static dd_result skip_input(job *j, int64_t bytes)
{
    dd_endpoint *in = &j->in;
    wchar_t m[256];
    DWORD e, got;

    if (bytes <= 0)
        return DD_OK;
    switch (in->kind) {
    case EP_FILE:
    case EP_BLOCK:
        if (in->size >= 0 && in->pos + bytes > in->size)
            warn(j, L"'%ls': cannot skip to specified offset", in->name);
        in->pos += bytes;
        return DD_OK;

    case EP_STREAM:
        while (bytes > 0) {
            DWORD n = bytes < (int64_t)j->ibs ? (DWORD)bytes : j->ibs;
            if (cancelled(j))
                return DD_CANCELLED;
            e = ep_read(in, j->ibuf, n, &got);
            if (e) {
                if (e == ERROR_OPERATION_ABORTED && cancelled(j))
                    return DD_CANCELLED;
                dd_errstr(e, m, 256);
                dd__msg(j->cb, DD_MSG_ERROR, L"'%ls': cannot skip: %ls", in->name, m);
                return DD_FAILED;
            }
            if (got == 0) {
                warn(j, L"'%ls': cannot skip to specified offset", in->name);
                return DD_OK;
            }
            bytes -= got;
        }
        return DD_OK;

    default:
        return DD_OK;   /* /dev/zero and friends look the same everywhere */
    }
}

static dd_result seek_output(job *j, int64_t bytes)
{
    const dd_options *opt = j->opt;
    dd_endpoint *out = &j->out;
    wchar_t m[256];
    DWORD e, ret;

    if (!(out->kind == EP_FILE && (opt->oflags & DD_FLAG_APPEND)) && bytes > 0) {
        if (ep_seekable(out)) {
            out->pos += bytes;
        } else if (out->kind == EP_STREAM) {
            dd__msg(j->cb, DD_MSG_ERROR, L"'%ls': cannot seek: Illegal seek", out->name);
            return DD_FAILED;
        }
    }

    if (out->kind == EP_FILE && out->own_handle &&
        !(opt->conv & DD_CONV_NOTRUNC) && !(opt->oflags & DD_FLAG_APPEND)) {
        e = ep_set_eof(out, out->pos);
        if (e) {
            dd_errstr(e, m, 256);
            dd__msg(j->cb, DD_MSG_ERROR, L"failed to truncate to %lld bytes in output file '%ls': %ls",
                    (long long)out->pos, out->name, m);
            return DD_FAILED;
        }
    }

    if ((opt->conv & DD_CONV_SPARSE) && out->kind == EP_FILE)
        DeviceIoControl(out->h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &ret, NULL);
    return DD_OK;
}

static int64_t expected_total(const job *j)
{
    const dd_options *opt = j->opt;
    int64_t avail = -1, limit = -1;

    if ((j->in.kind == EP_FILE || j->in.kind == EP_BLOCK || j->in.xz) && j->in.size >= 0)
        avail = j->in.size > j->in.pos ? j->in.size - j->in.pos : 0;
    else if (j->in.kind == EP_NULL)
        avail = 0;
    if (opt->count >= 0) {
        if (opt->iflags & DD_FLAG_COUNT_BYTES)
            limit = opt->count;
        else if (!mul64(opt->count, j->ibs, &limit))
            limit = -1;
    }
    if (avail < 0)
        return limit;
    if (limit < 0)
        return avail;
    return avail < limit ? avail : limit;
}

/* ---- fit: write an image's partitions only ----------------------------- */

/* An MBR image, or one without a partition table, written to a disk
   leaves the disk's old end in place.  If the disk was GPT before, its
   backup GPT is still there, and gdisk and other tools then report a
   damaged GPT.  The backup's place (a 16 KiB partition array and a
   header in the last sector) is cleared, unless the image's data reaches
   into it. */
#define GPT_BACKUP_ARRAY 16384

static void plan_clear(job *j, int64_t data_end)
{
    const dd_endpoint *out = &j->out;
    int64_t ss = out->align ? out->align : 512;
    int64_t from;

    if (out->kind != EP_BLOCK || !out->whole_disk || out->size <= 0)
        return;
    from = out->size - (GPT_BACKUP_ARRAY + ss);
    if (from < data_end)
        from = (data_end + ss - 1) / ss * ss;
    if (from >= out->size)
        return;
    j->clear_off = from;
    j->clear_len = out->size - from;
}

/* Checks before anything is locked or written that the image's data fits
   the output, and prepares the GPT for the output's size. */
static dd_result check_fit(job *j)
{
    const dd_layout *l = j->layout;
    dd_endpoint *out = &j->out;
    wchar_t a[32], b[32], err[512];
    int64_t needed = dd_layout_needed(l);

    if (l->note[0])
        dd__msg(j->cb, DD_MSG_WARNING, L"'%ls': %ls", j->in.name, l->note);
    if (l->scheme == DD_SCHEME_NONE) {
        dd__msg(j->cb, DD_MSG_INFO, L"'%ls' has no partition table; writing all of it", j->in.name);
        if (j->in.size >= 0)
            plan_clear(j, j->in.size);
        return DD_OK;
    }
    if (l->scheme == DD_SCHEME_GPT && out->kind != EP_BLOCK && out->kind != EP_FILE && !out->xz) {
        dd__msg(j->cb, DD_MSG_ERROR, L"fitting a GPT image needs a disk or a file as output");
        return DD_FAILED;
    }
    /* An image file gets exactly the data plus its own backup GPT, so it
       is a valid disk image of its new size.  Compressed, the same image
       is produced in one pass (patch_gpt_inline); so is a file that is
       hashed, so that the hash covers it in order. */
    if (l->scheme == DD_SCHEME_GPT && (out->kind == EP_FILE || out->xz)) {
        if (!dd__gpt_fix_prepare(j->opt->input, needed, &j->gpt, err, 512)) {
            dd__msg(j->cb, DD_MSG_ERROR, L"%ls", err);
            return DD_FAILED;
        }
        j->have_gpt = true;
        if (out->xz || j->out_hash)
            j->gpt_inline = true;
        else
            j->gpt_file_end = needed;
    }
    if (out->kind == EP_BLOCK) {
        if (l->sector_size != out->align) {     /* the table counts in sectors */
            dd__msg(j->cb, DD_MSG_ERROR, L"'%ls' uses %lu-byte sectors but '%ls' uses %lu-byte sectors",
                    j->in.name, l->sector_size, out->name, out->align);
            return DD_FAILED;
        }
        if (out->size >= 0 && needed > out->size) {
            dd_format_size(needed, false, a, 32);
            dd_format_size(out->size, false, b, 32);
            dd__msg(j->cb, DD_MSG_ERROR, L"the partitions of '%ls' need %lld bytes (%ls), but '%ls' holds only %lld bytes (%ls)",
                    j->in.name, (long long)needed, a, out->name, (long long)out->size, b);
            return DD_FAILED;
        }
        if (l->scheme == DD_SCHEME_GPT) {
            if (out->size < 0) {
                dd__msg(j->cb, DD_MSG_ERROR, L"cannot find the size of '%ls' to place the backup GPT", out->name);
                return DD_FAILED;
            }
            if (!dd__gpt_fix_prepare(j->opt->input, out->size, &j->gpt, err, 512)) {
                dd__msg(j->cb, DD_MSG_ERROR, L"%ls", err);
                return DD_FAILED;
            }
            j->have_gpt = true;
        } else {
            plan_clear(j, needed);
        }
    }
    dd_format_size(l->data_end, false, a, 32);
    dd_format_size(l->image_size, false, b, 32);
    dd__msg(j->cb, DD_MSG_INFO, L"%ls partition table: copying %lld bytes (%ls) of %lld (%ls), up to the end of the last partition",
            l->scheme == DD_SCHEME_GPT ? L"GPT" : L"MBR",
            (long long)(l->data_end < l->image_size ? l->data_end : l->image_size), a,
            (long long)l->image_size, b);
    return DD_OK;
}

/* After the data: the backup GPT goes at the end of the disk, then the
   primary header and protective MBR that point at it. */
static dd_result write_gpt(job *j)
{
    const dd__gpt_fix *f = &j->gpt;
    const struct { int64_t off; const uint8_t *p; DWORD n; } w[4] = {
        { f->backup_entries_off, f->entries, f->entries_len },
        { f->backup_off, f->backup, f->ss },
        { f->ss, f->primary, f->ss },
        { 0, f->mbr, f->ss },
    };
    DWORD e, put;
    int i;

    if (j->gpt_inline) {
        /* the headers were patched in passing; the backup follows the data,
           with zeros first if the input ended before its last partition */
        static const uint8_t zeros[4096] = { 0 };
        int64_t at = j->st->bytes_written;
        while (at < f->backup_entries_off) {
            DWORD n = f->backup_entries_off - at > (int64_t)sizeof zeros ? (DWORD)sizeof zeros
                                                                         : (DWORD)(f->backup_entries_off - at);
            dd__sha256_update(j->out_hash, zeros, n);
            if ((e = ep_write(&j->out, zeros, n, &put)) != 0)
                goto failed;
            at += n;
        }
        if (at != f->backup_entries_off) {
            e = ERROR_INVALID_DATA;
            goto failed;
        }
        dd__sha256_update(j->out_hash, f->entries, f->entries_len);
        dd__sha256_update(j->out_hash, f->backup, f->ss);
        if ((e = ep_write(&j->out, f->entries, f->entries_len, &put)) != 0 ||
            (e = ep_write(&j->out, f->backup, f->ss, &put)) != 0)
            goto failed;
        dd__msg(j->cb, DD_MSG_INFO, L"rebuilt the GPT for the size of the image");
        return DD_OK;
    failed:
        report(j, L"error writing the GPT to", &j->out, e);
        return DD_FAILED;
    }

    for (i = 0; i < 4; i++) {
        j->out.pos = w[i].off;
        e = ep_write(&j->out, w[i].p, w[i].n, &put);
        if (e || put != w[i].n) {
            report(j, L"error writing the GPT to", &j->out, e ? e : ERROR_WRITE_FAULT);
            return DD_FAILED;
        }
    }
    /* the backup GPT must be the last thing in an image file */
    if (j->gpt_file_end > 0) {
        e = ep_set_eof(&j->out, j->gpt_file_end);
        if (e) {
            report(j, L"failed to set the length of", &j->out, e);
            return DD_FAILED;
        }
    }
    dd__msg(j->cb, DD_MSG_INFO, L"rebuilt the GPT for the size of '%ls'", j->out.name);
    return DD_OK;
}

static dd_result clear_end(job *j)
{
    uint8_t *zeros = alloc_buffer((DWORD)j->clear_len);
    wchar_t size[32];
    DWORD e, put = 0;

    if (!zeros) {
        dd__msg(j->cb, DD_MSG_ERROR, L"memory exhausted");
        return DD_FAILED;
    }
    j->out.pos = j->clear_off;
    e = ep_write(&j->out, zeros, (DWORD)j->clear_len, &put);
    VirtualFree(zeros, 0, MEM_RELEASE);
    if (e || put != (DWORD)j->clear_len) {
        report(j, L"error clearing the end of", &j->out, e ? e : ERROR_WRITE_FAULT);
        return DD_FAILED;
    }
    dd_format_size(j->clear_len, false, size, 32);
    dd__msg(j->cb, DD_MSG_INFO, L"cleared the last %lld bytes (%ls) of '%ls', so no backup GPT from its earlier use is left",
            (long long)j->clear_len, size, j->out.name);
    return DD_OK;
}

/* ---- the copy ---------------------------------------------------------- */

static dd_result copy_loop(job *j)
{
    const dd_options *opt = j->opt;
    dd_stats *st = j->st;
    dd_result res = DD_OK;
    int64_t max_records = -1, max_bytes = 0;
    bool write_stopped = false;
    wchar_t m[256];
    DWORD e;

    if (opt->count >= 0) {
        if (opt->iflags & DD_FLAG_COUNT_BYTES) {
            max_records = opt->count / j->ibs;
            max_bytes = opt->count % j->ibs;
        } else {
            max_records = opt->count;
        }
    }

    for (;;) {
        DWORD req, nread, n;
        step s;

        progress_tick(j);
        if (cancelled(j)) {
            res = DD_CANCELLED;
            write_stopped = true;
            break;
        }
        if (max_records >= 0 && st->r_partial + st->r_full >= max_records + (max_bytes ? 1 : 0))
            break;
        req = (max_records >= 0 && st->r_partial + st->r_full >= max_records) ? (DWORD)max_bytes : j->ibs;

        if (opt->conv & DD_CONV_SYNC)
            memset(j->ibuf, 0, j->ibs);

        e = read_block(j, j->ibuf, req, &nread);
        if (e) {
            if (e == ERROR_OPERATION_ABORTED && cancelled(j)) {
                res = DD_CANCELLED;
                write_stopped = true;
                break;
            }
            if (!(opt->conv & DD_CONV_NOERROR) || j->in.xz || opt->status != DD_STATUS_NONE)
                report(j, L"error reading", &j->in, e);
            /* nothing after an error in a compressed stream can be found */
            if (!(opt->conv & DD_CONV_NOERROR) || j->in.xz) {
                res = DD_FAILED;
                break;      /* still write what is already gathered */
            }
            res = DD_FAILED;
            /* step over the unreadable part and carry on */
            if (ep_seekable(&j->in))
                j->in.pos += req - nread;
            if (nread == 0 && !(opt->conv & DD_CONV_SYNC))
                continue;
        } else if (nread == 0) {
            break;
        }

        st->bytes_read += nread;
        n = nread;
        if (n < j->ibs) {
            st->r_partial++;
            if (opt->conv & DD_CONV_SYNC)
                n = j->ibs;          /* the buffer was zeroed before the read */
        } else {
            st->r_full++;
        }

        if (j->direct) {
            DWORD put = 0;
            e = out_write(j, j->ibuf, n, &put);
            st->bytes_written += put;
            if (e || put != n) {
                s = write_failed(j, e);
                res = s == STEP_CANCEL ? DD_CANCELLED : DD_FAILED;
                write_stopped = true;
                break;
            }
            if (n == j->ibs)
                st->w_full++;
            else
                st->w_partial++;
            continue;
        }

        convert_case(j, j->ibuf, n);
        if (opt->conv & DD_CONV_SWAB)
            s = copy_simple(j, j->sbuf, swab_into(j, j->ibuf, n));
        else
            s = copy_simple(j, j->ibuf, n);
        if (s != STEP_OK) {
            res = s == STEP_CANCEL ? DD_CANCELLED : DD_FAILED;
            write_stopped = true;
            break;
        }
    }

    if (!write_stopped && !j->direct) {
        if (j->saved_char >= 0) {
            uint8_t c = (uint8_t)j->saved_char;
            j->saved_char = -1;
            if (copy_simple(j, &c, 1) != STEP_OK) {
                res = DD_FAILED;
                write_stopped = true;
            }
        }
        if (!write_stopped && j->oc > 0) {
            DWORD put = 0;
            e = out_write(j, j->obuf, j->oc, &put);
            st->bytes_written += put;
            if (put)
                st->w_partial++;
            if (e || put != j->oc) {
                res = write_failed(j, e) == STEP_CANCEL ? DD_CANCELLED : DD_FAILED;
                write_stopped = true;
            }
            j->oc = 0;
        }
    }

    if (res == DD_CANCELLED) {
        j->st->xz_bytes = xz_bytes(j);
        j->st->xz_done = j->out.xz ? dd__xz_compressed_in(j->out.xz) : -1;
        return res;
    }

    if (opt->conv & DD_CONV_SPARSE) {
        e = ep_extend_to_pos(&j->out);
        if (e) {
            dd_errstr(e, m, 256);
            dd__msg(j->cb, DD_MSG_ERROR, L"failed to truncate to %lld bytes in output file '%ls': %ls",
                    (long long)j->out.pos, j->out.name, m);
            res = DD_FAILED;
        }
    }

    if (res == DD_OK && j->have_gpt)
        res = write_gpt(j);
    if (res == DD_OK && j->clear_len)
        res = clear_end(j);

    /* Raw devices are always flushed: Windows has no sync command and
       the device may be unplugged as soon as dd exits. */
    if ((opt->conv & (DD_CONV_FSYNC | DD_CONV_FDATASYNC)) || j->out.kind == EP_BLOCK) {
        e = ep_flush(&j->out);
        if (e) {
            report(j, L"fsync failed for", &j->out, e);
            res = DD_FAILED;
        }
    }
    j->st->elapsed = elapsed(j);
    j->st->xz_bytes = xz_bytes(j);      /* compressing, set again once finished */
    j->st->xz_done = j->out.xz ? dd__xz_compressed_in(j->out.xz) : -1;
    return res;
}

/* ---- hashes and verify ------------------------------------------------- */

static dd_result no_memory(const job *j)
{
    dd__msg(j->cb, DD_MSG_ERROR, L"memory exhausted");
    return DD_FAILED;
}

static bool get_vbuf(job *j)
{
    if (!j->vbuf)
        j->vbuf = VirtualAlloc(NULL, VBUF_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!j->vbuf)
        no_memory(j);
    return j->vbuf != NULL;
}

/* Prepares to check the input against its .hashes file as it is read.
   Only the whole input can be checked, so not with skip, nor with count
   unless fit set it.  With required (--verify-hashes) a missing hash file
   or hash is an error. */
static dd_result setup_input_check(job *j, bool required)
{
    const dd_options *opt = j->opt;
    wchar_t name[MAX_PATH], m[256];
    DWORD e;

    if (opt->hashes == DD_HASHES_OFF || opt->skip || (opt->count >= 0 && !j->layout))
        return DD_OK;
    if (opt->sparse_read && !j->in.xz) {
        /* free space reads as zeros, so the image's hash cannot match.  (A
           compressed input is read whole, and hashed before zeroing.) */
        if (j->in.kind == EP_FILE && dd_hash_file_name(opt->input, j->in_hashes, HASH_PATH) &&
            GetFileAttributesW(j->in_hashes) != INVALID_FILE_ATTRIBUTES)
            dd__msg(j->cb, DD_MSG_INFO, L"not checking '%ls' against %ls: sparse reading leaves out its free space",
                    j->in.name, j->in_hashes);
        return DD_OK;
    }
    if (!j->in.own_handle || (j->in.kind != EP_FILE && !j->in.xz) ||
        !dd_hash_file_name(opt->input, j->in_hashes, HASH_PATH)) {
        if (required)
            dd__msg(j->cb, DD_MSG_ERROR, L"'%ls' is not an image file", j->in.name);
        return required ? DD_FAILED : DD_OK;
    }
    e = dd__hashes_read(j->in_hashes, &j->in_list);
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
        if (required)
            dd__msg(j->cb, DD_MSG_ERROR, L"'%ls' has no hash file: %ls does not exist", j->in.name, j->in_hashes);
        return required ? DD_FAILED : DD_OK;
    }
    if (e) {
        dd_errstr(e, m, 256);
        dd__msg(j->cb, required ? DD_MSG_ERROR : DD_MSG_WARNING, L"cannot use the hash file %ls: %ls",
                j->in_hashes, e == ERROR_INVALID_DATA ? L"it holds no SHA-256 hashes" : m);
        return required ? DD_FAILED : DD_OK;
    }
    if (j->in.xz) {
        j->in_raw_want = dd__hashes_find(&j->in_list, opt->input);
        dd__uncompressed_name(opt->input, name, MAX_PATH);
        j->in_want = dd__hashes_find(&j->in_list, name);
    } else {
        j->in_want = dd__hashes_find(&j->in_list, opt->input);
    }
    if (!j->in_want && !j->in_raw_want) {
        dd__msg(j->cb, required ? DD_MSG_ERROR : DD_MSG_WARNING, L"%ls has no hash for '%ls'",
                j->in_hashes, dd__base_name(opt->input));
        return required ? DD_FAILED : DD_OK;
    }
    if (j->in_want && (j->in_hash = dd__sha256_new()) == NULL)
        return no_memory(j);
    if (j->in_raw_want) {
        if ((j->in_raw_hash = dd__sha256_new()) == NULL)
            return no_memory(j);
        dd__xz_set_hash(j->in.xz, j->in_raw_hash);
    }
    dd__msg(j->cb, DD_MSG_INFO, L"checking '%ls' against %ls", j->in.name, j->in_hashes);
    return DD_OK;
}

/* Reads the rest of the input, so that its hashes cover all of it. */
static dd_result drain_input(job *j)
{
    DWORD got, e;

    if (!get_vbuf(j))
        return DD_FAILED;
    for (;;) {
        progress_tick(j);
        if (cancelled(j))
            return DD_CANCELLED;
        e = ep_read(&j->in, j->vbuf, VBUF_SIZE, &got);
        if (e) {
            if (e == ERROR_OPERATION_ABORTED && cancelled(j))
                return DD_CANCELLED;
            report(j, L"error reading", &j->in, e);
            return DD_FAILED;
        }
        if (got == 0)
            return DD_OK;
        dd__sha256_update(j->in_hash, j->vbuf, got);
        j->st->phase_done += got;
    }
}

static bool hash_matches(const job *j, dd__sha256 *s, const uint8_t *want, const wchar_t *what, const wchar_t *from)
{
    uint8_t got[32];
    wchar_t hex[65];

    if (!dd__sha256_final(s, got)) {
        dd__msg(j->cb, DD_MSG_ERROR, L"cannot compute the SHA-256 of %ls", what);
        return false;
    }
    /* the full path was given when the check began */
    if (memcmp(got, want, 32) == 0) {
        dd__msg(j->cb, DD_MSG_INFO, L"SHA-256 of %ls matches %ls", what, dd__base_name(from));
        return true;
    }
    dd__hex(got, hex);
    dd__msg(j->cb, DD_MSG_ERROR,
            L"SHA-256 of %ls is %ls, which does not match %ls: the image is damaged, or %ls is for another image",
            what, hex, dd__base_name(from), dd__base_name(from));
    return false;
}

static dd_result check_input(job *j)
{
    wchar_t name[MAX_PATH], what[MAX_PATH + 32];
    bool ok = true;

    if (j->in_raw_want)
        ok = hash_matches(j, j->in_raw_hash, j->in_raw_want, dd__base_name(j->opt->input),
                          j->raw_want_from ? j->raw_want_from : j->in_hashes);
    if (j->in_want) {
        if (j->in.xz) {
            dd__uncompressed_name(j->opt->input, name, MAX_PATH);
            _snwprintf_s(what, MAX_PATH + 32, _TRUNCATE, L"%ls (uncompressed)", name);
        } else {
            wcsncpy_s(what, MAX_PATH + 32, dd__base_name(j->opt->input), _TRUNCATE);
        }
        ok = hash_matches(j, j->in_hash, j->in_want, what, j->in_hashes) && ok;
    }
    if (!ok && j->st->bytes_written > 0 && j->out.kind != EP_NULL)
        dd__msg(j->cb, DD_MSG_ERROR, L"'%ls' was written from an image that failed its hash check", j->out.name);
    return ok ? DD_OK : DD_FAILED;
}

/* The output's .hashes file (written for a new image of a disk, or any new
   file with DD_HASHES_ON) and verify both hash what is written. */
static dd_result setup_output(job *j, bool out_is_file)
{
    const dd_options *opt = j->opt;
    dd_endpoint *out = &j->out;
    bool fresh;

    j->out_is_file = out_is_file && out->own_handle;
    fresh = j->out_is_file && !opt->seek && !(opt->conv & DD_CONV_NOTRUNC) && !(opt->oflags & DD_FLAG_APPEND);
    if (opt->hashes == DD_HASHES_ON || (opt->hashes == DD_HASHES_AUTO && j->in.kind == EP_BLOCK)) {
        wchar_t in_hashes[HASH_PATH];
        if (fresh && dd_hash_file_name(opt->output, j->out_hashes, HASH_PATH)) {
            j->make_hashes = true;
            /* disk.img to disk.img.xz: both use disk.img.hashes.  If the data
               changes on the way, its hash would no longer be disk.img's. */
            if (opt->input && dd_hash_file_name(opt->input, in_hashes, HASH_PATH) &&
                dd_same_file(in_hashes, j->out_hashes)) {
                const wchar_t *how = opt->sparse_read ? L"--sparse-read" : opt->fit ? L"--fit"
                                   : opt->skip > 0 ? L"skip=" : opt->count >= 0 ? L"count="
                                   : opt->conv & (DD_CONV_SYNC | DD_CONV_SWAB | DD_CONV_LCASE | DD_CONV_UCASE) ? L"conv="
                                   : NULL;
                j->hashes_shared = true;
                if (how) {
                    j->make_hashes = false;
                    if (opt->status != DD_STATUS_NONE)
                        dd__msg(j->cb, DD_MSG_WARNING, L"not writing %ls: it is the input's hash file too, and %ls changes the data",
                                j->out_hashes, how);
                }
            }
        } else if (opt->hashes == DD_HASHES_ON) {
            dd__msg(j->cb, DD_MSG_ERROR, L"--hashes needs a new image file as output; '%ls' is not one", out->name);
            return DD_FAILED;
        }
    }
    if (opt->verify) {
        if (!j->out_is_file && out->kind != EP_BLOCK) {
            dd__msg(j->cb, DD_MSG_ERROR, L"--verify reads the output back, so it needs a file or a disk; '%ls' is neither",
                    out->name);
            return DD_FAILED;
        }
        if ((opt->conv & DD_CONV_SPARSE) && out->kind == EP_BLOCK) {
            dd__msg(j->cb, DD_MSG_ERROR, L"--verify cannot check conv=sparse on a device, which keeps its old data where blocks are skipped");
            return DD_FAILED;
        }
    }
    if (!j->make_hashes && !opt->verify)
        return DD_OK;
    if ((j->out_hash = dd__sha256_new()) == NULL)
        return no_memory(j);
    if (out->xz) {
        if ((j->out_raw_hash = dd__sha256_new()) == NULL)
            return no_memory(j);
        dd__xz_set_hash(out->xz, j->out_raw_hash);
    }
    /* it described the image being replaced; one that is the input's too
       is still checked against, and rewritten only when all is well */
    if (j->make_hashes && !j->hashes_shared)
        DeleteFileW(j->out_hashes);
    return DD_OK;
}

static bool finish_out_hashes(job *j)
{
    if (j->out_final)
        return true;
    j->out_bytes = dd__sha256_bytes(j->out_hash);
    if (!dd__sha256_final(j->out_hash, j->out_digest) ||
        (j->out_raw_hash && !dd__sha256_final(j->out_raw_hash, j->out_raw_digest))) {
        dd__msg(j->cb, DD_MSG_ERROR, L"cannot compute the SHA-256 of '%ls'", j->out.name);
        return false;
    }
    j->out_final = true;
    return true;
}

static dd_result write_hashes(job *j)
{
    const wchar_t *output = j->opt->output;
    dd__hash_list *old = NULL;
    dd__hash_entry e[DD_MAX_HASHES];
    wchar_t m[256];
    size_t n = 1, i;
    DWORD err;

    if (!finish_out_hashes(j))
        return DD_FAILED;
    if (j->out.xz)
        dd__uncompressed_name(output, e[0].name, MAX_PATH);
    else
        wcsncpy_s(e[0].name, MAX_PATH, dd__base_name(output), _TRUNCATE);
    memcpy(e[0].digest, j->out_digest, 32);
    if (j->out_raw_hash) {
        wcsncpy_s(e[1].name, MAX_PATH, dd__base_name(output), _TRUNCATE);
        memcpy(e[1].digest, j->out_raw_digest, 32);
        n = 2;
    }
    /* the input's file too (disk.img.xz to disk.img): its other entries,
       such as the compressed file's, still hold */
    if (j->hashes_shared && (old = malloc(sizeof *old)) != NULL && dd__hashes_read(j->out_hashes, old) == 0)
        for (i = 0; i < old->count && n < DD_MAX_HASHES; i++)
            if (_wcsicmp(old->e[i].name, e[0].name) != 0 && (n < 2 || _wcsicmp(old->e[i].name, e[1].name) != 0))
                e[n++] = old->e[i];
    free(old);
    err = dd__hashes_write(j->out_hashes, e, n);
    if (err) {
        dd_errstr(err, m, 256);
        dd__msg(j->cb, DD_MSG_ERROR, L"cannot write the hash file %ls: %ls", j->out_hashes, m);
        return DD_FAILED;
    }
    dd__msg(j->cb, DD_MSG_INFO, L"SHA-256 hashes written to %ls", j->out_hashes);
    return DD_OK;
}

static DWORD read_at(HANDLE h, void *buf, DWORD len, int64_t off, DWORD *got)
{
    OVERLAPPED ov;

    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    if (ReadFile(h, buf, len, got, &ov))
        return 0;
    *got = 0;
    return GetLastError() == ERROR_HANDLE_EOF ? 0 : GetLastError();
}

static dd_result short_readback(const job *j, int64_t left)
{
    dd__msg(j->cb, DD_MSG_ERROR, L"verify: '%ls' ends %lld bytes early when read back",
            j->out.name, (long long)left);
    return DD_FAILED;
}

static dd_result readback_failed(const job *j, DWORD e)
{
    if (e == ERROR_OPERATION_ABORTED && cancelled(j))
        return DD_CANCELLED;
    report(j, L"verify: error reading back", &j->out, e);
    return DD_FAILED;
}

/* A disk or volume: read through the copy's own handle, which also holds
   the volume locks.  Raw devices are not cached by Windows. */
static dd_result verify_device(job *j, dd__sha256 *s)
{
    int64_t left = j->out_bytes;
    DWORD got, e;

    j->out.pos = j->out_start;
    while (left > 0) {
        DWORD want = left < VBUF_SIZE ? (DWORD)left : VBUF_SIZE;
        progress_tick(j);
        if (cancelled(j))
            return DD_CANCELLED;
        e = ep_read(&j->out, j->vbuf, want, &got);
        if (e)
            return readback_failed(j, e);
        if (got == 0)
            return short_readback(j, left);
        dd__sha256_update(s, j->vbuf, got);
        left -= got;
        j->st->phase_done += got;
    }
    return DD_OK;
}

/* fit to a disk: the backup GPT went to the end of the disk, outside the
   data hashed above. */
static dd_result verify_gpt(job *j)
{
    const dd__gpt_fix *f = &j->gpt;
    const struct { int64_t off; const uint8_t *p; DWORD n; } w[2] = {
        { f->backup_entries_off, f->entries, f->entries_len },
        { f->backup_off, f->backup, f->ss },
    };
    DWORD got, e, done, n;
    int i;

    for (i = 0; i < 2; i++)
        for (done = 0; done < w[i].n; done += n) {     /* the array can be larger than vbuf */
            n = w[i].n - done < VBUF_SIZE ? w[i].n - done : VBUF_SIZE;
            j->out.pos = w[i].off + done;
            e = ep_read(&j->out, j->vbuf, n, &got);
            if (e)
                return readback_failed(j, e);
            if (got != n || memcmp(j->vbuf, w[i].p + done, n) != 0) {
                dd__msg(j->cb, DD_MSG_ERROR, L"verification failed: the backup GPT on '%ls' does not read back as written",
                        j->out.name);
                return DD_FAILED;
            }
        }
    return DD_OK;
}

/* fit to a disk: the cleared end must read back as zeros */
static dd_result verify_cleared(job *j)
{
    DWORD got, e, i;

    j->out.pos = j->clear_off;
    e = ep_read(&j->out, j->vbuf, (DWORD)j->clear_len, &got);
    if (e)
        return readback_failed(j, e);
    for (i = 0; i < got && j->vbuf[i] == 0; i++)
        ;
    if (got != (DWORD)j->clear_len || i != got) {
        dd__msg(j->cb, DD_MSG_ERROR, L"verification failed: the cleared end of '%ls' does not read back as zeros",
                j->out.name);
        return DD_FAILED;
    }
    return DD_OK;
}

/* A file: flushed, then read with a second, unbuffered handle, so the data
   comes from the disk and not from the cache. */
static HANDLE open_readback(job *j, DWORD *e)
{
    HANDLE h;

    if (!FlushFileBuffers(j->out.h)) {
        *e = GetLastError();
        return INVALID_HANDLE_VALUE;
    }
    h = CreateFileW(j->out.path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    *e = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    return h;
}

static dd_result verify_file(job *j, dd__sha256 *s)
{
    int64_t start = j->out_start, end = start + j->out_bytes;
    int64_t pos = start - start % VALIGN;
    dd_result r = DD_OK;
    DWORD got, e;
    HANDLE h = open_readback(j, &e);

    if (h == INVALID_HANDLE_VALUE)
        return readback_failed(j, e);
    while (pos < end) {
        int64_t a, b;
        progress_tick(j);
        if (cancelled(j)) {
            r = DD_CANCELLED;
            break;
        }
        e = read_at(h, j->vbuf, VBUF_SIZE, pos, &got);
        if (e) {
            r = readback_failed(j, e);
            break;
        }
        if (got == 0) {
            r = short_readback(j, end - pos);
            break;
        }
        a = pos > start ? pos : start;
        b = pos + got < end ? pos + got : end;
        if (b > a) {
            dd__sha256_update(s, j->vbuf + (a - pos), (DWORD)(b - a));
            j->st->phase_done += b - a;
        }
        pos += got;
    }
    CloseHandle(h);
    return r;
}

/* A compressed file: decompressed again, hashing both the file and the data. */
static dd_result verify_xz(job *j, dd__sha256 *s, dd__sha256 *raw)
{
    dd_result r = DD_OK;
    dd__xz *x = NULL;
    DWORD got, e;
    HANDLE h = open_readback(j, &e);

    if (h == INVALID_HANDLE_VALUE)
        return readback_failed(j, e);
    e = dd__xz_decoder(&x, h, j->xz_budget);      /* the encoder has finished */
    if (e) {
        CloseHandle(h);
        return readback_failed(j, e);
    }
    dd__xz_set_hash(x, raw);
    for (;;) {
        progress_tick(j);
        if (cancelled(j)) {
            r = DD_CANCELLED;
            break;
        }
        e = dd__xz_read(x, j->vbuf, VBUF_SIZE, &got);
        dd__sha256_update(s, j->vbuf, got);
        j->st->phase_done += got;
        if (e) {
            r = readback_failed(j, e);
            break;
        }
        if (got == 0)
            break;
    }
    dd__xz_free(x);
    CloseHandle(h);
    return r;
}

/* Reads the output back and checks it holds exactly what was written. */
static dd_result verify_output(job *j)
{
    dd__sha256 *s = NULL, *raw = NULL;
    uint8_t d[32], rd[32];
    wchar_t hex[65];
    dd_result r;
    bool ok;

    if (!finish_out_hashes(j) || !get_vbuf(j))
        return DD_FAILED;
    s = dd__sha256_new();
    if (j->out_raw_hash)
        raw = dd__sha256_new();
    if (!s || (j->out_raw_hash && !raw)) {
        dd__sha256_free(s);
        dd__sha256_free(raw);
        return no_memory(j);
    }
    start_phase(j, DD_PHASE_VERIFY, j->out_bytes);
    dd__msg(j->cb, DD_MSG_INFO, L"verifying '%ls' by reading it back", j->out.name);
    if (j->out.kind == EP_BLOCK)
        r = verify_device(j, s);
    else if (j->out.xz)
        r = verify_xz(j, s, raw);
    else
        r = verify_file(j, s);
    if (r == DD_OK && j->out.kind == EP_BLOCK && j->have_gpt && !j->gpt_inline)
        r = verify_gpt(j);
    if (r == DD_OK && j->out.kind == EP_BLOCK && j->clear_len)
        r = verify_cleared(j);
    if (r == DD_OK) {
        ok = dd__sha256_final(s, d) && dd__sha256_bytes(s) == j->out_bytes &&
             memcmp(d, j->out_digest, 32) == 0;
        if (raw)
            ok = dd__sha256_final(raw, rd) && memcmp(rd, j->out_raw_digest, 32) == 0 && ok;
        if (ok) {
            dd__hex(j->out_digest, hex);
            dd__msg(j->cb, DD_MSG_INFO, L"verified: '%ls' reads back exactly as written (%lld bytes, SHA-256 %ls)",
                    j->out.name, (long long)j->out_bytes, hex);
        } else {
            dd__msg(j->cb, DD_MSG_ERROR, L"verification failed: '%ls' does not read back as written", j->out.name);
            r = DD_FAILED;
        }
    } else if (r == DD_CANCELLED) {
        dd__msg(j->cb, DD_MSG_WARNING, L"verification of '%ls' stopped", j->out.name);
    }
    j->st->phase_elapsed = phase_elapsed(j);
    dd__sha256_free(s);
    dd__sha256_free(raw);
    return r;
}

/* ---- running ----------------------------------------------------------- */

static dd_result open_input(job *j)
{
    const dd_options *opt = j->opt;
    DWORD e;

    e = ep_open_input(&j->in, opt->input, opt->iflags);
    if (opt->input_name)
        wcsncpy_s(j->in.name, sizeof j->in.name / sizeof *j->in.name, opt->input_name, _TRUNCATE);
    if (e) {
        report(j, L"failed to open", &j->in, e);
        return DD_FAILED;
    }
    if (!opt->xz_raw) {
        bool xz;
        wchar_t a[DD_SIZE_EXACT_CHARS], b[DD_SIZE_EXACT_CHARS];
        /* compressing too, the decoder gets a quarter of the memory */
        e = ep_detect_xz(&j->in, &xz, opt->xz_level >= 0 ? j->xz_budget / 4 : j->xz_budget);
        if (e) {
            report(j, L"cannot decompress", &j->in, e);
            return DD_FAILED;
        }
        if (xz && opt->status != DD_STATUS_NONE) {
            dd_format_size_exact(j->in.xz_file_size, false, false, a, DD_SIZE_EXACT_CHARS);
            dd_format_size_exact(j->in.size, false, false, b, DD_SIZE_EXACT_CHARS);
            dd__msg(j->cb, DD_MSG_INFO, L"'%ls' is xz-compressed: %ls, %ls uncompressed; decompressing",
                    j->in.name, a, b);
        }
    }
    return DD_OK;
}

/* What sparse reading will read of each volume. */
static void report_sparse(job *j, const dd_fs_report *rep)
{
    wchar_t a[32], b[32], label[64];
    size_t i;

    for (i = 0; i < rep->count; i++) {
        const dd_fs_volume *v = &rep->vols[i];
        if (v->partition)
            _snwprintf_s(label, 64, _TRUNCATE, L"partition %d", v->partition);
        else
            wcscpy_s(label, 64, L"the input");
        if (v->size <= 0) {
            dd__msg(j->cb, DD_MSG_INFO, L"%ls: %ls", label, v->note);
            continue;
        }
        dd_format_size(v->used, false, a, 32);
        dd_format_size(v->size, false, b, 32);
        if (v->sparse)
            dd__msg(j->cb, v->unclean ? DD_MSG_WARNING : DD_MSG_INFO, L"%ls (%ls): reading the %ls in use of %ls%ls%ls",
                    label, v->type, a, b, v->note[0] ? L"; " : L"", v->note);
        else if (v->type[0])
            dd__msg(j->cb, DD_MSG_INFO, L"%ls (%ls): reading all %ls: %ls", label, v->type, b, v->note);
        else
            dd__msg(j->cb, DD_MSG_INFO, L"%ls: no FAT, exFAT, NTFS, ext2/3/4 or swap found; reading all %ls%ls%ls",
                    label, b, v->note[0] ? L"; " : L"", v->note);
    }
    dd_format_size(rep->read, false, a, 32);
    dd_format_size(rep->size, false, b, 32);
    dd__msg(j->cb, DD_MSG_INFO, L"sparse read: reading %ls of %ls; the rest is free space, written as zeros", a, b);
}

/* A compressed input: its free space comes from its sparse map, which must
   be this image's.  Its sizes are checked now, and the hash of the .xz
   file as it is read (the input is drained after the copy if need be). */
static dd_result setup_sparse_stream(job *j, dd_fs_report *rep)
{
    const dd_options *opt = j->opt;
    dd__map_id id;
    wchar_t m[256];
    DWORD e;

    if (!dd_sparse_map_name(opt->input, j->map_path, HASH_PATH)) {
        dd__msg(j->cb, DD_MSG_ERROR, L"'%ls' is xz-compressed, and has no name for its sparse map", j->in.name);
        return DD_FAILED;
    }
    j->fsmap = dd__fsmap_load(j->map_path, &id, rep, &e);
    if (!j->fsmap) {
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            dd__msg(j->cb, DD_MSG_ERROR,
                    L"'%ls' is xz-compressed, so it is read in order, and --sparse-read needs its file systems mapped first: "
                    L"there is no sparse map %ls; make it with dd --make-sparse-map if=\"%ls\"%ls (or the GUI's Make sparse map button)",
                    j->in.name, j->map_path, opt->input, opt->sparse_unclean ? L" --allow-unclean" : L"");
            return DD_FAILED;
        }
        dd_errstr(e, m, 256);
        dd__msg(j->cb, DD_MSG_ERROR, L"cannot use the sparse map %ls: %ls", j->map_path,
                e == ERROR_INVALID_DATA ? L"it is damaged; make it again" : m);
        return DD_FAILED;
    }
    if (id.file_size != j->in.xz_file_size || id.image_size != j->in.size) {
        dd__msg(j->cb, DD_MSG_ERROR, L"the sparse map %ls is for another image than '%ls'; make it again",
                j->map_path, j->in.name);
        return DD_FAILED;
    }
    if (id.unclean && !opt->sparse_unclean) {
        dd__msg(j->cb, DD_MSG_ERROR,
                L"the sparse map %ls was made with --allow-unclean; give --allow-unclean too, or make the map again without it",
                j->map_path);
        return DD_FAILED;
    }
    memcpy(j->map_sha, id.sha256, 32);
    if (j->in_raw_want) {
        if (memcmp(j->in_raw_want, j->map_sha, 32) != 0) {
            dd__msg(j->cb, DD_MSG_ERROR, L"the sparse map %ls and the hash file %ls disagree about '%ls'; one of them is for another image",
                    j->map_path, j->in_hashes, j->in.name);
            return DD_FAILED;
        }
    } else {
        if ((j->in_raw_hash = dd__sha256_new()) == NULL)
            return no_memory(j);
        dd__xz_set_hash(j->in.xz, j->in_raw_hash);
        j->in_raw_want = j->map_sha;
        j->raw_want_from = j->map_path;
    }
    j->sparse_stream = true;
    if (opt->status != DD_STATUS_NONE)
        dd__msg(j->cb, DD_MSG_INFO, L"'%ls' is read in order; its free space, from the sparse map %ls, is written as zeros",
                j->in.name, j->map_path);
    return DD_OK;
}

/* sparse_read: maps the input's file systems, and says what will be read. */
static dd_result setup_sparse(job *j)
{
    const bool quiet = j->opt->status == DD_STATUS_NONE;
    dd_fs_report *rep;
    dd__fs_source src;
    dd_result res = DD_OK;
    size_t i;
    DWORD e;

    if (!j->in.xz && (!ep_seekable(&j->in) || j->in.size < 0)) {
        dd__msg(j->cb, DD_MSG_ERROR, L"--sparse-read needs an image file or a device as input; '%ls' is neither", j->in.name);
        return DD_FAILED;
    }
    rep = malloc(sizeof *rep);
    if (!rep)
        return no_memory(j);
    if (j->in.xz) {
        res = setup_sparse_stream(j, rep);
    } else {
        memset(&src, 0, sizeof src);
        src.h = j->in.h;
        src.size = j->in.size;
        src.device = j->in.kind == EP_BLOCK;
        src.is_volume = j->in.is_volume;
        src.locked = j->opt->lock_input ? &j->in : NULL;
        j->fsmap = dd__fsmap_build(&src, j->opt->sparse_unclean, rep, &e);
        if (!j->fsmap)
            res = no_memory(j);
    }
    if (res != DD_OK) {
        free(rep);
        return res;
    }
    if (!quiet) {
        report_sparse(j, rep);
        if (j->in.kind == EP_BLOCK && !j->opt->lock_input) {
            bool unclean = false;
            for (i = 0; i < rep->count; i++)
                unclean = unclean || (rep->vols[i].unclean && !rep->vols[i].sparse);
            dd__msg(j->cb, DD_MSG_WARNING,
                    L"the file systems on '%ls' were mapped before reading them; if they are mounted, --lock-input stops them changing in between",
                    j->in.name);
            if (unclean)
                dd__msg(j->cb, DD_MSG_INFO,
                        L"Windows marks a FAT volume it has mounted as not clean while it has changes to write; with --lock-input it writes them, and the mark is ignored");
        }
    }
    free(rep);
    return DD_OK;
}

/* --verify-hashes: reads the image and checks it against its .hashes file. */
static dd_result check_only(job *j)
{
    dd_result res = open_input(j);

    if (res == DD_OK)
        res = setup_input_check(j, true);
    if (res != DD_OK)
        return res;
    if (!get_vbuf(j))
        return DD_FAILED;
    j->st->started = true;
    QueryPerformanceFrequency(&j->freq);
    QueryPerformanceCounter(&j->start);
    start_phase(j, DD_PHASE_CHECK, j->in.size);
    res = drain_input(j);
    j->st->phase_elapsed = phase_elapsed(j);
    j->st->elapsed = elapsed(j);
    return res == DD_OK ? check_input(j) : res;
}

static dd_result run_job(job *j)
{
    const dd_options *opt = j->opt;
    int64_t skip_bytes, seek_bytes;
    dd_result res;
    bool out_is_file;
    DWORD e;

    res = open_input(j);
    if (res == DD_OK)
        res = setup_input_check(j, false);
    if (res != DD_OK)
        return res;
    if (opt->lock_input && ep_lock_input(&j->in, j->cb, opt->keep_letters))
        return DD_FAILED;
    /* after the lock, which flushes the file systems */
    if (opt->sparse_read && setup_sparse(j) != DD_OK)
        return DD_FAILED;

    if (!filter_precheck(j)) {
        filter_refused(j);
        return DD_FAILED;
    }
    e = ep_open_output(&j->out, opt->output, opt->oflags, opt->conv);
    if (opt->output_name)
        wcsncpy_s(j->out.name, sizeof j->out.name / sizeof *j->out.name, opt->output_name, _TRUNCATE);
    if (e) {
        report(j, L"failed to open", &j->out, e);
        if (e == ERROR_ACCESS_DENIED && opt->lock_input && j->in.kind == EP_BLOCK)
            dd__msg(j->cb, DD_MSG_INFO, L"is '%ls' on a volume locked for reading the input?", j->out.name);
        return DD_FAILED;
    }
    out_is_file = j->out.kind == EP_FILE;
    if (!filter_match(&j->out, opt->filter)) {
        filter_refused(j);
        return DD_FAILED;
    }
    if (opt->xz_level >= 0) {
        uint64_t decoding = j->in.xz ? dd__xz_memusage(j->in.xz) : 0;
        uint64_t limit = j->xz_budget > decoding ? j->xz_budget - decoding : 0;
        e = ep_start_compress(&j->out, opt->xz_level, limit);
        if (e) {
            report(j, L"cannot compress to", &j->out, e);
            return DD_FAILED;
        }
        if (opt->status != DD_STATUS_NONE && j->out.xz) {
            int level = dd__xz_level(j->out.xz);
            uint32_t threads = ep_xz_threads(&j->out);
            wchar_t need[32], may[32], uses[32], free[32], budget[32], reserve[32], dec[32], how[256];
            dd_format_size((int64_t)dd__xz_encoder_memusage(opt->xz_level, 1), false, need, 32);
            dd_format_size((int64_t)limit, false, may, 32);
            dd_format_size((int64_t)dd__xz_memusage(j->out.xz), false, uses, 32);
            dd_format_size((int64_t)j->xz_free, false, free, 32);
            dd_format_size((int64_t)j->xz_budget, false, budget, 32);
            dd_format_size((int64_t)j->xz_reserve, false, reserve, 32);
            dd_format_size((int64_t)decoding, false, dec, 32);
            /* where the budget comes from */
            if (opt->xz_memory > 0)
                _snwprintf_s(how, 256, _TRUNCATE, L"%ls was free before starting, and --xz-memory lets dd use %ls", free, budget);
            else if (j->xz_budget_floor)
                _snwprintf_s(how, 256, _TRUNCATE, L"%ls was free before starting, no more than the %ls to keep free, so dd may use a quarter of it, %ls",
                             free, reserve, budget);
            else
                _snwprintf_s(how, 256, _TRUNCATE, L"%ls was free before starting, and dd may use %ls of it, keeping %ls free (%ls)",
                             free, budget, reserve, j->xz_reserve == XZ_RESERVE_SWAP ? L"there is a page file" : L"there is no page file");
            if (decoding)
                _snwprintf_s(how + wcslen(how), 256 - wcslen(how), _TRUNCATE, L"; decompressing the input has %ls of that", dec);
            if (level != opt->xz_level)
                dd__msg(j->cb, DD_MSG_WARNING,
                        L"xz level %d needs %ls of memory even on one thread, more than the %ls compressing may use; compressing at level %d instead",
                        opt->xz_level, need, may, level);
            if (dd__xz_memusage(j->out.xz) > limit)
                dd__msg(j->cb, DD_MSG_WARNING,
                        L"even xz level 0 on one thread needs %ls of memory, more than the %ls compressing may use; it uses it anyway",
                        uses, may);
            dd__msg(j->cb, DD_MSG_INFO, L"compressing to xz at level %d on %lu thread%ls, using up to %ls of memory; %ls",
                    level, threads, threads == 1 ? L"" : L"s", uses, how);
        }
    }
    if (setup_output(j, out_is_file) != DD_OK)
        return DD_FAILED;
    if (j->layout && check_fit(j) != DD_OK)
        return DD_FAILED;
    if (ep_lock_output(&j->out, j->cb, opt->force_dismount, opt->keep_letters))
        return DD_FAILED;

    j->ibuf = alloc_buffer(j->ibs);
    if (!j->direct) {
        j->obuf = alloc_buffer(j->obs);
        j->sbuf = alloc_buffer(j->ibs + 1);
    }
    if (!j->ibuf || (!j->direct && (!j->obuf || !j->sbuf))) {
        dd__msg(j->cb, DD_MSG_ERROR, L"memory exhausted");
        return DD_FAILED;
    }

    j->st->started = true;
    QueryPerformanceFrequency(&j->freq);
    QueryPerformanceCounter(&j->start);

    if (opt->iflags & DD_FLAG_SKIP_BYTES)
        skip_bytes = opt->skip;
    else if (!mul64(opt->skip, j->ibs, &skip_bytes)) {
        dd__msg(j->cb, DD_MSG_ERROR, L"'%ls': cannot skip: Value too large", j->in.name);
        return DD_FAILED;
    }
    if (opt->oflags & DD_FLAG_SEEK_BYTES)
        seek_bytes = opt->seek;
    else if (!mul64(opt->seek, j->obs, &seek_bytes)) {
        dd__msg(j->cb, DD_MSG_ERROR, L"'%ls': cannot seek: Value too large", j->out.name);
        return DD_FAILED;
    }

    res = skip_input(j, skip_bytes);
    if (res != DD_OK)
        return res;
    res = seek_output(j, seek_bytes);
    if (res != DD_OK)
        return res;
    j->out_start = j->out.pos;

    j->st->bytes_total = expected_total(j);
    res = copy_loop(j);
    if (j->fsmap && opt->status != DD_STATUS_NONE) {
        wchar_t a[DD_SIZE_EXACT_CHARS];
        dd_format_size_exact(j->st->bytes_unused, false, false, a, DD_SIZE_EXACT_CHARS);
        dd__msg(j->cb, DD_MSG_INFO, L"sparse read: %ls of free space written as zeros", a);
    }

    /* the input's hashes cover all of it, so read what the copy left */
    if (res == DD_OK && (j->in_hash || j->in_raw_hash)) {
        start_phase(j, DD_PHASE_CHECK, j->in.size >= 0 ? j->in.size - j->st->bytes_read : -1);
        res = drain_input(j);
        if (res == DD_OK)
            res = check_input(j);
    }
    return res;
}

dd_result dd_run(const dd_options *opt, const dd_callbacks *cb, dd_cancel *cancel, dd_stats *stats)
{
    dd_stats local;
    wchar_t err[512];
    dd_options fitted;
    dd_layout *layout = NULL;
    dd_result res;
    job j;

    if (!stats)
        stats = &local;
    memset(stats, 0, sizeof *stats);
    stats->bytes_total = -1;
    stats->xz_bytes = -1;
    stats->xz_done = -1;

    if (!dd__validate(opt, err, sizeof err / sizeof *err)) {
        dd__msg(cb, DD_MSG_ERROR, L"%ls", err);
        return DD_FAILED;
    }

    /* fit: copy the image only up to the end of its data */
    if (opt->fit) {
        layout = malloc(sizeof *layout);
        if (!layout) {
            dd__msg(cb, DD_MSG_ERROR, L"memory exhausted");
            return DD_FAILED;
        }
        if (!dd_read_layout(opt->input, layout, err, sizeof err / sizeof *err)) {
            dd__msg(cb, DD_MSG_ERROR, L"%ls", err);
            free(layout);
            return DD_FAILED;
        }
        fitted = *opt;
        if (layout->scheme != DD_SCHEME_NONE) {
            fitted.count = layout->data_end;
            fitted.iflags |= DD_FLAG_COUNT_BYTES;
        }
        opt = &fitted;
    }

    memset(&j, 0, sizeof j);
    j.layout = layout;
    j.opt = opt;
    j.cb = cb;
    j.cancel = cancel;
    j.st = stats;
    j.ibs = (DWORD)(opt->bs ? opt->bs : opt->ibs ? opt->ibs : 512);
    j.obs = (DWORD)(opt->bs ? opt->bs : opt->obs ? opt->obs : 512);
    j.direct = opt->bs > 0 && !(opt->conv & (DD_CONV_SWAB | DD_CONV_LCASE | DD_CONV_UCASE));
    j.saved_char = -1;
    /* All of what is free, but 4 GiB for everything else running (2 GiB if
       there is a page file to fall back on); with no more than that free, a
       quarter of it. */
    j.xz_free = dd__xz_memory_available();
    j.xz_reserve = dd__xz_page_file() ? XZ_RESERVE_SWAP : XZ_RESERVE;
    if (opt->xz_memory > 0) {
        j.xz_budget = (uint64_t)opt->xz_memory;
    } else {
        j.xz_budget_floor = j.xz_free <= j.xz_reserve;
        j.xz_budget = j.xz_budget_floor ? j.xz_free / 4 : j.xz_free - j.xz_reserve;
    }
    j.warn_partial = j.direct && !(opt->iflags & DD_FLAG_FULLBLOCK) &&
                     (opt->skip > 0 || opt->count > 0 || ((opt->iflags | opt->oflags) & DD_FLAG_DIRECT));
    ep_init(&j.in);
    ep_init(&j.out);

    cancel_attach(cancel);
    res = opt->check_hashes ? check_only(&j) : run_job(&j);
    if (j.out.xz) {
        /* end the .xz stream even after an error or Ctrl+C, so what was
           written so far is a valid (if shorter) image */
        int64_t compressed;
        DWORD e = ep_finish_compress(&j.out, &compressed, stats->started ? finish_tick : NULL, &j);
        if (!e && stats->started) {
            stats->xz_bytes = compressed;
            stats->xz_done = dd__xz_compressed_in(j.out.xz);
        }
        if (e) {
            report(&j, L"error compressing to", &j.out, e);
            if (res == DD_OK)
                res = DD_FAILED;
        } else if (stats->started && opt->status != DD_STATUS_NONE) {
            wchar_t a[DD_SIZE_EXACT_CHARS], b[DD_SIZE_EXACT_CHARS];
            int64_t total = stats->bytes_written;
            dd_format_size_exact(total, false, false, a, DD_SIZE_EXACT_CHARS);
            dd_format_size_exact(compressed, false, false, b, DD_SIZE_EXACT_CHARS);
            dd__msg(cb, DD_MSG_INFO, L"compressed %ls to %ls (%.1f%%)", a, b,
                    total > 0 ? 100.0 * (double)compressed / (double)total : 0.0);
        }
        if (!e && (opt->conv & (DD_CONV_FSYNC | DD_CONV_FDATASYNC)) && (e = ep_flush(&j.out)) != 0) {
            report(&j, L"fsync failed for", &j.out, e);
            res = DD_FAILED;
        }
    }
    if (j.in.xz && !j.out.xz && stats->started && !opt->check_hashes &&
        opt->status != DD_STATUS_NONE && stats->xz_bytes >= 0) {
        /* the counterpart of "compressed ... to ..." */
        wchar_t a[DD_SIZE_EXACT_CHARS], b[DD_SIZE_EXACT_CHARS];
        dd_format_size_exact(stats->bytes_read, false, false, a, DD_SIZE_EXACT_CHARS);
        dd_format_size_exact(stats->xz_bytes, false, false, b, DD_SIZE_EXACT_CHARS);
        dd__msg(cb, DD_MSG_INFO, L"decompressed %ls from %ls", a, b);
    }
    if (stats->started && stats->phase == DD_PHASE_COPY)
        stats->elapsed = elapsed(&j);
    if (res == DD_OK && j.make_hashes)
        res = write_hashes(&j);
    if (res == DD_OK && opt->verify) {
        /* the input is read: its decoder's memory goes to the verifier's */
        dd__xz_free(j.in.xz);
        j.in.xz = NULL;
        res = verify_output(&j);
    }
    ep_close(&j.in);
    ep_close(&j.out);
    cancel_detach(cancel);

    if (j.ibuf)
        VirtualFree(j.ibuf, 0, MEM_RELEASE);
    if (j.obuf)
        VirtualFree(j.obuf, 0, MEM_RELEASE);
    if (j.sbuf)
        VirtualFree(j.sbuf, 0, MEM_RELEASE);
    if (j.vbuf)
        VirtualFree(j.vbuf, 0, MEM_RELEASE);
    dd__sha256_free(j.in_hash);
    dd__sha256_free(j.in_raw_hash);
    dd__sha256_free(j.out_hash);
    dd__sha256_free(j.out_raw_hash);
    dd__gpt_fix_free(&j.gpt);
    dd__fsmap_free(j.fsmap);
    free(layout);
    return res;
}
