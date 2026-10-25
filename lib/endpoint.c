/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later

    An endpoint hides the differences between files, raw devices, pipes and
    the /dev/ names.  Raw devices only accept sector aligned offsets and
    lengths, so unaligned requests go through a bounce buffer, and partial
    sectors are written by read-modify-write.  Device reads and writes stop
    at the end of the device, which gives GNU's end-of-file and "no space"
    behaviour instead of the Windows parameter errors.
*/
#include "dd_internal.h"
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "advapi32.lib")     /* RegGetValueW */

#define BOUNCE_SIZE (1u << 20)
#define DIRECT_FILE_ALIGN 4096u

void ep_init(dd_endpoint *ep)
{
    memset(ep, 0, sizeof *ep);
    ep->h = INVALID_HANDLE_VALUE;
    ep->size = -1;
    ep->disk_number = -1;
}

bool ep_seekable(const dd_endpoint *ep)
{
    return ep->kind == EP_FILE || ep->kind == EP_BLOCK;
}

/* \\.\X: \\.\PhysicalDrive0 \\?\Volume{...} \\?\GLOBALROOT\Device\... and
   device interface paths such as \\?\usbstor#disk&...#{53f56307-...} */
bool dd__is_device_path(const wchar_t *p)
{
    if (wcsncmp(p, L"\\\\.\\", 4) == 0 || wcsncmp(p, L"\\\\?\\", 4) == 0) {
        if (_wcsnicmp(p + 4, L"GLOBALROOT\\", 11) == 0)
            return true;
        return p[4] && wcschr(p + 4, L'\\') == NULL;
    }
    return false;
}

/* Accepts the 0.6 syntax \\?\Device\HarddiskN\PartitionM (an NT object
   path) by routing it through GLOBALROOT, and drops the trailing backslash
   of a volume name, which would otherwise open the root directory. */
wchar_t *dd__normalize_path(const wchar_t *p)
{
    size_t len = wcslen(p);
    wchar_t *r;

    if (_wcsnicmp(p, L"\\\\?\\Device\\", 11) == 0) {
        size_t n = len + 16;
        r = malloc(n * sizeof *r);
        if (r)
            _snwprintf_s(r, n, _TRUNCATE, L"\\\\?\\GLOBALROOT%ls", p + 3);
        return r;
    }
    r = _wcsdup(p);
    if (r && _wcsnicmp(r, L"\\\\?\\Volume{", 11) == 0 && len > 0 && r[len - 1] == L'\\')
        r[len - 1] = 0;
    return r;
}

#define LONG_PATH_CHARS 32768           /* the longest path Windows takes */

/* The volume and file index of an existing file or folder. */
static bool file_id(const wchar_t *path, BY_HANDLE_FILE_INFORMATION *fi)
{
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    bool ok;

    if (h == INVALID_HANDLE_VALUE)
        return false;
    ok = GetFileInformationByHandle(h, fi) != 0;
    CloseHandle(h);
    return ok;
}

static bool same_id(const BY_HANDLE_FILE_INFORMATION *a, const BY_HANDLE_FILE_INFORMATION *b)
{
    return a->dwVolumeSerialNumber == b->dwVolumeSerialNumber && a->nFileIndexHigh == b->nFileIndexHigh &&
           a->nFileIndexLow == b->nFileIndexLow;
}

/* Splits a full path into its folder and its long last name. */
static bool split_full(const wchar_t *path, wchar_t *dir, wchar_t *leaf, size_t n)
{
    wchar_t *full = malloc(n * sizeof *full), *name;
    WIN32_FIND_DATAW fd;
    HANDLE f;
    DWORD len;

    if (!full)
        return false;
    len = GetFullPathNameW(path, (DWORD)n, full, &name);
    if (len == 0 || len >= n || !name) {
        free(full);
        return false;
    }
    wcsncpy_s(leaf, n, name, _TRUNCATE);
    /* an existing file may have been named by its short 8.3 name */
    if ((f = FindFirstFileW(full, &fd)) != INVALID_HANDLE_VALUE) {
        wcsncpy_s(leaf, n, fd.cFileName, _TRUNCATE);
        FindClose(f);
    }
    *name = 0;
    wcsncpy_s(dir, n, full, _TRUNCATE);
    free(full);
    return true;
}

bool dd_same_file(const wchar_t *a, const wchar_t *b)
{
    BY_HANDLE_FILE_INFORMATION fa, fb;
    wchar_t *buf = malloc(4 * LONG_PATH_CHARS * sizeof *buf);
    wchar_t *da = buf, *la = buf + LONG_PATH_CHARS, *db = buf + 2 * LONG_PATH_CHARS, *lb = buf + 3 * LONG_PATH_CHARS;
    bool same;

    if (file_id(a, &fa) && file_id(b, &fb)) {
        free(buf);
        return same_id(&fa, &fb);       /* links, mapped drives and shares included */
    }
    if (!buf)
        return _wcsicmp(a, b) == 0;
    if (!split_full(a, da, la, LONG_PATH_CHARS) || !split_full(b, db, lb, LONG_PATH_CHARS))
        same = _wcsicmp(a, b) == 0;
    else if (_wcsicmp(la, lb) != 0)
        same = false;
    else if (file_id(da, &fa) && file_id(db, &fb))
        same = same_id(&fa, &fb);
    else
        same = _wcsicmp(da, db) == 0;
    free(buf);
    return same;
}

