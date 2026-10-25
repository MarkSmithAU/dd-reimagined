/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later

    NTFS for sparse reading.  $Bitmap (MFT record 6) has a bit per cluster,
    set when the cluster is in use; the MFT, the log, the boot sectors and
    bad clusters are all marked in it.  The MFT's first records are found
    from the boot sector: the system files are always in its first run.

    Clean means $Volume's dirty flag is clear and $LogFile's restart area
    says the log is clean.  Windows' Fast Startup and hibernation leave the
    log in use without setting the dirty flag, with metadata still in the
    hibernated memory: such a volume is not clean.  An unclean NTFS volume
    is read whole even with --allow-unclean, because only replaying its log
    (a format Microsoft does not document) would say what it holds.  A
    volume Windows has mounted always has its log in use; when dd holds its
    lock, Windows has written everything out, and Windows' word that it
    mounted clean is taken instead.
*/
#include "fs.h"
#include <stdlib.h>
#include <string.h>

#define CHUNK       (1u << 20)
#define FIXUP_BLOCK 512          /* NTFS fixes up every 512 bytes of a record */
#define MAX_RUNS    4096

#define REC_LOGFILE 2
#define REC_VOLUME  3
#define REC_BITMAP  6

#define AT_VOLUME_INFORMATION 0x70u
#define AT_DATA               0x80u
#define AT_END                0xFFFFFFFFu

typedef struct ntfs {
    uint32_t bps;               /* bytes per sector */
    uint32_t cluster;           /* bytes */
    uint32_t record;            /* bytes per MFT record */
    uint64_t sectors;
    uint64_t clusters;
    uint64_t mft_lcn;
} ntfs;

typedef struct run {
    uint64_t vcn, lcn, len;     /* lcn = UINT64_MAX: a sparse run */
} run;

static bool pow2(uint64_t v) { return v && !(v & (v - 1)); }

static bool parse(const uint8_t *b, int64_t vol_size, ntfs *x)
{
    uint8_t spc = b[13];
    int8_t cpr = (int8_t)b[0x40];

    (void)vol_size;
    if (memcmp(b + 3, "NTFS    ", 8) != 0 || b[510] != 0x55 || b[511] != 0xAA)
        return false;
    x->bps = dd__le16(b + 11);
    if (x->bps < 256 || x->bps > 4096 || !pow2(x->bps))
        return false;
    /* more than 128 sectors per cluster is written as a negative power of two */
    x->cluster = spc <= 0x80 ? spc * x->bps : x->bps << (256 - spc);
    if (spc == 0 || (spc > 0x80 && 256 - spc > 12) || x->cluster > (2u << 20) || !pow2(x->cluster))
        return false;
    x->record = cpr > 0 ? (uint32_t)cpr * x->cluster : cpr < 0 && cpr >= -31 ? 1u << -cpr : 0;
    if (x->record < 1024 || x->record > 65536 || !pow2(x->record))
        return false;
    x->sectors = dd__le64(b + 0x28);
    x->mft_lcn = dd__le64(b + 0x30);
    x->clusters = x->sectors * x->bps / x->cluster;
    return x->sectors > 0 && x->clusters > 0 && x->mft_lcn < x->clusters;
}

static bool ntfs_probe(const uint8_t *head, int64_t vol_size)
{
    ntfs x;
    return parse(head, vol_size, &x);
}

/* The update sequence: the last two bytes of each 512-byte block were
   replaced by a check value when written; puts the real bytes back. */
static bool fixup(uint8_t *rec, uint32_t size, const char magic[4])
{
    uint16_t ofs = dd__le16(rec + 4), count = dd__le16(rec + 6), check, i;

    if (memcmp(rec, magic, 4) != 0 || count < 2 || (uint32_t)(count - 1) * FIXUP_BLOCK > size ||
        ofs + 2u * count > size || ofs < 8)
        return false;
    check = dd__le16(rec + ofs);
    for (i = 1; i < count; i++) {
        uint8_t *end = rec + i * FIXUP_BLOCK - 2;
        if (dd__le16(end) != check)
            return false;           /* a torn write */
        end[0] = rec[ofs + 2 * i];
        end[1] = rec[ofs + 2 * i + 1];
    }
    return true;
}

static DWORD read_record(dd__fs_vol *v, const ntfs *x, uint32_t n, uint8_t *rec)
{
    DWORD e = dd__fs_read(v, (int64_t)(x->mft_lcn * x->cluster) + (int64_t)n * x->record, rec, x->record);
    if (e)
        return e;
    return fixup(rec, x->record, "FILE") && (dd__le16(rec + 0x16) & 1) ? 0 : ERROR_INVALID_DATA;
}

