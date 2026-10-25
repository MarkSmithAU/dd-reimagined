/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later
*/
#include "dd.h"
#include <winioctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static dd_status_level g_status;
static size_t g_progress_len;      /* characters of progress on screen */
static dd_cancel g_cancel;

/* Console gets UTF-16; pipes and files get UTF-8. */
static void write_text(HANDLE h, const wchar_t *s)
{
    DWORD mode, done;
    int len = (int)wcslen(s), bytes;
    char small[4096], *buf;

    if (len == 0 || h == NULL || h == INVALID_HANDLE_VALUE)
        return;
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, s, (DWORD)len, &done, NULL);
        return;
    }
    bytes = WideCharToMultiByte(CP_UTF8, 0, s, len, NULL, 0, NULL, NULL);
    buf = bytes <= (int)sizeof small ? small : malloc(bytes);
    if (!buf)
        return;
    WideCharToMultiByte(CP_UTF8, 0, s, len, buf, bytes, NULL, NULL);
    WriteFile(h, buf, (DWORD)bytes, &done, NULL);
    if (buf != small)
        free(buf);
}

static void vprint(DWORD which, const wchar_t *fmt, va_list ap)
{
    wchar_t small[4096], *buf = small;
    va_list aq;
    int n;

    /* --help is longer than any fixed buffer should have to be */
    va_copy(aq, ap);
    n = _vscwprintf(fmt, aq);
    va_end(aq);
    if (n >= (int)(sizeof small / sizeof *small) && (buf = malloc(((size_t)n + 1) * sizeof *buf)) == NULL)
        buf = small;
    _vsnwprintf_s(buf, buf == small ? sizeof small / sizeof *small : (size_t)n + 1, _TRUNCATE, fmt, ap);
    write_text(GetStdHandle(which), buf);
    if (buf != small)
        free(buf);
}

static void eprint(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprint(STD_ERROR_HANDLE, fmt, ap);
    va_end(ap);
}

static void oprint(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprint(STD_OUTPUT_HANDLE, fmt, ap);
    va_end(ap);
}

static void end_progress_line(void)
{
    if (g_progress_len) {
        eprint(L"\n");
        g_progress_len = 0;
    }
}

static void on_message(void *ctx, dd_msg_kind kind, const wchar_t *text)
{
    (void)ctx;
    if (kind != DD_MSG_ERROR && g_status == DD_STATUS_NONE)
        return;
    end_progress_line();
    eprint(L"dd: %ls\n", text);
}

/* "verified 1.2 GiB of 3.8 GiB (31%), 12 s, 104 MB/s" */
static void format_phase(const dd_stats *s, wchar_t *line, size_t n)
{
    wchar_t done[32], total[32], rate[32];
    const wchar_t *what = s->phase == DD_PHASE_VERIFY ? L"verified"
                        : s->phase == DD_PHASE_MAP ? (s->phase_total > 0 ? L"hashed" : L"decompressed")
                        : L"checked";

    dd_format_size(s->phase_done, false, done, 32);
    if (s->phase_elapsed > 0)
        dd_format_size((int64_t)(s->phase_done / s->phase_elapsed), true, rate, 32);
    else
        wcscpy_s(rate, 32, L"0 B");
    if (s->phase_total > 0) {
        dd_format_size(s->phase_total, false, total, 32);
        _snwprintf_s(line, n, _TRUNCATE, L"%ls %ls of %ls (%d%%), %.0f s, %ls/s", what, done, total,
                     (int)(s->phase_done * 100 / s->phase_total), s->phase_elapsed, rate);
    } else {
        _snwprintf_s(line, n, _TRUNCATE, L"%ls %ls, %.0f s, %ls/s", what, done, s->phase_elapsed, rate);
    }
}

static void on_progress(void *ctx, const dd_stats *s)
{
    wchar_t line[256], xz[96];
    size_t len;

    (void)ctx;
    if (s->phase == DD_PHASE_COPY) {
        dd_format_transfer(s, true, line, 256);
        dd_format_xz(s, xz, 96);
        if (xz[0]) {
            wcsncat_s(line, 256, L"; ", _TRUNCATE);
            wcsncat_s(line, 256, xz, _TRUNCATE);
        }
    } else {
        format_phase(s, line, 256);
    }
    len = wcslen(line);
    if (len < g_progress_len)
        eprint(L"\r%ls%*ls", line, (int)(g_progress_len - len), L"");
    else
        eprint(L"\r%ls", line);
    g_progress_len = len;
}

