# dd reimagined for Windows

## Motivation

Most disk imaging software aimed at external storage is focused on the individual tasks and does not integrate them into a streamlined process.  This program was borne out of a desire to have a perfect solution for reading and writing image files, with the workflow that I would normally perform including clearing free space, a raw image read and then compression, as well as writing back to a drive safely for restoration. The core of the design is a streaming pipeline to handle all the operations without intermediate files. This enables new functionality such as old images through this program and re-process it (i.e. sparse-process an existing image file and then compress it). 

## Absolute Disclaimer of Any Warranty or Liability - Use At Your Own Risk

The actions this program is capable of are destructive, either from incorrect usage or due to bugs.  

While every effort has been made to check for bugs and adding safeguards, including a comprehensive test suite, you use this software AT YOUR OWN RISK.

If you cannot agree with the above, do not use it.

## About

Version 1.0.0. A C rewrite of John Newbigin's dd for Windows 0.6beta3. It
behaves like GNU dd, keeps 0.6's Windows extras (`--list`, `id=`/`od=`,
`--filter`, `dd-<filter>.exe` naming) and adds a graphical front end and
the features below.

![dd-gui.exe imaging a card, compressed, reading only used space](docs/images/gui-read.png)

- **Programs:** `dd.exe` (command line) and `dd-gui.exe` (graphical),
  statically linked: no installer or runtime.
- **Requirements:** Windows 7 or later, x64. Disk access needs an elevated
  prompt; dd-gui.exe asks for one itself.
- **Licence:** GPL version 2 or later ([COPYING.txt](COPYING.txt)); liblzma
  from XZ Utils 5.8.4 is under the BSD Zero Clause License.
- **Release notes:** [CHANGES.md](CHANGES.md).

## Features

- **Safe disk writing.** Volumes are locked, dismounted and lose their
  drive letters during the copy. The disks Windows runs and boots from
  are never written. Disks are opened by device path, so one plugged in
  during a confirmation cannot receive the write.
- **Images that fit.** `--fit` copies only the partitions: an image of a
  64 GB card with 32 GB of partitions goes onto a 32 GB card. A GPT's
  backup is rebuilt at the end of the new disk.
- **Only the used space.** `--sparse-read` maps FAT12/16/32, exFAT, NTFS,
  ext2/3/4 and Linux swap, reading only what files use and writing free
  space as zeros, so mostly empty cards image fast and compress well.
- **Compressed images.** `.xz`, written with multithreaded LZMA2, read
  on the fly with progress.
- **Hash files and verification.** Disk images get a SHA-256 hash file,
  checked whenever the image is read; `--verify` reads the output back
  and compares it.
- **GNU dd semantics.** `ibs`/`obs`, `conv=`, `iflag=`/`oflag=`, GNU size
  suffixes, `status=progress`, exit codes and messages.
- **A graphical front end** showing the partition table, what sparse
  reading would read and the equivalent `dd.exe` command.

## Download

Download `dd-reimagined-<version>-win-x64.zip` from
[Releases](../../releases), check it against `SHA256SUMS` and unzip it
anywhere. It holds both programs, this README, the release notes and the
licences. Not being code-signed, dd-gui.exe gets a SmartScreen warning
until it is better known.

## Quick start

**dd-gui.exe:** pick the *Input* (disk, image file, or zeros or random
data to wipe a disk) and *Output* (disk, image file, or nothing for a
read test) and press *Start*. It opens reading a disk into an image file,
which changes no disk. Writing a disk asks for confirmation, naming the
disk, its size and drive letters.

![The confirmation before overwriting a disk](docs/images/gui-confirm.png)

**dd.exe**, from an elevated prompt:

    dd --list                                         disks, optical drives and volumes
    dd id=E: of=card.img.xz --fit --sparse-read --lock-input
                                                      image the card in E: (compressed,
                                                      partitions only, used space only)
    dd --layout if=card.img.xz                        show an image's partition table
    dd --verify-hashes if=card.img.xz                 check an image against its hash file
    dd if=card.img.xz od=E: --fit --verify            write it back and read it back
    dd if=card.img.xz --make-sparse-map               map a compressed image for --sparse-read

`dd --help` lists every operand and option.

## Documentation

| Document | What it covers |
|---|---|
| [docs/gui.md](docs/gui.md) | dd-gui.exe, screen by screen |
| [docs/command-line.md](docs/command-line.md) | dd.exe: operands, Windows devices, every option, recipes |
| [docs/disks-and-safety.md](docs/disks-and-safety.md) | locking, drive letters, what is never written, filters, disk identity |
| [docs/images.md](docs/images.md) | partitions (`--fit`, `--layout`), `.xz`, hash files and `--verify`, sparse reading |
| [docs/differences.md](docs/differences.md) | what changed from dd 0.6, and what GNU dd behaviour was added |
| [DEVELOPMENT.md](DEVELOPMENT.md) | for contributors: code layout, engine API, building, tests, releases, design decisions |
| [CHANGES.md](CHANGES.md) | release notes |

## Building from source

    build.cmd                      # MSVC, no CMake needed; finds vcvars itself
    cmake -B build && cmake --build build --config Release

Either gives `build\dd.exe` and `build\dd-gui.exe`. Needs Visual Studio or
its Build Tools with the C++ workload; liblzma is built from
`third_party/xz`, nothing is downloaded. `tests\run-all.ps1`, elevated,
runs the suites in `tests\` on virtual disks it creates. See
[DEVELOPMENT.md](DEVELOPMENT.md).

## Credits

dd for Windows 0.6 is by John Newbigin, under the GPL; this rewrite keeps
its command line and licence. Compression is by liblzma from
[XZ Utils](https://tukaani.org/xz/).
