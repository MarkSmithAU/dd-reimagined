# --sparse-read of a disk with FAT12, FAT16, FAT32, exFAT, NTFS (formatted
# by Windows) and ext4 (tests\fixtures) partitions.  Needs an elevated prompt.
# The image is written back to a second VHDX, where Windows checks the file
# systems and the files are compared.  Writes only to its own VHDXs.
#   powershell -ExecutionPolicy Bypass -File tests\sparse-device-tests.ps1 [-DD build\dd.exe] [-Keep]
# -Keep leaves the images in the work folder, and says where.
param(
    [string]$DD = (Join-Path $PSScriptRoot '..\build\dd.exe'),
    [switch]$Keep,
    [string]$Fixtures = (Join-Path $PSScriptRoot 'fixtures')
)

$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error 'sparse-device-tests.ps1 needs an elevated prompt'
}
$DD = (Resolve-Path $DD).Path
# The fixtures are made, not kept with the sources: without them, say so
# and stop, rather than fail
if (-not ((Test-Path (Join-Path $Fixtures 'ext4.img.xz')) -and (Test-Path (Join-Path $Fixtures 'ntfs.img.xz')))) {
    Write-Host "skipped: no fixtures in $Fixtures; make them with tests\regenerate-fixtures.ps1 (elevated)"
    exit 0
}
$fixtures = (Resolve-Path $Fixtures).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("ddsparsedev-" + [Guid]::NewGuid())
New-Item -ItemType Directory $work | Out-Null
$script:failures = 0
$script:count = 0
$existing = @(Get-Disk | ForEach-Object Number)

function F([string]$name) { Join-Path $work $name }

function Run([string]$operands) {
    $err = F 'stderr.txt'
    $out = F 'stdout.txt'
    cmd /c "cd /d `"$work`" && `"$DD`" $operands >`"$out`" 2>`"$err`""
    $code = $LASTEXITCODE
    [pscustomobject]@{
        Code = $code
        Out  = ([IO.File]::ReadAllText($out)).Replace("`r", '')
        Err  = ([IO.File]::ReadAllText($err)).Replace("`r", '')
    }
}

function Check([string]$what, [bool]$ok, [string]$detail = '') {
    $script:count++
    if ($ok) { Write-Host "ok   $what" }
    else {
        $script:failures++
        Write-Host "FAIL $what" -ForegroundColor Red
        if ($detail) { Write-Host "     $detail" }
    }
}

function Diskpart([string[]]$lines) {
    $s = F 'dp.txt'
    Set-Content -Encoding ASCII $s $lines
    diskpart.exe /s $s | Out-Null
}

