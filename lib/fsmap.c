/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later

    The used-space map of an input, for sparse reading: which bytes the
    file systems on it hold data in.  The input's partition table gives the
    volumes (or the input is one volume); each is offered to the drivers in
    drivers[] (see fs.h).  Every byte no driver vouches for is read.

    A file system is read whole, whatever its driver says, when it might
    share bytes with something else: it overlaps another partition, or it
    lies inside an ISO 9660 file system (hybrid ISOs keep an EFI image's
    FAT inside an ISO file, so the FAT's free space is file data there).

    A compressed image is read in order, but mapping needs metadata from
    anywhere, so it is mapped first (dd__fsmap_build_xz) through a second
    decoder that seeks, decompressing only the .xz blocks the file systems
    need.  The map is kept in memory for that run only, and the file is held
    against writers from mapping to the end of the copy (dd__fs_hold), so
    the map is always the image's.
*/
#include "fs.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define GRAIN    4096u              /* reads are aligned to this, for devices */
#define WINDOW   (1u << 20)         /* the read cache */
#define MIN_READ (64u << 10)        /* a small read fetches this much */


/* The order matters: the first driver whose probe says yes maps the volume. */
static const dd__fs_driver *const drivers[] = {
    &dd__fs_exfat,          /* before FAT: both have a boot sector at 0 */
    &dd__fs_fat,
    &dd__fs_ntfs,
    &dd__fs_ext,
    &dd__fs_swap,
};

/* ---- reading a volume -------------------------------------------------- */

struct dd__fs_vol {
    const dd__fs_source *src;
    int64_t  base, size;      /* the volume in the input */
    bool     os_clean;        /* see dd__fs_vol_os_clean */
    uint8_t *buf;             /* src->h: WINDOW bytes, page aligned: a cache */
    int64_t  buf_off;         /*   of the input from here */
    DWORD    buf_len;
};

int64_t dd__fs_vol_size(const dd__fs_vol *v)
{
    return v->size;
}

bool dd__fs_vol_os_clean(const dd__fs_vol *v)
{
    return v->os_clean;
}

DWORD dd__fs_read(dd__fs_vol *v, int64_t off, void *buf, size_t len)
{
    const dd__fs_source *src = v->src;
    uint8_t *out = buf;
    int64_t at, end;

    if (off < 0 || off > v->size || len > (uint64_t)(v->size - off))
        return ERROR_HANDLE_EOF;
    at = v->base + off;
    end = at + (int64_t)len;
    if (src->rd)
        return len ? src->rd(src->rd_ctx, at, buf, len) : 0;
    while (at < end) {
        OVERLAPPED ov;
        int64_t a, b;
        DWORD got;

        if (v->buf_len && at >= v->buf_off && at < v->buf_off + v->buf_len) {
            int64_t stop = v->buf_off + v->buf_len < end ? v->buf_off + v->buf_len : end;
            memcpy(out, v->buf + (at - v->buf_off), (size_t)(stop - at));
            out += stop - at;
            at = stop;
            continue;
        }
        a = at - at % GRAIN;
        b = end - a < MIN_READ ? a + MIN_READ : end;
        b = (b + GRAIN - 1) / GRAIN * GRAIN;
        if (b - a > WINDOW)
            b = a + WINDOW;
        if (src->device && b > src->size)
            b = src->size;              /* a whole number of sectors */
        memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)a;
        ov.OffsetHigh = (DWORD)((uint64_t)a >> 32);
        v->buf_len = 0;
        if (!ReadFile(src->h, v->buf, (DWORD)(b - a), &got, &ov))
            return GetLastError();
        if (got <= (DWORD)(at - a))
            return ERROR_HANDLE_EOF;
        v->buf_off = a;
        v->buf_len = got;
    }
    return 0;
}

/* ---- helpers for drivers ----------------------------------------------- */

bool dd__fs_alloc_bits(dd__fs_map *m, uint64_t nbits)
{
    uint64_t bytes = (nbits + 63) / 64 * 8;     /* whole words, for run scans */

    free(m->bits);
    m->bits = NULL;
    m->nbits = 0;
    if (bytes > (uint64_t)SIZE_MAX / 2)
        return false;
    m->bits = calloc(1, bytes ? (size_t)bytes : 8);
    if (m->bits)
        m->nbits = nbits;
    return m->bits != NULL;
}

