# dd-gui.exe with an image bigger than the disk: the partition table is
# shown, and the write goes ahead because the partitions fit.  Needs an
# elevated prompt.  Uses its own VHDXs only.
#   powershell -ExecutionPolicy Bypass -File tests\gui-fit-test.ps1 [-Screenshot file.png]
param(
    [string]$Gui = (Join-Path $PSScriptRoot '..\build\dd-gui.exe'),
    [string]$DD = (Join-Path $PSScriptRoot '..\build\dd.exe'),
    [string]$Screenshot = ''
)

$ErrorActionPreference = 'Stop'
$Gui = (Resolve-Path $Gui).Path
$DD = (Resolve-Path $DD).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("ddguifit-" + [Guid]::NewGuid())
New-Item -ItemType Directory $work | Out-Null
$script:failures = 0
$script:count = 0
$existing = @(Get-Disk | ForEach-Object Number)

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
  [DllImport("user32")] public static extern bool IsWindowEnabled(IntPtr h);
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
  [DllImport("user32")] static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("kernel32")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32")] static extern IntPtr VirtualAllocEx(IntPtr p, IntPtr a, IntPtr n, uint type, uint prot);
  [DllImport("kernel32")] static extern bool VirtualFreeEx(IntPtr p, IntPtr a, IntPtr n, uint type);
  [DllImport("kernel32")] static extern bool WriteProcessMemory(IntPtr p, IntPtr a, byte[] b, IntPtr n, out IntPtr done);
  [DllImport("kernel32")] static extern bool ReadProcessMemory(IntPtr p, IntPtr a, byte[] b, IntPtr n, out IntPtr done);
  // A list view cell of another process: LVM_GETITEMTEXTW with the LVITEMW in that process.
  public static string Cell(IntPtr lv, int row, int col) {
    int pid; GetWindowThreadProcessId(lv, out pid);
    IntPtr p = OpenProcess(0x0038, false, pid);      // VM_OPERATION | VM_READ | VM_WRITE
    IntPtr mem = VirtualAllocEx(p, IntPtr.Zero, (IntPtr)4096, 0x3000, 0x04), done;
    byte[] item = new byte[88];
    BitConverter.GetBytes(1).CopyTo(item, 0);                     // mask = LVIF_TEXT
    BitConverter.GetBytes(row).CopyTo(item, 4);
    BitConverter.GetBytes(col).CopyTo(item, 8);
    BitConverter.GetBytes((long)mem + 256).CopyTo(item, 24);      // pszText
    BitConverter.GetBytes(1000).CopyTo(item, 32);                 // cchTextMax
    WriteProcessMemory(p, mem, item, (IntPtr)item.Length, out done);
    SendMessageW(lv, 0x1073, (IntPtr)row, mem);                   // LVM_GETITEMTEXTW
    byte[] text = new byte[2000];
    ReadProcessMemory(p, mem + 256, text, (IntPtr)text.Length, out done);
    VirtualFreeEx(p, mem, IntPtr.Zero, 0x8000); CloseHandle(p);
    string s = Encoding.Unicode.GetString(text); int z = s.IndexOf('\0');
    return z >= 0 ? s.Substring(0, z) : s;
  }
}
'@

$IDC_DISK = 1003; $IDC_SHOWFIXED = 1005; $IDC_FILE = 1006; $IDC_START = 1014
$IDC_ETA = 1012; $IDC_LOG = 1013; $IDC_LAYOUT = 1016; $IDC_PARTS = 1017
$IDC_SRCFILE = 1029; $IDC_DISK2 = 1031; $IDC_INPUT = 1035; $IDC_OUTPUT = 1036

