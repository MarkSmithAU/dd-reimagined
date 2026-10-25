# Raw device tests for dd.exe.  Needs an elevated prompt.  Creates a 64 MB
# VHDX, attaches it, and only ever writes to that virtual disk.
#   powershell -ExecutionPolicy Bypass -File tests\device-tests.ps1 [-DD build\dd.exe]
param(
    [string]$DD = (Join-Path $PSScriptRoot '..\build\dd.exe')
)

$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error 'device-tests.ps1 needs an elevated prompt'
}

$DD = (Resolve-Path $DD).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("dddev-" + [Guid]::NewGuid())
New-Item -ItemType Directory $work | Out-Null
$vhd = Join-Path $work 'test.vhdx'
$used = (Get-PSDrive -PSProvider FileSystem).Name
$letter = [string]((68..90 | ForEach-Object { [char]$_ } | Where-Object { $used -notcontains [string]$_ }) | Select-Object -Last 1)
$script:failures = 0
$script:count = 0
$size = 64MB

function F([string]$name) { Join-Path $work $name }
function Bytes([string]$name) { ,[IO.File]::ReadAllBytes((F $name)) }

function Run([string]$operands) {
    $err = F 'stderr.txt'
    cmd /c "cd /d `"$work`" && `"$DD`" $operands 2>`"$err`""
    $code = $LASTEXITCODE
    [pscustomobject]@{ Code = $code; Err = ([IO.File]::ReadAllText($err)).Replace("`r", '') }
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

function Same([byte[]]$a, [byte[]]$b) { [Linq.Enumerable]::SequenceEqual($a, $b) }

# Whether the test volume has its drive letter (asks the mount manager, so
# a locked volume still counts).
function Has-Letter {
    cmd /c "mountvol ${letter}:\ /L >nul 2>nul"
    $LASTEXITCODE -eq 0
}

function Slice([byte[]]$a, [int]$start, [int]$len) {
    $r = New-Object byte[] $len
    [Array]::Copy($a, $start, $r, 0, $len)
    ,$r
}

function Diskpart([string[]]$lines) {
    $script = F 'diskpart.txt'
    Set-Content -Encoding ASCII $script $lines
    diskpart.exe /s $script | Out-Null
}

try {
    Diskpart @(
        "create vdisk file=`"$vhd`" maximum=64 type=fixed",
        "select vdisk file=`"$vhd`"",
        'attach vdisk',
        'convert mbr',
        'create partition primary',
        'format fs=fat32 quick label=DDTEST',
        "assign letter=$letter")
    $n = (Get-DiskImage -ImagePath $vhd).Number
    $disk = "\\.\PhysicalDrive$n"
    Write-Host "test disk is $disk, volume ${letter}:"
    [IO.File]::WriteAllText("${letter}:\hello.txt", 'hello')

    # --- reading ----------------------------------------------------------
    $r = Run "id=${letter}: of=img.bin bs=1M"
    Check 'id= selects the whole disk' ($r.Code -eq 0 -and $r.Err -match [regex]::Escape("selects $disk (whole disk)")) $r.Err
    $img = Bytes 'img.bin'
    Check 'id= reads the whole disk' ($img.Length -eq $size) "$($img.Length) bytes"
    $line = if (Test-Path (F 'img.bin.hashes')) { [IO.File]::ReadAllText((F 'img.bin.hashes')) } else { '' }
    $sha = (Get-FileHash (F 'img.bin') -Algorithm SHA256).Hash.ToLower()
    Check 'an image of a disk gets a hash file by default' ($line -eq "$sha  img.bin`n") $line
    $r = Run "if=$disk of=img-v.bin bs=1M --no-hashes --verify"
    Check 'reading a disk with --verify' ($r.Code -eq 0 -and $r.Err -match "verified: 'img-v\.bin'" -and -not (Test-Path (F 'img-v.bin.hashes'))) $r.Err

    # the volume is mounted and Windows may update its FAT at any time, so
    # these compare with a read made just before, not with img.bin
    $null = Run "if=$disk of=head.bin bs=1M count=1 --no-hashes"
    $head = Bytes 'head.bin'
    $r = Run "if=$disk of=p1.bin bs=1000 count=70"
    Check 'unaligned bs reads' ($r.Code -eq 0 -and (Same (Bytes 'p1.bin') (Slice $head 0 70000))) $r.Err
    $r = Run "if=$disk of=p2.bin bs=1 skip=513 count=1000"
    Check 'unaligned skip reads' ($r.Code -eq 0 -and (Same (Bytes 'p2.bin') (Slice $head 513 1000))) $r.Err
    $r = Run "if=$disk of=nul bs=1000 skip=67000"
    Check 'reads stop at the end of the device' ($r.Code -eq 0 -and $r.Err -match '108\+1 records in' -and $r.Err -match "^108864 bytes|\n108864 bytes") $r.Err

    # --- --lock-input: locked for reading, never dismounted -----------------
    $marker = 'lock-input-' + [Guid]::NewGuid()
    [IO.File]::WriteAllText("${letter}:\marker.txt", $marker)
    $r = Run "if=$disk of=lock.bin bs=1M --no-hashes --lock-input"
    Check '--lock-input locks the disk''s volume' ($r.Code -eq 0 -and $r.Err -match "locked .*\(${letter}:\\\) while it is read" -and $r.Err -notmatch 'dismount') $r.Err
    $text = [Text.Encoding]::ASCII.GetString((Bytes 'lock.bin'))
    Check 'locking flushes a file just written into the image' ($text.Contains($marker))
    $mounted = try { [IO.File]::ReadAllText("${letter}:\marker.txt") -eq $marker } catch { $false }
    Check 'the volume stays mounted after --lock-input' $mounted
    $r = Run "if=\\.\${letter}: of=lockv.bin bs=1M --no-hashes --lock-input"
    Check '--lock-input locks a volume input' ($r.Code -eq 0 -and $r.Err -match 'locked' -and (Get-Item (F 'lockv.bin')).Length -gt 0) $r.Err

    $held = [IO.File]::Open("${letter}:\hello.txt", 'Open', 'Read', 'Read')
    $r = Run "if=\\.\${letter}: of=busy.bin bs=1M --lock-input"
    $r2 = Run "if=$disk of=busyd.bin bs=1M --no-hashes --lock-input"
    $r3 = Run "if=\\.\${letter}: of=busy2.bin bs=1M count=1"
    $held.Close()
    Check '--lock-input refuses a busy volume, before creating the output' ($r.Code -eq 1 -and $r.Err -match 'cannot lock .* for reading' -and -not (Test-Path (F 'busy.bin'))) $r.Err
    Check '--lock-input refuses a disk with a busy volume' ($r2.Code -eq 1 -and $r2.Err -match 'cannot lock') $r2.Err
    Check 'without --lock-input a busy volume is still read' ($r3.Code -eq 0) $r3.Err

    # dd blocks writing to a pipe nobody reads, holding the lock meanwhile
    $p = Start-Process cmd -PassThru -WindowStyle Hidden -ArgumentList '/c',
        "`"`"$DD`" if=$disk bs=1M --lock-input status=none | ping -n 4 127.0.0.1 >nul`""
    $blocked = $false
    for ($i = 0; $i -lt 40 -and -not $blocked -and -not $p.HasExited; $i++) {
        try { [IO.File]::Open("${letter}:\hello.txt", 'Open', 'Read', 'Read').Close(); Start-Sleep -Milliseconds 100 }
        catch { $blocked = $true }
    }
    $p.WaitForExit()
    $after = try { [IO.File]::ReadAllText("${letter}:\hello.txt") -eq 'hello' } catch { $false }
    Check 'other programs cannot open files while the input is locked' $blocked
    Check '... and can again once dd is done' $after

    $sysdisk = (Get-Partition -DriveLetter $env:SystemDrive[0]).DiskNumber
    $r = Run "if=\\.\PhysicalDrive$sysdisk of=nul count=1 --lock-input"
    Check '--lock-input refuses the Windows disk, locking nothing' ($r.Code -eq 1 -and $r.Err -match 'Windows is running from' -and $r.Err -notmatch 'while it is read') $r.Err

    # --- filters ----------------------------------------------------------
    $r = Run "if=/dev/zero of=$disk count=0 --filter=removable"
    Check 'filter=removable refuses a VHD' ($r.Code -eq 1 -and $r.Err -match 'refusing') $r.Err
    $r = Run "if=/dev/zero of=\\.\${letter}: count=0 --filter=disk"
    Check 'filter=disk refuses a volume' ($r.Code -eq 1 -and $r.Err -match 'refusing') $r.Err

    # --- locking ----------------------------------------------------------
    $blk = New-Object byte[] 4096
    (New-Object Random 7).NextBytes($blk)
    [IO.File]::WriteAllBytes((F 'blk.bin'), $blk)

    $held = [IO.File]::Open("${letter}:\hello.txt", 'Open', 'Read', 'Read')
    $r = Run "if=/dev/zero of=\\.\${letter}: count=1"
    $held.Close()
    Check 'busy volume is not written' ($r.Code -eq 1 -and $r.Err -match 'cannot lock') $r.Err

    $held = [IO.File]::Open("${letter}:\hello.txt", 'Open', 'Read', 'Read')
    $r = Run "if=blk.bin of=\\.\${letter}: bs=512 seek=16 --force-dismount"
    $stale = $false
    try { $held.Read((New-Object byte[] 1), 0, 1) | Out-Null } catch { $stale = $true }
    $held.Close()
    Check 'force-dismount writes a busy volume' ($r.Code -eq 0 -and $r.Err -match 'forcibly dismounted') $r.Err
    Check 'force-dismount invalidates open files' $stale

    # --- the disk Windows runs from is refused before anything is locked ----
    $sysdisk = (Get-Partition -DriveLetter $env:SystemDrive[0]).DiskNumber
    $r = Run "if=/dev/zero of=\\.\PhysicalDrive$sysdisk count=0 --force-dismount"
    Check 'never writes the Windows disk' ($r.Code -eq 1 -and $r.Err -match 'refusing' -and $r.Err -notmatch 'dismounted') $r.Err
    $r = Run "if=/dev/zero of=\\.\$env:SystemDrive count=0 --force-dismount"
    Check 'never writes the Windows volume' ($r.Code -eq 1 -and $r.Err -match 'refusing' -and $r.Err -notmatch 'dismounted') $r.Err

    # --- writing a volume ---------------------------------------------------
    $r = Run "if=blk.bin of=\\.\${letter}: bs=512 seek=8 --filter=partition --verify"
    Check 'volume write locks and dismounts' ($r.Code -eq 0 -and $r.Err -match 'dismounted') $r.Err
    Check 'a volume write is verified through the locked handle' ($r.Err -match "verified: '\\\\\.\\${letter}:' reads back exactly as written \(4096 bytes") $r.Err
    Check 'the locked volume''s drive letter is removed, and put back' ($r.Err -match "removed drive letter ${letter}:" -and
        $r.Err -match "put drive letter ${letter}: back" -and (Has-Letter)) $r.Err
    $r = Run "if=\\.\${letter}: of=blk2.bin bs=512 skip=8 count=8"
    Check 'volume write lands' (Same (Bytes 'blk2.bin') $blk) $r.Err

    # a file held open for 1.5 s, in another process: the lock is retried
    $ready = F 'ready.txt'
    $closed = F 'closed.txt'
    $holder = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList '-NoProfile', '-Command',
        ("`$f = [IO.File]::Open('${letter}:\hello.txt', 'Open', 'Read', 'Read'); " +
         "[IO.File]::WriteAllText('$ready', ''); Start-Sleep -Milliseconds 1500; `$f.Close(); " +
         "[IO.File]::WriteAllText('$closed', [string][DateTime]::UtcNow.Ticks)")
    for ($i = 0; $i -lt 100 -and -not (Test-Path $ready); $i++) { Start-Sleep -Milliseconds 100 }
    $started = [DateTime]::UtcNow.Ticks
    $r = Run "if=blk.bin of=\\.\${letter}: bs=512 seek=24"
    $holder.WaitForExit()
    $heldAtStart = (Test-Path $ready) -and [long][IO.File]::ReadAllText($closed) -gt $started
    Check 'briefly busy volume is locked on retry' ($heldAtStart -and $r.Code -eq 0 -and $r.Err -match 'dismounted' -and $r.Err -notmatch 'forcibly') $r.Err
    $r = Run "if=\\.\${letter}: of=blk4.bin bs=512 skip=24 count=8"
    Check 'write after the retried lock lands' (Same (Bytes 'blk4.bin') $blk) $r.Err

    # --- drive letters ------------------------------------------------------
    # dd holds the lock while it waits for a pipe that writes after 2 s
    Set-Content -Encoding ASCII (F 'slow.cmd') "@(ping -n 3 127.0.0.1 >nul & echo x) | `"$DD`" of=\\.\${letter}: bs=512 seek=24 count=1 status=none"
    $p = Start-Process cmd -PassThru -WindowStyle Hidden -ArgumentList '/c', "`"$(F 'slow.cmd')`""
    $gone = $false
    for ($i = 0; $i -lt 25 -and -not $gone -and -not $p.HasExited; $i++) {
        Start-Sleep -Milliseconds 100
        $gone = -not (Has-Letter)
    }
    $p.WaitForExit()
    Check 'the drive letter is gone while the volume is locked' $gone
    $files = try { [IO.File]::ReadAllText("${letter}:\hello.txt") -eq 'hello' } catch { $false }
    Check '... and back, with its files, once dd is done' ((Has-Letter) -and $files)
    $r = Run "if=\\.\${letter}: of=nul bs=1M count=1 --lock-input"
    Check '--lock-input removes the letter too' ($r.Code -eq 0 -and $r.Err -match "removed drive letter ${letter}:" -and (Has-Letter)) $r.Err
    $r = Run "if=\\.\${letter}: of=nul bs=1M count=1 --lock-input --keep-letters"
    Check '--keep-letters leaves the letter in place' ($r.Code -eq 0 -and $r.Err -match 'locked' -and
        $r.Err -notmatch 'drive letter' -and (Has-Letter)) $r.Err

    # --- writing the disk ---------------------------------------------------
    $r = Run "if=$disk of=before.bin bs=1M count=2"
    $before = Bytes 'before.bin'
    $aa = New-Object byte[] 700
    for ($i = 0; $i -lt 700; $i++) { $aa[$i] = 0xAA }
    [IO.File]::WriteAllBytes((F 'aa.bin'), $aa)
    $held = [IO.File]::Open("${letter}:\hello.txt", 'Open', 'Read', 'Read')
    $r = Run "if=aa.bin of=$disk seek=1048676B --filter=fixed"
    Check 'busy disk is not written' ($r.Code -eq 1 -and $r.Err -match 'cannot lock') $r.Err
    $r = Run "if=aa.bin of=$disk seek=1048676B --filter=fixed --force-dismount"
    $held.Close()
    Check 'partial sector write with force-dismount' ($r.Code -eq 0 -and $r.Err -match 'forcibly dismounted') $r.Err
    $r = Run "if=$disk of=after.bin bs=1M count=2"
    $after = Bytes 'after.bin'
    Check 'bytes before the write are kept' (Same (Slice $after 1048576 100) (Slice $before 1048576 100))
    Check 'written bytes land' (Same (Slice $after 1048676 700) $aa)
    Check 'bytes after the write are kept' (Same (Slice $after 1049376 224) (Slice $before 1049376 224))

    $w = New-Object byte[] 1000123
    (New-Object Random 9).NextBytes($w)
    [IO.File]::WriteAllBytes((F 'w.bin'), $w)
    $r = Run "if=w.bin of=$disk bs=4096"
    Check 'image with a short last block' ($r.Code -eq 0 -and $r.Err -match '244\+1 records out') $r.Err
    Check 'a letter whose volume was overwritten is kept for it' ($r.Err -match "drive letter ${letter}: is kept for its volume" -and
        -not (Has-Letter)) $r.Err
    $r = Run "if=$disk of=wb.bin bs=1000123 count=1"
    Check 'image lands' (Same (Bytes 'wb.bin') $w) $r.Err

    # --- writing an image that has a hash file, verified -----------------------
    $r = Run "if=img.bin of=$disk bs=1M --verify"
    Check 'writing an image checks its hash file' ($r.Code -eq 0 -and $r.Err -match 'SHA-256 of img\.bin matches img\.bin\.hashes') $r.Err
    Check 'a disk write is verified' ($r.Err -match "verified: '\\\\\.\\PhysicalDrive$n' reads back exactly as written \($size bytes") $r.Err
    for ($i = 0; $i -lt 50 -and -not (Has-Letter); $i++) { Start-Sleep -Milliseconds 200 }
    Check '... and the volume gets its letter back when the image returns it' (Has-Letter)
    [IO.File]::WriteAllText((F 'img.bin.hashes'), ('0' * 64) + "  img.bin`n")
    $r = Run "if=img.bin of=$disk bs=1M --verify"
    Check 'a wrong hash fails the write and skips the verify' ($r.Code -eq 1 -and $r.Err -match 'does not match' -and $r.Err -match 'failed its hash check' -and $r.Err -notmatch 'verif') $r.Err

    # --- device interface paths (bound to the device, not its number) -------
    $iface = (Get-Disk -Number $n).Path
    $r = Run "if=/dev/zero of=`"$iface`" count=0 --filter=removable"
    Check 'filter=removable refuses a VHD by interface path' ($r.Code -eq 1 -and $r.Err -match 'refusing') $r.Err
    $r = Run "if=/dev/zero of=\\?\PhysicalDrive$n count=0 --filter=disk"
    Check '\\?\PhysicalDriveN counts as a device path' ($r.Code -eq 0) $r.Err
    $r = Run "if=blk.bin of=`"$iface`" bs=512 seek=6000 --filter=disk"
    Check 'write by interface path' ($r.Code -eq 0) $r.Err
    $r = Run "if=$disk of=blk3.bin bs=512 skip=6000 count=8"
    Check 'interface path write lands on the same disk' (Same (Bytes 'blk3.bin') $blk) $r.Err

    $r = Run "if=/dev/zero of=$disk bs=1M count=100"
    Check 'writing past the end fails' ($r.Code -eq 1 -and $r.Err -match 'error writing' -and $r.Err -match '64\+0 records out' -and $r.Err -match '67108864 bytes') $r.Err
} finally {
    try {
        Diskpart @("select vdisk file=`"$vhd`"", 'detach vdisk')
    } catch { }
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "$($script:count - $script:failures) of $script:count passed"
exit [int]($script:failures -ne 0)
