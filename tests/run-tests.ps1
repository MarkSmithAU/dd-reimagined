# Behaviour tests for dd.exe against GNU dd semantics.  Uses files only;
# nothing here touches a disk device.
#   powershell -ExecutionPolicy Bypass -File tests\run-tests.ps1 [-DD build\dd.exe]
param(
    [string]$DD = (Join-Path $PSScriptRoot '..\build\dd.exe')
)

$ErrorActionPreference = 'Stop'
$DD = (Resolve-Path $DD).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("ddtest-" + [Guid]::NewGuid())
New-Item -ItemType Directory $work | Out-Null
$script:failures = 0
$script:count = 0

function F([string]$name) { Join-Path $work $name }

function New-Data([string]$name, [int]$size, [int]$seed = 1) {
    $bytes = New-Object byte[] $size
    $rng = New-Object Random $seed
    $rng.NextBytes($bytes)
    [IO.File]::WriteAllBytes((F $name), $bytes)
    return ,$bytes
}

function Bytes([string]$name) { ,[IO.File]::ReadAllBytes((F $name)) }

# Runs dd through cmd so pipes and redirections carry binary data.
function Run([string]$cmdline) {
    $err = F 'stderr.txt'
    cmd /c "cd /d `"$work`" && $cmdline 2>`"$err`""
    $code = $LASTEXITCODE
    $text = if (Test-Path $err) { [IO.File]::ReadAllText($err) } else { '' }
    return [pscustomobject]@{ Code = $code; Err = $text.Replace("`r", '') }
}

function DDRun([string]$operands) { Run "`"$DD`" $operands" }

function Check([string]$what, [bool]$ok, [string]$detail = '') {
    $script:count++
    if ($ok) {
        Write-Host "ok   $what"
    } else {
        $script:failures++
        Write-Host "FAIL $what" -ForegroundColor Red
        if ($detail) { Write-Host "     $detail" }
    }
}

function Same([byte[]]$a, [byte[]]$b) { [Linq.Enumerable]::SequenceEqual($a, $b) }

function Slice([byte[]]$a, [int]$start, [int]$len) {
    $r = New-Object byte[] $len
    [Array]::Copy($a, $start, $r, 0, $len)
    return ,$r
}

function Records([string]$err) {
    ($err -split "`n" | Where-Object { $_ -match 'records (in|out)$' }) -join '|'
}

