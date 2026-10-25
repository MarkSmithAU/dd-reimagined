/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later

    Copies what the Input row names (a disk, an image file, zeros or random
    data) to what the Output row names (a disk, an image file, or nothing,
    to test that the input reads), or checks an image against its SHA-256
    hash file.  The copy runs dd_run() on a worker thread; its callbacks
    post messages back to the dialog.
*/
#include "dd.h"
#include "resource.h"
#include <commctrl.h>
#include <commdlg.h>
#include <dbt.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WM_APP_MESSAGE  (WM_APP + 1)   /* wParam dd_msg_kind, lParam wchar_t* to free */
#define WM_APP_PROGRESS (WM_APP + 2)   /* lParam dd_stats* to free */
#define WM_APP_DONE     (WM_APP + 3)   /* wParam dd_result, lParam dd_stats* to free */
#define TIMER_REFRESH   1
#define PATH_CHARS      1024
#define IDM_ABOUT       0x0010         /* in the system menu: below 0xF000, low 4 bits clear */

/* What the window, its progress and its message boxes are called. */
#define APP_NAME        DD_WIDEN(DD_PRODUCT_NAME)
#define APP_TITLE       APP_NAME L" " DD_VERSION

/* What the Input and Output lists offer */
typedef enum src { SRC_DISK, SRC_FILE, SRC_ZERO, SRC_RANDOM } src;
typedef enum dst { DST_DISK, DST_FILE, DST_NULL } dst;

/* The operations, from the input and output chosen:
   image to disk, disk to image, image to nothing (checks its hash file, or
   only reads it), image to image, disk to disk, zeros or random data to a
   disk, and disk to nothing (a read test). */
typedef enum op { OP_WRITE, OP_READ, OP_CHECK, OP_IMAGES, OP_DISKS, OP_WIPE, OP_TEST } op;

/* What the worker thread does */
typedef enum task { TASK_COPY, TASK_MAP } task;

typedef struct app {
    HWND           dlg;
    HICON          icon, icon_small;
    HDEVNOTIFY     disk_notify;               /* disks arriving and leaving */
    dd_device_list devices;
    wchar_t        system_root[MAX_PATH];     /* e.g. C:\ */
    HANDLE         worker;
    dd_cancel      cancel;
    dd_options     opt;
    wchar_t        input[PATH_CHARS];
    wchar_t        output[PATH_CHARS];
    wchar_t        confirmed_overwrite[PATH_CHARS];
    wchar_t        target[512];
    wchar_t        disk_name[32];             /* \\.\PhysicalDriveN, for messages */
    wchar_t        src_name[32];              /* the same, of the disk read when copying disks */
    task           task;
    bool           map_unclean;               /* TASK_MAP: with --allow-unclean */
    wchar_t        map_input[PATH_CHARS];     /* TASK_MAP: the image mapped */
    bool           confirming;                /* a confirmation box is open */
    bool           refresh_pending;           /* a device change arrived meanwhile */
    int            bar_ahead;                 /* the progress bar is pale up to here (of 1000) */
    dd_layout      layout;                    /* of the image file, write mode */
    bool           layout_ok;
    wchar_t        layout_err[512];
    dd_fs_report  *fs;                        /* reading: what sparse reading would read */
    bool           fs_ok;
    bool           running;
    bool           writing;
    bool           checking;                  /* only checking an image's hashes */
    bool           testing;                   /* only reading, to /dev/null */
    bool           verified;                  /* the copy was read back */
    bool           removable;
    bool           close_when_done;
} app;

static app g;

static const struct { const wchar_t *label; int64_t size; } block_sizes[] = {
    { L"512 bytes", 512 },
    { L"4 KiB",     4096 },
    { L"64 KiB",    65536 },
    { L"1 MiB",     1048576 },
    { L"4 MiB",     4194304 },
    { L"16 MiB",    16777216 },
};
#define DEFAULT_BLOCK_SIZE 3

/* The Input and Output lists, in their order */
static const struct { const wchar_t *label; src src; } inputs[] = {
    { L"Disk",               SRC_DISK },
    { L"Image file",         SRC_FILE },
    { L"Zeros (wipe)",       SRC_ZERO },
    { L"Random data (wipe)", SRC_RANDOM },
};
static const struct { const wchar_t *label; dst dst; } outputs[] = {
    { L"Disk",                DST_DISK },
    { L"Image file",          DST_FILE },
    { L"Nothing (read test)", DST_NULL },
};

/* ---- helpers ----------------------------------------------------------- */

static int list_data(int id, int none)
{
    LRESULT i = SendDlgItemMessageW(g.dlg, id, CB_GETCURSEL, 0, 0);
    return i == CB_ERR ? none : (int)SendDlgItemMessageW(g.dlg, id, CB_GETITEMDATA, (WPARAM)i, 0);
}

/* What is read and what is written, as chosen in the two lists */
static src input_kind(void)
{
    return (src)list_data(IDC_INPUT, SRC_DISK);
}

static dst output_kind(void)
{
    return (dst)list_data(IDC_OUTPUT, DST_FILE);
}

static op operation(void)
{
    src s = input_kind();
    dst d = output_kind();

    if (s == SRC_ZERO || s == SRC_RANDOM)
        return OP_WIPE;             /* the Output list then offers only Disk */
    if (s == SRC_DISK)
        return d == DST_DISK ? OP_DISKS : d == DST_FILE ? OP_READ : OP_TEST;
    return d == DST_DISK ? OP_WRITE : d == DST_FILE ? OP_IMAGES : OP_CHECK;
}

static bool source_is_disk(void)
{
    return input_kind() == SRC_DISK;
}

static bool source_is_file(void)
{
    return input_kind() == SRC_FILE;
}

static bool target_is_disk(void)
{
    return output_kind() == DST_DISK;
}

static bool target_is_file(void)
{
    return output_kind() == DST_FILE;
}

/* Appends text to the log, turning \n into the \r\n an edit control needs. */
static void log_text(const wchar_t *text)
{
    HWND edit = GetDlgItem(g.dlg, IDC_LOG);
    wchar_t buf[4096];
    size_t o = 0;
    int len;

    for (; *text && o + 3 < sizeof buf / sizeof *buf; text++) {
        if (*text == L'\n')
            buf[o++] = L'\r';
        buf[o++] = *text;
    }
    buf[o] = 0;
    len = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETSEL, len, len);
    SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)buf);
}