static void print_stats(const dd_stats *s)
{
    wchar_t buf[512];

    if (g_status == DD_STATUS_NONE)
        return;
    end_progress_line();
    dd_format_records(s, buf, 512);
    eprint(L"%ls", buf);
    if (g_status != DD_STATUS_NOXFER) {
        dd_format_transfer(s, false, buf, 512);
        eprint(L"%ls\n", buf);
    }
}

static BOOL WINAPI on_ctrl(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT)
        return FALSE;
    if (g_cancel.requested)
        return FALSE;       /* second Ctrl+C: let Windows end the process */
    dd_cancel_request(&g_cancel);
    return TRUE;
}

static void try_help(void)
{
    eprint(L"Try 'dd --help' for more information.\n");
}

static void usage(void)
{
    oprint(L"Usage: dd [OPERAND]...\n"
           L"  or:  dd OPTION\n"
           L"Copy a file, converting and formatting according to the operands.\n"
           L"\n"
           L"  bs=BYTES        read and write up to BYTES bytes at a time (default: 512);\n"
           L"                  overrides ibs and obs\n"
           L"  conv=CONVS      convert the file as per the comma separated symbol list\n"
           L"  count=N         copy only N input blocks\n"
           L"  ibs=BYTES       read up to BYTES bytes at a time (default: 512)\n"
           L"  if=FILE         read from FILE instead of standard input\n"
           L"  iflag=FLAGS     read as per the comma separated symbol list\n"
           L"  obs=BYTES       write BYTES bytes at a time (default: 512)\n"
           L"  of=FILE         write to FILE instead of standard output\n"
           L"  oflag=FLAGS     write as per the comma separated symbol list\n"
           L"  seek=N          (or oseek=N) skip N obs-sized output blocks\n"
           L"  skip=N          (or iseek=N) skip N ibs-sized input blocks\n"
           L"  status=LEVEL    The LEVEL of information to print to standard error;\n"
           L"                  'none' suppresses everything but error messages,\n"
           L"                  'noxfer' suppresses the final transfer statistics,\n"
           L"                  'progress' shows periodic transfer statistics\n"
           L"\n"
           L"N and BYTES may be followed by the following multiplicative suffixes:\n"
           L"c=1, w=2, b=512, kB=1000, K=1024, MB=1000*1000, M=1024*1024, xM=M,\n"
           L"GB=1000*1000*1000, G=1024*1024*1024, and so on for T, P, E.\n"
           L"Binary prefixes can be used, too: KiB=K, MiB=M, and so on.\n"
           L"If N ends in 'B', it counts bytes not blocks.\n"
           L"\n"
           L"Each CONV symbol may be:\n"
           L"\n"
           L"  lcase     change upper case to lower case\n"
           L"  ucase     change lower case to upper case\n"
           L"  sparse    try to seek rather than write all-NUL output blocks\n"
           L"  swab      swap every pair of input bytes\n"
           L"  sync      pad every input block with NULs to ibs-size\n"
           L"  excl      fail if the output file already exists\n"
           L"  nocreat   do not create the output file\n"
           L"  notrunc   do not truncate the output file\n"
           L"  noerror   continue after read errors\n"
           L"  fdatasync physically write output file data before finishing\n"
           L"  fsync     likewise, but also write metadata\n"
           L"\n"
           L"Each FLAG symbol may be:\n"
           L"\n"
           L"  append    append mode (makes sense only for output; conv=notrunc suggested)\n"
           L"  direct    use unbuffered I/O for data\n"
           L"  dsync     use write-through I/O for data\n"
           L"  sync      likewise, but also for metadata\n"
           L"  fullblock accumulate full blocks of input (iflag only)\n"
           L"  count_bytes  treat 'count=N' as a byte count (iflag only)\n"
           L"  skip_bytes  treat 'skip=N' as a byte count (iflag only)\n"
           L"  seek_bytes  treat 'seek=N' as a byte count (oflag only)\n"
           L"\n"
           L"Windows devices and special files:\n"
           L"  \\\\.\\PhysicalDriveN    whole disk N        \\\\.\\X:          volume X:\n"
           L"  \\\\.\\CdRomN            optical drive N     \\\\?\\Volume{GUID} a volume\n"
           L"  \\\\?\\Device\\HarddiskN\\PartitionM          NT device names, as in dd 0.6\n"
           L"  - /dev/stdin /dev/stdout /dev/stderr /dev/zero /dev/null /dev/random\n"
           L"\n"
           L"Before writing to a volume or a whole disk, dd locks and dismounts the\n"
           L"volumes on it, and stops if one is in use.  It never writes to the disk\n"
           L"Windows is running from.  Reading locks nothing unless --lock-input is\n"
           L"given.  While volumes are locked, their drive letters are removed, and\n"
           L"they are put back when dd is done.  Raw device access needs an elevated\n"
           L"prompt.\n"
           L"\n"
           L"Windows options:\n"
           L"  id=DRIVE        read the whole disk holding DRIVE (e.g. id=E:), provided it\n"
           L"                  is the disk's only partition; defaults to bs=1M (an optical\n"
           L"                  drive: one 2048-byte sector) and progress\n"
           L"  od=DRIVE        write the whole disk holding DRIVE, as for id=\n"
           L"  --list          list disks, optical drives and volumes\n"
           L"  --filter=TYPE   refuse to write unless the output is TYPE: removable, fixed,\n"
           L"                  disk, partition or file.  Running dd as dd-TYPE.exe sets\n"
           L"                  this filter.  With --list, only matching devices are shown\n"
           L"  --force-dismount  dismount output volumes even while programs have files\n"
           L"                  open on them; those programs lose their open files\n"
           L"  --lock-input    lock the volumes of if= (a disk or volume) while reading it,\n"
           L"                  so nothing can change them during the copy.  They stay\n"
           L"                  mounted.  Fails while programs have files open on them\n"
           L"  --keep-letters  leave the drive letters of locked volumes in place\n"
           L"  --layout        show the MBR or GPT partition table of if= (or id=) and exit\n"
           L"  --fit           copy if= (an image or a disk) only up to the end of its last\n"
           L"                  partition.  Writing, an image bigger than the disk can be\n"
           L"                  written when its partitions fit; reading a card (id=E:\n"
           L"                  of=card.img --fit), the image holds only the partitions.\n"
           L"                  A GPT's backup copy is rebuilt at the end of the output.\n"
           L"  --sparse-read   read only the space the file systems of if= use: FAT12/16/32,\n"
           L"                  exFAT, NTFS, ext2/3/4 and Linux swap, on each partition or on\n"
           L"                  a volume.  Free space is written as zeros without being\n"
           L"                  read, so an image of a mostly empty card is quick to make\n"
           L"                  and compresses well (--xz, or conv=sparse for a sparse\n"
           L"                  file).  Partition tables, gaps and other partitions are\n"
           L"                  read as usual.  A file system not cleanly unmounted is\n"
           L"                  read whole.  With --layout, shows what would be read.\n"
           L"                  An xz-compressed if= is read in order, so it needs its file\n"
           L"                  systems mapped first, by --make-sparse-map; its free space\n"
           L"                  is then read and written as zeros\n"
           L"  --make-sparse-map  map the file systems of if=, an xz-compressed image, and\n"
           L"                  write what --sparse-read needs to IMAGE.sparse (named as\n"
           L"                  the hash file is), bound to the .xz file by its SHA-256\n"
           L"  --allow-unclean with --sparse-read, map file systems that were not cleanly\n"
           L"                  unmounted too, counting everything that may be in use\n"
           L"                  (ext3/4: what the journal holds; FAT: every FAT copy).\n"
           L"                  Data written just before an unclean unmount may be lost\n"
           L"  --xz[=LEVEL]    compress the output to .xz (LZMA2, multithreaded) at preset\n"
           L"                  LEVEL 0-9 (default 9; the dictionary grows from 256 KiB\n"
           L"                  to 64 MiB, and speed falls: level 1 is several times\n"
           L"                  faster than 6, and 9 is slow).  An of= ending in .xz is\n"
           L"                  compressed even without this option.  An .xz input is\n"
           L"                  always decompressed on the fly\n"
           L"  --xz-memory=SIZE  the memory compressing and decompressing may use\n"
           L"                  together (default: what is free when dd starts, less\n"
           L"                  2 GiB if there is a page file, or 4 GiB if not; a quarter\n"
           L"                  of it if no more than that is free).\n"
           L"                  Each compressing thread needs 1.2 GiB at level 9, 165 MiB\n"
           L"                  at 6; fewer threads are run to fit, and if one thread does\n"
           L"                  not fit, a lower level is used, with a warning\n"
           L"  --no-xz         raw: never compress or decompress\n"
           L"  --hashes        write a SHA-256 hash file for any new output file.  It is\n"
           L"                  named after the image without .xz: disk.img and disk.img.xz\n"
           L"                  both use disk.img.hashes, in sha256sum's format, holding the\n"
           L"                  hash of the image and, if compressed, of the .xz file.\n"
           L"                  Reading a disk or volume into a file, it is written anyway;\n"
           L"                  an input image that has one is checked as it is read\n"
           L"  --no-hashes     neither write nor check hash files\n"
           L"  --verify        after the copy, read the output back (unbuffered) and check\n"
           L"                  it holds exactly what was written; an .xz output is\n"
           L"                  decompressed again\n"
           L"  --verify-hashes check if= (an image) against its hash file and exit; nothing\n"
           L"                  is written\n"
           L"  --progress      same as status=progress\n"
           L"  --size          accepted for compatibility; device sizes are always honoured\n"
           L"\n"
           L"      --help      display this help and exit\n"
           L"      --version   output version information and exit\n"
           L"\n"
           L"Sending Ctrl+C to a running dd makes it print statistics and stop.\n");
}

