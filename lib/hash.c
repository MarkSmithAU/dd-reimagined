/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later

    Hashes come from Windows CNG (bcrypt).  A .hashes file sits next to its
    image in sha256sum's format, one "<hex>  <name>" line per file, so
    `sha256sum -c --ignore-missing` can check it too.  An image and its .xz
    share one file: disk.img and disk.img.xz both use disk.img.hashes, which
    holds the hash of the uncompressed image and, if it was compressed, of
    the .xz file.
*/
#include "dd_internal.h"
#include <bcrypt.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define HASHES_MAX_FILE (64 * 1024)

struct dd__sha256 {
    BCRYPT_ALG_HANDLE  alg;
    BCRYPT_HASH_HANDLE h;
    int64_t            bytes;
    bool               failed;
};

dd__sha256 *dd__sha256_new(void)
{
    dd__sha256 *s = calloc(1, sizeof *s);

    if (!s)
        return NULL;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&s->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0))) {
        free(s);
        return NULL;
    }
    if (!BCRYPT_SUCCESS(BCryptCreateHash(s->alg, &s->h, NULL, 0, NULL, 0, 0))) {
        BCryptCloseAlgorithmProvider(s->alg, 0);
        free(s);
        return NULL;
    }
    return s;
}

void dd__sha256_update(dd__sha256 *s, const void *p, DWORD n)
{
    if (!s || n == 0)
        return;
    if (!BCRYPT_SUCCESS(BCryptHashData(s->h, (PUCHAR)p, n, 0)))
        s->failed = true;
    s->bytes += n;
}

int64_t dd__sha256_bytes(const dd__sha256 *s)
{
    return s ? s->bytes : 0;
}

bool dd__sha256_final(dd__sha256 *s, uint8_t out[32])
{
    return !s->failed && BCRYPT_SUCCESS(BCryptFinishHash(s->h, out, 32, 0));
}

void dd__sha256_free(dd__sha256 *s)
{
    if (!s)
        return;
    BCryptDestroyHash(s->h);
    BCryptCloseAlgorithmProvider(s->alg, 0);
    free(s);
}

void dd__hex(const uint8_t d[32], wchar_t out[65])
{
    static const wchar_t digits[] = L"0123456789abcdef";
    int i;

    for (i = 0; i < 32; i++) {
        out[2 * i] = digits[d[i] >> 4];
        out[2 * i + 1] = digits[d[i] & 15];
    }
    out[64] = 0;
}

/* ---- names ------------------------------------------------------------- */

static bool has_xz_suffix(const wchar_t *s)
{
    size_t n = wcslen(s);
    return n > 3 && _wcsicmp(s + n - 3, L".xz") == 0;
}

const wchar_t *dd__base_name(const wchar_t *path)
{
    const wchar_t *b = path, *p;

    for (p = path; *p; p++)
        if (*p == L'\\' || *p == L'/' || *p == L':')
            b = p + 1;
    return b;
}

/* disk.img and disk.img.xz both give disk.img<suffix> */
static bool side_file_name(const wchar_t *image, const wchar_t *suffix, wchar_t *out, size_t n)
{
    size_t len;

    if (!image || !image[0] || wcscmp(image, L"-") == 0 || _wcsnicmp(image, L"/dev/", 5) == 0 ||
        dd__is_device_path(image))
        return false;
    if (wcscpy_s(out, n, image) != 0)
        return false;
    len = wcslen(out);
    if (has_xz_suffix(out))
        out[len - 3] = 0;
    return wcscat_s(out, n, suffix) == 0;
}

bool dd_hash_file_name(const wchar_t *image, wchar_t *out, size_t n)
{
    return side_file_name(image, L".hashes", out, n);
}

bool dd_sparse_map_name(const wchar_t *image, wchar_t *out, size_t n)
{
    return side_file_name(image, L".sparse", out, n);
}

/* The name the uncompressed image goes by: disk.img for disk.img.xz.  A
   compressed file without the .xz suffix has no such name, so one is made. */
