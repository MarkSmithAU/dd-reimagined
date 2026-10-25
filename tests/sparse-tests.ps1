# --sparse-read on ext2/3/4 images (tests\fixtures, made by make-sparse-fixtures.ps1
# with e2fsprogs), in files only: no elevation needed.  FAT and exFAT, which
# Windows formats itself, are tested on VHDXs by sparse-device-tests.ps1.
#   powershell -ExecutionPolicy Bypass -File tests\sparse-tests.ps1 [-DD build\dd.exe]
param(
    [string]$DD = (Join-Path $PSScriptRoot '..\build\dd.exe'),
    [string]$Fixtures = (Join-Path $PSScriptRoot 'fixtures')
)

$ErrorActionPreference = 'Stop'
$DD = (Resolve-Path $DD).Path
# The fixtures are made, not kept with the sources: without them, say so
# and stop, rather than fail
if (-not ((Test-Path (Join-Path $Fixtures 'ext4.img.xz')) -and (Test-Path (Join-Path $Fixtures 'ntfs.img.xz')))) {
    Write-Host "skipped: no fixtures in $Fixtures; make them with tests\regenerate-fixtures.ps1 (elevated)"
    exit 0
}
$fixtures = (Resolve-Path $Fixtures).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("ddsparse-" + [Guid]::NewGuid())
New-Item -ItemType Directory $work | Out-Null
$script:failures = 0
$script:count = 0

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

function Hash([string]$name) { (Get-FileHash (F $name)).Hash }

# Compares a sparse read with its input, block by block, against the free
# blocks dumpe2fs listed: a used block must be identical; a free one either
# identical or zeros.  Bytes outside the file system must be identical.
Add-Type -TypeDefinition @'
using System;
using System.IO;
public static class SparseCheck {
    // { used blocks that differ, free blocks zeroed, free nonzero blocks kept, bytes outside that differ, the first kept block }
    public static long[] Run(string input, string output, long fsOffset, int bs, long count, string free) {
        bool[] isFree = new bool[count];
        foreach (string part in free.Split(',')) {
            string p = part.Trim();
            if (p.Length == 0) continue;
            string[] ab = p.Split('-');
            long a = long.Parse(ab[0]), b = ab.Length > 1 ? long.Parse(ab[1]) : a;
            for (long k = a; k <= b && k < count; k++) isFree[k] = true;
        }
        byte[] x = File.ReadAllBytes(input), y = File.ReadAllBytes(output);
        long[] r = new long[5];
        r[4] = -1;
        if (x.Length != y.Length) { r[3] = -1; return r; }
        long end = fsOffset + count * bs;
        for (long i = 0; i < x.Length; i++)
            if ((i < fsOffset || i >= end) && x[i] != y[i]) r[3]++;
        for (long blk = 0; blk < count; blk++) {
            long o = fsOffset + blk * bs;
            bool same = true, zero = true, inZero = true;
            for (int i = 0; i < bs; i++) {
                if (x[o + i] != y[o + i]) same = false;
                if (y[o + i] != 0) zero = false;
                if (x[o + i] != 0) inZero = false;
            }
            if (same) {
                if (isFree[blk] && !inZero) { r[2]++; if (r[4] < 0) r[4] = blk; }
            } else if (!isFree[blk] || !zero) r[0]++;
            else r[1]++;
        }
        return r;
    }
    // The first len bytes of two files are the same
    public static bool Same(string a, string b, int len) {
        byte[] x = File.ReadAllBytes(a), y = File.ReadAllBytes(b);
        if (x.Length < len || y.Length < len) return false;
        for (int i = 0; i < len; i++) if (x[i] != y[i]) return false;
        return true;
    }
}
'@

function Fixture([string]$name) {
    $info = @{}
    foreach ($l in Get-Content (Join-Path $fixtures "$name.free")) {
        $k, $v = $l.Split(' ', 2)
        $info[$k] = $v
    }
    $info
}

function Blocks([string]$in, [string]$out, $info, [long]$offset = 0) {
    $r = [SparseCheck]::Run((F $in), (F $out), $offset, [int]$info.block_size, [long]$info.block_count, $info.free)
    [pscustomobject]@{ Bad = $r[0]; Zeroed = $r[1]; Kept = $r[2]; Outside = $r[3]; FirstKept = $r[4] }
}

# A map's "check" line is the SHA-256 of everything above it: an edited map
# with a new one, as a deliberate edit would have
function Reseal([string]$t) {
    $body = $t.Substring(0, $t.LastIndexOf("`ncheck ") + 1)
    $sha = [Security.Cryptography.SHA256]::Create()
    $h = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($body)) | ForEach-Object { $_.ToString('x2') })
    $body + "check $h`nend`n"
}

