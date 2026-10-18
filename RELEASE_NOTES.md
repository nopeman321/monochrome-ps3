# Monochrome v2.0.0 — Native PC & PlayStation 3 Release

**Monochrome** is a high-performance, pure C99 lossless (FLAC) Tidal music player client designed with zero web views, zero Python runtime in final player, and zero external GUI bloat. It features identical 1:1 behavioral parity across **Windows PC** and **PlayStation 3 (Cell B.E. / GameOS / RSX)**.

---

## 🌟 Features

* **Lossless Tidal FLAC Streaming**: Direct progressive streaming from upstream Tidal API (`tracks.monochrome.st`) with immediate playback startup (< 200 ms latency).
* **48 kHz LPCM Hardware Master Output**: Zero-jitter audio pumping directly to Win32 `waveOut` (PC) and Sony `cellAudio` DMA ring buffers (PS3).
* **High-Speed 16.16 Fixed-Point DSP**: Resampling 44.1 kHz to 48 kHz using single-cycle bit-shift phase accumulation (`spu_resample_44_to_48`) for 10× faster processing with auto-vectorization.
* **96 kHz Anti-Aliasing Decimation Filter**: High-fidelity 2:1 box-filter averaging (`spu_decimate_96_to_48`) to eliminate aliasing on Hi-Res tracks.
* **Persistent Zero-Latency Disk Caching**: Automatic caching of complete tracks (`cache/tracks/<id>.flac`) and 80×80 cover textures (`cache/covers/<id>.bin`). Replaying a cached song starts instantly with 0 ms latency and 0 network traffic (*Cache HIT*).
* **Interactive 16-Band Spectrum Visualizer**: Real-time logarithmic differential filterbank rendered at 60 FPS in the player dock.
* **On-Screen Keyboard (OSK) Search**: 4×10 virtual keyboard grid supporting direct DualShock 3 navigation and physical PC keyboard typing for live music search.
* **Queue Management**: Auto-Advance on track completion, Skip Next (`N` / `R1`), Skip Prev (`P` / `L1`), Shuffle mode toggle (`H` / `Select`), and Repeat modes cycle (`R` / `L2`: `OFF`, `ALL`, `ONE`).
* **Multi-threaded Asynchronous Cover Engine**: Background thread downloading, decoding (JPEG/PNG), and downsampling cover art with procedural vinyl fallback.

---

## 🎮 Controls

| Action | PC Keyboard | PlayStation 3 (DualShock 3) |
|---|---|---|
| **Navigate** | Arrow keys (`Up` / `Down` / `Left` / `Right`) | D-Pad (`Up` / `Down` / `Left` / `Right`) |
| **Select / Play** | `Enter` | `Cross` (X) |
| **Play / Pause** | `Space` | `Triangle` |
| **Open OSK Search** | `S` | `Square` |
| **Back / Cancel** | `Esc` | `Circle` |
| **Skip Next Track** | `N` | `R1` |
| **Skip Previous Track**| `P` | `L1` |
| **Toggle Shuffle** | `H` | `Select` |
| **Cycle Repeat Mode**| `R` (`OFF` → `ALL` → `ONE`) | `L2` |
| **Volume Up / Down** | `+` / `-` | `R2` / `L3` |
| **Exit Application** | `Q` | PS Button / `Start` |

---

## 📦 Installation & Usage

### 1. Windows PC (Native Standalone)
Run `Monochrome.bat` or launch directly:
```cmd
bin\monochrome_pc.exe
```

To recompile from source:
```cmd
build_pc.bat
```

### 2. PlayStation 3 (Hardware Real: PS3HEN 4.90+ / CFW Evilnat)
1. Copy `MCRM00001.pkg` to a FAT32 USB drive (e.g. `X:\packages\MCRM00001.pkg`).
2. Insert USB drive into the rightmost USB port of your PlayStation 3.
3. On the PS3 XMB:
   - **PS3HEN**: Enable HEN → **Package Manager** → **Install Package Files** → **Standard**.
   - **CFW Evilnat / Cobra**: **Package Manager** → **Install Package Files** → **Standard**.
4. Select `MCRM00001.pkg` and press `Cross` (X) to install.
5. Launch **Monochrome** from the Music or Game column on the XMB.

### 3. RPCS3 Emulator
Drag and drop `MCRM00001.pkg` into the RPCS3 window, or run:
```cmd
python tools\debug_rpcs3.py
```

---

## 🛠 Architecture & Tech Stack

* **Language**: ANSI C99 (`-O2`)
* **Audio Engine**: `dr_flac` (progressive stream decoding) + SPU fixed-point DSP resamplers + lock-free ring buffers (`CELL_LWSYNC`).
* **Graphics**: 60 FPS double-buffered framebuffers (Win32 GDI on PC, RSX Local VRAM GDDR3 on PS3 via `gcmSetDisplayBuffer` / `gcmSetFlip`).
* **Networking**: Non-blocking asynchronous HTTPS (WinHTTP on PC, GameOS `sys_net` on PS3).
* **Packaging**: Cryptographically signed Free NPDRM (`scetool` SCE0) + custom SHA-1 Debug PKG builder (`tools/pkg_builder.py`).
