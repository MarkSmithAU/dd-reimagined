# dd.exe

dd.exe takes GNU dd's operands, size suffixes and messages, and adds
options for Windows disks, partitions, compressed images, hash files and
sparse reading. `dd --help` prints this reference in short.

Disks and volumes need an elevated prompt (*Run as administrator*); files,
pipes and `/dev/*` names do not.

## Recipes

Image the card in E: compressed, reading only the space its partitions'
files use, with the card locked meanwhile:

    dd id=E: of=D:\Images\card.img.xz --fit --sparse-read --lock-input

Show an image's partition table, and what sparse reading would read;
check it against its hash file:

    dd --layout if=D:\Images\card.img.xz
    dd --layout --sparse-read if=D:\Images\card.img
    dd --verify-hashes if=D:\Images\card.img.xz

Write it to the card up to the end of its partitions, then verify:

    dd if=D:\Images\card.img.xz od=E: --fit --verify

Write to disk 2 only if it is removable:

    dd if=raspios.img.xz of=\\.\PhysicalDrive2 bs=4M --filter=removable --progress

Copy disk to disk, up to the end of the partitions:

    dd if=\\.\PhysicalDrive2 of=\\.\PhysicalDrive3 bs=1M --fit --progress

Compress at level 3; decompress to a sparse file:

    dd if=card.img of=card.img.xz --xz=3
    dd if=card.img.xz of=card-sparse.img conv=sparse

Map a compressed image for sparse reading, then make a smaller copy with
free space zeroed:

    dd if=card.img.xz --make-sparse-map
    dd if=card.img.xz of=small.img.xz --sparse-read

Ordinary GNU dd jobs:

    dd if=/dev/zero of=blank.img bs=1M count=64
    dd if=disk.img of=mbr.bin bs=512 count=1
    dd if=disk.img of=part2.img bs=512 skip=1050624 count=2097152
    dd if=big.log bs=1 skip=1000 count=200 status=none | more

## Finding disks: `--list`

`dd --list` lists disks, optical drives and their volumes, then virtual
devices:

    \\.\PhysicalDrive1  USB Card Reader
        USB, removable, 32 GB, 30 GiB (31914983424 bytes), 512-byte sectors, MBR, 2 partitions
        \\?\Volume{00000000-0000-0000-0000-000000000001}
            mounted on E:\, FAT32 "boot", 537 MB, 512 MiB (536870912 bytes)

    \\.\CdRom0  DVD writer
        SATA, removable, no media

`--list --filter=removable` lists only disks dd may then write to.

## Operands

| Operand | Meaning |
|---|---|
| `if=FILE` | input (default standard input) |
| `of=FILE` | output (default standard output) |
| `bs=BYTES` | read and write up to BYTES at a time (default 512); overrides `ibs`, `obs` |
| `ibs=BYTES` | read up to BYTES at a time (default 512) |
| `obs=BYTES` | write BYTES at a time (default 512) |
| `count=N` | copy only N input blocks |
| `skip=N`, `iseek=N` | skip N `ibs`-sized input blocks |
| `seek=N`, `oseek=N` | skip N `obs`-sized output blocks |
| `conv=CONVS` | conversions, comma-separated |
| `iflag=FLAGS`, `oflag=FLAGS` | flags, comma-separated |
| `status=LEVEL` | `none` (errors only), `noxfer` (no final statistics), `progress` (periodic statistics) |
| `id=DRIVE` | read the whole disk holding DRIVE (`id=E:`), if it is the only partition |
| `od=DRIVE` | write the whole disk holding DRIVE, as `id=` |

**Sizes.** N and BYTES may end in `c` (1), `w` (2), `b` (512), `kB`
(1000), `K` (1024), `MB` (1000²), `M` (1024²), and so on for G, T, P, E;
`KiB`, `MiB` etc. equal `K`, `M`. Products such as `2xM` work. An N ending
in `B` counts bytes, not blocks (`skip=1000B`).