$vhds = @()
function New-Vhd([string]$name, [int]$mb, [string[]]$more = @()) {
    $path = F "$name.vhdx"
    $script:vhds += $path
    Diskpart (@("create vdisk file=`"$path`" maximum=$mb type=fixed", "select vdisk file=`"$path`"", 'attach vdisk') + $more)
    $n = (Get-DiskImage -ImagePath $path).Number
    if ($existing -contains $n) { throw "VHDX $name got the number of an existing disk ($n)" }
    $n
}
function Detach([string]$name) {
    Diskpart @("select vdisk file=`"$(F "$name.vhdx")`"", 'detach vdisk')
    $script:vhds = @($script:vhds | Where-Object { $_ -ne (F "$name.vhdx") })
}

Add-Type -TypeDefinition @'
using System;
using System.IO;
public static class SparseDiff {
    // Over [from, to): { bytes the sparse image has that are neither the original's nor zero,
    //                    bytes the sparse image zeroed }
    public static long[] Run(string full, string sparse, long from, long to) {
        long[] r = new long[2];
        using (FileStream a = File.OpenRead(full), b = File.OpenRead(sparse)) {
            byte[] x = new byte[1 << 20], y = new byte[1 << 20];
            a.Position = from; b.Position = from;
            for (long pos = from; pos < to; ) {
                int n = (int)Math.Min(x.Length, to - pos);
                if (a.Read(x, 0, n) != n || b.Read(y, 0, n) != n) { r[0] = -1; return r; }
                for (int i = 0; i < n; i++)
                    if (x[i] != y[i]) { if (y[i] != 0) r[0]++; else r[1]++; }
                pos += n;
            }
        }
        return r;
    }
    public static string Hash(string file, long from, long len) {
        using (FileStream a = File.OpenRead(file)) {
            byte[] buf = new byte[len];
            a.Position = from;
            int got = 0;
            while (got < len) { int n = a.Read(buf, got, (int)(len - got)); if (n <= 0) break; got += n; }
            return BitConverter.ToString(System.Security.Cryptography.SHA256.Create().ComputeHash(buf, 0, got));
        }
    }
}
'@

function Patch([string]$name, [long]$offset, [byte[]]$bytes) {
    $f = [IO.File]::Open((F $name), 'Open', 'ReadWrite')
    $f.Position = $offset; $f.Write($bytes, 0, $bytes.Length); $f.Close()
}
# size bytes of one value, written a MiB at a time
function Fill([string]$path, [long]$size, [byte]$value) {
    $chunk = New-Object byte[] 1MB
    $chunk[0] = $value
    for ($n = 1; $n -lt $chunk.Length; $n *= 2) { [Array]::Copy($chunk, 0, $chunk, $n, [Math]::Min($n, $chunk.Length - $n)) }
    $f = [IO.File]::Create($path)
    try { for ($n = 0; $n -lt $size; $n += $chunk.Length) { $f.Write($chunk, 0, [int][Math]::Min($chunk.Length, $size - $n)) } }
    finally { $f.Close() }
}
function Peek([string]$name, [long]$offset, [int]$n) {
    $f = [IO.File]::OpenRead((F $name)); $b = New-Object byte[] $n
    $f.Position = $offset; [void]$f.Read($b, 0, $n); $f.Close(); $b
}

try {
    $r = Run "if=`"$fixtures\ext4.img.xz`" of=ext4.img bs=1M status=none"
    $r = Run "if=ext4.img of=ext4.out bs=1M --sparse-read status=none"

    # --- the source disk: free space that is not zeros -----------------------------
    $s = New-Vhd 'src' 192
    Fill (F 'fill.bin') 192MB 0xA5
    $r = Run "if=fill.bin of=\\.\PhysicalDrive$s bs=1M status=none"
    if ($r.Code) { throw "cannot fill the source disk: $($r.Err)" }
    Diskpart @("select disk $s", 'convert mbr')
    Initialize-Disk -Number $s -PartitionStyle MBR -ErrorAction SilentlyContinue
    $vols = [ordered]@{}
    $want = [ordered]@{ 'FAT12' = @(4MB, 'FAT'); 'FAT16' = @(24MB, 'FAT'); 'FAT32' = @(48MB, 'FAT32') }
    foreach ($k in $want.Keys) {
        $p = New-Partition -DiskNumber $s -Size $want[$k][0] -MbrType FAT32 -AssignDriveLetter
        Format-Volume -Partition $p -FileSystem $want[$k][1] -NewFileSystemLabel $k -Force | Out-Null
        $vols[$k] = Get-Partition -DiskNumber $s -PartitionNumber $p.PartitionNumber
    }
    Diskpart @("select disk $s", 'create partition extended', 'create partition logical size=32', 'create partition logical size=16',
        'create partition logical size=24')
    Update-Disk -Number $s
    $after = $vols['FAT32'].Offset + $vols['FAT32'].Size
    $logical = @(Get-Partition -DiskNumber $s | Where-Object { $_.Type -ne 'Extended' -and $_.Offset -ge $after } | Sort-Object Offset)
    if ($logical.Count -ne 3) { throw "expected three logical partitions: $(Get-Partition -DiskNumber $s | Out-String)" }
    $p = $logical[0] | Add-PartitionAccessPath -AssignDriveLetter -PassThru
    $vols['exFAT'] = Get-Partition -DiskNumber $s -PartitionNumber $logical[0].PartitionNumber
    Format-Volume -Partition $vols['exFAT'] -FileSystem exFAT -NewFileSystemLabel EXF -Force | Out-Null
    $vols['exFAT'] = Get-Partition -DiskNumber $s -PartitionNumber $logical[0].PartitionNumber
    $extPart = $logical[1]
    $logical[2] | Add-PartitionAccessPath -AssignDriveLetter
    $vols['NTFS'] = Get-Partition -DiskNumber $s -PartitionNumber $logical[2].PartitionNumber
    Format-Volume -Partition $vols['NTFS'] -FileSystem NTFS -NewFileSystemLabel NTF -Force | Out-Null
    $vols['NTFS'] = Get-Partition -DiskNumber $s -PartitionNumber $logical[2].PartitionNumber

    # files that stay, and a filler file that is deleted to leave old data in free space
    $rng = New-Object Random 9
    $files = @{}
    foreach ($k in $vols.Keys) {
        $root = "$($vols[$k].DriveLetter):\"
        New-Item -ItemType Directory "$root\dir" | Out-Null
        foreach ($f in @(@('a.bin', 300000), @('dir\b.bin', 900000), @('c.txt', 5000))) {
            $b = New-Object byte[] $f[1]; $rng.NextBytes($b)
            [IO.File]::WriteAllBytes("$root$($f[0])", $b)
            $files["$k|$($f[0])"] = (Get-FileHash "$root$($f[0])").Hash
        }
        $free = (Get-Volume -DriveLetter $vols[$k].DriveLetter).SizeRemaining
        Fill "${root}filler.bin" ([Math]::Max(0, $free - 256KB)) 0x3C
        Remove-Item "${root}filler.bin"
    }
    $freeTotal = 0
    foreach ($k in $vols.Keys) { $freeTotal += (Get-Volume -DriveLetter $vols[$k].DriveLetter).SizeRemaining }
    # NTFS read from a mounted disk always has its log in use: in an image
    # file it is read whole, so image-file checks leave its free space out
    $freeNoNtfs = $freeTotal - (Get-Volume -DriveLetter $vols['NTFS'].DriveLetter).SizeRemaining
    # ext4 goes into the second logical partition, as raw bytes
    $r = Run "if=ext4.img of=\\.\PhysicalDrive$s bs=1M seek=$($extPart.Offset) oflag=seek_bytes conv=notrunc status=none"
    Check 'the ext4 image is written into its partition' ($r.Code -eq 0) $r.Err
    Start-Sleep -Seconds 2
    foreach ($k in @($vols.Keys)) { $vols[$k] = Get-Partition -DiskNumber $s -PartitionNumber $vols[$k].PartitionNumber }

    # --- reading it -------------------------------------------------------------------
    $r = Run "if=\\.\PhysicalDrive$s of=nolock.img bs=1M --sparse-read"
    Check 'reading a mounted disk without --lock-input warns' ($r.Code -eq 0 -and $r.Err -match 'mapped before reading them; if they are mounted, --lock-input') $r.Err
    Check '... and the mounted NTFS volume, its log in use, is read whole' ($r.Err -match 'partition 7 \(NTFS\): reading all .*: its log needs replaying') $r.Err
    $r = Run "if=\\.\PhysicalDrive$s of=full.img bs=1M --lock-input --no-hashes status=none"
    Check 'the whole disk is read' ($r.Code -eq 0) $r.Err
    $r = Run "if=\\.\PhysicalDrive$s of=sparse.img bs=1M --lock-input --sparse-read --hashes"
    Check 'sparse read with --lock-input' ($r.Code -eq 0 -and $r.Err -notmatch 'mapped before reading') $r.Err
    foreach ($k in 'FAT12', 'FAT16', 'FAT32', 'exFAT') {
        Check "$k is found and read sparsely" ($r.Err -match "partition \d \($k\): reading the [\d.]+ [KM]iB in use of") $r.Err
    }
    Check 'ext4 in a logical partition is read sparsely' ($r.Err -match 'partition 6 \(ext4\): reading the') $r.Err
    Check '... and so is exFAT, in the other logical partition' ($r.Err -match 'partition 5 \(exFAT\): reading the') $r.Err
    Check 'NTFS, mounted and locked (Windows says it mounted clean), is read sparsely' ($r.Err -match 'partition 7 \(NTFS\): reading the [\d.]+ [KM]iB in use of') $r.Err
    Check 'a sparse disk image gets a hash file too' (Test-Path (F 'sparse.img.hashes'))
    # NTFS writes its log and MFT sequence numbers each time it is locked, so
    # between these two reads its metadata changes: it is checked by the
    # files and chkdsk on the second disk below, and against Windows' own
    # bitmap in sparse-tests.  Everything else must differ only by zeros.
    $len = (Get-Item (F 'full.img')).Length
    $nOff = $vols['NTFS'].Offset; $nEnd = $nOff + $vols['NTFS'].Size
    $d1 = [SparseDiff]::Run((F 'full.img'), (F 'sparse.img'), 0, $nOff)
    $d2 = [SparseDiff]::Run((F 'full.img'), (F 'sparse.img'), $nEnd, $len)
    $d3 = [SparseDiff]::Run((F 'full.img'), (F 'sparse.img'), $nOff, $nEnd)
    Check 'the sparse image differs from the disk only by zeros (outside NTFS)' ($d1[0] -eq 0 -and $d2[0] -eq 0) "$($d1[0] + $d2[0]) bytes differ otherwise"
    Check 'most of the free space is zeroed' ($d1[1] + $d2[1] + $d3[1] -gt $freeTotal * 0.85) "zeroed $($d1[1] + $d2[1] + $d3[1]) of about $freeTotal free"
    $ext = [SparseDiff]::Hash((F 'sparse.img'), $extPart.Offset, 16MB)
    Check 'the ext4 partition is as a sparse read of the ext4 image alone' ($ext -eq [SparseDiff]::Hash((F 'ext4.out'), 0, 16MB))

    # --- a volume as input ---------------------------------------------------------------
    $v = $vols['FAT32']
    $r = Run "if=\\.\$($v.DriveLetter): of=vol.img bs=1M --lock-input --sparse-read"
    Check 'a FAT32 volume as input' ($r.Code -eq 0 -and $r.Err -match 'the input \(FAT32\): reading the') $r.Err
    Check '... gives the same bytes as its partition in the disk image' ([SparseDiff]::Hash((F 'vol.img'), 0, $v.Size) -eq [SparseDiff]::Hash((F 'sparse.img'), $v.Offset, $v.Size))
    $r = Run "--layout --sparse-read if=\\.\PhysicalDrive$s"
    Check '--layout --sparse-read of the disk' ($r.Code -eq 0 -and $r.Out -match 'FAT12' -and $r.Out -match 'FAT16' -and $r.Out -match 'FAT32' -and
        $r.Out -match 'exFAT' -and $r.Out -match 'ext4') $r.Out
    # marked dirty, Windows says so even under the lock: read whole
    fsutil dirty set "$($vols['NTFS'].DriveLetter):" | Out-Null
    $r = Run "if=\\.\PhysicalDrive$s of=ntfsdirty.img bs=1M --lock-input --sparse-read"
    Check 'NTFS marked dirty, read with --lock-input: read whole' ($r.Code -eq 0 -and
        $r.Err -match 'partition 7 \(NTFS\): reading all .*: it was not cleanly unmounted, says \$Volume') $r.Err
    Detach 'src'

    # --- not cleanly unmounted (in a copy of the image) ----------------------------------
    Copy-Item (F 'full.img') (F 'dirty.img')
    $o16 = $vols['FAT16'].Offset; $o32 = $vols['FAT32'].Offset; $oex = $vols['exFAT'].Offset
    $bs16 = Peek 'dirty.img' $o16 32
    $fat16 = $o16 + [BitConverter]::ToUInt16($bs16, 14) * [BitConverter]::ToUInt16($bs16, 11)
    $e1 = Peek 'dirty.img' ($fat16 + 2) 2
    Patch 'dirty.img' ($fat16 + 2) @($e1[0], ($e1[1] -band 0x7F))          # FAT16: clean shutdown bit clear
    $b41 = Peek 'dirty.img' ($o32 + 0x41) 1
    Patch 'dirty.img' ($o32 + 0x41) @(($b41[0] -bor 1))                    # FAT32: boot sector dirty bit
    $fl = Peek 'dirty.img' ($oex + 106) 1
    Patch 'dirty.img' ($oex + 106) @(($fl[0] -bor 2))                      # exFAT: VolumeDirty
    $r = Run "if=dirty.img of=dirty.out bs=1M --sparse-read"
    foreach ($k in 'FAT16', 'FAT32', 'exFAT') {
        Check "$k not cleanly unmounted: read whole" ($r.Err -match "partition \d \($k\): reading all .*: it was not cleanly unmounted") $r.Err
    }
    Check '... the clean FAT12 is still sparse' ($r.Err -match 'partition 1 \(FAT12\): reading the') $r.Err
    Check '... those partitions are copied unchanged' ([SparseDiff]::Hash((F 'dirty.out'), $o16, $vols['FAT16'].Size) -eq [SparseDiff]::Hash((F 'dirty.img'), $o16, $vols['FAT16'].Size) -and
        [SparseDiff]::Hash((F 'dirty.out'), $oex, $vols['exFAT'].Size) -eq [SparseDiff]::Hash((F 'dirty.img'), $oex, $vols['exFAT'].Size))
    $r = Run "if=dirty.img of=dirty2.out bs=1M --sparse-read --allow-unclean"
    foreach ($k in 'FAT16', 'FAT32', 'exFAT') {
        Check "$k with --allow-unclean: sparse, with a caution" ($r.Err -match "partition \d \($k\): reading the .*not cleanly unmounted") $r.Err
    }
    $d = [SparseDiff]::Run((F 'dirty.img'), (F 'dirty2.out'), 0, (Get-Item (F 'dirty.img')).Length)
    Check '... differing only by zeros, and as much zeroed as when clean' ($d[0] -eq 0 -and $d[1] -gt $freeNoNtfs * 0.85) "$($d[0]) / $($d[1])"

    # --- the sparse image, written to another disk -------------------------------------------
    $t = New-Vhd 'dst' 192
    $r = Run "if=sparse.img of=\\.\PhysicalDrive$t bs=1M --verify"
    Check 'the sparse image is written to a disk and verified' ($r.Code -eq 0 -and $r.Err -match 'SHA-256 of sparse\.img matches' -and $r.Err -match 'verified') $r.Err
    Start-Sleep -Seconds 2
    Update-Disk -Number $t -ErrorAction SilentlyContinue
    foreach ($k in $vols.Keys) {
        $p = Get-Partition -DiskNumber $t -PartitionNumber $vols[$k].PartitionNumber
        if (-not $p.DriveLetter -or $p.DriveLetter -eq [char]0) { $p | Add-PartitionAccessPath -AssignDriveLetter; Start-Sleep -Seconds 1 }
        $p = Get-Partition -DiskNumber $t -PartitionNumber $vols[$k].PartitionNumber
        $root = "$($p.DriveLetter):\"
        $same = $true
        foreach ($f in 'a.bin', 'dir\b.bin', 'c.txt') {
            if (-not (Test-Path "$root$f") -or (Get-FileHash "$root$f").Hash -ne $files["$k|$f"]) { $same = $false }
        }
        Check "${k}: every file reads back intact" $same (Get-ChildItem -Recurse $root | Out-String)
        $c = cmd /c "chkdsk $($p.DriveLetter): 2>&1"
        Check "${k}: chkdsk finds no problems (exit $LASTEXITCODE)" ($LASTEXITCODE -eq 0) ($c -join "`n")
    }
    Detach 'dst'

    # --- the dirty image on a disk: Windows mounts it dirty, and says so ------------------
    $t = New-Vhd 'dirtydst' 192
    $r = Run "if=dirty.img of=\\.\PhysicalDrive$t bs=1M status=none"
    Start-Sleep -Seconds 2
    Update-Disk -Number $t -ErrorAction SilentlyContinue
    $r = Run "if=\\.\PhysicalDrive$t of=dirty3.img bs=1M --lock-input --sparse-read"
    # Linux and Windows mark a dirty FAT volume in its boot sector, which
    # Windows checks when it mounts it.  The FAT16 was marked only in FAT
    # entry 1, which Windows does not check and rewrites itself.
    foreach ($k in 'FAT32', 'exFAT') {
        Check "$k mounted dirty, read with --lock-input: still read whole" ($r.Err -match "partition \d \($k\): reading all .*: it was not cleanly unmounted") $r.Err
    }
    Check 'FAT16 marked only in its FAT, which Windows mounted as clean: sparse' ($r.Err -match 'partition 2 \(FAT16\): reading the') $r.Err
    Detach 'dirtydst'
} finally {
    foreach ($v in $vhds) { try { Diskpart @("select vdisk file=`"$v`"", 'detach vdisk') } catch { } }
    if ($Keep) { Write-Host "kept $work" } else { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}

Write-Host ""
Write-Host "$($script:count - $script:failures) of $script:count passed"
exit [int]($script:failures -ne 0)