/* Fills in kind, size, alignment and device identity for an open handle. */
static void classify(dd_endpoint *ep)
{
    GET_LENGTH_INFORMATION li;
    STORAGE_DEVICE_NUMBER sdn;
    BYTE buf[sizeof(VOLUME_DISK_EXTENTS) + 32 * sizeof(DISK_EXTENT)];
    DWORD type = GetFileType(ep->h);
    DWORD ret, sector = 0;
    int64_t gsize = -1;
    int media = 0;
    bool block = false;

    if (type == FILE_TYPE_CHAR || type == FILE_TYPE_PIPE || type == FILE_TYPE_UNKNOWN) {
        ep->kind = EP_STREAM;
        ep->size = -1;
        return;
    }

    if (DeviceIoControl(ep->h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof li, &ret, NULL)) {
        block = true;
        ep->size = li.Length.QuadPart;
    }
    if (dd__geometry(ep->h, &gsize, &sector, &media)) {
        if (!block) {
            block = true;
            ep->size = gsize;
        }
    }

    if (!block) {
        LARGE_INTEGER sz;
        ep->kind = EP_FILE;
        ep->size = GetFileSizeEx(ep->h, &sz) ? sz.QuadPart : -1;
        ep->align = ep->direct ? DIRECT_FILE_ALIGN : 0;
        return;
    }

    ep->kind = EP_BLOCK;
    ep->align = sector ? sector : 512;
    if (dd__device_number(ep->h, &sdn)) {
        ep->disk_number = (LONG)sdn.DeviceNumber;
        ep->cdrom = sdn.DeviceType == FILE_DEVICE_CD_ROM || sdn.DeviceType == FILE_DEVICE_DVD;
        ep->whole_disk = sdn.DeviceType == FILE_DEVICE_DISK && sdn.PartitionNumber == 0;
    }
    if (ep->cdrom && ep->align < 2048)
        ep->align = 2048;
    /* Disks with removable media (USB card readers and sticks) answer the
       volume extents query on the whole-disk handle too, with one extent
       covering the disk.  Partition 0 of a disk is the whole disk, whatever
       that query says; volumes report partition 1 and up. */
    if (!ep->whole_disk &&
        DeviceIoControl(ep->h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, buf, sizeof buf, &ret, NULL)) {
        const VOLUME_DISK_EXTENTS *ext = (const VOLUME_DISK_EXTENTS *)buf;
        ep->is_volume = true;
        ep->whole_disk = false;
        if (ext->NumberOfDiskExtents == 1 && ep->disk_number < 0)
            ep->disk_number = (LONG)ext->Extents[0].DiskNumber;
    }
    if (ep->whole_disk) {
        int bus;
        bool rem;
        wchar_t v[2], p[2];
        if (dd__storage_info(ep->h, &bus, &rem, v, 2, p, 2))
            ep->removable = rem || dd__bus_is_removable(bus);
    }
}

static DWORD open_std(dd_endpoint *ep, DWORD which, const wchar_t *name)
{
    HANDLE h = GetStdHandle(which);
    LARGE_INTEGER zero, cur;

    wcscpy_s(ep->name, sizeof ep->name / sizeof *ep->name, name);
    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return ERROR_INVALID_HANDLE;
    ep->h = h;
    ep->own_handle = false;
    classify(ep);
    if (ep->kind == EP_FILE) {
        /* Continue from wherever the shell left the handle, as GNU does. */
        zero.QuadPart = 0;
        if (SetFilePointerEx(h, zero, &cur, FILE_CURRENT))
            ep->pos = cur.QuadPart;
    }
    return 0;
}

static bool is_name(const wchar_t *p, const wchar_t *name)
{
    return _wcsicmp(p, name) == 0;
}

DWORD ep_open_input(dd_endpoint *ep, const wchar_t *path, unsigned iflags)
{
    if (!path || wcscmp(path, L"-") == 0 || is_name(path, L"/dev/stdin"))
        return open_std(ep, STD_INPUT_HANDLE, L"standard input");

    wcscpy_s(ep->name, sizeof ep->name / sizeof *ep->name, path);
    if (is_name(path, L"/dev/zero")) {
        ep->kind = EP_ZERO;
        return 0;
    }
    if (is_name(path, L"/dev/random") || is_name(path, L"/dev/urandom")) {
        ep->kind = EP_RANDOM;
        return 0;
    }
    if (is_name(path, L"/dev/null")) {
        ep->kind = EP_NULL;
        ep->size = 0;
        return 0;
    }

    ep->path = dd__normalize_path(path);
    if (!ep->path)
        return ERROR_NOT_ENOUGH_MEMORY;
    ep->device_path = dd__is_device_path(ep->path);
    ep->direct = (iflags & DD_FLAG_DIRECT) != 0;
    ep->access = GENERIC_READ;
    ep->share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    ep->flags = ep->direct ? FILE_FLAG_NO_BUFFERING : FILE_FLAG_SEQUENTIAL_SCAN;
    if (iflags & (DD_FLAG_SYNC | DD_FLAG_DSYNC))
        ep->flags |= FILE_FLAG_WRITE_THROUGH;

    ep->h = CreateFileW(ep->path, ep->access, ep->share, NULL, OPEN_EXISTING, ep->flags, NULL);
    if (ep->h == INVALID_HANDLE_VALUE)
        return GetLastError();
    ep->own_handle = true;
    classify(ep);
    return 0;
}