try {
    # --- plain copies and record counts -------------------------------------
    $d = New-Data 'a.bin' 10000
    $r = DDRun 'if=a.bin of=b.bin'
    Check 'default bs copies file' ($r.Code -eq 0 -and (Same $d (Bytes 'b.bin'))) $r.Err
    Check 'records 19+1' ((Records $r.Err) -eq '19+1 records in|19+1 records out') (Records $r.Err)
    Check 'transfer line' ($r.Err -match '(?m)^10000 bytes \(10 kB, 9\.8 KiB\) copied, [0-9.e-]+ s, .+B/s$') $r.Err

    $r = DDRun 'if=a.bin of=c.bin bs=4096 count=2'
    Check 'bs=4096 count=2' ($r.Code -eq 0 -and (Same (Slice $d 0 8192) (Bytes 'c.bin')))
    Check 'records 2+0' ((Records $r.Err) -eq '2+0 records in|2+0 records out') (Records $r.Err)

    $d1000 = New-Data 'k.bin' 1000
    $r = DDRun 'if=k.bin of=k2.bin'
    Check '1000 bytes wording' ($r.Err -match '(?m)^1000 bytes \(1\.0 kB\) copied') $r.Err
    $r = DDRun 'if=k.bin of=k3.bin count=1 bs=1'
    Check '1 byte wording' ($r.Err -match '(?m)^1 byte copied') $r.Err

    # --- size suffixes ----------------------------------------------------
    $r = DDRun 'if=a.bin of=s1.bin bs=1K count=2'
    Check 'bs=1K' ((Bytes 's1.bin').Length -eq 2048)
    $r = DDRun 'if=a.bin of=s2.bin bs=1kB count=2'
    Check 'bs=1kB is 1000' ((Bytes 's2.bin').Length -eq 2000)
    $r = DDRun 'if=a.bin of=s3.bin bs=2x512 count=1'
    Check 'bs=2x512' ((Bytes 's3.bin').Length -eq 1024)
    $r = DDRun 'if=a.bin of=s4.bin bs=1b count=3'
    Check 'b suffix is 512' ((Bytes 's4.bin').Length -eq 1536)
    $r = DDRun 'if=a.bin of=s5.bin bs=1KiB count=1'
    Check 'KiB suffix' ((Bytes 's5.bin').Length -eq 1024)
    $r = DDRun 'if=a.bin of=s6.bin bs=1q'
    Check 'unknown suffix fails' ($r.Code -eq 1 -and $r.Err -match "invalid number: '1q'") $r.Err
    $r = DDRun 'if=a.bin of=s7.bin bs=0'
    Check 'bs=0 fails' ($r.Code -eq 1 -and $r.Err -match 'invalid number') $r.Err

    # --- skip / seek / count in bytes ---------------------------------------
    $r = DDRun 'if=a.bin of=sk.bin bs=100 skip=3 count=5'
    Check 'skip=3 blocks' (Same (Slice $d 300 500) (Bytes 'sk.bin'))
    $r = DDRun 'if=a.bin of=skb.bin bs=100 skip=150B count=250B'
    Check 'skip/count with B suffix are bytes' (Same (Slice $d 150 250) (Bytes 'skb.bin')) $r.Err
    Check 'count_bytes records 2+1' ((Records $r.Err) -eq '2+1 records in|2+1 records out') (Records $r.Err)
    $r = DDRun 'if=a.bin of=skf.bin bs=100 skip=7 iflag=skip_bytes count=1'
    Check 'iflag=skip_bytes' (Same (Slice $d 7 100) (Bytes 'skf.bin'))
    $r = DDRun 'if=a.bin of=skp.bin skip=100'
    Check 'skip past end warns' ($r.Code -eq 0 -and $r.Err -match "cannot skip to specified offset" -and (Bytes 'skp.bin').Length -eq 0) $r.Err

    # seek truncates by default, keeps the tail with notrunc
    [IO.File]::WriteAllBytes((F 'dst.bin'), (Slice $d 5000 1000))
    $r = DDRun 'if=k.bin of=dst.bin bs=100 seek=2 count=1'
    $o = Bytes 'dst.bin'
    Check 'seek truncates to seek+data' ($o.Length -eq 300 -and (Same (Slice $o 0 200) (Slice $d 5000 200)) -and (Same (Slice $o 200 100) (Slice $d1000 0 100)))
    [IO.File]::WriteAllBytes((F 'dst2.bin'), (Slice $d 5000 1000))
    $r = DDRun 'if=k.bin of=dst2.bin bs=100 seek=2 count=1 conv=notrunc'
    $o = Bytes 'dst2.bin'
    Check 'conv=notrunc keeps the rest' ($o.Length -eq 1000 -and (Same (Slice $o 300 700) (Slice $d 5300 700)) -and (Same (Slice $o 200 100) (Slice $d1000 0 100)))
    [IO.File]::WriteAllBytes((F 'dst3.bin'), (Slice $d 5000 1000))
    $r = DDRun 'if=k.bin of=dst3.bin'
    Check 'existing output is truncated' ((Bytes 'dst3.bin').Length -eq 1000 -and (Same (Bytes 'dst3.bin') $d1000))
    $r = DDRun 'if=k.bin of=sb.bin bs=100 seek=50B oflag=seek_bytes count=1'
    Check 'oflag=seek_bytes' ((Bytes 'sb.bin').Length -eq 150)

    # --- reblocking -------------------------------------------------------
    $r = DDRun 'if=a.bin of=rb.bin ibs=100 obs=300'
    Check 'ibs/obs reblock data' (Same $d (Bytes 'rb.bin'))
    Check 'ibs/obs records' ((Records $r.Err) -eq '100+0 records in|33+1 records out') (Records $r.Err)

    # --- conversions ------------------------------------------------------
    [IO.File]::WriteAllText((F 'txt.txt'), 'Hello World abc')
    $null = DDRun 'if=txt.txt of=up.txt conv=ucase'
    Check 'conv=ucase' ([IO.File]::ReadAllText((F 'up.txt')) -ceq 'HELLO WORLD ABC')
    $null = DDRun 'if=txt.txt of=lo.txt conv=lcase'
    Check 'conv=lcase' ([IO.File]::ReadAllText((F 'lo.txt')) -ceq 'hello world abc')
    $null = DDRun 'if=txt.txt of=sw.txt conv=swab'
    Check 'conv=swab odd length' ([IO.File]::ReadAllText((F 'sw.txt')) -ceq 'eHll ooWlr dbac')
    $null = DDRun 'if=txt.txt of=sw2.txt conv=swab ibs=3'
    Check 'conv=swab across blocks' ([IO.File]::ReadAllText((F 'sw2.txt')) -ceq 'eHll ooWlr dbac')
    $r = DDRun 'if=txt.txt of=x.txt conv=lcase,ucase'
    Check 'lcase+ucase rejected' ($r.Code -eq 1 -and $r.Err -match 'cannot combine lcase and ucase') $r.Err
    $r = DDRun 'if=txt.txt of=x.txt conv=ebcdic'
    Check 'unsupported conversion' ($r.Code -eq 1 -and $r.Err -match "not supported") $r.Err
    $r = DDRun 'if=txt.txt of=x.txt conv=bogus'
    Check 'invalid conversion' ($r.Code -eq 1 -and $r.Err -match "invalid conversion: 'bogus'") $r.Err

    New-Data 'p.bin' 250 3 | Out-Null
    $r = DDRun 'if=p.bin of=sy.bin ibs=100 conv=sync'
    $o = Bytes 'sy.bin'
    Check 'conv=sync pads last block' ($o.Length -eq 300 -and $o[299] -eq 0 -and $o[250] -eq 0)
    Check 'conv=sync records' ((Records $r.Err) -eq '2+1 records in|0+1 records out') (Records $r.Err)

    # sparse: content identical, file marked sparse
    $z = New-Object byte[] 65536
    [Array]::Copy($d, 0, $z, 0, 100)
    [IO.File]::WriteAllBytes((F 'zz.bin'), $z)
    $r = DDRun 'if=zz.bin of=sp.bin bs=4096 conv=sparse'
    Check 'conv=sparse content' (Same $z (Bytes 'sp.bin')) $r.Err
    Check 'conv=sparse sets attribute' (((Get-Item (F 'sp.bin')).Attributes -band [IO.FileAttributes]::SparseFile) -ne 0)

    # --- creation flags -----------------------------------------------------
    $r = DDRun 'if=k.bin of=k.bin2 conv=excl'
    Check 'excl creates' ($r.Code -eq 0)
    $r = DDRun 'if=k.bin of=k.bin2 conv=excl'
    Check 'excl fails on existing' ($r.Code -eq 1 -and $r.Err -match "failed to open 'k.bin2'") $r.Err
    $r = DDRun 'if=k.bin of=nothere.bin conv=nocreat'
    Check 'nocreat fails on missing' ($r.Code -eq 1 -and -not (Test-Path (F 'nothere.bin'))) $r.Err
    $r = DDRun 'if=k.bin of=x conv=excl,nocreat'
    Check 'excl+nocreat rejected' ($r.Code -eq 1 -and $r.Err -match 'cannot combine excl and nocreat') $r.Err

    [IO.File]::WriteAllBytes((F 'ap.bin'), (Slice $d 0 10))
    $r = DDRun 'if=k.bin of=ap.bin oflag=append conv=notrunc'
    $o = Bytes 'ap.bin'
    Check 'oflag=append' ($o.Length -eq 1010 -and (Same (Slice $o 10 1000) $d1000))

    # --- direct I/O with unaligned sizes --------------------------------------
    $r = DDRun 'if=a.bin of=dio.bin bs=4096 iflag=direct oflag=direct'
    Check 'direct I/O with a short last block' ($r.Code -eq 0 -and (Same $d (Bytes 'dio.bin'))) $r.Err
    $r = DDRun 'if=a.bin of=dio2.bin bs=1000 iflag=direct oflag=direct'
    Check 'direct I/O with unaligned bs' ($r.Code -eq 0 -and (Same $d (Bytes 'dio2.bin'))) $r.Err

    # --- virtual devices and standard handles -------------------------------
    $r = DDRun 'if=/dev/zero of=z.bin bs=1M count=3'
    $o = Bytes 'z.bin'
    Check '/dev/zero' (Same $o (New-Object byte[] 3145728))
    $r = DDRun 'if=/dev/random of=rnd.bin bs=1000 count=5'
    $o = Bytes 'rnd.bin'
    Check '/dev/random' ($o.Length -eq 5000 -and ($o | Sort-Object -Unique).Count -gt 200)
    $r = DDRun 'if=a.bin of=/dev/null bs=1K'
    Check '/dev/null output' ($r.Code -eq 0 -and (Records $r.Err) -eq '9+1 records in|9+1 records out')
    $r = DDRun 'if=/dev/null of=n.bin'
    Check '/dev/null input' ($r.Code -eq 0 -and (Bytes 'n.bin').Length -eq 0 -and (Records $r.Err) -eq '0+0 records in|0+0 records out')

    $r = Run "type a.bin | `"$DD`" of=pipe.bin bs=4096"
    Check 'stdin from a pipe' ($r.Code -eq 0 -and (Same $d (Bytes 'pipe.bin'))) $r.Err
    $r = Run "type a.bin | `"$DD`" of=pipe2.bin bs=100 skip=7"
    Check 'skip on a pipe counts bytes read' (Same (Slice $d 700 9300) (Bytes 'pipe2.bin')) $r.Err
    $r = Run "type a.bin | `"$DD`" of=pipe3.bin bs=1M iflag=fullblock count=1"
    Check 'fullblock on a pipe' (Same $d (Bytes 'pipe3.bin')) $r.Err
    $r = Run "`"$DD`" if=a.bin bs=512 > out.bin"
    Check 'stdout redirected to a file' ($r.Code -eq 0 -and (Same $d (Bytes 'out.bin'))) $r.Err
    $r = Run "`"$DD`" if=a.bin | `"$DD`" of=chain.bin"
    Check 'dd | dd' (Same $d (Bytes 'chain.bin')) $r.Err
    $r = Run "`"$DD`" < a.bin of=redir.bin skip=10"
    Check 'redirected stdin is seekable' (Same (Slice $d 5120 4880) (Bytes 'redir.bin')) $r.Err
    $null = Run "`"$DD`" if=a.bin seek=1 2>seekerr.txt | more >nul"
    $e = [IO.File]::ReadAllText((F 'seekerr.txt'))
    Check 'seek on a pipe fails' ($e -match "'standard output': cannot seek") $e

    # --- status levels ----------------------------------------------------
    $r = DDRun 'if=a.bin of=st.bin status=none'
    Check 'status=none is silent' ($r.Code -eq 0 -and $r.Err -eq '') $r.Err
    $r = DDRun 'if=a.bin of=st.bin status=noxfer'
    Check 'status=noxfer' ($r.Err -match 'records out' -and $r.Err -notmatch 'copied') $r.Err
    $r = DDRun 'if=a.bin of=st.bin status=loud'
    Check 'bad status level' ($r.Code -eq 1 -and $r.Err -match "invalid status level: 'loud'") $r.Err

    # --- errors and exit codes ----------------------------------------------
    $r = DDRun 'if=missing.bin of=x.bin'
    Check 'missing input exits 1' ($r.Code -eq 1 -and $r.Err -match "failed to open 'missing.bin'" -and $r.Err -notmatch 'records') $r.Err
    $r = DDRun 'foo=bar'
    Check 'unknown operand' ($r.Code -eq 1 -and $r.Err -match "unrecognized operand 'foo=bar'" -and $r.Err -match "Try 'dd --help'") $r.Err
    $r = DDRun 'iflag=append'
    Check 'output-only flag on input' ($r.Code -eq 1 -and $r.Err -match "invalid input flag: 'append'") $r.Err
    $r = DDRun '--bogus'
    Check 'unknown option' ($r.Code -eq 1 -and $r.Err -match "unrecognized option '--bogus'") $r.Err

    # --- filters ----------------------------------------------------------
    $r = DDRun 'if=k.bin of=f1.bin --filter=removable'
    Check 'filter refuses a file' ($r.Code -eq 1 -and $r.Err -match 'refusing' -and -not (Test-Path (F 'f1.bin'))) $r.Err
    $r = DDRun 'if=k.bin of=f2.bin --filter=file'
    Check 'filter=file allows a file' ($r.Code -eq 0 -and (Test-Path (F 'f2.bin'))) $r.Err
    $r = DDRun 'if=k.bin of=\\.\PhysicalDrive0 --filter=file'
    Check 'filter=file refuses a disk' ($r.Code -eq 1 -and $r.Err -match 'refusing') $r.Err
    Copy-Item $DD (F 'dd-removable.exe')
    $r = Run "`"$(F 'dd-removable.exe')`" if=k.bin of=f3.bin"
    Check 'dd-removable.exe implies the filter' ($r.Code -eq 1 -and $r.Err -match 'filter=removable') $r.Err

    # --- --lock-input (the device side is in device-tests) ------------------
    $r = DDRun 'if=k.bin of=lk.bin --lock-input'
    Check '--lock-input on a file warns and copies' ($r.Code -eq 0 -and $r.Err -match 'nothing to lock' -and (Test-Path (F 'lk.bin'))) $r.Err
    $r = DDRun 'if=k.bin --verify-hashes --lock-input'
    Check '--lock-input is rejected with --verify-hashes' ($r.Code -eq 1 -and $r.Err -match 'cannot be combined') $r.Err

    # --- informational ----------------------------------------------------
    $r = Run "`"$DD`" --version > ver.txt"
    $ver = [regex]::Match((Get-Content (Join-Path $PSScriptRoot '..\include\dd_version.h') -Raw), '#define DD_VERSION_STR\s+"([^"]+)"').Groups[1].Value
    Check '--version' ($r.Code -eq 0 -and $ver -and (Get-Content (F 'ver.txt') -Raw) -match "dd reimagined for Windows\) $([regex]::Escape($ver))")
    $r = Run "`"$DD`" --help > help.txt"
    Check '--help' ($r.Code -eq 0 -and (Get-Content (F 'help.txt') -Raw) -match 'Usage: dd')
    $r = Run "`"$DD`" --list > list.txt"
    $list = Get-Content (F 'list.txt') -Raw
    Check '--list shows disks and volumes' ($r.Code -eq 0 -and $list -match 'PhysicalDrive\d' -and $list -match 'Volume\{') $list
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "$($script:count - $script:failures) of $script:count passed"
exit [int]($script:failures -ne 0)
