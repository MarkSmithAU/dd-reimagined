/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later

    The only file that talks to liblzma (third_party/xz).  Images are
    compressed with the multithreaded LZMA2 encoder, which splits the data
    into blocks that later decompress in parallel too.  The coders keep to
    a memory budget (copy.c: what is free less 2 or 4 GiB, or --xz-memory): the
    encoder by running fewer threads, then a lower level; the decoder by
    running fewer threads.
*/
#include "dd_internal.h"
#include <lzma.h>
#include <stdlib.h>
#include <string.h>

#define XZ_BUF (1u << 20)

struct dd__xz {
    lzma_stream s;
    HANDLE   h;
    bool     encoder;
    uint8_t *buf;            /* encoder: output; decoder: input */
    int64_t  in_pos;         /* decoder: next offset to read in the file */
    bool     eof, done;
    DWORD    error;          /* decoder: once failed, it fails for good */
    int64_t  total_out;      /* encoder: compressed bytes written */
    int64_t  total_in;       /* encoder, once finished: all it was given */
    uint32_t threads;
    int      level;          /* encoder: the preset used */
    uint64_t memusage;       /* encoder: what it needs; decoder: its limit */
    dd__sha256 *raw_hash;    /* hashes the compressed bytes, or NULL */
};

void dd__xz_set_hash(dd__xz *x, dd__sha256 *raw)
{
    x->raw_hash = raw;
}

bool dd__xz_magic(const uint8_t *p, size_t n)
{
    static const uint8_t magic[6] = { 0xFD, '7', 'z', 'X', 'Z', 0x00 };
    return n >= sizeof magic && memcmp(p, magic, sizeof magic) == 0;
}

static DWORD map(lzma_ret r)
{
    switch (r) {
    case LZMA_MEM_ERROR:
    case LZMA_MEMLIMIT_ERROR: return DD_ERROR_XZ_MEMORY;
    case LZMA_FORMAT_ERROR:   return DD_ERROR_XZ_FORMAT;
    case LZMA_OPTIONS_ERROR:  return DD_ERROR_XZ_OPTIONS;
    case LZMA_DATA_ERROR:     return DD_ERROR_XZ_CORRUPT;
    case LZMA_BUF_ERROR:      return DD_ERROR_XZ_TRUNCATED;
    default:                  return DD_ERROR_XZ_INTERNAL;
    }
}

static uint32_t cpu_threads(void)
{
    uint32_t n = lzma_cputhreads();
    return n ? n : 1;
}

static dd__xz *alloc_xz(HANDLE h, bool encoder)
{
    dd__xz *x = calloc(1, sizeof *x);
    lzma_stream init = LZMA_STREAM_INIT;

    if (!x)
        return NULL;
    /* page aligned, so a decoder can read a FILE_FLAG_NO_BUFFERING handle */
    x->buf = VirtualAlloc(NULL, XZ_BUF, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!x->buf) {
        free(x);
        return NULL;
    }
    x->s = init;
    x->h = h;
    x->encoder = encoder;
    return x;
}

void dd__xz_free(dd__xz *x)
{
    if (!x)
        return;
    lzma_end(&x->s);
    VirtualFree(x->buf, 0, MEM_RELEASE);
    free(x);
}

/* ---- memory -------------------------------------------------------------- */

/* What can be allocated now without paging: the free physical memory, or
   the commit that is left, whichever is less. */
uint64_t dd__xz_memory_available(void)
{
    MEMORYSTATUSEX ms;

    ms.dwLength = sizeof ms;
    if (!GlobalMemoryStatusEx(&ms))
        return lzma_physmem() / 2;
    return ms.ullAvailPhys < ms.ullAvailPageFile ? ms.ullAvailPhys : ms.ullAvailPageFile;
}

/* There is swap: the commit limit is more than the physical memory, so
   other programs' sudden needs can be paged out rather than fail. */
bool dd__xz_page_file(void)
{
    MEMORYSTATUSEX ms;

    ms.dwLength = sizeof ms;
    return GlobalMemoryStatusEx(&ms) && ms.ullTotalPageFile > ms.ullTotalPhys;
}