void dd__fs_set_bits(dd__fs_map *m, uint64_t first, uint64_t count)
{
    uint64_t i = first, end;

    if (first >= m->nbits)
        return;
    end = count > m->nbits - first ? m->nbits : first + count;
    for (; i < end && (i & 7); i++)
        m->bits[i >> 3] |= (uint8_t)(1u << (i & 7));
    if (end - i >= 8) {
        memset(m->bits + (i >> 3), 0xFF, (size_t)((end - i) >> 3));
        i += (end - i) & ~(uint64_t)7;
    }
    for (; i < end; i++)
        m->bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}

void dd__fs_note(dd__fs_map *m, const wchar_t *fmt, ...)
{
    va_list ap;

    if (m->note[0])
        return;     /* the first problem is the one worth reporting */
    va_start(ap, fmt);
    _vsnwprintf_s(m->note, sizeof m->note / sizeof *m->note, _TRUNCATE, fmt, ap);
    va_end(ap);
}

/* ---- the map ----------------------------------------------------------- */

/* A file system's place in the input, with its bits */
typedef struct region {
    int64_t  start, end;
    int64_t  bits_start, bits_end; /* where the bits apply */
    uint32_t unit;
    uint8_t *bits;
} region;

struct dd__fsmap {
    size_t  n, cap;
    region *r;                     /* sorted by start; they never overlap */
};

void dd__fsmap_free(dd__fsmap *fm)
{
    size_t i;

    if (!fm)
        return;
    for (i = 0; i < fm->n; i++)
        free(fm->r[i].bits);
    free(fm->r);
    free(fm);
}

static region *add_region(dd__fsmap *fm)
{
    if (fm->n == fm->cap) {
        size_t cap = fm->cap ? fm->cap * 2 : 64;
        region *r = realloc(fm->r, cap * sizeof *r);
        if (!r)
            return NULL;
        fm->r = r;
        fm->cap = cap;
    }
    memset(&fm->r[fm->n], 0, sizeof *fm->r);
    return &fm->r[fm->n++];
}

/* The first bit in [i, end) that is not state, or end. */
static uint64_t scan_bits(const uint8_t *b, uint64_t i, uint64_t end, bool state)
{
    const uint64_t word = state ? ~(uint64_t)0 : 0;
    const uint8_t byte = state ? 0xFF : 0x00;

    while (i < end) {
        if ((i & 63) == 0 && end - i >= 64) {
            uint64_t w;
            memcpy(&w, b + (i >> 3), 8);
            if (w == word) {
                i += 64;
                continue;
            }
        }
        if ((i & 7) == 0 && end - i >= 8 && b[i >> 3] == byte) {
            i += 8;
            continue;
        }
        if ((bool)((b[i >> 3] >> (i & 7)) & 1) != state)
            return i;
        i++;
    }
    return end;
}

int64_t dd__fsmap_run(const dd__fsmap *fm, int64_t pos, int64_t max, bool *used)
{
    const region *r;
    size_t lo = 0, hi = fm->n;
    uint64_t i, last, j;
    int64_t limit, stop;
    bool state;

    while (lo < hi) {               /* the first region ending after pos */
        size_t mid = (lo + hi) / 2;
        if (fm->r[mid].end <= pos)
            lo = mid + 1;
        else
            hi = mid;
    }
    *used = true;
    if (lo == fm->n)
        return max;
    r = &fm->r[lo];
    if (pos < r->start)
        return r->start - pos < max ? r->start - pos : max;
    if (pos < r->bits_start)
        return r->bits_start - pos < max ? r->bits_start - pos : max;
    if (pos >= r->bits_end)
        return r->end - pos < max ? r->end - pos : max;

    limit = r->bits_end - pos < max ? r->bits_end : pos + max;
    i = (uint64_t)(pos - r->bits_start) / r->unit;
    last = (uint64_t)(limit - 1 - r->bits_start) / r->unit;
    state = (r->bits[i >> 3] >> (i & 7)) & 1;
    j = scan_bits(r->bits, i + 1, last + 1, state);
    stop = r->bits_start + (int64_t)(j * r->unit);
    if (stop > limit)
        stop = limit;
    *used = state;
    return stop - pos;
}