/* An unnamed attribute of the record; NULL if it has none (or it is in an
   attribute list, which the system files read here never need). */
static const uint8_t *find_attr(const uint8_t *rec, uint32_t size, uint32_t type)
{
    uint32_t at = dd__le16(rec + 0x14), used = dd__le32(rec + 0x18);

    if (used > size)
        return NULL;
    while (at + 16 <= used) {
        const uint8_t *a = rec + at;
        uint32_t t = dd__le32(a), len = dd__le32(a + 4);
        if (t == AT_END || len < 16 || at + len > used)
            return NULL;
        /* its header must fit: 0x40 bytes non-resident, 0x18 resident */
        if (t == type && a[9] == 0)
            return len >= (a[8] ? 0x40u : 0x18u) ? a : NULL;
        at += len;
    }
    return NULL;
}

/* The runs of a non-resident attribute.  Returns how many, or -1. */
static int decode_runs(const uint8_t *a, run *runs, int max)
{
    uint32_t len = dd__le32(a + 4), at = dd__le16(a + 0x20);
    uint64_t vcn = dd__le64(a + 0x10);
    int64_t lcn = 0;
    int n = 0;

    if (a[8] != 1 || at >= len)
        return -1;
    while (at < len && a[at] != 0) {
        uint8_t h = a[at++];
        uint32_t lb = h & 15, ob = h >> 4, i;
        uint64_t count = 0;
        int64_t delta = 0;
        if (lb == 0 || lb > 8 || ob > 8 || at + lb + ob > len || n >= max)
            return -1;
        for (i = 0; i < lb; i++)
            count |= (uint64_t)a[at + i] << (8 * i);
        at += lb;
        if (ob) {
            for (i = 0; i < ob; i++)
                delta |= (int64_t)((uint64_t)a[at + i] << (8 * i));
            if (a[at + ob - 1] & 0x80 && ob < 8)
                delta |= (int64_t)(~(uint64_t)0 << (8 * ob));   /* sign extend */
            at += ob;
            lcn += delta;
            if (lcn < 0)
                return -1;
        }
        runs[n].vcn = vcn;
        runs[n].lcn = ob ? (uint64_t)lcn : UINT64_MAX;
        runs[n].len = count;
        vcn += count;
        n++;
    }
    return n;
}

/* Reads [0, len) of a non-resident attribute's data. */
static DWORD read_data(dd__fs_vol *v, const ntfs *x, const run *runs, int n, uint8_t *out, uint64_t len)
{
    uint64_t done = 0;
    int i;
    DWORD e;

    for (i = 0; i < n && done < len; i++) {
        uint64_t start = runs[i].vcn * x->cluster, bytes = runs[i].len * x->cluster, k;
        if (start != done)
            return ERROR_INVALID_DATA;          /* the runs must follow on */
        if (bytes > len - done)
            bytes = len - done;
        for (k = 0; k < bytes; ) {
            uint64_t c = bytes - k < CHUNK ? bytes - k : CHUNK;
            if (runs[i].lcn == UINT64_MAX)
                memset(out + done + k, 0, (size_t)c);
            else if (runs[i].lcn + runs[i].len > x->clusters)
                return ERROR_INVALID_DATA;
            else if ((e = dd__fs_read(v, (int64_t)(runs[i].lcn * x->cluster + k), out + done + k, (size_t)c)) != 0)
                return e;
            k += c;
        }
        done += bytes;
    }
    return done == len ? 0 : ERROR_INVALID_DATA;
}

/* Whether the log ($LogFile's restart area) says the volume is clean.
   A log reset by chkdsk is all 0xFF, which is clean too. */
static DWORD log_clean(dd__fs_vol *v, const ntfs *x, uint8_t *rec, run *runs, bool *clean)
{
    uint8_t *page = NULL;
    uint32_t psize, i;
    const uint8_t *a;
    int n;
    DWORD e;

    *clean = false;
    if ((e = read_record(v, x, REC_LOGFILE, rec)) != 0)
        return e;
    a = find_attr(rec, x->record, AT_DATA);
    if (!a || (n = decode_runs(a, runs, MAX_RUNS)) <= 0)
        return ERROR_INVALID_DATA;
    page = malloc(2 * 65536);
    if (!page)
        return ERROR_NOT_ENOUGH_MEMORY;
    /* the two restart pages; their size is in the first */
    e = read_data(v, x, runs, n, page, 4096);
    if (!e && dd__le32(page) == 0xFFFFFFFFu) {
        *clean = true;                          /* empty */
        goto out;
    }
    psize = dd__le32(page + 0x10);
    if (!e && (psize < 4096 || psize > 65536 || !pow2(psize)))
        e = ERROR_INVALID_DATA;
    if (!e)
        e = read_data(v, x, runs, n, page, 2 * (uint64_t)psize);
    if (e)
        goto out;
    *clean = true;
    for (i = 0; i < 2; i++) {
        uint8_t *p = page + (size_t)i * psize;
        uint16_t ra;
        if (!fixup(p, psize, "RSTR") && !fixup(p, psize, "CHKD")) {
            if (i == 0)
                e = ERROR_INVALID_DATA;
            continue;                           /* the second copy may be stale */
        }
        ra = dd__le16(p + 0x18);
        if (ra + 16u > psize) {
            e = ERROR_INVALID_DATA;
            break;
        }
        /* clients in use, and not marked clean: there is work to replay */
        if (dd__le16(p + ra + 0x0C) != 0xFFFF && !(dd__le16(p + ra + 0x0E) & 0x0002))
            *clean = false;
    }
out:
    free(page);
    return e;
}

