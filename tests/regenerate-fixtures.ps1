# Rebuilds every binary test fixture in tests\fixtures from scratch, on a
# bare Windows machine with the Visual Studio C++ build tools:
#   1. builds dd.exe (build.cmd);
#   2. downloads Cygwin's installer, checks its signature, and installs
#      e2fsprogs (mke2fs, dumpe2fs, debugfs) into a private folder, without
#      touching the registry or any existing Cygwin;
#   3. makes the ext images (make-sparse-fixtures.ps1) and the NTFS images
#      (make-ntfs-fixtures.ps1, on a VHDX Windows formats) in a staging folder;
#   4. checks them: sparse-tests and sparse-device-tests must pass against
#      them, and none may hold this machine's name, user name, profile path or
#      SID (machine-sid.ps1 puts a placeholder in the NTFS images);
#   5. puts them in tests\fixtures (replacing any there), saying which changed;
#   6. deletes everything it downloaded or made on the way, whatever happens,
#      including the record cygwin1.dll leaves of its folder in the registry.
# Needs an elevated prompt (the NTFS images and sparse-device-tests use a
# VHDX) and about 1 GB free in %TEMP%.  The ext images record when they were
# made and a random UUID, so they always differ from the old ones.
#   powershell -ExecutionPolicy Bypass -File tests\regenerate-fixtures.ps1 [-Site <Cygwin mirror>] [-NoReplace] [-KeepWork]
param(
    [string]$Site = 'https://mirrors.kernel.org/sourceware/cygwin/',
    [switch]$NoReplace,             # make and check them, but leave tests\fixtures alone
    [switch]$KeepWork               # keep the work folder (Cygwin, staging), for a look afterwards
)

# native programs report through exit codes, and some write notes to stderr
# (vcvars64.bat: "'vswhere.exe' is not recognized"), which is not an error;
# PowerShell's own steps that must not fail say -ErrorAction Stop
$ErrorActionPreference = 'Continue'
$PSDefaultParameterValues['*-Item:ErrorAction'] = 'Stop'
$PSDefaultParameterValues['Invoke-WebRequest:ErrorAction'] = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$fixtures = Join-Path $PSScriptRoot 'fixtures'
$work = Join-Path ([IO.Path]::GetTempPath()) ("ddregen-" + [Guid]::NewGuid())
$stage = Join-Path $work 'fixtures'
$cygwin = Join-Path $work 'cygwin'
# e2fsprogs that made the ext fixtures; another version still works, but
# may lay the images out differently
$E2fsVersion = '1.44.5'

function Step([string]$what) { Write-Host ""; Write-Host "== $what" -ForegroundColor Cyan }
function Fail([string]$why) { throw "regenerate-fixtures: $why" }
# runs a test suite and fails unless every check passed
function Suite([string]$name, [string[]]$more) {
    $o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "$name.ps1") @more 2>&1 | ForEach-Object { "$_" }
    $line = $o | Select-String '(\d+) of (\d+) passed$' | Select-Object -Last 1
    if (-not $line -or $line.Matches[0].Groups[1].Value -ne $line.Matches[0].Groups[2].Value) {
        $o | Select-String '^FAIL' -Context 0, 3 | ForEach-Object { Write-Host "$_" }
        Fail "$name did not pass against the new fixtures: $line"
    }
    Write-Host ("{0,-20} {1}" -f $name, $line.Line)
}
# Deletes a folder that may hold read-only files (Cygwin's) and long paths
function Remove-Tree([string]$path) {
    if (-not (Test-Path $path)) { return }
    cmd /c "attrib -r -s -h `"$path\*`" /s /d >nul 2>nul & rd /s /q `"\\?\$path`" 2>nul"
    if (Test-Path $path) { Remove-Item -Recurse -Force $path -ErrorAction SilentlyContinue }
    if (Test-Path $path) { Write-Warning "could not delete all of $path; delete it by hand" }
}

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail 'needs an elevated prompt: the NTFS images and sparse-device-tests use a VHDX'
}
$drive = (Get-Item ([IO.Path]::GetTempPath())).PSDrive
if ($drive.Free -lt 1GB) { Fail ("needs about 1 GB free in {0}; {1:N0} MB is" -f [IO.Path]::GetTempPath(), ($drive.Free / 1MB)) }

