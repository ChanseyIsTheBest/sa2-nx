# Captures a window (by process name) or the whole primary screen to a PNG.
# Usage: powershell -File tools\screenshot.ps1 -Out shot.png [-Process renut]
param([string]$Out = "shot.png", [string]$Process = "", [string]$PathLike = "")
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[W]::SetProcessDPIAware() | Out-Null
$rect = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
if ($Process -ne "") {
  $p = Get-Process -Name $Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 -and ($PathLike -eq "" -or $_.Path -like $PathLike) } | Select-Object -First 1
  if ($p) {
    [W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 300
    $r = New-Object W+RECT
    [W]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $rect = New-Object System.Drawing.Rectangle($r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T))
  } else { Write-Output "process $Process has no window; capturing screen" }
}
$bmp = New-Object System.Drawing.Bitmap($rect.Width, $rect.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($rect.Location, [System.Drawing.Point]::Empty, $rect.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "saved $Out ($($rect.Width)x$($rect.Height))"
