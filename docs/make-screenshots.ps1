# Makes the screenshots in docs\images by driving dd-gui.exe through a read,
# a write, a hash check and a wipe.  Needs an elevated prompt.  Uses its own VHDXs
# only, and maps its work folder to a drive letter with subst, so no user
# names or local paths appear in the pictures.
#   powershell -ExecutionPolicy Bypass -File docs\make-screenshots.ps1
param(
    [string]$Gui = (Join-Path $PSScriptRoot '..\build\dd-gui.exe'),
    [string]$Out = (Join-Path $PSScriptRoot 'images')
)

$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error 'make-screenshots.ps1 needs an elevated prompt'
}
$Gui = (Resolve-Path $Gui).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$work = Join-Path ([IO.Path]::GetTempPath()) ("ddshots-" + [Guid]::NewGuid())
New-Item -ItemType Directory $work, "$work\Images" | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
  [DllImport("user32")] public static extern IntPtr GetDlgItem(IntPtr dlg, int id);
  [DllImport("user32", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32")] public static extern bool SetProcessDPIAware();
  [DllImport("user32")] public static extern bool IsWindowVisible(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
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
[W]::SetProcessDPIAware() | Out-Null      # real pixels, as PrintWindow draws them

$IDC_DISK = 1003; $IDC_SHOWFIXED = 1005; $IDC_FILE = 1006; $IDC_ETA = 1012; $IDC_START = 1014
$IDC_LOCKIN = 1024; $IDC_SPARSE = 1025; $IDC_VERIFY = 1022; $IDC_SRCFILE = 1029; $IDC_DISK2 = 1031
$IDC_INPUT = 1035; $IDC_OUTPUT = 1036

function Diskpart([string[]]$lines) {
    $s = Join-Path $work 'dp.txt'
    Set-Content -Encoding ASCII $s $lines
    diskpart.exe /s $s | Out-Null
}
function New-Vhd([string]$name, [int]$mb, [string[]]$more) {
    $path = Join-Path $work "$name.vhdx"
    Diskpart (@("create vdisk file=`"$path`" maximum=$mb type=fixed", "select vdisk file=`"$path`"", 'attach vdisk') + $more)
    (Get-DiskImage -ImagePath $path).Number
}
function Wait-For([scriptblock]$cond, [int]$ms = 15000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $ms) {
        $v = & $cond
        if ($v) { return $v }
        Start-Sleep -Milliseconds 200
    }
    $null
}
# Input: 0 Disk, 1 Image file, 2 Zeros, 3 Random data.  Output: 0 Disk,
# 1 Image file, 2 Nothing (only Disk after Zeros or Random data).
function Set-List([IntPtr]$dlg, [int]$id, [int]$index) {
    $cb = [W]::GetDlgItem($dlg, $id)
    [W]::SendMessageW($cb, 0x14E, [IntPtr]$index, [IntPtr]::Zero) | Out-Null          # CB_SETCURSEL
    [W]::SendMessageW($dlg, 0x111, [IntPtr]($id -bor (1 -shl 16)), $cb) | Out-Null     # CBN_SELCHANGE
    Start-Sleep -Milliseconds 300
}
function Set-IO([IntPtr]$dlg, [int]$in, [int]$out) {
    Set-List $dlg $IDC_INPUT $in
    Set-List $dlg $IDC_OUTPUT $out
}
function Select-Disk([IntPtr]$dlg, [int]$number, [int]$list = $IDC_DISK) {
    $cb = [W]::GetDlgItem($dlg, $list)
    $n = [int][W]::SendMessageW($cb, 0x146, [IntPtr]::Zero, [IntPtr]::Zero)
    for ($i = 0; $i -lt $n; $i++) {
        if ([W]::Item($cb, $i) -like "Disk ${number}:*") {
            [W]::SendMessageW($cb, 0x14E, [IntPtr]$i, [IntPtr]::Zero) | Out-Null
            [W]::SendMessageW($dlg, 0x111, [IntPtr]($list -bor (1 -shl 16)), $cb) | Out-Null
            return
        }
    }
    throw "disk $number is not listed"
}
function Tick([IntPtr]$dlg, [int]$id) {
    $b = [W]::GetDlgItem($dlg, $id)
    if ([int][W]::SendMessageW($b, 0xF0, [IntPtr]::Zero, [IntPtr]::Zero) -eq 0) {     # BM_GETCHECK
        [W]::SendMessageW($b, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null        # BM_CLICK
    }
}
function Set-File([IntPtr]$dlg, [string]$path, [int]$box = $IDC_FILE) {
    [W]::SendMessageW([W]::GetDlgItem($dlg, $box), 0x0C, [IntPtr]::Zero, $path) | Out-Null
    Start-Sleep -Milliseconds 800
}
# every path's folder is masked (tests\screenshot.ps1)
. (Join-Path $PSScriptRoot '..\tests\screenshot.ps1')
function Shot([IntPtr]$h, [string]$name) {
    Save-Shot $h (Join-Path $Out $name)
    Write-Host "  $name"
}
# Clicks Start, answers Yes to a confirmation (shot first if named), and
# waits for the result line.
function Start-Copy([IntPtr]$dlg, [string]$confirmShot = '') {
    $eta = [W]::GetDlgItem($dlg, $IDC_ETA)
    [W]::PostMessageW($dlg, 0x111, [IntPtr]$IDC_START, [IntPtr]::Zero) | Out-Null
    if ($confirmShot) {
        $box = Wait-For { $h = [W]::FindWindowW('#32770', 'Overwrite disk'); if ($h -ne [IntPtr]::Zero) { $h } }
        if (-not $box) { throw 'no confirmation box' }
        Start-Sleep -Milliseconds 300
        Shot $box $confirmShot
        [W]::PostMessageW($box, 0x111, [IntPtr]6, [IntPtr]::Zero) | Out-Null          # IDYES
    }
    Wait-For { [W]::Text($eta) -notmatch '^(Finished|Failed|Stopped)' } 5000 | Out-Null
    $done = Wait-For { $x = [W]::Text($eta); if ($x -match '^(Finished|Failed|Stopped)') { $x } } 120000
    if ("$done" -notlike 'Finished*') { throw "copy did not finish: $done" }
    Start-Sleep -Milliseconds 500
}

# A fresh dd-gui.exe window for each scene, so no log or progress is left over.
function Open-Gui {
    if ($script:proc -and -not $script:proc.HasExited) { $script:proc.Kill(); $script:proc.WaitForExit() }
    $script:proc = Start-Process $Gui -PassThru
    $h = Wait-For { $script:proc.Refresh(); if ($script:proc.MainWindowHandle -ne [IntPtr]::Zero) { $script:proc.MainWindowHandle } }
    if (-not $h) { throw 'dd-gui window not found' }
    # a click during WM_INITDIALOG is undone by its setup
    if (-not (Wait-For { if ([W]::IsWindowVisible($h)) { $true } })) { throw 'dd-gui window never shown' }
    Start-Sleep -Milliseconds 500
    $h
}

# a free drive letter for the work folder, from the end of the alphabet
$used = (Get-PSDrive -PSProvider FileSystem).Name
$drive = [string]((73..89 | ForEach-Object { [char]$_ } | Where-Object { $used -notcontains [string]$_ }) | Select-Object -First 1)
$proc = $null
$vhds = @()
try {
    subst "${drive}:" $work
    if ($LASTEXITCODE -ne 0) { throw "subst ${drive}: failed" }

    # a card: a FAT32 boot partition and an exFAT data partition, a few files
    # on each (a mounted NTFS volume always shows as not cleanly unmounted)
    $card = New-Vhd 'card' 512 @('convert mbr',
        'create partition primary size=128', 'format fs=fat32 quick label=BOOT', 'assign',
        'create partition primary', 'format fs=exfat quick label=DATA', 'assign')
    $vhds += 'card'
    Start-Sleep -Seconds 2
    foreach ($p in Get-Partition -DiskNumber $card | Where-Object DriveLetter) {
        $root = "$($p.DriveLetter):\"
        $rnd = New-Object byte[] (6MB)
        (New-Object Random $p.PartitionNumber).NextBytes($rnd)
        [IO.File]::WriteAllBytes("${root}payload.bin", $rnd)
        Set-Content -Encoding ASCII "${root}readme.txt" 'sample card for the dd-gui.exe screenshots'
    }
    # a blank disk to write to, formatted, so it has a letter to take away
    $target = New-Vhd 'target' 768 @('convert mbr', 'create partition primary',
        'format fs=fat32 quick label=OLD', 'assign')
    $vhds += 'target'
    Start-Sleep -Seconds 2

    # 1: disk to image file, used space only, locked, compressed
    $dlg = Open-Gui
    Set-IO $dlg 0 1
    Select-Disk $dlg $card
    Set-File $dlg "${drive}:\Images\card.img"
    Tick $dlg $IDC_SPARSE
    Tick $dlg $IDC_LOCKIN
    Start-Sleep -Milliseconds 800
    Shot $dlg 'gui-read.png'
    Start-Copy $dlg
    Shot $dlg 'gui-read-done.png'

    # 1b: a disk of random data at xz level 6, caught while what is read runs
    #     ahead of what is compressed: the two-tone progress bar.  Random
    #     data compresses slowly (the card's zeros keep up with the read);
    #     level 6's 24 MiB block per thread lets the read get well ahead
    #     without the first block taking minutes.
    $noise = New-Vhd 'noise' 512 @()
    $vhds += 'noise'
    & (Join-Path (Split-Path $Gui) 'dd.exe') if=/dev/random "of=\\.\PhysicalDrive$noise" bs=1M count=512 status=none
    if ($LASTEXITCODE -ne 0) { throw 'could not fill the noise disk' }
    $dlg = Open-Gui
    Set-IO $dlg 0 1
    Select-Disk $dlg $noise
    Set-File $dlg "${drive}:\Images\noise.img"
    $xz = [W]::GetDlgItem($dlg, 1019)                                   # IDC_XZ
    [W]::SendMessageW($xz, 0x14E, [IntPtr]7, [IntPtr]::Zero) | Out-Null           # xz level 6
    [W]::SendMessageW($dlg, 0x111, [IntPtr](1019 -bor (1 -shl 16)), $xz) | Out-Null
    Start-Sleep -Milliseconds 500
    $eta = [W]::GetDlgItem($dlg, $IDC_ETA)
    [W]::PostMessageW($dlg, 0x111, [IntPtr]$IDC_START, [IntPtr]::Zero) | Out-Null
    # "40% done (75% read)": compressed and read both shown, far enough apart to label
    $caught = Wait-For { $m = [regex]::Match([W]::Text($eta), '^(\d+)% done \((\d+)% read\)')
                         if ($m.Success -and [int]$m.Groups[1].Value -ge 10 -and [int]$m.Groups[2].Value - [int]$m.Groups[1].Value -ge 25) { $true } } 120000
    if (-not $caught) { throw 'the read never ran far enough ahead of the compression' }
    Shot $dlg 'gui-progress.png'
    $done = Wait-For { $x = [W]::Text($eta); if ($x -match '^(Finished|Failed|Stopped)') { $x } } 300000
    if ("$done" -notlike 'Finished*') { throw "copy did not finish: $done" }

    # 2: image file to disk, confirmed, verified
    $dlg = Open-Gui
    Set-IO $dlg 1 0
    [W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_SHOWFIXED), 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 500
    Select-Disk $dlg $target $IDC_DISK2
    Set-File $dlg "${drive}:\Images\card.img.xz" $IDC_SRCFILE
    Tick $dlg $IDC_VERIFY
    Start-Sleep -Milliseconds 800
    Shot $dlg 'gui-write.png'
    Start-Copy $dlg 'gui-confirm.png'
    Shot $dlg 'gui-write-done.png'

    # 3: image file to nothing: its hash file checked
    $dlg = Open-Gui
    Set-IO $dlg 1 2
    Set-File $dlg "${drive}:\Images\card.img.xz" $IDC_SRCFILE
    Start-Copy $dlg
    Shot $dlg 'gui-check.png'

    # 4: zeros to a disk, before Start (the disk list shows only the test disks)
    $dlg = Open-Gui
    Set-List $dlg $IDC_INPUT 2
    [W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_SHOWFIXED), 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 500
    Select-Disk $dlg $target $IDC_DISK2
    Tick $dlg $IDC_VERIFY
    Start-Sleep -Milliseconds 500
    Shot $dlg 'gui-wipe.png'

    # 5: About, from the window's system menu
    [W]::PostMessageW($dlg, 0x112, [IntPtr]0x10, [IntPtr]::Zero) | Out-Null        # WM_SYSCOMMAND, IDM_ABOUT
    $box = Wait-For { $x = [W]::FindWindowW('#32770', 'About dd reimagined for Windows'); if ($x -ne [IntPtr]::Zero) { $x } }
    if (-not $box) { throw 'no About box' }
    Start-Sleep -Milliseconds 300
    Shot $box 'gui-about.png'
    [W]::PostMessageW($box, 0x111, [IntPtr]1, [IntPtr]::Zero) | Out-Null           # IDOK
} finally {
    if ($proc -and -not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit() }
    foreach ($v in $vhds) {
        try { Diskpart @("select vdisk file=`"$(Join-Path $work "$v.vhdx")`"", 'detach vdisk') } catch { }
    }
    subst "${drive}:" /d 2>$null | Out-Null
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
