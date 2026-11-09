# tools/capture_clean_screenshots.ps1 - Captura limpia con prioridad absoluta en ventana y fullscreen
param(
  [switch]$KeepRunning
)

$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
$rpcs3 = Join-Path $repo 'emulator\rpcs3'
$shotsDir = Join-Path $repo 'docs\screenshots'
$evDir = Join-Path $repo 'evidence'

New-Item -ItemType Directory -Force -Path $shotsDir | Out-Null
New-Item -ItemType Directory -Force -Path $evDir | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices; using System.Drawing;
public static class WinCap {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint uCode, uint uMapType);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);

  public delegate bool EnumProc(IntPtr h, IntPtr l);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public struct POINT { public int X; public int Y; }

  public static IntPtr Find(uint pid, string needle) {
    IntPtr f = IntPtr.Zero;
    EnumWindows((h, l) => {
      uint wp;
      GetWindowThreadProcessId(h, out wp);
      if (wp != pid) return true;
      var sb = new StringBuilder(512);
      GetWindowText(h, sb, 512);
      if (sb.ToString().Contains(needle)) { f = h; return false; }
      return true;
    }, IntPtr.Zero);
    return f;
  }

  public static void ForceTopmost(IntPtr h) {
    ShowWindow(h, 9); // SW_RESTORE
    ShowWindow(h, 3); // SW_MAXIMIZE
    SetWindowPos(h, new IntPtr(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0040); // HWND_TOPMOST
    BringWindowToTop(h);
    SetForegroundWindow(h);
    System.Threading.Thread.Sleep(300);
  }

  public static void PressKey(byte vk) {
    uint scan = MapVirtualKey(vk, 0);
    keybd_event(vk, (byte)scan, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(80);
    keybd_event(vk, (byte)scan, 2, UIntPtr.Zero);
    System.Threading.Thread.Sleep(200);
  }
}
"@

# Mapeo exacto segun Default.yml de RPCS3:
# Square: X (0x58), Cross: Z (0x5A), Circle: C (0x43), Triangle: V (0x56)
# L1: Q (0x51), R1: E (0x45), L2: W (0x57), R2: R (0x52)
$KEY_SQUARE   = [byte]0x58  # 'X'
$KEY_CROSS    = [byte]0x5A  # 'Z'
$KEY_CIRCLE   = [byte]0x43  # 'C'
$KEY_TRIANGLE = [byte]0x56  # 'V'
$KEY_L1       = [byte]0x51  # 'Q'
$KEY_R1       = [byte]0x45  # 'E'

function Grab-Screen([IntPtr]$h, [string]$outPath) {
  [WinCap]::ForceTopmost($h)
  Start-Sleep -Milliseconds 600

  $rc = New-Object WinCap+RECT
  [WinCap]::GetClientRect($h, [ref]$rc)
  $pt = New-Object WinCap+POINT
  $pt.X = 0; $pt.Y = 0
  [WinCap]::ClientToScreen($h, [ref]$pt)

  $w = $rc.Right - $rc.Left
  $h_px = $rc.Bottom - $rc.Top

  if ($w -lt 400 -or $h_px -lt 300) {
    $rw = New-Object WinCap+RECT
    [WinCap]::GetWindowRect($h, [ref]$rw)
    $pt.X = $rw.Left; $pt.Y = $rw.Top
    $w = $rw.Right - $rw.Left
    $h_px = $rw.Bottom - $rw.Top
  }

  # Recorte exacto para eliminar la barra de menu superior de RPCS3 (26px) y la barra de estado inferior (24px)
  $top_bar = 26
  $bot_bar = 24
  $crop_y = $pt.Y + $top_bar
  $crop_h = $h_px - $top_bar - $bot_bar

  if ($crop_h -lt 200) {
    $crop_y = $pt.Y
    $crop_h = $h_px
  }

  $bmp = New-Object System.Drawing.Bitmap($w, $crop_h)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($pt.X, $crop_y, 0, 0, (New-Object System.Drawing.Size($w, $crop_h)))
  $g.Dispose()

  $bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Host "   -> Guardado (viewport puro): $(Split-Path -Leaf $outPath) ($w x $crop_h)" -ForegroundColor Green
}

try {
  Write-Host "[1/6] Minimizando otras ventanas e iniciando RPCS3..." -ForegroundColor Cyan
  (New-Object -ComObject Shell.Application).MinimizeAll()
  Start-Sleep -Seconds 1

  Get-Process -Name rpcs3 -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Seconds 2

  $p = Start-Process -FilePath (Join-Path $rpcs3 'rpcs3.exe') `
    -ArgumentList 'dev_hdd0/game/MONO00024/USRDIR/EBOOT.BIN' `
    -WorkingDirectory $rpcs3 -PassThru -WindowStyle Normal

  $hwnd = [IntPtr]::Zero
  for ($w = 0; $w -lt 30 -and $hwnd -eq [IntPtr]::Zero; $w++) {
    Start-Sleep -Milliseconds 800
    if ($p.HasExited) { break }
    $hwnd = [WinCap]::Find([uint32]$p.Id, 'Monochrome Music')
  }

  if ($hwnd -eq [IntPtr]::Zero) { throw "No se encontro la ventana Monochrome Music de RPCS3" }
  Write-Host "[OK] Ventana Monochrome encontrada: $hwnd" -ForegroundColor Green

  # Espera para carga de tracks desde tracks.monochrome.st (10 segundos)
  Write-Host "[..] Esperando carga de la API y estabilizacion de ventana (10s)..." -ForegroundColor Yellow
  Start-Sleep -Seconds 10
  [WinCap]::ForceTopmost($hwnd)
  Start-Sleep -Seconds 1

  # --- 1. Pantalla HOME ---
  Write-Host "[2/6] Capturando HOME (8 pestañas)..." -ForegroundColor Cyan
  $homePath = Join-Path $shotsDir 'home.png'
  Grab-Screen $hwnd $homePath
  Copy-Item $homePath (Join-Path $evDir 'screenshot_home.png') -Force

  # --- 2. Pantalla SEARCH con 5 sub-pestañas y OSK ---
  Write-Host "[3/6] Navegando a SEARCH (R1)..." -ForegroundColor Cyan
  [WinCap]::PressKey($KEY_R1) # HOME -> SEARCH
  Start-Sleep -Milliseconds 1500
  $searchPath = Join-Path $shotsDir 'search.png'
  Grab-Screen $hwnd $searchPath
  Copy-Item $searchPath (Join-Path $evDir 'screenshot_search.png') -Force

  # --- 3. Pantalla LIBRARY ---
  Write-Host "[4/6] Navegando a LIBRARY (R1)..." -ForegroundColor Cyan
  [WinCap]::PressKey($KEY_R1) # SEARCH -> LIBRARY
  Start-Sleep -Milliseconds 1500
  $libPath = Join-Path $shotsDir 'library.png'
  Grab-Screen $hwnd $libPath
  Copy-Item $libPath (Join-Path $evDir 'screenshot_library.png') -Force

  # --- 4. Pantalla UNRELEASED ---
  Write-Host "[5/6] Navegando a UNRELEASED (R1 x 2)..." -ForegroundColor Cyan
  [WinCap]::PressKey($KEY_R1) # LIBRARY -> RECENT
  Start-Sleep -Milliseconds 800
  [WinCap]::PressKey($KEY_R1) # RECENT -> UNRELEASED
  Start-Sleep -Milliseconds 1500
  $unrPath = Join-Path $shotsDir 'unreleased.png'
  Grab-Screen $hwnd $unrPath
  Copy-Item $unrPath (Join-Path $evDir 'screenshot_unreleased.png') -Force

  # Retornar a HOME con L1 x 4
  Write-Host "   -> Retornando a HOME (L1 x 4)..."
  [WinCap]::PressKey($KEY_L1)
  Start-Sleep -Milliseconds 400
  [WinCap]::PressKey($KEY_L1)
  Start-Sleep -Milliseconds 400
  [WinCap]::PressKey($KEY_L1)
  Start-Sleep -Milliseconds 400
  [WinCap]::PressKey($KEY_L1)
  Start-Sleep -Milliseconds 1200

  # --- 5. Pantalla PLAYING (reproduccion en vivo con caratula y visualizador) ---
  Write-Host "[6/6] Iniciando reproduccion (pulsando Cruz)..." -ForegroundColor Cyan
  [WinCap]::PressKey($KEY_CROSS)
  # Esperar 5 segundos para que descargue la caratula y anime el espectro
  Start-Sleep -Seconds 5
  $playPath = Join-Path $shotsDir 'playing.png'
  Grab-Screen $hwnd $playPath
  Copy-Item $playPath (Join-Path $evDir 'screenshot_playing.png') -Force

  if (-not $KeepRunning) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
  }

  Write-Host "`n[OK] Todas las capturas generadas con prioridad exclusiva y guardadas con exito." -ForegroundColor Green

} catch {
  Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
}
