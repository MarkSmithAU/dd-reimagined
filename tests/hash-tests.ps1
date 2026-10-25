# SHA-256 hash files, --verify and --verify-hashes.  Files only; no admin
# needed.  The device side (images of disks, verifying disks) is in
# device-tests.ps1 and fit-tests.ps1.
#   powershell -ExecutionPolicy Bypass -File tests\hash-tests.ps1 [-DD build\dd.exe]
param(
    [string]$DD = (Join-Path $PSScriptRoot '..\build\dd.exe')
)

$DD = (Resolve-Path $DD).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("ddhash-" + [Guid]::NewGuid())
New-Item -ItemType Directory $work | Out-Null
$script:failures = 0
$script:count = 0

function F([string]$name) { Join-Path $work $name }
function Run([string]$cmdline) {
    $err = F 'stderr.txt'
    cmd /c "cd /d `"$work`" && $cmdline 2>`"$err`""
    [pscustomobject]@{ Code = $LASTEXITCODE; Err = ([IO.File]::ReadAllText($err)).Replace("`r", '') }
}
function DDRun([string]$operands) { Run "`"$DD`" $operands" }
function Check([string]$what, [bool]$ok, [string]$detail = '') {
    $script:count++
    if ($ok) { Write-Host "ok   $what" }
    else {
        $script:failures++
        Write-Host "FAIL $what" -ForegroundColor Red
        if ($detail) { Write-Host "     $detail" }
    }
}
function Hash([string]$name) { (Get-FileHash (F $name) -Algorithm SHA256).Hash.ToLower() }
# name -> hash, from a .hashes file
function Hashes([string]$name) {
    $h = @{}
    if (Test-Path (F $name)) {
        foreach ($l in [IO.File]::ReadAllLines((F $name))) {
            if ($l -match '^([0-9a-f]{64})  (.+)$') { $h[$Matches[2]] = $Matches[1] }
        }
    }
    $h
}
function Lines([string]$name) { @([IO.File]::ReadAllLines((F $name))) }
# replaces the hash recorded for one name with another value
function Spoil([string]$hashes, [string]$name) {
    $t = [IO.File]::ReadAllText((F $hashes))
    $t = [regex]::Replace($t, "(?m)^[0-9a-f]{64}(  $([regex]::Escape($name)))$", ('0' * 64) + '$1')
    [IO.File]::WriteAllText((F $hashes), $t)
}

try {
    $data = New-Object byte[] (3MB + 4321)
    (New-Object Random 21).NextBytes($data)
    for ($i = 0; $i -lt 1MB; $i++) { $data[$i] = [byte](65 + $i % 23) }     # some of it compresses
    [IO.File]::WriteAllBytes((F 'src.bin'), $data)
    $h = Hash 'src.bin'

    # --- writing hash files ----------------------------------------------------
    $r = DDRun "if=src.bin of=plain.img bs=1M --hashes"
    $hs = Hashes 'plain.img.hashes'
    $l = @(Lines 'plain.img.hashes')
    Check '--hashes writes image.hashes in sha256sum format' ($r.Code -eq 0 -and $l.Count -eq 1 -and
        $l[0] -match '^[0-9a-f]{64}  plain\.img$') ($l -join '|')
    Check 'the recorded hash is the image''s SHA-256' ($hs['plain.img'] -eq $h -and (Hash 'plain.img') -eq $h) "$($hs['plain.img']) vs $h"
    Check 'the summary names the hash file' ($r.Err -match 'SHA-256 hashes written to plain\.img\.hashes') $r.Err

    $r = DDRun "if=src.bin of=packed.img.xz bs=1M --hashes"
    $hs = Hashes 'packed.img.hashes'
    Check 'compressed: one file, packed.img.hashes, without .xz in its name' ($r.Code -eq 0 -and (Test-Path (F 'packed.img.hashes')) -and -not (Test-Path (F 'packed.img.xz.hashes'))) $r.Err
    Check 'compressed: the uncompressed image''s hash is recorded' ($hs['packed.img'] -eq $h) "$($hs['packed.img']) vs $h"
    Check 'compressed: the .xz file''s hash is recorded' ($hs['packed.img.xz'] -eq (Hash 'packed.img.xz')) "$($hs['packed.img.xz'])"
    Check 'compressed: uncompressed first, then the .xz' ((Lines 'packed.img.hashes')[0] -match '  packed\.img$' -and (Lines 'packed.img.hashes')[1] -match '  packed\.img\.xz$')

    $r = DDRun "if=src.bin of=auto.img bs=1M"
    Check 'a file copied from a file gets no hash file by default' ($r.Code -eq 0 -and -not (Test-Path (F 'auto.img.hashes'))) $r.Err
    $r = DDRun "if=src.bin of=none.img bs=1M --hashes --no-hashes"
    Check '--no-hashes wins' ($r.Code -eq 0 -and -not (Test-Path (F 'none.img.hashes'))) $r.Err

    [IO.File]::WriteAllText((F 'stale.img.hashes'), ('1' * 64) + "  stale.img`n")
    $r = DDRun "if=src.bin of=stale.img bs=1M --hashes status=none"
    Check 'an old hash file is replaced' ((Hashes 'stale.img.hashes')['stale.img'] -eq $h) ((Lines 'stale.img.hashes') -join '|')

    $r = DDRun "if=src.bin of=seek.img seek=1 --hashes"
    Check '--hashes with seek is refused' ($r.Code -eq 1 -and $r.Err -match 'whole new image') $r.Err
    $r = Run "`"$DD`" if=src.bin --hashes > piped.img"
    Check '--hashes to standard output is refused' ($r.Code -eq 1 -and $r.Err -match 'needs a new image file') $r.Err

    # --- checking an image as it is read -------------------------------------------
    $r = DDRun "if=packed.img.xz of=unpacked.img bs=1M"
    Check 'decompressing checks both hashes' ($r.Code -eq 0 -and $r.Err -match 'checking ''packed\.img\.xz'' against packed\.img\.hashes' -and
        $r.Err -match 'SHA-256 of packed\.img\.xz matches' -and $r.Err -match 'SHA-256 of packed\.img \(uncompressed\) matches') $r.Err
    $r = DDRun "if=plain.img of=copy.img bs=64K"
    Check 'an uncompressed image is checked' ($r.Code -eq 0 -and $r.Err -match 'SHA-256 of plain\.img matches') $r.Err
    $r = DDRun "if=plain.img of=part.img bs=64K count=3"
    Check 'with count the image is not checked' ($r.Code -eq 0 -and $r.Err -notmatch 'checking') $r.Err
    $r = DDRun "if=plain.img of=part.img bs=64K skip=3"
    Check 'with skip the image is not checked' ($r.Code -eq 0 -and $r.Err -notmatch 'checking') $r.Err

    Copy-Item (F 'packed.img.xz') (F 'bad1.img.xz'); Copy-Item (F 'packed.img.hashes') (F 'bad1.img.hashes')
    (Get-Content (F 'bad1.img.hashes') -Raw).Replace('packed.img', 'bad1.img') | Set-Content -NoNewline (F 'bad1.img.hashes')
    Spoil 'bad1.img.hashes' 'bad1.img'
    $r = DDRun "if=bad1.img.xz of=out1.img bs=1M"
    Check 'a wrong uncompressed hash fails the copy' ($r.Code -eq 1 -and $r.Err -match 'bad1\.img \(uncompressed\) is [0-9a-f]{64}, which does not match' -and
        $r.Err -match 'bad1\.img\.xz matches' -and $r.Err -match 'written from an image that failed its hash check') $r.Err
    $r = DDRun "if=bad1.img.xz of=out1.img bs=1M --no-hashes"
    Check '--no-hashes skips the check' ($r.Code -eq 0 -and $r.Err -notmatch 'SHA-256') $r.Err

    Copy-Item (F 'packed.img.xz') (F 'bad2.img.xz'); Copy-Item (F 'packed.img.hashes') (F 'bad2.img.hashes')
    (Get-Content (F 'bad2.img.hashes') -Raw).Replace('packed.img', 'bad2.img') | Set-Content -NoNewline (F 'bad2.img.hashes')
    Spoil 'bad2.img.hashes' 'bad2.img.xz'
    $r = DDRun "if=bad2.img.xz of=out2.img bs=1M"
    Check 'a wrong .xz hash fails the copy' ($r.Code -eq 1 -and $r.Err -match 'bad2\.img\.xz is [0-9a-f]{64}, which does not match' -and $r.Err -match 'bad2\.img \(uncompressed\) matches') $r.Err

    [IO.File]::WriteAllText((F 'junk.img.hashes'), "not a hash file`n")
    Copy-Item (F 'src.bin') (F 'junk.img')
    $r = DDRun "if=junk.img of=out3.img bs=1M"
    Check 'an unreadable hash file is a warning, not a failure' ($r.Code -eq 0 -and $r.Err -match 'holds no SHA-256 hashes') $r.Err

    # --- --verify-hashes -----------------------------------------------------------------
    $r = DDRun "if=packed.img.xz --verify-hashes"
    Check '--verify-hashes passes a good .xz image' ($r.Code -eq 0 -and $r.Err -match 'packed\.img\.xz matches' -and $r.Err -match 'packed\.img \(uncompressed\) matches' -and $r.Err -notmatch 'records') $r.Err
    $r = DDRun "if=plain.img --verify-hashes"
    Check '--verify-hashes passes a good plain image' ($r.Code -eq 0 -and $r.Err -match 'plain\.img matches') $r.Err
    $r = DDRun "if=unpacked.img --verify-hashes"
    Check 'the decompressed image has no hash file of its own' ($r.Code -eq 1 -and $r.Err -match 'has no hash file') $r.Err
    Copy-Item (F 'packed.img.hashes') (F 'unpacked.img.hashes')
    (Get-Content (F 'unpacked.img.hashes') -Raw).Replace('packed.img', 'unpacked.img') | Set-Content -NoNewline (F 'unpacked.img.hashes')
    $r = DDRun "if=unpacked.img --verify-hashes"
    Check 'a decompressed image checks against the shared hash file' ($r.Code -eq 0 -and $r.Err -match 'unpacked\.img matches') $r.Err
    $r = DDRun "if=bad1.img.xz --verify-hashes"
    Check '--verify-hashes fails a wrong hash' ($r.Code -eq 1 -and $r.Err -match 'does not match') $r.Err

    $b = [IO.File]::ReadAllBytes((F 'plain.img')); $b[2MB] = $b[2MB] -bxor 1
    [IO.File]::WriteAllBytes((F 'flip.img'), $b)
    [IO.File]::WriteAllText((F 'flip.img.hashes'), "$h  flip.img`n")
    $r = DDRun "if=flip.img --verify-hashes"
    Check '--verify-hashes finds a flipped bit' ($r.Code -eq 1 -and $r.Err -match 'flip\.img is [0-9a-f]{64}, which does not match') $r.Err
    [IO.File]::WriteAllText((F 'other.img.hashes'), "$h  something-else.img`n")
    Copy-Item (F 'src.bin') (F 'other.img')
    $r = DDRun "if=other.img --verify-hashes"
    Check '--verify-hashes needs a hash for this image' ($r.Code -eq 1 -and $r.Err -match "has no hash for 'other\.img'") $r.Err
    $r = DDRun "if=plain.img of=x.img --verify-hashes"
    Check '--verify-hashes with of= is refused' ($r.Code -eq 1 -and $r.Err -match 'only reads if=') $r.Err
    $r = DDRun "--verify-hashes"
    Check '--verify-hashes needs if=' ($r.Code -eq 1 -and $r.Err -match 'needs an image file') $r.Err

    # --- --verify ------------------------------------------------------------------------
    $r = DDRun "if=src.bin of=v.img bs=1M --verify"
    Check '--verify reads a file back' ($r.Code -eq 0 -and $r.Err -match "verified: 'v\.img' reads back exactly as written \($($data.Length) bytes, SHA-256 $h\)") $r.Err
    $r = DDRun "if=src.bin of=v.img.xz bs=1M --verify"
    Check '--verify decompresses an .xz output again' ($r.Code -eq 0 -and $r.Err -match "verified: 'v\.img\.xz'" -and $r.Err -match "SHA-256 $h") $r.Err
    Copy-Item (F 'src.bin') (F 'v2.img')
    $r = DDRun "if=src.bin of=v2.img bs=1000 seek=7 count=50 conv=notrunc --verify"
    Check '--verify checks only the part written' ($r.Code -eq 0 -and $r.Err -match "verified: 'v2\.img' reads back exactly as written \(50000 bytes") $r.Err
    $r = DDRun "if=src.bin of=v3.img bs=1M --verify --hashes"
    Check '--verify with --hashes' ($r.Code -eq 0 -and $r.Err -match 'verified' -and (Hashes 'v3.img.hashes')['v3.img'] -eq $h) $r.Err
    $r = Run "`"$DD`" if=src.bin --verify > p.img"
    Check '--verify to standard output is refused' ($r.Code -eq 1 -and $r.Err -match 'needs a file or a disk') $r.Err
    $r = DDRun "if=src.bin of=/dev/null --verify"
    Check '--verify to /dev/null is refused' ($r.Code -eq 1 -and $r.Err -match 'needs a file or a disk') $r.Err
    $r = DDRun "if=src.bin of=v4.img conv=sparse,notrunc --verify"
    Check '--verify with sparse and notrunc is refused' ($r.Code -eq 1 -and $r.Err -match 'conv=sparse') $r.Err
    $r = DDRun "if=src.bin of=v5.img bs=64K conv=sparse --verify"
    Check '--verify with sparse to a new file' ($r.Code -eq 0 -and $r.Err -match 'verified') $r.Err

    # --- disk.img and disk.img.xz share disk.img.hashes ---------------------------
    [void][IO.Directory]::CreateDirectory((F 'sh'))
    $r = DDRun "if=src.bin of=sh\disk.img.xz bs=1M --xz=0 --hashes"
    $before = [IO.File]::ReadAllText((F 'sh\disk.img.hashes'))
    $r = DDRun "if=sh\disk.img.xz of=sh\disk.img bs=1M count=1 --hashes"
    Check 'shared hash file: count= leaves it as it was' ($r.Code -eq 0 -and $r.Err -match 'not writing .*count= changes the data' -and
        [IO.File]::ReadAllText((F 'sh\disk.img.hashes')) -eq $before) $r.Err
    $r = DDRun "if=sh\disk.img.xz of=sh\disk.img bs=1M --hashes"
    $hs = Hashes 'sh\disk.img.hashes'
    Check 'shared hash file: a full copy keeps the .xz file''s entry' ($r.Code -eq 0 -and $hs['disk.img'] -eq $h -and
        $hs['disk.img.xz'] -eq (Hash 'sh\disk.img.xz')) $r.Err
    Spoil 'sh\disk.img.hashes' 'disk.img'
    $spoiled = [IO.File]::ReadAllText((F 'sh\disk.img.hashes'))
    $r = DDRun "if=sh\disk.img.xz of=sh\disk.img bs=1M --hashes"
    Check 'shared hash file: kept when the input fails its check' ($r.Code -eq 1 -and $r.Err -match 'does not match' -and
        [IO.File]::ReadAllText((F 'sh\disk.img.hashes')) -eq $spoiled) $r.Err
    $r = DDRun "if=sh/disk.img of=`"$(F 'sh\disk.img.xz')`" bs=1M --xz=0 --sparse-read --hashes"
    Check 'shared hash file: found under another spelling of its path' ($r.Err -match 'not writing .*--sparse-read changes the data' -and
        [IO.File]::ReadAllText((F 'sh\disk.img.hashes')) -eq $spoiled) $r.Err
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "$($script:count - $script:failures) of $script:count passed"
exit [int]($script:failures -ne 0)