DWORD ep_open_output(dd_endpoint *ep, const wchar_t *path, unsigned oflags, unsigned conv)
{
    DWORD disposition;

    if (!path || wcscmp(path, L"-") == 0 || is_name(path, L"/dev/stdout"))
        return open_std(ep, STD_OUTPUT_HANDLE, L"standard output");
    if (is_name(path, L"/dev/stderr"))
        return open_std(ep, STD_ERROR_HANDLE, L"standard error");

    wcscpy_s(ep->name, sizeof ep->name / sizeof *ep->name, path);
    if (is_name(path, L"/dev/null") || is_name(path, L"/dev/zero") ||
        is_name(path, L"/dev/random") || is_name(path, L"/dev/urandom")) {
        ep->kind = EP_NULL;
        return 0;
    }

    ep->path = dd__normalize_path(path);
    if (!ep->path)
        return ERROR_NOT_ENOUGH_MEMORY;
    ep->device_path = dd__is_device_path(ep->path);
    ep->direct = (oflags & DD_FLAG_DIRECT) != 0;

    if (ep->device_path || (conv & DD_CONV_NOCREAT))
        disposition = OPEN_EXISTING;
    else if (conv & DD_CONV_EXCL)
        disposition = CREATE_NEW;
    else
        disposition = OPEN_ALWAYS;

    /* Devices are shared with the rest of the system; their volumes get
       locked separately.  Direct I/O files must allow our own reopen. */
    ep->share = (ep->device_path || ep->direct) ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ;
    ep->flags = 0;
    if (ep->direct)
        ep->flags |= FILE_FLAG_NO_BUFFERING;
    if (oflags & (DD_FLAG_SYNC | DD_FLAG_DSYNC))
        ep->flags |= FILE_FLAG_WRITE_THROUGH;

    /* Read access is needed for read-modify-write of partial sectors. */
    ep->access = GENERIC_READ | GENERIC_WRITE;
    ep->h = CreateFileW(ep->path, ep->access, ep->share, NULL, disposition, ep->flags, NULL);
    if (ep->h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED && !ep->device_path) {
        ep->access = GENERIC_WRITE;
        ep->h = CreateFileW(ep->path, ep->access, ep->share, NULL, disposition, ep->flags, NULL);
    }
    if (ep->h == INVALID_HANDLE_VALUE)
        return GetLastError();
    ep->own_handle = true;
    classify(ep);
    if (ep->kind == EP_FILE && (oflags & DD_FLAG_APPEND))
        ep->pos = ep->size > 0 ? ep->size : 0;
    return 0;
}

/* ---- locking ----------------------------------------------------------- */

static DWORD try_lock(HANDLE h, int tries)
{
    DWORD ret, e = 0;

    while (tries-- > 0) {
        if (DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &ret, NULL))
            return 0;
        e = GetLastError();
        if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION)
            return e;
        if (tries)
            Sleep(250);
    }
    return e;
}

/* Locks, then dismounts, a volume.  A lock is refused while files on the
   volume are open; with force the volume is dismounted anyway, which
   invalidates those handles.  The lock stays refused for as long as the
   stale handles stay open, but Windows allows raw writes to a dismounted
   volume, so the copy goes ahead unlocked. */
static DWORD lock_and_dismount(HANDLE h, bool force, bool *forced)
{
    DWORD ret, e;

    *forced = false;
    e = try_lock(h, force ? 4 : 20);
    if (e && force && (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION)) {
        if (!DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &ret, NULL))
            return GetLastError();
        *forced = true;
        e = try_lock(h, 4);
        if (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION)
            return 0;
    }
    if (e)
        return e;
    DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &ret, NULL);
    return 0;
}

/* \\?\Volume{GUID}\ of the volume holding the Windows directory. */
static bool system_volume(wchar_t *vol, size_t n)
{
    wchar_t win[MAX_PATH], root[MAX_PATH];

    return GetWindowsDirectoryW(win, MAX_PATH) &&
           GetVolumePathNameW(win, root, MAX_PATH) &&
           GetVolumeNameForVolumeMountPointW(root, vol, (DWORD)n);
}

typedef struct extents {
    VOLUME_DISK_EXTENTS x;
    DISK_EXTENT more[31];
} extents;

static int volume_extents(const wchar_t *path, extents *x)
{
    HANDLE s = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD ret;
    bool ok;

    if (s == INVALID_HANDLE_VALUE)
        return -1;
    ok = DeviceIoControl(s, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, x, sizeof *x, &ret, NULL) &&
         x->x.NumberOfDiskExtents > 0;
    CloseHandle(s);
    return ok ? 1 : 0;
}

/* Where the volume Windows runs from lies.  -1 if it cannot be found at
   all; 0 if it is on no disk (a RAM disk, as in Windows PE). */
static int system_extents(extents *x)
{
    wchar_t vol[MAX_PATH];
    size_t len;

    if (!system_volume(vol, MAX_PATH))
        return -1;
    len = wcslen(vol);
    if (len > 0 && vol[len - 1] == L'\\')
        vol[len - 1] = 0;
    return volume_extents(vol, x);
}

/* Where the partition Windows booted from lies (the EFI system partition,
   or the active one holding the boot manager), as Setup records it. */
static bool boot_extents(extents *x)
{
    wchar_t dev[MAX_PATH], path[MAX_PATH + 16];
    DWORD n = sizeof dev;

    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", L"SystemPartition", RRF_RT_REG_SZ, NULL, dev, &n) !=
        ERROR_SUCCESS)
        return false;
    _snwprintf_s(path, MAX_PATH + 16, _TRUNCATE, L"\\\\?\\GLOBALROOT%ls", dev);
    return volume_extents(path, x) > 0;
}

static bool overlaps(const extents *x, DWORD disk, int64_t off, int64_t len)
{
    DWORD i;

    for (i = 0; i < x->x.NumberOfDiskExtents && i < 32; i++) {
        const DISK_EXTENT *d = &x->x.Extents[i];
        if (d->DiskNumber == disk &&
            (len < 0 || (off < d->StartingOffset.QuadPart + d->ExtentLength.QuadPart && d->StartingOffset.QuadPart < off + len)))
            return true;
    }
    return false;
}

/* Whether [off, off + len) of a disk holds part of the volume Windows runs
   from, or the partition it boots from (len < 0: anywhere on the disk).
   With strict, a system volume that cannot be found counts. */
static bool on_system(DWORD disk, int64_t off, int64_t len, bool strict)
{
    extents x;
    int found = system_extents(&x);

    if (found < 0 && strict)
        return true;
    if (found > 0 && overlaps(&x, disk, off, len))
        return true;
    return boot_extents(&x) && overlaps(&x, disk, off, len);
}

bool dd_disk_is_system(DWORD disk)
{
    return on_system(disk, 0, -1, false);
}

