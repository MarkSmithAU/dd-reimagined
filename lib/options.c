/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later
*/
#include "dd_internal.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

void dd_options_init(dd_options *o)
{
    memset(o, 0, sizeof *o);
    o->count = -1;
    o->xz_level = -1;
    o->status = DD_STATUS_DEFAULT;
    o->filter = DD_FILTER_NONE;
}

static bool mul_ok(int64_t a, int64_t b, int64_t *r)
{
    if (a < 0 || b < 0)
        return false;
    if (a != 0 && b > INT64_MAX / a)
        return false;
    *r = a * b;
    return true;
}

/* One factor of a number: digits followed by an optional suffix.  Digits may
   be left out after an 'x' so that "2xM" works as GNU documents. */
static bool parse_factor(const wchar_t *p, const wchar_t *end, bool allow_no_digits, int64_t *out)
{
    int64_t v = 0;
    int64_t mult = 1;
    size_t sl;

    if (p == end || *p < L'0' || *p > L'9') {
        if (!allow_no_digits)
            return false;
        v = 1;
    } else {
        while (p < end && *p >= L'0' && *p <= L'9') {
            int d = *p - L'0';
            if (v > (INT64_MAX - d) / 10)
                return false;
            v = v * 10 + d;
            p++;
        }
    }

    sl = (size_t)(end - p);
    if (sl == 0) {
        mult = 1;
    } else if (sl == 1 && *p == L'c') {
        mult = 1;
    } else if (sl == 1 && *p == L'w') {
        mult = 2;
    } else if (sl == 1 && *p == L'b') {
        mult = 512;
    } else if (sl == 1 && *p == L'B') {
        mult = 1;
    } else {
        static const wchar_t letters[] = L"KMGTPEZYRQ";
        wchar_t c = (*p == L'k') ? L'K' : *p;
        const wchar_t *q = wcschr(letters, c);
        int base, e, i;

        if (!q || c == 0)
            return false;
        e = (int)(q - letters) + 1;
        if (sl == 1)
            base = 1024;
        else if (sl == 2 && p[1] == L'B')
            base = 1000;
        else if (sl == 3 && p[1] == L'i' && p[2] == L'B')
            base = 1024;
        else
            return false;
        for (i = 0; i < e; i++)
            if (!mul_ok(mult, base, &mult))
                return false;
    }
    return mul_ok(v, mult, out);
}

bool dd_parse_number(const wchar_t *s, int64_t *out, bool *bytes)
{
    const wchar_t *p, *end;
    int64_t total = 1;
    bool first = true;
    size_t n;

    if (!s || !*s)
        return false;
    n = wcslen(s);
    end = s + n;
    if (bytes)
        *bytes = (s[n - 1] == L'B');

    p = s;
    for (;;) {
        const wchar_t *x = wcschr(p, L'x');
        const wchar_t *fend = x ? x : end;
        int64_t f;

        if (!parse_factor(p, fend, !first, &f))
            return false;
        if (!mul_ok(total, f, &total))
            return false;
        if (!x)
            break;
        p = x + 1;
        first = false;
        if (!*p)
            return false;
    }
    *out = total;
    return true;
}

typedef struct symbol {
    const wchar_t *name;
    unsigned bit;
    bool in_ok, out_ok;
} symbol;

static const symbol conv_symbols[] = {
    { L"notrunc",   DD_CONV_NOTRUNC,   true, true },
    { L"noerror",   DD_CONV_NOERROR,   true, true },
    { L"sync",      DD_CONV_SYNC,      true, true },
    { L"fsync",     DD_CONV_FSYNC,     true, true },
    { L"fdatasync", DD_CONV_FDATASYNC, true, true },
    { L"excl",      DD_CONV_EXCL,      true, true },
    { L"nocreat",   DD_CONV_NOCREAT,   true, true },
    { L"sparse",    DD_CONV_SPARSE,    true, true },
    { L"swab",      DD_CONV_SWAB,      true, true },
    { L"lcase",     DD_CONV_LCASE,     true, true },
    { L"ucase",     DD_CONV_UCASE,     true, true },
};

