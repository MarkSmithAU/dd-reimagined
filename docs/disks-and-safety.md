# Disks and safety

Writing to the wrong disk is the classic dd accident. dd.exe and
dd-gui.exe share one copy engine, and all the guards below.

## Locking before writing

Windows refuses raw writes to a mounted file system, so before writing a
volume or disk, dd locks and dismounts the output volume, or every volume
on the output disk.

- A volume in use (a program has files open on it) is retried for 5 s,
  then dd stops before writing anything.
- **`--force-dismount`** (*Force dismount if in use* in dd-gui.exe)
  dismounts it anyway, and programs lose their open files. Use it when
  Explorer or another program holds the disk's partitions open.
- Locks are held until the copy, and any `--verify`, finish. Windows then
  re-reads the partition table and mounts what the disk now holds.
- Raw device outputs are flushed before dd exits, so a USB stick can be
  removed as soon as dd returns.

## Locking the input: `--lock-input`

Reading locks nothing by default, so a mounted file system can change
while imaged. `--lock-input` (*Lock the source's volumes*) locks the input
volume, or every volume on an input disk, for the whole read. The file
system writes out its cache, and no other program can open files there.
The volumes stay mounted and are usable again afterwards.

The lock is tried for 5 s and refused while files are open; dd then stops
before creating the output, so an existing image is untouched. The volume
Windows runs from cannot be locked; read it without `--lock-input`.

## Drive letters

While a volume is locked, for writing or with `--lock-input`, dd removes
its drive letters, so Explorer cannot offer to format it and no program
can remount it by opening a file on it.

    dd: dismounted \\?\Volume{...} (E:\)
    dd: removed drive letter E: while its volume is locked
    ...
    dd: put drive letter E: back

When the locks are released:

- **The volume is still there** (read, or written by volume): its letter
  goes back at once.
- **The disk was rewritten** with an image: Windows mounts the image's
  volumes. dd waits up to 3 s for the old volume to return (as when the
  same image is written back) and gives back its letter.
  - If Windows gave the letter to a new volume on the disk: `Windows gave
    drive letter E: to a new volume on the disk`.
  - If the volume is gone, dd restores Windows' record of the letter
    exactly, so Windows returns it if the volume ever does: `drive letter
    E: is kept for its volume, which is not there after the copy; Windows
    gives it back if the volume returns`.
- **Another disk took the letter** meanwhile: it keeps it, and dd warns.

`--keep-letters` leaves letters in place; dd-gui.exe always removes them.

Letters are put back when dd finishes, fails, or is stopped (Ctrl+C or
dd-gui.exe's *Stop*). If dd is killed outright (Task Manager, power cut),
a letter it removed stays removed: reassign it with *Disk Management*
(Change Drive Letter and Paths) or `mountvol`.

## What is never written

- the volume Windows runs from;
- any disk holding part of it, spanned and mirrored volumes included;
- the partition Windows boots from (its EFI system partition), and its
  disk.

This holds even with `--force-dismount`. All volumes on a disk are
checked before any is dismounted, and dd refuses if it cannot tell where
Windows runs from. dd-gui.exe does not offer that disk for writing.

## Choosing the right disk

- **`--list`** shows each disk's bus (USB, SATA, NVMe), removability,
  size, partition table, and volumes with labels and letters.
- **`id=E:` / `od=E:`** name the whole disk holding E:, if E: is its only
  partition; otherwise use `\\.\PhysicalDriveN`.
- **`--filter=TYPE`** writes only to a `removable`, `fixed`, `disk`,
  `partition` or `file` output. Removable includes USB, SD and MMC disks
  reporting fixed media. A copy named `dd-TYPE.exe` (e.g.
  `dd-removable.exe`) sets it, so scripts cannot forget it. It is checked
  before the output is opened, so a refused output is never created.
- **dd-gui.exe** offers only removable disks unless *Show fixed disks* is
  ticked; its confirmation names the disk, size and drive letters.

## Disk numbers change

Windows numbers disks in the order found and reuses numbers: unplug disk
2, plug in another, and it may become disk 2.

- `id=`/`od=` and dd-gui.exe open a disk by its **device interface
  path**, which belongs to the device, so a disk plugged in during a
  confirmation cannot receive the write.
- If devices change while its confirmation is open, dd-gui.exe rechecks
  the disk (a card swapped in the same reader keeps the reader's path),
  and does not start if anything changed.
- A typed `\\.\PhysicalDriveN`, and the command dd-gui.exe's *Command
  line* button shows, open by number.

## Elevation

Raw disk and volume access needs administrator rights. dd.exe needs an
elevated prompt (and says so); dd-gui.exe asks for elevation at start.
