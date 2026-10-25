/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later

    The engine never writes to the console.  Everything it has to say goes
    through dd_callbacks, so the command line tool and a GUI can share it.
    dd_run() is synchronous: a GUI calls it on a worker thread and stops it
    from the UI thread with dd_cancel_request().
*/
#ifndef DD_H
#define DD_H

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "dd_version.h"

#define DD_WIDEN_(s) L##s
#define DD_WIDEN(s) DD_WIDEN_(s)
#define DD_VERSION DD_WIDEN(DD_VERSION_STR)

/* Largest ibs/obs/bs accepted (a single ReadFile/WriteFile must fit a DWORD). */
#define DD_MAX_BLOCK 0x7FFFF000LL

/* conv= symbols */
#define DD_CONV_NOTRUNC   0x0001u
#define DD_CONV_NOERROR   0x0002u
#define DD_CONV_SYNC      0x0004u
#define DD_CONV_FSYNC     0x0008u
#define DD_CONV_FDATASYNC 0x0010u
#define DD_CONV_EXCL      0x0020u
#define DD_CONV_NOCREAT   0x0040u
#define DD_CONV_SPARSE    0x0080u
#define DD_CONV_SWAB      0x0100u
#define DD_CONV_LCASE     0x0200u
#define DD_CONV_UCASE     0x0400u

/* iflag= / oflag= symbols */
#define DD_FLAG_FULLBLOCK   0x0001u  /* input only */
#define DD_FLAG_COUNT_BYTES 0x0002u  /* input only */
#define DD_FLAG_SKIP_BYTES  0x0004u  /* input only */
#define DD_FLAG_SEEK_BYTES  0x0008u  /* output only */
#define DD_FLAG_APPEND      0x0010u  /* output only */
#define DD_FLAG_DIRECT      0x0020u
#define DD_FLAG_SYNC        0x0040u
#define DD_FLAG_DSYNC       0x0080u

typedef enum dd_status_level {
    DD_STATUS_DEFAULT,
    DD_STATUS_NONE,
    DD_STATUS_NOXFER,
    DD_STATUS_PROGRESS
} dd_status_level;

/* Restricts what the output may be (the old --filter option). */
typedef enum dd_filter {
    DD_FILTER_NONE,
    DD_FILTER_REMOVABLE,   /* whole removable disk (USB, SD, ...) */
    DD_FILTER_FIXED,       /* whole fixed disk */
    DD_FILTER_DISK,        /* any whole disk */
    DD_FILTER_PARTITION,   /* a volume, partition, floppy or optical drive */
    DD_FILTER_FILE         /* a regular file */
} dd_filter;

/* SHA-256 .hashes file next to an image (see dd_hash_file_name). */
typedef enum dd_hashes {
    DD_HASHES_AUTO,        /* write one for an image of a disk or volume;
                              check an input image that has one */
    DD_HASHES_ON,          /* write one for any new output file, too */
    DD_HASHES_OFF          /* neither write nor check */
} dd_hashes;