static void log_line(const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf_s(buf, sizeof buf / sizeof *buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    wcsncat_s(buf, sizeof buf / sizeof *buf, L"\n", _TRUNCATE);
    log_text(buf);
}

static void error_box(const wchar_t *text)
{
    MessageBoxW(g.dlg, text, APP_NAME, MB_OK | MB_ICONERROR);
}

/* Mount points of the volumes on a disk, "E:\, F:\". */
static void disk_mounts(const dd_disk_info *d, wchar_t *buf, size_t n)
{
    size_t i;

    buf[0] = 0;
    for (i = 0; i < g.devices.nvolumes; i++) {
        const dd_volume_info *v = &g.devices.volumes[i];
        const wchar_t *m;
        if (v->parent_kind != d->kind || v->parent_number != d->number)
            continue;
        for (m = v->mounts; m && *m; m += wcslen(m) + 1) {
            if (buf[0])
                wcsncat_s(buf, n, L", ", _TRUNCATE);
            wcsncat_s(buf, n, m, _TRUNCATE);
        }
    }
}

static bool holds_windows(const dd_disk_info *d)
{
    size_t i;

    if (d->kind != DD_DEV_DISK)
        return false;
    if (dd_disk_is_system(d->number))
        return true;                /* also a mirror of it, or the disk it boots from */
    if (!g.system_root[0])
        return false;
    for (i = 0; i < g.devices.nvolumes; i++) {
        const dd_volume_info *v = &g.devices.volumes[i];
        const wchar_t *m;
        if (v->parent_kind != DD_DEV_DISK || v->parent_number != d->number)
            continue;
        for (m = v->mounts; m && *m; m += wcslen(m) + 1)
            if (_wcsicmp(m, g.system_root) == 0)
                return true;
    }
    return false;
}

static void describe_disk(const dd_disk_info *d, wchar_t *buf, size_t n)
{
    wchar_t size[DD_SIZE_EXACT_CHARS], mounts[256];
    const wchar_t *name = d->product[0] ? d->product : L"unnamed device";

    if (d->media_present && d->size >= 0)
        dd_format_size_exact(d->size, true, true, size, DD_SIZE_EXACT_CHARS);
    else
        wcscpy_s(size, DD_SIZE_EXACT_CHARS, L"no media");
    disk_mounts(d, mounts, 256);
    _snwprintf_s(buf, n, _TRUNCATE, L"%ls %lu: %ls%ls%ls - %ls, %ls%ls%ls%ls",
                 d->kind == DD_DEV_CDROM ? L"Optical drive" : L"Disk", d->number,
                 d->vendor, d->vendor[0] ? L" " : L"", name,
                 size, dd_bus_name(d->bus_type),
                 mounts[0] ? L"  [" : L"", mounts, mounts[0] ? L"]" : L"");
}

static const dd_disk_info *disk_in(int id)
{
    HWND cb = GetDlgItem(g.dlg, id);
    LRESULT sel = SendMessageW(cb, CB_GETCURSEL, 0, 0);
    size_t i;

    if (sel == CB_ERR)
        return NULL;
    i = (size_t)SendMessageW(cb, CB_GETITEMDATA, (WPARAM)sel, 0);
    return i < g.devices.ndisks ? &g.devices.disks[i] : NULL;
}

/* What identifies the media in a disk: its partition table as read from it
   (not Windows' view, which changes as volumes come and go). */
typedef struct media_id {
    dd_scheme scheme;
    DWORD     signature;
    GUID      guid;
    size_t    count;
    int64_t   size;
} media_id;

static void read_media_id(const dd_disk_info *d, media_id *id)
{
    dd_layout *l = calloc(1, sizeof *l);
    wchar_t err[256];

    memset(id, 0, sizeof *id);
    id->size = d->media_present ? d->size : -1;
    if (l && dd_read_layout(d->interface_path, l, err, 256)) {
        id->scheme = l->scheme;
        id->signature = l->mbr_signature;
        id->guid = l->disk_guid;
        id->count = l->count;
    }
    free(l);
}

/* After a device change: whether the disk with this interface path still
   holds the media it held (a card swapped in the same reader keeps the
   reader's interface path). */
static bool same_disk(const dd_disk_info *before, const media_id *was)
{
    media_id now;
    size_t i;

    for (i = 0; i < g.devices.ndisks; i++) {
        const dd_disk_info *d = &g.devices.disks[i];
        if (_wcsicmp(d->interface_path, before->interface_path) != 0)
            continue;
        read_media_id(d, &now);
        return memcmp(&now, was, sizeof now) == 0;
    }
    return false;
}

/* A path that does not depend on the current folder: from a drive's root,
   or a share. */
static bool full_path(const wchar_t *p)
{
    if (iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/'))
        return true;
    return (p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/');
}

static bool need_full_path(const wchar_t *p)
{
    wchar_t msg[PATH_CHARS + 256];

    if (full_path(p))
        return true;
    _snwprintf_s(msg, sizeof msg / sizeof *msg, _TRUNCATE,
                 L"Give the image file's full path, such as D:\\Images\\card.img, not \"%ls\".", p);
    error_box(msg);
    return false;
}

/* The disk written, if a disk is written */
static const dd_disk_info *target_disk(void)
{
    return target_is_disk() ? disk_in(IDC_DISK2) : NULL;
}

/* The disk read, if a disk is read */
static const dd_disk_info *source_disk(void)
{
    return source_is_disk() ? disk_in(IDC_DISK) : NULL;
}

static void format_duration(double s, wchar_t *buf, size_t n)
{
    long t = (long)(s + 0.5);
    if (t >= 3600)
        _snwprintf_s(buf, n, _TRUNCATE, L"%ld h %02ld min", t / 3600, (t / 60) % 60);
    else if (t >= 60)
        _snwprintf_s(buf, n, _TRUNCATE, L"%ld min %02ld s", t / 60, t % 60);
    else
        _snwprintf_s(buf, n, _TRUNCATE, L"%ld s", t);
}

/* ---- state ------------------------------------------------------------- */

static bool has_text(int id)
{
    return GetWindowTextLengthW(GetDlgItem(g.dlg, id)) > 0;
}

/* Image to nothing with the hash file box ticked: only checking the image
   against its hash file, as --verify-hashes does.  Unticked, it is read
   through to /dev/null. */
static bool checking_hashes(void)
{
    return operation() == OP_CHECK && IsDlgButtonChecked(g.dlg, IDC_HASHES) == BST_CHECKED;
}

/* The options of the Reading box that copy part of the source: not for
   checking an image, or wiping a disk, which reads nothing. */
static bool reads_partly(void)
{
    op m = operation();
    return m != OP_CHECK && m != OP_WIPE;
}

/* The image read is compressed: sparse reading needs its sparse map. */
static bool source_compressed(void)
{
    return source_is_file() && reads_partly() && g.layout_ok && g.layout.compressed;
}

static void update_controls(void)
{
    static const int always[] = {
        IDC_INPUT, IDC_OUTPUT,
        IDC_FILE, IDC_BROWSE, IDC_SRCFILE, IDC_BROWSE_SRC, IDC_BS, IDC_CMDLINE,
    };
    op m = operation();
    bool idle = !g.running, ready;
    size_t i;

    for (i = 0; i < sizeof always / sizeof *always; i++)
        EnableWindow(GetDlgItem(g.dlg, always[i]), idle);
    EnableWindow(GetDlgItem(g.dlg, IDC_DISK), idle && source_is_disk());
    EnableWindow(GetDlgItem(g.dlg, IDC_DISK2), idle && target_is_disk());
    EnableWindow(GetDlgItem(g.dlg, IDC_REFRESH), idle && (source_is_disk() || target_is_disk()));
    EnableWindow(GetDlgItem(g.dlg, IDC_SHOWFIXED), idle && target_is_disk());
    EnableWindow(GetDlgItem(g.dlg, IDC_FORCE), idle && target_is_disk());
    EnableWindow(GetDlgItem(g.dlg, IDC_LOCKIN), idle && source_is_disk());
    EnableWindow(GetDlgItem(g.dlg, IDC_XZ), idle && target_is_file());   /* writing a disk: detected from the file */
    EnableWindow(GetDlgItem(g.dlg, IDC_FIT), idle && reads_partly() && g.layout_ok && g.layout.scheme != DD_SCHEME_NONE);
    EnableWindow(GetDlgItem(g.dlg, IDC_HASHES), idle && (m == OP_WRITE || m == OP_READ || m == OP_IMAGES || m == OP_CHECK));
    EnableWindow(GetDlgItem(g.dlg, IDC_VERIFY), idle && (target_is_disk() || target_is_file()));
    EnableWindow(GetDlgItem(g.dlg, IDC_SPARSE), idle && reads_partly());
    EnableWindow(GetDlgItem(g.dlg, IDC_UNCLEAN), idle && reads_partly() &&
                 IsDlgButtonChecked(g.dlg, IDC_SPARSE) == BST_CHECKED);
    /* g.fs_ok: the image has a map, for it and for Allow unclean as ticked */
    EnableWindow(GetDlgItem(g.dlg, IDC_MAKEMAP), idle && source_compressed() && !g.fs_ok);
    switch (input_kind()) {
    case SRC_DISK: ready = disk_in(IDC_DISK) != NULL; break;
    case SRC_FILE: ready = has_text(IDC_SRCFILE); break;
    default:       ready = true; break;
    }
    switch (output_kind()) {
    case DST_DISK: ready = ready && disk_in(IDC_DISK2); break;
    case DST_FILE: ready = ready && has_text(IDC_FILE); break;
    default:       break;
    }
    EnableWindow(GetDlgItem(g.dlg, IDC_START), idle && ready);
    EnableWindow(GetDlgItem(g.dlg, IDC_STOP), g.running);
}

static void load_layout(void);

/* Fills a disk list, keeping the device that was selected if it is still
   there.  A list to write to leaves out optical drives, the disk Windows
   runs from, and fixed disks unless Show fixed disks is ticked.  Returns
   how many disks it shows. */
static int fill_list(int id, bool target, const wchar_t *keep)
{
    HWND cb = GetDlgItem(g.dlg, id);
    bool fixed = IsDlgButtonChecked(g.dlg, IDC_SHOWFIXED) == BST_CHECKED;
    int select = -1, shown = 0;
    size_t i;

    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < g.devices.ndisks; i++) {
        const dd_disk_info *d = &g.devices.disks[i];
        bool windows = holds_windows(d);
        wchar_t label[512];
        LRESULT idx;

        if (target && (d->kind == DD_DEV_CDROM || windows || (!fixed && !d->removable)))
            continue;
        describe_disk(d, label, 512);
        if (windows)
            wcsncat_s(label, 512, L" - Windows", _TRUNCATE);
        idx = SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)label);
        SendMessageW(cb, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
        if (keep[0] && _wcsicmp(d->interface_path, keep) == 0)
            select = (int)idx;
        shown++;
    }
    /* never preselect a disk to write to unless it is the only choice, and
       when the disk chosen is removed, leave the list empty rather than
       show another disk's partitions in its place */
    if (select < 0 && !keep[0] && (shown == 1 || (!target && shown > 0)))
        select = 0;
    SendMessageW(cb, CB_SETCURSEL, (WPARAM)select, 0);
    return shown;
}

static void fill_disks(void)
{
    const dd_disk_info *a = disk_in(IDC_DISK), *b = disk_in(IDC_DISK2);
    wchar_t keep1[512], keep2[512];
    int first, second;

    /* Keep a selection only if it is the same device.  Disk numbers are
       reused, so a new disk often gets the number of one just removed. */
    wcscpy_s(keep1, 512, a ? a->interface_path : L"");
    wcscpy_s(keep2, 512, b ? b->interface_path : L"");
    g.refresh_pending = false;
    dd_free_device_list(&g.devices);
    dd_list_devices(&g.devices);
    first = fill_list(IDC_DISK, false, keep1);
    second = fill_list(IDC_DISK2, true, keep2);

    if (!g.running) {
        const wchar_t *none = L"No removable disks found. Insert one, or tick Show fixed disks.";
        if (target_is_disk() && second == 0)
            SetDlgItemTextW(g.dlg, IDC_STATUS, none);
        else if (source_is_disk() && first == 0)
            SetDlgItemTextW(g.dlg, IDC_STATUS, L"No disks found.");
        else
            SetDlgItemTextW(g.dlg, IDC_STATUS, L"");
    }
    load_layout();          /* the disk read may have changed */
    update_controls();
}

/* ---- partition tables --------------------------------------------------- */

/* Writing, the table is the image file's; reading, the selected disk's.
   Unless Raw is ticked, only the partitions are copied. */
static bool fitting(void)
{
    return g.layout_ok && g.layout.scheme != DD_SCHEME_NONE &&
           IsDlgButtonChecked(g.dlg, IDC_FIT) != BST_CHECKED;
}

static void show(int id, bool on)
{
    ShowWindow(GetDlgItem(g.dlg, id), on ? SW_SHOW : SW_HIDE);
}

/* The Output list: zeros and random data go only to a disk.  Keeps the
   output chosen if it is still offered. */
static void fill_outputs(void)
{
    HWND cb = GetDlgItem(g.dlg, IDC_OUTPUT);
    dst was = output_kind();
    bool wipe = input_kind() == SRC_ZERO || input_kind() == SRC_RANDOM;
    LRESULT select = 0;
    size_t i;

    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < sizeof outputs / sizeof *outputs; i++) {
        LRESULT idx;
        if (wipe && outputs[i].dst != DST_DISK)
            continue;
        idx = SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)outputs[i].label);
        SendMessageW(cb, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)outputs[i].dst);
        if (outputs[i].dst == was)
            select = idx;
    }
    SendMessageW(cb, CB_SETCURSEL, (WPARAM)select, 0);
}

/* Selects the entry of list id whose data is value. */
static void select_data(int id, int value)
{
    LRESULT i, n = SendDlgItemMessageW(g.dlg, id, CB_GETCOUNT, 0, 0);

    for (i = 0; i < n; i++)
        if ((int)SendDlgItemMessageW(g.dlg, id, CB_GETITEMDATA, (WPARAM)i, 0) == value)
            SendDlgItemMessageW(g.dlg, id, CB_SETCURSEL, (WPARAM)i, 0);
}

static void set_rows(void)
{
    /* in the order of op; the Reading and Writing boxes say the rest */
    static const wchar_t *const hashes[] = {
        L"Check the image's hash fil&e",            /* OP_WRITE */
        L"Write a hash fil&e",                      /* OP_READ */
        L"Check the image's hash fil&e",            /* OP_CHECK: unticked, only read it */
        L"Check and write hash fil&es",             /* OP_IMAGES */
        L"",                                        /* OP_DISKS: hidden, disks have no hash files */
        L"",                                        /* OP_WIPE */
        L"",                                        /* OP_TEST */
    };
    op m = operation();
    src s = input_kind();
    dst d = output_kind();

    SetDlgItemTextW(g.dlg, IDC_HASHES, hashes[m]);
    /* each row holds a disk list, a file box, or a note, as its list says */
    show(IDC_DISK, s == SRC_DISK);
    show(IDC_SRCFILE, s == SRC_FILE);
    show(IDC_BROWSE_SRC, s == SRC_FILE);
    show(IDC_SRCNOTE, s == SRC_ZERO || s == SRC_RANDOM);
    SetDlgItemTextW(g.dlg, IDC_SRCNOTE, s == SRC_ZERO ? L"Every byte written is zero."
                    : L"Every byte written comes from the system's random number generator.");
    show(IDC_DISK2, d == DST_DISK);
    show(IDC_FILE, d == DST_FILE);
    show(IDC_BROWSE, d == DST_FILE);
    show(IDC_DSTNOTE, d == DST_NULL);
    SetDlgItemTextW(g.dlg, IDC_DSTNOTE, L"What is read is discarded: nothing is written.");
    show(IDC_HASHES, hashes[m][0] != 0);
    show(IDC_SPARSE, reads_partly());
    show(IDC_UNCLEAN, reads_partly());
    show(IDC_MAKEMAP, s == SRC_FILE && reads_partly());
}

/* ---- sparse reading --------------------------------------------------------- */

static bool sparse_ticked(void)
{
    return reads_partly() && IsDlgButtonChecked(g.dlg, IDC_SPARSE) == BST_CHECKED;
}

