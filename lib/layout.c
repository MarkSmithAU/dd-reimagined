/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later

    Reads an image's MBR (with logical partitions) or GPT, and works out
    how much of the image holds data: up to the end of the last partition,
    the partition tables, and any ISO 9660 volume (hybrid ISOs keep their
    file system outside the partitions).  For --fit, rebuilds a GPT for the
    output disk: the backup header and partition array move to the disk's
    last sectors, and the primary header and protective MBR are updated to
    match, as sgdisk -e does.
*/
#include "dd_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define GRAIN 4096u                       /* devices need sector aligned reads */
#define MAX_GPT_ARRAY (4u << 20)

/* ---- helpers ----------------------------------------------------------- */

static uint32_t crc_table[256];

static void crc_init(void)
{
    uint32_t i, k, c;

    if (crc_table[1])
        return;
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;

    crc_init();
    while (n--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

/* Partition tables are at the start of an image, so of an .xz image only
   the start is decompressed, into a cache, up to this much. */
#define XZ_LAYOUT_LIMIT (256u << 20)
/* No disk has 2^48 sectors; anything past it would overflow a byte offset. */
#define MAX_LBA ((uint64_t)1 << 48)

typedef struct image {
    HANDLE   h;
    int64_t  size;           /* uncompressed */
    int64_t  file_size;
    dd__xz  *xz;             /* an .xz image */
    uint8_t *cache;          /* its first `cached` bytes, decompressed */
    size_t   cached, cache_cap;
    dd__read_at_fn rd;       /* or read through this, if set */
    void    *rd_ctx;
    DWORD    ss;             /* a disk's logical sector size; 0 for a file */
} image;

static void image_sector_size(image *im)
{
    int64_t size;
    DWORD sector;
    int media;

    if (dd__geometry(im->h, &size, &sector, &media) && (sector == 512 || sector == 4096))
        im->ss = sector;
}

static bool raw_read(HANDLE h, int64_t off, void *buf, DWORD len, DWORD *got)
{
    OVERLAPPED ov;

    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    return ReadFile(h, buf, len, got, &ov) != 0;
}

static bool image_open(image *im, const wchar_t *path)
{
    wchar_t *norm = dd__normalize_path(path);
    LARGE_INTEGER sz;
    GET_LENGTH_INFORMATION li;
    uint8_t head[8];
    DWORD ret, got, e;

    memset(im, 0, sizeof *im);
    im->h = INVALID_HANDLE_VALUE;
    if (!norm) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    im->h = CreateFileW(norm, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    free(norm);
    if (im->h == INVALID_HANDLE_VALUE)
        return false;
    if (DeviceIoControl(im->h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof li, &ret, NULL)) {
        im->size = li.Length.QuadPart;      /* a device: never compressed */
        im->file_size = im->size;
        image_sector_size(im);
        return true;
    }
    if (!GetFileSizeEx(im->h, &sz)) {
        e = GetLastError();
        CloseHandle(im->h);
        im->h = INVALID_HANDLE_VALUE;
        SetLastError(e);
        return false;
    }
    im->size = im->file_size = sz.QuadPart;
    if (im->size >= (int64_t)sizeof head && raw_read(im->h, 0, head, sizeof head, &got) &&
        dd__xz_magic(head, got)) {
        e = dd__xz_info(im->h, im->file_size, &im->size);
        if (!e)
            e = dd__xz_decoder(&im->xz, im->h, dd__xz_memory_available() / 4);
        if (e) {
            CloseHandle(im->h);
            im->h = INVALID_HANDLE_VALUE;
            SetLastError(e);
            return false;
        }
    }
    return true;
}

static void image_close(image *im)
{
    if (im->h != INVALID_HANDLE_VALUE)
        CloseHandle(im->h);
    im->h = INVALID_HANDLE_VALUE;
    dd__xz_free(im->xz);
    free(im->cache);
    im->xz = NULL;
    im->cache = NULL;
}

/* An .xz image: decompress from the start until [off, off + len) is cached. */
static bool xz_read(image *im, int64_t off, void *buf, size_t len)
{
    size_t want = (size_t)(off + (int64_t)len);
    DWORD got;

    if (want > XZ_LAYOUT_LIMIT)
        return false;
    while (im->cached < want) {
        size_t n;
        if (im->cached == im->cache_cap) {
            size_t cap = im->cache_cap ? im->cache_cap * 2 : (1u << 20);
            uint8_t *p;
            while (cap < want)
                cap *= 2;
            if (cap > XZ_LAYOUT_LIMIT)
                cap = XZ_LAYOUT_LIMIT;
            p = realloc(im->cache, cap);
            if (!p)
                return false;
            im->cache = p;
            im->cache_cap = cap;
        }
        n = im->cache_cap - im->cached;
        if (dd__xz_read(im->xz, im->cache + im->cached, n > (1u << 20) ? (1u << 20) : (DWORD)n, &got) || got == 0)
            return false;
        im->cached += got;
    }
    memcpy(buf, im->cache + off, len);
    return true;
}

/* Reads [off, off + len), which must lie inside the image.  Reads whole
   GRAIN units so that devices accept the request. */
static bool image_read(image *im, int64_t off, void *buf, size_t len)
{
    int64_t a = off - off % GRAIN, b = off + (int64_t)len;
    size_t n;
    uint8_t *tmp;
    DWORD got;
    bool ok;

    if (off < 0 || b > im->size || len == 0)
        return false;
    if (im->rd)
        return im->rd(im->rd_ctx, off, buf, len) == 0;
    if (im->xz)
        return xz_read(im, off, buf, len);
    b = (b + GRAIN - 1) / GRAIN * GRAIN;
    n = (size_t)(b - a);
    tmp = VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!tmp)
        return false;
    /* the last GRAIN of a file may be short; only [off, off+len) matters */
    ok = raw_read(im->h, a, tmp, (DWORD)n, &got) && got >= (DWORD)(off + (int64_t)len - a);
    if (ok)
        memcpy(buf, tmp + (off - a), len);
    VirtualFree(tmp, 0, MEM_RELEASE);
    return ok;
}
static void note(dd_layout *l, const wchar_t *fmt, ...)
{
    va_list ap;

    if (l->note[0])
        return;     /* the first problem is the one worth reporting */
    va_start(ap, fmt);
    _vsnwprintf_s(l->note, sizeof l->note / sizeof *l->note, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static dd_partition *add_part(dd_layout *l)
{
    dd_partition *p;

    if (l->count >= DD_MAX_PARTITIONS) {
        note(l, L"only the first %d partitions are shown", DD_MAX_PARTITIONS);
        return NULL;
    }
    p = &l->parts[l->count++];
    memset(p, 0, sizeof *p);
    return p;
}

static int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }

/* ---- MBR --------------------------------------------------------------- */

static bool is_extended(uint8_t t) { return t == 0x05 || t == 0x0F || t == 0x85; }

/* A boot sector without a partition table (a "superfloppy" FAT or NTFS
   volume) also ends in 55 AA; its partition slots hold code, which shows up
   as bad status bytes or impossible extents. */
static bool mbr_valid(const uint8_t *s, int64_t size, bool *any)
{
    int i;

    *any = false;
    if (s[510] != 0x55 || s[511] != 0xAA)
        return false;
    for (i = 0; i < 4; i++) {
        const uint8_t *e = s + 446 + 16 * i;
        uint32_t start = le32(e + 8), count = le32(e + 12);
        if (e[0] != 0x00 && e[0] != 0x80)
            return false;
        if (e[4] == 0)
            continue;
        if (start == 0 || count == 0)
            return false;
        (void)size;
        *any = true;
    }
    return true;
}

static void read_logical(image *im, dd_layout *l, DWORD ss, uint32_t ext_base, int64_t *end)
{
    uint8_t s[512];
    int64_t ebr = ext_base;
    int number = 5, hops;

    for (hops = 0; hops < 256; hops++) {
        const uint8_t *e0 = s + 446, *e1 = s + 462;
        if (!image_read(im, ebr * ss, s, 512)) {
            if (im->xz && ebr * ss < im->size)
                note(l, L"logical partitions more than %u MiB into a compressed image are not shown",
                     XZ_LAYOUT_LIMIT >> 20);
            else
                note(l, L"an extended partition table lies beyond the end of the image");
            return;
        }
        if (s[510] != 0x55 || s[511] != 0xAA) {
            note(l, L"an extended partition table is damaged");
            return;
        }
        if (e0[4] != 0 && le32(e0 + 12) != 0) {
            int64_t start = (ebr + le32(e0 + 8)) * ss, sz = (int64_t)le32(e0 + 12) * ss;
            dd_partition *p = add_part(l);
            if (p) {
                p->number = number;
                p->start = start;
                p->size = sz;
                p->mbr_type = e0[4];
                p->bootable = e0[0] == 0x80;
                p->logical = true;
            }
            number++;
            *end = max64(*end, start + sz);
        }
        if (!is_extended(e1[4]) || le32(e1 + 8) == 0)
            return;
        if ((int64_t)ext_base + le32(e1 + 8) <= ebr) {
            note(l, L"the extended partition chain loops");
            return;
        }
        ebr = (int64_t)ext_base + le32(e1 + 8);
    }
    note(l, L"the extended partition chain is too long");
}

/* Whether a volume (or an extended partition table) starts at off. */
static bool volume_at(image *im, int64_t off)
{
    uint8_t b[4096];
    size_t n;

    if (off <= 0 || off >= im->size || (im->xz && off > XZ_LAYOUT_LIMIT))
        return false;
    n = im->size - off < (int64_t)sizeof b ? (size_t)(im->size - off) : sizeof b;
    memset(b, 0, sizeof b);
    if (n < 512 || !image_read(im, off, b, n))
        return false;
    return (b[510] == 0x55 && b[511] == 0xAA) || le16(b + 1080) == 0xEF53 ||
           memcmp(b + 4086, "SWAPSPACE2", 10) == 0 || memcmp(b, "LUKS\xBA\xBE", 6) == 0 ||
           memcmp(b + 512, "LABELONE", 8) == 0;
}

/* An MBR counts in the disk's logical sectors.  A disk says what they are;
   for an image, they are 4096 bytes only if no partition's first sector
   holds anything at 512 bytes and one does at 4096. */
static DWORD mbr_sector_size(image *im, const uint8_t *s)
{
    static const DWORD sizes[2] = { 512, 4096 };
    int i, k;

    if (im->ss)
        return im->ss;
    for (k = 0; k < 2; k++)
        for (i = 0; i < 4; i++) {
            const uint8_t *e = s + 446 + 16 * i;
            if (e[4] != 0 && volume_at(im, (int64_t)le32(e + 8) * sizes[k]))
                return sizes[k];
        }
    return 512;
}

static void read_mbr(image *im, dd_layout *l, const uint8_t *s)
{
    DWORD ss = mbr_sector_size(im, s);
    int64_t end = 512;
    int i;

    l->scheme = DD_SCHEME_MBR;
    l->sector_size = ss;
    if (ss != 512 && !im->ss)
        note(l, L"its partition table counts 4096-byte sectors");
    l->mbr_signature = le32(s + 440);
    for (i = 0; i < 4; i++) {
        const uint8_t *e = s + 446 + 16 * i;
        int64_t start = (int64_t)le32(e + 8) * ss, sz = (int64_t)le32(e + 12) * ss;
        dd_partition *p;
        if (e[4] == 0)
            continue;
        p = add_part(l);
        if (p) {
            p->number = i + 1;
            p->start = start;
            p->size = sz;
            p->mbr_type = e[4];
            p->bootable = e[0] == 0x80;
            p->extended = is_extended(e[4]);
        }
        end = max64(end, start + sz);
        if (is_extended(e[4]))
            read_logical(im, l, ss, le32(e + 8), &end);
    }
    l->data_end = end;
}

/* ---- GPT --------------------------------------------------------------- */

typedef struct gpt_hdr {
    uint32_t size, crc;
    uint64_t my_lba, alt_lba, first_usable, last_usable, entries_lba;
    uint32_t entries, entry_size, entries_crc;
    GUID     disk_guid;
} gpt_hdr;

static bool parse_gpt_header(const uint8_t *h, DWORD ss, gpt_hdr *g)
{
    uint8_t tmp[4096];

    if (memcmp(h, "EFI PART", 8) != 0)
        return false;
    g->size = le32(h + 12);
    if (g->size < 92 || g->size > ss)
        return false;
    memcpy(tmp, h, g->size);
    memset(tmp + 16, 0, 4);
    g->crc = le32(h + 16);
    if (crc32(tmp, g->size) != g->crc)
        return false;
    g->my_lba = le64(h + 24);
    g->alt_lba = le64(h + 32);
    g->first_usable = le64(h + 40);
    g->last_usable = le64(h + 48);
    memcpy(&g->disk_guid, h + 56, 16);
    g->entries_lba = le64(h + 72);
    g->entries = le32(h + 80);
    g->entry_size = le32(h + 84);
    g->entries_crc = le32(h + 88);
    if (g->entry_size < 128 || g->entry_size % 8 || g->entries == 0 ||
        (uint64_t)g->entries * g->entry_size > MAX_GPT_ARRAY || g->entries_lba < 2 ||
        g->entries_lba > MAX_LBA || g->alt_lba > MAX_LBA || g->first_usable > MAX_LBA || g->last_usable > MAX_LBA)
        return false;
    return true;
}

static uint64_t array_sectors(const gpt_hdr *g, DWORD ss)
{
    return ((uint64_t)g->entries * g->entry_size + ss - 1) / ss;
}

static const GUID zero_guid = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

static bool read_gpt(image *im, dd_layout *l, const uint8_t *mbr, DWORD ss)
{
    uint8_t hdr[4096];
    gpt_hdr g, b;
    uint8_t *arr;
    size_t len;
    int64_t end, iso_limit;
    uint32_t i;
    int j;

    if (!image_read(im, ss, hdr, ss) || !parse_gpt_header(hdr, ss, &g) || g.my_lba != 1)
        return false;
    len = (size_t)g.entries * g.entry_size;
    arr = malloc(len);
    if (!arr)
        return false;
    if (!image_read(im, (int64_t)g.entries_lba * ss, arr, len)) {
        free(arr);
        note(l, L"the GPT partition array lies beyond the end of the image");
        return false;
    }
    if (crc32(arr, len) != g.entries_crc) {
        free(arr);
        note(l, L"the GPT partition array is damaged (bad checksum)");
        return false;
    }

    l->scheme = DD_SCHEME_GPT;
    l->sector_size = ss;
    l->disk_guid = g.disk_guid;
    end = (int64_t)(g.entries_lba + array_sectors(&g, ss)) * ss;
    for (i = 0; i < g.entries; i++) {
        const uint8_t *e = arr + (size_t)i * g.entry_size;
        uint64_t first = le64(e + 32), last = le64(e + 40);
        dd_partition *p;
        if (memcmp(e, &zero_guid, 16) == 0)
            continue;
        if (last < first) {
            note(l, L"GPT entry %lu ends before it starts", i + 1);
            continue;
        }
        if (last >= MAX_LBA) {
            note(l, L"GPT entry %lu ends past any disk", i + 1);
            continue;
        }
        end = max64(end, (int64_t)(last + 1) * ss);
        p = add_part(l);
        if (!p)
            continue;
        p->number = (int)i + 1;
        p->start = (int64_t)first * ss;
        p->size = (int64_t)(last - first + 1) * ss;
        memcpy(&p->type, e, 16);
        memcpy(&p->id, e + 16, 16);
        p->attributes = le64(e + 48);
        for (j = 0; j < 36; j++)
            p->name[j] = (wchar_t)le16(e + 56 + 2 * j);
        p->name[36] = 0;
    }
    free(arr);

    /* hybrid MBRs give BIOSes their own view of some partitions */
    for (j = 0; j < 4; j++) {
        const uint8_t *e = mbr + 446 + 16 * j;
        if (e[4] != 0 && e[4] != 0xEE)
            end = max64(end, ((int64_t)le32(e + 8) + le32(e + 12)) * ss);
    }

    /* the image's own backup GPT is rebuilt, not copied, so an ISO volume
       that runs over it only counts up to there */
    iso_limit = INT64_MAX;
    if (im->xz || im->rd) {
        /* the backup is at the very end; checking it would mean
           decompressing the whole image, and it is rebuilt anyway */
        if (g.alt_lba > array_sectors(&g, ss))
            iso_limit = (int64_t)(g.alt_lba - array_sectors(&g, ss)) * ss;
    } else if (g.alt_lba > 1 && (int64_t)(g.alt_lba + 1) * ss <= im->size &&
               image_read(im, (int64_t)g.alt_lba * ss, hdr, ss) && parse_gpt_header(hdr, ss, &b) &&
               b.my_lba == g.alt_lba) {
        l->gpt_backup_ok = true;
        iso_limit = (int64_t)b.entries_lba * ss;
    } else {
        note(l, L"the image's backup GPT is missing or damaged; it will be rebuilt");
    }
    if (l->iso_size)
        end = max64(end, l->iso_size < iso_limit ? l->iso_size : iso_limit);
    l->data_end = (end + ss - 1) / ss * ss;     /* an ISO can end mid-sector */
    l->gpt_reserve = (int64_t)(array_sectors(&g, ss) + 1) * ss;
    return true;
}

/* ---- public ------------------------------------------------------------ */

static void read_layout(image *im, dd_layout *l)
{
    uint8_t mbr[512], pvd[2048];
    bool any = false;
    int i;

    memset(l, 0, sizeof *l);
    l->sector_size = 512;
    l->image_size = im->size;
    l->data_end = im->size;
    l->compressed = im->xz != NULL;
    l->file_size = im->file_size;

    if (im->size >= 0x8000 + 2048 && image_read(im, 0x8000, pvd, sizeof pvd) &&
        pvd[0] == 1 && memcmp(pvd + 1, "CD001", 5) == 0)
        l->iso_size = (int64_t)le32(pvd + 80) * le16(pvd + 128);

    if (im->size < 512 || !image_read(im, 0, mbr, 512))
        return;
    if (mbr_valid(mbr, im->size, &any) && any) {
        bool protective = false;
        for (i = 0; i < 4; i++)
            if (mbr[446 + 16 * i + 4] == 0xEE)
                protective = true;
        if (protective) {
            if (!read_gpt(im, l, mbr, 512) && !read_gpt(im, l, mbr, 4096)) {
                l->scheme = DD_SCHEME_NONE;
                l->count = 0;
                l->data_end = im->size;
                note(l, L"the image has a protective MBR but no valid GPT; it will be written whole");
            }
        } else {
            read_mbr(im, l, mbr);
            if (l->iso_size)
                l->data_end = max64(l->data_end, (l->iso_size + l->sector_size - 1) / l->sector_size * l->sector_size);
        }
    }
}

bool dd_read_layout(const wchar_t *path, dd_layout *l, wchar_t *err, size_t errlen)
{
    image im;

    if (!image_open(&im, path)) {
        wchar_t m[256];
        dd_errstr(GetLastError(), m, 256);
        memset(l, 0, sizeof *l);
        l->sector_size = 512;
        _snwprintf_s(err, errlen, _TRUNCATE, L"cannot open '%ls': %ls", path, m);
        return false;
    }
    read_layout(&im, l);
    image_close(&im);
    return true;
}

void dd__read_layout_handle(HANDLE h, int64_t size, dd_layout *l)
{
    image im;

    memset(&im, 0, sizeof im);
    im.h = h;                   /* not ours: image_close is not called */
    im.size = im.file_size = size;
    image_sector_size(&im);
    read_layout(&im, l);
}

void dd__read_layout_reader(dd__read_at_fn rd, void *ctx, int64_t size, dd_layout *l)
{
    image im;

    memset(&im, 0, sizeof im);
    im.h = INVALID_HANDLE_VALUE;
    im.rd = rd;
    im.rd_ctx = ctx;
    im.size = im.file_size = size;
    read_layout(&im, l);
}

int64_t dd_layout_needed(const dd_layout *l)
{
    return l->data_end + (l->scheme == DD_SCHEME_GPT ? l->gpt_reserve : 0);
}

static const struct { uint8_t t; const wchar_t *name; } mbr_types[] = {
    { 0x01, L"FAT12" }, { 0x04, L"FAT16 (<32 MB)" }, { 0x05, L"Extended" }, { 0x06, L"FAT16" },
    { 0x07, L"NTFS / exFAT" }, { 0x0B, L"FAT32" }, { 0x0C, L"FAT32 (LBA)" }, { 0x0E, L"FAT16 (LBA)" },
    { 0x0F, L"Extended (LBA)" }, { 0x11, L"Hidden FAT12" }, { 0x12, L"Diagnostics / recovery" },
    { 0x14, L"Hidden FAT16" }, { 0x16, L"Hidden FAT16" }, { 0x17, L"Hidden NTFS" },
    { 0x1B, L"Hidden FAT32" }, { 0x1C, L"Hidden FAT32 (LBA)" }, { 0x27, L"Windows recovery" },
    { 0x42, L"Windows dynamic (LDM)" }, { 0x82, L"Linux swap" }, { 0x83, L"Linux" },
    { 0x85, L"Linux extended" }, { 0x8E, L"Linux LVM" }, { 0xA5, L"FreeBSD" }, { 0xA6, L"OpenBSD" },
    { 0xA8, L"macOS UFS" }, { 0xA9, L"NetBSD" }, { 0xAF, L"macOS HFS+" }, { 0xDA, L"Non-FS data" },
    { 0xEE, L"GPT protective" }, { 0xEF, L"EFI system" }, { 0xFD, L"Linux RAID" },
};

static const struct { GUID g; const wchar_t *name; } gpt_types[] = {
    { { 0xC12A7328, 0xF81F, 0x11D2, { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B } }, L"EFI system" },
    { { 0x21686148, 0x6449, 0x6E6F, { 0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49 } }, L"BIOS boot" },
    { { 0xEBD0A0A2, 0xB9E5, 0x4433, { 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 } }, L"Basic data" },
    { { 0xE3C9E316, 0x0B5C, 0x4DB8, { 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE } }, L"Microsoft reserved" },
    { { 0xDE94BBA4, 0x06D1, 0x4D40, { 0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC } }, L"Windows recovery" },
    { { 0x5808C8AA, 0x7E8F, 0x42E0, { 0x85, 0xD2, 0xE1, 0xE9, 0x04, 0x34, 0xCF, 0xB3 } }, L"Windows LDM metadata" },
    { { 0xAF9B60A0, 0x1431, 0x4F62, { 0xBC, 0x68, 0x33, 0x11, 0x71, 0x4A, 0x69, 0xAD } }, L"Windows LDM data" },
    { { 0x0FC63DAF, 0x8483, 0x4772, { 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 } }, L"Linux filesystem" },
    { { 0x4F68BCE3, 0xE8CD, 0x4DB1, { 0x96, 0xE7, 0xFB, 0xCA, 0xF9, 0x84, 0xB7, 0x09 } }, L"Linux root (x86-64)" },
    { { 0xB921B045, 0x1DF0, 0x41C3, { 0xAF, 0x44, 0x4C, 0x6F, 0x28, 0x0D, 0x3F, 0xAE } }, L"Linux root (ARM64)" },
    { { 0x44479540, 0xF297, 0x41B2, { 0x9A, 0xF7, 0xD1, 0x31, 0xD5, 0xF0, 0x45, 0x8A } }, L"Linux root (x86)" },
    { { 0x69DAD710, 0x2CE4, 0x4E3C, { 0xB1, 0x6C, 0x21, 0xA1, 0xD4, 0x9A, 0xBE, 0xD3 } }, L"Linux root (ARM)" },
    { { 0xBC13C2FF, 0x59E6, 0x4262, { 0xA3, 0x52, 0xB2, 0x75, 0xFD, 0x6F, 0x71, 0x72 } }, L"Linux /boot" },
    { { 0x933AC7E1, 0x2EB4, 0x4F13, { 0xB8, 0x44, 0x0E, 0x14, 0xE2, 0xAE, 0xF9, 0x15 } }, L"Linux /home" },
    { { 0x0657FD6D, 0xA4AB, 0x43C4, { 0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F } }, L"Linux swap" },
    { { 0xE6D6D379, 0xF507, 0x44C2, { 0xA2, 0x3C, 0x23, 0x8F, 0x2A, 0x3D, 0xF9, 0x28 } }, L"Linux LVM" },
    { { 0xA19D880F, 0x05FC, 0x4D3B, { 0xA0, 0x06, 0x74, 0x3F, 0x0F, 0x84, 0x91, 0x1E } }, L"Linux RAID" },
    { { 0xCA7D7CCB, 0x63ED, 0x4C53, { 0x86, 0x1C, 0x17, 0x42, 0x53, 0x60, 0x59, 0xCC } }, L"Linux LUKS" },
    { { 0x48465300, 0x0000, 0x11AA, { 0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC } }, L"Apple HFS+" },
    { { 0x7C3457EF, 0x0000, 0x11AA, { 0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC } }, L"Apple APFS" },
    { { 0x516E7CB4, 0x6ECF, 0x11D6, { 0x8F, 0xF8, 0x00, 0x02, 0x2D, 0x09, 0x71, 0x2B } }, L"FreeBSD data" },
    { { 0xFE3A2392, 0x2231, 0x4FA0, { 0x88, 0x63, 0x6E, 0x53, 0x76, 0x6B, 0xBB, 0x27 } }, L"ChromeOS kernel" },
    { { 0x3CB8E202, 0x3B7E, 0x47DD, { 0x8A, 0x3C, 0x7F, 0xF2, 0xA1, 0x3C, 0xFC, 0xEC } }, L"ChromeOS root" },
};

void dd_partition_type_name(const dd_layout *l, const dd_partition *p, wchar_t *buf, size_t n)
{
    size_t i;

    if (l->scheme == DD_SCHEME_GPT) {
        const GUID *g = &p->type;
        for (i = 0; i < sizeof gpt_types / sizeof *gpt_types; i++)
            if (memcmp(g, &gpt_types[i].g, sizeof *g) == 0) {
                wcsncpy_s(buf, n, gpt_types[i].name, _TRUNCATE);
                return;
            }
        _snwprintf_s(buf, n, _TRUNCATE, L"%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                     g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2],
                     g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
        return;
    }
    for (i = 0; i < sizeof mbr_types / sizeof *mbr_types; i++)
        if (mbr_types[i].t == p->mbr_type) {
            _snwprintf_s(buf, n, _TRUNCATE, L"%ls (%02X)", mbr_types[i].name, p->mbr_type);
            return;
        }
    _snwprintf_s(buf, n, _TRUNCATE, L"Type %02X", p->mbr_type);
}

/* ---- rebuilding a GPT for the output disk ------------------------------- */

void dd__gpt_fix_free(dd__gpt_fix *f)
{
    free(f->mbr);
    free(f->primary);
    free(f->backup);
    free(f->entries);
    memset(f, 0, sizeof *f);
}

bool dd__gpt_fix_prepare(const wchar_t *path, int64_t disk_size, dd__gpt_fix *f, wchar_t *err, size_t errlen)
{
    dd_layout *l = malloc(sizeof *l);
    image im;
    gpt_hdr g;
    uint64_t disk_last, esec, last_used = 0, new_last_usable;
    DWORD ss;
    size_t len;
    uint32_t i;
    int k;
    bool ok = false;

    memset(f, 0, sizeof *f);
    if (!l || !dd_read_layout(path, l, err, errlen)) {
        free(l);
        return false;
    }
    ss = l->sector_size;
    if (l->scheme != DD_SCHEME_GPT) {
        free(l);
        _snwprintf_s(err, errlen, _TRUNCATE, L"'%ls' has no valid GPT", path);
        return false;
    }
    free(l);
    if (!image_open(&im, path)) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"cannot open '%ls'", path);
        return false;
    }
    f->ss = ss;
    f->mbr = malloc(ss);
    f->primary = malloc(ss);
    f->backup = malloc(ss);
    if (!f->mbr || !f->primary || !f->backup ||
        !image_read(&im, 0, f->mbr, ss) || !image_read(&im, ss, f->primary, ss) ||
        !parse_gpt_header(f->primary, ss, &g)) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"cannot read the GPT of '%ls'", path);
        goto out;
    }
    esec = array_sectors(&g, ss);
    len = (size_t)(esec * ss);
    f->entries = calloc(1, len);
    if (!f->entries || !image_read(&im, (int64_t)g.entries_lba * ss, f->entries,
                                   (size_t)g.entries * g.entry_size)) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"cannot read the GPT partition array of '%ls'", path);
        goto out;
    }
    f->entries_len = (DWORD)len;

    for (i = 0; i < g.entries; i++) {
        const uint8_t *e = f->entries + (size_t)i * g.entry_size;
        if (memcmp(e, &zero_guid, 16) != 0 && le64(e + 40) > last_used)
            last_used = le64(e + 40);
    }
    disk_last = (uint64_t)(disk_size / ss) - 1;
    if (disk_last < esec + 2 + g.entries_lba) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"the disk is too small for a GPT");
        goto out;
    }
    new_last_usable = disk_last - esec - 1;
    if (last_used > new_last_usable) {
        _snwprintf_s(err, errlen, _TRUNCATE,
                     L"the image's partitions end at sector %llu, past the last usable sector of the disk (%llu)",
                     (unsigned long long)last_used, (unsigned long long)new_last_usable);
        goto out;
    }

    /* primary: points at the new backup and may use the whole disk */
    put64(f->primary + 32, disk_last);
    put64(f->primary + 48, new_last_usable);
    put32(f->primary + 16, 0);
    put32(f->primary + 16, crc32(f->primary, g.size));

    /* backup: the same header seen from the end of the disk */
    memcpy(f->backup, f->primary, ss);
    put64(f->backup + 24, disk_last);
    put64(f->backup + 32, 1);
    put64(f->backup + 72, disk_last - esec);
    put32(f->backup + 16, 0);
    put32(f->backup + 16, crc32(f->backup, g.size));

    f->backup_entries_off = (int64_t)(disk_last - esec) * ss;
    f->backup_off = (int64_t)disk_last * ss;

    /* a protective MBR entry that covered the whole image now covers the disk */
    for (k = 0; k < 4; k++) {
        uint8_t *e = f->mbr + 446 + 16 * k;
        if (e[4] == 0xEE && le32(e + 8) == 1 &&
            (le32(e + 12) == 0xFFFFFFFFu || (uint64_t)le32(e + 12) >= g.alt_lba))
            put32(e + 12, disk_last > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)disk_last);
    }
    ok = true;
out:
    image_close(&im);
    if (!ok)
        dd__gpt_fix_free(f);
    return ok;
}
