# .xz compression and decompression.  Files only; no admin needed.
#   powershell -ExecutionPolicy Bypass -File tests\xz-tests.ps1 [-DD build\dd.exe]
param(
    [string]$DD = (Join-Path $PSScriptRoot '..\build\dd.exe')
)

$DD = (Resolve-Path $DD).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("ddxz-" + [Guid]::NewGuid())
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
function Hash([string]$name) { (Get-FileHash (F $name)).Hash }
function IsXz([string]$name) {
    $b = New-Object byte[] 6
    $fs = [IO.File]::OpenRead((F $name)); $n = $fs.Read($b, 0, 6); $fs.Close()
    $n -eq 6 -and $b[0] -eq 0xFD -and $b[1] -eq 0x37 -and $b[2] -eq 0x7A -and $b[3] -eq 0x58 -and $b[4] -eq 0x5A -and $b[5] -eq 0
}

try {
    # 4 MB: text-like data, zeros and random bytes
    $data = New-Object byte[] (4MB)
    $r = New-Object Random 11
    for ($i = 0; $i -lt 1MB; $i++) { $data[$i] = [byte](97 + ($i * 7 + [int]($i / 13)) % 26) }
    $rnd = New-Object byte[] (1MB); $r.NextBytes($rnd); [Array]::Copy($rnd, 0, $data, 3MB, 1MB)
    [IO.File]::WriteAllBytes((F 'a.img'), $data)
    $h = Hash 'a.img'

    # --- every level round-trips ---------------------------------------------
    $bad = @()
    foreach ($lv in 0..9) {
        $r1 = DDRun "if=a.img of=l$lv.out --xz=$lv bs=1M status=none"
        $r2 = DDRun "if=l$lv.out of=l$lv.back bs=1M status=none"
        if ($r1.Code -ne 0 -or $r2.Code -ne 0 -or -not (IsXz "l$lv.out") -or (Hash "l$lv.back") -ne $h) { $bad += $lv }
    }
    Check 'levels 0-9 compress and decompress exactly' ($bad.Count -eq 0) "failed levels: $bad"
    # 1 MB of the 4 is random: that part is stored, the rest all but vanishes
    $sizes = 0..9 | ForEach-Object { (Get-Item (F "l$_.out")).Length }
    Check 'every level keeps random data at its size and shrinks the rest' (($sizes | Where-Object { $_ -gt 1.02MB -or $_ -lt 1MB }).Count -eq 0) "sizes: $sizes"

    # --- naming, defaults and raw ----------------------------------------------
    $r1 = DDRun "if=a.img of=auto.img.xz bs=1M"
    Check 'an of= ending in .xz is compressed' ($r1.Code -eq 0 -and (IsXz 'auto.img.xz') -and $r1.Err -match 'level 9') $r1.Err
    Check 'the summary gives the compressed size' ($r1.Err -match 'compressed 4\.0 MiB \(4194304\) to') $r1.Err
    $r1 = DDRun "if=a.img of=raw.img.xz --no-xz status=none"
    Check '--no-xz writes .xz names uncompressed' ($r1.Code -eq 0 -and (Hash 'raw.img.xz') -eq $h) $r1.Err
    $r1 = DDRun "if=auto.img.xz of=still.xz --no-xz status=none"
    Check '--no-xz copies an .xz input as it is' ($r1.Code -eq 0 -and (Hash 'still.xz') -eq (Hash 'auto.img.xz')) $r1.Err
    $r1 = Run "`"$DD`" if=a.img --xz=1 status=none > piped.xz"
    Check 'compressing to standard output' ($r1.Code -eq 0 -and (IsXz 'piped.xz')) $r1.Err
    $r1 = DDRun "if=piped.xz of=piped.back status=none"
    Check 'that stream decompresses' ((Hash 'piped.back') -eq $h)

    # --- decompressing ------------------------------------------------------------
    $r1 = DDRun "if=auto.img.xz of=back.img bs=1M"
    Check 'an .xz input is decompressed' ($r1.Code -eq 0 -and (Hash 'back.img') -eq $h -and $r1.Err -match 'xz-compressed') $r1.Err
    Check 'records count the uncompressed data' ($r1.Err -match '4\+0 records in')
    $r1 = DDRun "if=auto.img.xz of=part.img bs=1M skip=3 count=1 status=none"
    $want = New-Object byte[] (1MB); [Array]::Copy($data, 3MB, $want, 0, 1MB)
    Check 'skip and count on an .xz input' ($r1.Code -eq 0 -and [Convert]::ToBase64String([IO.File]::ReadAllBytes((F 'part.img'))) -eq [Convert]::ToBase64String($want)) $r1.Err

    # two streams joined end to end decompress as one
    $joined = [IO.File]::ReadAllBytes((F 'l1.out')) + [IO.File]::ReadAllBytes((F 'l2.out'))
    [IO.File]::WriteAllBytes((F 'joined.xz'), [byte[]]$joined)
    $r1 = DDRun "if=joined.xz of=joined.img status=none"
    Check 'concatenated .xz streams' ($r1.Code -eq 0 -and (Get-Item (F 'joined.img')).Length -eq 8MB) $r1.Err

    # many blocks: decoded by several threads.  Compression tests never use
    # more than 16 MB; level 0's 1 MiB blocks still give 16 of them.
    $big = New-Object byte[] (16MB)
    for ($i = 0; $i -lt 16; $i++) { $r.NextBytes($rnd); [Array]::Copy($rnd, 0, $big, $i * 1MB, 256KB) }
    [IO.File]::WriteAllBytes((F 'big.img'), $big)
    $r1 = DDRun "if=big.img of=big.xz --xz=0 bs=1M"
    $r2 = DDRun "if=big.xz of=big.back bs=1M status=none"
    Check 'a 16 MB multi-block image round-trips' ($r1.Err -match 'threads' -and $r2.Code -eq 0 -and (Hash 'big.back') -eq (Hash 'big.img')) "$($r1.Err)$($r2.Err)"

    # --- dd runs at idle priority, compression workers included ----------------------
    # Random data is fed through a pipe a block at a time, so dd stays alive
    # with its workers started while the threads are sampled: at most 16 MB.
    $psi = New-Object Diagnostics.ProcessStartInfo $DD, "if=- of=`"$(F 'prio.xz')`" --xz=0 bs=1M status=none"
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.CreateNoWindow = $true
    $p = [Diagnostics.Process]::Start($psi)
    $in = $p.StandardInput.BaseStream
    $sample = $null
    for ($i = 0; $i -lt 16 -and -not $p.HasExited; $i++) {
        $r.NextBytes($rnd)
        $in.Write($rnd, 0, $rnd.Length)
        $in.Flush()
        Start-Sleep -Milliseconds 250
        try {
            $p.Refresh()
            if ($p.Threads.Count -ge 4) {
                $sample = [pscustomobject]@{
                    Class   = "$($p.PriorityClass)"
                    Threads = $p.Threads.Count
                    MaxBase = ($p.Threads | ForEach-Object { $_.BasePriority } | Measure-Object -Maximum).Maximum
                }
                break
            }
        } catch { }
    }
    $in.Close()
    if (-not $p.WaitForExit(10000)) { $p.Kill() }
    Check 'dd runs in the Idle priority class' ($sample -and $sample.Class -eq 'Idle') "$sample"
    # in the Idle class a normal thread has base priority 4; nothing may be above 6
    Check 'every thread, compression workers included, is at idle priority' ($sample -and $sample.Threads -ge 4 -and $sample.MaxBase -le 6) "$sample"

    # --- damage ---------------------------------------------------------------------
    $c = [IO.File]::ReadAllBytes((F 'l6.out'))
    $c[[int]($c.Length / 2)] = $c[[int]($c.Length / 2)] -bxor 0x55
    [IO.File]::WriteAllBytes((F 'corrupt.xz'), $c)
    $r1 = DDRun "if=corrupt.xz of=x.img status=none"
    Check 'corrupt data is an error' ($r1.Code -eq 1 -and $r1.Err -match 'corrupt') $r1.Err
    $c = [IO.File]::ReadAllBytes((F 'l6.out'))
    [IO.File]::WriteAllBytes((F 'short.xz'), $c[0..([int]($c.Length * 0.6))])
    $r1 = DDRun "if=short.xz of=x.img status=none"
    Check 'a truncated file is reported as truncated' ($r1.Code -eq 1 -and $r1.Err -match 'truncated') $r1.Err

    # --- options ----------------------------------------------------------------------
    $r1 = DDRun "if=a.img of=x.xz --xz seek=1"
    Check 'compressing with seek is rejected' ($r1.Code -eq 1 -and $r1.Err -match 'one pass') $r1.Err
    $r1 = DDRun "if=a.img of=x.xz --xz conv=notrunc"
    Check 'compressing with notrunc is rejected' ($r1.Code -eq 1 -and $r1.Err -match 'one pass') $r1.Err
    $r1 = DDRun "if=a.img of=x.xz --xz=12"
    Check 'a level above 9 is rejected' ($r1.Code -eq 1 -and $r1.Err -match 'invalid xz level') $r1.Err
    $r1 = DDRun "if=auto.img.xz of=x.img --fit --no-xz"
    Check '--fit with --no-xz is rejected' ($r1.Code -eq 1 -and $r1.Err -match 'no-xz') $r1.Err

    # --- an MBR image: the table is read through the compression ----------------------
    $m = New-Object byte[] (8MB)
    $m[446 + 4] = 0x0C; [BitConverter]::GetBytes([uint32]2048).CopyTo($m, 446 + 8); [BitConverter]::GetBytes([uint32]4096).CopyTo($m, 446 + 12)
    $m[510] = 0x55; $m[511] = 0xAA
    for ($i = 1MB; $i -lt 3MB; $i += 4096) { $m[$i] = 1 }
    [IO.File]::WriteAllBytes((F 'mbr.img'), $m)
    $r1 = DDRun "if=mbr.img of=mbr.img.xz status=none"
    $lay = (& $DD --layout "if=$(F 'mbr.img.xz')") -join "`n"
    Check '--layout reads the table of a compressed image' ($lay -match "`nMBR" -and $lay -match 'xz-compressed' -and $lay -match 'data ends at [^(]*\(3145728\)') $lay
    $r1 = DDRun "if=mbr.img.xz of=mbr-fit.img --fit status=none"
    Check '--fit from a compressed image stops at the last partition' ($r1.Code -eq 0 -and (Get-Item (F 'mbr-fit.img')).Length -eq 3MB) $r1.Err
    $r1 = DDRun "if=mbr.img of=mbr-fit.img.xz --fit status=none"
    $r2 = DDRun "if=mbr-fit.img.xz of=mbr-fit2.img status=none"
    Check '--fit into a compressed image' ($r1.Code -eq 0 -and (Hash 'mbr-fit2.img') -eq (Hash 'mbr-fit.img')) "$($r1.Err)$($r2.Err)"

    # --- memory: threads, then the level, fit what may be used ------------------------
    $r1 = DDRun "if=a.img of=m-default.xz --xz=9 bs=1M"
    # 2 GiB is kept free when there is a page file, else 4 GiB
    $os = Get-CimInstance Win32_OperatingSystem
    $swap = $os.TotalVirtualMemorySize -gt $os.TotalVisibleMemorySize
    $keep = if ($swap) { '2\.0 GiB free \(there is a page file\)' } else { '4\.0 GiB free \(there is no page file\)' }
    $rule = "(and dd may use [\d.]+ [MG]iB of it, keeping $keep|no more than the [\d.]+ GiB to keep free, so dd may use a quarter of it)"
    Check 'by default dd keeps 2 GiB free (with a page file) or 4 GiB, and says what was free and what it uses' ($r1.Code -eq 0 -and
        $r1.Err -match "compressing to xz at level 9 on \d+ threads?, using up to [\d.]+ [MG]iB of memory; [\d.]+ [MG]iB was free before starting, $rule") $r1.Err
    $r1 = DDRun "if=a.img of=m-1g.xz --xz=9 --xz-memory=1G bs=1M"
    Check '--xz-memory=1G: level 9 does not fit one thread, so level 8 on one' ($r1.Code -eq 0 -and
        $r1.Err -match 'xz level 9 needs 1\.2 GiB of memory even on one thread, more than the 1\.0 GiB compressing may use; compressing at level 8 instead' -and
        $r1.Err -match 'at level 8 on 1 thread, using up to 657 MiB of memory; [\d.]+ [MG]iB was free before starting, and --xz-memory lets dd use 1\.0 GiB') $r1.Err
    $r1 = DDRun "if=a.img of=m-100m.xz --xz=9 --xz-memory=100M bs=1M"
    $r2 = DDRun "if=m-100m.xz of=m-100m.back bs=1M status=none"
    Check '--xz-memory=100M: level 4, and the image is intact' ($r1.Code -eq 0 -and $r1.Err -match 'compressing at level 4 instead' -and (Hash 'm-100m.back') -eq $h) $r1.Err
    $r1 = DDRun "if=a.img of=m-6.xz --xz=6 --xz-memory=400M bs=1M"
    Check '--xz-memory=400M at level 6: fewer threads, the same level' ($r1.Code -eq 0 -and $r1.Err -notmatch 'instead' -and
        $r1.Err -match 'at level 6 on [12] threads?') $r1.Err
    $r1 = DDRun "if=l9.out of=m-xz2xz.xz --xz=9 --xz-memory=2G --verify bs=1M"
    Check '.xz to .xz: the decompressor takes a quarter, the compressor the rest; verified' ($r1.Code -eq 0 -and
        $r1.Err -match 'level 9 on 1 thread, using up to 1\.2 GiB of memory; .*--xz-memory lets dd use 2\.0 GiB; decompressing the input has 512 MiB of that' -and
        $r1.Err -match 'verified:') $r1.Err
    $r1 = DDRun "if=a.img of=m-bad.xz --xz-memory=0"
    Check 'a memory size of 0 is refused' ($r1.Code -eq 1 -and $r1.Err -match "invalid memory size '0'") $r1.Err
    $r1 = DDRun "if=a.img of=m-tiny.xz --xz=1 --xz-memory=1"
    Check 'below what level 0 needs: said so' ($r1.Code -eq 0 -and $r1.Err -match 'even xz level 0 on one thread needs') $r1.Err

    # --- damaged compressed data: nothing after it can be found, so the copy
    # ends even with conv=noerror (it used to retry, or pad, without end) ---
    $x = [IO.File]::ReadAllBytes((F 'l1.out'))
    for ($i = 20000; $i -lt 20100; $i++) { $x[$i] = $x[$i] -bxor 0x5A }
    [IO.File]::WriteAllBytes((F 'bad.xz'), $x)
    $p = Start-Process -FilePath $DD -ArgumentList "if=`"$(F 'bad.xz')`"", "of=`"$(F 'bad.out')`"", 'conv=noerror,sync', 'status=none' `
        -NoNewWindow -PassThru -RedirectStandardError (F 'bad.err')
    $null = $p.Handle                   # without it, ExitCode stays empty
    $ended = $p.WaitForExit(30000)
    if (-not $ended) { $p.Kill() }
    Check 'damaged .xz with conv=noerror,sync: fails, and ends' ($ended -and $p.ExitCode -eq 1 -and (Get-Item (F 'bad.out')).Length -le 4MB -and
        (Get-Content (F 'bad.err') -Raw) -match 'error reading') "ended: $ended"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "$($script:count - $script:failures) of $script:count passed"
exit [int]($script:failures -ne 0)