/* GNU conversions this port does not implement. */
static const wchar_t *const unsupported_conv[] = {
    L"ascii", L"ebcdic", L"ibm", L"block", L"unblock",
};

/* bit 0 entries are accepted and ignored: they mean nothing on Windows. */
static const symbol flag_symbols[] = {
    { L"fullblock",   DD_FLAG_FULLBLOCK,   true,  false },
    { L"count_bytes", DD_FLAG_COUNT_BYTES, true,  false },
    { L"skip_bytes",  DD_FLAG_SKIP_BYTES,  true,  false },
    { L"seek_bytes",  DD_FLAG_SEEK_BYTES,  false, true  },
    { L"append",      DD_FLAG_APPEND,      false, true  },
    { L"direct",      DD_FLAG_DIRECT,      true,  true  },
    { L"sync",        DD_FLAG_SYNC,        true,  true  },
    { L"dsync",       DD_FLAG_DSYNC,       true,  true  },
    { L"binary",      0,                   true,  true  },
    { L"text",        0,                   true,  true  },
    { L"noctty",      0,                   true,  true  },
    { L"nonblock",    0,                   true,  true  },
    { L"noatime",     0,                   true,  true  },
    { L"nocache",     0,                   true,  true  },
    { L"nofollow",    0,                   true,  true  },
};

/* Parses a comma separated list.  which: 0 = conv, 1 = iflag, 2 = oflag. */
static bool parse_symbols(const wchar_t *v, int which, unsigned *bits, wchar_t *err, size_t errlen)
{
    const symbol *table = which == 0 ? conv_symbols : flag_symbols;
    size_t ntable = which == 0 ? sizeof conv_symbols / sizeof *conv_symbols
                               : sizeof flag_symbols / sizeof *flag_symbols;
    const wchar_t *what = which == 0 ? L"conversion" : which == 1 ? L"input flag" : L"output flag";
    const wchar_t *p = v;

    for (;;) {
        const wchar_t *comma = wcschr(p, L',');
        size_t len = comma ? (size_t)(comma - p) : wcslen(p);
        bool found = false;
        size_t i;

        for (i = 0; i < ntable; i++) {
            if (wcslen(table[i].name) == len && wcsncmp(table[i].name, p, len) == 0) {
                bool ok = which == 0 || (which == 1 ? table[i].in_ok : table[i].out_ok);
                if (!ok)
                    break;
                *bits |= table[i].bit;
                found = true;
                break;
            }
        }
        if (!found && which == 0) {
            for (i = 0; i < sizeof unsupported_conv / sizeof *unsupported_conv; i++) {
                if (wcslen(unsupported_conv[i]) == len && wcsncmp(unsupported_conv[i], p, len) == 0) {
                    _snwprintf_s(err, errlen, _TRUNCATE,
                                 L"conversion '%.*ls' is not supported by this version of dd",
                                 (int)len, p);
                    return false;
                }
            }
        }
        if (!found) {
            _snwprintf_s(err, errlen, _TRUNCATE, L"invalid %ls: '%.*ls'", what, (int)len, p);
            return false;
        }
        if (!comma)
            return true;
        p = comma + 1;
    }
}

bool dd_parse_operand(dd_options *o, const wchar_t *arg, wchar_t *err, size_t errlen)
{
    const wchar_t *eq = wcschr(arg, L'=');
    const wchar_t *v;
    size_t kl;
    int64_t n;
    bool bytes;

    if (!eq) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"unrecognized operand '%ls'", arg);
        return false;
    }
    kl = (size_t)(eq - arg);
    v = eq + 1;

