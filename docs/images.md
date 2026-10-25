# Images: partitions, compression, hashes and sparse reading

## Partitions: `--layout` and `--fit`

`--layout if=IMAGE` prints an image's or disk's MBR or GPT partition table
and where its data ends. `--fit` copies only that far: an image of a 64 GB
card with 32 GB of partitions fits a 32 GB card, and reading a card with
`--fit` gives an image of just its partitions. `--fit`:

- writes the partitions, partition tables and any ISO 9660 volume, refusing
  before anything is locked or written if they don't fit;
- rebuilds a GPT's backup at the end of the output, adjusting the primary
  header and protective MBR to match (as `sgdisk -e` does);
- writing an MBR image (or one without a partition table) to a disk,
  clears the disk's last 16.5 KiB (33 sectors of 512 bytes), where an old
  backup GPT would make gdisk and other tools report a damaged GPT. The
  image's own data is never cleared; `--verify` checks the cleared end.

An image without a partition table is copied whole. An MBR is read in the
disk's sector size (4096 bytes on many large USB drives), or for an image
file, as its partitions' boot sectors show (dd says if 4096 bytes). `--fit`
is refused to a disk whose sector size differs from the image's, and
cannot be combined with `count`, `skip`, `seek`, conversions or `--no-xz`.

## Compressed images (.xz)

dd uses liblzma from XZ Utils 5.8.4.

- `--xz[=LEVEL]` compresses the output with multithreaded LZMA2, level 9
  by default, as does an `of=` ending in `.xz`. An .xz input is
  decompressed on the fly, multithreaded if it has several blocks, with a
  progress percentage from its index. `--no-xz` turns off both.
- **Speed and size.** Level 9 is smallest but slow (about 7 MB/s on 8
  threads); level 3 is about 15 times faster and 20% larger.
- **Memory.** Each compressing thread (one per CPU thread) needs about
  1.2 GiB at level 9, 165 MiB at level 6. Compressing and decompressing
  together are limited to `--xz-memory=SIZE`, or else the memory free at
  start less 2 GiB with a page file or 4 GiB without (a quarter of it if
  no more is free). The compressor runs fewer threads to fit, or a lower
  level if one thread doesn't fit, and reports it, e.g.
  `compressing to xz at level 9 on 6 threads, using up to 7.3 GiB of
  memory; 9.9 GiB was free before starting, and dd may use 7.9 GiB of it,
  keeping 2.0 GiB free (there is a page file)`.
- `--fit` works through compression, rebuilding a GPT in the same pass.
  Compressed output cannot be a device, nor combined with `seek`,
  `notrunc`, `sparse`, `append` or `direct`.

## Hash files and verification

- **Naming.** `disk.img` and `disk.img.xz` both use `disk.img.hashes`: the
  hash of the uncompressed image and, for .xz, of the .xz file, in
  sha256sum's format (`sha256sum -c --ignore-missing disk.img.hashes`
  works).
- **Writing.** A hash file is written when a disk or volume is read into a
  new file, or for any new output file with `--hashes`. A `--fit` GPT image
  is hashed as finally written. Copying `disk.img` to `disk.img.xz` with
  `--fit` or `--sparse-read` leaves `disk.img.hashes` alone, as it would
  no longer describe `disk.img`.
- **Checking.** An input's hash file (both hashes for .xz) is checked
  during whole reads (no `skip` or `count`), including writing the image
  to a disk; with `--fit` the rest of the image is read after the copy. A
  mismatch fails the run (exit 1) only after writing, so check first with
  `--verify-hashes if=IMAGE`, which writes nothing. `--no-hashes` turns
  off writing and checking.
- **`--verify`** reads the output back and checks it matches what was
  written. A file is flushed and read unbuffered, from the disk, not the
  cache; .xz output is decompressed again and both hashes compared; a disk
  or volume is read through dd's own handle while its volumes stay locked,
  including a `--fit` rebuilt backup GPT.

## Sparse reading

`--sparse-read` reads only the space the input's file systems use, writing
free space as zeros unread: quick for a mostly empty card, compresses well
with `--xz`, and sparse with `conv=sparse`. `--layout --sparse-read
if=IMAGE` shows how much of each file system would be read.

- **Mapped:** FAT12, FAT16, FAT32, exFAT, NTFS, ext2, ext3, ext4 and Linux
  swap, on each partition of an MBR (logical partitions included) or GPT
  disk, on a volume, or on an image without a partition table.
  - In use: FAT, clusters with an entry in any FAT copy; exFAT, the
    allocation bitmaps; NTFS, `$Bitmap`; ext, the block bitmaps (lazily
    initialised groups, `flex_bg`, `meta_bg`, `64bit` and `bigalloc`
    included). Boot sectors, FATs, superblocks, group descriptors, bitmaps
    and inode tables are always read.
  - Linux swap: only the header page (4 KiB pages only); the rest is
    meaningless once its system has stopped. Swap holding a hibernation
    image is read whole.
- **Read as usual:** partition tables, gaps between partitions (where boot
  loaders live), other file systems, and a partition's end past its file
  system. LUKS, BitLocker, LVM, Linux RAID and bcache partitions (by
  signature or partition type) are read whole, whatever old superblock
  remains.
- **Read whole if untrustworthy:** a file system that is damaged, uses a
  feature dd does not know, is larger than its partition, overlaps another,
  or lies inside an ISO 9660 volume (hybrid ISOs). dd says which and why.
- **Not cleanly unmounted** (the dirty bit, or an ext3/4 journal needing
  recovery): read whole unless `--allow-unclean`, which counts everything
  that might be in use:
  - ext3/4: every block the journal would write or a journalled block
    bitmap marks; after recovery (e2fsck or mounting) the image holds all
    the original would;
  - exFAT: every cluster a file or directory names, in the bitmap or not;
  - FAT: every FAT copy (as always);
  - NTFS: never. Only replaying its undocumented log would show what an
    unclean volume holds, so it is read whole. Fast Startup, hibernation
    and any mount by Windows leave the log in use, which counts as
    unclean.

  Data written just before an unclean unmount but not yet recorded can
  still be lost, as with fsck.
- **Mounted volumes.** Windows marks a mounted FAT volume dirty while it
  has unwritten changes. `--lock-input` makes Windows write them first, and
  dd asks Windows whether the volume was dirty when mounted. Without it, a
  mounted volume can change between dd reading its map and its data; dd
  warns.
- **Hash files.** An uncompressed input's hash file is not checked, as free
  space reads as zeros; a compressed input is read whole, so it is.
- **Compressed images** can only be read in order, but mapping needs
  metadata from anywhere, so map them first: `dd if=disk.img.xz
  --make-sparse-map` (or dd-gui.exe's *Make sparse map*) decompresses only
  the parts needed and writes `disk.img.sparse` beside the image.
  `dd if=disk.img.xz of=new.img.xz --sparse-read` then reads it in order,
  writing free space as zeros, as for the uncompressed image.
  - Mapping decompresses from the start of each .xz block it needs. dd's
    .xz files have many blocks; one from `xz` without threads is a single
    block, slower to map.
  - The map is bound to the .xz file by its SHA-256, checked during the
    read (and against the image's hash file beforehand). A map made with
    `--allow-unclean` is only used with it.
  - The .sparse file is text ending in a SHA-256 of itself. A damaged or
    edited map is refused, as is one whose free ranges differ from what
    its volumes say is free. A read error while mapping fails the map: in
    a compressed file it means damage.