static bool unclean_ticked(void)
{
    return sparse_ticked() && IsDlgButtonChecked(g.dlg, IDC_UNCLEAN) == BST_CHECKED;
}

/* The report's entry for partition number (0: a disk without a table). */
static const dd_fs_volume *fs_volume(int number)
{
    size_t i;

    for (i = 0; g.fs_ok && i < g.fs->count; i++)
        if (g.fs->vols[i].partition == number)
            return &g.fs->vols[i];
    return NULL;
}

/* A FAT or exFAT volume read whole only because it is marked dirty: on a
   card Windows has mounted, that mark may be Windows' own (see below). */
static bool fat_marked_dirty(const dd_fs_volume *v)
{
    return v->unclean && !v->sparse && (wcsncmp(v->type, L"FAT", 3) == 0 || wcscmp(v->type, L"exFAT") == 0);
}

/* The "Sparse read" column: can sparse reading map this partition? */
static void sparse_cell(const dd_partition *p, wchar_t *buf, size_t n)
{
    const dd_fs_volume *v = fs_volume(p->number);
    wchar_t used[32];

    if (p->extended) {
        wcscpy_s(buf, n, L"-");
        return;
    }
    if (!g.fs_ok) {
        wcscpy_s(buf, n, g.layout_ok && g.layout.compressed ? L"? Mapped when you start" : L"");
        return;
    }
    if (!v) {
        buf[0] = 0;
        return;
    }
    dd_format_size(v->used, true, used, 32);
    if (v->sparse)
        _snwprintf_s(buf, n, _TRUNCATE, L"Yes: %ls, reads %ls", v->type, used);
    else if (v->unclean)
        _snwprintf_s(buf, n, _TRUNCATE, L"No: %ls, not cleanly unmounted", v->type);
    else if (v->type[0])
        _snwprintf_s(buf, n, _TRUNCATE, L"No: %ls, %ls", v->type, v->size > 0 ? v->note : L"outside the disk");
    else
        wcscpy_s(buf, n, v->size > 0 ? L"No: not FAT, exFAT, NTFS, ext or swap" : L"No: outside the disk");
}

/* Reading: what sparse reading would make of the selected disk. */
static void load_fs(const wchar_t *disk)
{
    wchar_t err[512];

    g.fs_ok = g.fs && disk && dd_fs_report_read(disk, unclean_ticked(), g.fs, err, 512);
}

/* "disk.img.hashes", or a note that there is none */
static void hash_file_note(const wchar_t *image, wchar_t *buf, size_t n)
{
    wchar_t path[PATH_CHARS + 16];

    buf[0] = 0;
    if (!dd_hash_file_name(image, path, sizeof path / sizeof *path))
        return;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        _snwprintf_s(buf, n, _TRUNCATE, L"No hash file (%ls).", path);
    else
        _snwprintf_s(buf, n, _TRUNCATE, L"Hash file: %ls.", path);
}

/* ---- compression ---------------------------------------------------------- */

/* The level chosen for reading, -1 for none. */
static int xz_level(void)
{
    LRESULT i = SendDlgItemMessageW(g.dlg, IDC_XZ, CB_GETCURSEL, 0, 0);
    return i == CB_ERR ? -1 : (int)SendDlgItemMessageW(g.dlg, IDC_XZ, CB_GETITEMDATA, (WPARAM)i, 0);
}

static void init_xz_list(void)
{
    HWND cb = GetDlgItem(g.dlg, IDC_XZ);
    wchar_t label[128], dict[32];
    LRESULT idx;
    int level;

    idx = SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"None: a raw, uncompressed image");
    SendMessageW(cb, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)-1);
    for (level = 0; level <= 9; level++) {
        dd_format_size(dd_xz_dictionary(level), false, dict, 32);
        _snwprintf_s(label, 128, _TRUNCATE, L"xz level %d - %ls dictionary%ls", level, dict,
                     level == 1 ? L" (fastest)" : level == 6 ? L" (smaller, slower)"
                     : level == 9 ? (DD_XZ_DEFAULT_LEVEL == 9 ? L" (smallest, slow; default)" : L" (smallest, slow)")
                     : level == DD_XZ_DEFAULT_LEVEL ? L" (default)" : L"");
        idx = SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)label);
        SendMessageW(cb, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)level);
    }
    SendMessageW(cb, CB_SETCURSEL, DD_XZ_DEFAULT_LEVEL + 1, 0);
}

static bool has_xz_suffix(const wchar_t *s)
{
    size_t n = wcslen(s);
    return n > 3 && _wcsicmp(s + n - 3, L".xz") == 0;
}

/* Reading: the file name ends in .xz exactly when the image is compressed. */
static void fix_extension(void)
{
    wchar_t file[PATH_CHARS];
    size_t n;

    if (!target_is_file())
        return;
    GetDlgItemTextW(g.dlg, IDC_FILE, file, PATH_CHARS);
    n = wcslen(file);
    if (n == 0)
        return;
    if (xz_level() >= 0 && !has_xz_suffix(file))
        wcsncat_s(file, PATH_CHARS, L".xz", _TRUNCATE);
    else if (xz_level() < 0 && has_xz_suffix(file))
        file[n - 3] = 0;
    else
        return;
    SetDlgItemTextW(g.dlg, IDC_FILE, file);
}

static void table_summary(const dd_layout *l, const wchar_t *what, wchar_t *text, size_t n)
{
    wchar_t end[DD_SIZE_EXACT_CHARS], all[DD_SIZE_EXACT_CHARS], packed[DD_SIZE_EXACT_CHARS];

    dd_format_size_exact(l->data_end, true, true, end, DD_SIZE_EXACT_CHARS);
    dd_format_size_exact(l->image_size, true, true, all, DD_SIZE_EXACT_CHARS);
    text[0] = 0;
    if (l->compressed) {
        dd_format_size_exact(l->file_size, true, true, packed, DD_SIZE_EXACT_CHARS);
        _snwprintf_s(text, n, _TRUNCATE, L"xz-compressed: %ls, %ls uncompressed. ", packed, all);
    }
    if (l->scheme == DD_SCHEME_NONE)
        _snwprintf_s(text + wcslen(text), n - wcslen(text), _TRUNCATE, L"No partition table on this %ls %ls.",
                     all, what);
    else
        _snwprintf_s(text + wcslen(text), n - wcslen(text), _TRUNCATE,
                     L"%ls, %zu partition%ls, %lu-byte sectors. Data ends at %ls of this %ls %ls.",
                     l->scheme == DD_SCHEME_GPT ? L"GPT" : L"MBR", l->count, l->count == 1 ? L"" : L"s",
                     l->sector_size, end, all, what);
    if (l->note[0]) {
        wcsncat_s(text, n, L" Note: ", _TRUNCATE);
        wcsncat_s(text, n, l->note, _TRUNCATE);
        wcsncat_s(text, n, L".", _TRUNCATE);
    }
}

/* With Only used space ticked: how much of the source is read. */
static void sparse_summary(wchar_t *text, size_t n)
{
    const dd_layout *l = &g.layout;
    wchar_t size[DD_SIZE_EXACT_CHARS];
    size_t i;
    bool dirty = false;

    if (!sparse_ticked())
        return;
    if (!g.fs_ok) {
        if (g.layout_ok && g.layout.compressed)
            wcsncat_s(text, n, L"\nThe image is compressed, so it is read in order: Start maps its file systems"
                               L" first, or Make sparse map does now.", _TRUNCATE);
        return;
    }
    {
        /* everything past the data end is outside any file system, so all
           of it counts as read: --fit leaves it out */
        int64_t read = g.fs->read, total = g.fs->size;
        if (fitting() && l->data_end < total) {
            read -= total - l->data_end;
            total = l->data_end;
        }
        dd_format_size_exact(read, true, true, size, DD_SIZE_EXACT_CHARS);
        _snwprintf_s(text + wcslen(text), n - wcslen(text), _TRUNCATE,
                     L"\nOnly the space in use is %ls: %ls; %ls.",
                     l->compressed ? L"kept" : L"read", size,
                     operation() == OP_TEST ? L"free space is not read" : L"free space is written as zeros");
    }
    for (i = 0; i < g.fs->count; i++)
        dirty = dirty || fat_marked_dirty(&g.fs->vols[i]);
    if (dirty && source_is_disk())
        wcsncat_s(text, n, L" A FAT volume Windows has mounted is marked not clean until Windows writes"
                           L" its changes: tick Lock to have them written first.", _TRUNCATE);
}

/* Reading a disk, or copying an image: how big the new image will be.
   Testing a disk: how much of it is read. */
static void show_read_fit(void)
{
    const dd_layout *l = &g.layout;
    bool disk = source_is_disk();
    wchar_t text[1024], size[DD_SIZE_EXACT_CHARS];

    if ((disk && !source_disk()) || !g.layout_ok) {
        SetDlgItemTextW(g.dlg, IDC_LAYOUT, g.layout_err);
        return;
    }
    table_summary(l, disk ? L"disk" : L"image", text, 1024);
    wcsncat_s(text, 1024, L"\n", _TRUNCATE);
    if (operation() == OP_TEST) {
        bool fit = fitting() && l->data_end < l->image_size;
        dd_format_size_exact(fit ? l->data_end : l->image_size, true, true, size, DD_SIZE_EXACT_CHARS);
        _snwprintf_s(text + wcslen(text), 1024 - wcslen(text), _TRUNCATE,
                     L"%ls, %ls, will be read to test that it can be; nothing is written.",
                     fit ? L"The partitions" : L"The whole disk", size);
        sparse_summary(text, 1024);
        SetDlgItemTextW(g.dlg, IDC_LAYOUT, text);
        return;
    }
    if (fitting()) {
        dd_format_size_exact(dd_layout_needed(l), true, true, size, DD_SIZE_EXACT_CHARS);
        _snwprintf_s(text + wcslen(text), 1024 - wcslen(text), _TRUNCATE,
                     L"The image will hold %ls: the partitions%ls", size,
                     l->scheme == DD_SCHEME_GPT ? L" and a new backup GPT" : L"");
    } else {
        dd_format_size_exact(l->image_size, true, true, size, DD_SIZE_EXACT_CHARS);
        _snwprintf_s(text + wcslen(text), 1024 - wcslen(text), _TRUNCATE,
                     disk ? L"The whole disk, %ls, will be read" : L"The whole image, %ls, will be copied", size);
    }
    if (xz_level() >= 0)
        _snwprintf_s(text + wcslen(text), 1024 - wcslen(text), _TRUNCATE,
                     L", compressed with xz level %d.", xz_level());
    else
        wcsncat_s(text, 1024, L", uncompressed.", _TRUNCATE);
    sparse_summary(text, 1024);
    SetDlgItemTextW(g.dlg, IDC_LAYOUT, text);
}

/* The summary above the partition list: what the table says, and whether
   the source fits the disk written (writing an image, copying a disk) or
   how big the new image will be (reading a disk, copying an image). */