/* Is the volume or partition open on h, or any part of it, the one Windows
   runs from? */
static bool is_system_volume(HANDLE h)
{
    extents x;
    STORAGE_DEVICE_NUMBER sdn;
    PARTITION_INFORMATION_EX pi;
    DWORD i, ret;

    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &x, sizeof x, &ret, NULL) &&
        x.x.NumberOfDiskExtents > 0) {
        for (i = 0; i < x.x.NumberOfDiskExtents && i < 32; i++)
            if (on_system(x.x.Extents[i].DiskNumber, x.x.Extents[i].StartingOffset.QuadPart,
                          x.x.Extents[i].ExtentLength.QuadPart, true))
                return true;
        return false;
    }
    /* a partition that is not a volume Windows knows */
    if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &sdn, sizeof sdn, &ret, NULL) &&
        DeviceIoControl(h, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0, &pi, sizeof pi, &ret, NULL))
        return on_system(sdn.DeviceNumber, pi.StartingOffset.QuadPart, pi.PartitionLength.QuadPart, true);
    return false;
}

static void describe_mounts(const wchar_t *mounts, wchar_t *buf, size_t n)
{
    buf[0] = 0;
    for (; mounts && *mounts; mounts += wcslen(mounts) + 1) {
        if (buf[0])
            wcsncat_s(buf, n, L", ", _TRUNCATE);
        wcsncat_s(buf, n, mounts, _TRUNCATE);
    }
    if (!buf[0])
        wcsncpy_s(buf, n, L"not mounted", _TRUNCATE);
}

/* ---- drive letters ------------------------------------------------------- */

#define MOUNTED_DEVICES L"SYSTEM\\MountedDevices"

static bool is_letter_root(const wchar_t *m)
{
    return ((m[0] >= L'A' && m[0] <= L'Z') || (m[0] >= L'a' && m[0] <= L'z')) &&
           m[1] == L':' && m[2] == L'\\' && m[3] == 0;
}

/* Takes the drive letters off a volume dd has locked, so that nothing can
   open it by letter during the copy (Explorer offering to format a volume
   being overwritten, say).  Each is remembered, with the mount manager's
   record of it, for restore_letters. */
static void take_letters(dd_endpoint *ep, const dd_callbacks *cb, const wchar_t *root, const wchar_t *mounts,
                         DWORD disk)
{
    wchar_t name[16], msg[256];
    const wchar_t *m;

    ep->letters_cb = cb;
    for (m = mounts; m && *m; m += wcslen(m) + 1) {
        dd__letter *l;
        DWORD n;

        if (!is_letter_root(m) || ep->nletters >= DD_MAX_LETTERS)
            continue;
        l = &ep->letters[ep->nletters];
        memset(l, 0, sizeof *l);
        l->letter = (wchar_t)towupper(m[0]);
        wcsncpy_s(l->volume, 64, root, _TRUNCATE);
        l->disk = disk;
        _snwprintf_s(name, 16, _TRUNCATE, L"\\DosDevices\\%lc:", l->letter);
        n = sizeof l->id;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, MOUNTED_DEVICES, name, RRF_RT_REG_BINARY, NULL, l->id, &n) == ERROR_SUCCESS)
            l->idlen = n;
        if (!DeleteVolumeMountPointW(m)) {
            dd_errstr(GetLastError(), msg, 256);
            dd__msg(cb, DD_MSG_WARNING, L"cannot remove drive letter %lc: while its volume is locked: %ls", l->letter, msg);
            continue;
        }
        ep->nletters++;
        dd__msg(cb, DD_MSG_INFO, L"removed drive letter %lc: while its volume is locked", l->letter);
    }
}

typedef struct same_place_ctx {
    dd_endpoint *ep;
    const dd_callbacks *cb;
    DWORD disk;
    int64_t offset;
} same_place_ctx;

static bool take_letters_at(void *vctx, const dd__volume *v)
{
    same_place_ctx *ctx = vctx;

    if (!v->ext || v->ext->NumberOfDiskExtents != 1 || v->ext->Extents[0].DiskNumber != ctx->disk ||
        v->ext->Extents[0].StartingOffset.QuadPart != ctx->offset)
        return true;
    take_letters(ctx->ep, ctx->cb, v->root, v->mounts, ctx->disk);
    return false;
}

/* The same for a volume opened by any of its names: it is found by where
   it lies. */
static void take_volume_letters(dd_endpoint *ep, const dd_callbacks *cb)
{
    BYTE buf[sizeof(VOLUME_DISK_EXTENTS) + 32 * sizeof(DISK_EXTENT)];
    const VOLUME_DISK_EXTENTS *ext = (const VOLUME_DISK_EXTENTS *)buf;
    same_place_ctx ctx;
    DWORD ret;

    if (!ep->is_volume ||
        !DeviceIoControl(ep->h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, buf, sizeof buf, &ret, NULL) ||
        ext->NumberOfDiskExtents != 1)
        return;
    ctx.ep = ep;
    ctx.cb = cb;
    ctx.disk = ext->Extents[0].DiskNumber;
    ctx.offset = ext->Extents[0].StartingOffset.QuadPart;
    dd__for_each_volume(take_letters_at, &ctx);
}

/* Whether the volume \\?\Volume{GUID}\ lies on the disk. */
static bool on_same_disk(const wchar_t *root, DWORD disk)
{
    wchar_t dev[64];
    extents x;
    size_t len;
    DWORD i;

    wcsncpy_s(dev, 64, root, _TRUNCATE);
    len = wcslen(dev);
    if (len > 0 && dev[len - 1] == L'\\')
        dev[len - 1] = 0;
    if (volume_extents(dev, &x) <= 0)
        return false;
    for (i = 0; i < x.x.NumberOfDiskExtents && i < 32; i++)
        if (x.x.Extents[i].DiskNumber == disk)
            return true;
    return false;
}

