# verify_runtime.ps1 - Lanza RPCS3, estabiliza la ventana, captura y resume en JSON.
# Uso: pwsh -File tools\verify_runtime.ps1 -Tag "home" -Keys "" -Shots 3 [-Play 1]
param(
  [string]$Tag = "run",
  [string]$Keys = "",          # teclas separadas por coma: X,Z,V,C,UP,DOWN,LEFT,RIGHT,SPACE,L1,R1
  [int]$Shots = 3,
  [int]$WaitMs = 9000,         # espera tras arrancar antes de la primera captura
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
$rpcs3 = Join-Path $repo 'emulator\rpcs3'
$ev = Join-Path $repo 'evidence'
New-Item -ItemType Directory -Force -Path $ev | Out-Null
$stamp = Get-Date -Format 'HHmmss'
$result = [ordered]@{ tag=$Tag; started=(Get-Date).ToString('s'); build_mtime=''; fps=@(); shots=@(); tty=@(); errors=@(); title='' }

# --- build actual (no recompila: solo reporta mtime de artefactos) ---
$elf = Join-Path $repo 'build\EBOOT.ELF'
if (Test-Path $elf) { $result.build_mtime = (Get-Item $elf).LastWriteTime.ToString('s') }

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public static class VR {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public static void ForceForeground(IntPtr h) {
    IntPtr fg = GetForegroundWindow();
    uint fgPid; uint fgTid = GetWindowThreadProcessId(fg, out fgPid);
    uint myTid = GetCurrentThreadId();
    bool attached = AttachThreadInput(myTid, fgTid, true);
    try {
      ShowWindow(h, 9); ShowWindow(h, 3);
      SetWindowPos(h, new IntPtr(-1), 0,0,0,0, 0x0001|0x0002|0x0040);
      BringWindowToTop(h);
      SetForegroundWindow(h);
      keybd_event(0x12, 0, 0, UIntPtr.Zero);   // ALT down
      keybd_event(0x12, 0, 2, UIntPtr.Zero);   // ALT up
      SetForegroundWindow(h);
    } finally { if (attached) AttachThreadInput(myTid, fgTid, false); }
  }
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  public static IntPtr Find(uint pid, string needle) {
    IntPtr f = IntPtr.Zero;
    EnumWindows((h,l) => { uint wp; GetWindowThreadProcessId(h, out wp); if (wp!=pid) return true;
      var sb=new StringBuilder(512); GetWindowText(h,sb,512);
      if (sb.ToString().Contains(needle)) { f=h; return false; } return true; }, IntPtr.Zero);
    return f;
  }
  public static string Title(IntPtr h){ var sb=new StringBuilder(512); GetWindowText(h,sb,512); return sb.ToString(); }
  public static void Key(byte vk){ keybd_event(vk,0,0,UIntPtr.Zero); System.Threading.Thread.Sleep(50); keybd_event(vk,0,2,UIntPtr.Zero); }
  // Flechas: RPCS3 (Qt) necesita VK real + scan code + KEYEVENTF_EXTENDEDKEY
  public static void KeyScan(byte vk, byte scan) {
    keybd_event(vk, scan, 0x0001, UIntPtr.Zero);
    System.Threading.Thread.Sleep(60);
    keybd_event(vk, scan, 0x0001 | 0x0002, UIntPtr.Zero);
  }
}
"@
$VK = @{ X=0x58; Z=0x5A; V=0x56; C=0x43; SPACE=0x20; UP=0x26; DOWN=0x28; LEFT=0x25; RIGHT=0x27; L1=0x51; R1=0x45; ENTER=0x0D }

function Stabilize([IntPtr]$h) {
  # Metodo simple: AttachThreadInput rompia la entrega de teclas en RPCS3 y no mejoraba el foco.
  if ([VR]::IsIconic($h)) { [void][VR]::ShowWindow($h, 9) }
  [void][VR]::ShowWindow($h, 3)
  [void][VR]::SetWindowPos($h, [IntPtr]::new(-1), 0, 0, 0, 0, 0x0001 -bor 0x0002 -bor 0x0040)
  [void][VR]::SetForegroundWindow($h)
}

try {
  Get-Process -Name rpcs3 -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Seconds 2
  $p = Start-Process -FilePath (Join-Path $rpcs3 'rpcs3.exe') -ArgumentList 'dev_hdd0/game/MCRM00001/USRDIR/EBOOT.BIN' -WorkingDirectory $rpcs3 -PassThru -WindowStyle Normal
  Start-Sleep -Seconds 4
  $hwnd = [VR]::Find([uint32]$p.Id, 'Monochrome Music')
  if ($hwnd -eq [IntPtr]::Zero) { throw 'no se encontro la ventana Monochrome Music' }
  Start-Sleep -Milliseconds $WaitMs

  if ($Keys) {
    $hwnd = [VR]::Find([uint32]$p.Id, 'Monochrome Music')
    Stabilize $hwnd
    Start-Sleep -Milliseconds 700
    foreach ($k in ($Keys -split ',')) {
      $k = $k.Trim().ToUpper()
      # La ventana se minimiza sin avisar: re-estabilizar antes de CADA tecla
      $hh = [VR]::Find([uint32]$p.Id, 'Monochrome Music')
      if ($hh -ne [IntPtr]::Zero) { Stabilize $hh; Start-Sleep -Milliseconds 500 }
      $scans = @{ UP = @(0x26,0x48); DOWN = @(0x28,0x50); LEFT = @(0x25,0x4B); RIGHT = @(0x27,0x4D) }
      if ($scans.ContainsKey($k)) { [VR]::KeyScan([byte]$scans[$k][0], [byte]$scans[$k][1]) }
      elseif ($VK.ContainsKey($k)) { [VR]::Key([byte]$VK[$k]) }
      Start-Sleep -Milliseconds 700
    }
    Start-Sleep -Milliseconds 800
  }

  $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
  for ($i = 0; $i -lt $Shots; $i++) {
    # La ventana se minimiza intermitentemente y RPCS3 puede recrearla: re-buscar handle.
    $ok = $false
    for ($try = 0; $try -lt 6 -and -not $ok; $try++) {
      $hwnd = [VR]::Find([uint32]$p.Id, 'Monochrome Music')
      if ($hwnd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 300; continue }
      Stabilize $hwnd
      Start-Sleep -Milliseconds 450
      if (-not [VR]::IsIconic($hwnd)) { $ok = $true }
    }
    if (-not $ok) { $result.errors += "shot $i : ventana minimizada" }
    $result.title = [VR]::Title($hwnd)
    if ($result.title -match 'FPS:\s*([0-9.]+)') { $result.fps += [double]$matches[1] }
    $path = Join-Path $ev ("{0}_{1}_{2}.png" -f $Tag,$stamp,$i)
    $bmp = New-Object System.Drawing.Bitmap($b.Width, $b.Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.X, $b.Y, 0, 0, $b.Size); $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    $result.shots += (Split-Path -Leaf $path)
    Start-Sleep -Milliseconds 250
  }

  # --- logs (copiar primero: RPCS3 mantiene el fichero abierto) ---
  $ttySrc = Join-Path $rpcs3 'log\TTY.log'
  $ttyTmp = Join-Path $env:TEMP "tty_$stamp.log"
  if (Test-Path $ttySrc) { Copy-Item $ttySrc $ttyTmp -Force -ErrorAction SilentlyContinue }
  if (Test-Path $ttyTmp) {
    $lines = Get-Content $ttyTmp
    $result.tty = @($lines | Select-String -Pattern 'RSX\] local_base|buf0 ea|PRESENT|AUDIO\] State|LPCM|STREAM\] SUCCESS|FLAC Format|NOW PLAYING|HTTP\]|INPUT\]|UI\]|ERROR|Error|underrun' | Select-Object -Last 40 -ExpandProperty Line)
  }
  $logSrc = Join-Path $rpcs3 'log\RPCS3.log'
  $logTmp = Join-Path $env:TEMP "rpcs3_$stamp.log"
  if (Test-Path $logSrc) { Copy-Item $logSrc $logTmp -Force -ErrorAction SilentlyContinue }
  if (Test-Path $logTmp) {
    $result.errors = @(Get-Content $logTmp | Select-String -Pattern 'SIG:|Access violation|Guru Meditation|CELL_ENOENT|PPU Trap|deadlock' | Select-Object -Last 10 -ExpandProperty Line)
  }

  # --- puntuacion de las capturas (detecta frames negros/tapados) ---
  $pyFile = Join-Path $ev "_score_thumb.py"
  @'
import sys, os
from PIL import Image, ImageStat
best = 0
for f in sys.argv[1:]:
    try:
        im = Image.open(f).convert('RGB')
        crop = im.crop((8, 32, im.width-8, im.height-40))
        colors = len(crop.getcolors(maxcolors=1000000) or [])
        st = ImageStat.Stat(crop)
        print(f"{os.path.basename(f)} colors={colors} stddev={round(sum(st.stddev)/3,1)}")
        if colors > best: best = colors
    except Exception as e:
        print("ERR", f, e)
print("BEST_COLORS=" + str(best))
'@ | Set-Content -Path $pyFile -Encoding UTF8
  $shotPaths = $result.shots | ForEach-Object { Join-Path $ev $_ }
  $score = & python $pyFile @shotPaths 2>&1
  $score | ForEach-Object { Write-Output $_ }
  $bestLine = ($score | Select-String 'BEST_COLORS=(\d+)' | Select-Object -Last 1)
  if ($bestLine -and $bestLine.Matches.Count -gt 0) { $result.best_colors = [int]$bestLine.Matches[0].Groups[1].Value }
  Remove-Item $pyFile -Force -ErrorAction SilentlyContinue

  if (-not $KeepRunning) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
} catch {
  $result.errors += ("EXCEPTION: " + $_.Exception.Message)
} finally {
  $json = $result | ConvertTo-Json -Depth 5
  $out = Join-Path $ev "verify_$($Tag)_$stamp.json"
  Set-Content -Path $out -Value $json -Encoding UTF8
  Write-Output $json
  Write-Output ("REPORT=" + $out)
}
