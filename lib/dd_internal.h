/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later
*/
#ifndef DD_INTERNAL_H
#define DD_INTERNAL_H

#include "dd.h"
#include <winioctl.h>

/* What an if= or of= turned out to be once opened. */
typedef enum ep_kind {
    EP_FILE,      /* regular file: seekable, size known */
    EP_BLOCK,     /* disk, volume, optical drive: sector aligned I/O */
    EP_STREAM,    /* pipe, console, character device: not seekable */
    EP_ZERO,      /* /dev/zero */
    EP_RANDOM,    /* /dev/random, /dev/urandom */
    EP_NULL       /* /dev/null: empty input, discarding output */
} ep_kind;

#define DD_MAX_LOCKED 64
#define DD_MAX_LETTERS 26

/* A drive letter taken off a locked volume, to be put back by ep_close. */
typedef struct dd__letter {
    wchar_t letter;
    wchar_t volume[64];      /* \\?\Volume{GUID}\ */
    DWORD   disk;            /* the disk the volume was on */
    BYTE    id[512];         /* its MountedDevices entry, if idlen */
    DWORD   idlen;
} dd__letter;

typedef struct dd_endpoint {
    ep_kind  kind;
    wchar_t *path;           /* path passed to CreateFile, NULL if none */
    wchar_t  name[MAX_PATH + 32]; /* name used in messages */
    HANDLE   h;
    bool     own_handle;     /* false for standard handles */
    bool     device_path;    /* path names a device, not a file */
    DWORD    access, share, flags;
    HANDLE   h_buffered;     /* direct I/O files: handle for unaligned I/O */
    bool     direct;
    int64_t  pos;
    int64_t  size;           /* -1 unknown */
    DWORD    align;          /* required alignment, 0 = none */

    uint8_t *bounce;

    /* EP_BLOCK identity */
    bool     is_volume;
    bool     whole_disk;
    bool     removable;
    bool     cdrom;
    LONG     disk_number;    /* -1 unknown */

    HANDLE   locked[DD_MAX_LOCKED];
    int      nlocked;
    dd__letter letters[DD_MAX_LETTERS];
    int      nletters;
    const dd_callbacks *letters_cb;   /* for ep_close's messages */

    /* .xz: input decompressed or output compressed on the fly.  Such an
       endpoint is a stream (not seekable); size is the uncompressed size. */
    struct dd__xz *xz;
    int64_t  xz_file_size;   /* input: compressed size */
} dd_endpoint;

void  ep_init(dd_endpoint *ep);
/* return 0 or a Windows error code */
DWORD ep_open_input(dd_endpoint *ep, const wchar_t *path, unsigned iflags);
DWORD ep_open_output(dd_endpoint *ep, const wchar_t *path, unsigned oflags, unsigned conv);
DWORD ep_lock_output(dd_endpoint *ep, const dd_callbacks *cb, bool force, bool keep_letters);
DWORD ep_lock_input(dd_endpoint *ep, const dd_callbacks *cb, bool keep_letters);
DWORD ep_read(dd_endpoint *ep, void *buf, DWORD len, DWORD *got);
DWORD ep_write(dd_endpoint *ep, const void *buf, DWORD len, DWORD *put);
DWORD ep_set_eof(dd_endpoint *ep, int64_t length);
DWORD ep_extend_to_pos(dd_endpoint *ep);
DWORD ep_flush(dd_endpoint *ep);
/* .xz: decompress a regular file input that starts with the xz magic */
DWORD ep_detect_xz(dd_endpoint *ep, bool *found, uint64_t memlimit);
/* .xz: compress everything written to a file or stream output */
DWORD ep_start_compress(dd_endpoint *ep, int level, uint64_t memlimit);
DWORD ep_finish_compress(dd_endpoint *ep, int64_t *compressed, void (*tick)(void *), void *ctx);
uint32_t ep_xz_threads(const dd_endpoint *ep);
void  ep_close(dd_endpoint *ep);
bool  ep_seekable(const dd_endpoint *ep);

/* Paths */
bool     dd__is_device_path(const wchar_t *p);
wchar_t *dd__normalize_path(const wchar_t *p);

/* Devices */
bool dd__device_number(HANDLE h, STORAGE_DEVICE_NUMBER *n);
bool dd__storage_info(HANDLE h, int *bus, bool *removable,
                      wchar_t *vendor, size_t vn, wchar_t *product, size_t pn);
bool dd__geometry(HANDLE h, int64_t *size, DWORD *sector, int *media);
int  dd__count_partitions(HANDLE h, int *style);
bool dd__bus_is_removable(int bus);

typedef struct dd__volume {
    const wchar_t *root;      /* \\?\Volume{GUID}\ */
    const wchar_t *dev;       /* \\?\Volume{GUID} */
    const wchar_t *mounts;    /* double NUL terminated */
    const VOLUME_DISK_EXTENTS *ext;  /* NULL if not available */
    HANDLE h;                 /* opened with no access, may be INVALID_HANDLE_VALUE */
} dd__volume;

typedef bool (*dd__volume_fn)(void *ctx, const dd__volume *v);
void dd__for_each_volume(dd__volume_fn fn, void *ctx);

/* GPT rebuilt for an output disk of disk_size bytes (layout.c).  After the
   data is copied, write entries at backup_entries_off, backup at backup_off,
   primary at sector 1 and mbr at sector 0; each is ss bytes except entries. */