static void show_fit(void)
{
    const dd_layout *l = &g.layout;
    const dd_disk_info *d = target_disk();
    op m = operation();
    wchar_t text[1024], need[DD_SIZE_EXACT_CHARS], disk[DD_SIZE_EXACT_CHARS], put[DD_SIZE_EXACT_CHARS];
    int64_t needed, written;

    if (m == OP_WIPE) {
        const wchar_t *with = input_kind() == SRC_ZERO ? L"zeros" : L"random data";
        if (!d)
            _snwprintf_s(text, 1024, _TRUNCATE, L"Choose the disk to overwrite with %ls.", with);
        else if (!d->media_present || d->size < 0)
            _snwprintf_s(text, 1024, _TRUNCATE, L"There is no media in disk %lu.", d->number);
        else {
            dd_format_size_exact(d->size, true, true, disk, DD_SIZE_EXACT_CHARS);
            _snwprintf_s(text, 1024, _TRUNCATE,
                         L"All of disk %lu, %ls, will be overwritten with %ls: its partitions and everything"
                         L" on them are erased.", d->number, disk, with);
        }
        SetDlgItemTextW(g.dlg, IDC_LAYOUT, text);
        return;
    }
    if (target_is_file() || m == OP_TEST) {
        show_read_fit();
        return;
    }
    if (!g.layout_ok) {
        SetDlgItemTextW(g.dlg, IDC_LAYOUT, g.layout_err);
        return;
    }
    table_summary(l, m == OP_DISKS ? L"disk" : L"image", text, 1024);
    if (m == OP_CHECK) {
        wchar_t file[PATH_CHARS], note[PATH_CHARS + 64];
        GetDlgItemTextW(g.dlg, IDC_SRCFILE, file, PATH_CHARS);
        if (checking_hashes())
            hash_file_note(file, note, sizeof note / sizeof *note);
        else
            _snwprintf_s(note, sizeof note / sizeof *note, _TRUNCATE,
                         L"All of it will be read%ls to test that it can be; nothing is written.",
                         l->compressed ? L" and decompressed" : L"");
        wcsncat_s(text, 1024, L"\n", _TRUNCATE);
        wcsncat_s(text, 1024, note, _TRUNCATE);
        SetDlgItemTextW(g.dlg, IDC_LAYOUT, text);
        return;
    }

    if (d && d->kind == DD_DEV_DISK && d->size >= 0) {
        bool fit = fitting();
        needed = fit ? dd_layout_needed(l) : l->image_size;
        written = fit && l->data_end < l->image_size ? l->data_end : l->image_size;
        dd_format_size_exact(d->size, true, true, disk, DD_SIZE_EXACT_CHARS);
        dd_format_size_exact(needed, true, true, need, DD_SIZE_EXACT_CHARS);
        dd_format_size_exact(written, true, true, put, DD_SIZE_EXACT_CHARS);
        wcsncat_s(text, 1024, L"\n", _TRUNCATE);
        if (needed <= d->size)
            _snwprintf_s(text + wcslen(text), 1024 - wcslen(text), _TRUNCATE,
                         L"Fits disk %lu, %ls: %ls will be written.", d->number, disk, put);
        else
            _snwprintf_s(text + wcslen(text), 1024 - wcslen(text), _TRUNCATE,
                         L"Too big for disk %lu, %ls: needs %ls.%ls", d->number, disk, need,
                         !fit && l->scheme != DD_SCHEME_NONE && dd_layout_needed(l) <= d->size
                             ? L" Its partitions fit: untick Raw." : L"");
    }
    sparse_summary(text, 1024);
    SetDlgItemTextW(g.dlg, IDC_LAYOUT, text);
}

#define PART_COLUMNS 6

static void add_row(HWND lv, int row, const wchar_t *cols[PART_COLUMNS])
{
    LVITEMW it;
    int c;

    memset(&it, 0, sizeof it);
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.pszText = (wchar_t *)cols[0];
    row = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
    for (c = 1; c < PART_COLUMNS; c++) {
        it.iSubItem = c;
        it.pszText = (wchar_t *)cols[c];
        SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
    }
}

/* Reads the partition table of what is read - the image file (writing,
   checking, copying images) or the disk (reading, copying disks) - and
   shows it. */
static void load_layout(void)
{
    HWND lv = GetDlgItem(g.dlg, IDC_PARTS);
    const dd_disk_info *d = source_disk();
    wchar_t file[PATH_CHARS];
    op m = operation();
    size_t i;

    SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);
    g.layout_ok = false;
    g.layout_err[0] = 0;
    g.fs_ok = false;
    file[0] = 0;
    if (d) {
        if (d->media_present && d->interface_path[0]) {
            wcscpy_s(file, PATH_CHARS, d->interface_path);
            g.layout_ok = dd_read_layout(file, &g.layout, g.layout_err, 512);
        }
    } else if (source_is_file()) {
        GetDlgItemTextW(g.dlg, IDC_SRCFILE, file, PATH_CHARS);
        if (file[0]) {
            DWORD a = GetFileAttributesW(file);
            /* nothing while a typed name is incomplete */
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
                g.layout_ok = dd_read_layout(file, &g.layout, g.layout_err, 512);
        }
    }
    if (g.layout_ok && m != OP_CHECK)
        load_fs(file);
    for (i = 0; g.layout_ok && i < g.layout.count; i++) {
        const dd_partition *p = &g.layout.parts[i];
        wchar_t num[16], start[DD_SIZE_EXACT_CHARS], size[DD_SIZE_EXACT_CHARS], type[64], sparse[128];
        const wchar_t *cols[PART_COLUMNS];

        _snwprintf_s(num, 16, _TRUNCATE, L"%d", p->number);
        dd_format_size_exact(p->start, true, true, start, DD_SIZE_EXACT_CHARS);
        dd_format_size_exact(p->size, true, true, size, DD_SIZE_EXACT_CHARS);
        dd_partition_type_name(&g.layout, p, type, 64);
        cols[0] = num;
        cols[1] = start;
        cols[2] = size;
        cols[3] = type;
        cols[4] = g.layout.scheme == DD_SCHEME_GPT ? p->name
                : p->logical ? L"logical" : p->extended ? L"holds the logical partitions"
                : p->bootable ? L"active" : L"";
        sparse[0] = 0;
        if (m != OP_CHECK)
            sparse_cell(p, sparse, 128);
        cols[5] = sparse;
        add_row(lv, (int)i, cols);
    }
    show_fit();
    update_controls();
}

static void init_partition_list(void)
{
    /* widths in dialog units; the list is 506 wide */
    static const struct { const wchar_t *title; int width; } cols[PART_COLUMNS] = {
        { L"#", 14 }, { L"Start", 92 }, { L"Size", 92 }, { L"Type", 76 }, { L"Name", 78 }, { L"Sparse read", 144 },
    };
    HWND lv = GetDlgItem(g.dlg, IDC_PARTS);
    LVCOLUMNW c;
    RECT r = { 0, 0, 4, 8 };
    int i;

    MapDialogRect(g.dlg, &r);       /* dialog units to pixels at this DPI */
    SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    memset(&c, 0, sizeof c);
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    for (i = 0; i < (int)(sizeof cols / sizeof *cols); i++) {
        c.pszText = (wchar_t *)cols[i].title;
        c.cx = MulDiv(cols[i].width, r.right, 4);
        c.iSubItem = i;
        SendMessageW(lv, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&c);
    }
}

/* ---- the copy ---------------------------------------------------------- */

static void cb_message(void *ctx, dd_msg_kind kind, const wchar_t *text)
{
    wchar_t *copy = _wcsdup(text);
    (void)ctx;
    if (copy && !PostMessageW(g.dlg, WM_APP_MESSAGE, (WPARAM)kind, (LPARAM)copy))
        free(copy);
}

static void cb_progress(void *ctx, const dd_stats *s)
{
    dd_stats *copy = malloc(sizeof *copy);
    (void)ctx;
    if (!copy)
        return;
    *copy = *s;
    if (!PostMessageW(g.dlg, WM_APP_PROGRESS, 0, (LPARAM)copy))
        free(copy);
}

static DWORD WINAPI worker(void *arg)
{
    dd_callbacks cb;
    dd_stats *st = calloc(1, sizeof *st);
    dd_result r;

    (void)arg;
    memset(&cb, 0, sizeof cb);
    cb.message = cb_message;
    cb.progress = cb_progress;
    cb.progress_interval_ms = 250;
    r = g.task == TASK_MAP ? dd_make_sparse_map(g.map_input, g.map_unclean, &cb, &g.cancel, st, NULL)
                           : dd_run(&g.opt, &cb, &g.cancel, st);
    PostMessageW(g.dlg, WM_APP_DONE, (WPARAM)r, (LPARAM)st);
    return 0;
}

/* ---- the progress bar ---------------------------------------------------- */

/* Compressing, the encoder takes in a block per thread before it compresses
   it, so what is read runs ahead of what is compressed - by gigabytes with
   many threads - and a bar of what is read would sit full while the last
   blocks are compressed.  The bar is solid up to its position, what is
   done, and pale on from there to g.bar_ahead, what is read.  While it
   is both, each part is labelled, as long as a label fits. */

/* Writes the first of labels that fits in [x0, x1) of the bar, in black or
   white, whichever shows better against the fill there. */
static void label_bar(HDC dc, const DWORD *px, int stride, const RECT *in, int x0, int x1,
                      const wchar_t *const *labels, int n)
{
    RECT t = { x0, in->top, x1, in->bottom };
    DWORD c;
    SIZE sz;
    int i, luma;

    if (x1 - x0 < 8)
        return;
    c = px[((in->top + in->bottom) / 2) * stride + (x0 + x1) / 2];
    luma = (int)(((c >> 16) & 0xFF) * 299 + ((c >> 8) & 0xFF) * 587 + (c & 0xFF) * 114) / 1000;
    SetTextColor(dc, luma > 140 ? RGB(0, 0, 0) : RGB(255, 255, 255));
    for (i = 0; i < n; i++) {
        int len = (int)wcslen(labels[i]);
        if (GetTextExtentPoint32W(dc, labels[i], len, &sz) && sz.cx + 8 <= x1 - x0) {
            DrawTextW(dc, labels[i], len, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            return;
        }
    }
}

static void paint_bar(HWND w, HDC dc)
{
    RECT r, in;
    BITMAPINFO bi;
    DWORD *px;
    HBITMAP bmp;
    HGDIOBJ old;
    HDC mem;
    HTHEME th;
    int pos = (int)SendMessageW(w, PBM_GETPOS, 0, 0);
    int state = (int)SendMessageW(w, PBM_GETSTATE, 0, 0);
    int ahead = g.bar_ahead > pos ? g.bar_ahead : pos;
    int done_x, ahead_x, x, y;

    GetClientRect(w, &r);
    if (r.right <= 0 || r.bottom <= 0)
        return;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = r.right;
    bi.bmiHeader.biHeight = -r.bottom;      /* top down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    mem = CreateCompatibleDC(dc);
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!mem || !bmp) {
        if (bmp)
            DeleteObject(bmp);
        if (mem)
            DeleteDC(mem);
        return;
    }
    old = SelectObject(mem, bmp);

    th = OpenThemeData(w, L"PROGRESS");
    if (th) {
        DrawThemeParentBackground(w, mem, &r);
        DrawThemeBackground(th, mem, PP_BAR, 0, &r, NULL);
        if (FAILED(GetThemeBackgroundContentRect(th, mem, PP_BAR, 0, &r, &in)))
            in = r;
    } else {
        FillRect(mem, &r, GetSysColorBrush(COLOR_BTNFACE));
        DrawEdge(mem, &r, EDGE_SUNKEN, BF_RECT);
        in = r;
        InflateRect(&in, -2, -2);
    }
    done_x = in.left + MulDiv(in.right - in.left, pos, 1000);
    ahead_x = in.left + MulDiv(in.right - in.left, ahead, 1000);
    if (ahead_x > in.left) {
        RECT f = in;
        f.right = ahead_x;
        if (th)     /* PBST_NORMAL, _ERROR and _PAUSED are PBFS_ of the same numbers */
            DrawThemeBackground(th, mem, PP_FILL, state >= PBST_NORMAL && state <= PBST_PAUSED ? state : PBFS_NORMAL,
                                &f, NULL);
        else
            FillRect(mem, &f, GetSysColorBrush(COLOR_HIGHLIGHT));
    }
    if (th)
        CloseThemeData(th);

    /* what is read but not yet done: the fill, most of the way to white */
    GdiFlush();
    for (y = in.top; y < in.bottom; y++)
        for (x = done_x; x < ahead_x; x++) {
            DWORD c = px[y * r.right + x];
            DWORD b = c & 0xFF, gr = (c >> 8) & 0xFF, rd = (c >> 16) & 0xFF;
            b += (255 - b) * 3 / 5;
            gr += (255 - gr) * 3 / 5;
            rd += (255 - rd) * 3 / 5;
            px[y * r.right + x] = (c & 0xFF000000) | (rd << 16) | (gr << 8) | b;
        }

    if (ahead_x > done_x) {
        static const wchar_t *const done_labels[] = { L"Compressed", L"Done" };
        static const wchar_t *const ahead_labels[] = { L"Read, not yet compressed", L"Read" };
        HGDIOBJ font = SelectObject(mem, (HFONT)SendMessageW(GetParent(w), WM_GETFONT, 0, 0));
        SetBkMode(mem, TRANSPARENT);
        label_bar(mem, px, r.right, &in, in.left, done_x, done_labels, 2);
        label_bar(mem, px, r.right, &in, done_x, ahead_x, ahead_labels, 2);
        SelectObject(mem, font);
    }

    BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK bar_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    PAINTSTRUCT ps;

    (void)ref;
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_bar(w, BeginPaint(w, &ps));
        EndPaint(w, &ps);
        return 0;
    case WM_PRINTCLIENT:
        paint_bar(w, (HDC)wp);
        return 0;
    case WM_NCDESTROY:
        RemoveWindowSubclass(w, bar_proc, id);
        break;
    }
    return DefSubclassProc(w, msg, wp, lp);
}