static int64_t used_bytes(const dd__fsmap *fm, int64_t from, int64_t to)
{
    int64_t total = 0;

    while (from < to) {
        bool used;
        int64_t n = dd__fsmap_run(fm, from, to - from, &used);
        if (used)
            total += n;
        from += n;
    }
    return total;
}

/* ---- building it ------------------------------------------------------- */

typedef struct vol {
    dd_fs_volume *rep;
    dd__fs_map    m;
    bool          mapped;
    const wchar_t *container;   /* its partition type says it holds one */
} vol;

static void whole(vol *v, const wchar_t *fmt, ...)
{
    va_list ap;

    free(v->m.bits);
    v->m.bits = NULL;
    v->mapped = false;
    va_start(ap, fmt);
    _vsnwprintf_s(v->rep->note, sizeof v->rep->note / sizeof *v->rep->note, _TRUNCATE, fmt, ap);
    va_end(ap);
}

/* Encryption, RAID and volume managers write their own header and leave
   the rest of the old file system's first blocks as they were, so a stale
   superblock can still be found there; its block map describes nothing
   now.  These are read whole, whatever else the volume seems to hold. */
#define MD_MAGIC 0xa92b4efcu

static const wchar_t *container(dd__fs_vol *fv, const uint8_t *head)
{
    static const uint8_t luks[6] = { 'L', 'U', 'K', 'S', 0xBA, 0xBE };
    /* {4967D63B-2E29-4AD8-8399-F6A339E3D001}: BitLocker, and BitLocker To Go */
    static const uint8_t bitlocker[16] = { 0x3B, 0xD6, 0x67, 0x49, 0x29, 0x2E, 0xD8, 0x4A,
                                           0x83, 0x99, 0xF6, 0xA3, 0x39, 0xE3, 0xD0, 0x01 };
    static const uint8_t bcache[16] = { 0xC6, 0x85, 0x73, 0xF6, 0x4E, 0x1A, 0x45, 0xCA,
                                        0x82, 0x65, 0xF5, 0x7F, 0x48, 0xBA, 0x6D, 0x81 };
    int64_t sectors = fv->size >> 9, md_end[2];
    uint8_t b[16];
    int i;

    if (memcmp(head, luks, sizeof luks) == 0)
        return L"LUKS";
    if (memcmp(head + 3, "-FVE-FS-", 8) == 0 || memcmp(head + 0xA0, bitlocker, 16) == 0 ||
        memcmp(head + 0x1A8, bitlocker, 16) == 0)
        return L"BitLocker";
    for (i = 0; i < 4; i++)
        if (memcmp(head + 512 * i, "LABELONE", 8) == 0)
            return L"LVM";
    if (dd__le32(head) == MD_MAGIC || (dd__fs_read(fv, 4096, b, 4) == 0 && dd__le32(b) == MD_MAGIC))
        return L"Linux RAID";
    md_end[0] = ((sectors - 16) & ~(int64_t)7) << 9;            /* metadata 1.0 */
    md_end[1] = ((sectors & ~(int64_t)127) - 128) << 9;         /* metadata 0.90 */
    for (i = 0; i < 2; i++)
        if (md_end[i] >= 4096 && dd__fs_read(fv, md_end[i], b, 4) == 0 && dd__le32(b) == MD_MAGIC)
            return L"Linux RAID";
    if (dd__fs_read(fv, 4096 + 24, b, 16) == 0 && memcmp(b, bcache, 16) == 0)
        return L"bcache";
    return NULL;
}