typedef struct dd__gpt_fix {
    DWORD    ss;
    uint8_t *mbr, *primary, *backup, *entries;
    DWORD    entries_len;
    int64_t  backup_entries_off, backup_off;
} dd__gpt_fix;

bool dd__gpt_fix_prepare(const wchar_t *image, int64_t disk_size, dd__gpt_fix *f, wchar_t *err, size_t errlen);
void dd__gpt_fix_free(dd__gpt_fix *f);

/* Reads exactly len bytes at off; ERROR_HANDLE_EOF if they are not all there. */
typedef DWORD (*dd__read_at_fn)(void *ctx, int64_t off, void *buf, size_t len);

/* dd_read_layout on an open, uncompressed input of size bytes (layout.c).
   h is a synchronous handle; reads are 4096-byte aligned. */
void dd__read_layout_handle(HANDLE h, int64_t size, dd_layout *l);
/* The same through a reader, e.g. a compressed image read out of order. */
void dd__read_layout_reader(dd__read_at_fn rd, void *ctx, int64_t size, dd_layout *l);

/* .xz streams (xz.c, the only user of liblzma) */
typedef struct dd__xz dd__xz;
bool     dd__xz_magic(const uint8_t *p, size_t n);
DWORD    dd__xz_encoder(dd__xz **x, HANDLE h, int level, uint64_t memlimit);  /* writes h sequentially;
                                                     may lower level to fit memlimit */
DWORD    dd__xz_write(dd__xz *x, const void *buf, DWORD len);
DWORD    dd__xz_finish(dd__xz *x, int64_t *compressed,
                       void (*tick)(void *), void *ctx);   /* tick: while it waits, if not NULL */
DWORD    dd__xz_decoder(dd__xz **x, HANDLE h, uint64_t memlimit);  /* reads h from offset 0; more
                                                     threads while within memlimit */
DWORD    dd__xz_read(dd__xz *x, void *buf, DWORD len, DWORD *got);
DWORD    dd__xz_info(HANDLE h, int64_t file_size, int64_t *uncompressed);
uint32_t dd__xz_threads(const dd__xz *x);
int64_t  dd__xz_compressed_in(const dd__xz *x);   /* encoder: input it has compressed */
int      dd__xz_level(const dd__xz *x);            /* the level an encoder uses */
uint64_t dd__xz_memusage(const dd__xz *x);         /* encoder: its memory; decoder: its limit */
uint64_t dd__xz_memory_available(void);            /* free memory, without paging */
bool     dd__xz_page_file(void);                   /* there is swap */
uint64_t dd__xz_encoder_memusage(int level, uint32_t threads);
int64_t  dd__xz_file_bytes(const dd__xz *x);
void     dd__xz_free(dd__xz *x);

/* An .xz file read out of order (for mapping its file systems): reads
   decompress from the start of the block that holds them. */
typedef struct dd__xz_seek dd__xz_seek;
typedef void (*dd__xz_seek_progress)(void *ctx, uint64_t decoded);
DWORD    dd__xz_seek_open(dd__xz_seek **z, HANDLE h, int64_t file_size);
DWORD    dd__xz_seek_read(dd__xz_seek *z, int64_t off, void *buf, size_t len);   /* exact */
int64_t  dd__xz_seek_size(const dd__xz_seek *z);                                /* uncompressed */
uint64_t dd__xz_seek_decoded(const dd__xz_seek *z);                             /* bytes decompressed so far */
void     dd__xz_seek_set_progress(dd__xz_seek *z, dd__xz_seek_progress fn, void *ctx);
void     dd__xz_seek_free(dd__xz_seek *z);

/* SHA-256 (hash.c) */
typedef struct dd__sha256 dd__sha256;
dd__sha256 *dd__sha256_new(void);
void        dd__sha256_update(dd__sha256 *s, const void *p, DWORD n);
int64_t     dd__sha256_bytes(const dd__sha256 *s);
bool        dd__sha256_final(dd__sha256 *s, uint8_t out[32]);
void        dd__sha256_free(dd__sha256 *s);
void        dd__hex(const uint8_t d[32], wchar_t out[65]);

/* .xz: also hash the compressed bytes as the file is written or read */
void dd__xz_set_hash(dd__xz *x, dd__sha256 *raw);

/* .hashes files (hash.c) */
#define DD_MAX_HASHES 8
typedef struct dd__hash_entry {
    wchar_t name[MAX_PATH];
    uint8_t digest[32];
} dd__hash_entry;

typedef struct dd__hash_list {
    size_t count;
    dd__hash_entry e[DD_MAX_HASHES];
} dd__hash_list;

const wchar_t *dd__base_name(const wchar_t *path);
void           dd__uncompressed_name(const wchar_t *name, wchar_t *out, size_t n);
DWORD          dd__hashes_read(const wchar_t *path, dd__hash_list *l);   /* ERROR_INVALID_DATA: no hashes in it */
const uint8_t *dd__hashes_find(const dd__hash_list *l, const wchar_t *name);
DWORD          dd__hashes_write(const wchar_t *path, const dd__hash_entry *entries, size_t n);

/* Options */
bool dd__validate(const dd_options *o, wchar_t *err, size_t errlen);

/* Messages */
void dd__msg(const dd_callbacks *cb, dd_msg_kind kind, const wchar_t *fmt, ...);

#endif