/* Puts the letters back once the locks are gone.  A volume on a disk that
   was written may take a moment to come back, or be gone for good; then
   the mount manager's record is put back, as it was before dd took the
   letter, so that Windows gives the letter back if the volume returns. */
static void restore_letters(dd_endpoint *ep)
{
    const dd_callbacks *cb = ep->letters_cb;
    ULONGLONG until = GetTickCount64() + 3000;
    bool done[DD_MAX_LETTERS] = { false };
    wchar_t root[4], cur[64], name[16], msg[256];
    int i, left = ep->nletters;

    for (;;) {
        for (i = 0; i < ep->nletters; i++) {
            const dd__letter *l = &ep->letters[i];
            if (done[i])
                continue;
            _snwprintf_s(root, 4, _TRUNCATE, L"%lc:\\", l->letter);
            if (GetVolumeNameForVolumeMountPointW(root, cur, 64)) {
                done[i] = true;
                left--;
                /* the volume is back with its letter, or the disk's new one has it */
                if (_wcsicmp(cur, l->volume) != 0) {
                    if (on_same_disk(cur, l->disk))
                        dd__msg(cb, DD_MSG_INFO, L"Windows gave drive letter %lc: to a new volume on the disk",
                                l->letter);
                    else
                        dd__msg(cb, DD_MSG_WARNING, L"drive letter %lc: was given to another volume during the "
                                L"copy; it is not put back", l->letter);
                }
                continue;
            }
            if (SetVolumeMountPointW(root, l->volume)) {
                done[i] = true;
                left--;
                dd__msg(cb, DD_MSG_INFO, L"put drive letter %lc: back", l->letter);
            }
        }
        if (left == 0 || GetTickCount64() >= until)
            break;
        Sleep(250);
    }
    for (i = 0; i < ep->nletters; i++) {
        const dd__letter *l = &ep->letters[i];
        DWORD e = ERROR_NOT_FOUND, n = 0;
        if (done[i])
            continue;
        _snwprintf_s(name, 16, _TRUNCATE, L"\\DosDevices\\%lc:", l->letter);
        if (l->idlen) {
            if (RegGetValueW(HKEY_LOCAL_MACHINE, MOUNTED_DEVICES, name, RRF_RT_REG_BINARY, NULL, NULL, &n) == ERROR_SUCCESS)
                e = ERROR_ALREADY_ASSIGNED;
            else
                e = RegSetKeyValueW(HKEY_LOCAL_MACHINE, MOUNTED_DEVICES, name, REG_BINARY, l->id, l->idlen);
        }
        if (e == 0) {
            dd__msg(cb, DD_MSG_INFO, L"drive letter %lc: is kept for its volume, which is not there after the copy; "
                    L"Windows gives it back if the volume returns", l->letter);
        } else {
            dd_errstr(e, msg, 256);
            dd__msg(cb, DD_MSG_WARNING, L"cannot put drive letter %lc: back: %ls; Disk Management can assign it",
                    l->letter, msg);
        }
    }
    ep->nletters = 0;
}

/* ---- locking, continued ------------------------------------------------- */

typedef struct lock_ctx {
    dd_endpoint *ep;
    const dd_callbacks *cb;
    bool force;
    bool keep_letters;
    bool reading;               /* lock_input: lock only, never dismount */
    bool check_only;            /* first pass: only look for the system volume */
    bool found_system;
    wchar_t system[MAX_PATH];
    wchar_t system_where[512];
    DWORD err;
} lock_ctx;

static void report_lock(const dd_callbacks *cb, const wchar_t *what, const wchar_t *where,
                        DWORD e, bool force, bool forced, bool reading)
{
    wchar_t msg[256];

    if (reading) {
        if (e == 0) {
            dd__msg(cb, DD_MSG_INFO, L"locked %ls (%ls) while it is read", what, where);
            return;
        }
        dd_errstr(e, msg, 256);
        dd__msg(cb, DD_MSG_ERROR, L"cannot lock %ls (%ls) for reading: %ls; close the programs using it",
                what, where, msg);
        return;
    }
    if (e == 0) {
        if (forced)
            dd__msg(cb, DD_MSG_WARNING,
                    L"forcibly dismounted %ls (%ls); programs with files open on it have lost them, "
                    L"and the volume may be remounted if anything opens it during the copy",
                    what, where);
        else
            dd__msg(cb, DD_MSG_INFO, L"dismounted %ls (%ls)", what, where);
        return;
    }
    dd_errstr(e, msg, 256);
    dd__msg(cb, DD_MSG_ERROR, L"cannot lock %ls (%ls): %ls%ls", what, where, msg,
            force ? L"" : L"; close the programs using it, or force the dismount");
}

static bool lock_volume_on_disk(void *vctx, const dd__volume *v)
{
    lock_ctx *ctx = vctx;
    dd_endpoint *ep = ctx->ep;
    wchar_t where[512], msg[256];
    bool on_disk = false, forced;
    DWORD i, e;
    HANDLE h;

    if (!v->ext)
        return true;
    for (i = 0; i < v->ext->NumberOfDiskExtents; i++)
        if ((LONG)v->ext->Extents[i].DiskNumber == ep->disk_number)
            on_disk = true;
    if (!on_disk)
        return true;

    describe_mounts(v->mounts, where, 512);
    if (ctx->check_only) {
        if (ctx->system[0] && _wcsicmp(v->root, ctx->system) == 0) {
            ctx->found_system = true;
            wcsncpy_s(ctx->system_where, 512, where, _TRUNCATE);
            return false;
        }
        return true;
    }
    if (ep->nlocked >= DD_MAX_LOCKED) {
        ctx->err = ERROR_TOO_MANY_OPEN_FILES;
        return false;
    }
    h = CreateFileW(v->dev, ctx->reading ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        e = GetLastError();
        dd_errstr(e, msg, 256);
        dd__msg(ctx->cb, DD_MSG_ERROR, L"cannot open volume %ls (%ls): %ls", v->dev, where, msg);
        ctx->err = e;
        return false;
    }
    if (ctx->reading) {
        e = try_lock(h, 20);
        forced = false;
    } else {
        e = lock_and_dismount(h, ctx->force, &forced);
    }
    report_lock(ctx->cb, v->dev, where, e, ctx->force, forced, ctx->reading);
    if (e) {
        CloseHandle(h);
        ctx->err = e;
        return false;
    }
    ep->locked[ep->nlocked++] = h;
    if (!ctx->keep_letters)
        take_letters(ep, ctx->cb, v->root, v->mounts, (DWORD)ep->disk_number);
    return true;
}