**Conversions** (`conv=`): `lcase`, `ucase`, `sparse` (seek over all-zero
output blocks), `swab`, `sync` (zero-pad input blocks to `ibs`), `excl`
(fail if the output exists), `nocreat`, `notrunc`, `noerror` (continue
after read errors), `fdatasync`, `fsync`. Without `notrunc` the output is
truncated.

**Flags** (`iflag=`, `oflag=`): `append`, `direct` (unbuffered), `dsync`,
`sync`, `fullblock` (input), `count_bytes`, `skip_bytes` (input),
`seek_bytes` (output).

`id=`/`od=` default to `bs=1M` and progress; optical drives to one
2048-byte sector, floppies to one 18 KiB track.

## Windows devices and special files

| Name | What it is |
|---|---|
| `\\.\PhysicalDriveN` | whole disk N |
| `\\.\X:` | volume X: |
| `\\?\Volume{GUID}` | a volume, by name |
| `\\.\CdRomN` | optical drive N |
| `\\?\Device\HarddiskN\PartitionM` | NT device names, as in dd 0.6 |
| `\\?\usbstor#disk&...` | device interface path (`Get-Disk`'s `Path`) |
| `-`, `/dev/stdin`, `/dev/stdout`, `/dev/stderr` | standard handles |
| `/dev/zero` | zeros |
| `/dev/null` | empty input; discards output |
| `/dev/random`, `/dev/urandom` | system CSPRNG data |

Reads stop at a device's end; writes past it fail with "not enough
space". Any offset or length works: unaligned reads use a bounce buffer;
partial sectors are written by read-modify-write.

## Windows options

Disks and safety ([disks-and-safety.md](disks-and-safety.md)):

| Option | Meaning |
|---|---|
| `--list` | list disks, optical drives and volumes |
| `--filter=TYPE` | write only to a `removable`, `fixed`, `disk`, `partition` or `file` output; set by naming dd `dd-TYPE.exe`; filters `--list` |
| `--force-dismount` | dismount output volumes despite open files, which their programs lose |
| `--lock-input` | lock the volumes of `if=` (disk or volume) while reading; they stay mounted. Fails while files are open |
| `--keep-letters` | do not remove locked volumes' drive letters (by default removed, then put back) |

Partitions, compression, hashes and sparse reading
([images.md](images.md)):

| Option | Meaning |
|---|---|
| `--layout` | show the MBR/GPT partition table of `if=`/`id=` and exit; with `--sparse-read`, also what would be read |
| `--fit` | copy only to the end of the last partition, refuse if that does not fit, rebuild a GPT's backup at the end |
| `--sparse-read` | read only space the file systems use; write free space as zeros |
| `--allow-unclean` | with `--sparse-read`, also map uncleanly unmounted file systems, conservatively |
| `--make-sparse-map` | map the file systems of an `.xz` `if=` into `IMAGE.sparse` |
| `--xz[=LEVEL]` | compress output at level 0-9 (default 9); automatic for `of=*.xz` |
| `--xz-memory=SIZE` | memory limit for compression and decompression together |
| `--no-xz` | never compress or decompress |
| `--hashes` | write a SHA-256 hash file for any new output file |
| `--no-hashes` | neither write nor check hash files |
| `--verify` | read the output back and check it matches |
| `--verify-hashes` | check image `if=` against its hash file and exit, writing nothing |
| `--progress` | `status=progress` |
| `--size` | accepted for dd 0.6 compatibility; device sizes are always honoured |
| `--help`, `--version` | print help or version and exit |

## Messages, exit status and Ctrl+C

- Messages and statistics go to standard error in GNU's wording
  (`dd: ...`, `N+M records in`, `X bytes (...) copied, T s, R/s`).
- Exit status: 0 success; 1 any failure, including read errors skipped
  with `conv=noerror`; 130 after Ctrl+C.
- Ctrl+C stops the copy (even a read blocked on a pipe), prints
  statistics, releases locks and puts drive letters back.
- dd.exe runs at Idle CPU priority, using only spare CPU time; disk I/O
  priority is unchanged.

Not implemented, and rejected with a clear message: `conv=ascii`,
`ebcdic`, `ibm`, `block`, `unblock`, `cbs=`.
