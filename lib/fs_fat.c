/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later

    FAT12, FAT16 and FAT32 for sparse reading.  A cluster is in use when its
    entry in any copy of the FAT is not zero, so the copies need not agree
    (bad clusters count as in use).  Everything before the data area - boot
    sectors, the FATs and a FAT12/16 root directory - is always read.

    Not cleanly unmounted means the "clean shutdown" bit in FAT entry 1 is
    clear (FAT16/32), or the dirty bit in the boot sector is set (Windows
    and Linux set it while the volume is mounted).  A cluster written but not
    yet linked into a FAT is then lost; --allow-unclean accepts that.

    On a volume Windows has mounted, both marks are Windows' working state:
    it sets them while it has changes to write and clears them later, even
    after it has granted dd the lock.  When dd holds the lock Windows has
    written everything, and what counts is whether Windows found the volume
    dirty when it mounted it (Linux and Windows both mark a dirty FAT
    volume in its boot sector, which Windows checks), so the marks on disk
    are ignored if Windows says it was clean.
*/
#include "fs.h"
#include <stdlib.h>
#include <string.h>

#define CHUNK (1u << 20)        /* FAT bytes read at a time */

typedef struct bpb {
    uint32_t bps;               /* bytes per sector */
    uint32_t spc;               /* sectors per cluster */
    uint32_t reserved, nfats, fat_sectors, root_sectors;
    uint64_t total_sectors;
    uint32_t clusters;
    int      bits;              /* 12, 16 or 32 */
} bpb;

static bool pow2(uint32_t v) { return v && !(v & (v - 1)); }

/* The BPB checks of the FAT specification; the FAT type comes from the
   cluster count, as it says, not from the "FAT16   " label. */
static bool parse(const uint8_t *b, bpb *p)
{
    uint32_t root_entries, fat16 = dd__le16(b + 22);
    uint64_t meta;

    if (b[0] != 0xEB && b[0] != 0xE9)
        return false;
    if (memcmp(b + 3, "EXFAT   ", 8) == 0 || memcmp(b + 3, "NTFS    ", 8) == 0)
        return false;
    p->bps = dd__le16(b + 11);
    p->spc = b[13];
    p->reserved = dd__le16(b + 14);
    p->nfats = b[16];
    root_entries = dd__le16(b + 17);
    p->total_sectors = dd__le16(b + 19) ? dd__le16(b + 19) : dd__le32(b + 32);
    p->fat_sectors = fat16 ? fat16 : dd__le32(b + 36);
    if (p->bps < 512 || p->bps > 4096 || !pow2(p->bps) || !pow2(p->spc) || p->reserved == 0 ||
        p->nfats == 0 || p->nfats > 4 || (b[21] != 0xF0 && b[21] < 0xF8) ||
        p->fat_sectors == 0 || p->total_sectors == 0)
        return false;
    p->root_sectors = (root_entries * 32 + p->bps - 1) / p->bps;
    meta = p->reserved + (uint64_t)p->nfats * p->fat_sectors + p->root_sectors;
    if (meta >= p->total_sectors || (p->total_sectors - meta) / p->spc > 0x0FFFFFF5u)
        return false;
    p->clusters = (uint32_t)((p->total_sectors - meta) / p->spc);
    if (p->clusters == 0)
        return false;
    p->bits = p->clusters < 4085 ? 12 : p->clusters < 65525 ? 16 : 32;
    if ((p->bits == 32) != (root_entries == 0) || (p->bits == 32 && fat16 != 0))
        return false;
    /* the FAT must hold an entry for every cluster */
    return (uint64_t)p->fat_sectors * p->bps * 8 / (unsigned)p->bits >= (uint64_t)p->clusters + 2;
}

static bool fat_probe(const uint8_t *head, int64_t vol_size)
{
    bpb p;
    (void)vol_size;
    return parse(head, &p);
}

static uint32_t entry(const uint8_t *fat, int bits, uint32_t n)
{
    switch (bits) {
    case 12: {
        uint16_t v = dd__le16(fat + n + n / 2);
        return n & 1 ? v >> 4 : v & 0xFFFu;
    }
    case 16:
        return dd__le16(fat + 2 * (size_t)n);
    default:
        return dd__le32(fat + 4 * (size_t)n) & 0x0FFFFFFFu;
    }
}

