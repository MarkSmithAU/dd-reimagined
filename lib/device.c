/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later

    Version 0.6 walked the NT object directory through ntdll.  This uses
    only documented interfaces: SetupAPI device interfaces for disks and
    optical drives, the Volume API for volumes and mount points, and
    storage IOCTLs to tie volumes to disks.
*/
#include "dd_internal.h"
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

static const GUID guid_devinterface_disk =
    { 0x53f56307, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };
static const GUID guid_devinterface_cdrom =
    { 0x53f56308, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };
static const GUID guid_partition_msft_reserved =
    { 0xe3c9e316, 0x0b5c, 0x4db8, { 0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae } };

static HANDLE open_device(const wchar_t *path, DWORD access)
{
    return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

bool dd__device_number(HANDLE h, STORAGE_DEVICE_NUMBER *n)
{
    DWORD ret;
    return DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, n, sizeof *n, &ret, NULL) != 0;
}

static void copy_descriptor_string(const BYTE *buf, DWORD len, DWORD offset, wchar_t *out, size_t n)
{
    size_t i = 0;
    const char *s;

    out[0] = 0;
    if (offset == 0 || offset >= len || n == 0)
        return;
    s = (const char *)buf + offset;
    while (*s == ' ')
        s++;
    while (i + 1 < n && s < (const char *)buf + len && *s)
        out[i++] = (wchar_t)(unsigned char)*s++;
    while (i > 0 && out[i - 1] == L' ')
        i--;
    out[i] = 0;
}

bool dd__storage_info(HANDLE h, int *bus, bool *removable,
                      wchar_t *vendor, size_t vn, wchar_t *product, size_t pn)
{
    STORAGE_PROPERTY_QUERY q;
    BYTE buf[1024];
    const STORAGE_DEVICE_DESCRIPTOR *d = (const STORAGE_DEVICE_DESCRIPTOR *)buf;
    DWORD ret;

    memset(&q, 0, sizeof q);
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &ret, NULL))
        return false;
    *bus = (int)d->BusType;
    *removable = d->RemovableMedia != 0;
    copy_descriptor_string(buf, ret, d->VendorIdOffset, vendor, vn);
    copy_descriptor_string(buf, ret, d->ProductIdOffset, product, pn);
    return true;
}

bool dd__bus_is_removable(int bus)
{
    return bus == BusTypeUsb || bus == BusTypeSd || bus == BusTypeMmc;
}

bool dd__geometry(HANDLE h, int64_t *size, DWORD *sector, int *media)
{
    BYTE buf[512];
    DISK_GEOMETRY g;
    DWORD ret;

    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, buf, sizeof buf, &ret, NULL)) {
        const DISK_GEOMETRY_EX *gx = (const DISK_GEOMETRY_EX *)buf;
        *size = gx->DiskSize.QuadPart;
        *sector = gx->Geometry.BytesPerSector;
        *media = gx->Geometry.MediaType;
        return true;
    }
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &g, sizeof g, &ret, NULL)) {
        *size = g.Cylinders.QuadPart * g.TracksPerCylinder * g.SectorsPerTrack * g.BytesPerSector;
        *sector = g.BytesPerSector;
        *media = g.MediaType;
        return true;
    }
    return false;
}

/* Partitions that hold data: not empty MBR slots, not extended partition
   containers and not the GPT Microsoft reserved partition.  -1 on error
   (GetLastError is preserved). */
int dd__count_partitions(HANDLE h, int *style)
{
    DWORD size = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 127 * sizeof(PARTITION_INFORMATION_EX);
    DRIVE_LAYOUT_INFORMATION_EX *l = NULL;
    DWORD ret, i;
    int n = 0;

    for (;;) {
        l = malloc(size);
        if (!l) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return -1;
        }
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, l, size, &ret, NULL))
            break;
        free(l);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return -1;
        size *= 2;
    }
    *style = (int)l->PartitionStyle;
    for (i = 0; i < l->PartitionCount; i++) {
        const PARTITION_INFORMATION_EX *p = &l->PartitionEntry[i];
        if (p->PartitionLength.QuadPart == 0)
            continue;
        if (l->PartitionStyle == PARTITION_STYLE_MBR) {
            BYTE t = p->Mbr.PartitionType;
            if (t == PARTITION_ENTRY_UNUSED || t == PARTITION_EXTENDED || t == PARTITION_XINT13_EXTENDED)
                continue;
        } else if (l->PartitionStyle == PARTITION_STYLE_GPT) {
            if (memcmp(&p->Gpt.PartitionType, &guid_partition_msft_reserved, sizeof(GUID)) == 0)
                continue;
        }
        n++;
    }
    free(l);
    return n;
}

