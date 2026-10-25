# Device changes while dd-gui.exe's "Overwrite disk" box is open.  Needs an
# elevated prompt.  Uses its own small VHDXs only.
#   powershell -ExecutionPolicy Bypass -File tests\gui-race-test.ps1
param(
    [string]$Gui = (Join-Path $PSScriptRoot '..\build\dd-gui.exe'),
    [string]$DD = (Join-Path $PSScriptRoot '..\build\dd.exe')
)

$ErrorActionPreference = 'Stop'
$Gui = (Resolve-Path $Gui).Path
$DD = (Resolve-Path $DD).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("ddgui-" + [Guid]::NewGuid())
New-Item -ItemType Directory $work | Out-Null
$script:failures = 0
$script:count = 0

Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
  [DllImport("user32")] public static extern IntPtr GetDlgItem(IntPtr dlg, int id);
  [DllImport("user32")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  public static string Text(IntPtr h) {
    int n = (int)SendMessageW(h, 0x0E, IntPtr.Zero, IntPtr.Zero);
    var sb = new StringBuilder(n + 1); SendMessageW(h, 0x0D, (IntPtr)(n + 1), sb); return sb.ToString();
  }
  public static string Item(IntPtr cb, int i) {
    int n = (int)SendMessageW(cb, 0x149, (IntPtr)i, IntPtr.Zero);
    var sb = new StringBuilder(n + 1); SendMessageW(cb, 0x148, (IntPtr)i, sb); return sb.ToString();
  }
}
'@

