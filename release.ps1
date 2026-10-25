# Makes a release: a clean build, the file-only test suites, then in dist\
#   dd-reimagined-<ver>-win-x64.zip   dd.exe, dd-gui.exe, README, CHANGES, licences
#   dd-reimagined-<ver>-src.zip       the sources (without original\ and build\)
#   SHA256SUMS                        both zips, in sha256sum's format
# .github\workflows\release.yml runs this when a GitHub release is published
# and attaches dist\ to it; run it locally only to try a release.
# The version comes from include\dd_version.h.  The device and GUI suites
# need an elevated prompt, so they are not run here: run tests\run-all.ps1
# elevated before publishing.
#   powershell -ExecutionPolicy Bypass -File release.ps1 [-SkipTests]
param([switch]$SkipTests)

# native programs report through exit codes; their stderr is not an error
$ErrorActionPreference = 'Continue'
$PSDefaultParameterValues['*-Item:ErrorAction'] = 'Stop'
$root = $PSScriptRoot
Set-Location $root

function Fail([string]$why) { Write-Host "release: $why" -ForegroundColor Red; exit 1 }

# An editor's indexer or a virus scanner may still have a just-copied file
# open; retry for a while rather than fail.
function Remove-Retrying([string]$path) {
    for ($i = 1; ; $i++) {
        try { Remove-Item -Recurse -Force $path -ErrorAction Stop; return }
        catch { if ($i -ge 20) { throw }; Start-Sleep -Milliseconds 500 }
    }
}

$h = Get-Content (Join-Path $root 'include\dd_version.h') -Raw
if ($h -notmatch '#define DD_VERSION_STR\s+"([^"]+)"') { Fail 'no DD_VERSION_STR in include\dd_version.h' }
$ver = $Matches[1]
if ((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw) -notmatch "VERSION $([regex]::Escape($ver)) LANGUAGES") {
    Fail "CMakeLists.txt's project() version is not $ver"
}
if ((Get-Content (Join-Path $root 'CHANGES.md') -Raw) -notmatch "(?m)^## $([regex]::Escape($ver))\b") {
    Fail "CHANGES.md has no '## $ver' section"
}
if ((Get-Content (Join-Path $root 'README.md') -Raw) -notmatch "(?m)^Version $([regex]::Escape($ver))\.") {
    Fail "README.md does not start 'Version $ver.'"
}
if ((Get-Content (Join-Path $root 'gui\dd-gui.manifest') -Raw) -notmatch "name=`"ddreimagined.gui`" version=`"$([regex]::Escape($ver))\.0`"") {
    Fail "gui\dd-gui.manifest's assemblyIdentity version is not $ver.0"
}
Write-Host "release $ver"

# clean build, liblzma included
if (Test-Path build) { Remove-Item -Recurse -Force build }
cmd /c "`"$root\build.cmd`""
if ($LASTEXITCODE -ne 0) { Fail 'build failed' }
$v = & "$root\build\dd.exe" --version
if (($v -join "`n") -notmatch "\) $([regex]::Escape($ver))") { Fail "dd --version says: $($v[0])" }

if (-not $SkipTests) {
    foreach ($t in 'run-tests', 'xz-tests', 'hash-tests', 'sparse-tests') {
        $o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tests\$t.ps1")
        $skip = $o | Select-String '^skipped: ' | Select-Object -First 1
        if ($skip) { Write-Host ("{0,-11} {1}" -f $t, $skip.Line); continue }
        $line = ($o | Select-String '(\d+) of (\d+) passed$' | Select-Object -Last 1)
        if (-not $line -or $line.Matches[0].Groups[1].Value -ne $line.Matches[0].Groups[2].Value) {
            $o | Select-String '^FAIL' -Context 0, 3 | ForEach-Object { Write-Host "$_" }
            Fail "$t did not pass: $line"
        }
        Write-Host ("{0,-11} {1}" -f $t, $line.Line)
    }
}

$dist = Join-Path $root 'dist'
$bin = "dd-reimagined-$ver-win-x64"
$src = "dd-reimagined-$ver-src"
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory "$dist\$bin", "$dist\$src" | Out-Null

Copy-Item build\dd.exe, build\dd-gui.exe, README.md, CHANGES.md, COPYING.txt "$dist\$bin"
Copy-Item third_party\xz\COPYING.0BSD "$dist\$bin\LICENSE-liblzma.txt"
# the documents README.md links to, with their screenshots
New-Item -ItemType Directory "$dist\$bin\docs" | Out-Null
Copy-Item docs\*.md "$dist\$bin\docs"
Copy-Item -Recurse docs\images "$dist\$bin\docs\images"

Copy-Item CMakeLists.txt, build.cmd, release.ps1, clean.ps1, README.md, CHANGES.md, COPYING.txt, DEVELOPMENT.md "$dist\$src"
foreach ($d in 'cli', 'docs', 'gui', 'include', 'lib', 'tests', 'third_party') { Copy-Item -Recurse $d "$dist\$src\$d" }
# the test fixtures are made by tests\regenerate-fixtures.ps1, not shipped
if (Test-Path "$dist\$src\tests\fixtures") { Remove-Retrying "$dist\$src\tests\fixtures" }

foreach ($n in $bin, $src) {
    tar.exe -a -c -f "$dist\$n.zip" -C $dist $n
    if ($LASTEXITCODE -ne 0) { Fail "could not make $n.zip" }
    Remove-Retrying "$dist\$n"
}

$sums = foreach ($n in $bin, $src) {
    '{0}  {1}.zip' -f (Get-FileHash "$dist\$n.zip" -Algorithm SHA256).Hash.ToLower(), $n
}
[IO.File]::WriteAllText("$dist\SHA256SUMS", ($sums -join "`n") + "`n")

Get-ChildItem $dist | ForEach-Object { Write-Host ("{0,-40} {1,12:N0}" -f $_.Name, $_.Length) }
Write-Host 'Before publishing, run tests\run-all.ps1 from an elevated prompt.'
