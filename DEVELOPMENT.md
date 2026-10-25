# Developing dd reimagined for Windows

[README.md](README.md) describes what the programs do; [CHANGES.md](CHANGES.md)
has the release notes. This file is for contributors: code layout, engine
API, building, testing, releasing, design decisions and open items. It
describes release 1.0.0.

## Background

dd reimagined for Windows is a C rewrite of John Newbigin's Delphi tool
"dd for Windows 0.6beta3" (GPL v2 or later). Design goals: supported
Windows APIs only (no ntdll); GNU dd semantics; Windows 7 or later, x64;
one copy engine shared by the command line and the GUI.

## Files

    include/dd.h          public engine API (options, dd_run, callbacks, cancel, devices)
    include/dd_version.h  version, product name, copyright: the one place they are set
    lib/dd_internal.h     endpoint struct and internal helpers
    lib/options.c         operand parsing, GNU number suffixes, validation
    lib/copy.c            dd_run(): skip/seek/truncate, the copy loop after GNU dd_copy(),
                          hash checking, --verify
    lib/endpoint.c        open/classify files, devices, pipes, /dev/*; aligned I/O,
                          bounce buffer, read-modify-write; volume lock/dismount;
                          system-volume protection
    lib/device.c          SetupAPI/Volume API enumeration, partition counting, id=/od=
    lib/format.c          GNU-format stats, human and exact sizes, error text
    lib/layout.c          MBR/GPT/ISO 9660 reading, data end, GPT rebuild for --fit
    lib/xz.c              .xz through liblzma (the only file that uses it)
    lib/hash.c            SHA-256 (CNG) and .hashes files
    lib/fs.h              --sparse-read: the file system driver interface
    lib/fsmap.c           --sparse-read: input volumes, driver table, used-space map
                          and run queries, dd_fs_report_read()
    lib/fs_fat.c, fs_exfat.c, fs_ntfs.c, fs_ext.c, fs_swap.c
                          drivers: FAT12/16/32, exFAT, NTFS, ext2/3/4 (+ jbd2), Linux swap
    cli/main.c, dd.rc     dd.exe front end and its version resource
    gui/gui.c, gui.rc, resource.h, dd-gui.manifest, dd.ico
                          dd-gui.exe (Win32 dialog, worker thread; requireAdministrator,
                          comctl6, per-monitor v2 DPI)
    gui/make-icon.py      draws dd.ico, the giraffe (16-256 px); needs Python, Pillow
                          and numpy; not part of the build
    third_party/xz/       liblzma from XZ Utils 5.8.4, signature-verified; see README.dd.md
    tests/                PowerShell test suites (below); regenerate-fixtures.ps1
                          (elevated) generates tests/fixtures, which is not in the
                          source tree; screenshot.ps1: screenshots with paths masked
    docs/                 user documentation; docs/images: its screenshots, made by
                          docs/make-screenshots.ps1 (elevated)
    build.cmd             MSVC build without CMake -> build\dd.exe, build\dd-gui.exe
    CMakeLists.txt        the same via CMake (targets lzma, ddcore, dd, dd-gui)
    release.ps1           clean build, file-only tests, zips in dist\
    clean.ps1             removes build\, dist\, tests\fixtures and test leftovers
    .github/workflows/release.yml  runs release.ps1 when a GitHub release is
                          published, and attaches the zips to it

## The engine API

The engine (`ddcore`: all of `lib/`) never writes to the console. A front
end fills in `dd_options` and supplies `dd_callbacks`:

- `message` gets errors, warnings and info lines;
- `progress` is called from the copy thread every `progress_interval_ms`
  with a `dd_stats`, including `bytes_total` when the size is known and
  `bytes_unused` for `--sparse-read`;
- `dd_cancel_request()` works from any thread and also aborts a blocked
  read or write, for example on a pipe.

`gui/gui.c` shows the pattern: `dd_run()` (or `dd_make_sparse_map()`) on a
worker thread, callbacks posting window messages to the UI thread,
`dd_options` filled in directly rather than by parsing text, the device
picker from `dd_list_devices()`, the *Sparse read* column from
`dd_fs_report_read()`.

## Building

You need Visual Studio (or the Build Tools) with the C++ workload: MSVC 14.x
and the Windows SDK (for rc.exe). Nothing is downloaded: liblzma is
compiled from `third_party/xz`.

- **`build.cmd`** finds vcvars64 itself. Its "'vswhere.exe' is not
  recognized" comes from vcvars64.bat and is harmless. liblzma is built
  once into `build\lzma.lib`; delete it to rebuild.
- **CMake** builds the same targets.
- Both are /W4 clean and statically linked (/MT): no runtime needed.
- The MinGW path in CMakeLists.txt (`-municode`) has never been tried.

## Testing

| Suite | Checks | Needs | Covers |
|---|---|---|---|
| run-tests | 70 | — | GNU semantics, suffixes, conversions, pipes, filters, --help/--version/--list |
| xz-tests | 32 | — | every level, stdout, concatenated, corrupt and truncated streams, --fit through .xz, idle priority, memory budget |
| hash-tests | 41 | — | .hashes files, --verify on files, --verify-hashes |
| device-tests | 51 | admin | 64 MB VHDX: locking, force-dismount, --lock-input, drive letters, --keep-letters, system-disk refusal, read-modify-write, interface paths, verify |
| ctrlc-test | 5 | admin | real Ctrl+C: stats, exit 130, blocked pipe read released |
| gui-race-test | 6 | admin | disks swapped or added while the GUI's confirmation is open |
| fit-tests | 50 | admin | --fit for MBR and GPT, both directions, through .xz and hashes; clearing an old backup GPT |
| gui-fit-test | 70 | admin | the GUI end to end: lists, read, write, check, Command line, lock, Sparse read, image to image (.xz, sparse, sparse map), disk to disk, read tests, wipes |
| sparse-tests | 91 | fixtures | ext2/3/4 against dumpe2fs's free list, journal recovery, damage, MBR, overlap, options; .xz maps and streaming, hash files; NTFS against Windows' bitmap; swap |
| sparse-device-tests | 43 | admin, fixtures | Windows-formatted FAT12/16/32, exFAT, NTFS plus ext4 on one VHDX: read, written back, chkdsk, file hashes; dirty bits; mounted volumes |

`tests\run-all.ps1` runs all ten suites, one line each. For 1.0.0 it
passed 459 of 459 in about three minutes. gui-race-test adds a check per
physical disk other than the boot disk. About 20 s of device-tests is dd's
own 5 s lock retry, waited out by four refused-lock checks.

**Fixtures.** `tests\fixtures` holds 16 MiB ext images (`.img.xz`) with
free block lists (`.free`), from `tests\make-sparse-fixtures.ps1`, and
NTFS images with Windows' own free cluster list, from
`tests\make-ntfs-fixtures.ps1`. It is not in the source tree: `.gitignore`
ignores `tests/fixtures/`, `clean.ps1` removes it, and `release.ps1` never
zips it. Without it, sparse-tests and sparse-device-tests print "skipped:
no fixtures ..." and exit 0, and `release.ps1` and `run-all.ps1` report
the skip.

`tests\regenerate-fixtures.ps1` (elevated; needs the VS C++ build tools
and internet; about three minutes):

1. builds dd.exe;
2. downloads Cygwin's installer, checks its Authenticode signature, and
   installs e2fsprogs 1.44.5 into `%TEMP%` (`--no-write-registry`; an
   existing Cygwin is left alone);
3. runs both generators into a staging folder (`-Out`);
4. runs sparse-tests and sparse-device-tests against it (`-Fixtures`) and
   checks no image, compressed or not, holds the machine name, profile
   path or machine SID;
5. only then puts them in `tests\fixtures`, listing what changed;
6. deletes everything it made, including the value cygwin1.dll writes
   under `HKLM\SOFTWARE\Cygwin\Installations`, even after a failure.

`-NoReplace` stops before step 5, `-KeepWork` keeps the staging folder,
`-Site` picks a Cygwin mirror. The `.free` lists are reproducible; images
differ only in mke2fs timestamps and UUIDs and the NTFS serial number.

- Windows has no mke2fs, hence Cygwin's e2fsprogs 1.44.5 (still shipped;
  the script warns on another version). Its `debugfs` writes a journal
  transaction (`jo`/`jw`/`jc`) for the `ext4-recover` fixture, a journal
  that needs recovery.
- Windows writes the formatting machine's SID into NTFS security
  descriptors (group `<machine>\None`). The generator replaces it with
  `S-1-5-21-1111111111-2222222222-3333333333`; sparse-tests puts the local
  SID back when unpacking (`tests\machine-sid.ps1`). Only the three
  machine-specific numbers are matched: one copy straddles a sector
  boundary, where NTFS's update sequence fixup changes its header bytes.
  dd never reads the descriptors.

Rules for the tests:

- **Only VHDXs they create**, with diskpart, detached and deleted in
  `finally` blocks. Real disks are never written; gui-race-test hashes the
  first MiB of each, read-only, to prove it.
- **At most 16 MB through the compressor.** Level 9 runs at about 7 MB/s.
- **Elevation.** From an unelevated session use
  `Start-Process powershell -Verb RunAs -Wait` with `run-all.ps1 -Log file`.
- **Leftovers.** Each suite works in `%TEMP%\dd<suite>-<GUID>` and deletes
  it; a killed suite can leave it with a VHDX attached. `clean.ps1`
  (elevated; `-WhatIf` lists actions) removes those folders, detaching
  only VHDXs inside them. Add a new suite's prefix to its pattern.
- **Screenshots.** `gui-fit-test -Screenshot x.png` and
  `docs\make-screenshots.ps1` use `Save-Shot` in `tests\screenshot.ps1`,
  which masks the folder part of every path in the window (edit boxes by
  `EM_POSFROMCHAR`, static text by swapping it while printing), so no user
  name or local path is published. Check them by eye anyway.
- **GUI timing.** Wait for the dialog to be visible before clicking: a
  click during WM_INITDIALOG is undone by its setup.
- **Fast PowerShell.** Return a `byte[]` as `,$array`, or PowerShell
  unrolls it (seconds for 64 MB). Use `[Linq.Enumerable]::SequenceEqual`
  and `[Array]::Copy`, never PowerShell loops. Poll for a condition (a
  disk rescan) rather than sleeping a fixed time.

## Releasing

1. Set the version in `include\dd_version.h`, CMakeLists.txt's
   `project()` and `gui\dd-gui.manifest`'s assemblyIdentity version
   (`<version>.0`). Add a `## <version> (<date>)` section to CHANGES.md
   and update README.md's `Version <version>.` line.
2. On a test machine, run `tests\regenerate-fixtures.ps1`, then
   `tests\run-all.ps1`, elevated. GitHub's runners cannot run the device
   and GUI suites and have no fixtures.
3. Push, then publish a GitHub release tagged with the version (`v1.0.0`
   or `1.0.0`). `.github/workflows/release.yml` then:
   - checks the tag matches `include\dd_version.h`;
   - runs `release.ps1`, which checks the version agrees in those five
     files, does a clean build, checks `dd --version`, runs run-tests,
     xz-tests, hash-tests and (only if fixtures exist) sparse-tests, and
     writes to `dist\`:
     - `dd-reimagined-<ver>-win-x64.zip`: programs, README, CHANGES,
       COPYING.txt, LICENSE-liblzma.txt;
     - `dd-reimagined-<ver>-src.zip`: sources, build and test scripts and
       these documents, without `tests\fixtures`;
     - `SHA256SUMS`;
   - attaches the three files to the release.

   If any step fails nothing is attached: fix it and re-run the workflow.
   `powershell -ExecutionPolicy Bypass -File release.ps1` tries a release
   locally before tagging.

The binaries are not code-signed. SmartScreen warns about dd-gui.exe,
which asks for elevation, until it has a signature or a reputation.

## Design decisions (and why)

- **Engine/front-end split.** `ddcore` never prints; it reports through
  `dd_callbacks`. `dd_cancel_request()` is thread-safe and calls
  CancelSynchronousIo. The GUI runs `dd_run` (or `dd_make_sparse_map`) on
  a worker thread and posts WM_APP messages back.
  - GUI input and output (`src`, `dst` in gui.c): the Input list
    (`IDC_INPUT`) picks a disk, image file, zeros or random data; the
    Output list (`IDC_OUTPUT`) a disk, image file or nothing. The Input
    row holds `IDC_DISK` (disk read), `IDC_SRCFILE` or a note; the Output
    row `IDC_DISK2` (disk written), `IDC_FILE` (new image) or a note.
    `operation()` derives `op` from the pair: write, read, check (image to
    nothing), images, disks, wipe (zeros or random to a disk), test (disk
    to nothing). `source_is_disk()`/`target_is_disk()`/`target_is_file()`
    decide most of the rest; the partition table and Sparse read column
    are always the source's.
  - Zeros and random data never end, so a wipe gets
    `count=<disk size> iflag=count_bytes`. A read test with Raw unticked
    gets `count=<data end>`, not `--fit`, which would rebuild a GPT backup
    and so refuses `/dev/null`.
  - `options_from_controls()` builds the options for both Start and
    Command line, so the two cannot disagree.
- **GNU semantics.** With `bs=` each input block is written as read;
  otherwise output is gathered into obs blocks. swab, lcase and ucase force
  reblocking, as in GNU. Exit codes: 0 success; 1 any failure, including
  read errors skipped by conv=noerror; 130 cancel. Stats are printed only
  if copying started.
- **Last partial input block.** Always written, only its real bytes,
  counted as a partial record. On devices a partial sector is written by
  read-modify-write, keeping neighbouring bytes.
- **Device I/O.** Offsets go in OVERLAPPED on synchronous handles. Reads
  are clamped to the device size (EOF); writes past the end give
  ERROR_DISK_FULL. Unaligned I/O uses a 1 MiB aligned bounce buffer;
  direct-I/O files use a ReOpenFile buffered handle for unaligned pieces.
- **Locking.**
  - An output volume is locked and dismounted; for a whole-disk output,
    every volume with an extent on the disk.
  - The lock is retried for 5 s (20 × 250 ms, `try_lock`) so a briefly
    busy volume succeeds.
  - `force_dismount`: 4 lock attempts, FSCTL_DISMOUNT_VOLUME, 4 more. If
    stale handles still block it, the copy continues dismounted but
    unlocked, as Rufus does, with a warning.
  - A dismounted volume's flush fails with ERROR_NOT_READY, so the disks
    under it are flushed instead. IOCTL_DISK_UPDATE_PROPERTIES runs when a
    whole-disk output closes. Raw device outputs are always flushed.
  - **Drive letters** (`take_letters`/`restore_letters`, endpoint.c): once
    a volume is locked (writing or `lock_input`), its letters are removed
    with DeleteVolumeMountPoint unless `keep_letters` (`--keep-letters`),
    after saving each letter's `HKLM\SYSTEM\MountedDevices` value (the
    mount manager's record). `ep_close`, after unlocking, restores each
    with SetVolumeMountPoint, polling 3 s while a rewritten disk's volumes
    return. A letter already taken by a volume on the same disk is left
    (Windows gave it to the image's new volume). If the volume does not
    return, the saved registry value is written back, so the mount manager
    restores the letter if it ever does, as without dd. A killed process
    leaves its letters removed.
  - `lock_input` (`ep_lock_input`) locks the input volume, or every volume
    on an input disk, for the whole read but never dismounts. The lock
    flushes the file system and keeps other programs off. It is taken
    before the output is opened, so a busy input leaves an existing output
    untouched. No force option: forcing means dismounting, and a remount
    mid-read defeats the point. The Windows volume cannot be locked, so it
    is refused up front. Opt-in, because reading a disk in use is often
    what people want.
- **System protection** (`on_system` in endpoint.c; `dd_disk_is_system`
  for the GUI). Never written, whole, by volume or by partition: every
  extent of the %WINDIR% volume and the partition in
  `HKLM\SYSTEM\Setup\SystemPartition` (the ESP), and their disks. If
  Windows' location is unknown, the write is refused. The disk check is a
  separate first pass, so nothing is dismounted before a refusal. The GUI
  hides those disks in write mode.
- **Disk identity.** The GUI and `id=`/`od=` open disks by SetupAPI
  interface path (`dd_disk_info.interface_path`), not
  `\\.\PhysicalDriveN`: Windows reuses disk numbers. The GUI copies the
  chosen disk's entry and holds refreshes back while a confirmation is
  open. A disk whose whole-disk handle answers
  IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS (some USB readers) is still a disk,
  because IOCTL_STORAGE_GET_DEVICE_NUMBER reports PartitionNumber 0.
- **Filters.** Path syntax is checked against the filter before opening,
  so a refused output is never created; the device class after.
  "Removable" means RemovableMedia, or a USB, SD or MMC bus.
- **--fit.**
  - The data end is the furthest of: partition ends (including MBR
    extended containers and hybrid MBR entries), the end of the primary
    GPT array, and an ISO 9660 volume.
  - A GPT gets a new backup at the end of the target, with its primary
    header and protective MBR patched to match, as `sgdisk -e` does. Into
    .xz or a hashed file, sectors 0 and 1 are patched inline as they
    stream past, so one pass gives the final bytes.
  - An MBR or tableless image written to a whole disk clears the disk's
    last 16 KiB + 1 sector (`plan_clear`, `clear_end`), where a backup GPT
    from earlier use would be. Unconditional (not only when "EFI PART" is
    found) so the result does not depend on what was there; clamped clear
    of the image's data; checked as zeros by `--verify`.
- **.xz.**
  - liblzma over the 7-Zip SDK: signed releases, MT encode and decode,
    size from the index, 0BSD licence.
  - **Memory.** An encoder thread needs 1.2 GiB at level 9 (165 MiB at 6,
    6 MiB at 0); one per CPU thread on a 32-thread machine is about
    43 GiB, so memory is budgeted:
    - The budget (`job.xz_budget`) is `--xz-memory` (`dd_options.xz_memory`),
      or free memory at start (lesser of free physical and free commit,
      `dd__xz_memory_available`) less 2 GiB (`XZ_RESERVE_SWAP`) with a
      page file (`dd__xz_page_file`: commit limit above physical memory),
      else 4 GiB (`XZ_RESERVE`); with no more than the reserve free, a
      quarter of it. The message gives free memory and budget.
    - An .xz input's decoder gets a quarter when the output is compressed
      too, else all; past its limit liblzma decodes single-threaded rather
      than failing.
    - The encoder gets the rest: threads are cut first; if one thread does
      not fit, the level is lowered with a warning, since a lower level
      costs size where threads only cost time.
    - Encoder memory is freed when the stream finishes (`lzma_end` in
      `dd__xz_finish`), before `--verify` decompresses the output.
    - The GUI uses the same default and has no budget control.
  - Default level 9, for the smallest images. At level 9 the compressed
    count stays 0 for the first ~1.1 GiB of incompressible data, as each
    thread fills a 192 MiB block first.
- **Hash files.**
  - `disk.img` and `disk.img.xz` share `disk.img.hashes` (sha256sum
    format, LF, bare names). A compressed output without `.xz` records its
    data under `<name>.uncompressed`.
  - Made by default only for an image of a disk or volume; `--hashes`
    makes one for any new file. A stale one is deleted when its image is
    rewritten.
  - If the output's hash file is also the input's (`dd_same_file`: file
    ID, or folder ID and name, so any path spelling matches;
    `hashes_shared` in copy.c), it is not deleted at the start (the input
    is checked against it); it is left alone with `--fit`, `--sparse-read`,
    `skip=`, `count=` or a data-changing `conv=`; otherwise it is rewritten
    after a successful copy, keeping the other file's entries.
  - Checking happens during the write, so a mismatch fails the run after
    the data is on the disk. The GUI's check mode, or `--verify-hashes`,
    checks an image beforehand.
  - `--verify` compares a SHA-256 of the read-back output with one taken
    while writing; it does not re-read the source. Files are read back
    through a `FILE_FLAG_NO_BUFFERING` handle; disks through the copy's
    own handle, volumes still locked.
- **Sparse reading (`--sparse-read`).**
  - Drivers (`lib/fs.h`) have `probe` (first 4 KiB, no I/O) and `map` (a
    bitmap of used units over part of the volume); bytes outside the
    bitmap are always read. A driver returns `DD__FS_WHOLE` whenever
    unsure, and the volume is read whole. A new file system is a new
    `fs_*.c` and a line in `drivers[]` in `fsmap.c`.
  - The map is built after `--lock-input`'s lock, through the input's own
    handle, with the partition table from `dd__read_layout_handle`.
  - `fsmap.c` reads whole, whatever a driver says, a file system that
    overlaps another partition (or, if primary, the extended partition),
    lies inside an ISO 9660 volume (hybrid ISOs keep an EFI FAT inside an
    ISO file), or is larger than its partition (drivers allocate nothing
    for that and return `DD__FS_MAPPED` with no bits; fsmap says why).
  - Containers are read whole before any probe (`container()`): LUKS,
    BitLocker, LVM, md (all four superblock places) and bcache signatures,
    and LUKS/RAID/LVM partition types. They leave an old file system's
    first blocks intact, so a stale ext or swap superblock would be
    believed: a LUKS1 header over old swap would have its payload zeroed.
    Swap also needs its first 1 KiB blank, as mkswap leaves it.
  - ext trusts `BLOCK_UNINIT` only if the descriptor checksum holds (crc16
    for uninit_bg, crc32c for metadata_csum: `desc_csum_ok`), else reads
    whole. jbd2 tags are 2 bytes longer with csum v2. The journal's extent
    tree is limited to as many nodes as it has blocks.
  - exFAT's `--allow-unclean` walk fails (whole) on a broken or short
    chain, or when all chains together pass the cluster count
    (cross-linked).
  - `sparse_read` in copy.c reads each block's used runs and zero-fills
    the rest. Free gaps under 64 KiB are read with their neighbours, then
    zeroed, so output does not depend on how it was read. Records count as
    if everything was read.
  - An uncompressed input's hash file is not checked (free space reads as
    zeros), and `setup_output` leaves a shared one alone (see Hash files).
  - **Compressed inputs** are mapped beforehand (`dd_make_sparse_map`,
    `--make-sparse-map`, the GUI button) into `disk.img.sparse`:
    - `dd__xz_seek` (xz.c) reads .xz out of order: it finds the block for
      an offset in the index (`lzma_index_iter_locate`), decodes its
      header and runs `lzma_block_decoder` from there, caching 64 1 MiB
      chunks (LRU). Drivers read through `dd__fs_source.rd`, the partition
      table through `dd__read_layout_reader`. A single-block .xz (`xz`
      without threads) restarts from the start on every backward read.
    - The `lzma_block` lives in its struct, not on the stack: liblzma's
      block decoder points to it until the block ends. The block size must
      match the index, and its decoder fit a quarter of free memory.
    - The .sparse file (text; `save_map`/`dd__fsmap_load`, format 2) holds
      the .xz's SHA-256 and sizes, `unclean`, the report's `volume` lines
      and `free <offset> <length>` ranges, then `check <SHA-256 of all
      above>` and `end`. Loading checks the free ranges lie in sparse
      volumes and sum to each one's size less used; each becomes a
      `free_run` region.
    - A failed read while mapping fails `dd_make_sparse_map` (in a
      compressed file it means damage) rather than giving a map that reads
      parts whole without saying why.
    - The copy (`setup_sparse_stream`) reads in order and zeroes free
      ranges after hashing (`zero_free`), so the input's .hashes still
      check. The map's SHA-256 is checked like a hash-file entry
      (`raw_want_from` names the map), draining the input if need be; a
      .hashes entry for the .xz must agree up front. Sizes are checked
      before anything is opened for writing.
    - `ep_read` advances `pos` (uncompressed) for .xz input too, for
      `zero_free` and `expected_total`.
  - **Unclean** file systems are read whole unless `--allow-unclean`. Then
    ext3/4 walks the jbd2 log from `s_start`, ORing every journalled block
    bitmap copy into the map plus every block the log would write
    (uncommitted and revoked too: more is safe); exFAT walks the directory
    tree, since free clusters' FAT entries are undefined and Windows'
    format leaves old data there.
  - **Windows-mounted FAT/exFAT.** Measured on Windows 11: fastfat sets
    the boot sector dirty bit while it has pending changes and later
    rewrites FAT[1]'s clean bit itself; it marks the volume clean when it
    grants a lock, but not always before dd's first read. So with
    `--lock-input` the on-disk marks are ignored when
    `FSCTL_IS_VOLUME_DIRTY` on the locked handle says the volume was clean
    when mounted. That FSCTL is mount-time state (`fsutil dirty query`
    says "NOT Dirty" while the bit is set) and catches a card pulled from
    Linux or Windows, which both mark the boot sector. ext ignores this:
    its journal is on-disk state.
- **Idle priority.** Both programs run in the Idle priority class, which
  liblzma's threads inherit. Disk I/O priority stays normal: background
  I/O mode made card writes very slow.

## Known gaps and open items

Safety limitations:

- **Not yet system-protected (medium):** page-file volumes, the recovery
  partition; with `--force-dismount`, a host disk of a native-boot VHDX,
  and Storage Spaces pool members.
- **`od=` on a dynamic (LDM) disk (medium).** The table shows one LDM
  partition, so `od=E:` selects the whole disk even with other dynamic
  volumes on it. Fix: count volumes with extents on the disk.
- **Fail-open when IOCTL_STORAGE_GET_DEVICE_NUMBER fails (medium-low).**
  `ep_lock_output` skips the system-disk check. `--filter` and the GUI
  refuse such a disk; the plain CLI should too.
- **The GUI preselects the only listed target (low)**, which with *Show
  fixed disks* can be a fixed data disk.
- **`od=` silently overrides `of=`, and `id=` overrides `if=` (low).**
- **The whole-disk lock skips volumes whose extents cannot be read
  (low).** Windows still blocks raw writes to mounted sectors.
- **A typed `of=\\.\PhysicalDriveN` is bound by number (by design):** it
  is what was typed. The GUI's Command line output shares this; README.md
  says so.

Behaviour gaps:

- **Old data after `--fit`**, past the data end (except the cleared
  backup-GPT area), is not wiped, though nothing reads it.
- **A hash mismatch is found only after the write.** The GUI could run
  check mode first, at the cost of reading the image twice.
- **An optical disc's size.** Drives report capacity, which can include
  unreadable sectors: 150 run-out sectors (300 KiB past the ISO 9660
  volume) on a pressed CD tested, 1 on a burned DVD. Reads end cleanly at
  the last readable sector, so images are complete, but `--layout` gives
  the capacity as the data end and progress stops just short of 100%.

Sparse reading:

- **GUI preview without the lock.** The *Sparse read* column comes from
  `dd_fs_report_read()`, reading unlocked. A FAT card Windows has just
  written can show "not cleanly unmounted" (Windows' pending-changes
  mark), though a run with *Lock* ticked maps it; the summary says so.
  The column is recomputed on every disk change or refresh, reading every
  bitmap: fine for cards, slow for a large fixed disk.
- ext with an **external journal** needing recovery, or with **fast
  commits** (dd cannot read them), is read whole.
- ext2 not cleanly unmounted, with `--allow-unclean`, may miss blocks
  written since the last clean unmount. An inode scan would fix that, at
  the cost of reading every inode table.
- Bad FAT clusters count as used, so they are read (and may give read
  errors on failing media).
- NTFS: `$Bitmap` must be in the MFT's first run with its runs in the base
  record (no attribute list), else the volume is read whole. An unclean
  volume (`$Volume` dirty, or `$LogFile`'s restart area in use and not
  clean) is read whole regardless of `--allow-unclean`
  (`dd__fs_map.unclean_whole`). Fixture free clusters come from
  `FSCTL_GET_VOLUME_BITMAP`, so the driver is checked against Windows, not
  itself. Until NTFS commits a deletion its live bitmap still counts the
  file's clusters, and a mount allocates a little, so the generator
  dismounts and mounts, then takes the bitmap and the clean image with
  nothing in between. NTFS rewrites log pages and MFT sequence numbers on
  each lock, so two reads of a mounted NTFS volume never match exactly.
- Swap: 4 KiB pages only; hibernation signatures (S1SUSPEND, S2SUSPEND,
  ULSUSPEND, LINHIB0001, TuxOnIce) read whole.
- MBR sector size: a disk's own (`dd__geometry`); for an image file, 4096
  only if no partition's first sector holds a boot sector, ext or swap
  signature at 512-byte units and one does at 4096 (`mbr_sector_size`).
  All-blank partitions mean 512.

Not tested:

- real camera SD cards; TexFAT (two exFAT FATs); a wrapped jbd2 log;
- in the suites, FAT from Linux's mkfs.fat (no dosfstools on Windows) and
  ext from mke2fs newer than 1.44.5; the Raspberry Pi card below has both;
- writing to optical drives (never attempted), floppies, 4K-native disks
  (the bounce-buffer path for a 512-byte remainder), 4Kn GPT images;
- real hybrid ISOs (e.g. Ubuntu), hybrid MBRs, images whose partitions run
  past the end of the file;
- a second Ctrl+C killing the process;
- the MinGW build.

Tested by hand:

- **Optical discs** in a USB DVD writer: a pressed CD (ISO 9660) and a
  burned DVD (UDF with ISO 9660 bridge, 1.6 GB), by `\\.\CdRomN`,
  `\\.\X:` and `id=`: whole, `--fit`, unaligned and mid-sector reads,
  reads past 1 GB and to the last readable sector, `--lock-input`,
  `--sparse-read` (read whole), `.xz`, `--verify`. Every read matched a
  whole-disc image, which matched its hash file.
- **A Raspberry Pi OS card** (64 GB, MBR): a 512 MiB FAT32 boot partition
  from Linux's mkfs.fat and an ext4 root from a newer mke2fs
  (`orphan_file`, `metadata_csum_seed`, `metadata_csum`). Both are mapped;
  a sparse read written back to the card works.

Ideas:

- taskbar progress (ITaskbarList3);
- count, skip and seek fields in the GUI (the CLI has them);
- conv=ascii, ebcdic, ibm, block, unblock and cbs= (needs GNU's
  translation tables);
- code signing for releases.
