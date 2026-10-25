# Makes the ext2/3/4 images that sparse-tests.ps1 uses, in tests\fixtures:
# <name>.img.xz (16 MiB images, compressed with dd) and <name>.free (the
# block size, the block count and the free blocks, from dumpe2fs).  Free
# space is filled with 0xA5 first, so sparse reading visibly zeroes it.
# Needs e2fsprogs (mke2fs, dumpe2fs, debugfs), for example Cygwin's:
# regenerate-fixtures.ps1 downloads them and runs this.
#   powershell -ExecutionPolicy Bypass -File tests\make-sparse-fixtures.ps1 -E2fs <dir with mke2fs.exe> [-Bin <dir with cygwin1.dll>] [-Out <dir>]
param(
    [Parameter(Mandatory)] [string]$E2fs,
    [string]$Bin = '',
    [string]$DD = (Join-Path $PSScriptRoot '..\build\dd.exe'),
    [string]$Out = (Join-Path $PSScriptRoot 'fixtures')
)

$ErrorActionPreference = 'Continue'
$DD = (Resolve-Path $DD).Path
$out = $Out
$work = Join-Path ([IO.Path]::GetTempPath()) ("ddfix-" + [Guid]::NewGuid())
New-Item -ItemType Directory -Force $out, $work, "$work\src\sub" | Out-Null
if ($Bin) { $env:PATH = "$Bin;$env:PATH" }
function Tool([string]$name) { & (Join-Path $E2fs "$name.exe") @args 2>&1 | ForEach-Object { "$_" } }

# files that compress well but are not trivial
$line = 0
foreach ($f in @(@('a.txt', 900000), @('b.txt', 40000), @('sub\c.txt', 1300000), @('sub\d.txt', 500))) {
    $sb = New-Object Text.StringBuilder
    while ($sb.Length -lt $f[1]) { [void]$sb.AppendLine("line $((++$line)) of $($f[0])") }
    [IO.File]::WriteAllText("$work\src\$($f[0])", $sb.ToString(0, $f[1]))
}

function Image([string]$name, [string]$opts, [int]$mb = 16) {
    $img = "$work\$name.img"
    $fs = [IO.File]::Create($img)
    $blk = New-Object byte[] 1MB
    for ($i = 0; $i -lt $blk.Length; $i++) { $blk[$i] = 0xA5 }
    for ($n = 0; $n -lt $mb; $n++) { $fs.Write($blk, 0, $blk.Length) }
    $fs.Close()
    Tool mke2fs -q -F -E nodiscard $opts.Split(' ') -d "$work\src" $img | Out-Null
    $img
}

# $img is the file system; $whole (if given) is the image compressed, which
# holds it at the offset named in $extra
function Finish([string]$name, [string]$img, [string[]]$extra = @(), [string]$whole = '') {
    if (-not $whole) { $whole = $img }
    $d = (Tool dumpe2fs $img) -join "`n"
    $bs = [regex]::Match($d, '(?m)^Block size:\s+(\d+)').Groups[1].Value
    $count = [regex]::Match($d, '(?m)^Block count:\s+(\d+)').Groups[1].Value
    $free = [regex]::Matches($d, '(?m)^\s+Free blocks: (.+)$') | ForEach-Object { $_.Groups[1].Value.Trim() }
    $lines = @("block_size $bs", "block_count $count") + $extra + @("free $(($free -join ', '))")
    [IO.File]::WriteAllText("$out\$name.free", ($lines -join "`n") + "`n")
    Remove-Item "$out\$name.img.xz", "$out\$name.img.hashes" -ErrorAction SilentlyContinue
    & $DD if=$whole of="$out\$name.img.xz" bs=1M --no-hashes status=none
    Write-Host ("{0,-16} {1,8:N0} bytes" -f $name, (Get-Item "$out\$name.img.xz").Length)
}

$cases = [ordered]@{
    'ext4'          = '-t ext4 -b 4096 -I 256 -g 1024'                 # 4 groups, flex_bg, uninitialised groups
    'ext4-1k'       = '-t ext4 -b 1024 -g 2048 -G 4'
    'ext3'          = '-t ext3 -b 1024 -g 2048'
    'ext2'          = '-t ext2 -b 2048 -g 2048'
    'ext4-bigalloc' = '-t ext4 -b 1024 -O bigalloc -C 16384'
    'ext4-metabg'   = '-t ext4 -b 1024 -g 1024 -O meta_bg,^resize_inode'
}
foreach ($name in $cases.Keys) { Finish $name (Image $name $cases[$name]) }

# A 16 MiB disk image: an MBR, then a 14 MiB ext4 partition at 1 MiB, with
# 0x5C in the gap and after it (not a file system: always read)
$part = Image 'mbr-ext4-part' '-t ext4 -b 1024 -g 2048' 14
$disk = New-Object byte[] (16MB)
for ($i = 512; $i -lt $disk.Length; $i++) { $disk[$i] = 0x5C }
[Array]::Copy([IO.File]::ReadAllBytes($part), 0, $disk, 1MB, 14MB)
$disk[446 + 4] = 0x83
[BitConverter]::GetBytes([uint32](1MB / 512)).CopyTo($disk, 446 + 8)
[BitConverter]::GetBytes([uint32](14MB / 512)).CopyTo($disk, 446 + 12)
$disk[510] = 0x55; $disk[511] = 0xAA
[IO.File]::WriteAllBytes("$work\mbr-ext4.img", $disk)
Finish 'mbr-ext4' $part @('offset 1048576') "$work\mbr-ext4.img"

# A journal that needs recovery: the last free block of group 0 gets data,
# and a journalled copy of group 0's bitmap marks it in use, as a crash
# before the checkpoint leaves it.  Recovery would keep that block.
$img = Image 'ext4-recover' '-t ext4 -b 4096 -g 1024 -O ^metadata_csum'
$d = (Tool dumpe2fs $img) -join "`n"
$bitmap = [long][regex]::Match($d, 'Group 0:[\s\S]*?Block bitmap at (\d+)').Groups[1].Value
$x = [long](([regex]::Match($d, 'Group 0:[\s\S]*?Free blocks: ([^\n]*)').Groups[1].Value.Split(',')[-1]).Trim().Split('-')[-1])
$f = [IO.File]::Open($img, 'Open', 'ReadWrite')
$pat = New-Object byte[] 4096
for ($i = 0; $i -lt $pat.Length; $i++) { $pat[$i] = 0x5A }
$f.Position = $x * 4096; $f.Write($pat, 0, 4096)
$bm = New-Object byte[] 4096
$f.Position = $bitmap * 4096; [void]$f.Read($bm, 0, 4096); $f.Close()
$bm[[int][Math]::Floor($x / 8)] = $bm[[int][Math]::Floor($x / 8)] -bor (1 -shl ($x % 8))
[IO.File]::WriteAllBytes("$work\bm.bin", $bm)
Set-Content -Encoding ASCII "$work\j.txt" @('jo', "jw -b $bitmap $($work.Replace('\', '/'))/bm.bin", 'jc')
Tool debugfs -w -f "$work\j.txt" $img | Out-Null
if (((Tool dumpe2fs -h $img) -join ' ') -notmatch 'needs_recovery') { Write-Error 'debugfs did not leave a journal to recover' }
Finish 'ext4-recover' $img @("journal_block $x")

Remove-Item -Recurse -Force $work
