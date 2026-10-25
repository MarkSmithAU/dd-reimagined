# Differences from dd for Windows 0.6

dd reimagined for Windows is a rewrite in C of John Newbigin's Delphi
"dd for Windows 0.6beta3". It keeps 0.6's command line and adds GNU dd's
behaviour.

## Bugs fixed

- Exit status: 0 on success, 1 on any failure (including read errors
  skipped with `conv=noerror`), 130 after Ctrl+C, which also prints the
  statistics.
- Existing output files are truncated unless `conv=notrunc`; 0.6 always
  behaved like `notrunc`.
- Block counters are 64-bit; 0.6's overflowed after 2^31 blocks.
- A partial last sector on a device is read-modify-written; 0.6 overwrote
  the rest of the sector with zeros.
- `skip=` on a pipe counts the bytes actually read.
- End of file is no longer reported as a read error.
- `of=X:` no longer goes down the Win9x path on NT.
- Bad numbers are errors, not silently 0.
- `id=`/`od=` fail cleanly on an unusable drive; 0.6 carried on with an
  empty filename.

## GNU behaviour added

- `ibs`/`obs` with reblocking; `iseek`/`oseek`.
- `conv=` notrunc, noerror, sync, fsync, fdatasync, excl, nocreat, sparse,
  swab, lcase, ucase.
- `iflag`/`oflag`: fullblock, count_bytes, skip_bytes, seek_bytes, append,
  direct, sync, dsync.
- GNU suffixes (`kB`=1000, `K`=`KiB`=1024, `b`=512, `NxM`); a trailing `B`
  on a count means bytes.
- `status=none|noxfer|progress`.
- GNU wording for statistics, messages and the partial-read warning.

## Kept from 0.6

Though not GNU: `-` for standard input and output, the `/dev/*` names,
`--list`, `--filter`, the `dd-<filter>.exe` naming, `\\?\Device\...` names
(now via `\\?\GLOBALROOT`), and `--size` (accepted but unneeded).

## New

dd-gui.exe; locking and dismounting before writing, removing drive letters
meanwhile; protection of the Windows disks; disks opened by device path;
`--lock-input`, `--force-dismount` and `--keep-letters`; `--layout` and
`--fit`; `.xz` images; hash files, `--verify` and `--verify-hashes`;
`--sparse-read`, `--allow-unclean` and `--make-sparse-map`. See
[CHANGES.md](../CHANGES.md).

## Changed

dd uses only documented Windows APIs (SetupAPI, the Volume API, storage
IOCTLs; no ntdll). No startup banner; use `--version`.

## Removed

Win9x support (DiskIO, the 16-bit thunk DLL), the `d` and `q` size
suffixes (not in GNU), and the unused md5/zlib/PE-resource "studio" code.

## Not implemented

`conv=ascii,ebcdic,ibm,block,unblock` and `cbs=` (rejected clearly).
