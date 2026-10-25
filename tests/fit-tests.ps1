# Partition table reading (--layout) and --fit.  Needs an elevated prompt.
# Builds MBR and GPT images with diskpart on VHDXs, then writes them to
# smaller and larger VHDXs.  Writes only to its own VHDXs.
#   powershell -ExecutionPolicy Bypass -File tests\fit-tests.ps1 [-DD build\dd.exe]
param(
    [string]$DD = (Join-Path $PSScriptRoot '..\build\dd.exe')
)

$ErrorActionPreference = 'Stop'
$DD = (Resolve-Path $DD).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("ddfit-" + [Guid]::NewGuid())
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

# After a raw write of a partition table: has Windows rescanned the disk and
# found $style with $parts?  Asked again for up to 10 s, rather than a fixed wait.
function Settle([int]$n, [string]$style, [string[]]$parts) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    do {
        Update-Disk -Number $n -ErrorAction SilentlyContinue
        if ((Get-Disk -Number $n).PartitionStyle -eq $style -and ((Parts $n) -join ',') -eq ($parts -join ',')) { return }
        Start-Sleep -Milliseconds 250
    } while ($sw.ElapsedMilliseconds -lt 10000)
}

# Partitions as Windows sees them: "offset+size" strings
function Parts([int]$n) {
    @(Get-Partition -DiskNumber $n -ErrorAction SilentlyContinue |
      Where-Object { $_.Type -ne 'Extended' } | Sort-Object Offset |
      ForEach-Object { "$($_.Offset)+$($_.Size)" })
}
function PartEnd([int]$n) {
    (Get-Partition -DiskNumber $n | ForEach-Object { $_.Offset + $_.Size } | Measure-Object -Maximum).Maximum
}

function HashRange([string]$file, [long]$from, [long]$to) {
    $fs = [IO.File]::OpenRead($file)
    try {
        $fs.Position = $from
        $buf = New-Object byte[] ($to - $from)
        $got = 0
        while ($got -lt $buf.Length) {
            $r = $fs.Read($buf, $got, $buf.Length - $got)
            if ($r -le 0) { break }
            $got += $r
        }
        $sha = [Security.Cryptography.SHA256]::Create()
        [BitConverter]::ToString($sha.ComputeHash($buf, 0, $got))
    } finally { $fs.Close() }
}

function DataEnd([string]$layoutOut) {
    if ($layoutOut -match 'data ends at [^(]*\((\d+)\)') { [long]$Matches[1] } else { -1 }
}