static void version(void)
{
    oprint(L"dd (" DD_WIDEN(DD_PRODUCT_NAME) L") " DD_VERSION L"\n"
           L"dd reimagined for Windows Copyright (C) 2026 Mark Smith\n"
           L"dd Copyright (C) 2003 John Newbigin\n"
           L"License GPLv2+: GNU GPL version 2 or later <https://gnu.org/licenses/gpl.html>.\n"
           L"This is free software: you are free to change and redistribute it.\n"
           L"There is NO WARRANTY, to the extent permitted by law.\n"
           L"Uses liblzma %ls from XZ Utils (BSD Zero Clause License).\n", dd_xz_version());
}

/* ---- --list ------------------------------------------------------------ */

static void print_size(int64_t size)
{
    wchar_t si[32], iec[32];
    dd_format_size(size, true, si, 32);
    dd_format_size(size, false, iec, 32);
    oprint(L"%ls, %ls (%lld bytes)", si, iec, (long long)size);
}

static void print_volume(const dd_volume_info *v, const wchar_t *indent)
{
    const wchar_t *m;
    bool any = false;

    oprint(L"%ls%ls\n%ls    ", indent, v->path, indent);
    for (m = v->mounts; m && *m; m += wcslen(m) + 1) {
        oprint(L"%ls%ls", any ? L", " : L"mounted on ", m);
        any = true;
    }
    if (!any)
        oprint(L"not mounted");
    if (v->filesystem[0])
        oprint(L", %ls", v->filesystem);
    if (v->label[0])
        oprint(L" \"%ls\"", v->label);
    if (v->size >= 0) {
        oprint(L", ");
        print_size(v->size);
    }
    oprint(L"\n");
}

