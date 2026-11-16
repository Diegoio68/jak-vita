# Captura UNA ventana concreta a PNG (nunca la pantalla completa).
# Uso:
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File screenshot.ps1 -ProcessName Vita3K -Out C:\ruta\shot.png
#   powershell.exe ... -File screenshot.ps1 -TitleLike "OpenGOAL*" -Out C:\ruta\shot.png
# -TitleLike busca entre las ventanas visibles la primera cuyo título encaje con el patrón
# (útil para el juego de PC, que WSLg muestra dentro del proceso msrdc junto a otras ventanas).
# No imprime títulos de otras ventanas.
# Usa PrintWindow(PW_RENDERFULLCONTENT) para que funcione aunque la ventana esté tapada;
# si sale negra, la trae al frente y copia solo el rectángulo de esa ventana.
param(
  [string]$ProcessName = "",
  [string]$TitleLike = "",
  [Parameter(Mandatory = $true)][string]$Out
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class W32 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
}
"@
[W32]::SetProcessDPIAware() | Out-Null

$h = [IntPtr]::Zero
if ($TitleLike -ne "") {
  $script:found = [IntPtr]::Zero
  $cb = [W32+EnumProc] {
    param($hw, $p)
    if (-not [W32]::IsWindowVisible($hw)) { return $true }
    $sb = New-Object System.Text.StringBuilder 512
    [W32]::GetWindowText($hw, $sb, 512) | Out-Null
    if ($sb.ToString() -like $TitleLike) { $script:found = $hw; return $false }
    return $true
  }
  [W32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
  $h = $script:found
} elseif ($ProcessName -ne "") {
  $proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
  if ($proc) { $h = $proc.MainWindowHandle }
}
if ($h -eq [IntPtr]::Zero) { Write-Error "No se encontró la ventana pedida"; exit 2 }

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
  $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)  # solo el rectángulo de la ventana
}
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "captura: $Out ($w x $hgt)"
