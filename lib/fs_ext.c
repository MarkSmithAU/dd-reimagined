/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later

    ext2, ext3 and ext4 for sparse reading.  The block bitmaps of the block
    groups say which blocks (clusters, with bigalloc) are in use.  A group
    whose bitmap was never initialised (BLOCK_UNINIT) holds only its own
    metadata.  On top of the bitmaps, every group's superblock and
    descriptor copies, bitmaps and inode table always count as in use.
    Block 0 of a 1 KiB-block file system (the boot block) is always read.

    Not cleanly unmounted means the superblock is not marked valid, has
    errors recorded, or its journal needs recovery.  With --allow-unclean,
    the journal is read: a block it would write, and every block a
    journalled copy of a bitmap marks, count as in use, so the image still
    holds everything the file system has after recovery.  Without a journal
    (ext2), blocks written since the last clean unmount may be lost.
*/
#include "fs.h"
#include <stdlib.h>
#include <string.h>

#define SB 1024

#define COMPAT_HAS_JOURNAL      0x0004u
#define COMPAT_SPARSE_SUPER2    0x0200u

#define INCOMPAT_RECOVER        0x0004u
#define INCOMPAT_JOURNAL_DEV    0x0008u
#define INCOMPAT_META_BG        0x0010u
#define INCOMPAT_EXTENTS        0x0040u
#define INCOMPAT_64BIT          0x0080u
#define INCOMPAT_FLEX_BG        0x0200u
#define INCOMPAT_CSUM_SEED      0x2000u
/* everything but compression (0x1) and journal_dev (0x8): none of the
   others changes what the block bitmaps mean */
#define INCOMPAT_KNOWN          0x3F7D6u

#define RO_SPARSE_SUPER         0x0001u
#define RO_HUGE_FILE            0x0008u
#define RO_GDT_CSUM             0x0010u
#define RO_DIR_NLINK            0x0020u
#define RO_EXTRA_ISIZE          0x0040u
#define RO_BIGALLOC             0x0200u
#define RO_METADATA_CSUM        0x0400u
#define RO_KNOWN                0x1FFFFu

#define BG_BLOCK_UNINIT         0x0002u

#define STATE_VALID             0x0001u
#define STATE_ERROR             0x0002u

#define JBD_MAGIC               0xC03B3998u
#define JBD_DESCRIPTOR          1u
#define JBD_COMMIT              2u
#define JBD_REVOKE              5u
#define JBD_64BIT               0x0002u
#define JBD_CSUM_V2             0x0008u
#define JBD_CSUM_V3             0x0010u
#define JBD_FAST_COMMIT         0x0020u
#define JBD_INCOMPAT_KNOWN      0x001Fu
#define JTAG_ESCAPE             1u
#define JTAG_SAME_UUID          2u
#define JTAG_LAST               8u
#define MAX_JOURNAL_BLOCKS      (1u << 22)

typedef struct ext {
    uint32_t block;             /* bytes */
    uint32_t cluster_bits;      /* log2(blocks per cluster) */
    uint64_t blocks;
    uint32_t first_data_block, blocks_per_group, clusters_per_group;
    uint32_t inodes_per_group, inode_size;
    uint32_t compat, incompat, ro;
    uint32_t desc_size, descs_per_block, groups, gdt_blocks;
    uint32_t first_meta_bg, reserved_gdt, backup_bgs[2];
    uint32_t journal_inum;
    uint16_t state;
    uint8_t  uuid[16];
    uint32_t csum_seed;         /* metadata_csum: crc32c of the uuid, or stored */
    uint8_t *gd;                /* every group descriptor */
} ext;

/* ---- group descriptor checksums ------------------------------------------ */

/* As the kernel's crc16() and crc32c(): reflected, no final inversion. */
static uint16_t crc16(uint16_t crc, const uint8_t *p, size_t n)
{
    int k;

    while (n--) {
        crc ^= *p++;
        for (k = 0; k < 8; k++)
            crc = (uint16_t)(crc & 1 ? (crc >> 1) ^ 0xA001u : crc >> 1);
    }
    return crc;
}