static const wchar_t *container_type(const dd_partition *p)
{
    static const struct { GUID g; const wchar_t *name; } types[] = {
        { { 0xCA7D7CCB, 0x63ED, 0x4C53, { 0x86, 0x1C, 0x17, 0x42, 0x53, 0x60, 0x59, 0xCC } }, L"LUKS" },
        { { 0xA19D880F, 0x05FC, 0x4D3B, { 0xA0, 0x06, 0x74, 0x3F, 0x0F, 0x84, 0x91, 0x1E } }, L"Linux RAID" },
        { { 0xE6D6D379, 0xF507, 0x44C2, { 0xA2, 0x3C, 0x23, 0x8F, 0x2A, 0x3D, 0xF9, 0x28 } }, L"LVM" },
    };
    size_t i;

    switch (p->mbr_type) {
    case 0xFD: return L"Linux RAID";
    case 0x8E: return L"LVM";
    case 0xE8: return L"LUKS";
    }
    for (i = 0; i < sizeof types / sizeof *types; i++)
        if (memcmp(&p->type, &types[i].g, sizeof p->type) == 0)
            return types[i].name;
    return NULL;
}

static void map_volume(dd__fs_vol *fv, vol *v, bool allow_unclean)
{
    uint8_t head[DD__FS_HEAD];
    size_t n = fv->size < DD__FS_HEAD ? (size_t)fv->size : DD__FS_HEAD, d;
    dd_fs_volume *rep = v->rep;
    dd__fs_status st;

    memset(head, 0, sizeof head);
    if (n < 512 || dd__fs_read(fv, 0, head, n) != 0) {
        whole(v, L"its first sectors cannot be read");
        return;
    }
    if (v->container || (v->container = container(fv, head)) != NULL) {
        whole(v, L"it holds %ls, not a file system dd can map", v->container);
        return;
    }
    for (d = 0; d < sizeof drivers / sizeof *drivers; d++) {
        if (!drivers[d]->probe(head, fv->size))
            continue;
        memset(&v->m, 0, sizeof v->m);
        st = drivers[d]->map(fv, head, allow_unclean, &v->m);
        wcsncpy_s(rep->type, sizeof rep->type / sizeof *rep->type, v->m.type, _TRUNCATE);
        rep->unclean = v->m.unclean;
        if (st == DD__FS_MAPPED && v->m.unit && v->m.nbits <= (uint64_t)INT64_MAX / v->m.unit &&
            v->m.size > 0 && v->m.map_start >= 0 && (v->m.bits || !v->m.nbits)) {
            v->mapped = true;
            wcsncpy_s(rep->note, sizeof rep->note / sizeof *rep->note, v->m.note, _TRUNCATE);
            /* a wrong partition table, or an image cut short: its own
               idea of what is in use is not to be trusted */
            if (v->m.size > fv->size)
                whole(v, fv->base + fv->size == fv->src->size ? L"the file system runs past the end of the input"
                                                              : L"the file system is larger than its partition");
        } else if (v->m.unclean && !allow_unclean && !v->m.unclean_whole) {
            whole(v, L"%ls (--allow-unclean reads only its used space)",
                  v->m.note[0] ? v->m.note : L"it was not cleanly unmounted");
        } else if (v->m.note[0]) {
            whole(v, L"%ls", v->m.note);
        } else {
            whole(v, L"it cannot be mapped");
        }
        return;
    }
}

/* A volume Windows has mounted and dd has locked: whether Windows says it
   was clean when mounted (FSCTL_IS_VOLUME_DIRTY reports that, not the
   mark Windows itself sets while it has changes pending). */
static bool locked_clean(HANDLE h)
{
    ULONG flags = 0;
    DWORD ret;

    return DeviceIoControl(h, FSCTL_IS_VOLUME_DIRTY, NULL, 0, &flags, sizeof flags, &ret, NULL) &&
           !(flags & VOLUME_IS_DIRTY);
}

/* The locked volume that starts at offset start of the input disk. */
static bool os_clean_at(const dd_endpoint *in, int64_t start)
{
    BYTE buf[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)];
    const VOLUME_DISK_EXTENTS *ext = (const VOLUME_DISK_EXTENTS *)buf;
    DWORD ret;
    int i;

    for (i = 0; i < in->nlocked; i++)
        if (DeviceIoControl(in->locked[i], IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, buf, sizeof buf, &ret, NULL) &&
            ext->NumberOfDiskExtents == 1 && (LONG)ext->Extents[0].DiskNumber == in->disk_number &&
            ext->Extents[0].StartingOffset.QuadPart == start)
            return locked_clean(in->locked[i]);
    return false;
}