typedef struct dd_options {
    const wchar_t *input;    /* if=   NULL or "-" means standard input */
    const wchar_t *output;   /* of=   NULL or "-" means standard output */
    int64_t bs;              /* bs=   0 = not given; overrides ibs and obs */
    int64_t ibs;             /* ibs=  0 = default (512) */
    int64_t obs;             /* obs=  0 = default (512) */
    int64_t count;           /* -1 = copy until end of input */
    int64_t skip;            /* input blocks (bytes with DD_FLAG_SKIP_BYTES) */
    int64_t seek;            /* output blocks (bytes with DD_FLAG_SEEK_BYTES) */
    unsigned conv;
    unsigned iflags;
    unsigned oflags;
    dd_status_level status;
    dd_filter filter;        /* restricts what the output may be */
    bool force_dismount;     /* dismount output volumes even while files on
                                them are open; those programs lose them */
    bool lock_input;         /* lock the input volume, or every volume on an
                                input disk, while it is read, so nothing can
                                change it during the copy.  The volumes stay
                                mounted.  Fails while files on them are open. */
    bool keep_letters;       /* leave the drive letters of locked volumes in
                                place; by default they are removed while the
                                volumes are locked, so nothing can open them
                                by letter, and put back afterwards */
    const wchar_t *input_name;   /* names used in messages instead of the  */
    const wchar_t *output_name;  /* paths, e.g. for device interface paths; NULL = path */
    int  xz_level;           /* -1 (the default): output is not compressed;
                                0-9: compress it to .xz at that preset level,
                                multithreaded.  A file or stream only. */
    bool xz_raw;             /* do not decompress an .xz input; by default
                                one is decompressed on the fly */
    int64_t xz_memory;       /* bytes compressing and decompressing may use
                                together; 0 (the default): the memory free
                                when dd starts, less 2 GiB if there is a page
                                file or 4 GiB if not (a quarter of it, if no
                                more than that is free).  The compressor
                                runs fewer threads to fit, and if one thread
                                at xz_level does not fit, a lower level.  An
                                .xz input's decompressor gets a quarter of it
                                when the output is compressed too. */
    bool fit;                /* the input is a disk image: write only up to
                                the end of its last partition, refuse if that
                                does not fit the output, and rebuild a GPT's
                                backup at the end of the output disk.  An
                                image without a partition table is copied
                                whole.  Excludes count/skip/seek. */
    dd_hashes hashes;        /* the .hashes file: the output's is written with
                                the SHA-256 of the image and, if compressed,
                                of the .xz file; the input's is checked as the
                                image is read (needs no skip, and no count
                                unless fit) */
    bool verify;             /* after the copy, read the output back and
                                check it holds what was written */
    bool check_hashes;       /* only read if= (an image file) and check it
                                against its .hashes file; no output */
    bool sparse_read;        /* read only the space the input's file systems
                                use (FAT12/16/32, exFAT, NTFS, ext2/3/4, Linux
                                swap); their free
                                space is written as zeros without being read.
                                Partition tables, gaps and anything not
                                recognised are read as usual.  A compressed
                                if= needs a sparse map (dd_make_sparse_map);
                                it is then read in order, and its free space
                                zeroed as it goes. */
    bool sparse_unclean;     /* sparse_read: also map file systems that were
                                not cleanly unmounted, conservatively (ext3/4:
                                bitmaps from the journal too; FAT: every FAT
                                copy; exFAT: the directory tree).  Without it they
                                are read whole. */
} dd_options;

void dd_options_init(dd_options *o);

/* Parses one GNU operand such as "bs=1M" or "conv=notrunc,fsync".  The
   option keeps pointers into arg for if= and of=, so arg must outlive it.
   On failure returns false with a GNU style message in err. */
bool dd_parse_operand(dd_options *o, const wchar_t *arg, wchar_t *err, size_t errlen);

/* A number with GNU suffixes (c w b kB K KiB MB M ... and NxM products).
   *bytes is set when the text ends in 'B' (count/skip/seek are then bytes). */
bool dd_parse_number(const wchar_t *s, int64_t *out, bool *bytes);

bool dd_parse_filter(const wchar_t *s, dd_filter *out);
const wchar_t *dd_filter_name(dd_filter f);

/* The .hashes file of an image: disk.img and disk.img.xz both give
   disk.img.hashes.  False for devices, standard streams and /dev names. */
bool dd_hash_file_name(const wchar_t *image, wchar_t *out, size_t n);
/* True when two names refer to the same file: compared by file identity
   when both exist (so links, mapped drives and 8.3 names match), else by
   their folder's identity and their name. */
bool dd_same_file(const wchar_t *a, const wchar_t *b);

/* ---- running a copy ---------------------------------------------------- */

typedef enum dd_phase {
    DD_PHASE_COPY,           /* the copy itself */
    DD_PHASE_VERIFY,         /* verify: reading the output back */
    DD_PHASE_CHECK,          /* reading the input to check its hashes */
    DD_PHASE_MAP             /* making a sparse map: decompressing the parts
                                the file systems need, then hashing the file */
} dd_phase;