/* Windows refuses raw writes to sectors that belong to a mounted file
   system, so an output volume, or every volume on an output disk, is
   locked and dismounted first.  The locks are released by ep_close.
   The volume Windows runs from is never touched.  Unless keep_letters, the
   locked volumes' drive letters are removed until ep_close. */
DWORD ep_lock_output(dd_endpoint *ep, const dd_callbacks *cb, bool force, bool keep_letters)
{
    bool forced;
    DWORD e;

    if (ep->kind != EP_BLOCK)
        return 0;

    if (ep->whole_disk && ep->disk_number >= 0) {
        lock_ctx ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ep = ep;
        ctx.cb = cb;
        ctx.force = force;
        ctx.keep_letters = keep_letters;
        if (!system_volume(ctx.system, MAX_PATH))
            ctx.system[0] = 0;
        /* look at every volume before dismounting any of them */
        ctx.check_only = true;
        dd__for_each_volume(lock_volume_on_disk, &ctx);
        if (ctx.found_system) {
            dd__msg(cb, DD_MSG_ERROR, L"refusing to overwrite the disk Windows is running from (%ls)",
                    ctx.system_where);
            return ERROR_ACCESS_DENIED;
        }
        /* any part of it (a spanned or mirrored volume), or not knowing */
        if (on_system((DWORD)ep->disk_number, 0, -1, true)) {
            dd__msg(cb, DD_MSG_ERROR, L"refusing to overwrite '%ls': it holds part of the volume Windows is running "
                    L"from, or that volume cannot be found", ep->name);
            return ERROR_ACCESS_DENIED;
        }
        ctx.check_only = false;
        dd__for_each_volume(lock_volume_on_disk, &ctx);
        return ctx.err;
    }

    if (is_system_volume(ep->h)) {
        dd__msg(cb, DD_MSG_ERROR, L"refusing to overwrite the volume Windows is running from (%ls)", ep->name);
        return ERROR_ACCESS_DENIED;
    }
    e = lock_and_dismount(ep->h, force, &forced);
    if (e && !ep->is_volume && (e == ERROR_INVALID_FUNCTION || e == ERROR_NOT_SUPPORTED ||
                                e == ERROR_INVALID_PARAMETER))
        return 0;   /* nothing mounted on this kind of device */
    report_lock(cb, ep->name, ep->is_volume ? L"volume" : L"device", e, force, forced, false);
    if (!e && !keep_letters)
        take_volume_letters(ep, cb);
    return e;
}

/* lock_input: the input volume, or every volume on an input disk, is
   locked but not dismounted.  Taking the lock makes the file system write
   out what it has cached, and holding it keeps every other program off the
   volume until ep_close, so the image is consistent.  The lock is refused
   while files are open on the volume, which is always the case for the
   volume Windows runs from. */
DWORD ep_lock_input(dd_endpoint *ep, const dd_callbacks *cb, bool keep_letters)
{
    DWORD e;

    if (ep->kind != EP_BLOCK) {
        dd__msg(cb, DD_MSG_WARNING, L"'%ls' is not a disk or volume; there is nothing to lock", ep->name);
        return 0;
    }

    if (ep->whole_disk && ep->disk_number >= 0) {
        lock_ctx ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ep = ep;
        ctx.cb = cb;
        ctx.reading = true;
        ctx.keep_letters = keep_letters;
        if (!system_volume(ctx.system, MAX_PATH))
            ctx.system[0] = 0;
        ctx.check_only = true;
        dd__for_each_volume(lock_volume_on_disk, &ctx);
        if (ctx.found_system) {
            dd__msg(cb, DD_MSG_ERROR, L"the disk Windows is running from (%ls) cannot be locked; read it without locking",
                    ctx.system_where);
            return ERROR_ACCESS_DENIED;
        }
        ctx.check_only = false;
        dd__for_each_volume(lock_volume_on_disk, &ctx);
        return ctx.err;
    }

    if (ep->is_volume && is_system_volume(ep->h)) {
        dd__msg(cb, DD_MSG_ERROR, L"the volume Windows is running from (%ls) cannot be locked; read it without locking",
                ep->name);
        return ERROR_ACCESS_DENIED;
    }
    e = try_lock(ep->h, 20);
    if (e && !ep->is_volume && (e == ERROR_INVALID_FUNCTION || e == ERROR_NOT_SUPPORTED ||
                                e == ERROR_INVALID_PARAMETER))
        return 0;   /* nothing mounted on this kind of device */
    report_lock(cb, ep->name, ep->is_volume ? L"volume" : L"device", e, false, false, true);
    if (!e && !keep_letters)
        take_volume_letters(ep, cb);
    return e;
}

/* ---- I/O --------------------------------------------------------------- */

static DWORD io_at(HANDLE h, bool write, void *buf, DWORD len, int64_t off, DWORD *done)
{
    OVERLAPPED ov;
    BOOL ok;

    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    *done = 0;
    ok = write ? WriteFile(h, buf, len, done, &ov) : ReadFile(h, buf, len, done, &ov);
    if (ok)
        return 0;
    if (!write && GetLastError() == ERROR_HANDLE_EOF) {
        *done = 0;
        return 0;
    }
    return GetLastError();
}

