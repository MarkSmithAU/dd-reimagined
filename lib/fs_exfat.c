/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later

    exFAT for sparse reading.  The allocation bitmap, a file found through
    the root directory, says which clusters are in use; a volume with two
    FATs (TexFAT) has two bitmaps, and a cluster used in either counts.
    The boot region, the FATs and anything before the cluster heap are
    always read.

    Not cleanly unmounted means VolumeDirty is set in the boot sector (or a
    volume Windows has mounted, and dd has locked, was dirty when Windows
    mounted it).  With --allow-unclean, the directory tree is walked as
    well, and every cluster a file or directory names counts as in use, in
    the bitmap or not.  (The FAT cannot be used for this: its entries for
    free clusters are undefined, and Windows leaves old data in them.)
    Clusters written but not yet named in a directory may still be lost.
*/
#include "fs.h"
#include <stdlib.h>
#include <string.h>

#define CHUNK       (1u << 20)
#define MAX_ROOT    (64u << 20)     /* root directory bytes searched for the bitmap */
#define MAX_ENTRIES (1u << 26)      /* directory entries walked, --allow-unclean */
#define MAX_DEPTH   64

typedef struct exfat {
    uint32_t bps_shift, spc_shift, nfats;
    uint64_t volume_sectors;
    uint32_t fat_offset, fat_length, heap_offset, clusters, root;
    uint16_t flags;
    int64_t  cluster;               /* bytes */
} exfat;

static bool parse(const uint8_t *b, exfat *x)
{
    size_t i;

    if (memcmp(b + 3, "EXFAT   ", 8) != 0)
        return false;
    for (i = 11; i < 64; i++)       /* where FAT keeps its BPB, exFAT has zeros */
        if (b[i])
            return false;
    x->volume_sectors = dd__le64(b + 72);
    x->fat_offset = dd__le32(b + 80);
    x->fat_length = dd__le32(b + 84);
    x->heap_offset = dd__le32(b + 88);
    x->clusters = dd__le32(b + 92);
    x->root = dd__le32(b + 96);
    x->flags = dd__le16(b + 106);
    x->bps_shift = b[108];
    x->spc_shift = b[109];
    x->nfats = b[110];
    if (x->bps_shift < 9 || x->bps_shift > 12 || x->spc_shift > 25 - x->bps_shift ||
        (x->nfats != 1 && x->nfats != 2) || x->fat_offset < 24 || x->fat_length == 0 ||
        x->heap_offset < x->fat_offset + (uint64_t)x->fat_length * x->nfats ||
        x->heap_offset >= x->volume_sectors || x->clusters == 0 || x->clusters > 0xFFFFFFF5u ||
        x->clusters > (x->volume_sectors - x->heap_offset) >> x->spc_shift ||
        ((uint64_t)x->fat_length << x->bps_shift) < ((uint64_t)x->clusters + 2) * 4 ||
        x->root < 2 || x->root > x->clusters + 1)
        return false;
    x->cluster = (int64_t)1 << (x->bps_shift + x->spc_shift);
    return true;
}

static bool exfat_probe(const uint8_t *head, int64_t vol_size)
{
    exfat x;
    (void)vol_size;
    return parse(head, &x);
}

static int64_t fat_base(const exfat *x, uint32_t copy)
{
    return ((int64_t)x->fat_offset + (int64_t)copy * x->fat_length) << x->bps_shift;
}

static int64_t cluster_off(const exfat *x, uint32_t c)
{
    return ((int64_t)x->heap_offset << x->bps_shift) + (int64_t)(c - 2) * x->cluster;
}

/* The cluster after c in a FAT chain; 0 at the end of the chain or if it
   is broken.  *broken (if given) is set when it ends other than with the
   end-of-chain mark: unreadable, or pointing outside the volume. */
static uint32_t next_cluster(dd__fs_vol *v, const exfat *x, uint32_t copy, uint32_t c, bool *broken)
{
    uint8_t e[4];
    uint32_t n;

    if (dd__fs_read(v, fat_base(x, copy) + 4 * (int64_t)c, e, 4) != 0) {
        if (broken)
            *broken = true;
        return 0;
    }
    n = dd__le32(e);
    if (n >= 2 && n <= x->clusters + 1)
        return n;
    if (broken && n != 0xFFFFFFFFu)
        *broken = true;
    return 0;
}

typedef struct bitmap_entry {
    uint32_t first;
    uint64_t length;
} bitmap_entry;

