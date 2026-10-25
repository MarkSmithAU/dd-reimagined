/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later
*/
#include "dd_internal.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

/* Like GNU human_readable() with autoscale, round to nearest and a space
   before the unit: "999 B", "1.1 GB", "1.0 GiB", "470 MB/s". */
static void human(double v, bool si, const wchar_t *unit, wchar_t *buf, size_t n)
{
    static const wchar_t si_prefix[] = L"kMGTPEZYRQ";
    static const wchar_t iec_prefix[] = L"KMGTPEZYRQ";
    const wchar_t *prefix = si ? si_prefix : iec_prefix;
    const wchar_t *infix = si ? L"" : L"i";
    double base = si ? 1000.0 : 1024.0;
    int e = 0;

    while (v >= base && e < 10) {
        v /= base;
        e++;
    }
    if (e == 0) {
        _snwprintf_s(buf, n, _TRUNCATE, L"%.0f %ls", v, unit);
        return;
    }
    if (v < 9.95) {
        _snwprintf_s(buf, n, _TRUNCATE, L"%.1f %lc%ls%ls", v, prefix[e - 1], infix, unit);
        return;
    }
    v = floor(v + 0.5);
    if (v >= base && e < 10) {
        v /= base;
        e++;
        _snwprintf_s(buf, n, _TRUNCATE, L"%.1f %lc%ls%ls", v, prefix[e - 1], infix, unit);
        return;
    }
    _snwprintf_s(buf, n, _TRUNCATE, L"%.0f %lc%ls%ls", v, prefix[e - 1], infix, unit);
}

void dd_format_size(int64_t bytes, bool si, wchar_t *buf, size_t n)
{
    human((double)bytes, si, L"B", buf, n);
}

void dd_format_bytes(int64_t bytes, bool group, wchar_t *buf, size_t n)
{
    wchar_t digits[32], sep[8];
    size_t len, i, o = 0, seplen;

    _snwprintf_s(digits, 32, _TRUNCATE, L"%lld", (long long)(bytes < 0 ? -bytes : bytes));
    if (!group || !GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, sep, 8))
        sep[0] = 0;
    seplen = wcslen(sep);
    len = wcslen(digits);
    if (n == 0)
        return;
    if (bytes < 0 && o + 1 < n)
        buf[o++] = L'-';
    for (i = 0; i < len && o + 1 < n; i++) {
        if (i > 0 && (len - i) % 3 == 0 && seplen && o + seplen + 1 < n) {
            wmemcpy(buf + o, sep, seplen);
            o += seplen;
        }
        buf[o++] = digits[i];
    }
    buf[o] = 0;
}

void dd_format_size_exact(int64_t bytes, bool si, bool group, wchar_t *buf, size_t n)
{
    wchar_t human_size[32], exact[48];

    dd_format_size(bytes, si, human_size, 32);
    dd_format_bytes(bytes, group, exact, 48);
    _snwprintf_s(buf, n, _TRUNCATE, L"%ls (%ls)", human_size, exact);
}

void dd_format_records(const dd_stats *s, wchar_t *buf, size_t n)
{
    _snwprintf_s(buf, n, _TRUNCATE,
                 L"%lld+%lld records in\n%lld+%lld records out\n",
                 (long long)s->r_full, (long long)s->r_partial,
                 (long long)s->w_full, (long long)s->w_partial);
}