/* Marks the clusters this copy of the FAT has entries for. */
static DWORD read_fat(dd__fs_vol *v, const bpb *p, uint32_t copy, uint8_t *buf, dd__fs_map *m)
{
    int64_t base = ((int64_t)p->reserved + (int64_t)copy * p->fat_sectors) * p->bps;
    uint32_t last = p->clusters + 1, n, i;        /* entries 2..last */
    DWORD e;

    if (p->bits == 12) {
        /* at most 4084 clusters: the whole FAT fits the buffer */
        size_t len = (size_t)last + last / 2 + 2;
        if (len > (size_t)p->fat_sectors * p->bps)
            len = (size_t)p->fat_sectors * p->bps;
        memset(buf, 0, len + 2);
        if ((e = dd__fs_read(v, base, buf, len)) != 0)
            return e;
        for (n = 2; n <= last; n++)
            if (entry(buf, 12, n))
                dd__fs_set_bits(m, n - 2, 1);
        return 0;
    }
    for (n = 0; n <= last; ) {
        uint32_t size = (uint32_t)p->bits / 8, count = CHUNK / size;
        if (count > last + 1 - n)
            count = last + 1 - n;
        if ((e = dd__fs_read(v, base + (int64_t)n * size, buf, (size_t)count * size)) != 0)
            return e;
        for (i = 0; i < count; i++)
            if (n + i >= 2 && entry(buf, p->bits, i))
                dd__fs_set_bits(m, n + i - 2, 1);
        n += count;
    }
    return 0;
}

static dd__fs_status fat_map(dd__fs_vol *v, const uint8_t *head, bool allow_unclean, dd__fs_map *m)
{
    uint8_t *buf;
    uint8_t first[8];
    uint32_t copy;
    bpb p;
    DWORD e = 0;
    bool boot_dirty, fat_dirty;

    if (!parse(head, &p))
        return DD__FS_WHOLE;
    wcscpy_s(m->type, sizeof m->type / sizeof *m->type,
             p.bits == 12 ? L"FAT12" : p.bits == 16 ? L"FAT16" : L"FAT32");

    if (dd__fs_read(v, (int64_t)p.reserved * p.bps, first, sizeof first) != 0) {
        dd__fs_note(m, L"its FAT cannot be read");
        return DD__FS_WHOLE;
    }
    boot_dirty = (head[p.bits == 32 ? 0x41 : 0x25] & 1) != 0;
    fat_dirty = p.bits == 16 ? !(dd__le16(first + 2) & 0x8000u)
              : p.bits == 32 ? !(dd__le32(first + 4) & 0x08000000u) : false;
    m->unclean = (boot_dirty || fat_dirty) && !dd__fs_vol_os_clean(v);
    if (m->unclean && !allow_unclean) {
        dd__fs_note(m, fat_dirty ? L"it was not cleanly unmounted, says its FAT"
                                 : L"it was not cleanly unmounted, says its boot sector");
        return DD__FS_WHOLE;
    }

    m->size = (int64_t)p.total_sectors * p.bps;
    m->unit = p.spc * p.bps;
    m->map_start = ((int64_t)p.reserved + (int64_t)p.nfats * p.fat_sectors + p.root_sectors) * p.bps;
    buf = malloc(CHUNK + 8);
    if (!buf || !dd__fs_alloc_bits(m, p.clusters)) {
        free(buf);
        dd__fs_note(m, L"there is not enough memory to map it");
        return DD__FS_WHOLE;
    }
    for (copy = 0; copy < p.nfats && !e; copy++)
        e = read_fat(v, &p, copy, buf, m);
    free(buf);
    if (e) {
        dd__fs_note(m, e == ERROR_HANDLE_EOF ? L"its FAT runs past the end of the volume"
                                             : L"its FAT cannot be read");
        return DD__FS_WHOLE;
    }
    if (m->unclean)
        dd__fs_note(m, L"it was not cleanly unmounted: clusters written since may not be in its FAT");
    return DD__FS_MAPPED;
}

const dd__fs_driver dd__fs_fat = { fat_probe, fat_map };