typedef struct dd_stats {
    bool    started;         /* false if dd_run failed before copying began */
    int64_t r_full, r_partial;
    int64_t w_full, w_partial;
    int64_t bytes_read;
    int64_t bytes_written;
    int64_t bytes_total;     /* expected bytes to write, -1 if unknown */
    int64_t xz_bytes;        /* the .xz file so far: written when compressing,
                                read when decompressing; -1 if there is none */
    int64_t xz_done;         /* compressing: of bytes_written, what the encoder
                                has compressed (it holds a block per thread,
                                so it lags); -1 if not compressing */
    int64_t bytes_unused;    /* sparse_read: of bytes_read, the free space
                                that was zero-filled rather than read */
    double  elapsed;         /* seconds the copy took (so far) */
    /* after the copy, or instead of it with check_hashes: */
    dd_phase phase;
    int64_t phase_done;      /* bytes read back or checked */
    int64_t phase_total;     /* -1 if unknown */
    double  phase_elapsed;   /* seconds since the phase started */
} dd_stats;

typedef enum dd_msg_kind { DD_MSG_ERROR, DD_MSG_WARNING, DD_MSG_INFO } dd_msg_kind;

typedef struct dd_callbacks {
    void *ctx;
    /* text has no "dd: " prefix and no trailing newline */
    void (*message)(void *ctx, dd_msg_kind kind, const wchar_t *text);
    /* called from inside dd_run at most every progress_interval_ms */
    void (*progress)(void *ctx, const dd_stats *stats);
    unsigned progress_interval_ms;   /* 0 = never call progress */
} dd_callbacks;

typedef struct dd_cancel {
    volatile LONG requested;
    SRWLOCK lock;
    HANDLE thread;                   /* set by dd_run while it runs */
} dd_cancel;

void dd_cancel_init(dd_cancel *c);
/* Safe from any thread, including a console control handler.  Aborts a
   blocked read or write as well as stopping between blocks. */
void dd_cancel_request(dd_cancel *c);

typedef enum dd_result {
    DD_OK = 0,
    DD_FAILED = 1,       /* includes read errors skipped with conv=noerror */
    DD_CANCELLED = 2
} dd_result;

/* cb and cancel may be NULL.  stats receives the final counts. */
dd_result dd_run(const dd_options *opt, const dd_callbacks *cb, dd_cancel *cancel, dd_stats *stats);

/* ---- formatting (GNU dd's exact wording) ------------------------------- */

/* "N+M records in\nN+M records out\n" */
void dd_format_records(const dd_stats *s, wchar_t *buf, size_t n);
/* "N bytes (1.1 GB, 1.0 GiB) copied, 2.3 s, 470 MB/s" (no newline).
   progress=true uses whole seconds as GNU does for status=progress. */
void dd_format_transfer(const dd_stats *s, bool progress, wchar_t *buf, size_t n);
/* "compressed: 4104 bytes (4.1 kB, 4.0 KiB)", the .xz side of the transfer;
   empty when nothing is compressed or decompressed */
void dd_format_xz(const dd_stats *s, wchar_t *buf, size_t n);
/* "14.9 GiB" style size */
void dd_format_size(int64_t bytes, bool si, wchar_t *buf, size_t n);
/* "23068672", or "23,068,672" with group (the user's thousands separator) */
void dd_format_bytes(int64_t bytes, bool group, wchar_t *buf, size_t n);
/* "23 MB (23,068,672)": a size for people, with its exact byte count */
void dd_format_size_exact(int64_t bytes, bool si, bool group, wchar_t *buf, size_t n);
#define DD_SIZE_EXACT_CHARS 80
/* Windows error text without the trailing period.  Also knows the
   DD_ERROR_* codes below. */
void dd_errstr(DWORD err, wchar_t *buf, size_t n);

/* Errors of the .xz layer (application-defined Windows error codes) */
#define DD_ERROR_XZ_MEMORY    0x20000101u
#define DD_ERROR_XZ_FORMAT    0x20000102u
#define DD_ERROR_XZ_OPTIONS   0x20000103u
#define DD_ERROR_XZ_CORRUPT   0x20000104u
#define DD_ERROR_XZ_TRUNCATED 0x20000105u
#define DD_ERROR_XZ_INTERNAL  0x20000106u
#define DD_ERROR_XZ_DEVICE    0x20000107u

