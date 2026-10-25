# Makes the NTFS images that sparse-tests.ps1 uses, in tests\fixtures.  Needs
# an elevated prompt: Windows formats a VHDX, which is the only way to get
# NTFS as Windows writes it.  Each image is 16 MiB: an MBR, then a 14 MiB
# NTFS partition at 1 MiB, free space filled with 0xA5 beforehand.
#   ntfs.img.xz        cleanly unmounted (the disk taken offline)
#   ntfs-dirty.img.xz  marked dirty (fsutil dirty set), then taken offline
#   ntfs-inuse.img.xz  read while mounted: its log is in use
# ntfs.free lists the free clusters as Windows' own bitmap gives them
# (FSCTL_GET_VOLUME_BITMAP), the answer dd's NTFS driver is checked against.
# This machine's SID is replaced in the images (see machine-sid.ps1).
#   powershell -ExecutionPolicy Bypass -File tests\make-ntfs-fixtures.ps1 [-Out <dir>]
param(
    [string]$DD = (Join-Path $PSScriptRoot '..\build\dd.exe'),
    [string]$Out = (Join-Path $PSScriptRoot 'fixtures')
)

$ErrorActionPreference = 'Stop'
$DD = (Resolve-Path $DD).Path
$out = $Out
$work = Join-Path ([IO.Path]::GetTempPath()) ("ddntfs-" + [Guid]::NewGuid())
New-Item -ItemType Directory -Force $out, $work | Out-Null
$existing = @(Get-Disk | ForEach-Object Number)
. (Join-Path $PSScriptRoot 'machine-sid.ps1')

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class VolBitmap {
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr t);
    [DllImport("kernel32", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, ref long inBuf, int inLen, byte[] outBuf, int outLen, out int got, IntPtr ov);
    [DllImport("kernel32", SetLastError = true)]
    static extern bool FlushFileBuffers(SafeFileHandle h);
    // Windows' allocation bitmap of a mounted volume: bit i = cluster i in use
    public static byte[] Get(string volume, out long clusters) {
        using (SafeFileHandle h = CreateFileW(volume, 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            if (h.IsInvalid) throw new Win32Exception();
            FlushFileBuffers(h);
            long start = 0;
            byte[] buf = new byte[16 + (1 << 20)];
            int got;
            if (!DeviceIoControl(h, 0x0009006F, ref start, 8, buf, buf.Length, out got, IntPtr.Zero))   // FSCTL_GET_VOLUME_BITMAP
                throw new Win32Exception();
            clusters = BitConverter.ToInt64(buf, 8);
            byte[] bits = new byte[(clusters + 7) / 8];
            Array.Copy(buf, 16, bits, 0, bits.Length);
            return bits;
        }
    }
}
'@

function Diskpart([string[]]$lines) {
    $s = Join-Path $work 'dp.txt'
    Set-Content -Encoding ASCII $s $lines
    diskpart.exe /s $s | Out-Null
}
function Fill([string]$path, [long]$size, [byte]$value) {
    $chunk = New-Object byte[] 1MB
    for ($i = 0; $i -lt $chunk.Length; $i++) { $chunk[$i] = $value }
    $f = [IO.File]::Create($path)
    try { for ($n = 0; $n -lt $size; $n += $chunk.Length) { $f.Write($chunk, 0, [int][Math]::Min($chunk.Length, $size - $n)) } }
    finally { $f.Close() }
}
function Offline([int]$n, [bool]$off) {
    Set-Disk -Number $n -IsOffline $off
    Start-Sleep -Seconds 2
}
function Capture([int]$n, [string]$name) {
    & $DD "if=\\.\PhysicalDrive$n" "of=$work\$name.img" bs=1M count=16 status=none
    if ($LASTEXITCODE) { throw "cannot read disk $n" }
    foreach ($sid in Get-LocalSids) { $null = Convert-Sid "$work\$name.img" $sid $PlaceholderSid }
    Remove-Item "$out\$name.img.xz", "$out\$name.img.hashes" -ErrorAction SilentlyContinue
    & $DD "if=$work\$name.img" "of=$out\$name.img.xz" bs=1M --no-hashes status=none
    Write-Host ("{0,-12} {1,8:N0} bytes" -f $name, (Get-Item "$out\$name.img.xz").Length)
}

$vhd = Join-Path $work 'ntfs.vhdx'
try {
    Diskpart @("create vdisk file=`"$vhd`" maximum=16 type=fixed", "select vdisk file=`"$vhd`"", 'attach vdisk')
    $n = (Get-DiskImage -ImagePath $vhd).Number
    if ($existing -contains $n) { throw "the VHDX got the number of an existing disk ($n)" }
    Fill (Join-Path $work 'fill.bin') 16MB 0xA5
    & $DD "if=$work\fill.bin" "of=\\.\PhysicalDrive$n" bs=1M status=none
    Initialize-Disk -Number $n -PartitionStyle MBR
    $p = New-Partition -DiskNumber $n -Offset 1MB -Size 14MB -AssignDriveLetter
    Format-Volume -Partition $p -FileSystem NTFS -NewFileSystemLabel DDNTFS -Force | Out-Null
    $p = Get-Partition -DiskNumber $n -PartitionNumber $p.PartitionNumber
    $root = "$($p.DriveLetter):\"
    # files that compress well, and a deleted filler that leaves 0x3C in free space
    $line = 0
    foreach ($f in @(@('a.txt', 900000), @('sub\c.txt', 1300000), @('d.txt', 500))) {
        $sb = New-Object Text.StringBuilder
        while ($sb.Length -lt $f[1]) { [void]$sb.AppendLine("line $((++$line)) of $($f[0])") }
        [void][IO.Directory]::CreateDirectory((Split-Path "$root$($f[0])"))
        [IO.File]::WriteAllText("$root$($f[0])", $sb.ToString(0, $f[1]))
    }
    Fill "${root}filler.bin" ((Get-Volume -DriveLetter $p.DriveLetter).SizeRemaining - 512KB) 0x3C
    Remove-Item "${root}filler.bin"

    # Until NTFS commits a deletion, its live bitmap still counts the deleted
    # file's clusters as in use, and mounting allocates a little: a dismount
    # and a mount first, then Windows' bitmap and the clean image are taken
    # with nothing written in between.
    function Remount {
        Offline $n $true
        Offline $n $false
        $script:p = Get-Partition -DiskNumber $n -PartitionNumber $p.PartitionNumber
        if (-not $p.DriveLetter -or $p.DriveLetter -eq [char]0) {
            $p | Add-PartitionAccessPath -AssignDriveLetter
            $script:p = Get-Partition -DiskNumber $n -PartitionNumber $p.PartitionNumber
        }
        Start-Sleep -Seconds 5
    }
    Remount

    # what Windows says is free, in clusters of the partition
    $clusters = 0
    $bits = [VolBitmap]::Get("\\.\$($p.DriveLetter):", [ref]$clusters)
    $cluster = (Get-Volume -DriveLetter $p.DriveLetter).AllocationUnitSize
    $free = New-Object Collections.Generic.List[string]
    $start = -1
    for ($c = 0; $c -le $clusters; $c++) {
        $isFree = $c -lt $clusters -and -not ($bits[[int][Math]::Floor($c / 8)] -band (1 -shl ($c % 8)))
        if ($isFree -and $start -lt 0) { $start = $c }
        if (-not $isFree -and $start -ge 0) { $free.Add($(if ($c - 1 -eq $start) { "$start" } else { "$start-$($c - 1)" })); $start = -1 }
    }
    $lines = @("block_size $cluster", "block_count $clusters", "offset 1048576", "free $($free -join ', ')")
    [IO.File]::WriteAllText("$out\ntfs.free", ($lines -join "`n") + "`n")

    Capture $n 'ntfs-inuse'             # mounted: the log is in use
    Offline $n $true                    # taking it offline dismounts it cleanly
    Capture $n 'ntfs'
    Remount
    fsutil dirty set "$($p.DriveLetter):" | Out-Null
    Offline $n $true
    Capture $n 'ntfs-dirty'
} finally {
    try { Diskpart @("select vdisk file=`"$vhd`"", 'detach vdisk') } catch { }
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