static void print_disk(const dd_disk_info *d)
{
    oprint(L"%ls", d->path);
    if (d->vendor[0] || d->product[0])
        oprint(L"  %ls%ls%ls", d->vendor, d->vendor[0] && d->product[0] ? L" " : L"", d->product);
    oprint(L"\n    %ls, %ls", dd_bus_name(d->bus_type), d->removable ? L"removable" : L"fixed");
    if (!d->media_present) {
        oprint(L", no media\n");
        return;
    }
    if (d->size >= 0) {
        oprint(L", ");
        print_size(d->size);
    }
    if (d->sector_size)
        oprint(L", %lu-byte sectors", d->sector_size);
    if (d->partitions >= 0) {
        const wchar_t *style = d->partition_style == PARTITION_STYLE_MBR ? L"MBR"
                             : d->partition_style == PARTITION_STYLE_GPT ? L"GPT" : L"no partition table";
        oprint(L", %ls, %d partition%ls", style, d->partitions, d->partitions == 1 ? L"" : L"s");
    }
    oprint(L"\n");
}

static bool disk_shown(const dd_disk_info *d, dd_filter f)
{
    if (d->kind == DD_DEV_CDROM)
        return f == DD_FILTER_NONE || f == DD_FILTER_PARTITION;
    switch (f) {
    case DD_FILTER_NONE:
    case DD_FILTER_DISK:      return true;
    case DD_FILTER_REMOVABLE: return d->removable;
    case DD_FILTER_FIXED:     return !d->removable;
    default:                  return false;
    }
}