try {
    # --- an MBR image with an extended partition and two logical ones -------
    $s = New-Vhd 'mbrsrc' 64 @('convert mbr', 'create partition primary size=8',
        'create partition extended size=20', 'create partition logical size=8', 'create partition logical size=8')
    $mbrParts = Parts $s
    $mbrWinEnd = PartEnd $s          # includes the extended container
    $r = Run "if=\\.\PhysicalDrive$s of=mbr.img bs=1M status=none"
    # reading a card: only up to the end of the last partition
    $r = Run "if=\\.\PhysicalDrive$s of=mbr-card.img bs=1M --fit status=none"
    Check 'reading an MBR disk with --fit stops at the last partition' ($r.Code -eq 0 -and (Get-Item (F 'mbr-card.img')).Length -eq $mbrWinEnd) "$((Get-Item (F 'mbr-card.img')).Length) bytes, want $mbrWinEnd`n$($r.Err)"
    Detach 'mbrsrc'

    $r = Run "--layout if=mbr.img"
    Check 'MBR layout is read' ($r.Code -eq 0 -and $r.Out -match '^MBR' -or $r.Out -match "`nMBR") $r.Out
    Check 'MBR shows primary, extended and both logicals' (([regex]::Matches($r.Out, "`n\s+[1256]\s")).Count -eq 4) $r.Out
    $mbrEnd = DataEnd $r.Out
    Check 'MBR data ends at the end of the extended partition' ($mbrEnd -eq $mbrWinEnd) "data end $mbrEnd, Windows says $mbrWinEnd"

    # --- a GPT image ---------------------------------------------------------
    $s = New-Vhd 'gptsrc' 64 @('convert gpt', 'create partition primary size=4',
        'create partition efi size=6', 'create partition primary size=3')     # data ends under 16 MB: compression tests stay small
    $gptParts = Parts $s
    $gptEnd = PartEnd $s
    $r = Run "if=\\.\PhysicalDrive$s of=gpt.img bs=1M status=none"
    # reading a card: the partitions, then a backup GPT made for the image
    $r = Run "if=\\.\PhysicalDrive$s of=gpt-card.img bs=1M --fit"
    $gptCardLen = (Get-Item (F 'gpt-card.img')).Length
    Check 'reading a GPT disk with --fit gives partitions plus a backup GPT' ($r.Code -eq 0 -and $gptCardLen -eq $gptEnd + 33 * 512 -and $r.Err -match 'rebuilt the GPT') "$gptCardLen bytes, want $($gptEnd + 33 * 512)`n$($r.Err)"
    $cardSha = (Get-FileHash (F 'gpt-card.img')).Hash.ToLower()
    $hs = [IO.File]::ReadAllText((F 'gpt-card.img.hashes'))
    Check 'the --fit image''s hash file covers the rebuilt GPT too' ($hs -eq "$cardSha  gpt-card.img`n") $hs
    # the same, compressed: the GPT is patched as it streams past instead
    $r = Run "if=\\.\PhysicalDrive$s of=gpt-card.img.xz bs=1M --fit --verify"
    Check 'reading a GPT disk with --fit into .xz' ($r.Code -eq 0 -and $r.Err -match 'compressing to xz' -and $r.Err -match 'rebuilt the GPT') $r.Err
    Check '... and verified' ($r.Err -match "verified: 'gpt-card\.img\.xz'") $r.Err
    $hs = [IO.File]::ReadAllText((F 'gpt-card.img.hashes'))
    $xzSha = (Get-FileHash (F 'gpt-card.img.xz')).Hash.ToLower()
    Check 'the .xz shares the hash file, with the same image hash' ($hs -eq "$cardSha  gpt-card.img`n$xzSha  gpt-card.img.xz`n") $hs
    $r = Run "if=gpt-card.img.xz of=gpt-card-unxz.img bs=1M status=none"
    Check 'the compressed card image is byte-identical to the uncompressed one' ($r.Code -eq 0 -and (Get-FileHash (F 'gpt-card-unxz.img')).Hash -eq (Get-FileHash (F 'gpt-card.img')).Hash) $r.Err
    Detach 'gptsrc'
    $r = Run "--layout if=gpt-card.img.xz"
    Check '--layout reads the GPT inside an .xz image' ($r.Out -match "`nGPT" -and $r.Out -match 'xz-compressed' -and (DataEnd $r.Out) -eq $gptEnd) $r.Out
    $r = Run "--layout if=gpt-card.img"
    Check 'the trimmed GPT image is valid, backup included' ($r.Code -eq 0 -and $r.Out -match "`nGPT" -and $r.Out -notmatch 'note:' -and
        ([regex]::Matches($r.Out, "`n\s+\d+\s")).Count -eq $gptParts.Count -and (DataEnd $r.Out) -eq $gptEnd) $r.Out

    $r = Run "--layout if=gpt.img"
    Check 'GPT layout is read' ($r.Code -eq 0 -and $r.Out -match "`nGPT, 512-byte sectors") $r.Out
    Check 'GPT shows every partition' (([regex]::Matches($r.Out, "`n\s+\d+\s")).Count -eq $gptParts.Count) "$($gptParts.Count) expected`n$($r.Out)"
    Check 'GPT names the EFI system partition' ($r.Out -match 'EFI system') $r.Out
    $gEnd = DataEnd $r.Out
    Check 'GPT data ends at the end of the last partition' ($gEnd -eq $gptEnd) "data end $gEnd, Windows says $gptEnd"
    Check 'GPT image has a valid backup' ($r.Out -notmatch 'note:') $r.Out

    # --- no partition table ----------------------------------------------------
    $blob = New-Object byte[] 1048576
    (New-Object Random 3).NextBytes($blob)
    [IO.File]::WriteAllBytes((F 'raw.img'), $blob)
    $r = Run "--layout if=raw.img"
    Check 'random data has no partition table' ($r.Out -match 'no partition table' -and (DataEnd $r.Out) -eq 1048576) $r.Out

    # --- --fit: GPT image (64 MB) to a smaller disk (48 MB) -------------------
    $t = New-Vhd 'small' 48
    $r = Run "if=gpt.img of=\\.\PhysicalDrive$t bs=1M --fit"
    Check 'oversize GPT image fits a smaller disk' ($r.Code -eq 0 -and $r.Err -match 'rebuilt the GPT') $r.Err
    Check 'only the data is written' ($r.Err -match "^$gEnd bytes|`n$gEnd bytes") $r.Err
    Settle $t GPT $gptParts
    Check 'Windows reads the GPT on the smaller disk' ((Get-Disk -Number $t).PartitionStyle -eq 'GPT') (Get-Disk -Number $t | Out-String)
    $got = Parts $t
    Check 'Windows sees the same partitions' (($got -join ',') -eq ($gptParts -join ',')) "source: $($gptParts -join ', ')`ntarget: $($got -join ', ')"
    $r2 = Run "--layout if=\\.\PhysicalDrive$t"
    Check 'the rebuilt backup GPT is valid' ($r2.Code -eq 0 -and $r2.Out -match "`nGPT" -and $r2.Out -notmatch 'note:') $r2.Out
    $r = Run "if=\\.\PhysicalDrive$t of=small.bin bs=1M count=$([math]::Ceiling($gEnd / 1MB)) status=none"
    Check 'partition data is identical' ((HashRange (F 'small.bin') 1024 $gEnd) -eq (HashRange (F 'gpt.img') 1024 $gEnd))
    Detach 'small'

    # --- --fit: GPT image to a larger disk (96 MB) ----------------------------
    $t = New-Vhd 'large' 96
    $r = Run "if=gpt.img of=\\.\PhysicalDrive$t bs=1M --fit --verify"
    Check 'GPT image to a larger disk' ($r.Code -eq 0) $r.Err
    Check 'larger disk: the image''s hash is checked, past the part written' ($r.Err -match 'SHA-256 of gpt\.img matches') $r.Err
    Check 'larger disk: verified, the rebuilt GPT included' ($r.Err -match "verified: '\\\\\.\\PhysicalDrive$t'") $r.Err
    Settle $t GPT $gptParts
    $d = Get-Disk -Number $t
    Check 'larger disk: the free space after the partitions is usable' ($d.PartitionStyle -eq 'GPT' -and $d.LargestFreeExtent -gt 60MB) ($d | Format-List PartitionStyle, LargestFreeExtent | Out-String)
    Check 'larger disk: same partitions' (((Parts $t) -join ',') -eq ($gptParts -join ','))
    Detach 'large'

    # --- a trimmed card image goes back onto a disk ------------------------------
    $t = New-Vhd 'restore' 32
    $r = Run "if=gpt-card.img of=\\.\PhysicalDrive$t bs=1M --fit"
    Check 'trimmed GPT image restores to a disk' ($r.Code -eq 0) $r.Err
    Settle $t GPT $gptParts
    Check 'restored disk: Windows sees the same partitions' ((Get-Disk -Number $t).PartitionStyle -eq 'GPT' -and ((Parts $t) -join ',') -eq ($gptParts -join ',')) "$((Parts $t) -join ', ')"
    $r2 = Run "--layout if=\\.\PhysicalDrive$t"
    Check 'restored disk: backup GPT valid' ($r2.Out -notmatch 'note:') $r2.Out
    Detach 'restore'

    # --- a compressed card image goes back onto a disk -----------------------------
    $t = New-Vhd 'restorexz' 32
    $r = Run "if=gpt-card.img.xz of=\\.\PhysicalDrive$t bs=1M --fit"
    Check 'compressed GPT image restores to a disk' ($r.Code -eq 0 -and $r.Err -match 'xz-compressed') $r.Err
    Check 'from .xz: both hashes are checked' ($r.Err -match 'SHA-256 of gpt-card\.img\.xz matches' -and $r.Err -match 'SHA-256 of gpt-card\.img \(uncompressed\) matches') $r.Err
    Settle $t GPT $gptParts
    Check 'from .xz: Windows sees the same partitions' ((Get-Disk -Number $t).PartitionStyle -eq 'GPT' -and ((Parts $t) -join ',') -eq ($gptParts -join ',')) "$((Parts $t) -join ', ')"
    $r2 = Run "--layout if=\\.\PhysicalDrive$t"
    Check 'from .xz: backup GPT valid' ($r2.Out -notmatch 'note:') $r2.Out
    $before = Run "if=\\.\PhysicalDrive$t of=xzdev-before.bin bs=1M count=1 status=none"
    $r = Run "if=gpt.img of=\\.\PhysicalDrive$t --xz"
    $after = Run "if=\\.\PhysicalDrive$t of=xzdev-after.bin bs=1M count=1 status=none"
    Check 'compressing onto a disk is refused and the disk is untouched' ($r.Code -eq 1 -and $r.Err -match 'cannot compress onto a device' -and
        (Get-FileHash (F 'xzdev-before.bin')).Hash -eq (Get-FileHash (F 'xzdev-after.bin')).Hash) $r.Err
    Detach 'restorexz'

    # --- --fit refuses a disk that is too small, before writing anything -----
    $t = New-Vhd 'tiny' 8
    $before = Run "if=\\.\PhysicalDrive$t of=tiny-before.bin bs=1M count=1 status=none"
    $r = Run "if=gpt.img of=\\.\PhysicalDrive$t bs=1M --fit"
    Check 'too small a disk is refused' ($r.Code -eq 1 -and $r.Err -match 'need' -and $r.Err -notmatch 'records') $r.Err
    $after = Run "if=\\.\PhysicalDrive$t of=tiny-after.bin bs=1M count=1 status=none"
    Check 'the refused disk is untouched' ((Get-FileHash (F 'tiny-before.bin')).Hash -eq (Get-FileHash (F 'tiny-after.bin')).Hash)
    $r = Run "if=gpt.img of=\\.\PhysicalDrive$t bs=1M"
    Check 'without --fit an oversize image fails when the disk is full' ($r.Code -eq 1 -and $r.Err -match 'error writing') $r.Err
    Detach 'tiny'

    # --- --fit: MBR image (64 MB) to a smaller disk (40 MB) -------------------
    # The disk was GPT before, so it has a backup GPT at its end.
    $t = New-Vhd 'mbrdst' 40 @('convert gpt')
    $sectors = (Get-Disk -Number $t).Size / 512
    $tail = "skip=$($sectors - 33) bs=512 count=33 status=none"
    $before = Run "if=\\.\PhysicalDrive$t of=mbr-tail-before.bin $tail"
    $hadGpt = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((F 'mbr-tail-before.bin')), 32 * 512, 8) -eq 'EFI PART'
    $r = Run "if=mbr.img of=\\.\PhysicalDrive$t bs=1M --fit --verify"
    Check 'oversize MBR image fits a smaller disk' ($r.Code -eq 0 -and $r.Err -match "^$mbrEnd bytes|`n$mbrEnd bytes" -and $r.Err -notmatch 'rebuilt the GPT') $r.Err
    Check 'the old backup GPT at the disk''s end is cleared' ($hadGpt -and $r.Err -match "cleared the last 16896 bytes .* so no backup GPT") "had GPT: $hadGpt`n$($r.Err)"
    $after = Run "if=\\.\PhysicalDrive$t of=mbr-tail-after.bin $tail"
    $zero = -not ([IO.File]::ReadAllBytes((F 'mbr-tail-after.bin')) | Where-Object { $_ -ne 0 } | Select-Object -First 1)
    Check '... it reads back as zeros, and the verify covers it' ($zero -and $r.Err -match "verified: '\\\\\.\\PhysicalDrive$t'") $r.Err
    Settle $t MBR $mbrParts
    $got = Parts $t
    Check 'Windows sees the MBR partitions, logical ones included' ((Get-Disk -Number $t).PartitionStyle -eq 'MBR' -and ($got -join ',') -eq ($mbrParts -join ',')) "source: $($mbrParts -join ', ')`ntarget: $($got -join ', ')"
    Detach 'mbrdst'

    # --- option checks ----------------------------------------------------------
    $r = Run "if=gpt.img of=x.bin --fit count=1"
    Check '--fit with count is rejected' ($r.Code -eq 1 -and $r.Err -match 'cannot be combined') $r.Err
    $r = Run "if=gpt.img of=gpt-trim.img --fit status=none"
    $r2 = Run "--layout if=gpt-trim.img"
    Check '--fit of a GPT image to a file trims it and keeps the GPT valid' ($r.Code -eq 0 -and (Get-Item (F 'gpt-trim.img')).Length -eq $gEnd + 33 * 512 -and $r2.Out -notmatch 'note:') "$($r.Err)`n$($r2.Out)"
    [IO.File]::WriteAllBytes((F 'big.bin'), (New-Object byte[] (80MB)))
    $r = Run "if=gpt.img of=big.bin --fit conv=notrunc status=none"
    Check '--fit to a longer existing file still ends with the backup GPT' ($r.Code -eq 0 -and (Get-Item (F 'big.bin')).Length -eq $gEnd + 33 * 512) $r.Err
    cmd /c "cd /d `"$work`" && `"$DD`" if=gpt.img --fit 2>pipe-err.txt | more >nul"
    $pipeErr = Get-Content (F 'pipe-err.txt') -Raw
    Check '--fit of a GPT image to a pipe is refused' ($pipeErr -match 'needs a disk or a file') $pipeErr
    $r = Run "if=mbr.img of=mbr-trim.img --fit status=none"
    Check '--fit of an MBR image to a file trims it' ($r.Code -eq 0 -and (Get-Item (F 'mbr-trim.img')).Length -eq $mbrEnd) $r.Err
    $r = Run "if=raw.img of=raw-copy.img --fit status=none"
    Check '--fit without a partition table copies everything' ($r.Code -eq 0 -and (Get-Item (F 'raw-copy.img')).Length -eq 1048576) $r.Err
} finally {
    foreach ($v in $vhds) { try { Diskpart @("select vdisk file=`"$v`"", 'detach vdisk') } catch { } }
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "$($script:count - $script:failures) of $script:count passed"
exit [int]($script:failures -ne 0)
