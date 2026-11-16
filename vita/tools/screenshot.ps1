# Captura la ventana principal de un proceso a PNG.
# Uso: powershell.exe -NoProfile -ExecutionPolicy Bypass -File screenshot.ps1 -ProcessName Vita3K -Out C:\ruta\shot.png
# Usa PrintWindow(PW_RENDERFULLCONTENT) para que funcione aunque la ventana esté tapada;
# si sale negra, prueba CopyFromScreen trayendo la ventana al frente.
param(
  [string]$ProcessName = "Vita3K",
  [Parameter(Mandatory = $true)][string]$Out
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W32 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
}
"@
[W32]::SetProcessDPIAware() | Out-Null

$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
  Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
if (-not $proc) { Write-Error "No hay ventana del proceso $ProcessName"; exit 2 }
$h = $proc.MainWindowHandle

$r = New-Object W32+RECT
[W32]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hgt = $r.B - $r.T
if ($w -le 0 -or $hgt -le 0) { Write-Error "Ventana con tamaño inválido ($w x $hgt)"; exit 3 }

function Test-AllBlack([System.Drawing.Bitmap]$b) {
  for ($i = 1; $i -lt 10; $i++) {
    $p = $b.GetPixel([int]($b.Width * $i / 10), [int]($b.Height / 2))
    if (($p.R + $p.G + $p.B) -gt 30) { return $false }
  }
  return $true
}

$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[W32]::PrintWindow($h, $hdc, 2) | Out-Null  # 2 = PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc)

if (Test-AllBlack $bmp) {
  [W32]::ShowWindow($h, 9) | Out-Null  # SW_RESTORE
  [W32]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 500
  $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
}
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "captura: $Out ($w x $hgt)"