void dd__uncompressed_name(const wchar_t *name, wchar_t *out, size_t n)
{
    wcsncpy_s(out, n, dd__base_name(name), _TRUNCATE);
    if (has_xz_suffix(out))
        out[wcslen(out) - 3] = 0;
    else
        wcsncat_s(out, n, L".uncompressed", _TRUNCATE);
}

/* ---- .hashes files ------------------------------------------------------ */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* "<64 hex digits> <space or *><name>", as sha256sum writes and reads it */
static bool parse_line(const char *s, size_t len, dd__hash_entry *e)
{
    int i, hi, lo, wl;

    if (len < 67 || (s[64] != ' ') || (s[65] != ' ' && s[65] != '*'))
        return false;
    for (i = 0; i < 32; i++) {
        hi = hex_value(s[2 * i]);
        lo = hex_value(s[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        e->digest[i] = (uint8_t)(hi << 4 | lo);
    }
    wl = MultiByteToWideChar(CP_UTF8, 0, s + 66, (int)(len - 66), e->name,
                             (int)(sizeof e->name / sizeof *e->name) - 1);
    if (wl <= 0)
        return false;
    e->name[wl] = 0;
    return true;
}

DWORD dd__hashes_read(const wchar_t *path, dd__hash_list *l)
{
    HANDLE h;
    LARGE_INTEGER size;
    char *buf, *p, *end;
    DWORD got = 0, e = 0;

    l->count = 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!GetFileSizeEx(h, &size)) {
        e = GetLastError();
        CloseHandle(h);
        return e;
    }
    if (size.QuadPart > HASHES_MAX_FILE) {
        CloseHandle(h);
        return ERROR_INVALID_DATA;
    }
    buf = malloc((size_t)size.QuadPart + 1);
    if (!buf) {
        CloseHandle(h);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    if (!ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL))
        e = GetLastError();
    CloseHandle(h);
    if (e) {
        free(buf);
        return e;
    }

    p = buf;
    end = buf + got;
    if (got >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF)
        p += 3;
    while (p < end && l->count < DD_MAX_HASHES) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *eol = nl ? nl : end;
        size_t len = (size_t)(eol - p);

        if (len > 0 && p[len - 1] == '\r')
            len--;
        if (parse_line(p, len, &l->e[l->count]))
            l->count++;
        p = nl ? nl + 1 : end;
    }
    free(buf);
    return l->count ? 0 : ERROR_INVALID_DATA;
}

const uint8_t *dd__hashes_find(const dd__hash_list *l, const wchar_t *name)
{
    size_t i;

    for (i = 0; i < l->count; i++)
        if (_wcsicmp(dd__base_name(l->e[i].name), dd__base_name(name)) == 0)
            return l->e[i].digest;
    return NULL;
}

DWORD dd__hashes_write(const wchar_t *path, const dd__hash_entry *entries, size_t n)
{
    char line[64 + 2 + 4 * MAX_PATH + 2];
    wchar_t hex[65];
    HANDLE h;
    DWORD e = 0, put;
    size_t i;

    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError();
    for (i = 0; i < n && !e; i++) {
        int k, len;
        dd__hex(entries[i].digest, hex);
        for (k = 0; k < 64; k++)
            line[k] = (char)hex[k];
        line[64] = ' ';
        line[65] = ' ';
        len = WideCharToMultiByte(CP_UTF8, 0, entries[i].name, -1, line + 66,
                                  (int)sizeof line - 66 - 1, NULL, NULL);
        if (len <= 0) {
            e = GetLastError();
            break;
        }
        len = 66 + len - 1;         /* without the NUL */
        line[len++] = '\n';
        if (!WriteFile(h, line, (DWORD)len, &put, NULL))
            e = GetLastError();
    }
    if (!e && !FlushFileBuffers(h))
        e = GetLastError();
    CloseHandle(h);
    if (e)
        DeleteFileW(path);
    return e;
}
