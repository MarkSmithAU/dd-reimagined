# Ctrl+C test for dd.exe: runs dd in its own console, sends a real Ctrl+C
# to that console and checks for exit code 130 and printed statistics.
#   powershell -ExecutionPolicy Bypass -File tests\ctrlc-test.ps1 [-DD build\dd.exe]
param(
    [string]$DD = (Join-Path $PSScriptRoot '..\build\dd.exe')
)

$DD = (Resolve-Path $DD).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("ddctrlc-" + [Guid]::NewGuid())
New-Item -ItemType Directory $work | Out-Null
$script:failures = 0
$script:count = 0

# Attaches to the console of process $args[0] and sends Ctrl+C to it.
$sender = @'
Add-Type @"
using System.Runtime.InteropServices;
public static class C {
  [DllImport("kernel32")] public static extern bool FreeConsole();
  [DllImport("kernel32")] public static extern bool AttachConsole(uint pid);
  [DllImport("kernel32")] public static extern bool SetConsoleCtrlHandler(System.IntPtr h, bool add);
  [DllImport("kernel32")] public static extern bool GenerateConsoleCtrlEvent(uint ev, uint group);
}
"@
[C]::FreeConsole() | Out-Null
if (-not [C]::AttachConsole([uint32]$args[0])) { exit 2 }
[C]::SetConsoleCtrlHandler([IntPtr]::Zero, $true) | Out-Null
if (-not [C]::GenerateConsoleCtrlEvent(0, 0)) { exit 3 }
exit 0
'@
Set-Content -Encoding ASCII (Join-Path $work 'send.ps1') $sender

function Check([string]$what, [bool]$ok, [string]$detail = '') {
    $script:count++
    if ($ok) { Write-Host "ok   $what" }
    else {
        $script:failures++
        Write-Host "FAIL $what" -ForegroundColor Red
        if ($detail) { Write-Host "     $detail" }
    }
}

# Runs $cmdline under cmd in a new hidden console, waits, sends Ctrl+C to
# the console, and returns dd's exit code (from cmd) and stderr.
function Interrupt([string]$cmdline, [int]$waitMs = 1500) {
    $err = Join-Path $work 'err.txt'
    $rc = Join-Path $work 'rc.txt'
    Remove-Item $err, $rc -ErrorAction SilentlyContinue
    $full = "$cmdline 2>`"$err`" & call echo %^errorlevel%>`"$rc`""
    $p = Start-Process cmd -ArgumentList "/d /s /c `"$full`"" -WindowStyle Hidden -PassThru -WorkingDirectory $work
    Start-Sleep -Milliseconds $waitMs
    $s = Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        (Join-Path $work 'send.ps1'), $p.Id -WindowStyle Hidden -PassThru -Wait
    $done = $p.WaitForExit(10000)
    if (-not $done) { $p.Kill() }
    [pscustomobject]@{
        Sent   = $s.ExitCode
        Exited = $done
        Code   = if (Test-Path $rc) { (Get-Content $rc -Raw).Trim() } else { '' }
        Err    = if (Test-Path $err) { (Get-Content $err -Raw) -replace "`r", '' } else { '' }
    }
}

try {
    $r = Interrupt "`"$DD`" if=/dev/zero of=nul bs=1M status=progress"
    $detail = "sent=$($r.Sent) exited=$($r.Exited) code=$($r.Code) err=$($r.Err)"
    Check 'Ctrl+C stops a copy' ($r.Sent -eq 0 -and $r.Exited) $detail
    Check 'Ctrl+C exits 130' ($r.Code -eq '130') $detail
    Check 'Ctrl+C prints statistics' ($r.Err -match '\d+\+\d+ records in' -and $r.Err -match 'records out') $detail

    # The writer ignores Ctrl+C and writes nothing, so dd sits blocked in a
    # pipe read until CancelSynchronousIo frees it.
    $r = Interrupt "timeout /t 30 /nobreak >nul | `"$DD`" of=nul"
    $detail = "sent=$($r.Sent) exited=$($r.Exited) code=$($r.Code) err=$($r.Err)"
    Check 'Ctrl+C unblocks a pipe read' ($r.Exited) $detail
    Check 'blocked pipe read exits 130' ($r.Code -eq '130') $detail
} finally {
    Get-Process timeout -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "$($script:count - $script:failures) of $script:count passed"
exit [int]($script:failures -ne 0)