static bool os_clean(const dd__fs_source *src, int64_t start)
{
    const dd_endpoint *in = src->locked;

    if (!in || in->kind != EP_BLOCK)
        return false;
    return in->is_volume ? locked_clean(in->h) : in->whole_disk && os_clean_at(in, start);
}

static bool overlap(const dd_fs_volume *a, const dd_fs_volume *b)
{
    return a->size > 0 && b->size > 0 && a->start < b->start + b->size && b->start < a->start + a->size;
}

static int by_start(const void *a, const void *b)
{
    const region *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

dd__fsmap *dd__fsmap_build(const dd__fs_source *src, bool allow_unclean, dd_fs_report *rep, DWORD *err)
{
    dd__fsmap *fm = calloc(1, sizeof *fm);
    dd_layout *l = calloc(1, sizeof *l);
    vol *vols = calloc(DD_MAX_PARTITIONS, sizeof *vols);
    dd__fs_vol fv;
    size_t i, k;

    memset(rep, 0, sizeof *rep);
    memset(&fv, 0, sizeof fv);
    fv.src = src;
    if (!src->rd)
        fv.buf = VirtualAlloc(NULL, WINDOW, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!fm || !l || !vols || (!src->rd && !fv.buf)) {
        *err = ERROR_NOT_ENOUGH_MEMORY;
        free(fm);
        fm = NULL;
        goto out;
    }
    rep->size = src->size;

    /* the volumes: the partitions, or the whole input */
    if (!src->is_volume) {
        if (src->rd)
            dd__read_layout_reader(src->rd, src->rd_ctx, src->size, l);
        else
            dd__read_layout_handle(src->h, src->size, l);
    }
    if (src->is_volume || l->scheme == DD_SCHEME_NONE) {
        rep->vols[0].size = src->size;
        rep->count = 1;
    } else {
        for (i = 0; i < l->count; i++) {
            const dd_partition *p = &l->parts[i];
            dd_fs_volume *r = &rep->vols[rep->count];
            if (p->extended)
                continue;       /* its logical partitions are listed */
            r->partition = p->number;
            vols[rep->count].container = container_type(p);
            r->start = p->start;
            r->size = p->size;
            if (p->start >= src->size) {
                r->size = 0;
                wcscpy_s(r->note, sizeof r->note / sizeof *r->note, L"it lies past the end of the input");
            } else if (p->size > src->size - p->start) {
                r->size = src->size - p->start;
                wcscpy_s(r->note, sizeof r->note / sizeof *r->note, L"it runs past the end of the input");
            }
            rep->count++;
        }
    }

    for (i = 0; i < rep->count; i++) {
        vols[i].rep = &rep->vols[i];
        if (rep->vols[i].size <= 0)
            continue;
        fv.base = rep->vols[i].start;
        fv.size = rep->vols[i].size;
        fv.os_clean = os_clean(src, fv.base);
        map_volume(&fv, &vols[i], allow_unclean);
    }

    /* file systems that may share their bytes are read whole */
    for (i = 0; i < rep->count; i++) {
        if (!vols[i].mapped)
            continue;
        for (k = 0; k < rep->count; k++)
            if (k != i && overlap(&rep->vols[i], &rep->vols[k])) {
                whole(&vols[i], L"it overlaps partition %d", rep->vols[k].partition);
                break;
            }
        if (vols[i].mapped && l->iso_size > 0 && rep->vols[i].start < l->iso_size)
            whole(&vols[i], L"it lies inside an ISO 9660 file system, whose files may use its free space");
        /* a primary partition over the extended one: its free space may
           hold the tables of the logical partitions */
        for (k = 0; vols[i].mapped && k < l->count; k++) {
            const dd_partition *p = &l->parts[k];
            const dd_fs_volume *r = &rep->vols[i];
            if (p->extended && r->partition < 5 && r->start < p->start + p->size && p->start < r->start + r->size)
                whole(&vols[i], L"it overlaps the extended partition");
        }
    }

    for (i = 0; i < rep->count; i++) {
        region *r;
        const dd__fs_map *m = &vols[i].m;
        if (!vols[i].mapped)
            continue;
        if ((r = add_region(fm)) == NULL) {
            *err = ERROR_NOT_ENOUGH_MEMORY;
            dd__fsmap_free(fm);
            fm = NULL;
            goto out;
        }
        r->start = rep->vols[i].start;
        r->end = r->start + m->size;
        r->unit = m->unit;
        r->bits_start = r->start + (m->map_start < m->size ? m->map_start : m->size);
        /* map_volume checked that nbits * unit fits */
        r->bits_end = r->end - r->bits_start < (int64_t)m->nbits * m->unit ? r->end
                      : r->bits_start + (int64_t)m->nbits * m->unit;
        r->bits = vols[i].m.bits;
        vols[i].m.bits = NULL;          /* the region owns them now */
        rep->vols[i].sparse = true;
    }
    qsort(fm->r, fm->n, sizeof *fm->r, by_start);

    for (i = 0; i < rep->count; i++) {
        dd_fs_volume *r = &rep->vols[i];
        r->used = r->sparse ? used_bytes(fm, r->start, r->start + r->size) : r->size;
    }
    rep->read = used_bytes(fm, 0, src->size);
    *err = 0;

out:
    if (vols)
        for (i = 0; i < DD_MAX_PARTITIONS; i++)
            free(vols[i].m.bits);
    free(vols);
    free(l);
    if (fv.buf)
        VirtualFree(fv.buf, 0, MEM_RELEASE);
    return fm;
}

/* ---- mapping a compressed image ------------------------------------------ */

typedef struct mapper {
    dd__xz_seek        *z;
    const dd_callbacks *cb;
    dd_cancel          *cancel;
    dd_stats           *st;
    LARGE_INTEGER       freq, start;
    double              last;
    DWORD               error;      /* the first read that failed */
} mapper;

static double since(const mapper *mp)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - mp->start.QuadPart) / (double)mp->freq.QuadPart;
}

