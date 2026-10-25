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

    A compressed image is read in order, so its map is made beforehand
    (dd_make_sparse_map) and kept in a .sparse file beside it: the free
    ranges, the report, and the SHA-256 of the .xz file it belongs to.
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

#define MAP_MAGIC "dd sparse map 2"
#define MAX_MAP_FILE (1u << 30)

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

/* A file system's place in the input, with its bits; or, in a loaded map,
   one free range (free_run). */
typedef struct region {
    int64_t  start, end;
    int64_t  bits_start, bits_end; /* where the bits apply */
    uint32_t unit;
    uint8_t *bits;
    bool     free_run;             /* all of [start, end) is free */
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
    if (r->free_run) {
        *used = false;
        return r->end - pos < max ? r->end - pos : max;
    }
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

/* ---- the .sparse file ---------------------------------------------------- */

typedef struct text {
    char  *p;
    size_t len, cap;
    bool   failed;
} text;

static void put(text *t, const char *fmt, ...)
{
    va_list ap;
    int n;

    for (;;) {
        if (t->failed)
            return;
        va_start(ap, fmt);
        n = t->cap > t->len ? vsnprintf(t->p + t->len, t->cap - t->len, fmt, ap) : -1;
        va_end(ap);
        if (n >= 0 && (size_t)n < t->cap - t->len) {
            t->len += (size_t)n;
            return;
        }
        {
            size_t cap = t->cap ? t->cap * 2 : 1u << 16;
            char *q = realloc(t->p, cap);
            if (!q) {
                t->failed = true;
                return;
            }
            t->p = q;
            t->cap = cap;
        }
    }
}

static void put_utf8(text *t, const wchar_t *s)
{
    char buf[1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof buf, NULL, NULL);
    size_t i;

    if (n <= 0)
        buf[0] = 0;
    for (i = 0; buf[i]; i++)            /* one line per entry */
        if (buf[i] == '\n' || buf[i] == '\r')
            buf[i] = ' ';
    put(t, "%s", buf);
}

static DWORD save_map(const wchar_t *path, const dd__fsmap *fm, const dd_fs_report *rep, const dd__map_id *id)
{
    wchar_t tmp[1100 + 8], hex[65];     /* path is at most 1100 */
    text t = { NULL, 0, 0, false };
    int64_t pos = 0, free_at = -1, free_len = 0;
    char hexa[65];
    DWORD e = 0, w;
    HANDLE h;
    size_t i;

    dd__hex(id->sha256, hex);
    for (i = 0; i < 65; i++)
        hexa[i] = (char)hex[i];
    put(&t, MAP_MAGIC "\nsha256 %s\nfile_size %lld\nimage_size %lld\nunclean %d\n",
        hexa, (long long)id->file_size, (long long)id->image_size, id->unclean ? 1 : 0);
    for (i = 0; i < rep->count; i++) {
        const dd_fs_volume *v = &rep->vols[i];
        put(&t, "volume %d %lld %lld %lld %d %d ", v->partition, (long long)v->start, (long long)v->size,
            (long long)v->used, v->sparse ? 1 : 0, v->unclean ? 1 : 0);
        if (v->type[0])
            put_utf8(&t, v->type);
        else
            put(&t, "-");
        put(&t, " ");
        put_utf8(&t, v->note);
        put(&t, "\n");
    }
    while (pos < id->image_size) {
        bool used;
        int64_t n = dd__fsmap_run(fm, pos, id->image_size - pos, &used);
        if (!used) {
            if (free_at >= 0 && free_at + free_len == pos) {
                free_len += n;
            } else {
                if (free_at >= 0)
                    put(&t, "free %lld %lld\n", (long long)free_at, (long long)free_len);
                free_at = pos;
                free_len = n;
            }
        }
        pos += n;
    }
    if (free_at >= 0)
        put(&t, "free %lld %lld\n", (long long)free_at, (long long)free_len);
    if (!t.failed) {
        dd__sha256 *sh = dd__sha256_new();
        uint8_t d[32];
        if (t.len > MAX_MAP_FILE - 128) {
            dd__sha256_free(sh);
            free(t.p);
            return ERROR_FILE_TOO_LARGE;    /* dd__fsmap_load would refuse it */
        }
        if (sh) {
            dd__sha256_update(sh, t.p, (DWORD)t.len);
            if (dd__sha256_final(sh, d)) {
                dd__hex(d, hex);
                for (i = 0; i < 65; i++)
                    hexa[i] = (char)hex[i];
                put(&t, "check %s\nend\n", hexa);
            } else {
                t.failed = true;
            }
        } else {
            t.failed = true;
        }
        dd__sha256_free(sh);
    }
    if (t.failed) {
        free(t.p);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    /* written beside it, then put in place, so an old map is never half replaced */
    _snwprintf_s(tmp, sizeof tmp / sizeof *tmp, _TRUNCATE, L"%ls.tmp", path);
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        e = GetLastError();
    } else {
        if (!WriteFile(h, t.p, (DWORD)t.len, &w, NULL) || w != t.len)
            e = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
        if (!CloseHandle(h) && !e)
            e = GetLastError();
        if (!e && !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            e = GetLastError();
        if (e)
            DeleteFileW(tmp);
    }
    free(t.p);
    return e;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void get_utf8(const char *s, size_t n, wchar_t *out, size_t outn)
{
    int k = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, out, (int)outn - 1);
    out[k > 0 ? k : 0] = 0;
}

dd__fsmap *dd__fsmap_load(const wchar_t *path, dd__map_id *id, dd_fs_report *rep, DWORD *err)
{
    dd__fsmap *fm = NULL;
    dd_fs_report *r = rep ? rep : calloc(1, sizeof *r);
    LARGE_INTEGER size;
    char *buf = NULL, *line, *next;
    int64_t last_end = 0, free_total = 0, *vol_free = NULL;
    bool magic = false, ended = false, have_sha = false, checked = false;
    uint8_t body[32];
    HANDLE h;
    DWORD got;
    size_t k, x;

    memset(id, 0, sizeof *id);
    id->file_size = id->image_size = -1;
    *err = 0;
    if (!r) {
        *err = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    memset(r, 0, sizeof *r);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        goto out;
    }
    if (!GetFileSizeEx(h, &size) || size.QuadPart > MAX_MAP_FILE) {
        *err = ERROR_INVALID_DATA;      /* save_map never writes one so large */
        CloseHandle(h);
        goto out;
    }
    if ((buf = malloc((size_t)size.QuadPart + 1)) == NULL ||
        !ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) || got != (DWORD)size.QuadPart) {
        *err = buf ? ERROR_INVALID_DATA : ERROR_NOT_ENOUGH_MEMORY;
        CloseHandle(h);
        goto out;
    }
    CloseHandle(h);
    buf[size.QuadPart] = 0;
    /* the "check" line holds the SHA-256 of everything before it */
    {
        char *ck = NULL, *f = buf;
        dd__sha256 *sh;
        while ((f = strstr(f, "\ncheck ")) != NULL)
            ck = ++f;
        if (!ck || (sh = dd__sha256_new()) == NULL) {
            *err = ck ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_DATA;
            goto out;
        }
        dd__sha256_update(sh, buf, (DWORD)(ck - buf));
        if (!dd__sha256_final(sh, body)) {
            dd__sha256_free(sh);
            *err = ERROR_NOT_ENOUGH_MEMORY;
            goto out;
        }
        dd__sha256_free(sh);
    }
    if ((fm = calloc(1, sizeof *fm)) == NULL) {
        *err = ERROR_NOT_ENOUGH_MEMORY;
        goto out;
    }

    for (line = buf; line && *line && !ended; line = next) {
        long long a, b, c;
        int p, s, u, n = 0, i;
        char *nl = strchr(line, '\n');
        next = nl ? nl + 1 : NULL;
        if (nl)
            *nl = 0;
        if (!magic) {
            if (strcmp(line, MAP_MAGIC) != 0)
                break;
            magic = true;
        } else if (checked && strcmp(line, "end") != 0) {
            break;                  /* nothing but "end" after the check */
        } else if (strncmp(line, "sha256 ", 7) == 0 && strlen(line) == 7 + 64) {
            for (i = 0; i < 32; i++) {
                int hi = hexval(line[7 + 2 * i]), lo = hexval(line[8 + 2 * i]);
                if (hi < 0 || lo < 0)
                    break;
                id->sha256[i] = (uint8_t)(hi << 4 | lo);
            }
            have_sha = i == 32;
        } else if (sscanf_s(line, "file_size %lld", &a) == 1) {
            id->file_size = a;
        } else if (sscanf_s(line, "image_size %lld", &a) == 1) {
            id->image_size = a;
        } else if (sscanf_s(line, "unclean %d", &u) == 1) {
            id->unclean = u != 0;
        } else if (sscanf_s(line, "volume %d %lld %lld %lld %d %d %n", &p, &a, &b, &c, &s, &u, &n) == 6 && n > 0 &&
                   r->count < DD_MAX_PARTITIONS) {
            dd_fs_volume *v = &r->vols[r->count++];
            char *type = line + n, *sp = strchr(type, ' ');
            v->partition = p;
            v->start = a;
            v->size = b;
            v->used = c;
            v->sparse = s != 0;
            v->unclean = u != 0;
            get_utf8(type, sp ? (size_t)(sp - type) : strlen(type), v->type, sizeof v->type / sizeof *v->type);
            if (wcscmp(v->type, L"-") == 0)
                v->type[0] = 0;
            if (sp)
                get_utf8(sp + 1, strlen(sp + 1), v->note, sizeof v->note / sizeof *v->note);
        } else if (strncmp(line, "check ", 6) == 0 && strlen(line) == 6 + 64) {
            for (i = 0; i < 32; i++) {
                int hi = hexval(line[6 + 2 * i]), lo = hexval(line[7 + 2 * i]);
                if (hi < 0 || lo < 0 || (uint8_t)(hi << 4 | lo) != body[i])
                    break;
            }
            if (i != 32)
                break;
            checked = true;
        } else if (sscanf_s(line, "free %lld %lld", &a, &b) == 2) {
            region *rg;
            if (a < last_end || b <= 0 || id->image_size < 0 || b > id->image_size - a)
                break;              /* out of order, or outside the image */
            if ((rg = add_region(fm)) == NULL) {
                *err = ERROR_NOT_ENOUGH_MEMORY;
                goto out;
            }
            rg->start = rg->bits_start = a;
            rg->end = rg->bits_end = a + b;
            rg->free_run = true;
            last_end = a + b;
            free_total += b;
        } else if (strcmp(line, "end") == 0) {
            ended = true;
        } else {
            break;
        }
    }
    if (!ended || !checked || !have_sha || id->file_size < 0 || id->image_size < 0) {
        *err = ERROR_INVALID_DATA;
        goto out;
    }
    /* every free byte lies in a volume read sparsely, and adds up to what
       that volume says is free */
    if ((vol_free = calloc(r->count + 1, sizeof *vol_free)) == NULL) {
        *err = ERROR_NOT_ENOUGH_MEMORY;
        goto out;
    }
    for (x = 0; x < fm->n; x++) {
        const region *rg = &fm->r[x];
        int64_t covered = 0;
        for (k = 0; k < r->count; k++) {
            const dd_fs_volume *v = &r->vols[k];
            int64_t a0 = rg->start > v->start ? rg->start : v->start;
            int64_t b0 = rg->end < v->start + v->size ? rg->end : v->start + v->size;
            if (v->sparse && b0 > a0) {
                vol_free[k] += b0 - a0;
                covered += b0 - a0;
            }
        }
        if (covered != rg->end - rg->start) {
            *err = ERROR_INVALID_DATA;
            goto out;
        }
    }
    for (k = 0; k < r->count; k++)
        if (r->vols[k].sparse && vol_free[k] != r->vols[k].size - r->vols[k].used) {
            *err = ERROR_INVALID_DATA;
            goto out;
        }
    r->size = id->image_size;
    r->read = id->image_size - free_total;

out:
    free(buf);
    free(vol_free);
    if (r != rep)
        free(r);
    if (*err) {
        dd__fsmap_free(fm);
        fm = NULL;
    }
    return fm;
}

/* ---- making the map of a compressed image ----------------------------------- */

typedef struct maker {
    dd__xz_seek        *z;
    const dd_callbacks *cb;
    dd_cancel          *cancel;
    dd_stats           *st;
    LARGE_INTEGER       freq, start;
    double              last;
    DWORD               error;      /* the first read that failed */
} maker;

static double since(const maker *mk)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - mk->start.QuadPart) / (double)mk->freq.QuadPart;
}