/* Dictionary size of xz preset level 0-9 (256 KiB to 64 MiB), 0 if invalid */
int64_t dd_xz_dictionary(int level);
/* Level 9 (64 MiB dictionary): the smallest images, chosen over speed.  It
   measured ~7 MB/s on 8 threads, against ~110 MB/s for level 3. */
#define DD_XZ_DEFAULT_LEVEL 9
/* liblzma's version, e.g. "5.8.4" */
const wchar_t *dd_xz_version(void);
/* "USB", "SATA", "NVMe", ... for dd_disk_info.bus_type */
const wchar_t *dd_bus_name(int bus);

/* ---- devices ----------------------------------------------------------- */

typedef enum dd_device_kind { DD_DEV_NONE, DD_DEV_DISK, DD_DEV_CDROM } dd_device_kind;

typedef struct dd_disk_info {
    dd_device_kind kind;
    DWORD   number;          /* N in \\.\PhysicalDriveN or \\.\CdRomN */
    wchar_t path[32];
    /* SetupAPI device interface path.  Unlike path, it stays bound to this
       device: disk numbers are reassigned as devices come and go, so copy
       from or to this path.  Empty if it did not fit. */
    wchar_t interface_path[512];
    wchar_t vendor[64];
    wchar_t product[64];
    int     bus_type;        /* STORAGE_BUS_TYPE, -1 unknown */
    bool    removable;       /* removable media or USB/SD/MMC bus */
    bool    media_present;
    int64_t size;            /* -1 unknown */
    DWORD   sector_size;
    int     partition_style; /* PARTITION_STYLE_*, -1 unknown (needs admin) */
    int     partitions;      /* -1 unknown */
} dd_disk_info;

typedef struct dd_volume_info {
    wchar_t path[64];        /* \\?\Volume{GUID} usable as if=/of= */
    wchar_t *mounts;         /* double NUL terminated list, may be empty */
    wchar_t label[MAX_PATH + 1];
    wchar_t filesystem[MAX_PATH + 1];
    UINT    drive_type;      /* DRIVE_* */
    dd_device_kind parent_kind;  /* DD_DEV_NONE if unknown or spanned */
    DWORD   parent_number;
    int64_t offset;          /* on the parent disk, -1 unknown */
    int64_t size;            /* -1 unknown */
} dd_volume_info;

typedef struct dd_device_list {
    dd_disk_info   *disks;   /* disks then optical drives, by number */
    size_t          ndisks;
    dd_volume_info *volumes;
    size_t          nvolumes;
} dd_device_list;

/* Needs no administrator rights, but partition counts are then unknown. */
bool dd_list_devices(dd_device_list *list);
void dd_free_device_list(dd_device_list *list);
/* True when disk N holds part of the volume Windows runs from, or the
   partition it boots from (the EFI system partition).  dd never writes it. */
bool dd_disk_is_system(DWORD disk);

/* id=/od=: maps "E:" (or a mount point) to the device to copy. */
typedef struct dd_resolved {
    wchar_t path[MAX_PATH];  /* for a disk, its device interface path */
    wchar_t name[MAX_PATH];  /* \\.\PhysicalDriveN etc., for messages */
    int64_t default_bs;
    const wchar_t *what;     /* "whole disk", "optical drive", "floppy disk" */
} dd_resolved;

bool dd_resolve_drive(const wchar_t *spec, dd_resolved *out, wchar_t *err, size_t errlen);

/* ---- disk image layout -------------------------------------------------- */

typedef enum dd_scheme { DD_SCHEME_NONE, DD_SCHEME_MBR, DD_SCHEME_GPT } dd_scheme;

#define DD_MAX_PARTITIONS 256

typedef struct dd_partition {
    int      number;         /* MBR: 1-4 primary, 5+ logical; GPT: entry index + 1 */
    int64_t  start, size;    /* bytes */
    uint8_t  mbr_type;       /* MBR only */
    bool     bootable;       /* MBR active flag */
    bool     extended;       /* MBR extended container (holds the logical ones) */
    bool     logical;
    GUID     type, id;       /* GPT only */
    uint64_t attributes;     /* GPT only */
    wchar_t  name[37];       /* GPT only */
} dd_partition;