/* done and ahead in thousandths: solid to done, pale on to ahead */
static void set_bar(int done, int ahead)
{
    HWND bar = GetDlgItem(g.dlg, IDC_PROGRESS);
    g.bar_ahead = ahead;
    SendMessageW(bar, PBM_SETPOS, (WPARAM)done, 0);
    InvalidateRect(bar, NULL, FALSE);   /* SETPOS repaints only when done moves */
}

/* Verifying the copy, checking an image's hashes, or making a sparse map */
static void show_phase(const dd_stats *s)
{
    const wchar_t *what = s->phase == DD_PHASE_VERIFY ? L"Verifying"
                        : s->phase == DD_PHASE_MAP ? (s->phase_total > 0 ? L"Hashing" : L"Mapping")
                        : L"Checking";
    wchar_t line[256], eta[256], left[64], title[96], done[DD_SIZE_EXACT_CHARS];
    int percent = -1;

    dd_format_size_exact(s->phase_done, true, true, done, DD_SIZE_EXACT_CHARS);
    _snwprintf_s(line, 256, _TRUNCATE, L"%ls: %ls %ls", what, done,
                 s->phase == DD_PHASE_MAP && s->phase_total <= 0 ? L"decompressed" : L"read");
    SetDlgItemTextW(g.dlg, IDC_STATUS, line);
    eta[0] = 0;
    if (s->phase_total > 0) {
        percent = (int)(s->phase_done * 100 / s->phase_total);
        set_bar((int)(s->phase_done * 1000 / s->phase_total), 0);
        if (s->phase_done > 0 && s->phase_elapsed > 0) {
            double rate = s->phase_done / s->phase_elapsed;
            format_duration((double)(s->phase_total - s->phase_done) / rate, left, 64);
            _snwprintf_s(eta, 256, _TRUNCATE, L"%ls: %d%% done, about %ls left", what, percent, left);
        }
    }
    SetDlgItemTextW(g.dlg, IDC_ETA, eta);
    if (percent >= 0) {
        _snwprintf_s(title, 96, _TRUNCATE, L"%ls %d%% - " APP_NAME, what, percent);
        SetWindowTextW(g.dlg, title);
    }
}

static void show_progress(const dd_stats *s)
{
    wchar_t line[256], eta[256], left[64], title[96], xz[96];
    int percent = -1;
    /* compressing, what is done is what the encoder has compressed */
    bool lags = s->xz_done >= 0 && s->xz_done < s->bytes_written;
    int64_t done = lags ? s->xz_done : s->bytes_written;

    if (s->phase != DD_PHASE_COPY) {
        show_phase(s);
        return;
    }
    dd_format_transfer(s, true, line, 256);
    dd_format_xz(s, xz, 96);
    if (xz[0]) {
        wcsncat_s(line, 256, L"; ", _TRUNCATE);
        wcsncat_s(line, 256, xz, _TRUNCATE);
    }
    SetDlgItemTextW(g.dlg, IDC_STATUS, line);
    eta[0] = 0;
    if (s->bytes_total > 0) {
        int read = (int)(s->bytes_written * 100 / s->bytes_total);
        percent = (int)(done * 100 / s->bytes_total);
        set_bar((int)(done * 1000 / s->bytes_total), (int)(s->bytes_written * 1000 / s->bytes_total));
        if (done > 0 && s->elapsed > 0) {
            double rate = done / s->elapsed;
            format_duration((double)(s->bytes_total - done) / rate, left, 64);
            if (lags && s->bytes_written >= s->bytes_total)
                _snwprintf_s(eta, 256, _TRUNCATE, L"All read; compressing the rest: %d%% done, about %ls left",
                             percent, left);
            else if (lags && read > percent)
                _snwprintf_s(eta, 256, _TRUNCATE, L"%d%% done (%d%% read), about %ls left", percent, read, left);
            else
                _snwprintf_s(eta, 256, _TRUNCATE, L"%d%% done, about %ls left", percent, left);
        }
    }
    SetDlgItemTextW(g.dlg, IDC_ETA, eta);
    if (percent >= 0) {
        _snwprintf_s(title, 96, _TRUNCATE, L"%d%% - " APP_NAME, percent);
        SetWindowTextW(g.dlg, title);
    }
}

/* MessageBox runs a message loop, so device-change refreshes would rebuild
   g.devices under the caller.  They are held back until the box closes. */
static int confirm(const wchar_t *text, const wchar_t *title)
{
    int r;

    g.confirming = true;
    r = MessageBoxW(g.dlg, text, title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    g.confirming = false;
    return r;
}

static void run_worker(void)
{
    SendDlgItemMessageW(g.dlg, IDC_PROGRESS, PBM_SETSTATE, PBST_NORMAL, 0);
    set_bar(0, 0);
    SetDlgItemTextW(g.dlg, IDC_STATUS, L"Starting...");
    SetDlgItemTextW(g.dlg, IDC_ETA, L"");
    dd_cancel_init(&g.cancel);
    g.verified = g.opt.verify;
    g.close_when_done = false;
    g.running = true;
    g.worker = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!g.worker) {
        g.running = false;
        error_box(L"Could not start the copy.");
    }
    update_controls();
}

static bool ticked(int id)
{
    return IsDlgButtonChecked(g.dlg, id) == BST_CHECKED;
}

/* The options the controls ask for, apart from the paths.  Start and the
   "Command line" button both use this, so the command shown is the copy
   that Start runs.  target is the disk written, if any. */
static void options_from_controls(dd_options *o, const dd_disk_info *target)
{
    LRESULT bsel = SendDlgItemMessageW(g.dlg, IDC_BS, CB_GETCURSEL, 0, 0);
    op m = operation();

    dd_options_init(o);
    o->bs = block_sizes[bsel == CB_ERR ? DEFAULT_BLOCK_SIZE : bsel].size;
    if (checking_hashes()) {
        o->check_hashes = true;
        return;
    }
    if (m == OP_CHECK) {                /* read to the end, nothing more */
        o->hashes = DD_HASHES_OFF;
        return;
    }
    o->fit = fitting();
    o->verify = ticked(IDC_VERIFY) && output_kind() != DST_NULL;
    o->sparse_read = sparse_ticked();
    o->sparse_unclean = unclean_ticked();
    if (target_is_disk()) {
        /* the engine checks again that the output is the kind of disk shown */
        o->filter = target && target->removable ? DD_FILTER_REMOVABLE : DD_FILTER_DISK;
        o->force_dismount = ticked(IDC_FORCE);
    } else if (target_is_file()) {
        o->xz_level = xz_level();
        o->filter = DD_FILTER_FILE;
    }
    if (source_is_disk())
        o->lock_input = ticked(IDC_LOCKIN);
    /* zeros and random data never end: as much as the disk holds */
    if (m == OP_WIPE && target && target->size > 0) {
        o->count = target->size;
        o->iflags |= DD_FLAG_COUNT_BYTES;
    }
    /* a read test reads what --fit would, up to the end of the partitions;
       --fit itself also rebuilds a GPT's backup, which needs an output */
    if (m == OP_TEST && o->fit) {
        o->fit = false;
        o->count = g.layout.data_end;
        o->iflags |= DD_FLAG_COUNT_BYTES;
    }
    switch (m) {
    case OP_WRITE:
        o->hashes = ticked(IDC_HASHES) ? DD_HASHES_AUTO : DD_HASHES_OFF;
        break;
    case OP_DISKS:
    case OP_WIPE:
    case OP_TEST:
        o->hashes = DD_HASHES_AUTO;     /* no image file: nothing to write or check */
        break;
    default:
        o->hashes = ticked(IDC_HASHES) ? DD_HASHES_ON : DD_HASHES_OFF;
        break;
    }
}

/* " name=value", quoted when the value has a space */
static void add_arg(wchar_t *buf, size_t n, const wchar_t *name, const wchar_t *value)
{
    bool quote = wcschr(value, L' ') != NULL;
    size_t len = wcslen(buf);

    _snwprintf_s(buf + len, n - len, _TRUNCATE, L" %ls=%ls%ls%ls", name, quote ? L"\"" : L"", value,
                 quote ? L"\"" : L"");
}

/* The dd.exe command line for options o.  Disks are named \\.\PhysicalDriveN,
   which is the same disk as long as nothing is plugged in or removed. */