/* Finds the allocation bitmaps in the root directory. */
static int find_bitmaps(dd__fs_vol *v, const exfat *x, uint32_t copy, bitmap_entry bm[2])
{
    uint8_t d[32];
    uint32_t c = x->root, hops = 0;
    int found = 0;
    int64_t i, searched = 0;

    while (c && hops++ <= x->clusters && searched < MAX_ROOT) {
        for (i = 0; i < x->cluster; i += 32, searched += 32) {
            if (dd__fs_read(v, cluster_off(x, c) + i, d, 32) != 0 || d[0] == 0x00)
                return found;           /* 0x00: the end of the directory */
            if (d[0] == 0x81) {
                int which = x->nfats == 2 ? d[1] & 1 : 0;
                if (bm[which].first == 0) {
                    bm[which].first = dd__le32(d + 20);
                    bm[which].length = dd__le64(d + 24);
                    found++;
                }
                if (found == (int)x->nfats)
                    return found;
            }
        }
        c = next_cluster(v, x, copy, c, NULL);
    }
    return found;
}

/* ORs one allocation bitmap, read along its FAT chain, into the map. */
static bool read_bitmap(dd__fs_vol *v, const exfat *x, uint32_t copy, const bitmap_entry *b,
                        uint8_t *buf, dd__fs_map *m)
{
    uint64_t want = ((uint64_t)x->clusters + 7) / 8, done = 0, k;
    uint32_t c = b->first, hops = 0;

    if (c < 2 || c > x->clusters + 1 || b->length < want)
        return false;
    while (done < want) {
        int64_t in_cluster;
        if (!c || hops++ > x->clusters)
            return false;
        for (in_cluster = 0; in_cluster < x->cluster && done < want; ) {
            uint64_t n = want - done;
            if (n > CHUNK)
                n = CHUNK;
            if (n > (uint64_t)(x->cluster - in_cluster))
                n = (uint64_t)(x->cluster - in_cluster);
            if (dd__fs_read(v, cluster_off(x, c) + in_cluster, buf, (size_t)n) != 0)
                return false;
            for (k = 0; k < n; k++)
                m->bits[done + k] |= buf[k];
            done += n;
            in_cluster += (int64_t)n;
        }
        if (done < want)
            c = next_cluster(v, x, copy, c, NULL);
    }
    return true;
}

/* ---- --allow-unclean: the directory tree --------------------------------- */

/* The clusters of a file or directory, in order: contiguous (NoFatChain)
   or along the FAT. */
typedef struct stream {
    uint32_t cluster;           /* the current one; 0 when there are no more */
    uint64_t left;              /* clusters from the current one; UINT64_MAX:
                                   until the FAT chain ends (the root) */
    uint64_t hops;
    bool     contiguous;
} stream;

typedef struct walk {
    dd__fs_vol  *v;
    const exfat *x;
    uint32_t     copy;          /* the active FAT */
    dd__fs_map  *m;
    uint64_t     entries;       /* left to read */
    uint64_t     clusters;      /* left to follow, in all files: more than the
                                   volume holds means chains are cross-linked */
    bool         failed;        /* a chain could not be followed */
} walk;

static void stream_start(stream *s, const exfat *x, uint32_t first, bool contiguous, uint64_t bytes, bool root)
{
    s->contiguous = contiguous;
    s->hops = 0;
    s->left = root ? UINT64_MAX : (bytes + (uint64_t)x->cluster - 1) / (uint64_t)x->cluster;
    s->cluster = first >= 2 && first <= x->clusters + 1 && s->left ? first : 0;
}

static void stream_next(stream *s, walk *w)
{
    const exfat *x = w->x;
    bool broken = false;

    if (s->left != UINT64_MAX && --s->left == 0) {
        s->cluster = 0;
    } else if (++s->hops > x->clusters || w->clusters-- == 0) {
        s->cluster = 0;
        w->failed = true;
    } else if (s->contiguous) {
        s->cluster = s->cluster <= x->clusters ? s->cluster + 1 : 0;
        if (!s->cluster)
            w->failed = true;           /* it runs past the end of the volume */
    } else {
        s->cluster = next_cluster(w->v, x, w->copy, s->cluster, &broken);
        /* a file whose chain ends early: where the rest is, is unknown */
        if (broken || (!s->cluster && s->left != UINT64_MAX))
            w->failed = true;
    }
}

static void mark_stream(walk *w, stream *s)
{
    for (; s->cluster; stream_next(s, w))
        dd__fs_set_bits(w->m, s->cluster - 2, 1);
}

/* Marks a directory's clusters, and those of everything in it.  False if
   it cannot be read, or is too big or too deep to be believed. */