static void tick(maker *mk)
{
    double now;

    if (!mk->cb || !mk->cb->progress || !mk->cb->progress_interval_ms)
        return;
    now = since(mk);
    if (now - mk->last >= mk->cb->progress_interval_ms / 1000.0) {
        mk->last = now;
        mk->st->phase_elapsed = now;
        mk->st->elapsed = now;
        mk->cb->progress(mk->cb->ctx, mk->st);
    }
}

static void on_decoded(void *ctx, uint64_t decoded)
{
    maker *mk = ctx;
    mk->st->phase_done = (int64_t)decoded;
    tick(mk);
}

static DWORD read_xz(void *ctx, int64_t off, void *buf, size_t len)
{
    maker *mk = ctx;

    DWORD e;

    if (mk->cancel && mk->cancel->requested)
        return ERROR_OPERATION_ABORTED;
    e = dd__xz_seek_read(mk->z, off, buf, len);
    if (e && !mk->error)
        mk->error = e;
    return e;
}

static bool has_xz_magic(HANDLE h)
{
    uint8_t head[8];
    OVERLAPPED ov;
    DWORD got = 0;

    memset(&ov, 0, sizeof ov);
    return ReadFile(h, head, sizeof head, &got, &ov) && dd__xz_magic(head, got);
}