static uint32_t crc32c(uint32_t crc, const uint8_t *p, size_t n)
{
    int k;

    while (n--) {
        crc ^= *p++;
        for (k = 0; k < 8; k++)
            crc = crc & 1 ? (crc >> 1) ^ 0x82F63B78u : crc >> 1;
    }
    return crc;
}

static bool parse(const uint8_t *s, ext *x)
{
    uint32_t log_block = dd__le32(s + 24), log_cluster = dd__le32(s + 28), rev = dd__le32(s + 76);
    uint64_t span;

    memset(x, 0, sizeof *x);
    if (dd__le16(s + 56) != 0xEF53 || log_block > 6)
        return false;
    x->block = 1024u << log_block;
    x->first_data_block = dd__le32(s + 20);
    x->blocks_per_group = dd__le32(s + 32);
    x->inodes_per_group = dd__le32(s + 40);
    x->state = dd__le16(s + 58);
    if (rev >= 1) {
        x->inode_size = dd__le16(s + 88);
        x->compat = dd__le32(s + 92);
        x->incompat = dd__le32(s + 96);
        x->ro = dd__le32(s + 100);
        x->reserved_gdt = dd__le16(s + 206);
        x->journal_inum = dd__le32(s + 224);
        x->first_meta_bg = dd__le32(s + 260);
        x->backup_bgs[0] = dd__le32(s + 588);
        x->backup_bgs[1] = dd__le32(s + 592);
        memcpy(x->uuid, s + 0x68, 16);
        x->csum_seed = x->incompat & INCOMPAT_CSUM_SEED ? dd__le32(s + 0x270) : crc32c(~0u, x->uuid, 16);
    } else {
        x->inode_size = 128;
    }
    x->blocks = dd__le32(s + 4);
    if (x->incompat & INCOMPAT_64BIT)
        x->blocks |= (uint64_t)dd__le32(s + 336) << 32;
    x->desc_size = (x->incompat & INCOMPAT_64BIT) ? dd__le16(s + 254) : 32;
    if (x->ro & RO_BIGALLOC) {
        if (log_cluster < log_block || log_cluster - log_block > 16)
            return false;
        x->cluster_bits = log_cluster - log_block;
        x->clusters_per_group = dd__le32(s + 36);
    } else {
        x->clusters_per_group = x->blocks_per_group;
    }
    if (x->blocks_per_group == 0 || x->clusters_per_group == 0 || x->clusters_per_group % 8 ||
        x->clusters_per_group > 8 * x->block ||
        (uint64_t)x->clusters_per_group << x->cluster_bits != x->blocks_per_group ||
        x->inodes_per_group == 0 || x->inode_size < 128 || x->inode_size > x->block ||
        x->desc_size < 32 || x->desc_size > x->block || (x->desc_size & (x->desc_size - 1)) ||
        x->first_data_block > 1 || (x->cluster_bits && x->first_data_block) ||
        x->blocks <= x->first_data_block || x->blocks > ((uint64_t)1 << 40))
        return false;
    span = x->blocks - x->first_data_block;
    x->descs_per_block = x->block / x->desc_size;
    x->groups = (uint32_t)((span + x->blocks_per_group - 1) / x->blocks_per_group);
    x->gdt_blocks = (x->groups + x->descs_per_block - 1) / x->descs_per_block;
    return (span + x->blocks_per_group - 1) / x->blocks_per_group <= 0xFFFFFFFFu;
}

static bool ext_probe(const uint8_t *head, int64_t vol_size)
{
    ext x;
    (void)vol_size;
    return parse(head + SB, &x);
}

static const wchar_t *type_name(const ext *x)
{
    if ((x->incompat & (INCOMPAT_EXTENTS | INCOMPAT_64BIT | INCOMPAT_FLEX_BG)) ||
        (x->ro & (RO_HUGE_FILE | RO_GDT_CSUM | RO_DIR_NLINK | RO_EXTRA_ISIZE | RO_BIGALLOC | RO_METADATA_CSUM)))
        return L"ext4";
    return x->compat & COMPAT_HAS_JOURNAL ? L"ext3" : L"ext2";
}

/* ---- groups -------------------------------------------------------------- */

static uint64_t group_first(const ext *x, uint32_t g)
{
    return x->first_data_block + (uint64_t)g * x->blocks_per_group;
}