static bool walk_dir(walk *w, uint32_t first, bool contiguous, uint64_t bytes, bool root, int depth)
{
    const exfat *x = w->x;
    uint8_t d[32];
    stream s;
    bool file = false, dir = false;
    int64_t i;

    if (depth > MAX_DEPTH)
        return false;
    stream_start(&s, x, first, contiguous, bytes, root);
    for (; s.cluster && !w->failed; stream_next(&s, w)) {
        dd__fs_set_bits(w->m, s.cluster - 2, 1);
        for (i = 0; i < x->cluster; i += 32) {
            if (w->entries-- == 0 || dd__fs_read(w->v, cluster_off(x, s.cluster) + i, d, 32) != 0)
                return false;
            if (d[0] == 0x00)
                return true;                    /* the end of the directory */
            if (d[0] == 0x85) {                 /* a file: its stream extension follows */
                file = true;
                dir = (dd__le16(d + 4) & 0x10) != 0;
            } else if (d[0] == 0xC0 && file) {
                uint32_t c = dd__le32(d + 20);
                uint64_t len = dd__le64(d + 24);
                bool nofat = (d[1] & 2) != 0;
                file = false;
                if (!(d[1] & 1) || c == 0)
                    continue;                   /* nothing allocated */
                if (dir) {
                    if (!walk_dir(w, c, nofat, len, false, depth + 1))
                        return false;
                } else {
                    stream f;
                    stream_start(&f, x, c, nofat, len, false);
                    mark_stream(w, &f);
                }
            } else if (d[0] == 0x81 || d[0] == 0x82) {  /* the bitmap and the up-case table */
                stream f;
                stream_start(&f, x, dd__le32(d + 20), false, dd__le64(d + 24), false);
                mark_stream(w, &f);
            } else if (d[0] >= 0x80 && d[0] < 0xC0) {
                file = false;                   /* another primary entry */
            }
        }
    }
    return !w->failed;
}

static dd__fs_status exfat_map(dd__fs_vol *v, const uint8_t *head, bool allow_unclean, dd__fs_map *m)
{
    bitmap_entry bm[2];
    uint8_t *buf;
    uint32_t active, i;
    exfat x;
    bool ok = true;
    int found;

    if (!parse(head, &x))
        return DD__FS_WHOLE;
    wcscpy_s(m->type, sizeof m->type / sizeof *m->type, L"exFAT");
    m->unclean = (x.flags & 2) != 0 && !dd__fs_vol_os_clean(v);
    if (m->unclean && !allow_unclean) {
        dd__fs_note(m, L"it was not cleanly unmounted, says its boot sector");
        return DD__FS_WHOLE;
    }

    active = x.nfats == 2 && (x.flags & 1) ? 1 : 0;
    memset(bm, 0, sizeof bm);
    found = find_bitmaps(v, &x, active, bm);
    if (found == 0) {
        dd__fs_note(m, L"its allocation bitmap cannot be found");
        return DD__FS_WHOLE;
    }

    m->size = (int64_t)x.volume_sectors << x.bps_shift;
    m->unit = (uint32_t)x.cluster;
    m->map_start = (int64_t)x.heap_offset << x.bps_shift;
    /* larger than its volume: fsmap reads it whole, so allocate nothing */
    if (m->size > dd__fs_vol_size(v))
        return DD__FS_MAPPED;
    buf = malloc(CHUNK);
    if (!buf || !dd__fs_alloc_bits(m, x.clusters)) {
        free(buf);
        dd__fs_note(m, L"there is not enough memory to map it");
        return DD__FS_WHOLE;
    }
    for (i = 0; i < 2 && ok; i++)
        if (bm[i].first)
            ok = read_bitmap(v, &x, i, &bm[i], buf, m);    /* bitmap i goes with FAT i */
    free(buf);
    if (!ok) {
        dd__fs_note(m, L"its allocation bitmap is damaged");
        return DD__FS_WHOLE;
    }
    if (m->unclean) {
        walk w = { v, &x, active, m, MAX_ENTRIES, (uint64_t)x.clusters + 1, false };
        if (!walk_dir(&w, x.root, false, 0, true, 0)) {
            dd__fs_note(m, L"it was not cleanly unmounted, and its directories cannot all be read");
            return DD__FS_WHOLE;
        }
        dd__fs_note(m, L"it was not cleanly unmounted: what its directories name counts as in use, but clusters not yet in a directory may be lost");
    }
    return DD__FS_MAPPED;
}

const dd__fs_driver dd__fs_exfat = { exfat_probe, exfat_map };