dd_result dd_make_sparse_map(const wchar_t *image, bool allow_unclean, const dd_callbacks *cb,
                             dd_cancel *cancel, dd_stats *stats, dd_fs_report *out)
{
    dd_stats local;
    dd_fs_report *rep = NULL;
    dd__fsmap *fm = NULL;
    dd__fs_source src;
    dd__sha256 *sha = NULL;
    dd__map_id id;
    maker mk;
    wchar_t *norm = NULL, mapname[1100], m[256], a[32], b[32];
    LARGE_INTEGER fsize;
    uint8_t *buf = NULL;
    dd_result res = DD_FAILED;
    HANDLE h = INVALID_HANDLE_VALUE;
    int64_t pos;
    DWORD e = 0, got;

    if (!stats)
        stats = &local;
    memset(stats, 0, sizeof *stats);
    stats->bytes_total = stats->xz_bytes = stats->xz_done = -1;
    stats->phase = DD_PHASE_MAP;
    stats->phase_total = -1;
    memset(&mk, 0, sizeof mk);
    mk.cb = cb;
    mk.cancel = cancel;
    mk.st = stats;
    QueryPerformanceFrequency(&mk.freq);
    QueryPerformanceCounter(&mk.start);

    if (!image || !dd_sparse_map_name(image, mapname, sizeof mapname / sizeof *mapname) ||
        (norm = dd__normalize_path(image)) == NULL) {
        dd__msg(cb, DD_MSG_ERROR, L"a sparse map is made for an image file");
        goto done;
    }
    /* no writers: the file hashed must be the one mapped */
    h = CreateFileW(norm, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &fsize)) {
        dd_errstr(GetLastError(), m, 256);
        dd__msg(cb, DD_MSG_ERROR, L"cannot open '%ls': %ls", image, m);
        goto done;
    }
    if (!has_xz_magic(h)) {
        dd__msg(cb, DD_MSG_ERROR,
                L"'%ls' is not xz-compressed: --sparse-read maps an uncompressed image as it reads it, so it needs no sparse map",
                image);
        goto done;
    }
    rep = malloc(sizeof *rep);
    if (!rep || (e = dd__xz_seek_open(&mk.z, h, fsize.QuadPart)) != 0) {
        dd_errstr(rep ? e : ERROR_NOT_ENOUGH_MEMORY, m, 256);
        dd__msg(cb, DD_MSG_ERROR, L"cannot read '%ls': %ls", image, m);
        goto done;
    }
    stats->started = true;
    dd__xz_seek_set_progress(mk.z, on_decoded, &mk);
    dd__msg(cb, DD_MSG_INFO, L"mapping the file systems of '%ls' (decompressing the parts they need)", image);

    memset(&src, 0, sizeof src);
    src.rd = read_xz;
    src.rd_ctx = &mk;
    src.size = dd__xz_seek_size(mk.z);
    fm = dd__fsmap_build(&src, allow_unclean, rep, &e);
    if (cancel && cancel->requested) {
        res = DD_CANCELLED;
        goto done;
    }
    if (!fm) {
        dd__msg(cb, DD_MSG_ERROR, L"memory exhausted");
        goto done;
    }
    /* inside a compressed file, any read that fails means it is damaged:
       a map made around the gap would not be the image's */
    if (mk.error) {
        dd_errstr(mk.error, m, 256);
        dd__msg(cb, DD_MSG_ERROR, L"cannot map '%ls': %ls", image, m);
        goto done;
    }

    /* the map belongs to this .xz file: its hash, checked as it is read */
    stats->phase_done = 0;
    stats->phase_total = fsize.QuadPart;
    sha = dd__sha256_new();
    buf = malloc(1u << 20);
    if (!sha || !buf) {
        dd__msg(cb, DD_MSG_ERROR, L"memory exhausted");
        goto done;
    }
    for (pos = 0; pos < fsize.QuadPart; pos += got) {
        OVERLAPPED ov;
        if (cancel && cancel->requested) {
            res = DD_CANCELLED;
            goto done;
        }
        memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)pos;
        ov.OffsetHigh = (DWORD)((uint64_t)pos >> 32);
        if (!ReadFile(h, buf, 1u << 20, &got, &ov) || got == 0) {
            dd_errstr(GetLastError() ? GetLastError() : ERROR_HANDLE_EOF, m, 256);
            dd__msg(cb, DD_MSG_ERROR, L"error reading '%ls': %ls", image, m);
            goto done;
        }
        dd__sha256_update(sha, buf, got);
        stats->phase_done = pos + got;
        tick(&mk);
    }
    memset(&id, 0, sizeof id);
    if (!dd__sha256_final(sha, id.sha256)) {
        dd__msg(cb, DD_MSG_ERROR, L"cannot compute the SHA-256 of '%ls'", image);
        goto done;
    }
    id.file_size = fsize.QuadPart;
    id.image_size = src.size;
    id.unclean = allow_unclean;
    e = save_map(mapname, fm, rep, &id);
    if (e) {
        dd_errstr(e, m, 256);
        dd__msg(cb, DD_MSG_ERROR, L"cannot write the sparse map %ls: %ls", mapname, m);
        goto done;
    }
    dd_format_size(rep->read, false, a, 32);
    dd_format_size(rep->size, false, b, 32);
    dd__msg(cb, DD_MSG_INFO, L"sparse map written to %ls: --sparse-read keeps %ls of %ls", mapname, a, b);
    if (out)
        *out = *rep;
    res = DD_OK;