static void format_command(const dd_options *o, const wchar_t *in, const wchar_t *out, wchar_t *buf, size_t n)
{
    wchar_t bs[32];
    size_t len;

    wcscpy_s(buf, n, L"dd");
    if (in)
        add_arg(buf, n, L"if", in);
    if (out)
        add_arg(buf, n, L"of", out);
    if (o->bs % 1048576 == 0)
        _snwprintf_s(bs, 32, _TRUNCATE, L"%lldM", (long long)(o->bs / 1048576));
    else if (o->bs % 1024 == 0)
        _snwprintf_s(bs, 32, _TRUNCATE, L"%lldK", (long long)(o->bs / 1024));
    else
        _snwprintf_s(bs, 32, _TRUNCATE, L"%lld", (long long)o->bs);
    add_arg(buf, n, L"bs", bs);
    if (o->count >= 0) {
        len = wcslen(buf);
        _snwprintf_s(buf + len, n - len, _TRUNCATE, L" count=%lld%ls", (long long)o->count,
                     o->iflags & DD_FLAG_COUNT_BYTES ? L" iflag=count_bytes" : L"");
    }
    len = wcslen(buf);
    _snwprintf_s(buf + len, n - len, _TRUNCATE, L"%ls%ls%ls%ls%ls%ls%ls%ls%ls%ls%ls",
                 o->check_hashes ? L" --verify-hashes" : L"",
                 o->fit ? L" --fit" : L"",
                 o->filter != DD_FILTER_NONE ? L" --filter=" : L"",
                 o->filter != DD_FILTER_NONE ? dd_filter_name(o->filter) : L"",
                 o->force_dismount ? L" --force-dismount" : L"",
                 o->lock_input ? L" --lock-input" : L"",
                 o->sparse_read ? L" --sparse-read" : L"",
                 o->sparse_unclean ? L" --allow-unclean" : L"",
                 o->hashes == DD_HASHES_ON ? L" --hashes" : o->hashes == DD_HASHES_OFF ? L" --no-hashes" : L"",
                 o->verify ? L" --verify" : L"",
                 L" --progress");
    if (o->xz_level >= 0) {
        len = wcslen(buf);
        _snwprintf_s(buf + len, n - len, _TRUNCATE, L" --xz=%d", o->xz_level);
    }
}

/* The /dev/ name of what the Input row gives that is not a disk or file */
static const wchar_t *special_input(void)
{
    return input_kind() == SRC_ZERO ? L"/dev/zero" : L"/dev/random";
}

/* "Command line": what Start would run, as a dd.exe command, in the log. */
static void show_command(void)
{
    const dd_disk_info *a = source_disk(), *b = target_disk();
    wchar_t file[PATH_CHARS], src[PATH_CHARS], cmd[2 * PATH_CHARS + 256];
    const wchar_t *in, *out;
    dd_options o;

    if (g.running)
        return;
    fix_extension();        /* as Start does: .xz exactly when compressing */
    load_layout();          /* and from the partition table as it is now */
    GetDlgItemTextW(g.dlg, IDC_FILE, file, PATH_CHARS);
    GetDlgItemTextW(g.dlg, IDC_SRCFILE, src, PATH_CHARS);
    switch (input_kind()) {
    case SRC_DISK: in = a ? a->path : NULL; break;
    case SRC_FILE: in = src[0] ? src : NULL; break;
    default:       in = special_input(); break;
    }
    switch (output_kind()) {
    case DST_DISK: out = b ? b->path : NULL; break;
    case DST_FILE: out = file[0] ? file : NULL; break;
    default:       out = checking_hashes() ? L"" : L"/dev/null"; break;
    }
    if (!in || !out) {
        log_line(L"Choose the %ls to see the command line.",
                 !in && !out ? L"input and the output" : !in ? L"input" : L"output");
        return;
    }
    options_from_controls(&o, b);
    format_command(&o, in, out[0] ? out : NULL, cmd, sizeof cmd / sizeof *cmd);
    log_line(L"%ls", cmd);
}

/* Reads an image file and checks it against its hash file. */
static void start_check(const wchar_t *file)
{
    wchar_t hashes[PATH_CHARS + 16], msg[2048];
    DWORD a;

    if (!file[0] || g.running || g.confirming)
        return;
    a = GetFileAttributesW(file);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
        error_box(L"The image file does not exist.");
        return;
    }
    if (!dd_hash_file_name(file, hashes, sizeof hashes / sizeof *hashes) ||
        GetFileAttributesW(hashes) == INVALID_FILE_ATTRIBUTES) {
        _snwprintf_s(msg, 2048, _TRUNCATE, L"This image has no hash file:\n\n    %ls\n\nThere is nothing to check it against.",
                     dd_hash_file_name(file, hashes, sizeof hashes / sizeof *hashes) ? hashes : file);
        error_box(msg);
        return;
    }
    options_from_controls(&g.opt, NULL);
    wcscpy_s(g.input, PATH_CHARS, file);
    g.opt.input = g.input;

    SetDlgItemTextW(g.dlg, IDC_LOG, L"");
    log_line(L"Checking %ls against %ls", file, hashes);
    g.writing = false;
    g.removable = false;
    g.checking = true;
    run_worker();
}

static bool usable_disk(const dd_disk_info *d)
{
    if (!d->media_present) {
        error_box(L"There is no media in that drive.");
        return false;
    }
    if (!d->interface_path[0]) {
        error_box(L"This disk cannot be identified reliably, so dd will not use it.");
        return false;
    }
    return true;
}

/* A compressed image is read in order: reading only its used space needs
   its sparse map, which Start makes first when there is none (or none it
   can use).  g.layout and g.fs_ok must be the image's. */
static bool map_needed(void)
{
    return sparse_ticked() && g.layout_ok && g.layout.compressed && !g.fs_ok;
}

/* Asks before a disk is overwritten with what (size bytes, described by
   size_text), refusing if --fit's partitions cannot fit.  g.layout is the
   source's. */
static bool confirm_disk_write(const dd_disk_info *disk, const wchar_t *what, int64_t size, const wchar_t *size_text)
{
    wchar_t msg[2048], a[DD_SIZE_EXACT_CHARS], b[DD_SIZE_EXACT_CHARS], extra[512];

    extra[0] = 0;
    if (fitting()) {
        const dd_layout *l = &g.layout;
        int64_t needed = dd_layout_needed(l);
        if (disk->size >= 0 && needed > disk->size) {
            dd_format_size_exact(needed, true, true, a, DD_SIZE_EXACT_CHARS);
            dd_format_size_exact(disk->size, true, true, b, DD_SIZE_EXACT_CHARS);
            _snwprintf_s(msg, 2048, _TRUNCATE, L"The partitions need %ls, but the disk holds only %ls.", a, b);
            error_box(msg);
            return false;
        }
        if (l->data_end < size) {
            dd_format_size_exact(l->data_end, true, true, a, DD_SIZE_EXACT_CHARS);
            _snwprintf_s(extra, 512, _TRUNCATE,
                         L"\n\nOnly the first %ls, up to the end of the last partition, will be written.", a);
        }
        if (l->scheme == DD_SCHEME_GPT)
            wcsncat_s(extra, 512, L"\n\nThe GPT will be adjusted to the size of the disk.", _TRUNCATE);
    } else if (disk->size >= 0 && size > disk->size) {
        wcscpy_s(extra, 512, L"\n\nThe source is larger than the disk, so the copy will fail when the disk is full.");
    }
    _snwprintf_s(msg, 2048, _TRUNCATE,
                 L"Everything on this disk will be overwritten:\n\n    %ls\n\nwith %ls, %ls.%ls\n\nContinue?",
                 g.target, what, size_text, extra);
    return confirm(msg, L"Overwrite disk") == IDYES;
}

/* A copy waiting for its source's sparse map: what Start confirmed. */
static struct {
    bool         on;
    bool         mapped;        /* the copy running followed its map */
    op           m;
    dd_disk_info disk, from;
    media_id     disk_id, from_id;
} chained;

/* After a wait (a confirmation, or making the map): the disks must still
   hold what they held when the copy was set up.  When says what the wait
   was, for the message. */
static bool disks_unchanged(op m, const dd_disk_info *disk, const media_id *disk_id,
                            const dd_disk_info *from, const media_id *from_id, const wchar_t *when)
{
    wchar_t msg[256];

    if (!g.refresh_pending)
        return true;
    fill_disks();           /* only the list; the copy is bound to the disks */
    /* but a disk can change while bound: new media in the same reader */
    if ((m == OP_WRITE || m == OP_READ || m == OP_DISKS || m == OP_WIPE || m == OP_TEST) &&
        (!same_disk(disk, disk_id) || (m == OP_DISKS && !same_disk(from, from_id)))) {
        log_line(L"Not started: a disk changed %ls.", when);
        _snwprintf_s(msg, 256, _TRUNCATE, L"A disk was inserted, removed or changed %ls. "
                     L"Check the disks and press Start again.", when);
        error_box(msg);
        return false;
    }
    return true;
}

/* Maps a compressed image's file systems on the worker thread, so that it
   can be read sparsely. */
static void run_map(const wchar_t *file)
{
    wchar_t map[PATH_CHARS + 16];

    if (!dd_sparse_map_name(file, map, sizeof map / sizeof *map))
        map[0] = 0;
    wcscpy_s(g.map_input, PATH_CHARS, file);
    g.map_unclean = unclean_ticked();
    log_line(L"Making the sparse map of %ls%ls, in %ls", file,
             g.map_unclean ? L", including file systems not cleanly unmounted" : L"", map);
    g.task = TASK_MAP;
    run_worker();
}

/* Make sparse map: only the map, to see what sparse reading would read. */
static void start_make_map(void)
{
    wchar_t file[PATH_CHARS];

    if (g.running || g.confirming || !source_compressed())
        return;
    GetDlgItemTextW(g.dlg, IDC_SRCFILE, file, PATH_CHARS);
    if (!file[0] || !need_full_path(file))
        return;
    chained.on = false;
    SetDlgItemTextW(g.dlg, IDC_LOG, L"");
    g.opt.verify = false;
    g.writing = false;
    g.checking = false;
    g.removable = false;
    run_map(file);
}