static bool has_parent(const dd_device_list *l, const dd_volume_info *v)
{
    size_t i;
    for (i = 0; i < l->ndisks; i++)
        if (l->disks[i].kind == v->parent_kind && l->disks[i].number == v->parent_number)
            return true;
    return false;
}

static int list_devices(dd_filter filter)
{
    dd_device_list l;
    bool unknown_partitions = false, first = true, other = false;
    size_t i, k;

    if (filter == DD_FILTER_FILE) {
        oprint(L"--filter=file allows regular files only; there are no devices to list.\n");
        return 0;
    }
    dd_list_devices(&l);

    for (i = 0; i < l.ndisks; i++) {
        const dd_disk_info *d = &l.disks[i];
        if (!disk_shown(d, filter))
            continue;
        if (!first)
            oprint(L"\n");
        first = false;
        print_disk(d);
        if (d->kind == DD_DEV_DISK && d->media_present && d->partitions < 0)
            unknown_partitions = true;
        for (k = 0; k < l.nvolumes; k++)
            if (l.volumes[k].parent_kind == d->kind && l.volumes[k].parent_number == d->number)
                print_volume(&l.volumes[k], L"    ");
    }

    if (filter == DD_FILTER_NONE || filter == DD_FILTER_PARTITION) {
        for (k = 0; k < l.nvolumes; k++) {
            const dd_volume_info *v = &l.volumes[k];
            if (has_parent(&l, v))
                continue;
            if (!other) {
                oprint(L"%lsOther volumes\n", first ? L"" : L"\n");
                other = true;
                first = false;
            }
            print_volume(v, L"    ");
        }
    }

    if (filter == DD_FILTER_NONE) {
        oprint(L"%ls"
               L"Virtual input devices\n"
               L"    /dev/zero      (null data)\n"
               L"    /dev/random    (random data)\n"
               L"    -              (standard input)\n"
               L"\n"
               L"Virtual output devices\n"
               L"    -              (standard output)\n"
               L"    /dev/null      (discard the data)\n", first ? L"" : L"\n");
    }
    if (unknown_partitions)
        eprint(L"dd: partition tables can only be read from an elevated prompt\n");
    dd_free_device_list(&l);
    return 0;
}

/* ---- --layout ---------------------------------------------------------- */

/* --layout --sparse-read: the file systems, and how much of each is read */
static void print_fs_report(const dd_fs_report *r, const wchar_t *heading)
{
    wchar_t start[DD_SIZE_EXACT_CHARS], size[32], used[32];
    size_t i;

    oprint(L"\n%ls\n", heading);
    oprint(L"  #  %-29ls %-12ls %-8ls %-12ls %ls\n", L"Start", L"Size", L"Type", L"Read", L"");
    for (i = 0; i < r->count; i++) {
        const dd_fs_volume *v = &r->vols[i];
        dd_format_size_exact(v->start, false, false, start, DD_SIZE_EXACT_CHARS);
        dd_format_size(v->size, false, size, 32);
        dd_format_size(v->used, false, used, 32);
        oprint(L"%3d  %-29ls %-12ls %-8ls %-12ls %ls%ls%ls\n", v->partition, start, size,
               v->type[0] ? v->type : L"-", v->sparse ? used : L"all",
               v->unclean ? L"unclean" : L"", v->unclean && v->note[0] ? L": " : L"", v->note);
    }
    dd_format_size(r->read, false, used, 32);
    dd_format_size(r->size, false, size, 32);
    oprint(L"reads %ls of %ls (%lld of %lld bytes); the rest is free space, written as zeros\n",
           used, size, (long long)r->read, (long long)r->size);
}