static bool power_of(uint32_t g, uint32_t b)
{
    while (g > 1 && g % b == 0)
        g /= b;
    return g == 1;
}

/* Whether group g holds a copy of the superblock (ext4_bg_has_super). */
static bool has_super(const ext *x, uint32_t g)
{
    if (g == 0)
        return true;
    if (x->compat & COMPAT_SPARSE_SUPER2)
        return g == x->backup_bgs[0] || g == x->backup_bgs[1];
    if (g <= 1 || !(x->ro & RO_SPARSE_SUPER))
        return true;
    if (!(g & 1))
        return false;
    return power_of(g, 3) || power_of(g, 5) || power_of(g, 7);
}

/* The superblock is at byte 1024: block 1 of a 1 KiB-block file system,
   even with bigalloc, where block 1 is not the first data block. */
static bool sb_in_block_1(const ext *x)
{
    return x->block == 1024 && x->first_data_block == 0;
}

/* Where descriptor block i is (ext4's descriptor_loc). */
static uint64_t gdt_block(const ext *x, uint32_t i)
{
    uint32_t g;

    if (!(x->incompat & INCOMPAT_META_BG) || i < x->first_meta_bg)
        return (x->block == 1024 ? 1 : 0) + 1 + (uint64_t)i;
    g = i * x->descs_per_block;
    return group_first(x, g) + (has_super(x, g) ? 1 : 0) + (g == 0 && sb_in_block_1(x) ? 1 : 0);
}

/* Blocks at the start of group g that hold the superblock and descriptor
   copies, and the reserved descriptor blocks (ext4_num_base_meta_blocks). */
static uint64_t base_meta_blocks(const ext *x, uint32_t g)
{
    uint64_t n = has_super(x, g) ? 1 : 0;

    if (!(x->incompat & INCOMPAT_META_BG) || g < (uint64_t)x->first_meta_bg * x->descs_per_block) {
        if (n)
            n += ((x->incompat & INCOMPAT_META_BG) ? x->first_meta_bg : x->gdt_blocks) + x->reserved_gdt;
    } else {
        uint32_t r = g % x->descs_per_block;
        if (r == 0 || r == 1 || r == x->descs_per_block - 1)
            n++;
    }
    if (g == 0 && sb_in_block_1(x))
        n++;                    /* block 0 comes before the superblock */
    return n;
}

static uint64_t desc_field(const ext *x, uint32_t g, size_t lo, size_t hi)
{
    const uint8_t *d = x->gd + (size_t)g * x->desc_size;
    uint64_t v = dd__le32(d + lo);

    if (x->desc_size >= 64)
        v |= (uint64_t)dd__le32(d + hi) << 32;
    return v;
}

static uint64_t block_bitmap(const ext *x, uint32_t g) { return desc_field(x, g, 0x00, 0x20); }
static uint64_t inode_bitmap(const ext *x, uint32_t g) { return desc_field(x, g, 0x04, 0x24); }
static uint64_t inode_table(const ext *x, uint32_t g)  { return desc_field(x, g, 0x08, 0x28); }

static bool block_uninit(const ext *x, uint32_t g)
{
    return (x->ro & (RO_GDT_CSUM | RO_METADATA_CSUM)) &&
           (dd__le16(x->gd + (size_t)g * x->desc_size + 0x12) & BG_BLOCK_UNINIT);
}

/* ext4_group_desc_csum(): the descriptor without its own checksum field
   (0x1E), after the group number. */
static bool desc_csum_ok(const ext *x, uint32_t g)
{
    const uint8_t *d = x->gd + (size_t)g * x->desc_size;
    uint8_t le[4] = { (uint8_t)g, (uint8_t)(g >> 8), (uint8_t)(g >> 16), (uint8_t)(g >> 24) };
    static const uint8_t zero[2] = { 0, 0 };
    uint16_t want = dd__le16(d + 0x1E), crc;

    if (x->ro & RO_METADATA_CSUM) {
        uint32_t c = crc32c(x->csum_seed, le, 4);
        c = crc32c(c, d, 0x1E);
        c = crc32c(c, zero, 2);
        if (x->desc_size > 0x20)
            c = crc32c(c, d + 0x20, x->desc_size - 0x20);
        return (uint16_t)c == want;
    }
    crc = crc16(0xFFFF, x->uuid, 16);
    crc = crc16(crc, le, 4);
    crc = crc16(crc, d, 0x1E);
    if ((x->incompat & INCOMPAT_64BIT) && x->desc_size > 0x20)
        crc = crc16(crc, d + 0x20, x->desc_size - 0x20);
    return crc == want;
}