uint64_t dd__xz_encoder_memusage(int level, uint32_t threads)
{
    lzma_mt mt;

    memset(&mt, 0, sizeof mt);
    mt.preset = (uint32_t)level;
    mt.check = LZMA_CHECK_CRC64;
    mt.threads = threads;
    return lzma_stream_encoder_mt_memusage(&mt);
}

/* ---- compressing --------------------------------------------------------- */

/* Each encoder thread needs memory for its block and its match finder:
   1.2 GiB at level 9, 165 MiB at level 6.  The threads are cut until the
   encoder fits limit; if one thread at this level does not fit, the level
   is lowered until it does (level 0 needs 6 MiB).  Fewer threads only cost
   time; a lower level costs size, so it is the last resort. */
DWORD dd__xz_encoder(dd__xz **out, HANDLE h, int level, uint64_t limit)
{
    lzma_mt mt;
    lzma_ret r;
    dd__xz *x;

    *out = NULL;
    if (level < 0 || level > 9)
        return DD_ERROR_XZ_OPTIONS;
    x = alloc_xz(h, true);
    if (!x)
        return ERROR_NOT_ENOUGH_MEMORY;
    memset(&mt, 0, sizeof mt);
    mt.preset = (uint32_t)level;
    mt.check = LZMA_CHECK_CRC64;
    mt.timeout = 250;        /* so finishing can report progress meanwhile */
    for (;;) {
        mt.threads = cpu_threads();
        while (mt.threads > 1 && lzma_stream_encoder_mt_memusage(&mt) > limit)
            mt.threads--;
        if (lzma_stream_encoder_mt_memusage(&mt) <= limit || mt.preset == 0)
            break;
        mt.preset--;
    }
    r = lzma_stream_encoder_mt(&x->s, &mt);
    if (r != LZMA_OK) {
        dd__xz_free(x);
        return map(r);
    }
    x->threads = mt.threads;
    x->level = (int)mt.preset;
    x->memusage = lzma_stream_encoder_mt_memusage(&mt);
    x->s.next_out = x->buf;
    x->s.avail_out = XZ_BUF;
    *out = x;
    return 0;
}

uint32_t dd__xz_threads(const dd__xz *x)
{
    return x ? x->threads : 0;
}

int dd__xz_level(const dd__xz *x)
{
    return x ? x->level : -1;
}

uint64_t dd__xz_memusage(const dd__xz *x)
{
    return x ? x->memusage : 0;
}

/* Of the input, what the encoder's threads have compressed: up to a block
   per thread less than it has taken in. */
int64_t dd__xz_compressed_in(const dd__xz *x)
{
    uint64_t in, out;

    if (!x->encoder)
        return -1;
    if (x->done)
        return x->total_in;
    lzma_get_progress((lzma_stream *)&x->s, &in, &out);
    return (int64_t)in;
}

/* Compressed bytes so far: produced by an encoder (including what waits in
   its buffer), used by a decoder (not counting what it has read ahead). */
int64_t dd__xz_file_bytes(const dd__xz *x)
{
    return x->encoder ? x->total_out + (int64_t)(XZ_BUF - x->s.avail_out)
                      : x->in_pos - (int64_t)x->s.avail_in;
}

static DWORD write_out(dd__xz *x)
{
    DWORD n = XZ_BUF - (DWORD)x->s.avail_out, done = 0, w;

    while (done < n) {
        if (!WriteFile(x->h, x->buf + done, n - done, &w, NULL))
            return GetLastError();
        if (w == 0)
            return ERROR_WRITE_FAULT;
        done += w;
    }
    dd__sha256_update(x->raw_hash, x->buf, n);
    x->total_out += n;
    x->s.next_out = x->buf;
    x->s.avail_out = XZ_BUF;
    return 0;
}

DWORD dd__xz_write(dd__xz *x, const void *buf, DWORD len)
{
    lzma_ret r;
    DWORD e;

    x->s.next_in = buf;
    x->s.avail_in = len;
    while (x->s.avail_in > 0) {
        r = lzma_code(&x->s, LZMA_RUN);
        if (r != LZMA_OK)
            return map(r);
        if (x->s.avail_out == 0 && (e = write_out(x)) != 0)
            return e;
    }
    return 0;
}