function F([string]$name) { Join-Path $work $name }
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
    $s = F 'dp.txt'
    Set-Content -Encoding ASCII $s $lines
    diskpart.exe /s $s | Out-Null
}
$vhds = @()
function New-Vhd([string]$name, [int]$mb, [string[]]$more = @()) {
    $path = F "$name.vhdx"
    $script:vhds += $path
    Diskpart (@("create vdisk file=`"$path`" maximum=$mb type=fixed", "select vdisk file=`"$path`"", 'attach vdisk') + $more)
    $n = (Get-DiskImage -ImagePath $path).Number
    if ($existing -contains $n) { throw "VHDX $name got the number of an existing disk ($n)" }
    $n
}
function Detach([string]$name) { Diskpart @("select vdisk file=`"$(F "$name.vhdx")`"", 'detach vdisk') }
function Parts([int]$n) {
    @(Get-Partition -DiskNumber $n -ErrorAction SilentlyContinue | Sort-Object Offset |
      ForEach-Object { "$($_.Offset)+$($_.Size)" })
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

# The Input list: 0 Disk, 1 Image file, 2 Zeros, 3 Random data.  The
# Output list: 0 Disk, 1 Image file, 2 Nothing (only Disk after Zeros or
# Random data).  Chosen as a user would: the input first.
function Set-List([IntPtr]$dlg, [int]$id, [int]$index) {
    $cb = [W]::GetDlgItem($dlg, $id)
    [W]::SendMessageW($cb, 0x14E, [IntPtr]$index, [IntPtr]::Zero) | Out-Null          # CB_SETCURSEL
    [W]::SendMessageW($dlg, 0x111, [IntPtr]($id -bor (1 -shl 16)), $cb) | Out-Null     # CBN_SELCHANGE
}
function Set-IO([IntPtr]$dlg, [int]$in, [int]$out) {
    Set-List $dlg $IDC_INPUT $in
    Set-List $dlg $IDC_OUTPUT $out
}
# "Command line": the dd command it puts in the log
function Last-Command([IntPtr]$dlg) {
    [W]::SendMessageW($dlg, 0x111, [IntPtr]1023, [IntPtr]::Zero) | Out-Null      # IDC_CMDLINE
    @(([W]::Text([W]::GetDlgItem($dlg, $IDC_LOG))) -split "`r`n" | Where-Object { $_ -like 'dd *' })[-1]
}

function Select-Disk([IntPtr]$dlg, [int]$number, [int]$list = $IDC_DISK) {
    $cb = [W]::GetDlgItem($dlg, $list)
    $n = [int][W]::SendMessageW($cb, 0x146, [IntPtr]::Zero, [IntPtr]::Zero)
    for ($i = 0; $i -lt $n; $i++) {
        if ([W]::Item($cb, $i) -like "Disk ${number}:*") {
            [W]::SendMessageW($cb, 0x14E, [IntPtr]$i, [IntPtr]::Zero) | Out-Null
            [W]::SendMessageW($dlg, 0x111, [IntPtr](($list) -bor (1 -shl 16)), $cb) | Out-Null
            return $true
        }
    }
    $false
}

# Clicks Start (no confirmation expected) and waits for the result line.
function Run-Copy([IntPtr]$dlg) {
    $eta = [W]::GetDlgItem($dlg, $IDC_ETA)
    [W]::PostMessageW($dlg, 0x111, [IntPtr]$IDC_START, [IntPtr]::Zero) | Out-Null
    Wait-For { [W]::Text($eta) -notmatch '^(Finished|Failed|Stopped)' } 5000 | Out-Null
    Wait-For { $x = [W]::Text($eta); if ($x -match '^(Finished|Failed|Stopped)') { $x } } 60000
}

# Save-Shot, with every path's folder masked
. (Join-Path $PSScriptRoot 'screenshot.ps1')

$proc = $null
try {
    # data ends under 16 MB, so the compressed reads stay small
    $s = New-Vhd 'src' 64 @('convert gpt', 'create partition primary size=4',
        'create partition efi size=6', 'create partition primary size=3')
    # the first partition gets a file system, for the Sparse read column
    $fatPart = Get-Partition -DiskNumber $s | Where-Object Type -eq 'Basic' | Sort-Object Offset | Select-Object -First 1
    Format-Volume -Partition $fatPart -FileSystem FAT -Force | Out-Null
    $srcParts = Parts $s
    cmd /c "`"$DD`" if=\\.\PhysicalDrive$s of=`"$(F 'gpt.img')`" bs=1M status=none"
    $t = New-Vhd 'dst' 48           # src stays attached: it is read from later

    $proc = Start-Process $Gui -PassThru
    $dlg = Wait-For { $proc.Refresh(); if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $proc.MainWindowHandle } }
    if (-not $dlg) { throw 'dd-gui window not found' }
    # The dialog is shown once WM_INITDIALOG is done; a click before then
    # can be undone by its setup, which selects read mode.
    if (-not (Wait-For { if ([W]::IsWindowVisible($dlg)) { $true } })) { throw 'dd-gui window never shown' }
    $ver = [regex]::Match((Get-Content (Join-Path $PSScriptRoot '..\include\dd_version.h') -Raw), '#define DD_VERSION_STR\s+"([^"]+)"').Groups[1].Value
    Check 'the title names the product and its version' ([W]::Text($dlg) -eq "dd reimagined for Windows $ver") ([W]::Text($dlg))
    $proc.Refresh()
    Check 'the GUI runs in the Idle priority class' ("$($proc.PriorityClass)" -eq 'Idle') "$($proc.PriorityClass)"
    $inList = [W]::GetDlgItem($dlg, $IDC_INPUT); $outList = [W]::GetDlgItem($dlg, $IDC_OUTPUT)
    $ins = @(0..3 | ForEach-Object { [W]::Item($inList, $_) }); $outs = @(0..2 | ForEach-Object { [W]::Item($outList, $_) })
    Check 'the Input and Output lists, and the window opens on disk to image file' (($ins -join '|') -eq
        'Disk|Image file|Zeros (wipe)|Random data (wipe)' -and ($outs -join '|') -eq 'Disk|Image file|Nothing (read test)' -and
        [W]::Text($inList) -eq 'Disk' -and [W]::Text($outList) -eq 'Image file' -and
        [W]::Text([W]::GetDlgItem($dlg, 1021)) -match 'Write a hash fil&?e') "$($ins -join ' | ') / $($outs -join ' | ')"
    Set-IO $dlg 1 0                                                     # image file to disk
    if (-not (Wait-For { if ([W]::Text([W]::GetDlgItem($dlg, 1021)) -match '^Check') { $true } })) { throw 'write mode not entered' }
    [W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_SHOWFIXED), 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $forceBox = [W]::GetDlgItem($dlg, 1009); $lockBox = [W]::GetDlgItem($dlg, 1024)    # IDC_FORCE, IDC_LOCKIN
    Check 'writing: Force dismount is offered, the read lock is not' ([W]::IsWindowEnabled($forceBox) -and -not [W]::IsWindowEnabled($lockBox))
    $cb = [W]::GetDlgItem($dlg, $IDC_DISK2)
    if (-not (Select-Disk $dlg $t $IDC_DISK2)) { throw "disk $t is not offered to write" }
    [W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_SRCFILE), 0x0C, [IntPtr]::Zero, (F 'gpt.img')) | Out-Null
    Start-Sleep -Milliseconds 500

    $summary = [W]::Text([W]::GetDlgItem($dlg, $IDC_LAYOUT))
    $rows = [int][W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_PARTS), 0x1004, [IntPtr]::Zero, [IntPtr]::Zero)  # LVM_GETITEMCOUNT
    Check 'partition list shows every partition' ($rows -eq $srcParts.Count) "$rows rows, $($srcParts.Count) partitions"
    Check 'summary names the GPT' ($summary -match '^GPT, ') $summary
    Check 'summary says the image fits the disk' ($summary -match "Fits disk ${t}") $summary
    # exact byte counts, grouped as the user's locale does
    $dataEnd = ($srcParts | ForEach-Object { $a = $_.Split('+'); [long]$a[0] + [long]$a[1] } | Measure-Object -Maximum).Maximum
    $exactEnd = '{0:N0}' -f [long]$dataEnd
    $exactDisk = '{0:N0}' -f [long](Get-Disk -Number $t).Size
    Check 'summary shows the exact data end and disk size' ($summary.Contains("($exactEnd)") -and $summary.Contains("($exactDisk)")) "want $exactEnd and $exactDisk in: $summary"
    $label = [W]::Text($cb)
    Check 'disk list shows the exact disk size' ($label.Contains("($exactDisk)")) $label

    if ($Screenshot) { Save-Shot $dlg $Screenshot }

    [W]::PostMessageW($dlg, 0x111, [IntPtr]$IDC_START, [IntPtr]::Zero) | Out-Null
    $box = Wait-For { $h = [W]::FindWindowW('#32770', 'Overwrite disk'); if ($h -ne [IntPtr]::Zero) { $h } }
    $text = if ($box) { [W]::Text([W]::GetDlgItem($box, 0xFFFF)) } else { '' }
    Check 'confirmation says only the partitions are written' ($text -match 'Only the first' -and $text -match 'GPT will be adjusted') $text
    $exactImage = '{0:N0}' -f [long](Get-Item (F 'gpt.img')).Length
    Check 'confirmation shows exact sizes' ($text.Contains("($exactEnd)") -and $text.Contains("($exactImage)") -and $text.Contains("($exactDisk)")) $text
    if ($box) { [W]::PostMessageW($box, 0x111, [IntPtr]6, [IntPtr]::Zero) | Out-Null }
    $eta = [W]::GetDlgItem($dlg, $IDC_ETA)
    $done = Wait-For { $x = [W]::Text($eta); if ($x -match '^(Finished|Failed|Stopped)') { $x } } 60000
    $log = [W]::Text([W]::GetDlgItem($dlg, $IDC_LOG))
    Check 'the oversize image is written' ("$done" -like 'Finished*') "$done`n$log"
    Start-Sleep -Seconds 2
    Update-Disk -Number $t -ErrorAction SilentlyContinue
    $got = Parts $t
    Check 'Windows sees the GPT and the same partitions on the disk' ((Get-Disk -Number $t).PartitionStyle -eq 'GPT' -and ($got -join ',') -eq ($srcParts -join ',')) "source: $($srcParts -join ', ')`ntarget: $($got -join ', ')"

    # --- reading a card: only the partitions, unless Raw is ticked ------------
    $IDC_FIT = 1018; $IDC_XZ = 1019
    Set-IO $dlg 0 1                                                     # disk to image file
    Select-Disk $dlg $s | Out-Null
    [W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_FILE), 0x0C, [IntPtr]::Zero, (F 'read.img')) | Out-Null
    Start-Sleep -Milliseconds 500
    $summary = [W]::Text([W]::GetDlgItem($dlg, $IDC_LAYOUT))
    $rows = [int][W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_PARTS), 0x1004, [IntPtr]::Zero, [IntPtr]::Zero)
    $raw = [W]::GetDlgItem($dlg, $IDC_FIT)
    Check 'reading: the disk''s partition table is shown' ($rows -eq $srcParts.Count -and $summary -match '^GPT, ') "$rows rows: $summary"
    Check 'reading: Raw is offered, unticked' ([W]::Text($raw) -match 'all of it, not just its partitions' -and
        [int][W]::SendMessageW($raw, 0xF0, [IntPtr]::Zero, [IntPtr]::Zero) -eq 0) ([W]::Text($raw))
    $needed = [long]$dataEnd + 33 * 512
    Check 'reading: summary gives the image size' ($summary.Contains("($('{0:N0}' -f $needed))")) $summary
    $xzBox = [W]::GetDlgItem($dlg, $IDC_XZ)
    Check 'reading: compression defaults to xz level 9' ([W]::Text($xzBox) -match '^xz level 9' -and $summary -match 'xz level 9') "$([W]::Text($xzBox)) / $summary"
    # "Command line" puts the equivalent dd.exe command in the log
    [W]::SendMessageW($dlg, 0x111, [IntPtr]1023, [IntPtr]::Zero) | Out-Null      # IDC_CMDLINE
    $cmdLine = @(([W]::Text([W]::GetDlgItem($dlg, $IDC_LOG))) -split "`r`n" | Where-Object { $_ -like 'dd *' })[-1]
    Check 'Command line shows the read as a dd command' ($cmdLine -eq "dd if=\\.\PhysicalDrive$s of=$(F 'read.img.xz') bs=1M --fit --filter=file --hashes --progress --xz=9") $cmdLine
    if ($Screenshot) { Save-Shot $dlg ([IO.Path]::ChangeExtension($Screenshot, '.read.png')) }
    $done = Run-Copy $dlg
    $fileNow = [W]::Text([W]::GetDlgItem($dlg, $IDC_FILE))
    Check 'reading: .xz is added to the file name' ($fileNow -eq (F 'read.img.xz') -and (Test-Path (F 'read.img.xz')) -and -not (Test-Path (F 'read.img'))) $fileNow
    & $DD "if=$(F 'read.img.xz')" "of=$(F 'read-unxz.img')" status=none
    $len = (Get-Item (F 'read-unxz.img') -ErrorAction SilentlyContinue).Length
    $packed = (Get-Item (F 'read.img.xz') -ErrorAction SilentlyContinue).Length
    Check 'reading: compressed, then the partitions and a backup GPT' ("$done" -like 'Finished*' -and $len -eq $needed -and $packed -lt $needed / 4) "$done, $packed packed, $len bytes, want $needed"
    $lay = (& $DD --layout "if=$(F 'read-unxz.img')") -join "`n"
    Check 'reading: the trimmed image has a valid GPT and backup' ($lay -match "`nGPT" -and $lay -notmatch 'note:') $lay
    $hs = if (Test-Path (F 'read.img.hashes')) { [IO.File]::ReadAllText((F 'read.img.hashes')) } else { '' }
    $want = "$((Get-FileHash (F 'read-unxz.img')).Hash.ToLower())  read.img`n$((Get-FileHash (F 'read.img.xz')).Hash.ToLower())  read.img.xz`n"
    Check 'reading: a hash file is written by default, with both hashes' ($hs -eq $want) $hs
    # that command, run by dd.exe into another file, reads the same image
    $run = $cmdLine.Replace((F 'read.img.xz'), (F 'cmd.img.xz')) -replace '^dd ', "`"$DD`" "
    cmd /c "$run 2>nul"
    $cmdHash = if (Test-Path (F 'cmd.img.hashes')) { ([IO.File]::ReadAllLines((F 'cmd.img.hashes'))[0] -split '  ')[0] } else { 'none' }
    Check 'the command line reads the same image as the GUI' ($LASTEXITCODE -eq 0 -and $cmdHash -eq ($hs -split '  ')[0]) "$run -> $cmdHash"

    # reading: the lock box takes Force dismount's place, and gives --lock-input
    $shown = [W]::IsWindowEnabled($lockBox) -and -not [W]::IsWindowEnabled($forceBox)
    [W]::SendMessageW($lockBox, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null      # tick
    [W]::SendMessageW($dlg, 0x111, [IntPtr]1023, [IntPtr]::Zero) | Out-Null
    $lockLine = @(([W]::Text([W]::GetDlgItem($dlg, $IDC_LOG))) -split "`r`n" | Where-Object { $_ -like 'dd *' })[-1]
    [W]::SendMessageW($lockBox, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null      # untick
    Check 'reading: the lock box is offered, not Force dismount, and adds --lock-input' ($shown -and $lockLine -match ' --lock-input ') $lockLine

    # --- sparse reading: the Sparse read column, the boxes, the command ---------
    $IDC_SPARSE = 1025; $IDC_UNCLEAN = 1026; $IDC_REFRESH = 1004
    $sparseBox = [W]::GetDlgItem($dlg, $IDC_SPARSE); $uncleanBox = [W]::GetDlgItem($dlg, $IDC_UNCLEAN)
    $lv = [W]::GetDlgItem($dlg, $IDC_PARTS)
    function Column { $n = [int][W]::SendMessageW($lv, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero); @(for ($i = 0; $i -lt $n; $i++) { [W]::Cell($lv, $i, 5) }) }
    # Windows marks a freshly formatted FAT volume dirty for a few seconds; Refresh, as a user would
    $col = Wait-For { [W]::PostMessageW($dlg, 0x111, [IntPtr]$IDC_REFRESH, [IntPtr]::Zero) | Out-Null; Start-Sleep -Milliseconds 300
                      $c = Column; if ($c[0] -match '^Yes: FAT1[26], reads ') { $c } } 30000
    if (-not $col) { $col = Column }
    Check 'reading: the Sparse read column says the FAT partition can be read sparsely' ($col[0] -match '^Yes: FAT1[26], reads ') ($col -join ' | ')
    Check '... and the others cannot, saying why' ($col.Count -eq $srcParts.Count -and @($col[1..($col.Count - 1)] | Where-Object { $_ -ne 'No: not FAT, exFAT, NTFS, ext or swap' }).Count -eq 0) ($col -join ' | ')
    Check 'reading: Only used space is shown, Allow unclean waits for it' ([W]::IsWindowVisible($sparseBox) -and [W]::IsWindowEnabled($sparseBox) -and
        [W]::IsWindowVisible($uncleanBox) -and -not [W]::IsWindowEnabled($uncleanBox))
    [W]::SendMessageW($sparseBox, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null      # tick
    $summary = [W]::Text([W]::GetDlgItem($dlg, $IDC_LAYOUT))
    Check 'ticked: the summary says how much is read' ($summary -match 'Only the space in use is read: ' -and [W]::IsWindowEnabled($uncleanBox)) $summary
    [W]::SendMessageW($uncleanBox, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null     # tick
    [W]::SendMessageW($dlg, 0x111, [IntPtr]1023, [IntPtr]::Zero) | Out-Null
    $sparseLine = @(([W]::Text([W]::GetDlgItem($dlg, $IDC_LOG))) -split "`r`n" | Where-Object { $_ -like 'dd *' })[-1]
    Check 'ticked: Command line adds --sparse-read --allow-unclean' ($sparseLine -match ' --sparse-read --allow-unclean ') $sparseLine
    [W]::SendMessageW($uncleanBox, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null     # untick
    $xzBox0 = [W]::GetDlgItem($dlg, $IDC_XZ)
    [W]::SendMessageW($xzBox0, 0x14E, [IntPtr]0, [IntPtr]::Zero) | Out-Null           # None
    [W]::SendMessageW($dlg, 0x111, [IntPtr](($IDC_XZ) -bor (1 -shl 16)), $xzBox0) | Out-Null
    [W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_FILE), 0x0C, [IntPtr]::Zero, (F 'sparse.img')) | Out-Null
    [W]::SendMessageW($dlg, 0x111, [IntPtr]1023, [IntPtr]::Zero) | Out-Null
    $sparseLine = @(([W]::Text([W]::GetDlgItem($dlg, $IDC_LOG))) -split "`r`n" | Where-Object { $_ -like 'dd *' })[-1]
    $done = Run-Copy $dlg
    $log = [W]::Text([W]::GetDlgItem($dlg, $IDC_LOG))
    Check 'a sparse read from the GUI' ("$done" -like 'Finished*' -and $log -match 'partition 1 \(FAT1[26]\): reading the' -and $log -match 'free space written as zeros') "$done`n$log"
    $run = $sparseLine.Replace((F 'sparse.img'), (F 'sparse-cmd.img')) -replace '^dd ', "`"$DD`" "
    cmd /c "$run 2>nul"
    Check '... gives the same image as its command line' ($LASTEXITCODE -eq 0 -and (Get-FileHash (F 'sparse.img')).Hash -eq (Get-FileHash (F 'sparse-cmd.img')).Hash) $run
    [W]::SendMessageW($sparseBox, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null      # untick

    # raw, uncompressed
    [W]::SendMessageW($raw, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null      # tick Raw
    [W]::SendMessageW($xzBox, 0x14E, [IntPtr]0, [IntPtr]::Zero) | Out-Null       # None
    [W]::SendMessageW($dlg, 0x111, [IntPtr](($IDC_XZ) -bor (1 -shl 16)), $xzBox) | Out-Null
    [W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_FILE), 0x0C, [IntPtr]::Zero, (F 'read-raw.img')) | Out-Null
    Start-Sleep -Milliseconds 300
    $summary = [W]::Text([W]::GetDlgItem($dlg, $IDC_LAYOUT))
    Check 'reading raw: summary says the whole disk is read, uncompressed' ($summary -match 'whole disk' -and $summary -match 'uncompressed\.') $summary
    $done = Run-Copy $dlg
    $len = (Get-Item (F 'read-raw.img') -ErrorAction SilentlyContinue).Length
    $size = (Get-Disk -Number $s).Size
    Check 'reading raw: the image is the whole disk' ("$done" -like 'Finished*' -and $len -eq $size) "$done, $len bytes, want $size"

    # --- writing the .xz image back: decompressed on the fly ----------------------
    & $DD if=/dev/zero "of=\\.\PhysicalDrive$t" bs=1M count=2 status=none      # wipe the tables
    Update-Disk -Number $t -ErrorAction SilentlyContinue
    Set-IO $dlg 1 0                                                     # image file to disk
    Select-Disk $dlg $t $IDC_DISK2 | Out-Null
    [W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_SRCFILE), 0x0C, [IntPtr]::Zero, (F 'read.img.xz')) | Out-Null
    Start-Sleep -Milliseconds 500
    $summary = [W]::Text([W]::GetDlgItem($dlg, $IDC_LAYOUT))
    Check 'writing: an .xz image''s table is shown, and that it is compressed' ($summary -match '^xz-compressed' -and $summary -match 'GPT, 3 partitions') $summary
    $makeMap = [W]::GetDlgItem($dlg, 1034)                                       # IDC_MAKEMAP
    $col = Column
    Check 'writing a compressed image: the Sparse read column says Start maps it' ([W]::IsWindowVisible($sparseBox) -and
        [W]::IsWindowVisible($makeMap) -and [W]::IsWindowEnabled($makeMap) -and
        @($col | Where-Object { $_ -ne '? Mapped when you start' }).Count -eq 0) ($col -join ' | ')
    $IDC_HASHES = 1021; $IDC_VERIFY = 1022
    $verify = [W]::GetDlgItem($dlg, $IDC_VERIFY)
    Check 'Verify after starts unticked, the hash file check ticked' ([int][W]::SendMessageW($verify, 0xF0, [IntPtr]::Zero, [IntPtr]::Zero) -eq 0 -and
        [int][W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_HASHES), 0xF0, [IntPtr]::Zero, [IntPtr]::Zero) -eq 1 -and
        [W]::Text($verify) -match 'what is written')
    [W]::SendMessageW($verify, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null      # tick Verify after writing
    [W]::PostMessageW($dlg, 0x111, [IntPtr]$IDC_START, [IntPtr]::Zero) | Out-Null
    $box = Wait-For { $h = [W]::FindWindowW('#32770', 'Overwrite disk'); if ($h -ne [IntPtr]::Zero) { $h } }
    $text = if ($box) { [W]::Text([W]::GetDlgItem($box, 0xFFFF)) } else { '' }
    Check 'writing: the confirmation gives both sizes' ($text -match 'xz-compressed' -and $text -match 'uncompressed') $text
    if ($box) { [W]::PostMessageW($box, 0x111, [IntPtr]6, [IntPtr]::Zero) | Out-Null }
    $done = Wait-For { $x = [W]::Text([W]::GetDlgItem($dlg, $IDC_ETA)); if ($x -match '^(Finished|Failed|Stopped)') { $x } } 60000
    Start-Sleep -Seconds 2
    Update-Disk -Number $t -ErrorAction SilentlyContinue
    $got = Parts $t
    Check 'writing: the .xz image lands with the same partitions' ("$done" -like 'Finished*' -and (Get-Disk -Number $t).PartitionStyle -eq 'GPT' -and ($got -join ',') -eq ($srcParts -join ',')) "$done; $($got -join ', ')"
    $log = [W]::Text([W]::GetDlgItem($dlg, $IDC_LOG))
    Check 'writing: both hashes are checked, and the disk verified' ($log -match 'read\.img\.xz matches' -and $log -match 'read\.img \(uncompressed\) matches' -and
        $log -match 'verified:' -and "$done" -match 'verified') "$done`n$log"

    # --- copying an image to an image: compressed to compressed, sparse -----------
    Set-IO $dlg 1 1                                                     # image file to image file
    # back to the defaults: Raw and Verify were ticked above
    [W]::SendMessageW($raw, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    [W]::SendMessageW($verify, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $srcBox = [W]::GetDlgItem($dlg, $IDC_SRCFILE)
    Check 'copying images: two file boxes, compression offered, no disk boxes' ([W]::IsWindowVisible($srcBox) -and
        -not [W]::IsWindowVisible([W]::GetDlgItem($dlg, $IDC_DISK)) -and [W]::IsWindowVisible([W]::GetDlgItem($dlg, $IDC_FILE)) -and
        [W]::IsWindowEnabled([W]::GetDlgItem($dlg, $IDC_XZ)) -and -not [W]::IsWindowEnabled($forceBox) -and -not [W]::IsWindowEnabled($lockBox))
    [W]::SendMessageW($srcBox, 0x0C, [IntPtr]::Zero, (F 'read.img.xz')) | Out-Null
    Start-Sleep -Milliseconds 500
    $col = Column
    Check 'copying a compressed image: its table is shown, and it needs a sparse map' ($col.Count -eq $srcParts.Count -and $col[0] -eq '? Mapped when you start' -and
        [W]::IsWindowEnabled($makeMap)) ($col -join ' | ')
    [W]::PostMessageW($dlg, 0x111, [IntPtr]1034, [IntPtr]::Zero) | Out-Null           # Make sparse map
    $done = Wait-For { $x = [W]::Text([W]::GetDlgItem($dlg, $IDC_ETA)); if ($x -match '^(Finished|Failed|Stopped)') { $x } } 60000
    Start-Sleep -Milliseconds 300
    $col = Column
    Check 'Make sparse map maps the compressed image, and is then disabled' ("$done" -like 'Finished. The sparse map is made*' -and (Test-Path (F 'read.img.sparse')) -and
        $col[0] -match '^Yes: FAT1[26], reads ' -and -not [W]::IsWindowEnabled($makeMap)) "$done | $($col -join ' | ')`n$([W]::Text([W]::GetDlgItem($dlg, $IDC_LOG)))"
    # another image, with no map yet: the button comes back
    Copy-Item (F 'read.img.xz') (F 'auto.img.xz')
    [W]::SendMessageW($srcBox, 0x0C, [IntPtr]::Zero, (F 'auto.img.xz')) | Out-Null
    Start-Sleep -Milliseconds 300
    $on = [W]::IsWindowEnabled($makeMap)
    [W]::SendMessageW($srcBox, 0x0C, [IntPtr]::Zero, (F 'read.img.xz')) | Out-Null
    Start-Sleep -Milliseconds 300
    Check 'Make sparse map: offered again for an image without a map, not for one with' ($on -and -not [W]::IsWindowEnabled($makeMap))
    [W]::SendMessageW($sparseBox, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null      # tick
    $xzBox1 = [W]::GetDlgItem($dlg, $IDC_XZ)
    [W]::SendMessageW($xzBox1, 0x14E, [IntPtr]2, [IntPtr]::Zero) | Out-Null           # xz level 1
    [W]::SendMessageW($dlg, 0x111, [IntPtr](($IDC_XZ) -bor (1 -shl 16)), $xzBox1) | Out-Null
    [W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_FILE), 0x0C, [IntPtr]::Zero, (F 'copy.img')) | Out-Null
    [W]::SendMessageW($dlg, 0x111, [IntPtr]1023, [IntPtr]::Zero) | Out-Null
    $copyLine = @(([W]::Text([W]::GetDlgItem($dlg, $IDC_LOG))) -split "`r`n" | Where-Object { $_ -like 'dd *' })[-1]
    Check 'copying images: the command line reads one image into the other' ($copyLine -eq
        "dd if=$(F 'read.img.xz') of=$(F 'copy.img.xz') bs=1M --fit --filter=file --sparse-read --hashes --progress --xz=1") $copyLine
    $done = Run-Copy $dlg
    $log = [W]::Text([W]::GetDlgItem($dlg, $IDC_LOG))
    Check 'compressed to compressed, sparse: read in order, the map and hash file checked' ("$done" -like 'Finished*' -and
        $log -match 'its free space, from the sparse map' -and $log -match 'SHA-256 of read\.img\.xz matches') "$done`n$log"
    & $DD "if=$(F 'copy.img.xz')" "of=$(F 'copy.img')" status=none
    & $DD "if=$(F 'read-unxz.img')" "of=$(F 'copy-ref.img')" --sparse-read status=none
    Check '... giving exactly a sparse read of the uncompressed image' ((Get-FileHash (F 'copy.img')).Hash -eq (Get-FileHash (F 'copy-ref.img')).Hash)
    $run = $copyLine.Replace((F 'copy.img.xz'), (F 'copy-cmd.img.xz')) -replace '^dd ', "`"$DD`" "
    cmd /c "$run 2>nul"
    & $DD "if=$(F 'copy-cmd.img.xz')" "of=$(F 'copy-cmd.img')" status=none
    Check '... as its command line does' ((Get-FileHash (F 'copy-cmd.img')).Hash -eq (Get-FileHash (F 'copy.img')).Hash) $run
    # Start maps an image that has no map, then copies it
    [W]::SendMessageW($srcBox, 0x0C, [IntPtr]::Zero, (F 'auto.img.xz')) | Out-Null
    [W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_FILE), 0x0C, [IntPtr]::Zero, (F 'auto-copy.img')) | Out-Null
    Start-Sleep -Milliseconds 300
    $done = Run-Copy $dlg
    $log = [W]::Text([W]::GetDlgItem($dlg, $IDC_LOG))
    & $DD "if=$(F 'auto-copy.img.xz')" "of=$(F 'auto-copy.img')" status=none
    Check 'Start makes a missing sparse map, then copies' ("$done" -like 'Finished*' -and (Test-Path (F 'auto.img.sparse')) -and
        $log -match 'Making the sparse map of' -and $log -match 'now the copy' -and
        (Get-FileHash (F 'auto-copy.img')).Hash -eq (Get-FileHash (F 'copy.img')).Hash -and -not [W]::IsWindowEnabled($makeMap)) "$done`n$log"
    [W]::SendMessageW($sparseBox, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null      # untick

    # --- copying a disk to a disk -----------------------------------------------------
    Set-IO $dlg 0 0                                                     # disk to disk
    $disk2 = [W]::GetDlgItem($dlg, $IDC_DISK2)
    Check 'copying disks: two disk lists; Lock and Force offered; no file, compression or hash file' ([W]::IsWindowVisible($disk2) -and
        [W]::IsWindowVisible([W]::GetDlgItem($dlg, $IDC_DISK)) -and -not [W]::IsWindowVisible([W]::GetDlgItem($dlg, $IDC_FILE)) -and
        [W]::IsWindowEnabled($lockBox) -and [W]::IsWindowEnabled($forceBox) -and -not [W]::IsWindowEnabled([W]::GetDlgItem($dlg, $IDC_XZ)) -and
        -not [W]::IsWindowVisible([W]::GetDlgItem($dlg, 1021)))
    & $DD if=/dev/zero "of=\\.\PhysicalDrive$t" bs=1M count=2 status=none      # wipe the tables
    Update-Disk -Number $t -ErrorAction SilentlyContinue
    Select-Disk $dlg $s | Out-Null
    Select-Disk $dlg $t $IDC_DISK2 | Out-Null
    Start-Sleep -Milliseconds 500
    $summary = [W]::Text([W]::GetDlgItem($dlg, $IDC_LAYOUT))
    Check 'copying disks: the source disk''s table, and whether it fits the target' ($summary -match '^GPT, ' -and $summary -match "Fits disk ${t}") $summary
    [W]::SendMessageW($lockBox, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null      # tick Lock
    [W]::SendMessageW($sparseBox, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null    # tick Only used space
    [W]::SendMessageW($dlg, 0x111, [IntPtr]1023, [IntPtr]::Zero) | Out-Null
    $diskLine = @(([W]::Text([W]::GetDlgItem($dlg, $IDC_LOG))) -split "`r`n" | Where-Object { $_ -like 'dd *' })[-1]
    Check 'copying disks: the command line' ($diskLine -eq "dd if=\\.\PhysicalDrive$s of=\\.\PhysicalDrive$t bs=1M --fit --filter=disk --lock-input --sparse-read --progress") $diskLine
    [W]::PostMessageW($dlg, 0x111, [IntPtr]$IDC_START, [IntPtr]::Zero) | Out-Null
    $box = Wait-For { $h = [W]::FindWindowW('#32770', 'Overwrite disk'); if ($h -ne [IntPtr]::Zero) { $h } }
    $text = if ($box) { [W]::Text([W]::GetDlgItem($box, 0xFFFF)) } else { '' }
    Check 'copying disks: the confirmation names both disks' ($text -match "Disk ${t}:" -and $text -match "with Disk ${s}:") $text
    if ($box) { [W]::PostMessageW($box, 0x111, [IntPtr]6, [IntPtr]::Zero) | Out-Null }
    $done = Wait-For { $x = [W]::Text([W]::GetDlgItem($dlg, $IDC_ETA)); if ($x -match '^(Finished|Failed|Stopped)') { $x } } 60000
    $log = [W]::Text([W]::GetDlgItem($dlg, $IDC_LOG))
    Start-Sleep -Seconds 2
    Update-Disk -Number $t -ErrorAction SilentlyContinue
    $got = Parts $t
    Check 'a disk copied to a disk: locked, sparse, the same partitions' ("$done" -like 'Finished*' -and $log -match 'while it is read' -and
        $log -match 'partition 1 \(FAT1[26]\): reading the' -and (Get-Disk -Number $t).PartitionStyle -eq 'GPT' -and ($got -join ',') -eq ($srcParts -join ',')) "$done; $($got -join ', ')`n$log"
    [W]::SendMessageW($lockBox, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null      # untick
    [W]::SendMessageW($sparseBox, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null

    # --- checking an image against its hash file --------------------------------
    Set-IO $dlg 1 2                                                     # image file to nothing
    [W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_SRCFILE), 0x0C, [IntPtr]::Zero, (F 'read.img.xz')) | Out-Null
    Start-Sleep -Milliseconds 300
    $summary = [W]::Text([W]::GetDlgItem($dlg, $IDC_LAYOUT))
    Check 'checking: the summary names the hash file' ($summary -match [regex]::Escape("Hash file: $(F 'read.img.hashes')")) $summary
    Check 'checking: no disk is needed' (-not [W]::IsWindowEnabled([W]::GetDlgItem($dlg, $IDC_DISK)) -and [W]::IsWindowEnabled([W]::GetDlgItem($dlg, $IDC_START)))
    if ($Screenshot) { Save-Shot $dlg ([IO.Path]::ChangeExtension($Screenshot, '.check.png')) }
    $done = Run-Copy $dlg
    Check 'checking: a good image matches' ("$done" -eq 'Finished. The image matches its hash file.') "$done`n$([W]::Text([W]::GetDlgItem($dlg, $IDC_LOG)))"
    $b = [IO.File]::ReadAllBytes((F 'read.img.xz')); $mid = [int]($b.Length / 2); $b[$mid] = $b[$mid] -bxor 0x40
    [IO.File]::WriteAllBytes((F 'bad.img.xz'), $b)
    [IO.File]::WriteAllText((F 'bad.img.hashes'), ([IO.File]::ReadAllText((F 'read.img.hashes'))).Replace('read.img', 'bad.img'))
    [W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_SRCFILE), 0x0C, [IntPtr]::Zero, (F 'bad.img.xz')) | Out-Null
    Start-Sleep -Milliseconds 300
    $done = Run-Copy $dlg
    $log = [W]::Text([W]::GetDlgItem($dlg, $IDC_LOG))
    # xz's own CRC usually catches it first; either way the check fails
    Check 'checking: a damaged image fails' ("$done" -like 'Failed*' -and $log -match 'does not match|corrupt') "$done`n$log"

    # --- image to nothing, hash file box unticked: only read it -----------------
    $hashBox = [W]::GetDlgItem($dlg, 1021)
    [W]::SendMessageW([W]::GetDlgItem($dlg, $IDC_SRCFILE), 0x0C, [IntPtr]::Zero, (F 'read.img.xz')) | Out-Null
    [W]::SendMessageW($hashBox, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null      # untick
    $summary = [W]::Text([W]::GetDlgItem($dlg, $IDC_LAYOUT))
    $line = Last-Command $dlg
    Check 'image to nothing, unticked: read and decompressed to /dev/null' ($summary -match 'read and decompressed to test' -and
        $line -eq "dd if=$(F 'read.img.xz') of=/dev/null bs=1M --no-hashes --progress") "$summary`n$line"
    $done = Run-Copy $dlg
    Check '... and it reads without error' ("$done" -eq 'Finished. All of it was read without error.') "$done`n$([W]::Text([W]::GetDlgItem($dlg, $IDC_LOG)))"
    [W]::SendMessageW($hashBox, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null      # tick again

    # --- disk to nothing: a read test ----------------------------------------------
    Set-IO $dlg 0 2
    Select-Disk $dlg $s | Out-Null
    Start-Sleep -Milliseconds 500
    $summary = [W]::Text([W]::GetDlgItem($dlg, $IDC_LAYOUT))
    Check 'read test: no file, compression, hash file or Verify; a note that nothing is written' (
        -not [W]::IsWindowVisible([W]::GetDlgItem($dlg, $IDC_FILE)) -and [W]::IsWindowVisible([W]::GetDlgItem($dlg, 1038)) -and
        -not [W]::IsWindowEnabled([W]::GetDlgItem($dlg, $IDC_XZ)) -and -not [W]::IsWindowVisible($hashBox) -and
        -not [W]::IsWindowEnabled($verify) -and [W]::IsWindowEnabled($lockBox) -and $summary -match 'The partitions, .* will be read to test') $summary
    $line = Last-Command $dlg
    Check 'read test: the command line reads the disk to /dev/null' ($line -eq "dd if=\\.\PhysicalDrive$s of=/dev/null bs=1M count=$([long]$dataEnd) iflag=count_bytes --progress") $line
    $done = Run-Copy $dlg
    $log = [W]::Text([W]::GetDlgItem($dlg, $IDC_LOG))
    Check 'read test: the partitions are read, nothing written' ("$done" -eq 'Finished. All of it was read without error.' -and
        $log -match 'to test it; nothing is written' -and $log -match "`n$([long]$dataEnd) bytes") "$done`n$log"

    # --- zeros and random data to a disk: a wipe -----------------------------------
    $size = [long](Get-Disk -Number $t).Size
    Set-List $dlg $IDC_INPUT 2                                          # Zeros
    $n = [int][W]::SendMessageW($outList, 0x146, [IntPtr]::Zero, [IntPtr]::Zero)
    $outs = @(0..($n - 1) | ForEach-Object { [W]::Item($outList, $_) })
    Check 'wipe: zeros go only to a disk' (($outs -join '|') -eq 'Disk' -and [W]::IsWindowVisible([W]::GetDlgItem($dlg, $IDC_DISK2)) -and
        -not [W]::IsWindowVisible([W]::GetDlgItem($dlg, $IDC_DISK)) -and [W]::IsWindowVisible([W]::GetDlgItem($dlg, 1037)) -and
        -not [W]::IsWindowVisible($sparseBox) -and -not [W]::IsWindowEnabled($raw)) ($outs -join ' | ')
    Select-Disk $dlg $t $IDC_DISK2 | Out-Null
    Start-Sleep -Milliseconds 300
    $summary = [W]::Text([W]::GetDlgItem($dlg, $IDC_LAYOUT))
    Check 'wipe: the summary names the disk and its exact size' ($summary -match "All of disk ${t}," -and
        $summary.Contains("($('{0:N0}' -f $size))") -and $summary -match 'with zeros') $summary
    [W]::SendMessageW($verify, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null      # tick Verify
    $line = Last-Command $dlg
    Check 'wipe: the command line writes exactly the disk' ($line -eq "dd if=/dev/zero of=\\.\PhysicalDrive$t bs=1M count=$size iflag=count_bytes --filter=disk --verify --progress") $line
    [W]::PostMessageW($dlg, 0x111, [IntPtr]$IDC_START, [IntPtr]::Zero) | Out-Null
    $box = Wait-For { $h = [W]::FindWindowW('#32770', 'Overwrite disk'); if ($h -ne [IntPtr]::Zero) { $h } }
    $text = if ($box) { [W]::Text([W]::GetDlgItem($box, 0xFFFF)) } else { '' }
    Check 'wipe: confirmed first, naming the disk' ($text -match "Disk ${t}:" -and $text -match 'with zeros') $text
    if ($box) { [W]::PostMessageW($box, 0x111, [IntPtr]6, [IntPtr]::Zero) | Out-Null }
    $done = Wait-For { $x = [W]::Text([W]::GetDlgItem($dlg, $IDC_ETA)); if ($x -match '^(Finished|Failed|Stopped)') { $x } } 120000
    $log = [W]::Text([W]::GetDlgItem($dlg, $IDC_LOG))
    $tmp = F 'wiped.bin'
    & $DD "if=\\.\PhysicalDrive$t" "of=$tmp" bs=1M status=none
    $b = [IO.File]::ReadAllBytes($tmp)
    $allZero = $b.Length -eq $size -and [Array]::FindIndex($b, [Predicate[byte]] { param($x) $x -ne 0 }) -lt 0
    Check 'wipe: every byte of the disk is zero, and verified' ("$done" -like 'Finished*verified*' -and $allZero -and
        $log -match "`n$size bytes") "$done`n$log"
    Set-List $dlg $IDC_INPUT 3                                          # Random data
    Select-Disk $dlg $t $IDC_DISK2 | Out-Null
    $line = Last-Command $dlg
    Check 'wipe with random data: the command line' ($line -eq "dd if=/dev/random of=\\.\PhysicalDrive$t bs=1M count=$size iflag=count_bytes --filter=disk --verify --progress") $line
    [W]::PostMessageW($dlg, 0x111, [IntPtr]$IDC_START, [IntPtr]::Zero) | Out-Null
    $box = Wait-For { $h = [W]::FindWindowW('#32770', 'Overwrite disk'); if ($h -ne [IntPtr]::Zero) { $h } }
    if ($box) { [W]::PostMessageW($box, 0x111, [IntPtr]6, [IntPtr]::Zero) | Out-Null }
    $done = Wait-For { $x = [W]::Text([W]::GetDlgItem($dlg, $IDC_ETA)); if ($x -match '^(Finished|Failed|Stopped)') { $x } } 120000
    & $DD "if=\\.\PhysicalDrive$t" "of=$tmp" bs=1M status=none
    $b = [IO.File]::ReadAllBytes($tmp)
    $zeros = 0; for ($i = 0; $i -lt 65536; $i++) { if ($b[$i] -eq 0) { $zeros++ } }
    Check 'wipe with random data: the disk no longer reads as zeros, and is verified' ("$done" -like 'Finished*verified*' -and $zeros -lt 1024) "$done; $zeros zero bytes in the first 64 KiB"
    Set-List $dlg $IDC_INPUT 0                                          # back to a disk
    Check 'after a wipe, a disk input offers every output again' ([int][W]::SendMessageW($outList, 0x146, [IntPtr]::Zero, [IntPtr]::Zero) -eq 3)
} finally {
    if ($proc -and -not $proc.HasExited) { $proc.Kill() }
    foreach ($v in $vhds) { try { Diskpart @("select vdisk file=`"$v`"", 'detach vdisk') } catch { } }
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "$($script:count - $script:failures) of $script:count passed"
exit [int]($script:failures -ne 0)
