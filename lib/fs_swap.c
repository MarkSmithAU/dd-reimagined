/*
    dd reimagined for Windows Copyright (C) 2026 Mark Smith
    dd Copyright (C) 2003 John Newbigin
    GPL version 2 or later

    Linux swap for sparse reading.  What a swap area holds means nothing
    once the system that wrote it has stopped, so only its header page is
    read: the rest is written as zeros, which Linux uses as it is.  The
    exception is a hibernation image, which the next boot resumes from:
    the hibernating kernel replaces the signature with its own, and such an
    area is read whole.

    Only 4 KiB pages are recognised (x86 and most ARM systems); a swap area
    made with larger pages is read whole as an unknown partition.
*/
#include "fs.h"
#include <string.h>

#define PAGE     4096u
#define SIG      (PAGE - 10)        /* the signature: the page's last 10 bytes */

static const char *const hibernated[] = {
    "S1SUSPEND", "S2SUSPEND", "ULSUSPEND", "LINHIB0001", "\xed\xc3\x02\xe9\x98\x56\xe5\x0c",
};

static bool is_hibernated(const uint8_t *head)
{
    size_t i;

    for (i = 0; i < sizeof hibernated / sizeof *hibernated; i++)
        if (memcmp(head + SIG, hibernated[i], strlen(hibernated[i])) == 0)
            return true;
    return false;
}

static bool swap_probe(const uint8_t *head, int64_t vol_size)
{
    if (vol_size < 2 * PAGE)
        return false;
    return memcmp(head + SIG, "SWAPSPACE2", 10) == 0 || is_hibernated(head);
}

static dd__fs_status swap_map(dd__fs_vol *v, const uint8_t *head, bool allow_unclean, dd__fs_map *m)
{
    uint32_t version = dd__le32(head + 1024), last = dd__le32(head + 1028);
    size_t i;

    (void)v;
    (void)allow_unclean;
    wcscpy_s(m->type, sizeof m->type / sizeof *m->type, L"swap");
    if (is_hibernated(head)) {
        dd__fs_note(m, L"it holds a hibernation image, which the next boot resumes from");
        return DD__FS_WHOLE;
    }
    if (version != 1 || last < 1) {
        dd__fs_note(m, L"its header is not one dd knows (version %u)", version);
        return DD__FS_WHOLE;
    }
    /* mkswap clears the first 1 KiB.  Anything there now (a LUKS header,
       say) was written over the swap area since, and the header is stale. */
    for (i = 0; i < 1024; i++)
        if (head[i]) {
            dd__fs_note(m, L"its first 1 KiB is not blank, so something else may have replaced it");
            return DD__FS_WHOLE;
        }
    /* pages 1 to last are swap space; page 0 is the header, always read */
    m->size = ((int64_t)last + 1) * PAGE;
    m->unit = PAGE;
    m->map_start = PAGE;
    if (!dd__fs_alloc_bits(m, last)) {
        dd__fs_note(m, L"there is not enough memory to map it");
        return DD__FS_WHOLE;
    }
    return DD__FS_MAPPED;          /* every bit clear: all of it is free */
}

const dd__fs_driver dd__fs_swap = { swap_probe, swap_map };