/* Marks blocks [b, b + n) in use. */
static void mark(const ext *x, dd__fs_map *m, uint64_t b, uint64_t n)
{
    uint64_t first, last;

    if (n == 0 || b >= x->blocks)
        return;
    if (n > x->blocks - b)
        n = x->blocks - b;
    if (b < x->first_data_block) {          /* block 0 of a 1 KiB file system */
        if (b + n <= x->first_data_block)
            return;
        n -= x->first_data_block - b;
        b = x->first_data_block;
    }
    first = (b - x->first_data_block) >> x->cluster_bits;
    last = (b + n - 1 - x->first_data_block) >> x->cluster_bits;
    dd__fs_set_bits(m, first, last - first + 1);
}

static DWORD read_block(dd__fs_vol *v, const ext *x, uint64_t b, uint8_t *buf)
{
    if (b >= x->blocks)
        return ERROR_INVALID_DATA;
    return dd__fs_read(v, (int64_t)(b * x->block), buf, x->block);
}

/* ORs a group's block bitmap into the map. */
static void or_bitmap(const ext *x, dd__fs_map *m, uint32_t g, const uint8_t *bitmap)
{
    uint64_t first = (uint64_t)g * x->clusters_per_group, bytes, i;

    if (first >= m->nbits)
        return;
    bytes = (m->nbits - first < x->clusters_per_group ? m->nbits - first : x->clusters_per_group);
    bytes = (bytes + 7) / 8;
    for (i = 0; i < bytes; i++)
        m->bits[first / 8 + i] |= bitmap[i];
}

static DWORD read_groups(dd__fs_vol *v, ext *x, dd__fs_map *m, uint8_t *buf)
{
    uint64_t itable = ((uint64_t)x->inodes_per_group * x->inode_size + x->block - 1) / x->block;
    uint32_t i, g;
    DWORD e;

    x->gd = malloc((size_t)x->gdt_blocks * x->block);
    if (!x->gd)
        return ERROR_NOT_ENOUGH_MEMORY;
    for (i = 0; i < x->gdt_blocks; i++)
        if ((e = read_block(v, x, gdt_block(x, i), x->gd + (size_t)i * x->block)) != 0)
            return e;
    for (g = 0; g < x->groups; g++) {
        if (block_bitmap(x, g) >= x->blocks || inode_bitmap(x, g) >= x->blocks ||
            inode_table(x, g) >= x->blocks)
            return ERROR_INVALID_DATA;
        if (!block_uninit(x, g)) {
            if ((e = read_block(v, x, block_bitmap(x, g), buf)) != 0)
                return e;
            or_bitmap(x, m, g, buf);
        } else if (!desc_csum_ok(x, g)) {
            /* the kernel refuses such a descriptor, and e2fsck reads the
               bitmap after all: the flag says nothing to be trusted */
            return ERROR_CRC;
        }
        /* an uninitialised group holds only metadata, which this marks;
           for the others it is already in the bitmap */
        mark(x, m, group_first(x, g), base_meta_blocks(x, g));
        mark(x, m, block_bitmap(x, g), 1);
        mark(x, m, inode_bitmap(x, g), 1);
        mark(x, m, inode_table(x, g), itable);
    }
    return 0;
}

/* ---- the journal (--allow-unclean) --------------------------------------- */

typedef struct journal {
    uint64_t *map;              /* journal block -> file system block, 0 = hole */
    uint32_t  len;
    uint32_t  nodes;            /* extent tree blocks still to read: a real
                                   tree has fewer than the file has blocks */
} journal;