void dd__for_each_volume(dd__volume_fn fn, void *ctx)
{
    wchar_t root[MAX_PATH], dev[MAX_PATH];
    BYTE ebuf[sizeof(VOLUME_DISK_EXTENTS) + 32 * sizeof(DISK_EXTENT)];
    HANDLE find = FindFirstVolumeW(root, MAX_PATH);

    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        dd__volume v;
        wchar_t *mounts = NULL;
        DWORD len = 0, ret;
        size_t dl;
        bool more;

        wcscpy_s(dev, MAX_PATH, root);
        dl = wcslen(dev);
        if (dl > 0 && dev[dl - 1] == L'\\')
            dev[dl - 1] = 0;

        if (!GetVolumePathNamesForVolumeNameW(root, NULL, 0, &len) && GetLastError() == ERROR_MORE_DATA) {
            mounts = calloc(len + 1, sizeof *mounts);
            if (mounts && !GetVolumePathNamesForVolumeNameW(root, mounts, len, &len))
                mounts[0] = mounts[1] = 0;
        }

        memset(&v, 0, sizeof v);
        v.root = root;
        v.dev = dev;
        v.mounts = mounts ? mounts : L"\0";
        v.h = open_device(dev, 0);
        if (v.h != INVALID_HANDLE_VALUE &&
            DeviceIoControl(v.h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, ebuf, sizeof ebuf, &ret, NULL))
            v.ext = (const VOLUME_DISK_EXTENTS *)ebuf;

        more = fn(ctx, &v);
        if (v.h != INVALID_HANDLE_VALUE)
            CloseHandle(v.h);
        free(mounts);
        if (!more)
            break;
    } while (FindNextVolumeW(find, root, MAX_PATH));
    FindVolumeClose(find);
}

/* ---- listing ----------------------------------------------------------- */

static bool grow(void **array, size_t *cap, size_t count, size_t elem)
{
    void *p;
    size_t n;

    if (count < *cap)
        return true;
    n = *cap ? *cap * 2 : 8;
    p = realloc(*array, n * elem);
    if (!p)
        return false;
    *array = p;
    *cap = n;
    return true;
}

typedef struct list_ctx {
    dd_device_list *list;
    size_t disk_cap, volume_cap;
    dd_device_kind kind;
} list_ctx;

static void add_disk(list_ctx *ctx, const wchar_t *interface_path)
{
    dd_device_list *list = ctx->list;
    STORAGE_DEVICE_NUMBER sdn;
    dd_disk_info *d;
    HANDLE h, hr;
    int media;

    h = open_device(interface_path, 0);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (!dd__device_number(h, &sdn) ||
        !grow((void **)&list->disks, &ctx->disk_cap, list->ndisks, sizeof *list->disks)) {
        CloseHandle(h);
        return;
    }
    d = &list->disks[list->ndisks++];
    memset(d, 0, sizeof *d);
    d->kind = ctx->kind;
    d->number = sdn.DeviceNumber;
    if (wcslen(interface_path) < sizeof d->interface_path / sizeof *d->interface_path)
        wcscpy_s(d->interface_path, sizeof d->interface_path / sizeof *d->interface_path, interface_path);
    _snwprintf_s(d->path, 32, _TRUNCATE,
                 ctx->kind == DD_DEV_CDROM ? L"\\\\.\\CdRom%lu" : L"\\\\.\\PhysicalDrive%lu",
                 sdn.DeviceNumber);
    d->bus_type = -1;
    d->size = -1;
    d->partition_style = -1;
    d->partitions = -1;
    if (dd__storage_info(h, &d->bus_type, &d->removable, d->vendor, 64, d->product, 64))
        d->removable = d->removable || dd__bus_is_removable(d->bus_type);
    d->media_present = dd__geometry(h, &d->size, &d->sector_size, &media);
    CloseHandle(h);

    if (ctx->kind == DD_DEV_DISK && d->media_present) {
        /* the layout needs read access, which needs administrator rights */
        hr = open_device(d->path, GENERIC_READ);
        if (hr != INVALID_HANDLE_VALUE) {
            d->partitions = dd__count_partitions(hr, &d->partition_style);
            if (d->partitions < 0)
                d->partition_style = -1;
            CloseHandle(hr);
        }
    }
}

typedef bool (*interface_fn)(void *ctx, const wchar_t *path);

/* Calls fn with the path of each present interface of class guid until it
   returns false. */
