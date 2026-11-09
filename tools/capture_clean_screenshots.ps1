# tools/capture_clean_screenshots.ps1 - Captura limpia y verificada de las pantallas oficiales de Monochrome PS3
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
using System; using System.Text; using System.Runtime.InteropServices;
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

  public static void Stabilize(IntPtr h) {
    if (IsIconic(h)) { ShowWindow(h, 9); }
    ShowWindow(h, 3); // SW_MAXIMIZE
    SetWindowPos(h, new IntPtr(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0040);
    SetForegroundWindow(h);
    BringWindowToTop(h);
  }

  public static void Key(byte vk) {
    keybd_event(vk, 0, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(70);
    keybd_event(vk, 0, 2, UIntPtr.Zero);
    System.Threading.Thread.Sleep(100);
  }
}
"@

$VK = @{ X=0x58; Z=0x5A; V=0x56; C=0x43; SPACE=0x20; UP=0x26; DOWN=0x28; LEFT=0x25; RIGHT=0x27; L1=0x51; R1=0x45; ENTER=0x0D }

function Capture-Client([IntPtr]$h, [string]$outPath) {
  [WinCap]::Stabilize($h)
  Start-Sleep -Milliseconds 400

  $rc = New-Object WinCap+RECT
  [WinCap]::GetClientRect($h, [ref]$rc)
  $pt = New-Object WinCap+POINT
  $pt.X = 0; $pt.Y = 0
  [WinCap]::ClientToScreen($h, [ref]$pt)
  $w = $rc.Right - $rc.Left
  $h_px = $rc.Bottom - $rc.Top

  if ($w -lt 200 -or $h_px -lt 200) {
    $rw = New-Object WinCap+RECT
    [WinCap]::GetWindowRect($h, [ref]$rw)
    $pt.X = $rw.Left; $pt.Y = $rw.Top
    $w = $rw.Right - $rw.Left
    $h_px = $rw.Bottom - $rw.Top
  }

  $bmp = New-Object System.Drawing.Bitmap($w, $h_px)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, (New-Object System.Drawing.Size($w, $h_px)))
  $g.Dispose()
  $bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
}

try {
  Write-Host "[1/6] Iniciando RPCS3..." -ForegroundColor Cyan
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
  Write-Host "[OK] Ventana encontrada: $hwnd" -ForegroundColor Green

  # Espera para carga inicial de la API y renderizado del splash
  Write-Host "[..] Esperando inicializacion de motor (10s)..." -ForegroundColor Yellow
  Start-Sleep -Seconds 10
  [WinCap]::Stabilize($hwnd)
  Start-Sleep -Seconds 1

  # --- 1. Pantalla HOME ---
  Write-Host "[2/6] Capturando HOME (8 pestañas)..." -ForegroundColor Cyan
  $homePath = Join-Path $shotsDir 'home.png'
  Capture-Client $hwnd $homePath
  Copy-Item $homePath (Join-Path $evDir 'screenshot_home.png') -Force

  # --- 2. Pantalla SEARCH con 5 sub-pestañas y OSK ---
  Write-Host "[3/6] Navegando y capturando SEARCH..." -ForegroundColor Cyan
  [WinCap]::Key($VK['C'])  # Square -> STATE_SEARCH
  Start-Sleep -Milliseconds 1500
  $searchPath = Join-Path $shotsDir 'search.png'
  Capture-Client $hwnd $searchPath
  Copy-Item $searchPath (Join-Path $evDir 'screenshot_search.png') -Force

  # Retornar a HOME
  [WinCap]::Key($VK['Z'])  # Circle -> STATE_HOME
  Start-Sleep -Milliseconds 1000

  # --- 3. Pantalla LIBRARY ---
  Write-Host "[4/6] Navegando y capturando LIBRARY..." -ForegroundColor Cyan
  [WinCap]::Key($VK['R1']) # R1 -> SEARCH
  Start-Sleep -Milliseconds 500
  [WinCap]::Key($VK['R1']) # R1 -> LIBRARY
  Start-Sleep -Milliseconds 1500
  $libPath = Join-Path $shotsDir 'library.png'
  Capture-Client $hwnd $libPath
  Copy-Item $libPath (Join-Path $evDir 'screenshot_library.png') -Force

  # --- 4. Pantalla UNRELEASED ---
  Write-Host "[5/6] Navegando y capturando UNRELEASED..." -ForegroundColor Cyan
  [WinCap]::Key($VK['R1']) # R1 -> RECENT
  Start-Sleep -Milliseconds 500
  [WinCap]::Key($VK['R1']) # R1 -> UNRELEASED
  Start-Sleep -Milliseconds 1500
  $unrPath = Join-Path $shotsDir 'unreleased.png'
  Capture-Client $hwnd $unrPath
  Copy-Item $unrPath (Join-Path $evDir 'screenshot_unreleased.png') -Force

  # Retornar a HOME
  [WinCap]::Key($VK['Z'])  # Circle -> STATE_HOME
  Start-Sleep -Milliseconds 1000

  # --- 5. Pantalla PLAYING (reproduccion en vivo con caratula y visualizador) ---
  Write-Host "[6/6] Iniciando reproduccion y capturando PLAYING..." -ForegroundColor Cyan
  [WinCap]::Key($VK['X'])  # Cross -> Play track
  # Esperar 4.5 segundos para que descargue la caratula y anime el espectro
  Start-Sleep -Milliseconds 4500
  $playPath = Join-Path $shotsDir 'playing.png'
  Capture-Client $hwnd $playPath
  Copy-Item $playPath (Join-Path $evDir 'screenshot_playing.png') -Force

  if (-not $KeepRunning) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
  }

  Write-Host "`n[OK] Todas las capturas generadas y guardadas exitosamente en docs/screenshots/" -ForegroundColor Green

} catch {
  Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
}