#define KEY(k) (kl == wcslen(k) && wcsncmp(arg, (k), kl) == 0)
#define BAD_NUMBER() \
    do { _snwprintf_s(err, errlen, _TRUNCATE, L"invalid number: '%ls'", v); return false; } while (0)

    if (KEY(L"if")) {
        o->input = v;
    } else if (KEY(L"of")) {
        o->output = v;
    } else if (KEY(L"bs") || KEY(L"ibs") || KEY(L"obs") || KEY(L"cbs")) {
        if (!dd_parse_number(v, &n, NULL) || n <= 0 || n > DD_MAX_BLOCK)
            BAD_NUMBER();
        if (KEY(L"bs"))
            o->bs = n;
        else if (KEY(L"ibs"))
            o->ibs = n;
        else if (KEY(L"obs"))
            o->obs = n;
        /* cbs only matters for block/unblock, which are not supported */
    } else if (KEY(L"skip") || KEY(L"iseek")) {
        if (!dd_parse_number(v, &n, &bytes))
            BAD_NUMBER();
        o->skip = n;
        if (bytes)
            o->iflags |= DD_FLAG_SKIP_BYTES;
    } else if (KEY(L"seek") || KEY(L"oseek")) {
        if (!dd_parse_number(v, &n, &bytes))
            BAD_NUMBER();
        o->seek = n;
        if (bytes)
            o->oflags |= DD_FLAG_SEEK_BYTES;
    } else if (KEY(L"count")) {
        if (!dd_parse_number(v, &n, &bytes))
            BAD_NUMBER();
        o->count = n;
        if (bytes)
            o->iflags |= DD_FLAG_COUNT_BYTES;
    } else if (KEY(L"conv")) {
        return parse_symbols(v, 0, &o->conv, err, errlen);
    } else if (KEY(L"iflag")) {
        return parse_symbols(v, 1, &o->iflags, err, errlen);
    } else if (KEY(L"oflag")) {
        return parse_symbols(v, 2, &o->oflags, err, errlen);
    } else if (KEY(L"status")) {
        if (wcscmp(v, L"none") == 0)
            o->status = DD_STATUS_NONE;
        else if (wcscmp(v, L"noxfer") == 0)
            o->status = DD_STATUS_NOXFER;
        else if (wcscmp(v, L"progress") == 0)
            o->status = DD_STATUS_PROGRESS;
        else {
            _snwprintf_s(err, errlen, _TRUNCATE, L"invalid status level: '%ls'", v);
            return false;
        }
    } else {
        _snwprintf_s(err, errlen, _TRUNCATE, L"unrecognized operand '%ls'", arg);
        return false;
    }
    return true;
#undef KEY
#undef BAD_NUMBER
}