static void start(void)
{
    const dd_disk_info *sel = source_disk(), *sel2 = target_disk();
    dd_disk_info disk, from;
    media_id disk_id, from_id;          /* what they hold, before any confirmation */
    wchar_t file[PATH_CHARS], src[PATH_CHARS], msg[2048], what[512];
    wchar_t isize[2 * DD_SIZE_EXACT_CHARS + 32], a[DD_SIZE_EXACT_CHARS];
    op m = operation();
    DWORD attr;

    if (g.running || g.confirming)
        return;
    memset(&disk, 0, sizeof disk);
    memset(&from, 0, sizeof from);
    memset(&disk_id, 0, sizeof disk_id);
    memset(&from_id, 0, sizeof from_id);
    what[0] = 0;
    GetDlgItemTextW(g.dlg, IDC_SRCFILE, src, PATH_CHARS);
    g.task = TASK_COPY;
    g.testing = false;
    if (checking_hashes()) {
        start_check(src);
        return;
    }

    switch (m) {
    case OP_WRITE: {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        int64_t size;

        if (!sel2 || !src[0] || !need_full_path(src))
            return;
        /* The list may be rebuilt before the copy starts.  The copy uses
           the interface path, which no other device can take over, so it
           always reaches the disk named in the confirmation. */
        disk = *sel2;
        if (!usable_disk(&disk))
            return;
        read_media_id(&disk, &disk_id);
        describe_disk(&disk, g.target, 512);
        wcscpy_s(g.disk_name, 32, disk.path);
        if (!GetFileAttributesExW(src, GetFileExInfoStandard, &fa) ||
            (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            error_box(L"The image file does not exist.");
            return;
        }
        size = ((int64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        dd_format_size_exact(size, true, true, isize, DD_SIZE_EXACT_CHARS);
        load_layout();      /* the file may have changed since it was shown */
        if (g.layout_ok && g.layout.compressed) {
            /* what reaches the disk is the uncompressed image */
            size = g.layout.image_size;
            dd_format_size_exact(size, true, true, a, DD_SIZE_EXACT_CHARS);
            wcsncat_s(isize, sizeof isize / sizeof *isize, L" xz-compressed, ", _TRUNCATE);
            wcsncat_s(isize, sizeof isize / sizeof *isize, a, _TRUNCATE);
            wcsncat_s(isize, sizeof isize / sizeof *isize, L" uncompressed", _TRUNCATE);
        }
        if (!confirm_disk_write(&disk, src, size, isize))
            goto declined;
        options_from_controls(&g.opt, &disk);
        wcscpy_s(g.input, PATH_CHARS, src);
        wcscpy_s(g.output, PATH_CHARS, disk.interface_path);
        g.opt.output_name = g.disk_name;
        SetDlgItemTextW(g.dlg, IDC_LOG, L"");
        log_line(L"Writing %ls to %ls (%ls)", src, disk.path, g.target);
        g.writing = true;
        g.removable = disk.removable;
        break;
    }

    case OP_READ:
        GetDlgItemTextW(g.dlg, IDC_FILE, file, PATH_CHARS);
        if (!sel || !file[0] || !need_full_path(file))
            return;
        disk = *sel;
        if (!usable_disk(&disk))
            return;
        read_media_id(&disk, &disk_id);
        describe_disk(&disk, g.target, 512);
        wcscpy_s(g.disk_name, 32, disk.path);
        /* the disk's table decides; the engine reads it again itself */
        load_layout();
        fix_extension();        /* .xz exactly when compressing */
        GetDlgItemTextW(g.dlg, IDC_FILE, file, PATH_CHARS);
        options_from_controls(&g.opt, NULL);
        if (GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES && _wcsicmp(file, g.confirmed_overwrite) != 0) {
            _snwprintf_s(msg, 2048, _TRUNCATE, L"%ls already exists.\n\nReplace it?", file);
            if (confirm(msg, L"Replace file") != IDYES)
                goto declined;
        }
        g.confirmed_overwrite[0] = 0;   /* good for one run: the next would replace this one */
        wcscpy_s(g.input, PATH_CHARS, disk.interface_path);
        g.opt.input_name = g.disk_name;
        wcscpy_s(g.output, PATH_CHARS, file);
        SetDlgItemTextW(g.dlg, IDC_LOG, L"");
        log_line(L"Reading %ls (%ls) into %ls", disk.path, g.target, file);
        g.writing = false;
        g.removable = disk.removable;
        break;

    case OP_IMAGES:
        if (src[0] && !need_full_path(src))
            return;
        attr = GetFileAttributesW(src);
        if (!src[0] || attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            error_box(L"The image file to copy does not exist.");
            return;
        }
        load_layout();
        fix_extension();        /* .xz exactly when compressing */
        GetDlgItemTextW(g.dlg, IDC_FILE, file, PATH_CHARS);
        if (!file[0] || !need_full_path(file))
            return;
        if (dd_same_file(src, file)) {      /* by identity: links, shares and 8.3 names too */
            error_box(L"The new image would replace the one being copied. Choose another name for it.");
            return;
        }
        options_from_controls(&g.opt, NULL);
        if (GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES && _wcsicmp(file, g.confirmed_overwrite) != 0) {
            _snwprintf_s(msg, 2048, _TRUNCATE, L"%ls already exists.\n\nReplace it?", file);
            if (confirm(msg, L"Replace file") != IDYES)
                goto declined;
        }
        g.confirmed_overwrite[0] = 0;   /* good for one run: the next would replace this one */
        wcscpy_s(g.input, PATH_CHARS, src);
        wcscpy_s(g.output, PATH_CHARS, file);
        SetDlgItemTextW(g.dlg, IDC_LOG, L"");
        log_line(L"Copying %ls to %ls", src, file);
        g.writing = false;
        g.removable = false;
        break;

    case OP_DISKS:
        if (!sel || !sel2)
            return;
        from = *sel;
        disk = *sel2;
        if (!usable_disk(&from) || !usable_disk(&disk))
            return;
        read_media_id(&disk, &disk_id);
        read_media_id(&from, &from_id);
        if (_wcsicmp(from.interface_path, disk.interface_path) == 0) {
            error_box(L"Choose two different disks: a disk cannot be copied onto itself.");
            return;
        }
        load_layout();      /* the source disk's table */
        describe_disk(&disk, g.target, 512);
        wcscpy_s(g.disk_name, 32, disk.path);
        wcscpy_s(g.src_name, 32, from.path);
        describe_disk(&from, what, 512);
        dd_format_size_exact(from.size, true, true, isize, DD_SIZE_EXACT_CHARS);
        if (!confirm_disk_write(&disk, what, from.size, isize))
            goto declined;
        options_from_controls(&g.opt, &disk);
        wcscpy_s(g.input, PATH_CHARS, from.interface_path);
        g.opt.input_name = g.src_name;
        wcscpy_s(g.output, PATH_CHARS, disk.interface_path);
        g.opt.output_name = g.disk_name;
        SetDlgItemTextW(g.dlg, IDC_LOG, L"");
        log_line(L"Copying %ls (%ls) to %ls (%ls)", from.path, what, disk.path, g.target);
        g.writing = true;
        g.removable = disk.removable;
        break;

    case OP_WIPE: {
        const wchar_t *with = input_kind() == SRC_ZERO ? L"zeros" : L"random data";

        if (!sel2)
            return;
        disk = *sel2;
        if (!usable_disk(&disk))
            return;
        if (disk.size <= 0) {
            error_box(L"The size of this disk is not known, so dd cannot tell how much to write.");
            return;
        }
        read_media_id(&disk, &disk_id);
        describe_disk(&disk, g.target, 512);
        wcscpy_s(g.disk_name, 32, disk.path);
        load_layout();      /* none: nothing is read, so Raw does not apply */
        dd_format_size_exact(disk.size, true, true, isize, DD_SIZE_EXACT_CHARS);
        if (!confirm_disk_write(&disk, with, disk.size, isize))
            goto declined;
        options_from_controls(&g.opt, &disk);
        wcscpy_s(g.input, PATH_CHARS, special_input());
        wcscpy_s(g.output, PATH_CHARS, disk.interface_path);
        g.opt.output_name = g.disk_name;
        SetDlgItemTextW(g.dlg, IDC_LOG, L"");
        log_line(L"Overwriting %ls (%ls) with %ls", disk.path, g.target, with);
        g.writing = true;
        g.removable = disk.removable;
        break;
    }

    case OP_TEST:
        if (!sel)
            return;
        disk = *sel;
        if (!usable_disk(&disk))
            return;
        read_media_id(&disk, &disk_id);
        describe_disk(&disk, g.target, 512);
        wcscpy_s(g.disk_name, 32, disk.path);
        load_layout();      /* the disk's table decides what --fit reads */
        options_from_controls(&g.opt, NULL);
        wcscpy_s(g.input, PATH_CHARS, disk.interface_path);
        g.opt.input_name = g.disk_name;
        wcscpy_s(g.output, PATH_CHARS, L"/dev/null");
        SetDlgItemTextW(g.dlg, IDC_LOG, L"");
        log_line(L"Reading %ls (%ls) to test it; nothing is written", disk.path, g.target);
        g.writing = false;
        g.testing = true;
        g.removable = disk.removable;
        break;

    case OP_CHECK:          /* the hash file box unticked: only read the image */
        if (src[0] && !need_full_path(src))
            return;
        attr = GetFileAttributesW(src);
        if (!src[0] || attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            error_box(L"The image file does not exist.");
            return;
        }
        load_layout();
        options_from_controls(&g.opt, NULL);
        wcscpy_s(g.input, PATH_CHARS, src);
        wcscpy_s(g.output, PATH_CHARS, L"/dev/null");
        SetDlgItemTextW(g.dlg, IDC_LOG, L"");
        log_line(L"Reading %ls to test it; nothing is written", src);
        g.writing = false;
        g.testing = true;
        g.removable = false;
        break;
    }
    g.opt.input = g.input;
    g.opt.output = g.output;
    if (!disks_unchanged(m, &disk, &disk_id, &from, &from_id, L"while the confirmation was open"))
        return;
    g.checking = false;
    if (map_needed()) {
        /* the copy follows the map, without asking again */
        chained.on = true;
        chained.m = m;
        chained.disk = disk;
        chained.from = from;
        chained.disk_id = disk_id;
        chained.from_id = from_id;
        run_map(g.input);
        return;
    }
    run_worker();
    return;

declined:
    if (g.refresh_pending)
        fill_disks();
}

static void finished(dd_result r, dd_stats *st)
{
    wchar_t buf[512];
    bool mapped = g.task == TASK_MAP;

    WaitForSingleObject(g.worker, INFINITE);
    CloseHandle(g.worker);
    g.worker = NULL;
    g.running = false;

    if (mapped && chained.on) {
        chained.on = false;
        if (r == DD_OK && !g.close_when_done) {
            free(st);
            g.task = TASK_COPY;
            if (disks_unchanged(chained.m, &chained.disk, &chained.disk_id, &chained.from, &chained.from_id,
                                L"while the sparse map was made")) {
                log_line(L"The sparse map is made; now the copy.");
                chained.mapped = true;
                run_worker();
                return;
            }
            SetDlgItemTextW(g.dlg, IDC_ETA, L"Not started. The sparse map is made.");
            load_layout();
            return;
        }
    }

    if (st && st->started && (g.checking || mapped)) {
        show_phase(st);
    } else if (st && st->started) {
        dd_stats copy = *st;            /* the copy's line, even after verifying */
        copy.phase = DD_PHASE_COPY;
        show_progress(&copy);
        dd_format_records(st, buf, 512);
        log_text(buf);
        dd_format_transfer(st, false, buf, 512);
        log_line(L"%ls", buf);
    } else {
        SetDlgItemTextW(g.dlg, IDC_STATUS, L"");
    }
    SetWindowTextW(g.dlg, APP_TITLE);   /* after the percentages above */
    switch (r) {
    case DD_OK:
        set_bar(1000, 0);
        SetDlgItemTextW(g.dlg, IDC_ETA,
                        mapped ? L"Finished. The sparse map is made."
                        : g.checking ? L"Finished. The image matches its hash file."
                        : g.testing ? L"Finished. All of it was read without error."
                        : g.writing && g.removable && g.verified
                        ? L"Finished. The disk has been written, verified and flushed; you can remove it."
                        : g.writing && g.removable
                        ? L"Finished. The disk has been written and flushed; you can remove it."
                        : g.verified ? L"Finished and verified." : L"Finished.");
        MessageBeep(MB_OK);
        break;
    case DD_CANCELLED:
        SendDlgItemMessageW(g.dlg, IDC_PROGRESS, PBM_SETSTATE, PBST_PAUSED, 0);
        SetDlgItemTextW(g.dlg, IDC_ETA, mapped ? L"Stopped. No sparse map was written."
                        : g.checking ? L"Stopped. The image was not fully checked."
                        : g.testing ? L"Stopped. Not all of it was read."
                        : g.writing ? L"Stopped. The disk is only partly written, or not verified."
                        : L"Stopped. The image file is incomplete, or not verified.");
        break;
    default:
        SendDlgItemMessageW(g.dlg, IDC_PROGRESS, PBM_SETSTATE, PBST_ERROR, 0);
        SetDlgItemTextW(g.dlg, IDC_ETA, L"Failed. See the messages below.");
        MessageBeep(MB_ICONHAND);
        break;
    }
    free(st);
    g.task = TASK_COPY;

    if (g.close_when_done) {
        EndDialog(g.dlg, 0);
        return;
    }
    if (g.writing || g.refresh_pending)
        fill_disks();       /* the partition table has probably changed */
    else if (mapped || chained.mapped)
        load_layout();      /* the Sparse read column comes from the new map */
    chained.mapped = false;
    update_controls();
}

static void stop(void)
{
    if (g.running) {
        dd_cancel_request(&g.cancel);
        SetDlgItemTextW(g.dlg, IDC_ETA, L"Stopping...");
        EnableWindow(GetDlgItem(g.dlg, IDC_STOP), FALSE);
    }
}

/* ---- dialog ------------------------------------------------------------ */

/* Browse... for the file in box id: one to read, or the new image. */
static void browse(int id)
{
    wchar_t path[PATH_CHARS];
    OPENFILENAMEW ofn;
    bool save = id == IDC_FILE;         /* the Output row's file: the new image */
    BOOL ok;

    GetDlgItemTextW(g.dlg, id, path, PATH_CHARS);
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g.dlg;
    ofn.lpstrFile = path;
    ofn.nMaxFile = PATH_CHARS;
    if (!save)
        ofn.lpstrFilter = L"Disk images (*.img, *.iso, *.bin, *.raw, *.dd, *.xz)\0*.img;*.iso;*.bin;*.raw;*.dd;*.xz\0"
                          L"All files (*.*)\0*.*\0";
    else if (xz_level() >= 0)
        ofn.lpstrFilter = L"xz-compressed disk images (*.xz)\0*.xz\0All files (*.*)\0*.*\0";
    else
        ofn.lpstrFilter = L"Disk images (*.img, *.bin, *.raw, *.dd)\0*.img;*.bin;*.raw;*.dd\0"
                          L"All files (*.*)\0*.*\0";
    ofn.lpstrDefExt = L"img";   /* compressing, fix_extension adds .xz */
    if (!save) {
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        ok = GetOpenFileNameW(&ofn);
    } else {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOREADONLYRETURN | OFN_NOCHANGEDIR;
        ok = GetSaveFileNameW(&ofn);
        /* the dialog asked only if the file is there */
        if (ok && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            wcscpy_s(g.confirmed_overwrite, PATH_CHARS, path);
        else
            g.confirmed_overwrite[0] = 0;
    }
    if (ok) {
        SetDlgItemTextW(g.dlg, id, path);
        if (id == IDC_FILE)
            fix_extension();
    }
}

/* The version, as dd --version gives it. */
static void about(void)
{
    wchar_t text[768];

    _snwprintf_s(text, 768, _TRUNCATE,
                 APP_TITLE L"\n\n"
                 L"dd reimagined for Windows Copyright (C) 2026 Mark Smith\n"
                 L"dd Copyright (C) 2003 John Newbigin\n\n"
                 L"GNU GPL version 2 or later. This is free software: you are free to change "
                 L"and redistribute it. There is NO WARRANTY, to the extent permitted by law.\n\n"
                 L"Uses liblzma %ls from XZ Utils (BSD Zero Clause License).",
                 dd_xz_version());
    MessageBoxW(g.dlg, text, L"About " APP_NAME, MB_OK | MB_ICONINFORMATION);
}

static void init_dialog(HWND dlg)
{
    HINSTANCE inst = GetModuleHandleW(NULL);
    wchar_t windows[MAX_PATH];
    size_t i;

    g.dlg = dlg;
    g.icon = LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g.icon_small = LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                              GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)g.icon);
    SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)g.icon_small);
    SetWindowTextW(dlg, APP_TITLE);
    {
        HMENU sys = GetSystemMenu(dlg, FALSE);
        AppendMenuW(sys, MF_SEPARATOR, 0, NULL);
        AppendMenuW(sys, MF_STRING, IDM_ABOUT, L"&About " APP_NAME L"...");
    }

    if (!GetWindowsDirectoryW(windows, MAX_PATH) || !GetVolumePathNameW(windows, g.system_root, MAX_PATH))
        g.system_root[0] = 0;

    /* an elevated window still accepts files dragged from Explorer */
    ChangeWindowMessageFilterEx(dlg, WM_DROPFILES, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(dlg, WM_COPYDATA, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(dlg, 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, NULL);
    DragAcceptFiles(dlg, TRUE);

    /* Unasked, a window hears only of volumes, so a disk Windows mounts
       nothing from (a Linux image, say) would come and go unnoticed. */
    {
        static const GUID disk_class =      /* GUID_DEVINTERFACE_DISK */
            { 0x53f56307, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };
        DEV_BROADCAST_DEVICEINTERFACE_W f = { 0 };
        f.dbcc_size = sizeof f;
        f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        f.dbcc_classguid = disk_class;
        g.disk_notify = RegisterDeviceNotificationW(dlg, &f, DEVICE_NOTIFY_WINDOW_HANDLE);
    }

    for (i = 0; i < sizeof inputs / sizeof *inputs; i++) {
        LRESULT idx = SendDlgItemMessageW(dlg, IDC_INPUT, CB_ADDSTRING, 0, (LPARAM)inputs[i].label);
        SendDlgItemMessageW(dlg, IDC_INPUT, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)inputs[i].src);
    }
    /* disk to image: changes no disk */
    select_data(IDC_INPUT, SRC_DISK);
    fill_outputs();
    select_data(IDC_OUTPUT, DST_FILE);
    for (i = 0; i < sizeof block_sizes / sizeof *block_sizes; i++)
        SendDlgItemMessageW(dlg, IDC_BS, CB_ADDSTRING, 0, (LPARAM)block_sizes[i].label);
    SendDlgItemMessageW(dlg, IDC_BS, CB_SETCURSEL, DEFAULT_BLOCK_SIZE, 0);
    SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETRANGE32, 0, 1000);
    SetWindowSubclass(GetDlgItem(dlg, IDC_PROGRESS), bar_proc, 0, 0);
    SendDlgItemMessageW(dlg, IDC_FILE, EM_SETCUEBANNER, TRUE, (LPARAM)L"Choose the image file to write");
    SendDlgItemMessageW(dlg, IDC_SRCFILE, EM_SETCUEBANNER, TRUE, (LPARAM)L"Choose or drop the image file to read");
    CheckDlgButton(dlg, IDC_HASHES, BST_CHECKED);   /* Verify after starts unticked */
    g.fs = malloc(sizeof *g.fs);    /* without it, the Sparse read column stays empty */
    set_rows();             /* Raw starts unticked: only the partitions are copied */
    init_xz_list();
    init_partition_list();
    fill_disks();
}