# cygwin1.dll records every Cygwin root it runs from under this key (setup's
# --no-write-registry does not stop it); what this run adds is removed again
$cygKey = 'HKLM:\SOFTWARE\Cygwin'
$cygKeyWas = Test-Path $cygKey
function Remove-CygwinRecords {
    $inst = "$cygKey\Installations"
    if (Test-Path $inst) {
        $k = Get-Item $inst
        foreach ($n in $k.GetValueNames()) {
            if ("$($k.GetValue($n))" -like "*$work*") { Remove-ItemProperty -Path $inst -Name $n }
        }
        if (-not $cygKeyWas -and -not (Get-Item $inst).GetValueNames()) { Remove-Item $inst }
    }
    if (-not $cygKeyWas -and (Test-Path $cygKey) -and -not (Get-ChildItem $cygKey) -and -not (Get-Item $cygKey).GetValueNames()) {
        Remove-Item $cygKey
    }
}

New-Item -ItemType Directory $work, $stage | Out-Null
try {
    Step 'Building dd.exe'
    $log = Join-Path $work 'build.log'
    cmd /c "`"$root\build.cmd`" >`"$log`" 2>&1"
    $built = $LASTEXITCODE
    Get-Content $log | Select-String -Pattern ': (fatal )?(error|warning) ' | ForEach-Object { Write-Host "$_" }
    if ($built -ne 0) {
        Get-Content $log -Tail 15 | ForEach-Object { Write-Host $_ }
        Fail 'build.cmd failed (it needs the Visual Studio C++ build tools)'
    }
    $dd = Join-Path $root 'build\dd.exe'
    Write-Host (& $dd --version | Select-Object -First 1)

    Step "Installing e2fsprogs from Cygwin ($Site)"
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $setup = Join-Path $work 'setup-x86_64.exe'
    Invoke-WebRequest 'https://cygwin.com/setup-x86_64.exe' -OutFile $setup -UseBasicParsing
    $sig = Get-AuthenticodeSignature $setup
    if ($sig.Status -ne 'Valid') { Fail "Cygwin's installer is not validly signed: $($sig.Status) $($sig.StatusMessage)" }
    Write-Host "installer signed by $($sig.SignerCertificate.Subject)"
    # setup verifies the package list's signature itself
    $p = Start-Process $setup -Wait -PassThru -WindowStyle Hidden -ArgumentList @(
        '--quiet-mode', '--no-admin', '--no-shortcuts', '--no-write-registry', '--no-version-check',
        '--only-site', '--site', $Site, '--root', "`"$cygwin`"", '--local-package-dir', "`"$work\packages`"",
        '--packages', 'e2fsprogs')
    $e2fs = Join-Path $cygwin 'usr\sbin'
    foreach ($t in 'mke2fs', 'dumpe2fs', 'debugfs') {
        if (-not (Test-Path (Join-Path $e2fs "$t.exe"))) { Fail "Cygwin's installer (exit code $($p.ExitCode)) did not install $t.exe" }
    }
    $env:PATH = "$cygwin\bin;$env:PATH"
    $v = (& (Join-Path $e2fs 'mke2fs.exe') -V 2>&1 | ForEach-Object { "$_" }) -join ' '
    $got = [regex]::Match($v, 'mke2fs ([\d.]+)').Groups[1].Value
    Write-Host "e2fsprogs $got"
    if ($got -ne $E2fsVersion) { Write-Warning "the fixtures were made with e2fsprogs $E2fsVersion; $got may lay them out differently" }

    Step 'Making the ext images'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'make-sparse-fixtures.ps1') `
        -E2fs $e2fs -Bin "$cygwin\bin" -DD $dd -Out $stage
    if ($LASTEXITCODE) { Fail 'make-sparse-fixtures.ps1 failed' }

    Step 'Making the NTFS images'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'make-ntfs-fixtures.ps1') -DD $dd -Out $stage
    if ($LASTEXITCODE) { Fail 'make-ntfs-fixtures.ps1 failed' }

    # every fixture there is now must have been made again
    $old = @(if (Test-Path $fixtures) { Get-ChildItem $fixtures -File | Where-Object { $_.Name -notlike '*.hashes' } | ForEach-Object Name })
    $new = @(Get-ChildItem $stage -File | Where-Object { $_.Name -notlike '*.hashes' } | ForEach-Object Name)
    $missing = @($old | Where-Object { $new -notcontains $_ })
    if ($missing) { Fail "not made again: $($missing -join ', ')" }

    Step 'Checking them'
    Suite 'sparse-tests' @('-DD', $dd, '-Fixtures', $stage)
    Suite 'sparse-device-tests' @('-DD', $dd, '-Fixtures', $stage)

    # nothing of this machine or its user in them, compressed or not
    . (Join-Path $PSScriptRoot 'machine-sid.ps1')
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $needles = @($env:COMPUTERNAME, [Environment]::UserDomainName, $env:USERPROFILE, (Split-Path $env:USERPROFILE -Leaf)) |
        Where-Object { $_ -and $_.Length -ge 3 } | Select-Object -Unique
    $sids = @(Get-LocalSids | ForEach-Object { $latin1.GetString((SidParts $_)) })
    $raw = Join-Path $work 'raw'
    New-Item -ItemType Directory $raw | Out-Null
    $leaks = @()
    foreach ($f in Get-ChildItem $stage -File) {
        $path = $f.FullName
        if ($f.Name -like '*.xz') {
            $path = Join-Path $raw ($f.Name -replace '\.xz$', '')
            & $dd "if=$($f.FullName)" "of=$path" bs=1M --no-hashes status=none
            if ($LASTEXITCODE) { Fail "cannot decompress $($f.Name)" }
        }
        $bytes = [IO.File]::ReadAllBytes($path)
        $texts = @($latin1.GetString($bytes), [Text.Encoding]::Unicode.GetString($bytes))
        foreach ($n in $needles) {
            if ($texts | Where-Object { $_.IndexOf($n, [StringComparison]::OrdinalIgnoreCase) -ge 0 }) { $leaks += "$($f.Name): '$n'" }
        }
        foreach ($s in $sids) { if ($texts[0].Contains($s)) { $leaks += "$($f.Name): this machine's SID" } }
        if ($path -ne $f.FullName) { Remove-Item $path }
    }
    # a user name is often too short to search for alone; as a profile path it is not
    if ($leaks) { Fail "the new fixtures hold this machine's details: $($leaks -join '; ')" }
    Write-Host "no machine name, user name, profile path or machine SID in them"

    Step 'Comparing with tests\fixtures'
    foreach ($n in $new) {
        $a = Join-Path $fixtures $n
        $state = if (-not (Test-Path $a)) { 'new' }
                 elseif ((Get-FileHash $a).Hash -eq (Get-FileHash (Join-Path $stage $n)).Hash) { 'unchanged' }
                 else { 'changed' }
        Write-Host ("{0,-24} {1,-10} {2,10:N0} bytes" -f $n, $state, (Get-Item (Join-Path $stage $n)).Length)
    }

    if ($NoReplace) {
        Write-Host ""; Write-Host "-NoReplace: tests\fixtures left as it was"
    } else {
        Step 'Replacing tests\fixtures'
        if (Test-Path $fixtures) { Get-ChildItem $fixtures -File | Remove-Item }
        else { New-Item -ItemType Directory $fixtures | Out-Null }
        Copy-Item (Join-Path $stage '*') $fixtures
        Write-Host "$((Get-ChildItem $fixtures -File).Count) files in $fixtures"
    }
} finally {
    if ($KeepWork) {
        Write-Host ""; Write-Host "work folder kept: $work"
    } else {
        Step 'Cleaning up'
        Remove-Tree $work
        if (-not (Test-Path $work)) { Write-Host "deleted $work" }
        try { Remove-CygwinRecords; Write-Host "removed Cygwin's registry record of it" }
        catch { Write-Warning "could not remove Cygwin's record of $work from $cygKey\Installations: $_" }
    }
}