static dd__fs_status ntfs_map(dd__fs_vol *v, const uint8_t *head, bool allow_unclean, dd__fs_map *m)
{
    run *runs = NULL;
    uint8_t *rec = NULL;
    const uint8_t *a;
    uint64_t want;
    dd__fs_status st = DD__FS_WHOLE;
    bool dirty, clean_log;
    ntfs x;
    int n;
    DWORD e;

    if (!parse(head, dd__fs_vol_size(v), &x))
        return DD__FS_WHOLE;
    wcscpy_s(m->type, sizeof m->type / sizeof *m->type, L"NTFS");
    rec = malloc(x.record);
    runs = malloc(MAX_RUNS * sizeof *runs);
    if (!rec || !runs) {
        dd__fs_note(m, L"there is not enough memory to map it");
        goto out;
    }

    /* clean? */
    if ((e = read_record(v, &x, REC_VOLUME, rec)) != 0 ||
        (a = find_attr(rec, x.record, AT_VOLUME_INFORMATION)) == NULL || a[8] != 0 ||
        dd__le32(a + 0x10) < 12 || dd__le16(a + 0x14) + 12u > dd__le32(a + 4)) {
        dd__fs_note(m, L"its $Volume record cannot be read");
        goto out;
    }
    dirty = (dd__le16(a + dd__le16(a + 0x14) + 10) & 0x0001) != 0;
    if ((e = log_clean(v, &x, rec, runs, &clean_log)) != 0) {
        dd__fs_note(m, e == ERROR_NOT_ENOUGH_MEMORY ? L"there is not enough memory to map it"
                                                    : L"its log ($LogFile) cannot be read");
        goto out;
    }
    if ((dirty || !clean_log) && !dd__fs_vol_os_clean(v)) {
        m->unclean = true;
        m->unclean_whole = true;            /* --allow-unclean cannot help */
        dd__fs_note(m, dirty ? L"it was not cleanly unmounted, says $Volume; dd cannot replay an NTFS log, so it reads it whole"
                             : L"its log needs replaying: it was not cleanly unmounted, or Windows' Fast Startup or "
                               L"hibernation left it in use; dd cannot replay an NTFS log, so it reads it whole");
        (void)allow_unclean;
        goto out;
    }

    /* $Bitmap */
    if ((e = read_record(v, &x, REC_BITMAP, rec)) != 0 || (a = find_attr(rec, x.record, AT_DATA)) == NULL ||
        (n = decode_runs(a, runs, MAX_RUNS)) <= 0) {
        dd__fs_note(m, L"its $Bitmap record cannot be read");
        goto out;
    }
    want = (x.clusters + 7) / 8;
    if (dd__le64(a + 0x30) < want) {
        dd__fs_note(m, L"its $Bitmap is shorter than the volume");
        goto out;
    }
    m->size = (int64_t)(x.sectors * x.bps);
    m->unit = x.cluster;
    m->map_start = 0;
    /* larger than its volume: fsmap reads it whole, so allocate nothing */
    if (m->size > dd__fs_vol_size(v)) {
        st = DD__FS_MAPPED;
        goto out;
    }
    if (!dd__fs_alloc_bits(m, x.clusters)) {
        dd__fs_note(m, L"there is not enough memory to map it");
        goto out;
    }
    if ((e = read_data(v, &x, runs, n, m->bits, want)) != 0) {
        dd__fs_note(m, e == ERROR_HANDLE_EOF ? L"its $Bitmap runs past the end of the volume"
                                             : L"its $Bitmap cannot be read");
        goto out;
    }
    st = DD__FS_MAPPED;
out:
    free(rec);
    free(runs);
    return st;
}

const dd__fs_driver dd__fs_ntfs = { ntfs_probe, ntfs_map };