# A sparse map whose hash no longer matches its image: the first hex digit changed
function Tamper([string]$map) {
    $t = [IO.File]::ReadAllText((F $map))
    $m = [regex]::Match($t, '(?m)^sha256 (.)')
    $d = if ($m.Groups[1].Value -eq '0') { '1' } else { '0' }
    [IO.File]::WriteAllText((F $map), (Reseal ($t.Substring(0, $m.Groups[1].Index) + $d + $t.Substring($m.Groups[1].Index + 1))))
}

function FreeBlocks($info) {
    $n = 0
    foreach ($p in $info.free.Split(',')) { $a, $b = $p.Trim().Split('-'); $n += $(if ($b) { [long]$b - [long]$a + 1 } else { 1 }) }
    $n
}

try {
    # --- each kind of ext file system, whole --------------------------------
    $kinds = @{ 'ext4' = 'ext4'; 'ext4-1k' = 'ext4'; 'ext3' = 'ext3'; 'ext2' = 'ext2'; 'ext4-bigalloc' = 'ext4'; 'ext4-metabg' = 'ext4' }
    foreach ($name in 'ext4', 'ext4-1k', 'ext3', 'ext2', 'ext4-bigalloc', 'ext4-metabg', 'ext4-recover') {
        $r = Run "if=`"$fixtures\$name.img.xz`" of=$name.img bs=1M status=none"
        if ($r.Code) { throw "cannot decompress $name.img.xz: $($r.Err)" }
    }
    foreach ($name in 'ext4', 'ext4-1k', 'ext3', 'ext2', 'ext4-bigalloc', 'ext4-metabg') {
        $info = Fixture $name
        $r = Run "if=$name.img of=$name.out bs=1M --sparse-read"
        Check "${name}: read as $($kinds[$name]), only its used space" ($r.Code -eq 0 -and $r.Err -match "the input \($($kinds[$name])\): reading the [\d.]+ [KM]iB in use of 16 MiB") $r.Err
        $b = Blocks "$name.img" "$name.out" $info
        $free = FreeBlocks $info
        Check "${name}: used blocks are identical, and every free block is zeroed" ($b.Bad -eq 0 -and $b.Outside -eq 0 -and $b.Kept -eq 0 -and $b.Zeroed -gt $free * 0.9) "$($b | Out-String) of $free free"
        Check "${name}: the records count the whole image" ($r.Err -match "`n16\+0 records in`n16\+0 records out") $r.Err
        $m = [regex]::Match($r.Err, 'free space written as zeros') ; $z = [regex]::Match($r.Err, '\((\d+)\) of free space written as zeros')
        Check "${name}: the free space written as zeros is reported" ($z.Success -and [long]$z.Groups[1].Value -ge $b.Zeroed * [int]$info.block_size) $r.Err
    }

    # --- block sizes that do not match the file system's ------------------------
    $r = Run "if=ext4-1k.img of=odd.out bs=1000 --sparse-read status=none"
    Check 'an odd bs gives the same output' ($r.Code -eq 0 -and (Hash 'odd.out') -eq (Hash 'ext4-1k.out')) $r.Err
    $r = Run "if=ext4-1k.img of=odd2.out ibs=3000 obs=7000 iflag=fullblock --sparse-read status=none"
    Check 'ibs, obs and fullblock give the same output' ($r.Code -eq 0 -and (Hash 'odd2.out') -eq (Hash 'ext4-1k.out')) $r.Err
    $r = Run "if=ext4.img of=part.out bs=4096 skip=500 count=1000 --sparse-read status=none"
    $r2 = Run "if=ext4.out of=part-ref.out bs=4096 skip=500 count=1000 status=none"
    Check 'skip and count apply to the sparse input' ($r.Code -eq 0 -and (Hash 'part.out') -eq (Hash 'part-ref.out')) $r.Err

    # --- with other options ------------------------------------------------------------
    $r = Run "if=ext4.img of=ext4-xz.img.xz bs=1M --sparse-read --xz=1 status=none"
    $r2 = Run "if=ext4-xz.img.xz of=ext4-unxz.out bs=1M status=none"
    $plain = Run "if=ext4.img of=ext4-plain.img.xz bs=1M --xz=1 status=none"
    Check 'into .xz: decompresses to the sparse image' ($r.Code -eq 0 -and (Hash 'ext4-unxz.out') -eq (Hash 'ext4.out')) $r.Err
    Check '... and compresses better than the whole image' ((Get-Item (F 'ext4-xz.img.xz')).Length -lt (Get-Item (F 'ext4-plain.img.xz')).Length)
    $r = Run "if=ext4.img of=ext4-sp.out bs=1M --sparse-read conv=sparse --verify"
    Check 'with conv=sparse and --verify' ($r.Code -eq 0 -and $r.Err -match "verified: 'ext4-sp\.out'" -and (Hash 'ext4-sp.out') -eq (Hash 'ext4.out')) $r.Err
    $r = Run "if=ext4.img of=hashed.img bs=1M --hashes status=none"
    $r = Run "if=hashed.img of=hashed.out bs=1M --sparse-read"
    Check 'an input''s hash file is not checked, and says so' ($r.Code -eq 0 -and $r.Err -match 'not checking .* sparse reading leaves out its free space') $r.Err

    # --- a damaged or short file system is read whole --------------------------------
    $b = [IO.File]::ReadAllBytes((F 'ext4.img'))
    $f = [IO.File]::Create((F 'short.img')); $f.Write($b, 0, 10MB); $f.Close()
    $r = Run "if=short.img of=short.out bs=1M --sparse-read"
    Check 'a file system cut short is read whole' ($r.Code -eq 0 -and $r.Err -match 'reading all 10 MiB: the file system runs past the end of the input' -and (Hash 'short.out') -eq (Hash 'short.img')) $r.Err
    $b = [IO.File]::ReadAllBytes((F 'ext4.img'))
    $b[4096 + 0] = 0xFF; $b[4096 + 1] = 0xFF; $b[4096 + 2] = 0xFF; $b[4096 + 3] = 0x7F   # group 0's bitmap beyond the end
    [IO.File]::WriteAllBytes((F 'baddesc.img'), $b)
    $r = Run "if=baddesc.img of=baddesc.out bs=1M --sparse-read"
    Check 'damaged group descriptors: read whole' ($r.Code -eq 0 -and $r.Err -match 'group descriptors are damaged' -and (Hash 'baddesc.out') -eq (Hash 'baddesc.img')) $r.Err
    $b = [IO.File]::ReadAllBytes((F 'ext4.img'))
    $b[1024 + 96] = $b[1024 + 96] -bor 1     # incompat: compression, which dd does not know
    [IO.File]::WriteAllBytes((F 'unknown.img'), $b)
    $r = Run "if=unknown.img of=unknown.out bs=1M --sparse-read"
    Check 'an unknown incompatible feature: read whole' ($r.Code -eq 0 -and $r.Err -match 'features dd does not know' -and (Hash 'unknown.out') -eq (Hash 'unknown.img')) $r.Err

    # --- not cleanly unmounted -----------------------------------------------------------
    $info = Fixture 'ext4-recover'
    $r = Run "if=ext4-recover.img of=rec1.out bs=1M --sparse-read"
    Check 'a journal that needs recovery: read whole by default' ($r.Code -eq 0 -and $r.Err -match 'reading all 16 MiB: it was not cleanly unmounted: its journal needs recovery \(--allow-unclean' -and (Hash 'rec1.out') -eq (Hash 'ext4-recover.img')) $r.Err
    $r = Run "if=ext4-recover.img of=rec2.out bs=1M --sparse-read --allow-unclean"
    $b = Blocks 'ext4-recover.img' 'rec2.out' $info
    Check '--allow-unclean: sparse, with a warning' ($r.Code -eq 0 -and $r.Err -match 'dd: the input \(ext4\): reading the .* in use of 16 MiB; its journal needs recovery') $r.Err
    Check '--allow-unclean: the block only the journal marks in use is kept' ($b.Bad -eq 0 -and $b.Kept -eq 1 -and $b.FirstKept -eq [long]$info.journal_block -and $b.Zeroed -gt (FreeBlocks $info) * 0.9) ($b | Out-String)
    $b = [IO.File]::ReadAllBytes((F 'ext4.img'))
    $b[1024 + 58] = 0     # s_state: not valid (mounted, or crashed)
    [IO.File]::WriteAllBytes((F 'state.img'), $b)
    $r = Run "if=state.img of=state.out bs=1M --sparse-read"
    Check 'not marked clean: read whole by default' ($r.Code -eq 0 -and $r.Err -match 'not cleanly unmounted' -and (Hash 'state.out') -eq (Hash 'state.img')) $r.Err
    $r = Run "if=state.img of=state2.out bs=1M --sparse-read --allow-unclean"
    Check 'not marked clean, --allow-unclean: sparse, with a caution' ($r.Code -eq 0 -and $r.Err -match 'warning|blocks written since may not be in its bitmaps' -and (Hash 'state2.out') -ne (Hash 'state.img')) $r.Err

    # --- a partitioned image ------------------------------------------------------------
    # MBR: 1 MiB gap, partition 1 = ext4 (16 MiB), partition 2 = 2 MiB of
    # random data typed FAT32, then 1 MiB outside any partition.
    $img = New-Object byte[] (20MB)
    $rnd = New-Object byte[] (3MB); (New-Object Random 5).NextBytes($rnd)
    [Array]::Copy($rnd, 0, $img, 0, 1MB - 512)
    [Array]::Copy($rnd, 0, $img, 512, 1MB - 512)
    [Array]::Copy([IO.File]::ReadAllBytes((F 'ext4.img')), 0, $img, 1MB, 16MB)
    [Array]::Copy($rnd, 0, $img, 17MB, 3MB)
    function Entry([int]$i, [byte]$type, [long]$start, [long]$size) {
        $o = 446 + 16 * $i
        $img[$o] = 0; $img[$o + 4] = $type
        [BitConverter]::GetBytes([uint32]($start / 512)).CopyTo($img, $o + 8)
        [BitConverter]::GetBytes([uint32]($size / 512)).CopyTo($img, $o + 12)
    }
    for ($i = 446; $i -lt 510; $i++) { $img[$i] = 0 }
    Entry 0 0x83 1MB 16MB
    Entry 1 0x0C 17MB 2MB
    $img[510] = 0x55; $img[511] = 0xAA
    [IO.File]::WriteAllBytes((F 'mbr.img'), $img)
    $r = Run "if=mbr.img of=mbr.out bs=1M --sparse-read"
    Check 'MBR: the ext4 partition is sparse, the unknown one read whole' ($r.Code -eq 0 -and $r.Err -match 'partition 1 \(ext4\): reading the' -and
        $r.Err -match 'partition 2: no FAT, exFAT, NTFS, ext2/3/4 or swap found; reading all 2\.0 MiB') $r.Err
    $b = Blocks 'mbr.img' 'mbr.out' (Fixture 'ext4') 1MB
    Check 'MBR: the table, gaps and other partition are identical; ext4''s free space zeroed' ($b.Bad -eq 0 -and $b.Outside -eq 0 -and $b.Kept -eq 0 -and $b.Zeroed -gt 0) ($b | Out-String)
    $r = Run "--layout --sparse-read if=mbr.img"
    Check '--layout --sparse-read lists the file systems' ($r.Code -eq 0 -and $r.Out -match "File systems \(--sparse-read\)" -and
        $r.Out -match '(?m)^\s+1\s+1\.0 MiB.*ext4\s+[\d.]+ [KM]iB' -and $r.Out -match '(?m)^\s+2\s+.*\s-\s+all' -and $r.Out -match 'reads .* of 20 MiB') $r.Out
    $r2 = Run "--layout if=mbr.img"
    Check '--layout alone does not' ($r2.Code -eq 0 -and $r2.Out -notmatch 'File systems') $r2.Out
    $r2 = Run "--layout --sparse-read if=ext4-recover.img"
    Check '--layout shows an unclean file system' ($r2.Code -eq 0 -and $r2.Out -match 'ext4\s+all\s+unclean') $r2.Out

    Entry 1 0x0C (16MB) 2MB         # now partition 2 overlaps the end of partition 1
    [IO.File]::WriteAllBytes((F 'overlap.img'), $img)
    $r = Run "if=overlap.img of=overlap.out bs=1M --sparse-read"
    Check 'overlapping partitions are read whole' ($r.Code -eq 0 -and $r.Err -match 'partition 1 \(ext4\): reading all 16 MiB: it overlaps partition 2' -and (Hash 'overlap.out') -eq (Hash 'overlap.img')) $r.Err
    Entry 1 0x0C 17MB 8MB           # partition 2 runs past the end of the image
    [IO.File]::WriteAllBytes((F 'past.img'), $img)
    $r = Run "if=past.img of=past.out bs=1M --sparse-read"
    Check 'a partition past the end of the image is cut at the end' ($r.Code -eq 0 -and $r.Err -match 'partition 2: .*reading all 3\.0 MiB; it runs past the end of the input') $r.Err
    $r = Run "if=mbr.img of=mbr-fit.out bs=1M --sparse-read --fit status=none"
    $r2 = Run "if=mbr.out of=mbr-fit-ref.out bs=1M count=19 status=none"
    Check 'with --fit: only up to the last partition, sparse' ($r.Code -eq 0 -and (Hash 'mbr-fit.out') -eq (Hash 'mbr-fit-ref.out')) $r.Err

    # --- NTFS (made by make-ntfs-fixtures.ps1; the free clusters are Windows' own answer) ---
    # the fixtures hold a placeholder for the SID of the machine that made them
    . (Join-Path $PSScriptRoot 'machine-sid.ps1')
    $machineSid = Get-MachineSid
    foreach ($name in 'ntfs', 'ntfs-dirty', 'ntfs-inuse') {
        $r = Run "if=`"$fixtures\$name.img.xz`" of=$name.img bs=1M status=none"
        if ($r.Code) { throw "cannot decompress $name.img.xz: $($r.Err)" }
        $null = Convert-Sid (F "$name.img") $PlaceholderSid $machineSid
    }
    $info = Fixture 'ntfs'
    $r = Run "if=ntfs.img of=ntfs.out bs=1M --sparse-read"
    $b = Blocks 'ntfs.img' 'ntfs.out' $info ([long]$info.offset)
    Check 'NTFS, cleanly unmounted: only its used space' ($r.Code -eq 0 -and $r.Err -match 'partition 1 \(NTFS\): reading the [\d.]+ [KM]iB in use of 14 MiB') $r.Err
    Check 'NTFS: used clusters identical, every free one (per Windows) zeroed' ($b.Bad -eq 0 -and $b.Outside -eq 0 -and $b.Kept -eq 0 -and
        $b.Zeroed -gt (FreeBlocks $info) * 0.9) ($b | Out-String)
    $r = Run "if=ntfs-dirty.img of=ntfs-dirty.out bs=1M --sparse-read --allow-unclean"
    Check 'NTFS marked dirty: read whole, even with --allow-unclean, saying why' ($r.Code -eq 0 -and
        $r.Err -match 'partition 1 \(NTFS\): reading all 14 MiB: it was not cleanly unmounted, says \$Volume; dd cannot replay an NTFS log' -and
        $r.Err -notmatch '--allow-unclean reads' -and (Hash 'ntfs-dirty.out') -eq (Hash 'ntfs-dirty.img')) $r.Err
    $r = Run "if=ntfs-inuse.img of=ntfs-inuse.out bs=1M --sparse-read"
    Check 'NTFS read while mounted (its log in use): read whole' ($r.Code -eq 0 -and
        $r.Err -match 'partition 1 \(NTFS\): reading all 14 MiB: its log needs replaying' -and (Hash 'ntfs-inuse.out') -eq (Hash 'ntfs-inuse.img')) $r.Err
    $r = Run "--layout --sparse-read if=ntfs-dirty.img"
    Check '--layout marks the dirty NTFS volume' ($r.Code -eq 0 -and $r.Out -match 'NTFS\s+all\s+unclean') $r.Out

    # --- Linux swap: only its header page -------------------------------------------------
    # 8 MiB: a 4 KiB header page (version 1, last page 2047, SWAPSPACE2), then 0xA5
    $sw = New-Object byte[] (8MB)
    $sw[4096] = 0xA5
    for ($n = 1; 4096 + $n -lt $sw.Length; $n *= 2) { [Array]::Copy($sw, 4096, $sw, 4096 + $n, [Math]::Min($n, $sw.Length - 4096 - $n)) }
    [BitConverter]::GetBytes([uint32]1).CopyTo($sw, 1024)
    [BitConverter]::GetBytes([uint32]2047).CopyTo($sw, 1028)
    [Text.Encoding]::ASCII.GetBytes('SWAPSPACE2').CopyTo($sw, 4086)
    [IO.File]::WriteAllBytes((F 'swap.img'), $sw)
    $r = Run "if=swap.img of=swap.out bs=1M --sparse-read"
    $o = [IO.File]::ReadAllBytes((F 'swap.out'))
    $head = [SparseCheck]::Same((F 'swap.out'), (F 'swap.img'), 4096)
    $rest = $o.Length -gt 4096 -and [Linq.Enumerable]::SequenceEqual([Linq.Enumerable]::Skip($o, 4096), (New-Object byte[] ($o.Length - 4096)))
    Check 'Linux swap: the header page is read, the rest written as zeros' ($r.Code -eq 0 -and $r.Err -match 'the input \(swap\): reading the 4\.0 KiB in use of 8\.0 MiB' -and
        $head -and $rest -and $o.Length -eq $sw.Length) $r.Err
    [Text.Encoding]::ASCII.GetBytes('S1SUSPEND').CopyTo($sw, 4086)
    [IO.File]::WriteAllBytes((F 'hiber.img'), $sw)
    $r = Run "if=hiber.img of=hiber.out bs=1M --sparse-read"
    Check 'swap holding a hibernation image: read whole' ($r.Code -eq 0 -and $r.Err -match 'the input \(swap\): reading all 8\.0 MiB: it holds a hibernation image' -and
        (Hash 'hiber.out') -eq (Hash 'hiber.img')) $r.Err

    # --- encryption written over an old file system ---------------------------------------
    # LUKS1 writes 592 bytes at the start and key slots from 4096: an old
    # superblock in between survives, but describes nothing now
    $luks = [byte[]](0x4C, 0x55, 0x4B, 0x53, 0xBA, 0xBE, 0x00, 0x01)
    foreach ($c in @(@('swap.img', 'luks-swap.img'), @('ext4-1k.img', 'luks-ext.img'))) {
        $b = [IO.File]::ReadAllBytes((F $c[0]))
        [Array]::Copy($luks, 0, $b, 0, $luks.Length)
        [IO.File]::WriteAllBytes((F $c[1]), $b)
        $r = Run "if=$($c[1]) of=$($c[1]).out bs=1M --sparse-read"
        Check "a LUKS header over old $($c[0] -replace '\.img$'): read whole" ($r.Code -eq 0 -and $r.Err -match 'it holds LUKS, not a file system dd can map' -and
            (Hash "$($c[1]).out") -eq (Hash $c[1])) $r.Err
    }
    $b = [IO.File]::ReadAllBytes((F 'swap.img'))
    $b[300] = 0x42
    [IO.File]::WriteAllBytes((F 'swap-used.img'), $b)
    $r = Run "if=swap-used.img of=swap-used.out bs=1M --sparse-read"
    Check 'swap whose first 1 KiB is not blank: read whole' ($r.Code -eq 0 -and $r.Err -match 'its first 1 KiB is not blank') $r.Err

    # --- an uninitialised ext group whose descriptor fails its checksum ------------------
    # the kernel refuses it, and e2fsck reads the bitmap after all
    $b = [IO.File]::ReadAllBytes((F 'ext4-1k.img'))
    $ds = if ([BitConverter]::ToUInt32($b, 1024 + 96) -band 0x80) { [BitConverter]::ToUInt16($b, 1024 + 254) } else { 32 }
    $g = 1
    while ($g -lt 8 -and -not ([BitConverter]::ToUInt16($b, 2048 + $g * $ds + 0x12) -band 2)) { $g++ }
    $b[2048 + $g * $ds + 0x14] = $b[2048 + $g * $ds + 0x14] -bxor 0xFF      # a field dd does not use
    [IO.File]::WriteAllBytes((F 'bad-gd.img'), $b)
    $r = Run "if=bad-gd.img of=bad-gd.out bs=1M --sparse-read"
    Check 'ext group descriptor failing its checksum: read whole' ($g -lt 8 -and $r.Code -eq 0 -and
        $r.Err -match 'a group descriptor fails its checksum' -and (Hash 'bad-gd.out') -eq (Hash 'bad-gd.img')) "group $g`n$($r.Err)"

    # --- an MBR counting 4096-byte sectors --------------------------------------------------
    $b = New-Object byte[] (6MB)
    $b[450] = 0x0C
    [BitConverter]::GetBytes([uint32]256).CopyTo($b, 454)        # 1 MiB in 4 KiB sectors
    [BitConverter]::GetBytes([uint32]1024).CopyTo($b, 458)       # 4 MiB
    $b[510] = 0x55; $b[511] = 0xAA
    $b[1MB + 510] = 0x55; $b[1MB + 511] = 0xAA                   # the partition's boot sector
    [IO.File]::WriteAllBytes((F 'mbr4k.img'), $b)
    $r = Run "--layout if=mbr4k.img"
    Check 'an MBR in 4096-byte sectors is read as one' ($r.Code -eq 0 -and "$($r.Out)$($r.Err)" -match 'counts 4096-byte sectors' -and
        $r.Out -match 'data ends at 5\.0 MiB') "$($r.Out)$($r.Err)"

    # --- a read error with iflag=fullblock: the next block starts where it should ----------
    $fs = [IO.File]::Open((F 'ext4.img'), 'Open', 'ReadWrite', 'ReadWrite')
    try {
        $fs.Lock(4198400, 4096)         # in use, so --sparse-read reads it
        $r1 = Run "if=ext4.img of=fb.out bs=8M iflag=fullblock conv=noerror,sync --sparse-read --no-hashes"
        $r2 = Run "if=ext4.img of=nb.out bs=8M conv=noerror,sync --sparse-read --no-hashes"
    } finally {
        $fs.Close()
    }
    Check 'a read error with iflag=fullblock loses nothing after it' ($r1.Code -eq 1 -and $r2.Code -eq 1 -and (Hash 'fb.out') -eq (Hash 'nb.out')) "$($r1.Err)`n$($r2.Err)"

    # --- compressed images: the sparse map, then reading in order ---------------------------
    # (images of 16 MiB at most: compression in tests stays small)
    foreach ($c in @(@('ext4-1k', 1), @('ext4', 9), @('mbr-ext4', 1))) {
        $name = $c[0]; $level = $c[1]
        $r = Run "if=`"$fixtures\$name.img.xz`" of=$name.img bs=1M status=none"
        $r = Run "if=$name.img of=$name.out bs=1M --sparse-read status=none"
        $r = Run "if=$name.img of=z-$name.img.xz bs=1M --xz=$level status=none"
        $r = Run "if=z-$name.img.xz of=x.out --sparse-read"
        Check "${name}.xz without a sparse map: refused, saying how to make one" ($r.Code -eq 1 -and $r.Err -match 'needs its file systems mapped first.*--make-sparse-map' -and -not (Test-Path (F 'x.out'))) $r.Err
        $r = Run "if=z-$name.img.xz --make-sparse-map"
        $map = if (Test-Path (F "z-$name.img.sparse")) { [IO.File]::ReadAllText((F "z-$name.img.sparse")) } else { '' }
        Check "${name}.xz (level $level): --make-sparse-map writes z-$name.img.sparse" ($r.Code -eq 0 -and $r.Err -match 'sparse map written to' -and
            $map -match '^dd sparse map 2' -and $map -match "(?m)^sha256 $((Get-FileHash (F "z-$name.img.xz")).Hash.ToLower())$" -and $map -match "`ncheck [0-9a-f]{64}`nend`n$") "$($r.Err)`n$map"
        $r = Run "if=z-$name.img.xz of=z-$name-out.img.xz bs=1M --sparse-read --xz=1"
        Check "${name}.xz to .xz, sparse: read in order, the map's hash checked" ($r.Code -eq 0 -and $r.Err -match 'read in order; its free space, from the sparse map' -and
            $r.Err -match "SHA-256 of z-$name\.img\.xz matches z-$name\.img\.sparse") $r.Err
        $r2 = Run "if=z-$name-out.img.xz of=z-$name-out.img bs=1M status=none"
        Check "... gives exactly the sparse read of the uncompressed image" ((Hash "z-$name-out.img") -eq (Hash "$name.out"))
    }
    $info = Fixture 'mbr-ext4'
    $b = Blocks 'mbr-ext4.img' 'z-mbr-ext4-out.img' $info ([long]$info.offset)
    Check 'the compressed MBR image: table, gap and tail identical; the partition''s free space zeroed' ($b.Bad -eq 0 -and $b.Outside -eq 0 -and $b.Kept -eq 0 -and $b.Zeroed -gt (FreeBlocks $info) * 0.9) ($b | Out-String)
    $r = Run "--layout --sparse-read if=z-mbr-ext4.img.xz"
    Check '--layout --sparse-read answers a compressed image from its map' ($r.Code -eq 0 -and $r.Out -match '(?m)^\s+1\s+1\.0 MiB.*ext4') $r.Out
    $r = Run "if=z-mbr-ext4.img.xz of=z-fit.img bs=1M --sparse-read --fit status=none"
    Check 'with --fit, from the compressed image' ($r.Code -eq 0 -and [SparseCheck]::Same((F 'z-fit.img'), (F 'mbr-ext4.out'), 15MB)) $r.Err
    Copy-Item (F 'z-ext4-1k.img.sparse') (F 'z-ext4.img.sparse') -Force
    $r = Run "if=z-ext4.img.xz of=x.out --sparse-read"
    Check 'a sparse map of another image is refused (by size)' ($r.Code -eq 1 -and $r.Err -match 'is for another image') $r.Err
    $r = Run "if=z-ext4.img.xz --make-sparse-map status=none"
    Tamper 'z-ext4.img.sparse'
    $r = Run "if=z-ext4.img.xz of=x.out bs=1M --sparse-read"
    Check 'a sparse map whose hash is not the image''s fails the run' ($r.Code -eq 1 -and $r.Err -match 'does not match z-ext4\.img\.sparse') $r.Err
    $r = Run "if=z-ext4.img.xz --make-sparse-map status=none"
    [IO.File]::WriteAllText((F 'z-ext4.img.sparse'), ([IO.File]::ReadAllText((F 'z-ext4.img.sparse')) -replace "end`n$", ''))
    $r = Run "if=z-ext4.img.xz of=x.out --sparse-read"
    Check 'a cut-short sparse map is refused' ($r.Code -eq 1 -and $r.Err -match 'it is damaged; make it again') $r.Err
    # free space that is not the volume's: edited, then edited and resealed
    $r = Run "if=z-ext4.img.xz --make-sparse-map status=none"
    $good = [IO.File]::ReadAllText((F 'z-ext4.img.sparse'))
    $edited = [regex]::Replace($good, '(?m)^free \d+ \d+$', 'free 0 4194304', 1)
    [IO.File]::WriteAllText((F 'z-ext4.img.sparse'), $edited)
    $r = Run "if=z-ext4.img.xz of=x.out --sparse-read"
    Check 'an edited sparse map is refused (its check line)' ($r.Code -eq 1 -and $r.Err -match 'it is damaged; make it again') $r.Err
    [IO.File]::WriteAllText((F 'z-ext4.img.sparse'), (Reseal $edited))
    $r = Run "if=z-ext4.img.xz of=x.out --sparse-read"
    Check '... and resealed: refused, its free space is not what the volume says' ($r.Code -eq 1 -and $r.Err -match 'it is damaged; make it again') $r.Err
    # with a hash file, the two must agree, and the input's hashes are checked too (all of it is read)
    $r = Run "if=ext4.img of=h.img.xz bs=1M --xz=1 --hashes status=none"
    $r = Run "if=h.img.xz --make-sparse-map status=none"
    $r = Run "if=h.img.xz of=h.out bs=1M --sparse-read"
    Check 'with a hash file too: both of its hashes are checked, read in order' ($r.Code -eq 0 -and $r.Err -match 'SHA-256 of h\.img\.xz matches h\.img\.hashes' -and
        $r.Err -match 'SHA-256 of h\.img \(uncompressed\) matches') $r.Err
    Tamper 'h.img.sparse'
    $r = Run "if=h.img.xz of=disagree.out bs=1M --sparse-read"
    Check 'a sparse map and hash file that disagree: refused before reading' ($r.Code -eq 1 -and $r.Err -match 'disagree about' -and -not (Test-Path (F 'disagree.out'))) $r.Err
    # not cleanly unmounted: the map remembers --allow-unclean
    $r = Run "if=`"$fixtures\ext4-recover.img.xz`" of=rec.img.xz bs=1M --xz=1 status=none"
    $r = Run "if=rec.img.xz --make-sparse-map --allow-unclean"
    Check '--make-sparse-map --allow-unclean maps the journal' ($r.Code -eq 0 -and $r.Out -match 'unclean' -and (Get-Content (F 'rec.img.sparse') -Raw) -match '(?m)^unclean 1$') "$($r.Err)`n$($r.Out)"
    $r = Run "if=rec.img.xz of=x.out --sparse-read"
    Check '... and is used only with --allow-unclean' ($r.Code -eq 1 -and $r.Err -match 'was made with --allow-unclean') $r.Err
    $r = Run "if=rec.img.xz of=rec-xz.out bs=1M --sparse-read --allow-unclean status=none"
    Check '... then it gives what reading the uncompressed image does' ($r.Code -eq 0 -and (Hash 'rec-xz.out') -eq (Hash 'rec2.out')) $r.Err
    $r = Run "if=ext4.img --make-sparse-map"
    Check '--make-sparse-map of an uncompressed image: not needed, says why' ($r.Code -eq 1 -and $r.Err -match 'not xz-compressed: --sparse-read maps an uncompressed image as it reads it') $r.Err

    # --- refused ----------------------------------------------------------------------
    $r = Run "if=ext4.img of=x.out --allow-unclean"
    Check '--allow-unclean needs --sparse-read' ($r.Code -eq 1 -and $r.Err -match 'only applies to --sparse-read') $r.Err
    $r = Run "--layout --sparse-read if=`"$fixtures\ext4.img.xz`""
    Check '--layout --sparse-read of a compressed image without a map says to make one' ($r.Code -eq 1 -and $r.Err -match 'has no sparse map') $r.Err
    $r = Run "if=/dev/zero of=x.out count=1 --sparse-read"
    Check 'a /dev name is refused' ($r.Code -eq 1 -and $r.Err -match 'needs an image file or a device') $r.Err
    $r = Run "if=ext4.img --verify-hashes --sparse-read"
    Check 'not with --verify-hashes' ($r.Code -eq 1 -and $r.Err -match '--verify-hashes only reads') $r.Err
    $r = Run "--help"
    Check '--help describes --sparse-read and --allow-unclean' ($r.Out -match '--sparse-read' -and $r.Out -match '--allow-unclean') ''
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "$($script:count - $script:failures) of $script:count passed"
exit [int]($script:failures -ne 0)