/* node is size bytes: the 60 of i_block at the root, a block below it. */
static DWORD map_extents(dd__fs_vol *v, const ext *x, const uint8_t *node, size_t size, int depth, journal *j)
{
    uint32_t n = dd__le16(node + 2), i, k;
    uint8_t *child;
    DWORD e = 0;

    if (dd__le16(node) != 0xF30A || depth > 5 || 12 + 12 * (size_t)n > size)
        return ERROR_INVALID_DATA;
    if (dd__le16(node + 6) == 0) {
        for (i = 0; i < n; i++) {
            const uint8_t *ex = node + 12 + 12 * i;
            uint32_t first = dd__le32(ex), len = dd__le16(ex + 4);
            uint64_t start = (uint64_t)dd__le16(ex + 6) << 32 | dd__le32(ex + 8);
            if (len > 32768)
                len -= 32768;   /* unwritten */
            for (k = 0; k < len && first + k < j->len; k++)
                j->map[first + k] = start + k;
        }
        return 0;
    }
    child = malloc(x->block);
    if (!child)
        return ERROR_NOT_ENOUGH_MEMORY;
    for (i = 0; i < n && !e; i++) {
        const uint8_t *ix = node + 12 + 12 * i;
        uint64_t leaf = (uint64_t)dd__le16(ix + 8) << 32 | dd__le32(ix + 4);
        if (j->nodes-- == 0) {
            e = ERROR_INVALID_DATA;     /* nodes shared, to make it endless */
            break;
        }
        e = read_block(v, x, leaf, child);
        if (!e)
            e = map_extents(v, x, child, x->block, depth + 1, j);
    }
    free(child);
    return e;
}

/* Block-mapped files: i_block holds 12 direct, then an indirect, a double
   and a triple indirect block. */
static DWORD map_indirect(dd__fs_vol *v, const ext *x, uint32_t b, int level, uint64_t *lblk, journal *j)
{
    uint32_t per = x->block / 4, i;
    uint64_t span = 1;
    uint8_t *buf;
    DWORD e = 0;
    int k;

    for (k = 1; k < level; k++)
        span *= per;
    if (level == 0) {
        if (*lblk < j->len)
            j->map[*lblk] = b;
        (*lblk)++;
        return 0;
    }
    if (b == 0) {
        *lblk += span * per;
        return 0;
    }
    buf = malloc(x->block);
    if (!buf)
        return ERROR_NOT_ENOUGH_MEMORY;
    if ((e = read_block(v, x, b, buf)) == 0)
        for (i = 0; i < per && !e && *lblk < j->len; i++) {
            uint32_t c = dd__le32(buf + 4 * (size_t)i);
            if (c == 0)
                *lblk += span;
            else
                e = map_indirect(v, x, c, level - 1, lblk, j);
        }
    free(buf);
    return e;
}

static DWORD open_journal(dd__fs_vol *v, const ext *x, journal *j)
{
    uint32_t g = (x->journal_inum - 1) / x->inodes_per_group, idx = (x->journal_inum - 1) % x->inodes_per_group;
    uint8_t inode[160];
    uint64_t size, lblk = 0;
    DWORD e;
    int i;

    if (g >= x->groups)
        return ERROR_INVALID_DATA;
    if ((e = dd__fs_read(v, (int64_t)(inode_table(x, g) * x->block + (uint64_t)idx * x->inode_size),
                         inode, 128)) != 0)
        return e;
    size = dd__le32(inode + 4) | (uint64_t)dd__le32(inode + 0x6C) << 32;
    if (size / x->block < 2 || size / x->block > MAX_JOURNAL_BLOCKS)
        return ERROR_INVALID_DATA;
    j->len = (uint32_t)(size / x->block);
    j->nodes = j->len;
    j->map = calloc(j->len, sizeof *j->map);
    if (!j->map)
        return ERROR_NOT_ENOUGH_MEMORY;
    if (dd__le32(inode + 0x20) & 0x80000u)         /* EXT4_EXTENTS_FL */
        return map_extents(v, x, inode + 0x28, 60, 0, j);
    for (i = 0; i < 12; i++)
        if ((e = map_indirect(v, x, dd__le32(inode + 0x28 + 4 * i), 0, &lblk, j)) != 0)
            return e;
    for (i = 1; i <= 3 && !e; i++)
        e = map_indirect(v, x, dd__le32(inode + 0x28 + 4 * (11 + i)), i, &lblk, j);
    return e;
}

