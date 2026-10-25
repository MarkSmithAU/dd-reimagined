# Save-Shot: a PNG of a window, with the folder part of every file path in
# it masked, so screenshots show no user names or local paths.  A path's
# folder is everything up to its last backslash (C:\Users\me\card.img shows
# as a black block and card.img); device names (\\.\PhysicalDrive2) and
# drive roots in mount lists (E:\) are left as they are.
#   . (Join-Path $PSScriptRoot 'screenshot.ps1')
#   Save-Shot $hwnd 'file.png'

Add-Type -AssemblyName System.Drawing, System.Windows.Forms
if (-not ('ShotW' -as [type])) {
    Add-Type @'
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class ShotW {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32")] public static extern bool EnumChildWindows(IntPtr h, EnumProc f, IntPtr l);
  [DllImport("user32", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32")] public static extern bool SetProcessDPIAware();
  [DllImport("user32")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, StringBuilder l);
  public static List<IntPtr> Children(IntPtr h) {
    var r = new List<IntPtr>();
    EnumChildWindows(h, (c, l) => { r.Add(c); return true; }, IntPtr.Zero);
    return r;
  }
  public static string Class(IntPtr h) { var s = new StringBuilder(64); GetClassNameW(h, s, 64); return s.ToString(); }
  public static string Text(IntPtr h) {
    int n = (int)SendMessageW(h, 0x0E, IntPtr.Zero, IntPtr.Zero);
    var sb = new StringBuilder(n + 1); SendMessageW(h, 0x0D, (IntPtr)(n + 1), sb); return sb.ToString();
  }
  // EM_POSFROMCHAR: where character i is drawn, in client coordinates; null if not shown
  public static POINT? CharPos(IntPtr edit, int i) {
    long v = (long)SendMessageW(edit, 0xD6, (IntPtr)i, IntPtr.Zero);
    if (v == -1) return null;
    var p = new POINT(); p.X = (short)(v & 0xFFFF); p.Y = (short)((v >> 16) & 0xFFFF); return p;
  }
}
'@
}

# the folder part of a file path: a drive, then folders, then a name with an extension
$script:ShotPathDirs = '(?<![\w\\])[A-Za-z]:\\(?:[^\\/:*?"<>|\r\n]+\\)*(?=\w[^\\/:*?"<>|\r\n]*\.\w)'

function Save-Shot([IntPtr]$hwnd, [string]$file) {
    [ShotW]::SetProcessDPIAware() | Out-Null      # real pixels, as PrintWindow draws them
    $wr = New-Object ShotW+RECT
    [ShotW]::GetWindowRect($hwnd, [ref]$wr) | Out-Null

    # static text cannot say where a character is drawn: mask its text while the window is printed
    $restore = @()
    foreach ($c in [ShotW]::Children($hwnd)) {
        if ([ShotW]::Class($c) -ne 'Static') { continue }
        $t = [ShotW]::Text($c)
        $masked = [regex]::Replace($t, $script:ShotPathDirs, { param($m) [string]([char]0x2588) * $m.Length })
        if ($masked -ne $t) {
            [ShotW]::SendMessageW($c, 0x0C, [IntPtr]::Zero, $masked) | Out-Null       # WM_SETTEXT
            $restore += , @($c, $t)
        }
    }

    $bmp = New-Object Drawing.Bitmap ($wr.R - $wr.L), ($wr.B - $wr.T)
    $gr = [Drawing.Graphics]::FromImage($bmp)
    $hdc = $gr.GetHdc()
    [ShotW]::PrintWindow($hwnd, $hdc, 2) | Out-Null
    $gr.ReleaseHdc($hdc)
    foreach ($r in $restore) { [ShotW]::SendMessageW($r[0], 0x0C, [IntPtr]::Zero, [string]$r[1]) | Out-Null }

    # edit boxes (file names, the log): a block over each folder, where the edit control drew it
    $ink = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(32, 32, 32))
    foreach ($c in [ShotW]::Children($hwnd)) {
        if ([ShotW]::Class($c) -ne 'Edit' -or -not [ShotW]::IsWindowVisible($c)) { continue }
        $t = [ShotW]::Text($c)
        $ms = [regex]::Matches($t, $script:ShotPathDirs)
        if ($ms.Count -eq 0) { continue }
        $hf = [ShotW]::SendMessageW($c, 0x31, [IntPtr]::Zero, [IntPtr]::Zero)          # WM_GETFONT
        $font = if ($hf -ne [IntPtr]::Zero) { [Drawing.Font]::FromHfont($hf) } else { [Drawing.SystemFonts]::DefaultFont }
        $lineH = [Windows.Forms.TextRenderer]::MeasureText('Ag', $font).Height
        $cr = New-Object ShotW+RECT
        [ShotW]::GetClientRect($c, [ref]$cr) | Out-Null
        $o = New-Object ShotW+POINT
        [ShotW]::ClientToScreen($c, [ref]$o) | Out-Null
        $ox = $o.X - $wr.L; $oy = $o.Y - $wr.T
        foreach ($m in $ms) {
            $a = [ShotW]::CharPos($c, $m.Index)
            $b = [ShotW]::CharPos($c, $m.Index + $m.Length)                         # the name's first character
            if ($null -eq $a -and $null -eq $b) { continue }
            # (PowerShell unwraps the nullable: $a and $b are POINTs or $null)
            if ($null -eq $a) { $a = New-Object ShotW+POINT; $a.X = 0; $a.Y = $b.Y }
            if ($null -eq $b) { $b = New-Object ShotW+POINT; $b.X = $cr.R; $b.Y = $a.Y }
            # a folder wrapped over several lines: to the end of each line, then up to the name
            for ($y = $a.Y; $y -le $b.Y; $y += $lineH) {
                $x0 = if ($y -eq $a.Y) { $a.X } else { 0 }
                $x1 = if ($y + $lineH -gt $b.Y) { $b.X } else { $cr.R }
                $x0 = [Math]::Max(0, $x0) - 1; $x1 = [Math]::Min($cr.R, $x1)
                if ($x1 -le $x0 -or $y + $lineH -le 0 -or $y -ge $cr.B) { continue }
                $gr.FillRectangle($ink, $ox + $x0, $oy + [Math]::Max(0, $y), $x1 - $x0, [Math]::Min($lineH, $cr.B - $y))
            }
        }
    }
    $ink.Dispose()
    $bmp.Save($file, [Drawing.Imaging.ImageFormat]::Png)
    $gr.Dispose(); $bmp.Dispose()
}
