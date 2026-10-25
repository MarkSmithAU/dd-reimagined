/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later

    File system drivers for sparse reading (--sparse-read).  A driver
    recognises one family of file systems and says which parts of a volume
    hold data.  fsmap.c finds the volumes of the input (its partitions, or
    the whole input), offers each one to the drivers in turn, and answers
    the copy's question: is this byte in use?

    Anything a driver does not claim is read: partition tables, gaps between
    partitions, partitions no driver recognises, and the end of a partition
    past its file system.  A driver that is unsure (a damaged or unsupported
    file system) returns DD__FS_WHOLE, and its volume is read whole.

    Adding a file system:
      1. write fs_<name>.c with a dd__fs_driver (probe and map, below);
      2. add it to drivers[] in fsmap.c, before any driver that could
         mistake it for its own;
      3. add the file to build.cmd and CMakeLists.txt.
*/
#ifndef DD_FS_H
#define DD_FS_H

#include "dd_internal.h"

/* A volume as a driver sees it: offsets are from its first byte. */
typedef struct dd__fs_vol dd__fs_vol;

int64_t dd__fs_vol_size(const dd__fs_vol *v);
/* True when Windows has this volume mounted, dd holds its lock (so Windows
   has written out everything it had cached), and Windows says the volume
   was clean when it mounted it.  Windows marks a FAT volume dirty on disk
   while it has changes pending, so with this a FAT or exFAT driver can
   ignore that mark.  Not for a file system whose on-disk state needs
   recovery whatever Windows did (a journal). */
bool    dd__fs_vol_os_clean(const dd__fs_vol *v);
/* Reads exactly len bytes at off.  ERROR_HANDLE_EOF if they are not all
   inside the volume; reads are cached, so small ones are cheap. */
DWORD   dd__fs_read(dd__fs_vol *v, int64_t off, void *buf, size_t len);

/* What a driver found.  The file system covers [0, size) of the volume.
   Inside it, bit i says whether [map_start + i * unit, + unit) is in use;
   the bytes before map_start and after the last bit are always read
   (boot sectors, FATs, and so on). */
typedef struct dd__fs_map {
    wchar_t  type[16];        /* "FAT32", "exFAT", "ext4", ... */
    int64_t  size;
    int64_t  map_start;
    uint32_t unit;            /* bytes per bit: a cluster or block */
    uint64_t nbits;
    uint8_t *bits;            /* bit i is bits[i / 8] & (1 << i % 8); 1 = in use */
    bool     unclean;         /* not cleanly unmounted */
    bool     unclean_whole;   /* and read whole even with --allow-unclean:
                                 nothing safe can be made of it */
    wchar_t  note[200];       /* why it is read whole, or a caution */
} dd__fs_map;

typedef enum dd__fs_status {
    DD__FS_MAPPED,            /* the map says what is in use */
    DD__FS_WHOLE              /* recognised, but read whole; note says why */
} dd__fs_status;

/* The start of a volume handed to probe and map: a FAT or exFAT boot
   sector at 0, an ext superblock at 1024.  Zeros past a short volume. */
#define DD__FS_HEAD 4096

typedef struct dd__fs_driver {
    /* True if head looks like this driver's file system.  No I/O, and no
       claims on weak evidence: the first driver that says yes maps it. */
    bool (*probe)(const uint8_t *head, int64_t vol_size);
    /* Fills m: type always, the map when it returns DD__FS_MAPPED.  It sets
       m->unclean for a file system that was not cleanly unmounted, and then,
       unless allow_unclean, returns DD__FS_WHOLE without mapping it.  With
       allow_unclean it maps conservatively: everything that might be in use
       counts as in use. */
    dd__fs_status (*map)(dd__fs_vol *v, const uint8_t *head, bool allow_unclean, dd__fs_map *m);
} dd__fs_driver;

extern const dd__fs_driver dd__fs_fat, dd__fs_exfat, dd__fs_ntfs, dd__fs_ext, dd__fs_swap;

/* For drivers */
bool dd__fs_alloc_bits(dd__fs_map *m, uint64_t nbits);         /* all clear */
void dd__fs_set_bits(dd__fs_map *m, uint64_t first, uint64_t count);
void dd__fs_note(dd__fs_map *m, const wchar_t *fmt, ...);      /* keeps the first */

static __inline uint16_t dd__le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static __inline uint32_t dd__le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static __inline uint64_t dd__le64(const uint8_t *p) { return (uint64_t)dd__le32(p) | (uint64_t)dd__le32(p + 4) << 32; }
static __inline uint32_t dd__be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}
static __inline uint16_t dd__be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* ---- the map of a whole input (fsmap.c) -------------------------------- */

typedef struct dd__fsmap dd__fsmap;

/* What is mapped: an open input, read through its own handle (so a locked
   input can be mapped), or anything read through a reader (a compressed
   image, read out of order). */
typedef struct dd__fs_source {
    HANDLE             h;          /* read with ReadFile at offsets, or */
    dd__read_at_fn     rd;         /* through this, if set */
    void              *rd_ctx;
    int64_t            size;
    bool               device;     /* h: reads must stop at the device's end */
    bool               is_volume;  /* one volume, no partition table to read */
    const dd_endpoint *locked;     /* an input whose volumes dd has locked:
                                      Windows says which mounted clean */
} dd__fs_source;

/* Reads the source's partition table and file systems.  Fills rep, which
   the caller reports.  NULL with *err set if memory runs out; a file
   system that cannot be read is not an error: it is read whole. */
dd__fsmap *dd__fsmap_build(const dd__fs_source *src, bool allow_unclean, dd_fs_report *rep, DWORD *err);

/* A stored map (an image's .sparse file): what it was made for. */
typedef struct dd__map_id {
    uint8_t sha256[32];             /* of the image file as stored (the .xz) */
    int64_t file_size;              /* the .xz file */
    int64_t image_size;             /* uncompressed */
    bool    unclean;                /* made with --allow-unclean */
} dd__map_id;

/* Reads a .sparse file.  NULL with *err: ERROR_FILE_NOT_FOUND or
   ERROR_PATH_NOT_FOUND if there is none, ERROR_INVALID_DATA if it is
   damaged.  rep may be NULL. */
dd__fsmap *dd__fsmap_load(const wchar_t *path, dd__map_id *id, dd_fs_report *rep, DWORD *err);
/* Whether the byte at pos is in use, and how many bytes from pos (at
   most max) have the same answer. */
int64_t    dd__fsmap_run(const dd__fsmap *m, int64_t pos, int64_t max, bool *used);
void       dd__fsmap_free(dd__fsmap *m);

#endif