DWORD dd__xz_finish(dd__xz *x, int64_t *compressed, void (*tick)(void *), void *ctx)
{
    lzma_ret r;
    DWORD e;

    if (x->done) {
        *compressed = x->total_out;
        return 0;
    }
    x->s.next_in = NULL;
    x->s.avail_in = 0;
    for (;;) {
        r = lzma_code(&x->s, LZMA_FINISH);
        if (r != LZMA_OK && r != LZMA_STREAM_END)
            return map(r);
        if ((x->s.avail_out == 0 || r == LZMA_STREAM_END) && (e = write_out(x)) != 0)
            return e;
        if (r == LZMA_STREAM_END)
            break;
        if (tick)
            tick(ctx);
    }
    x->total_in = (int64_t)x->s.total_in;
    x->done = true;
    /* the threads' memory goes now, not when the output is closed: --verify
       decompresses the file again before that */
    lzma_end(&x->s);
    *compressed = x->total_out;
    return 0;
}

/* ---- decompressing ------------------------------------------------------- */

static bool read_at(HANDLE h, int64_t off, void *buf, DWORD len, DWORD *got)
{
    OVERLAPPED ov;

    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    if (ReadFile(h, buf, len, got, &ov))
        return true;
    if (GetLastError() == ERROR_HANDLE_EOF) {
        *got = 0;
        return true;
    }
    return false;
}

/* Up to limit, blocks are decompressed on several threads; past it, on one
   (which needs about the dictionary: 64 MiB at level 9), but never fails. */
DWORD dd__xz_decoder(dd__xz **out, HANDLE h, uint64_t limit)
{
    lzma_mt mt;
    lzma_ret r;
    dd__xz *x;

    *out = NULL;
    x = alloc_xz(h, false);
    if (!x)
        return ERROR_NOT_ENOUGH_MEMORY;
    memset(&mt, 0, sizeof mt);
    mt.flags = LZMA_CONCATENATED;     /* xz files may be joined end to end */
    mt.threads = cpu_threads();
    mt.memlimit_threading = limit;
    mt.memlimit_stop = UINT64_MAX;
    x->memusage = limit;
    r = lzma_stream_decoder_mt(&x->s, &mt);
    if (r != LZMA_OK) {
        dd__xz_free(x);
        return map(r);
    }
    x->threads = mt.threads;
    *out = x;
    return 0;
}

DWORD dd__xz_read(dd__xz *x, void *buf, DWORD len, DWORD *got)
{
    lzma_ret r;
    DWORD n;

    *got = 0;
    if (x->error)
        return x->error;
    if (x->done || len == 0)
        return 0;
    x->s.next_out = buf;
    x->s.avail_out = len;
    while (x->s.avail_out > 0) {
        if (x->s.avail_in == 0 && !x->eof) {
            if (!read_at(x->h, x->in_pos, x->buf, XZ_BUF, &n)) {
                *got = len - (DWORD)x->s.avail_out;
                return x->error = GetLastError();
            }
            /* a file reads short only at its end; asking again would read at
               an unaligned offset, which an unbuffered handle refuses */
            if (n < XZ_BUF)
                x->eof = true;
            if (n > 0) {
                dd__sha256_update(x->raw_hash, x->buf, n);
                x->s.next_in = x->buf;
                x->s.avail_in = n;
                x->in_pos += n;
            }
        }
        r = lzma_code(&x->s, x->eof ? LZMA_FINISH : LZMA_RUN);
        if (r == LZMA_STREAM_END) {
            x->done = true;
            break;
        }
        if (r != LZMA_OK) {
            *got = len - (DWORD)x->s.avail_out;
            return x->error = map(r);
        }
    }
    *got = len - (DWORD)x->s.avail_out;
    return 0;
}

/* The uncompressed size, from the indexes at the end of each stream; only
   the stream footers and indexes are read. */