/* --layout --sparse-read: the file systems, and how much of each is read */
static int show_file_systems(const wchar_t *path, bool allow_unclean)
{
    dd_fs_report *r = malloc(sizeof *r);
    wchar_t err[512];

    if (!r) {
        eprint(L"dd: memory exhausted\n");
        return 1;
    }
    if (!dd_fs_report_read(path, allow_unclean, r, err, 512)) {
        eprint(L"dd: %ls\n", err);
        free(r);
        return 1;
    }
    print_fs_report(r, allow_unclean ? L"File systems (--sparse-read --allow-unclean)" : L"File systems (--sparse-read)");
    free(r);
    return 0;
}

/* --make-sparse-map: maps a compressed image's file systems, for --sparse-read */
static int make_sparse_map(const wchar_t *path, bool allow_unclean, const dd_callbacks *cb)
{
    dd_fs_report *r = malloc(sizeof *r);
    dd_stats st;
    dd_result res;

    if (!r) {
        eprint(L"dd: memory exhausted\n");
        return 1;
    }
    if (!path || wcscmp(path, L"-") == 0) {
        eprint(L"dd: --make-sparse-map needs if=, an xz-compressed image\n");
        free(r);
        return 1;
    }
    res = dd_make_sparse_map(path, allow_unclean, cb, &g_cancel, &st, r);
    end_progress_line();
    if (res == DD_OK)
        print_fs_report(r, L"File systems, as the sparse map records them");
    free(r);
    return res == DD_CANCELLED ? 130 : res == DD_OK ? 0 : 1;
}

static int show_layout(const wchar_t *path, const wchar_t *name)
{
    dd_layout *l = malloc(sizeof *l);
    wchar_t err[512], start[DD_SIZE_EXACT_CHARS], size[DD_SIZE_EXACT_CHARS], type[64];
    size_t i;

    if (!l) {
        eprint(L"dd: memory exhausted\n");
        return 1;
    }
    if (!path || wcscmp(path, L"-") == 0) {
        eprint(L"dd: --layout needs if= or id=\n");
        free(l);
        return 1;
    }
    if (!dd_read_layout(path, l, err, 512)) {
        eprint(L"dd: %ls\n", err);
        free(l);
        return 1;
    }
    oprint(L"%ls: ", name);
    print_size(l->image_size);
    oprint(L"\n");
    if (l->compressed) {
        oprint(L"xz-compressed: ");
        print_size(l->file_size);
        oprint(L" on disk\n");
    }
    if (l->scheme == DD_SCHEME_NONE) {
        oprint(L"no partition table\n");
    } else {
        if (l->scheme == DD_SCHEME_GPT) {
            const GUID *g = &l->disk_guid;
            oprint(L"GPT, %lu-byte sectors, disk GUID %08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X\n",
                   l->sector_size, g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1],
                   g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
        } else {
            oprint(L"MBR, disk signature %08lX\n", l->mbr_signature);
        }
        oprint(L"  #  %-29ls %-29ls %-24ls %ls\n", L"Start", L"Size", L"Type", l->scheme == DD_SCHEME_GPT ? L"Name" : L"");
        for (i = 0; i < l->count; i++) {
            const dd_partition *p = &l->parts[i];
            dd_format_size_exact(p->start, false, false, start, DD_SIZE_EXACT_CHARS);
            dd_format_size_exact(p->size, false, false, size, DD_SIZE_EXACT_CHARS);
            dd_partition_type_name(l, p, type, 64);
            oprint(L"%3d  %-29ls %-29ls %-24ls %ls%ls\n", p->number, start, size, type,
                   l->scheme == DD_SCHEME_GPT ? p->name : p->bootable ? L"boot" : L"",
                   p->logical ? L"logical" : L"");
        }
    }
    if (l->iso_size) {
        oprint(L"ISO 9660 volume: ");
        print_size(l->iso_size);
        oprint(L"\n");
    }
    dd_format_size_exact(l->data_end, false, false, size, DD_SIZE_EXACT_CHARS);
    oprint(L"data ends at %ls", size);
    if (l->scheme != DD_SCHEME_NONE) {
        oprint(L"; a disk needs at least ");
        print_size(dd_layout_needed(l));
    }
    oprint(L"\n");
    if (l->note[0])
        oprint(L"note: %ls\n", l->note);
    free(l);
    return 0;
}