bool dd__validate(const dd_options *o, wchar_t *err, size_t errlen)
{
    if ((o->conv & DD_CONV_EXCL) && (o->conv & DD_CONV_NOCREAT)) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"cannot combine excl and nocreat");
        return false;
    }
    if ((o->conv & DD_CONV_LCASE) && (o->conv & DD_CONV_UCASE)) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"cannot combine lcase and ucase");
        return false;
    }
    if (o->count < -1 || o->skip < 0 || o->seek < 0) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"invalid number");
        return false;
    }
    if (o->xz_memory < 0) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"invalid xz memory size");
        return false;
    }
    if (o->xz_level < -1 || o->xz_level > 9) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"invalid xz level %d; use 0 to 9", o->xz_level);
        return false;
    }
    if (o->xz_level >= 0 &&
        (o->seek || (o->conv & (DD_CONV_NOTRUNC | DD_CONV_SPARSE)) ||
         (o->oflags & (DD_FLAG_APPEND | DD_FLAG_DIRECT)))) {
        _snwprintf_s(err, errlen, _TRUNCATE,
                     L"compressed output is written in one pass; it cannot be combined with seek, notrunc, sparse, append or direct");
        return false;
    }
    if (o->fit && o->xz_raw) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"--fit reads the image's partition table, so it cannot be combined with --no-xz");
        return false;
    }
    if (o->fit) {
        if (!o->input || wcscmp(o->input, L"-") == 0 || _wcsnicmp(o->input, L"/dev/", 5) == 0) {
            _snwprintf_s(err, errlen, _TRUNCATE, L"--fit needs an image file or disk as input");
            return false;
        }
        if (o->count >= 0 || o->skip || o->seek) {
            _snwprintf_s(err, errlen, _TRUNCATE, L"--fit sets the length itself; it cannot be combined with count, skip or seek");
            return false;
        }
        if ((o->conv & (DD_CONV_SWAB | DD_CONV_LCASE | DD_CONV_UCASE | DD_CONV_SYNC | DD_CONV_SPARSE)) ||
            (o->oflags & DD_FLAG_APPEND)) {
            _snwprintf_s(err, errlen, _TRUNCATE, L"--fit writes the image unchanged; it cannot be combined with conversions, sparse or append");
            return false;
        }
    }
    if (o->check_hashes) {
        if (!o->input || wcscmp(o->input, L"-") == 0 || _wcsnicmp(o->input, L"/dev/", 5) == 0 ||
            dd__is_device_path(o->input)) {
            _snwprintf_s(err, errlen, _TRUNCATE, L"--verify-hashes needs an image file as if=");
            return false;
        }
        if (o->output || o->count >= 0 || o->skip || o->seek || o->conv || o->fit ||
            o->xz_level >= 0 || o->verify || o->hashes == DD_HASHES_OFF || o->lock_input || o->sparse_read) {
            _snwprintf_s(err, errlen, _TRUNCATE,
                         L"--verify-hashes only reads if=; it cannot be combined with of=, count, skip, seek, conv, --fit, --xz, --verify, --no-hashes, --lock-input or --sparse-read");
            return false;
        }
    }
    if (o->sparse_unclean && !o->sparse_read) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"--allow-unclean only applies to --sparse-read");
        return false;
    }
    if (o->sparse_read &&
        (!o->input || wcscmp(o->input, L"-") == 0 || _wcsnicmp(o->input, L"/dev/", 5) == 0)) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"--sparse-read reads a file system's metadata first, so it needs an image file or a device as if=");
        return false;
    }
    if (o->hashes == DD_HASHES_ON &&
        (o->seek || (o->conv & DD_CONV_NOTRUNC) || (o->oflags & DD_FLAG_APPEND))) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"--hashes describes a whole new image; it cannot be combined with seek, notrunc or append");
        return false;
    }
    if (o->verify) {
        if (!o->output || wcscmp(o->output, L"-") == 0 || _wcsnicmp(o->output, L"/dev/", 5) == 0) {
            _snwprintf_s(err, errlen, _TRUNCATE, L"--verify reads the output back, so it needs a file or a disk as of=");
            return false;
        }
        if ((o->conv & DD_CONV_SPARSE) && (o->conv & DD_CONV_NOTRUNC || o->oflags & DD_FLAG_APPEND)) {
            _snwprintf_s(err, errlen, _TRUNCATE, L"--verify cannot check conv=sparse output that keeps old data (notrunc or append)");
            return false;
        }
    }
    return true;
}

static const wchar_t *const filter_names[] = {
    L"", L"removable", L"fixed", L"disk", L"partition", L"file"
};

bool dd_parse_filter(const wchar_t *s, dd_filter *out)
{
    int i;
    for (i = 1; i < (int)(sizeof filter_names / sizeof *filter_names); i++) {
        if (wcscmp(s, filter_names[i]) == 0) {
            *out = (dd_filter)i;
            return true;
        }
    }
    return false;
}

const wchar_t *dd_filter_name(dd_filter f)
{
    if ((int)f < 0 || (int)f >= (int)(sizeof filter_names / sizeof *filter_names))
        return L"";
    return filter_names[f];
}