static void for_each_interface(const GUID *guid, interface_fn fn, void *ctx)
{
    SP_DEVICE_INTERFACE_DATA ifd;
    HDEVINFO di = SetupDiGetClassDevsW(guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    DWORD i;
    bool more = true;

    if (di == INVALID_HANDLE_VALUE)
        return;
    ifd.cbSize = sizeof ifd;
    for (i = 0; more && SetupDiEnumDeviceInterfaces(di, NULL, guid, i, &ifd); i++) {
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;
        DWORD need = 0;

        SetupDiGetDeviceInterfaceDetailW(di, &ifd, NULL, 0, &need, NULL);
        if (need == 0)
            continue;
        detail = malloc(need);
        if (!detail)
            continue;
        detail->cbSize = sizeof *detail;
        if (SetupDiGetDeviceInterfaceDetailW(di, &ifd, detail, need, NULL, NULL))
            more = fn(ctx, detail->DevicePath);
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(di);
}

static bool add_disk_fn(void *ctx, const wchar_t *path)
{
    add_disk(ctx, path);
    return true;
}

static void enum_interfaces(list_ctx *ctx, const GUID *guid)
{
    for_each_interface(guid, add_disk_fn, ctx);
}

typedef struct find_ctx {
    DWORD number;
    wchar_t *out;
    size_t n;
    bool found;
} find_ctx;

static bool find_disk_fn(void *vctx, const wchar_t *path)
{
    find_ctx *ctx = vctx;
    STORAGE_DEVICE_NUMBER sdn;
    HANDLE h = open_device(path, 0);
    bool match;

    if (h == INVALID_HANDLE_VALUE)
        return true;
    match = dd__device_number(h, &sdn) && sdn.DeviceType == FILE_DEVICE_DISK &&
            sdn.DeviceNumber == ctx->number;
    CloseHandle(h);
    if (match && wcslen(path) < ctx->n) {
        wcscpy_s(ctx->out, ctx->n, path);
        ctx->found = true;
    }
    return !match;
}

/* The device interface path of the disk numbered number right now. */
static bool disk_interface(DWORD number, wchar_t *out, size_t n)
{
    find_ctx ctx;

    ctx.number = number;
    ctx.out = out;
    ctx.n = n;
    ctx.found = false;
    for_each_interface(&guid_devinterface_disk, find_disk_fn, &ctx);
    return ctx.found;
}

static bool single_extent_disk(HANDLE h, DWORD *disk)
{
    BYTE buf[sizeof(VOLUME_DISK_EXTENTS) + 32 * sizeof(DISK_EXTENT)];
    const VOLUME_DISK_EXTENTS *ext = (const VOLUME_DISK_EXTENTS *)buf;
    DWORD ret;

    if (!DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, buf, sizeof buf, &ret, NULL) ||
        ext->NumberOfDiskExtents != 1)
        return false;
    *disk = ext->Extents[0].DiskNumber;
    return true;
}

static bool add_volume(void *vctx, const dd__volume *v)
{
    list_ctx *ctx = vctx;
    dd_device_list *list = ctx->list;
    dd_volume_info *vi;
    STORAGE_DEVICE_NUMBER sdn;
    const wchar_t *m;
    size_t mlen;
    DWORD sector;
    int media;

    if (!grow((void **)&list->volumes, &ctx->volume_cap, list->nvolumes, sizeof *list->volumes))
        return false;
    vi = &list->volumes[list->nvolumes++];
    memset(vi, 0, sizeof *vi);
    wcsncpy_s(vi->path, 64, v->dev, _TRUNCATE);
    vi->offset = -1;
    vi->size = -1;
    vi->parent_kind = DD_DEV_NONE;

    for (m = v->mounts; *m; m += wcslen(m) + 1)
        ;
    mlen = (size_t)(m - v->mounts) + 1;
    vi->mounts = malloc((mlen + 1) * sizeof *vi->mounts);
    if (vi->mounts) {
        memcpy(vi->mounts, v->mounts, mlen * sizeof *vi->mounts);
        vi->mounts[mlen] = 0;
    }

    vi->drive_type = GetDriveTypeW(v->root);
    if (!GetVolumeInformationW(v->root, vi->label, MAX_PATH + 1, NULL, NULL, NULL,
                               vi->filesystem, MAX_PATH + 1)) {
        vi->label[0] = 0;
        vi->filesystem[0] = 0;
    }

    if (v->ext) {
        DWORD i;
        if (v->ext->NumberOfDiskExtents == 1) {
            vi->parent_kind = DD_DEV_DISK;
            vi->parent_number = v->ext->Extents[0].DiskNumber;
            vi->offset = v->ext->Extents[0].StartingOffset.QuadPart;
        }
        vi->size = 0;
        for (i = 0; i < v->ext->NumberOfDiskExtents; i++)
            vi->size += v->ext->Extents[i].ExtentLength.QuadPart;
    } else if (v->h != INVALID_HANDLE_VALUE) {
        if (dd__device_number(v->h, &sdn) &&
            (sdn.DeviceType == FILE_DEVICE_CD_ROM || sdn.DeviceType == FILE_DEVICE_DVD)) {
            vi->parent_kind = DD_DEV_CDROM;
            vi->parent_number = sdn.DeviceNumber;
        }
        if (!dd__geometry(v->h, &vi->size, &sector, &media))
            vi->size = -1;
    }
    return true;
}

static int compare_disks(const void *a, const void *b)
{
    const dd_disk_info *x = a, *y = b;
    if (x->kind != y->kind)
        return x->kind < y->kind ? -1 : 1;
    return x->number < y->number ? -1 : x->number > y->number;
}

static int compare_volumes(const void *a, const void *b)
{
    const dd_volume_info *x = a, *y = b;
    const wchar_t *mx = x->mounts && x->mounts[0] ? x->mounts : L"\xffff";
    const wchar_t *my = y->mounts && y->mounts[0] ? y->mounts : L"\xffff";
    if (x->parent_kind != y->parent_kind)
        return x->parent_kind < y->parent_kind ? -1 : 1;
    if (x->parent_number != y->parent_number)
        return x->parent_number < y->parent_number ? -1 : 1;
    if (x->offset != y->offset)
        return x->offset < y->offset ? -1 : 1;
    return _wcsicmp(mx, my);
}

bool dd_list_devices(dd_device_list *list)
{
    list_ctx ctx;

    memset(list, 0, sizeof *list);
    memset(&ctx, 0, sizeof ctx);
    ctx.list = list;
    ctx.kind = DD_DEV_DISK;
    enum_interfaces(&ctx, &guid_devinterface_disk);
    ctx.kind = DD_DEV_CDROM;
    enum_interfaces(&ctx, &guid_devinterface_cdrom);
    dd__for_each_volume(add_volume, &ctx);

    if (list->ndisks)
        qsort(list->disks, list->ndisks, sizeof *list->disks, compare_disks);
    if (list->nvolumes)
        qsort(list->volumes, list->nvolumes, sizeof *list->volumes, compare_volumes);
    return true;
}

void dd_free_device_list(dd_device_list *list)
{
    size_t i;
    for (i = 0; i < list->nvolumes; i++)
        free(list->volumes[i].mounts);
    free(list->volumes);
    free(list->disks);
    memset(list, 0, sizeof *list);
}

/* ---- id= / od= --------------------------------------------------------- */

static bool is_floppy_media(int media)
{
    return (media >= F5_1Pt2_512 && media <= F5_160_512) || (media >= F3_120M_512 && media <= 25);
}

static void fail(wchar_t *err, size_t errlen, const wchar_t *fmt, const wchar_t *a, DWORD e)
{
    wchar_t msg[256];
    dd_errstr(e, msg, 256);
    _snwprintf_s(err, errlen, _TRUNCATE, fmt, a, msg);
}

bool dd_resolve_drive(const wchar_t *spec, dd_resolved *out, wchar_t *err, size_t errlen)
{
    wchar_t mount[MAX_PATH], vol[MAX_PATH], volume_dev[MAX_PATH];
    BYTE ebuf[sizeof(VOLUME_DISK_EXTENTS) + 32 * sizeof(DISK_EXTENT)];
    const VOLUME_DISK_EXTENTS *ext = NULL;
    STORAGE_DEVICE_NUMBER sdn;
    bool letter = false, have_sdn;
    size_t len = wcslen(spec);
    DWORD ret;
    UINT type;
    HANDLE h;

    memset(out, 0, sizeof *out);

    /* "E", "E:" and "E:\" all mean drive E; anything else is a mount point */
    if (len >= 1 && len <= 3 && iswalpha(spec[0]) &&
        (len == 1 || (spec[1] == L':' && (len == 2 || spec[2] == L'\\' || spec[2] == L'/')))) {
        _snwprintf_s(mount, MAX_PATH, _TRUNCATE, L"%lc:\\", towupper(spec[0]));
        letter = true;
    } else {
        wcsncpy_s(mount, MAX_PATH, spec, _TRUNCATE);
        len = wcslen(mount);
        if (len > 0 && mount[len - 1] != L'\\')
            wcsncat_s(mount, MAX_PATH, L"\\", _TRUNCATE);
    }

    if (!GetVolumeNameForVolumeMountPointW(mount, vol, MAX_PATH)) {
        fail(err, errlen, L"'%ls' is not a drive or mount point: %ls", spec, GetLastError());
        return false;
    }
    if (letter)
        _snwprintf_s(volume_dev, MAX_PATH, _TRUNCATE, L"\\\\.\\%lc:", mount[0]);
    else {
        wcsncpy_s(volume_dev, MAX_PATH, vol, _TRUNCATE);
        len = wcslen(volume_dev);
        if (len > 0 && volume_dev[len - 1] == L'\\')
            volume_dev[len - 1] = 0;
    }

    type = GetDriveTypeW(mount);
    h = open_device(volume_dev, 0);
    if (h == INVALID_HANDLE_VALUE) {
        fail(err, errlen, L"cannot open '%ls': %ls", volume_dev, GetLastError());
        return false;
    }
    have_sdn = dd__device_number(h, &sdn);

    if (type == DRIVE_CDROM || (have_sdn && (sdn.DeviceType == FILE_DEVICE_CD_ROM ||
                                             sdn.DeviceType == FILE_DEVICE_DVD))) {
        if (have_sdn)
            _snwprintf_s(out->path, MAX_PATH, _TRUNCATE, L"\\\\.\\CdRom%lu", sdn.DeviceNumber);
        else
            wcsncpy_s(out->path, MAX_PATH, volume_dev, _TRUNCATE);
        wcscpy_s(out->name, MAX_PATH, out->path);
        out->default_bs = 2048;
        out->what = L"optical drive";
        CloseHandle(h);
        return true;
    }

    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, ebuf, sizeof ebuf, &ret, NULL))
        ext = (const VOLUME_DISK_EXTENTS *)ebuf;

    if (!ext) {
        int64_t size;
        DWORD sector;
        int media;
        bool floppy = dd__geometry(h, &size, &sector, &media) && is_floppy_media(media);
        CloseHandle(h);
        if (floppy) {
            wcsncpy_s(out->path, MAX_PATH, volume_dev, _TRUNCATE);
            wcscpy_s(out->name, MAX_PATH, out->path);
            out->default_bs = 18 * 1024;   /* one track of a 1.44 MB floppy */
            out->what = L"floppy disk";
            return true;
        }
        _snwprintf_s(err, errlen, _TRUNCATE, L"cannot find the disk that holds %ls", spec);
        return false;
    }

    if (ext->NumberOfDiskExtents != 1) {
        CloseHandle(h);
        _snwprintf_s(err, errlen, _TRUNCATE, L"%ls spans %lu disks; name the disk with if= or of=",
                     spec, ext->NumberOfDiskExtents);
        return false;
    } else {
        wchar_t disk[32];
        DWORD number = ext->Extents[0].DiskNumber, again;
        int style, parts;
        HANDLE hd;

        _snwprintf_s(disk, 32, _TRUNCATE, L"\\\\.\\PhysicalDrive%lu", number);
        hd = open_device(disk, GENERIC_READ);
        if (hd == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            CloseHandle(h);
            fail(err, errlen, L"cannot open '%ls' to count its partitions: %ls", disk, e);
            if (e == ERROR_ACCESS_DENIED)
                wcsncat_s(err, errlen, L" (raw disk access needs an elevated prompt)", _TRUNCATE);
            return false;
        }
        parts = dd__count_partitions(hd, &style);
        if (parts < 0) {
            fail(err, errlen, L"cannot read the partition table of '%ls': %ls", disk, GetLastError());
            CloseHandle(hd);
            CloseHandle(h);
            return false;
        }
        CloseHandle(hd);
        if (parts > 1) {
            CloseHandle(h);
            _snwprintf_s(err, errlen, _TRUNCATE,
                         L"%ls is one of %d partitions on %ls; use %ls for the whole disk or %ls for this partition",
                         spec, parts, disk, disk, volume_dev);
            return false;
        }
        /* Disk numbers are reassigned as devices come and go.  Bind to the
           device interface, and check that the volume is still on the disk
           with that number now that the interface has been found. */
        if (!disk_interface(number, out->path, MAX_PATH) ||
            !single_extent_disk(h, &again) || again != number) {
            CloseHandle(h);
            _snwprintf_s(err, errlen, _TRUNCATE,
                         L"the disks changed while %ls was being looked up; try again", spec);
            return false;
        }
        CloseHandle(h);
        wcscpy_s(out->name, MAX_PATH, disk);
        out->default_bs = 1024 * 1024;
        out->what = L"whole disk";
        return true;
    }
}