static bool aligned(const dd_endpoint *ep, const void *buf, DWORD len)
{
    DWORD a = ep->align;
    return a == 0 || (ep->pos % a == 0 && len % a == 0 && (uintptr_t)buf % a == 0);
}

static HANDLE buffered_handle(dd_endpoint *ep)
{
    if (!ep->h_buffered) {
        HANDLE h = ReOpenFile(ep->h, ep->access, ep->share, ep->flags & ~(DWORD)FILE_FLAG_NO_BUFFERING);
        if (h == INVALID_HANDLE_VALUE)
            return NULL;
        ep->h_buffered = h;
    }
    return ep->h_buffered;
}

/* Unaligned device I/O through an aligned buffer, at most BOUNCE_SIZE at a
   time.  Writes that cover only part of a sector read it first. */
static DWORD bounce_io(dd_endpoint *ep, bool write, uint8_t *buf, DWORD len, DWORD *done)
{
    DWORD ss = ep->align;

    *done = 0;
    if (!ep->bounce) {
        ep->bounce = VirtualAlloc(NULL, BOUNCE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!ep->bounce)
            return ERROR_NOT_ENOUGH_MEMORY;
    }
    while (*done < len) {
        int64_t cur = ep->pos + *done;
        int64_t base = cur - cur % ss;
        DWORD skip = (DWORD)(cur - base);
        DWORD want = len - *done;
        uint64_t span64 = ((uint64_t)skip + want + ss - 1) / ss * ss;
        DWORD span = span64 > BOUNCE_SIZE ? BOUNCE_SIZE : (DWORD)span64;
        DWORD n = span - skip;
        DWORD got;
        DWORD e;

        if (n > want)
            n = want;
        if (!write || skip != 0 || n != span) {
            e = io_at(ep->h, false, ep->bounce, span, base, &got);
            if (e)
                return e;
            if (!write) {
                if (got <= skip)
                    break;
                if (n > got - skip)
                    n = got - skip;
                memcpy(buf + *done, ep->bounce + skip, n);
                *done += n;
                if (got < span)
                    break;
                continue;
            }
            if (got != span)
                return ERROR_READ_FAULT;
        }
        memcpy(ep->bounce + skip, buf + *done, n);
        e = io_at(ep->h, true, ep->bounce, span, base, &got);
        if (e)
            return e;
        if (got != span)
            return ERROR_WRITE_FAULT;
        *done += n;
    }
    return 0;
}

DWORD ep_read(dd_endpoint *ep, void *buf, DWORD len, DWORD *got)
{
    DWORD e;

    *got = 0;
    if (ep->xz) {
        e = dd__xz_read(ep->xz, buf, len, got);
        ep->pos += *got;            /* in the uncompressed image */
        return e;
    }
    switch (ep->kind) {
    case EP_ZERO:
        memset(buf, 0, len);
        *got = len;
        return 0;

    case EP_RANDOM:
        if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, buf, len, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            return ERROR_GEN_FAILURE;
        *got = len;
        return 0;

    case EP_NULL:
        return 0;

    case EP_STREAM:
        if (ReadFile(ep->h, buf, len, got, NULL))
            return 0;
        e = GetLastError();
        if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) {
            *got = 0;
            return 0;   /* writer closed the pipe: end of input */
        }
        return e;

    case EP_FILE: {
        HANDLE h = ep->h;
        if (ep->direct && !aligned(ep, buf, len)) {
            h = buffered_handle(ep);
            if (!h)
                return GetLastError();
        }
        e = io_at(h, false, buf, len, ep->pos, got);
        ep->pos += *got;
        return e;
    }

    case EP_BLOCK:
        if (ep->size >= 0) {
            if (ep->pos >= ep->size)
                return 0;
            if ((int64_t)len > ep->size - ep->pos)
                len = (DWORD)(ep->size - ep->pos);
        }
        if (aligned(ep, buf, len))
            e = io_at(ep->h, false, buf, len, ep->pos, got);
        else
            e = bounce_io(ep, false, buf, len, got);
        ep->pos += *got;
        return e;
    }
    return ERROR_INVALID_FUNCTION;
}

DWORD ep_write(dd_endpoint *ep, const void *cbuf, DWORD len, DWORD *put)
{
    void *buf = (void *)cbuf;
    DWORD e;

    *put = 0;
    if (ep->xz) {
        e = dd__xz_write(ep->xz, buf, len);
        if (!e)
            *put = len;
        return e;
    }
    switch (ep->kind) {
    case EP_NULL:
    case EP_ZERO:
    case EP_RANDOM:
        *put = len;
        return 0;

    case EP_STREAM:
        while (*put < len) {
            DWORD n;
            if (!WriteFile(ep->h, (uint8_t *)buf + *put, len - *put, &n, NULL))
                return GetLastError();
            if (n == 0)
                return ERROR_WRITE_FAULT;
            *put += n;
        }
        return 0;

    case EP_FILE: {
        HANDLE h = ep->h;
        if (ep->direct && !aligned(ep, buf, len)) {
            h = buffered_handle(ep);
            if (!h)
                return GetLastError();
        }
        e = io_at(h, true, buf, len, ep->pos, put);
        ep->pos += *put;
        return e;
    }

    case EP_BLOCK: {
        bool full = false;
        if (ep->size >= 0) {
            if (ep->pos >= ep->size)
                return ERROR_DISK_FULL;
            if ((int64_t)len > ep->size - ep->pos) {
                len = (DWORD)(ep->size - ep->pos);
                full = true;
            }
        }
        if (aligned(ep, buf, len))
            e = io_at(ep->h, true, buf, len, ep->pos, put);
        else
            e = bounce_io(ep, true, buf, len, put);
        ep->pos += *put;
        if (!e && full)
            e = ERROR_DISK_FULL;
        return e;
    }
    }
    return ERROR_INVALID_FUNCTION;
}

