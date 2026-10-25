# Removes what building and testing leave behind:
#   build\ and dist\
#   tests\fixtures, made by tests\regenerate-fixtures.ps1 (the sparse suites
#   skip without them)
#   test work folders in %TEMP% (dd<suite>-<GUID>) that a stopped or killed
#   suite did not delete, detaching any of their VHDXs still attached
# Sources and original\ are never touched, and only VHDXs
# inside those work folders are detached.  Detaching needs an elevated
# prompt; without one, a folder whose VHDX is attached is left, and said so.
#   powershell -ExecutionPolicy Bypass -File clean.ps1 [-WhatIf]
[CmdletBinding(SupportsShouldProcess)]
param()

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
# loaded now, or -WhatIf lists the aliases the Storage module defines
$whatIf = $WhatIfPreference; $WhatIfPreference = $false
Import-Module Storage
$WhatIfPreference = $whatIf

function Remove-Leftover {
    [CmdletBinding(SupportsShouldProcess)]
    param([string]$path)
    if (-not $PSCmdlet.ShouldProcess($path, 'Remove')) { return }
    try { Remove-Item -Recurse -Force $path; Write-Host "removed $path" }
    catch { Write-Host "could not remove ${path}: $($_.Exception.Message)" -ForegroundColor Yellow }
}

foreach ($d in 'build', 'dist', 'tests\fixtures') {
    $p = Join-Path $root $d
    if (Test-Path $p) { Remove-Leftover $p }
}

# the prefixes the suites, fixture generators and docs\make-screenshots.ps1
# use (see their $work)
$pattern = '^dd(ctrlc|dev|fit|guifit|gui|hash|ntfs|fix|test|sparsedev|sparse|shots|xz)-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$'
$temp = [IO.Path]::GetTempPath()
foreach ($w in @(Get-ChildItem -Directory $temp | Where-Object { $_.Name -match $pattern })) {
    $keep = $false
    foreach ($v in @(Get-ChildItem -File -Recurse $w.FullName -Include *.vhd, *.vhdx -ErrorAction SilentlyContinue)) {
        $img = Get-DiskImage -ImagePath $v.FullName -ErrorAction SilentlyContinue
        if (-not ($img -and $img.Attached)) { continue }
        if (-not $elevated) {
            Write-Host "left $($w.FullName): $($v.Name) is attached; run elevated to detach it" -ForegroundColor Yellow
            $keep = $true
        } elseif ($PSCmdlet.ShouldProcess($v.FullName, 'Detach')) {
            Dismount-DiskImage -ImagePath $v.FullName | Out-Null
            Write-Host "detached $($v.FullName)"
        }
    }
    if (-not $keep) { Remove-Leftover $w.FullName }
}