static void on_decoded(void *ctx, uint64_t decoded)
{
    mapper *mp = ctx;
    double now;

    mp->st->phase_done = (int64_t)decoded;
    if (!mp->cb || !mp->cb->progress || !mp->cb->progress_interval_ms)
        return;
    now = since(mp);
    if (now - mp->last >= mp->cb->progress_interval_ms / 1000.0) {
        mp->last = now;
        mp->st->phase_elapsed = now;
        mp->cb->progress(mp->cb->ctx, mp->st);
    }
}

static DWORD read_xz(void *ctx, int64_t off, void *buf, size_t len)
{
    mapper *mp = ctx;
    DWORD e;

    if (mp->cancel && mp->cancel->requested)
        return ERROR_OPERATION_ABORTED;
    e = dd__xz_seek_read(mp->z, off, buf, len);
    if (e && !mp->error)
        mp->error = e;
    return e;
}

HANDLE dd__fs_hold(const wchar_t *path, DWORD *err)
{
    wchar_t *norm = dd__normalize_path(path);
    HANDLE h;

    if (!norm) {
        *err = ERROR_NOT_ENOUGH_MEMORY;
        return INVALID_HANDLE_VALUE;
    }
    /* readers only: refused while anyone has it open to write */
    h = CreateFileW(norm, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    *err = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    free(norm);
    return h;
}

dd__fsmap *dd__fsmap_build_xz(HANDLE h, int64_t image_size, bool allow_unclean, const dd_callbacks *cb,
                              dd_cancel *cancel, dd_stats *st, dd_fs_report *rep, DWORD *err)
{
    dd_stats local;
    dd__fsmap *fm = NULL;
    dd__fs_source src;
    LARGE_INTEGER fsize;
    mapper mp;

    *err = 0;
    if (!st)
        st = &local;
    memset(&mp, 0, sizeof mp);
    mp.cb = cb;
    mp.cancel = cancel;
    mp.st = st;
    QueryPerformanceFrequency(&mp.freq);
    QueryPerformanceCounter(&mp.start);
    st->phase = DD_PHASE_MAP;
    st->phase_done = 0;
    st->phase_total = -1;
    st->phase_elapsed = 0;

    if (!GetFileSizeEx(h, &fsize)) {
        *err = GetLastError();
        goto out;
    }
    if ((*err = dd__xz_seek_open(&mp.z, h, fsize.QuadPart)) != 0)
        goto out;
    if (image_size >= 0 && dd__xz_seek_size(mp.z) != image_size) {
        *err = ERROR_INVALID_DATA;      /* not the image the caller opened */
        goto out;
    }
    dd__xz_seek_set_progress(mp.z, on_decoded, &mp);

    memset(&src, 0, sizeof src);
    src.rd = read_xz;
    src.rd_ctx = &mp;
    src.size = dd__xz_seek_size(mp.z);
    fm = dd__fsmap_build(&src, allow_unclean, rep, err);
    if (cancel && cancel->requested)
        *err = ERROR_OPERATION_ABORTED;
    /* inside a compressed file, any read that fails means it is damaged:
       a map made around the gap would not be the image's */
    else if (fm && mp.error)
        *err = mp.error;

out:
    st->phase_elapsed = since(&mp);
    if (cb && cb->progress && cb->progress_interval_ms)
        cb->progress(cb->ctx, st);
    if (*err) {
        dd__fsmap_free(fm);
        fm = NULL;
    }
    dd__xz_seek_free(mp.z);
    return fm;
}

bool dd_fs_report_read(const wchar_t *path, bool allow_unclean, const dd_callbacks *cb, dd_cancel *cancel,
                       dd_stats *stats, dd_fs_report *out, wchar_t *err, size_t errlen)
{
    dd_endpoint ep;
    dd__fs_source src;
    dd__fsmap *fm;
    wchar_t m[256];
    bool xz = false;
    DWORD e;

    memset(out, 0, sizeof *out);
    ep_init(&ep);
    e = ep_open_input(&ep, path, 0);
    if (!e)
        e = ep_detect_xz(&ep, &xz, dd__xz_memory_available() / 4);
    if (e) {
        dd_errstr(e, m, 256);
        _snwprintf_s(err, errlen, _TRUNCATE, L"cannot open '%ls': %ls", path, m);
        ep_close(&ep);
        return false;
    }
    if (xz) {
        int64_t image_size = ep.size;
        bool file = ep.own_handle;      /* xz is only detected in files */
        ep_close(&ep);
        if (!file) {
            _snwprintf_s(err, errlen, _TRUNCATE, L"'%ls' is xz-compressed and comes from standard input, so it cannot be mapped", path);
            return false;
        }
        HANDLE h = dd__fs_hold(path, &e);
        if (h == INVALID_HANDLE_VALUE) {
            dd_errstr(e, m, 256);
            _snwprintf_s(err, errlen, _TRUNCATE, L"cannot open '%ls': %ls", path, m);
            return false;
        }
        fm = dd__fsmap_build_xz(h, image_size, allow_unclean, cb, cancel, stats, out, &e);
        CloseHandle(h);
    } else {
        if ((ep.kind != EP_FILE && ep.kind != EP_BLOCK) || ep.size < 0) {
            _snwprintf_s(err, errlen, _TRUNCATE, L"'%ls' is not an image file or a device", path);
            ep_close(&ep);
            return false;
        }
        memset(&src, 0, sizeof src);
        src.h = ep.h;
        src.size = ep.size;
        src.device = ep.kind == EP_BLOCK;
        src.is_volume = ep.is_volume;
        fm = dd__fsmap_build(&src, allow_unclean, out, &e);
        ep_close(&ep);
    }
    if (!fm) {
        if (e == ERROR_OPERATION_ABORTED)
            _snwprintf_s(err, errlen, _TRUNCATE, L"mapping '%ls' was stopped", path);
        else {
            dd_errstr(e, m, 256);
            _snwprintf_s(err, errlen, _TRUNCATE, L"cannot map the file systems of '%ls': %ls",
                         path, e == ERROR_INVALID_DATA ? L"it is damaged" : m);
        }
        memset(out, 0, sizeof *out);
        return false;
    }
    dd__fsmap_free(fm);
    return true;
}