DWORD ep_set_eof(dd_endpoint *ep, int64_t length)
{
    FILE_END_OF_FILE_INFO eof;

    if (ep->kind != EP_FILE)
        return 0;
    eof.EndOfFile.QuadPart = length;
    if (!SetFileInformationByHandle(ep->h, FileEndOfFileInfo, &eof, sizeof eof))
        return GetLastError();
    ep->size = length;
    return 0;
}

/* After conv=sparse skipped the last blocks, the file must still end at the
   output position. */
DWORD ep_extend_to_pos(dd_endpoint *ep)
{
    LARGE_INTEGER sz;

    if (ep->kind != EP_FILE)
        return 0;
    if (!GetFileSizeEx(ep->h, &sz))
        return GetLastError();
    if (sz.QuadPart >= ep->pos)
        return 0;
    return ep_set_eof(ep, ep->pos);
}

/* A dismounted volume refuses a flush with ERROR_NOT_READY.  Its writes
   went straight to the volume device, past any file system cache, so
   flushing the disks under it is what is left to do. */
static DWORD flush_volume_disks(HANDLE vol)
{
    BYTE buf[sizeof(VOLUME_DISK_EXTENTS) + 32 * sizeof(DISK_EXTENT)];
    const VOLUME_DISK_EXTENTS *ext = (const VOLUME_DISK_EXTENTS *)buf;
    wchar_t name[64];
    DWORD ret, i, e = 0;
    HANDLE h;

    if (!DeviceIoControl(vol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, buf, sizeof buf, &ret, NULL))
        return GetLastError();
    for (i = 0; i < ext->NumberOfDiskExtents; i++) {
        _snwprintf_s(name, 64, _TRUNCATE, L"\\\\.\\PhysicalDrive%lu", ext->Extents[i].DiskNumber);
        h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            return GetLastError();
        if (!FlushFileBuffers(h))
            e = GetLastError();
        CloseHandle(h);
        if (e)
            return e;
    }
    return 0;
}

/* ---- .xz ----------------------------------------------------------------- */

DWORD ep_detect_xz(dd_endpoint *ep, bool *found, uint64_t memlimit)
{
    uint8_t head[8];
    HANDLE h = ep->h;
    DWORD got = 0, e;

    *found = false;
    if (ep->kind != EP_FILE || ep->size < (int64_t)sizeof head)
        return 0;
    if (ep->direct && (h = buffered_handle(ep)) == NULL)
        return GetLastError();
    e = io_at(h, false, head, sizeof head, 0, &got);
    if (e || !dd__xz_magic(head, got))
        return e;

    ep->xz_file_size = ep->size;
    e = dd__xz_info(h, ep->size, &ep->size);
    if (!e)
        e = dd__xz_decoder(&ep->xz, h, memlimit);
    if (e) {
        ep->size = ep->xz_file_size;
        return e;
    }
    ep->kind = EP_STREAM;       /* read in order; skip= reads and discards */
    ep->pos = 0;
    *found = true;
    return 0;
}

DWORD ep_start_compress(dd_endpoint *ep, int level, uint64_t memlimit)
{
    LARGE_INTEGER zero;
    DWORD e;

    switch (ep->kind) {
    case EP_NULL:
        return 0;               /* nothing is kept anyway */
    case EP_BLOCK:
        return DD_ERROR_XZ_DEVICE;
    case EP_FILE:
        if (ep->own_handle) {
            if ((e = ep_set_eof(ep, 0)) != 0)
                return e;
            zero.QuadPart = 0;
            SetFilePointerEx(ep->h, zero, NULL, FILE_BEGIN);
        }
        break;
    default:
        break;
    }
    e = dd__xz_encoder(&ep->xz, ep->h, level, memlimit);
    if (e)
        return e;
    ep->kind = EP_STREAM;
    ep->size = -1;
    return 0;
}

uint32_t ep_xz_threads(const dd_endpoint *ep)
{
    return dd__xz_threads(ep->xz);
}

DWORD ep_finish_compress(dd_endpoint *ep, int64_t *compressed, void (*tick)(void *), void *ctx)
{
    *compressed = 0;
    return ep->xz ? dd__xz_finish(ep->xz, compressed, tick, ctx) : 0;
}

DWORD ep_flush(dd_endpoint *ep)
{
    if (ep->xz)
        return FlushFileBuffers(ep->h) ? 0 : GetLastError();
    if (ep->kind != EP_FILE && ep->kind != EP_BLOCK)
        return 0;
    if (ep->h_buffered && !FlushFileBuffers(ep->h_buffered))
        return GetLastError();
    if (!FlushFileBuffers(ep->h)) {
        DWORD e = GetLastError();
        if (e == ERROR_NOT_READY && ep->is_volume)
            return flush_volume_disks(ep->h);
        return e;
    }
    return 0;
}

void ep_close(dd_endpoint *ep)
{
    int i;

    if (ep->h != INVALID_HANDLE_VALUE) {
        if (ep->own_handle) {
            if (ep->kind == EP_BLOCK && ep->whole_disk && (ep->access & GENERIC_WRITE)) {
                /* make Windows re-read a partition table we may have written */
                DWORD ret;
                DeviceIoControl(ep->h, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &ret, NULL);
            }
            CloseHandle(ep->h);
        } else if (ep->kind == EP_FILE) {
            /* leave a redirected standard handle just after what we used */
            LARGE_INTEGER p;
            p.QuadPart = ep->pos;
            SetFilePointerEx(ep->h, p, NULL, FILE_BEGIN);
        }
    }
    if (ep->h_buffered)
        CloseHandle(ep->h_buffered);
    for (i = 0; i < ep->nlocked; i++)
        CloseHandle(ep->locked[i]);
    if (ep->nletters)
        restore_letters(ep);
    if (ep->bounce)
        VirtualFree(ep->bounce, 0, MEM_RELEASE);
    dd__xz_free(ep->xz);
    free(ep->path);
    ep_init(ep);
}