static DWORD read_jblock(dd__fs_vol *v, const ext *x, const journal *j, uint32_t b, uint8_t *buf)
{
    if (b >= j->len || j->map[b] == 0)
        return ERROR_INVALID_DATA;
    return read_block(v, x, j->map[b], buf);
}

typedef struct bitmap_loc {
    uint64_t block;
    uint32_t group;
} bitmap_loc;

static int by_block(const void *a, const void *b)
{
    const bitmap_loc *p = a, *q = b;
    return p->block < q->block ? -1 : p->block > q->block;
}

/* Walks the log from its start, as recovery would.  Uncommitted and
   revoked blocks count too: more in use is always safe. */
static DWORD replay_bitmaps(dd__fs_vol *v, const ext *x, dd__fs_map *m, uint8_t *desc, uint8_t *data,
                            dd__fs_map *note)
{
    journal j = { NULL, 0, 0 };
    bitmap_loc *locs = NULL;
    uint32_t maxlen, first, b, seq, steps, incompat = 0, tag, tail, g;
    DWORD e;

    if ((e = open_journal(v, x, &j)) != 0 || (e = read_jblock(v, x, &j, 0, desc)) != 0)
        goto out;
    if (dd__be32(desc) != JBD_MAGIC || (dd__be32(desc + 4) != 3 && dd__be32(desc + 4) != 4) ||
        dd__be32(desc + 12) != x->block) {
        e = ERROR_INVALID_DATA;
        goto out;
    }
    if (dd__be32(desc + 4) == 4)
        incompat = dd__be32(desc + 40);
    maxlen = dd__be32(desc + 16);
    first = dd__be32(desc + 20);
    seq = dd__be32(desc + 24);
    b = dd__be32(desc + 28);
    if (b == 0)
        goto out;                       /* the log is empty: nothing to recover */
    if (incompat & ~JBD_INCOMPAT_KNOWN || incompat & JBD_FAST_COMMIT) {
        dd__fs_note(note, L"its journal uses features dd cannot read");
        e = ERROR_NOT_SUPPORTED;
        goto out;
    }
    if (maxlen > j.len || first == 0 || first >= maxlen || b < first || b >= maxlen) {
        e = ERROR_INVALID_DATA;
        goto out;
    }
    /* journal_tag_bytes(): v3 tags are fixed; otherwise 8, plus the high
       block number with 64bit, plus a 16-bit checksum with csum v2 */
    tag = incompat & JBD_CSUM_V3 ? 16 : (incompat & JBD_64BIT ? 12 : 8) + (incompat & JBD_CSUM_V2 ? 2 : 0);
    tail = incompat & (JBD_CSUM_V2 | JBD_CSUM_V3) ? 4 : 0;

    locs = malloc((size_t)x->groups * sizeof *locs);
    if (!locs) {
        e = ERROR_NOT_ENOUGH_MEMORY;
        goto out;
    }
    for (g = 0; g < x->groups; g++) {
        locs[g].block = block_bitmap(x, g);
        locs[g].group = g;
    }
    qsort(locs, x->groups, sizeof *locs, by_block);

#define NEXT(n) ((n) + 1 >= maxlen ? first : (n) + 1)
    for (steps = 0; steps < maxlen; steps++) {
        uint32_t type;
        if (read_jblock(v, x, &j, b, desc) != 0 || dd__be32(desc) != JBD_MAGIC || dd__be32(desc + 8) != seq)
            break;                      /* the end of the log */
        type = dd__be32(desc + 4);
        if (type == JBD_DESCRIPTOR) {
            uint32_t off = 12, db = NEXT(b);
            while (off + tag <= x->block - tail) {
                const uint8_t *t = desc + off;
                uint32_t flags = incompat & JBD_CSUM_V3 ? dd__be32(t + 4) : dd__be16(t + 6);
                uint64_t home = dd__be32(t);
                bitmap_loc key, *hit;
                if (incompat & JBD_64BIT)
                    home |= (uint64_t)dd__be32(t + 8) << 32;
                mark(x, m, home, 1);
                key.block = home;
                hit = bsearch(&key, locs, x->groups, sizeof *locs, by_block);
                if (hit && read_jblock(v, x, &j, db, data) == 0) {
                    if (flags & JTAG_ESCAPE) {
                        data[0] = 0xC0; data[1] = 0x3B; data[2] = 0x39; data[3] = 0x98;
                    }
                    or_bitmap(x, m, hit->group, data);
                }
                db = NEXT(db);
                off += tag + (flags & JTAG_SAME_UUID ? 0 : 16);
                if (flags & JTAG_LAST)
                    break;
            }
            b = db;
        } else if (type == JBD_COMMIT) {
            seq++;
            b = NEXT(b);
        } else if (type == JBD_REVOKE) {
            b = NEXT(b);
        } else {
            break;
        }
    }
#undef NEXT
    e = 0;
out:
    free(locs);
    free(j.map);
    return e;
}