static void request_close(void)
{
    if (!g.running) {
        EndDialog(g.dlg, 0);
        return;
    }
    if (MessageBoxW(g.dlg, L"A copy is in progress. Stop it and close?", APP_NAME,
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
        if (!g.running) {           /* it ended while the question was open */
            EndDialog(g.dlg, 0);
            return;
        }
        g.close_when_done = true;
        stop();
    }
}

static INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        init_dialog(dlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_INPUT:
        case IDC_OUTPUT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                if (LOWORD(wp) == IDC_INPUT)
                    fill_outputs();
                set_rows();
                fill_disks();       /* also reads the right partition table */
            }
            return TRUE;
        case IDC_HASHES:
            if (HIWORD(wp) == BN_CLICKED) {
                show_fit();         /* image to nothing: checking its hashes, or only reading it */
                update_controls();
            }
            return TRUE;
        case IDC_SHOWFIXED:
        case IDC_REFRESH:
            if (HIWORD(wp) == BN_CLICKED)
                fill_disks();
            return TRUE;
        case IDC_XZ:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                fix_extension();
                show_fit();
            }
            return TRUE;
        case IDC_FIT:
            if (HIWORD(wp) == BN_CLICKED) {
                show_fit();
                update_controls();
            }
            return TRUE;
        case IDC_SPARSE:
        case IDC_UNCLEAN:
            if (HIWORD(wp) == BN_CLICKED)
                load_layout();      /* the Sparse read column and summary change */
            return TRUE;
        case IDC_DISK:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                if (source_is_disk())
                    load_layout();  /* the table is the disk's */
                else
                    show_fit();
                update_controls();
            }
            return TRUE;
        case IDC_DISK2:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                show_fit();         /* whether the source fits this disk */
                update_controls();
            }
            return TRUE;
        case IDC_FILE:
            if (HIWORD(wp) == EN_CHANGE)
                update_controls();  /* the file is only the output */
            return TRUE;
        case IDC_SRCFILE:
            if (HIWORD(wp) == EN_CHANGE)
                load_layout();
            return TRUE;
        case IDC_BROWSE:
            browse(IDC_FILE);
            return TRUE;
        case IDC_BROWSE_SRC:
            browse(IDC_SRCFILE);
            return TRUE;
        case IDC_MAKEMAP:
            start_make_map();
            return TRUE;
        case IDC_START:
            start();
            return TRUE;
        case IDC_CMDLINE:
            show_command();
            return TRUE;
        case IDC_STOP:
            stop();
            return TRUE;
        case IDCANCEL:
            request_close();
            return TRUE;
        }
        break;

    case WM_DROPFILES: {
        wchar_t path[PATH_CHARS];
        /* a dropped file is an image to read: the input becomes it */
        if (!g.running && DragQueryFileW((HDROP)wp, 0, path, PATH_CHARS) &&
            GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            if (!source_is_file()) {
                /* from reading a disk into an image, an image dropped is
                   surely one to write to a disk */
                bool was_read = operation() == OP_READ;
                select_data(IDC_INPUT, SRC_FILE);
                fill_outputs();
                if (was_read)
                    select_data(IDC_OUTPUT, DST_DISK);
                set_rows();
                fill_disks();
            }
            SetDlgItemTextW(dlg, IDC_SRCFILE, path);
        }
        DragFinish((HDROP)wp);
        return TRUE;
    }

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == IDM_ABOUT) {
            about();
            return TRUE;
        }
        return FALSE;

    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)
            SetTimer(dlg, TIMER_REFRESH, 700, NULL);   /* wait for the burst to settle */
        return TRUE;

    case WM_TIMER:
        if (wp == TIMER_REFRESH) {
            KillTimer(dlg, TIMER_REFRESH);
            if (g.running || g.confirming)
                g.refresh_pending = true;
            else
                fill_disks();
        }
        return TRUE;

    case WM_APP_MESSAGE: {
        wchar_t *text = (wchar_t *)lp;
        log_line(L"%ls%ls", wp == DD_MSG_ERROR ? L"Error: " : wp == DD_MSG_WARNING ? L"Warning: " : L"", text);
        free(text);
        return TRUE;
    }

    case WM_APP_PROGRESS:
        if (g.running)
            show_progress((const dd_stats *)lp);
        free((void *)lp);
        return TRUE;

    case WM_APP_DONE:
        finished((dd_result)wp, (dd_stats *)lp);
        return TRUE;

    case WM_DESTROY:
        if (g.disk_notify)
            UnregisterDeviceNotification(g.disk_notify);
        dd_free_device_list(&g.devices);
        if (g.icon)
            DestroyIcon(g.icon);
        if (g.icon_small)
            DestroyIcon(g.icon_small);
        return TRUE;
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    INITCOMMONCONTROLSEX icc;

    (void)prev;
    (void)cmd;
    (void)show;
    /* Copies and compression run as background work: every thread,
       including liblzma's workers, which inherit the class. */
    SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    return (int)DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), NULL, dialog_proc, 0);
}