done:
    stats->phase_elapsed = stats->elapsed = since(&mk);
    if (cb && cb->progress && stats->started)
        cb->progress(cb->ctx, stats);
    dd__sha256_free(sha);
    free(buf);
    dd__fsmap_free(fm);
    dd__xz_seek_free(mk.z);
    free(rep);
    free(norm);
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    return res;
}

/* ---- the public report ------------------------------------------------- */

bool dd_fs_report_read(const wchar_t *path, bool allow_unclean, dd_fs_report *out, wchar_t *err, size_t errlen)
{
    dd_endpoint ep;
    dd__fs_source src;
    dd__fsmap *fm;
    dd__map_id id;
    wchar_t m[256], mapname[1100];
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
        /* answered by its sparse map, if it is this image's */
        int64_t file_size = ep.xz_file_size, image_size = ep.size;
        ep_close(&ep);
        if (!dd_sparse_map_name(path, mapname, sizeof mapname / sizeof *mapname) ||
            (fm = dd__fsmap_load(mapname, &id, out, &e)) == NULL) {
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
                _snwprintf_s(err, errlen, _TRUNCATE, L"'%ls' is xz-compressed and has no sparse map (%ls); make one with --make-sparse-map",
                             path, mapname);
            else {
                dd_errstr(e, m, 256);
                _snwprintf_s(err, errlen, _TRUNCATE, L"cannot use the sparse map %ls: %ls", mapname, m);
            }
            memset(out, 0, sizeof *out);
            return false;
        }
        dd__fsmap_free(fm);
        if (id.file_size != file_size || id.image_size != image_size) {
            _snwprintf_s(err, errlen, _TRUNCATE, L"the sparse map %ls is for another image; make it again", mapname);
            memset(out, 0, sizeof *out);
            return false;
        }
        /* as a copy would: such a map is used only when asked for */
        if (id.unclean && !allow_unclean) {
            _snwprintf_s(err, errlen, _TRUNCATE,
                         L"the sparse map %ls was made with --allow-unclean; give --allow-unclean too, or make the map again without it",
                         mapname);
            memset(out, 0, sizeof *out);
            return false;
        }
        return true;
    }
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
    if (!fm) {
        dd_errstr(e, m, 256);
        _snwprintf_s(err, errlen, _TRUNCATE, L"'%ls': %ls", path, m);
        return false;
    }
    dd__fsmap_free(fm);
    return true;
}
