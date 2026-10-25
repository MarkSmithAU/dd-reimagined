# Changes

## 1.0.0 (2026-10-19)

The first release of dd reimagined for Windows: a rewrite in C of John
Newbigin's dd for Windows 0.6beta3 that keeps its command line.
[docs/differences.md](docs/differences.md) lists every change from 0.6.

New:

- **dd-gui.exe**, a graphical front end. Choose an input (a disk, an image
  file, or zeros or random data to wipe a disk) and an output (a disk, an
  image file, or nothing, to test that the input reads). It shows the
  source's partition table and what sparse reading would read, and
  *Command line* logs the equivalent dd.exe command.
  See [docs/gui.md](docs/gui.md).
- **Safe writing to disks.**
  - Before a disk or volume is written, its volumes are locked and
    dismounted, and their drive letters removed and put back afterwards.
    A volume in use stops the write unless `--force-dismount` is given.
  - The disk Windows runs from or boots from is never written.
  - Disks chosen by `id=`/`od=` or in dd-gui.exe are opened by their device
    path, so a disk plugged in meanwhile cannot receive the write.
- **`--fit`**, dd-gui.exe's default: only the partitions are read or
  written, so an image from a large card fits a smaller one. A GPT is
  rebuilt for the target. Disks with 4096-byte sectors are handled.
- **`--sparse-read`** reads only the space the file systems use (FAT12/16/32,
  exFAT, NTFS, ext2/3/4, Linux swap) and writes free space as zeros.
  `--allow-unclean` maps file systems not cleanly unmounted;
  `--make-sparse-map` lets a compressed image be read sparsely.
- **`--lock-input`** locks the disk being read, so it cannot change while
  it is imaged.
- **`.xz` images** through liblzma 5.8.4, multithreaded: written when the
  name ends in `.xz` or with `--xz[=LEVEL]`, read on the fly.
- **SHA-256 hash files** (`image.hashes`), written when a disk is imaged and
  checked when the image is written back; `--verify-hashes` checks an image
  alone. `--verify` reads the output back and compares it.
- **`--layout`** prints a partition table.
- **GNU dd semantics:** `ibs`/`obs`, `conv=`, `iflag`/`oflag`,
  `status=progress`, GNU size suffixes, wording and exit codes; Ctrl+C
  prints statistics and exits with 130.
- Both programs run in the Idle priority class.

Fixed from 0.6: errors set the exit status; output files are truncated
unless `conv=notrunc` is given; the 2^31 block limit is gone; a partial
last sector on a device is written by read-modify-write, not zero-padded.

Removed: Win9x support, the `d` and `q` size suffixes, the startup banner.

Requirements: Windows 7 or later, x64. Reading or writing disks needs an
elevated prompt; dd-gui.exe asks for one itself.