typedef struct dd_layout {
    dd_scheme scheme;
    DWORD    sector_size;    /* 512 or 4096 */
    int64_t  image_size;     /* uncompressed */
    bool     compressed;     /* an .xz file; its table is read by decompressing
                                the start, and its backup GPT is not checked */
    int64_t  file_size;      /* the file itself (compressed size) */
    int64_t  data_end;       /* bytes from the start that hold the partitions,
                                partition tables and any ISO 9660 volume */
    int64_t  gpt_reserve;    /* GPT: bytes the backup GPT needs at the end of
                                a disk; 0 otherwise */
    int64_t  iso_size;       /* ISO 9660 volume size, 0 if none */
    DWORD    mbr_signature;
    GUID     disk_guid;      /* GPT */
    bool     gpt_backup_ok;  /* GPT: the image's own backup header is valid */
    size_t   count;
    dd_partition parts[DD_MAX_PARTITIONS];
    wchar_t  note[256];      /* a warning about the table, or empty */
} dd_layout;

/* Reads the MBR or GPT partition table of an image file or a device.  An
   image without a partition table gives DD_SCHEME_NONE and data_end equal
   to the image size.  Returns false only if the image cannot be read. */
bool dd_read_layout(const wchar_t *path, dd_layout *out, wchar_t *err, size_t errlen);

/* Bytes a disk needs to hold the image's data: data_end, plus room for the
   backup GPT. */
int64_t dd_layout_needed(const dd_layout *l);

/* "Linux filesystem", "FAT32 (LBA)", or the type code / GUID */
void dd_partition_type_name(const dd_layout *l, const dd_partition *p, wchar_t *buf, size_t n);

/* ---- file systems: sparse reading -------------------------------------- */

typedef struct dd_fs_volume {
    int      partition;      /* its number in the partition table; 0 = the
                                whole input (a volume, or no partition table) */
    int64_t  start, size;    /* where it is in the input, bytes */
    wchar_t  type[16];       /* "FAT32", "exFAT", "ext4", ...; empty if no
                                file system was recognised */
    bool     sparse;         /* only its used space is read */
    bool     unclean;        /* not cleanly unmounted, or its journal needs
                                recovery */
    int64_t  used;           /* bytes of it that are read */
    wchar_t  note[200];      /* why it is read whole, or a caution; may be empty */
} dd_fs_volume;

typedef struct dd_fs_report {
    int64_t  size;           /* the input */
    int64_t  read;           /* bytes sparse reading reads; the rest of the
                                input is free space, written as zeros */
    size_t   count;
    dd_fs_volume vols[DD_MAX_PARTITIONS];
} dd_fs_report;

/* What --sparse-read would read of an image file or device: its volumes,
   their file systems and used space.  allow_unclean as sparse_unclean.
   A compressed image is answered from its sparse map, which must exist and
   match the image's sizes.  False if the input cannot be opened, has no
   usable sparse map, or memory runs out. */
bool dd_fs_report_read(const wchar_t *path, bool allow_unclean, dd_fs_report *out, wchar_t *err, size_t errlen);

/* A compressed image is read in order, so sparse reading needs its file
   systems mapped beforehand: its sparse map, disk.img.sparse for both
   disk.img and disk.img.xz (as for hash files).  False for devices,
   standard streams and /dev names. */
bool dd_sparse_map_name(const wchar_t *image, wchar_t *out, size_t n);

/* Makes the sparse map of a compressed image: decompresses the parts its
   file systems need (out of order, a block at a time), and hashes the .xz
   file, to which the map is bound.  Reports progress as DD_PHASE_MAP.
   allow_unclean as sparse_unclean; a map made with it is only used with
   it.  rep (may be NULL) receives what the map says. */
dd_result dd_make_sparse_map(const wchar_t *image, bool allow_unclean, const dd_callbacks *cb,
                             dd_cancel *cancel, dd_stats *stats, dd_fs_report *rep);

#ifdef __cplusplus
}
#endif

#endif