/* ---- map ------------------------------------------------------------------ */

static dd__fs_status ext_map(dd__fs_vol *v, const uint8_t *head, bool allow_unclean, dd__fs_map *m)
{
    uint8_t *buf = NULL, *data = NULL;
    dd__fs_status st = DD__FS_WHOLE;
    bool recover;
    ext x;
    DWORD e;

    if (!parse(head + SB, &x))
        return DD__FS_WHOLE;
    wcscpy_s(m->type, sizeof m->type / sizeof *m->type, type_name(&x));
    if (x.incompat & ~INCOMPAT_KNOWN || x.ro & ~RO_KNOWN) {
        dd__fs_note(m, L"it uses features dd does not know (incompat %X, ro_compat %X)",
                    x.incompat & ~INCOMPAT_KNOWN, x.ro & ~RO_KNOWN);
        return DD__FS_WHOLE;
    }
    recover = (x.incompat & INCOMPAT_RECOVER) && (x.compat & COMPAT_HAS_JOURNAL);
    m->unclean = recover || !(x.state & STATE_VALID) || (x.state & STATE_ERROR);
    if (m->unclean && !allow_unclean) {
        dd__fs_note(m, recover ? L"it was not cleanly unmounted: its journal needs recovery"
                     : (x.state & STATE_ERROR) ? L"it has errors recorded"
                                               : L"it was not cleanly unmounted, says its superblock");
        return DD__FS_WHOLE;
    }
    if (recover && x.journal_inum == 0) {
        dd__fs_note(m, L"its journal, on another device, needs recovery");
        return DD__FS_WHOLE;
    }

    m->size = (int64_t)(x.blocks * x.block);
    m->unit = x.block << x.cluster_bits;
    m->map_start = (int64_t)x.first_data_block * x.block;
    /* larger than its volume: fsmap reads it whole, so allocate nothing */
    if (m->size > dd__fs_vol_size(v))
        return DD__FS_MAPPED;
    buf = malloc(x.block);
    data = malloc(x.block);
    if (!buf || !data || !dd__fs_alloc_bits(m, ((x.blocks - x.first_data_block) + ((uint64_t)1 << x.cluster_bits) - 1)
                                                  >> x.cluster_bits)) {
        dd__fs_note(m, L"there is not enough memory to map it");
        goto out;
    }
    e = read_groups(v, &x, m, buf);
    if (e) {
        dd__fs_note(m, e == ERROR_INVALID_DATA ? L"its group descriptors are damaged"
                     : e == ERROR_CRC          ? L"a group descriptor fails its checksum"
                     : e == ERROR_HANDLE_EOF   ? L"it runs past the end of its partition"
                                               : L"its block bitmaps cannot be read");
        goto out;
    }
    if (recover) {
        e = replay_bitmaps(v, &x, m, buf, data, m);
        if (e) {
            dd__fs_note(m, L"its journal, which needs recovery, cannot be read");
            goto out;
        }
        dd__fs_note(m, L"its journal needs recovery: blocks the journal uses count as in use");
    } else if (m->unclean) {
        dd__fs_note(m, (x.compat & COMPAT_HAS_JOURNAL)
                       ? L"it was not cleanly unmounted (or has errors): blocks written since may not be in its bitmaps"
                       : L"it was not cleanly unmounted and has no journal: blocks written since may not be in its bitmaps");
    }
    st = DD__FS_MAPPED;
out:
    free(x.gd);
    free(buf);
    free(data);
    return st;
}

const dd__fs_driver dd__fs_ext = { ext_probe, ext_map };