DWORD dd__xz_info(HANDLE h, int64_t file_size, int64_t *uncompressed)
{
    lzma_stream s = LZMA_STREAM_INIT;
    lzma_index *idx = NULL;
    uint8_t *buf = malloc(64 * 1024);
    int64_t pos = 0;
    lzma_ret r;
    DWORD n = 0, e = 0;

    *uncompressed = -1;
    if (!buf)
        return ERROR_NOT_ENOUGH_MEMORY;
    r = lzma_file_info_decoder(&s, &idx, UINT64_MAX, (uint64_t)file_size);
    if (r != LZMA_OK) {
        free(buf);
        return map(r);
    }
    for (;;) {
        if (s.avail_in == 0) {
            if (!read_at(h, pos, buf, 64 * 1024, &n)) {
                e = GetLastError();
                break;
            }
            s.next_in = buf;
            s.avail_in = n;
            pos += n;
        }
        r = lzma_code(&s, LZMA_RUN);
        if (r == LZMA_SEEK_NEEDED) {
            pos = (int64_t)s.seek_pos;
            s.avail_in = 0;
            continue;
        }
        if (r == LZMA_STREAM_END) {
            *uncompressed = (int64_t)lzma_index_uncompressed_size(idx);
            break;
        }
        if (r != LZMA_OK || (s.avail_in == 0 && n == 0)) {
            /* the start said .xz but the end has no valid index: almost
               always a download or copy that stopped early */
            e = r == LZMA_DATA_ERROR || r == LZMA_FORMAT_ERROR || r == LZMA_OK ? DD_ERROR_XZ_TRUNCATED : map(r);
            break;
        }
    }
    if (idx)
        lzma_index_end(idx, NULL);
    lzma_end(&s);
    free(buf);
    return e;
}

/* ---- random access ---------------------------------------------------------- */

/* Mapping a compressed image's file systems reads it out of order.  The
   index at the end of an .xz file says where each block starts, so a read
   decompresses from the start of the block that holds it, unless the
   decoder is already in that block before it.  What was read is kept in
   1 MiB chunks, the most recently used CACHE_CHUNKS of them.  dd's own .xz
   files have a block per 192 MiB or less; a file made by xz without
   threads is one block, and a read before the decoder's position then
   starts again from the beginning. */

#define CHUNK        (1u << 20)
#define CACHE_CHUNKS 64

typedef struct chunk {
    int64_t  index;             /* offset / CHUNK; -1 if empty */
    uint64_t used;              /* for LRU */
    DWORD    len;               /* short at the end of the image */
    uint8_t *data;
} chunk;

struct dd__xz_seek {
    HANDLE       h;
    lzma_index  *idx;
    int64_t      size;          /* uncompressed */
    lzma_stream  s;
    lzma_block   block;         /* s keeps a pointer to it while decoding */
    bool         active;        /* s is a block decoder positioned at pos */
    int64_t      pos, block_end;
    int64_t      in_pos;        /* the next compressed byte to read */
    bool         in_eof;
    uint8_t     *in;            /* XZ_BUF of compressed input */
    uint8_t     *scratch;       /* CHUNK, for decompressed bytes passed over */
    chunk        cache[CACHE_CHUNKS];
    uint64_t     clock;
    uint64_t     decoded;       /* bytes decompressed in all */
    dd__xz_seek_progress progress;
    void        *progress_ctx;
};