$IDC_DISK2 = 1031; $IDC_SHOWFIXED = 1005; $IDC_SRCFILE = 1029; $IDC_START = 1014
$IDC_INPUT = 1035; $IDC_OUTPUT = 1036
$IDC_ETA = 1012; $IDC_LOG = 1013

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
    $s = Join-Path $work 'dp.txt'
    Set-Content -Encoding ASCII $s $lines
    diskpart.exe /s $s | Out-Null
}
function Vhd([string]$name) { Join-Path $work "$name.vhdx" }
function Attach([string]$name) {
    Diskpart @("select vdisk file=`"$(Vhd $name)`"", 'attach vdisk')
    (Get-DiskImage -ImagePath (Vhd $name)).Number
}
function Detach([string]$name) { Diskpart @("select vdisk file=`"$(Vhd $name)`"", 'detach vdisk') }

function Wait-For([scriptblock]$cond, [int]$ms = 15000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $ms) {
        $v = & $cond
        if ($v) { return $v }
        Start-Sleep -Milliseconds 200
    }
    $null
}

# First MiB of a disk, as a hash (read only).
function Head([int]$n) {
    $f = Join-Path $work "head$n.bin"
    cmd /c "`"$DD`" if=\\.\PhysicalDrive$n of=`"$f`" bs=1M count=1 status=none" | Out-Null
    (Get-FileHash $f).Hash
}

# The Input list: 0 Disk, 1 Image file, 2 Zeros, 3 Random data.  The
# Output list: 0 Disk, 1 Image file, 2 Nothing.  Chosen as a user would.
function Set-List([IntPtr]$dlg, [int]$id, [int]$index) {
    $cb = [W]::GetDlgItem($dlg, $id)
    [W]::SendMessageW($cb, 0x14E, [IntPtr]$index, [IntPtr]::Zero) | Out-Null          # CB_SETCURSEL
    [W]::SendMessageW($dlg, 0x111, [IntPtr]($id -bor (1 -shl 16)), $cb) | Out-Null     # CBN_SELCHANGE
}

function Select-Disk([IntPtr]$dlg, [int]$number) {
    $cb = [W]::GetDlgItem($dlg, $IDC_DISK2)
    $count = [int][W]::SendMessageW($cb, 0x146, [IntPtr]::Zero, [IntPtr]::Zero)
    for ($i = 0; $i -lt $count; $i++) {
        if ([W]::Item($cb, $i) -like "Disk ${number}:*") {
            [W]::SendMessageW($cb, 0x14E, [IntPtr]$i, [IntPtr]::Zero) | Out-Null
            return [W]::Item($cb, $i)
        }
    }
    $null
}

# Clicks Start, runs $during while the Overwrite disk box is open, clicks Yes,
# and returns the box text and the log once the copy has finished.  When the
# copy is not started, Refused holds the error box's text (the box is closed).
function Start-Confirm([IntPtr]$dlg, [scriptblock]$during) {
    [W]::PostMessageW($dlg, 0x111, [IntPtr]$IDC_START, [IntPtr]::Zero) | Out-Null
    $box = Wait-For { $h = [W]::FindWindowW('#32770', 'Overwrite disk'); if ($h -ne [IntPtr]::Zero) { $h } }
    if (-not $box) { return [pscustomobject]@{ Box = ''; Log = 'no confirmation box'; Eta = ''; Refused = '' } }
    $text = [W]::Text([W]::GetDlgItem($box, 0xFFFF))
    & $during
    [W]::PostMessageW($box, 0x111, [IntPtr]6, [IntPtr]::Zero) | Out-Null    # IDYES
    Wait-For { [W]::FindWindowW('#32770', 'Overwrite disk') -eq [IntPtr]::Zero } 5000 | Out-Null
    $eta = [W]::GetDlgItem($dlg, $IDC_ETA)
    $refused = ''
    # the error box has the main window's title; it is the window's enabled popup
    $done = Wait-For {
        $t = [W]::Text($eta)
        if ($t -match '^(Finished|Failed|Stopped)') { return $t }
        $p = [W]::GetWindow($dlg, 6)                                     # GW_ENABLEDPOPUP
        if ($p -ne [IntPtr]::Zero -and $p -ne $dlg) {
            $script:refusedText = [W]::Text([W]::GetDlgItem($p, 0xFFFF))
            [W]::PostMessageW($p, 0x111, [IntPtr]1, [IntPtr]::Zero) | Out-Null    # IDOK
            return 'refused'
        }
    } 30000
    if ($done -eq 'refused') { $refused = $script:refusedText; $done = '' }
    [pscustomobject]@{ Box = $text; Log = [W]::Text([W]::GetDlgItem($dlg, $IDC_LOG)); Eta = "$done"; Refused = $refused }
}

$proc = $null
try {
    foreach ($v in 'A', 'B', 'C') { Diskpart @("create vdisk file=`"$(Vhd $v)`" maximum=16 type=fixed") }
    $img = New-Object byte[] 1048576
    (New-Object Random 5).NextBytes($img)
    $imgFile = Join-Path $work 'image.img'
    [IO.File]::WriteAllBytes($imgFile, $img)
    $imgHash = (Get-FileHash $imgFile).Hash

    $others = @(Get-Disk | Where-Object { $_.BusType -ne 'File Backed Virtual' -and -not $_.IsBoot } | ForEach-Object Number)
    $before = @{}
    foreach ($o in $others) { $before[$o] = Head $o }

    $a = Attach 'A'
    $proc = Start-Process $Gui -PassThru
    $dlg = Wait-For { $proc.Refresh(); if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $proc.MainWindowHandle } }
    if (-not $dlg) { throw 'dd-gui window not found' }
    # The dialog is shown once WM_INITDIALOG is done; a click before then
    # can be undone by its setup, which selects read mode.
    if (-not (Wait-For { if ([W]::IsWindowVisible($dlg)) { $true } })) { throw 'dd-gui window never shown' }
    # the window opens in read mode; Show fixed disks is enabled only for writing
    Set-List $dlg $IDC_INPUT 1                                          # an image file
    Set-List $dlg $IDC_OUTPUT 0                                         # to a disk
    [W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_SHOWFIXED), 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    [W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_SRCFILE), 0x0C, [IntPtr]::Zero, $imgFile) | Out-Null

    # 1: the selected disk goes away and another one takes its number
    $label = Select-Disk $dlg $a
    Check 'test disk A is listed' ($null -ne $label) "disk $a"
    $script:bnum = -1
    $r = Start-Confirm $dlg {
        Detach 'A'
        $script:bnum = Attach 'B'
        Start-Sleep -Milliseconds 2500       # let the device-change refresh fire
    }
    Write-Host "     A was disk $a, B is disk $script:bnum"
    Check 'B took the number A had' ($script:bnum -eq $a)
    Check 'copy to a replaced disk is not started' ($r.Refused -like 'A disk was inserted, removed or changed*' -and
        $r.Log -match 'Not started: a disk changed') "$($r.Eta) / $($r.Refused) / $($r.Log)"
    Check 'replacement disk B is untouched' ((Head $script:bnum) -ne $imgHash)

    # 2: another disk arrives while the box is open; the copy still goes to
    #    the disk that was confirmed
    $label = Select-Disk $dlg $script:bnum
    $r = Start-Confirm $dlg {
        $script:cnum = Attach 'C'
        Start-Sleep -Milliseconds 2500
    }
    Check 'copy goes to the confirmed disk' ($r.Eta -like 'Finished*' -and (Head $script:bnum) -eq $imgHash) "$($r.Eta) / $($r.Log)"
    Check 'the disk that arrived is untouched' ((Head $script:cnum) -ne $imgHash)

    foreach ($o in $others) {
        Check "real disk $o is untouched" ((Head $o) -eq $before[$o])
    }
} finally {
    if ($proc -and -not $proc.HasExited) { $proc.Kill() }
    foreach ($v in 'A', 'B', 'C') { try { Detach $v } catch { } }
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "$($script:count - $script:failures) of $script:count passed"
exit [int]($script:failures -ne 0)