void dd_format_transfer(const dd_stats *s, bool progress, wchar_t *buf, size_t n)
{
    long long b = (long long)s->bytes_written;
    wchar_t si[32], iec[32], delta[40], rate[48];

    human((double)b, true, L"B", si, 32);
    human((double)b, false, L"B", iec, 32);
    _snwprintf_s(delta, 40, _TRUNCATE, progress ? L"%.0f s" : L"%g s", s->elapsed);
    if (s->elapsed > 0)
        human((double)b / s->elapsed, true, L"B/s", rate, 48);
    else
        wcscpy_s(rate, 48, L"Infinity B/s");

    if (b < 1000)
        _snwprintf_s(buf, n, _TRUNCATE, L"%lld byte%ls copied, %ls, %ls",
                     b, b == 1 ? L"" : L"s", delta, rate);
    else if (b < 1024)
        _snwprintf_s(buf, n, _TRUNCATE, L"%lld bytes (%ls) copied, %ls, %ls",
                     b, si, delta, rate);
    else
        _snwprintf_s(buf, n, _TRUNCATE, L"%lld bytes (%ls, %ls) copied, %ls, %ls",
                     b, si, iec, delta, rate);
}

void dd_format_xz(const dd_stats *s, wchar_t *buf, size_t n)
{
    long long b = (long long)s->xz_bytes;
    wchar_t si[32], iec[32];

    if (b < 0) {
        buf[0] = 0;
        return;
    }
    human((double)b, true, L"B", si, 32);
    human((double)b, false, L"B", iec, 32);
    if (b < 1000)
        _snwprintf_s(buf, n, _TRUNCATE, L"compressed: %lld byte%ls", b, b == 1 ? L"" : L"s");
    else if (b < 1024)
        _snwprintf_s(buf, n, _TRUNCATE, L"compressed: %lld bytes (%ls)", b, si);
    else
        _snwprintf_s(buf, n, _TRUNCATE, L"compressed: %lld bytes (%ls, %ls)", b, si, iec);
}

void dd_errstr(DWORD err, wchar_t *buf, size_t n)
{
    static const struct { DWORD code; const wchar_t *text; } own[] = {
        { DD_ERROR_XZ_MEMORY,    L"not enough memory for xz" },
        { DD_ERROR_XZ_FORMAT,    L"not a valid .xz file" },
        { DD_ERROR_XZ_OPTIONS,   L"unsupported .xz options" },
        { DD_ERROR_XZ_CORRUPT,   L"the compressed data is corrupt" },
        { DD_ERROR_XZ_TRUNCATED, L"the compressed file is truncated, or its end is damaged" },
        { DD_ERROR_XZ_INTERNAL,  L"internal error in xz" },
        { DD_ERROR_XZ_DEVICE,    L"cannot compress onto a device; write a file instead" },
    };
    DWORD len;
    size_t i;

    for (i = 0; i < sizeof own / sizeof *own; i++)
        if (own[i].code == err) {
            wcsncpy_s(buf, n, own[i].text, _TRUNCATE);
            return;
        }
    len = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                         NULL, err, 0, buf, (DWORD)n, NULL);
    if (len == 0) {
        _snwprintf_s(buf, n, _TRUNCATE, L"error %lu", err);
        return;
    }
    while (len > 0 && (buf[len - 1] == L'\r' || buf[len - 1] == L'\n' ||
                       buf[len - 1] == L' ' || buf[len - 1] == L'.'))
        buf[--len] = 0;
}

const wchar_t *dd_bus_name(int bus)
{
    static const wchar_t *const names[] = {
        L"unknown bus", L"SCSI", L"ATAPI", L"ATA", L"IEEE 1394", L"SSA", L"Fibre Channel",
        L"USB", L"RAID", L"iSCSI", L"SAS", L"SATA", L"SD", L"MMC", L"virtual",
        L"file-backed virtual", L"Storage Spaces", L"NVMe", L"SCM", L"UFS",
    };
    if (bus < 0 || bus >= (int)(sizeof names / sizeof *names))
        return L"unknown bus";
    return names[bus];
}

void dd__msg(const dd_callbacks *cb, dd_msg_kind kind, const wchar_t *fmt, ...)
{
    wchar_t text[2048];
    va_list ap;

    if (!cb || !cb->message)
        return;
    va_start(ap, fmt);
    _vsnwprintf_s(text, sizeof text / sizeof *text, _TRUNCATE, fmt, ap);
    va_end(ap);
    cb->message(cb->ctx, kind, text);
}