DWORD dd__xz_seek_open(dd__xz_seek **out, HANDLE h, int64_t file_size)
{
    lzma_stream s = LZMA_STREAM_INIT, init = LZMA_STREAM_INIT;
    dd__xz_seek *z;
    int64_t pos = 0;
    lzma_ret r;
    DWORD n = 0, e = 0;
    int i;

    *out = NULL;
    z = calloc(1, sizeof *z);
    if (!z)
        return ERROR_NOT_ENOUGH_MEMORY;
    z->h = h;
    z->s = init;
    z->in = malloc(XZ_BUF);
    z->scratch = malloc(CHUNK);
    for (i = 0; i < CACHE_CHUNKS; i++)
        z->cache[i].index = -1;
    if (!z->in || !z->scratch) {
        dd__xz_seek_free(z);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    /* the indexes, as dd__xz_info reads them, but kept */
    r = lzma_file_info_decoder(&s, &z->idx, UINT64_MAX, (uint64_t)file_size);
    if (r != LZMA_OK) {
        dd__xz_seek_free(z);
        return map(r);
    }
    for (;;) {
        if (s.avail_in == 0) {
            if (!read_at(h, pos, z->in, XZ_BUF, &n)) {
                e = GetLastError();
                break;
            }
            s.next_in = z->in;
            s.avail_in = n;
            pos += n;
        }
        r = lzma_code(&s, LZMA_RUN);
        if (r == LZMA_SEEK_NEEDED) {
            pos = (int64_t)s.seek_pos;
            s.avail_in = 0;
            continue;
        }
        if (r == LZMA_STREAM_END)
            break;
        if (r != LZMA_OK || (s.avail_in == 0 && n == 0)) {
            e = r == LZMA_DATA_ERROR || r == LZMA_FORMAT_ERROR || r == LZMA_OK ? DD_ERROR_XZ_TRUNCATED : map(r);
            break;
        }
    }
    lzma_end(&s);
    if (e || !z->idx) {
        dd__xz_seek_free(z);
        return e ? e : DD_ERROR_XZ_FORMAT;
    }
    z->size = (int64_t)lzma_index_uncompressed_size(z->idx);
    *out = z;
    return 0;
}

void dd__xz_seek_free(dd__xz_seek *z)
{
    int i;

    if (!z)
        return;
    lzma_end(&z->s);
    if (z->idx)
        lzma_index_end(z->idx, NULL);
    for (i = 0; i < CACHE_CHUNKS; i++)
        free(z->cache[i].data);
    free(z->in);
    free(z->scratch);
    free(z);
}

int64_t dd__xz_seek_size(const dd__xz_seek *z)
{
    return z->size;
}

uint64_t dd__xz_seek_decoded(const dd__xz_seek *z)
{
    return z->decoded;
}

void dd__xz_seek_set_progress(dd__xz_seek *z, dd__xz_seek_progress fn, void *ctx)
{
    z->progress = fn;
    z->progress_ctx = ctx;
}

/* Positions a block decoder at the start of the block holding offset u. */
static DWORD start_block(dd__xz_seek *z, int64_t u)
{
    lzma_filter filters[LZMA_FILTERS_MAX + 1];
    lzma_index_iter it;
    lzma_block *block = &z->block;
    uint8_t hdr[LZMA_BLOCK_HEADER_SIZE_MAX];
    DWORD got, hsize;
    lzma_ret r;

    z->active = false;
    lzma_index_iter_init(&it, z->idx);
    if (lzma_index_iter_locate(&it, (lzma_vli)u))
        return ERROR_HANDLE_EOF;
    if (!read_at(z->h, (int64_t)it.block.compressed_file_offset, hdr, 1, &got))
        return GetLastError();
    if (got != 1 || hdr[0] == 0)
        return DD_ERROR_XZ_CORRUPT;
    hsize = lzma_block_header_size_decode(hdr[0]);
    if (!read_at(z->h, (int64_t)it.block.compressed_file_offset, hdr, hsize, &got))
        return GetLastError();
    if (got != hsize)
        return DD_ERROR_XZ_TRUNCATED;
    memset(block, 0, sizeof *block);
    memset(filters, 0, sizeof filters);
    block->version = 1;
    block->check = it.stream.flags->check;
    block->filters = filters;
    block->header_size = hsize;
    r = lzma_block_header_decode(block, NULL, hdr);
    if (r == LZMA_OK)
        r = lzma_block_compressed_size(block, it.block.unpadded_size);
    /* offsets come from the index, so the block must agree with it */
    if (r == LZMA_OK && block->uncompressed_size == LZMA_VLI_UNKNOWN)
        block->uncompressed_size = it.block.uncompressed_size;
    else if (r == LZMA_OK && block->uncompressed_size != it.block.uncompressed_size)
        r = LZMA_DATA_ERROR;
    /* the same limit as the streaming decoder */
    if (r == LZMA_OK && lzma_raw_decoder_memusage(filters) > dd__xz_memory_available() / 4)
        r = LZMA_MEMLIMIT_ERROR;
    if (r == LZMA_OK)
        r = lzma_block_decoder(&z->s, block);
    lzma_filters_free(filters, NULL);
    block->filters = NULL;      /* the decoder has its own copy */
    if (r != LZMA_OK)
        return map(r);
    z->in_pos = (int64_t)it.block.compressed_file_offset + hsize;
    z->in_eof = false;
    z->s.avail_in = 0;
    z->pos = (int64_t)it.block.uncompressed_file_offset;
    z->block_end = z->pos + (int64_t)it.block.uncompressed_size;
    z->active = true;
    return 0;
}

/* Decompresses the next len bytes, all in the current block, to out. */
static DWORD decode(dd__xz_seek *z, uint8_t *out, size_t len)
{
    lzma_ret r;
    DWORD n;

    z->s.next_out = out;
    z->s.avail_out = len;
    while (z->s.avail_out > 0) {
        if (z->s.avail_in == 0 && !z->in_eof) {
            if (!read_at(z->h, z->in_pos, z->in, XZ_BUF, &n)) {
                z->active = false;      /* part of len may be decoded already */
                return GetLastError();
            }
            if (n == 0)
                z->in_eof = true;
            z->s.next_in = z->in;
            z->s.avail_in = n;
            z->in_pos += n;
        }
        r = lzma_code(&z->s, LZMA_RUN);
        if (r == LZMA_STREAM_END && z->s.avail_out == 0)
            break;
        if (r != LZMA_OK) {
            z->active = false;
            return r == LZMA_STREAM_END ? DD_ERROR_XZ_CORRUPT : map(r);
        }
        if (z->in_eof && z->s.avail_in == 0 && z->s.avail_out > 0) {
            z->active = false;
            return DD_ERROR_XZ_TRUNCATED;
        }
    }
    z->pos += (int64_t)len;
    z->decoded += len;
    if (z->progress)
        z->progress(z->progress_ctx, z->decoded);
    return 0;
}

/* Brings the decoder to offset u, in any block. */
static DWORD seek_to(dd__xz_seek *z, int64_t u)
{
    DWORD e;

    if (!z->active || u < z->pos || u >= z->block_end)
        if ((e = start_block(z, u)) != 0)
            return e;
    while (z->pos < u) {
        int64_t n = u - z->pos < CHUNK ? u - z->pos : CHUNK;
        if ((e = decode(z, z->scratch, (size_t)n)) != 0)
            return e;
    }
    return 0;
}

static chunk *get_chunk(dd__xz_seek *z, int64_t index, DWORD *err)
{
    chunk *c = &z->cache[0];
    int64_t at = index * CHUNK;
    DWORD len, done = 0;
    int i;

    *err = 0;
    for (i = 0; i < CACHE_CHUNKS; i++)
        if (z->cache[i].index == index) {
            z->cache[i].used = ++z->clock;
            return &z->cache[i];
        }
    for (i = 1; i < CACHE_CHUNKS; i++)          /* the least recently used */
        if (z->cache[i].used < c->used)
            c = &z->cache[i];
    if (!c->data && (c->data = malloc(CHUNK)) == NULL) {
        *err = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    c->index = -1;
    len = z->size - at < CHUNK ? (DWORD)(z->size - at) : CHUNK;
    while (done < len) {
        DWORD n;
        if ((*err = seek_to(z, at + done)) != 0)
            return NULL;
        n = z->block_end - z->pos < (int64_t)(len - done) ? (DWORD)(z->block_end - z->pos) : len - done;
        if ((*err = decode(z, c->data + done, n)) != 0)
            return NULL;
        done += n;
    }
    c->index = index;
    c->len = len;
    c->used = ++z->clock;
    return c;
}

DWORD dd__xz_seek_read(dd__xz_seek *z, int64_t off, void *buf, size_t len)
{
    uint8_t *out = buf;
    DWORD e;

    if (off < 0 || off > z->size || len > (uint64_t)(z->size - off))
        return ERROR_HANDLE_EOF;
    while (len > 0) {
        chunk *c = get_chunk(z, off / CHUNK, &e);
        size_t skip, n;
        if (!c)
            return e;
        skip = (size_t)(off % CHUNK);
        n = c->len - skip < len ? c->len - skip : len;
        memcpy(out, c->data + skip, n);
        out += n;
        off += (int64_t)n;
        len -= n;
    }
    return 0;
}

int64_t dd_xz_dictionary(int level)
{
    lzma_options_lzma o;

    if (level < 0 || level > 9 || lzma_lzma_preset(&o, (uint32_t)level))
        return 0;
    return o.dict_size;
}

const wchar_t *dd_xz_version(void)
{
    static wchar_t v[32];
    const char *s = lzma_version_string();
    size_t i;

    for (i = 0; s[i] && i + 1 < sizeof v / sizeof *v; i++)
        v[i] = (wchar_t)s[i];
    v[i] = 0;
    return v;
}