/* ---- main -------------------------------------------------------------- */

static bool starts_with(const wchar_t *s, const wchar_t *prefix)
{
    return wcsncmp(s, prefix, wcslen(prefix)) == 0;
}

/* dd-removable.exe and friends imply --filter=removable etc. */
static bool filter_from_program_name(const wchar_t *argv0, dd_filter *f)
{
    const wchar_t *base = argv0, *p;
    wchar_t name[64];
    size_t len;

    for (p = argv0; *p; p++)
        if (*p == L'\\' || *p == L'/' || *p == L':')
            base = p + 1;
    wcsncpy_s(name, 64, base, _TRUNCATE);
    _wcslwr_s(name, 64);
    len = wcslen(name);
    if (len > 4 && wcscmp(name + len - 4, L".exe") == 0)
        name[len - 4] = 0;
    if (wcsncmp(name, L"dd-", 3) != 0)
        return false;
    return dd_parse_filter(name + 3, f);
}

static bool resolve(const wchar_t *op, const wchar_t *spec, wchar_t *path, wchar_t *name, int64_t *bs)
{
    dd_resolved r;
    wchar_t err[512];

    if (!dd_resolve_drive(spec, &r, err, 512)) {
        eprint(L"dd: %ls='%ls': %ls\n", op, spec, err);
        return false;
    }
    wcscpy_s(path, MAX_PATH, r.path);
    wcscpy_s(name, MAX_PATH, r.name);
    if (r.default_bs > *bs)
        *bs = r.default_bs;
    if (g_status != DD_STATUS_NONE)
        eprint(L"dd: %ls=%ls selects %ls (%ls)\n", op, spec, r.name, r.what);
    return true;
}

