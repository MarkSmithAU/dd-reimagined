# Runs every test suite and prints one summary line each, plus any failures.
# The device and GUI suites need an elevated prompt.
#   powershell -ExecutionPolicy Bypass -File tests\run-all.ps1 [-Log file] [-Screenshot gui.png]
param(
    [string]$Log = '',
    [string]$Screenshot = ''
)

function Out([string]$line) {
    Write-Host $line
    if ($Log) { $line | Out-File -Append -Encoding utf8 $Log }
}

if ($Log) { Remove-Item $Log -ErrorAction SilentlyContinue }
Out ("disks before: " + ((Get-Disk | ForEach-Object { "$($_.Number) $($_.FriendlyName)" }) -join '; '))
foreach ($t in 'run-tests', 'device-tests', 'ctrlc-test', 'gui-race-test', 'fit-tests', 'xz-tests', 'hash-tests', 'gui-fit-test',
               'sparse-tests', 'sparse-device-tests') {
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot "$t.ps1"))
    if ($t -eq 'gui-fit-test' -and $Screenshot) { $a += @('-Screenshot', $Screenshot) }
    $o = & powershell @a 2>&1
    Out ("{0,-20} {1}" -f $t, (($o | Select-String 'passed$|^skipped: ') -join ''))
    $o | Select-String '^FAIL' -Context 0, 3 | ForEach-Object { Out "$_" }
}
Out ("disks after: " + ((Get-Disk).Number -join ','))