int wmain(int argc, wchar_t **argv)
{
    static wchar_t id_path[MAX_PATH], od_path[MAX_PATH], id_name[MAX_PATH], od_name[MAX_PATH];
    const wchar_t *id = NULL, *od = NULL;
    bool list = false, layout = false, status_given = false, no_xz = false, make_map = false;
    int64_t drive_bs = 0;
    wchar_t err[512];
    dd_callbacks cb;
    dd_options opt;
    dd_stats st;
    dd_result r;
    int i;

    /* Copies and compression run as background work: every thread,
       including liblzma's workers, which inherit the class. */
    SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);

    dd_options_init(&opt);
    filter_from_program_name(argv[0], &opt.filter);

    for (i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];

        if (wcscmp(a, L"--help") == 0 || wcscmp(a, L"-h") == 0) {
            usage();
            return 0;
        } else if (wcscmp(a, L"--version") == 0 || wcscmp(a, L"-V") == 0) {
            version();
            return 0;
        } else if (wcscmp(a, L"--") == 0) {
            continue;
        } else if (wcscmp(a, L"--list") == 0) {
            list = true;
        } else if (wcscmp(a, L"--progress") == 0) {
            opt.status = DD_STATUS_PROGRESS;
            status_given = true;
        } else if (wcscmp(a, L"--force-dismount") == 0) {
            opt.force_dismount = true;
        } else if (wcscmp(a, L"--lock-input") == 0) {
            opt.lock_input = true;
        } else if (wcscmp(a, L"--keep-letters") == 0) {
            opt.keep_letters = true;
        } else if (wcscmp(a, L"--sparse-read") == 0) {
            opt.sparse_read = true;
        } else if (wcscmp(a, L"--make-sparse-map") == 0) {
            make_map = true;
        } else if (wcscmp(a, L"--allow-unclean") == 0) {
            opt.sparse_unclean = true;
        } else if (wcscmp(a, L"--fit") == 0) {
            opt.fit = true;
        } else if (wcscmp(a, L"--hashes") == 0) {
            opt.hashes = DD_HASHES_ON;
        } else if (wcscmp(a, L"--no-hashes") == 0) {
            opt.hashes = DD_HASHES_OFF;
        } else if (wcscmp(a, L"--verify") == 0) {
            opt.verify = true;
        } else if (wcscmp(a, L"--verify-hashes") == 0) {
            opt.check_hashes = true;
        } else if (wcscmp(a, L"--xz") == 0) {
            opt.xz_level = DD_XZ_DEFAULT_LEVEL;
        } else if (starts_with(a, L"--xz=")) {
            if (a[5] < L'0' || a[5] > L'9' || a[6]) {
                eprint(L"dd: invalid xz level '%ls'; use 0 to 9\n", a + 5);
                try_help();
                return 1;
            }
            opt.xz_level = a[5] - L'0';
        } else if (starts_with(a, L"--xz-memory=")) {
            if (!dd_parse_number(a + 12, &opt.xz_memory, NULL) || opt.xz_memory <= 0) {
                eprint(L"dd: invalid memory size '%ls'\n", a + 12);
                try_help();
                return 1;
            }
        } else if (wcscmp(a, L"--no-xz") == 0) {
            no_xz = true;
        } else if (wcscmp(a, L"--layout") == 0) {
            layout = true;
        } else if (wcscmp(a, L"--size") == 0) {
            /* 0.6 needed this to stop at the end of USB devices */
        } else if (starts_with(a, L"--filter=")) {
            if (!dd_parse_filter(a + 9, &opt.filter)) {
                eprint(L"dd: invalid filter '%ls'; use removable, fixed, disk, partition or file\n", a + 9);
                try_help();
                return 1;
            }
        } else if (starts_with(a, L"--")) {
            eprint(L"dd: unrecognized option '%ls'\n", a);
            try_help();
            return 1;
        } else if (starts_with(a, L"id=")) {
            id = a + 3;
        } else if (starts_with(a, L"od=")) {
            od = a + 3;
        } else {
            if (!dd_parse_operand(&opt, a, err, 512)) {
                eprint(L"dd: %ls\n", err);
                try_help();
                return 1;
            }
            if (starts_with(a, L"status="))
                status_given = true;
        }
    }
    g_status = opt.status;

    if (list)
        return list_devices(opt.filter);

    if (id) {
        if (!resolve(L"id", id, id_path, id_name, &drive_bs))
            return 1;
        opt.input = id_path;
        opt.input_name = id_name;
    }
    if (od) {
        if (!resolve(L"od", od, od_path, od_name, &drive_bs))
            return 1;
        opt.output = od_path;
        opt.output_name = od_name;
    }
    if (layout) {
        int rc;
        if (opt.sparse_unclean && !opt.sparse_read) {
            eprint(L"dd: --allow-unclean only applies to --sparse-read\n");
            return 1;
        }
        rc = show_layout(opt.input, opt.input_name ? opt.input_name : opt.input);
        if (rc == 0 && opt.sparse_read)
            rc = show_file_systems(opt.input, opt.sparse_unclean);
        return rc;
    }

    /* .xz: an output named *.xz is compressed; --no-xz turns all of it off */
    if (no_xz) {
        opt.xz_level = -1;
        opt.xz_raw = true;
    } else if (opt.xz_level < 0 && opt.output) {
        size_t n = wcslen(opt.output);
        if (n > 3 && _wcsicmp(opt.output + n - 3, L".xz") == 0)
            opt.xz_level = DD_XZ_DEFAULT_LEVEL;
    }
    if ((id || od) && !opt.bs && !opt.ibs && !opt.obs)
        opt.bs = drive_bs;
    if ((id || od) && !status_given)
        opt.status = DD_STATUS_PROGRESS;
    g_status = opt.status;

    dd_cancel_init(&g_cancel);
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    memset(&cb, 0, sizeof cb);
    cb.message = on_message;
    cb.progress = on_progress;
    cb.progress_interval_ms = opt.status == DD_STATUS_PROGRESS ? 1000 : 0;
    if (make_map)
        return make_sparse_map(opt.input, opt.sparse_unclean, &cb);

    r = dd_run(&opt, &cb, &g_cancel, &st);
    if (st.started && !opt.check_hashes)
        print_stats(&st);
    else
        end_progress_line();
    if (r == DD_CANCELLED)
        return 130;
    return r == DD_OK ? 0 : 1;
}
