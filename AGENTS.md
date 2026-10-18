# Monochrome PS3 — OpenCode Agent Guide & Architecture

## Overview
**Monochrome PS3** is an ultra-low-latency, lossless audio streaming client (Tidal Hi-Res via Monochrome API) supporting three execution targets:
1. **Windows Desktop Client**: Standalone desktop window (via Edge/Chromium App Mode + local daemon in `tools/desktop_app.py`) and native C launcher (`bin/monochrome_pc.exe`).
2. **Web Client**: HTML5/CSS3/ES6+ standalone interface with responsive compact layout (`web/index.html`, `web/styles.css`, `web/app.js`).
3. **PlayStation 3 Homebrew (Cell B.E. / GameOS / RPCS3)**: Compiled for PowerPC64, signed as Free NPDRM SELF (`EBOOT.BIN`), and packaged as retail/homebrew PKG (`MCRM00001.pkg`).

---

## Workspace Directory & Key Files
- `src/main.c`: Core application lifecycle, state machine, and main loop.
- `src/platform_pc.c`: Host Windows audio, windowing, and HTTP implementation.
- `src/platform_ps3.c`: GameOS / Cell B.E. HAL (`ioPadInit`, `audioPortOpen`, RSX framebuffer 720x480).
- `src/audio.c` & `src/flac_decoder.c`: 48 kHz stereo LPCM resampler and FLAC decoder.
- `src/network.c`: Upstream Monochrome / Tidal REST API client with offline fallback catalogue.
- `tools/launcher.c`: Native Win32 launcher compiling to `bin/monochrome_pc.exe`.
- `tools/desktop_app.py`: Background HTTP server (port 7890) and standalone app mode launcher.
- `tools/pkg_builder.py`: Debug PKG builder with mandatory **16-byte aligned filename offsets** for SHA-1 stream cipher compatibility with RPCS3 and PS3HEN.
- `tools/configure_rpcs3.py`: Automated configuration synchronizer for RPCS3 (Vulkan, 720x480, 100ms audio buffer, Internet connected).

---

## Build Commands
- **Build Windows PC Client**:
  ```cmd
  build_pc.bat
  ```
  Produces `bin\monochrome_pc.exe` and `bin\monochrome_c99_core.exe`.

- **Build PS3 Homebrew (ELF -> SELF -> PKG)**:
  ```cmd
  build_homebrew.bat
  ```
  Produces `MCRM00001.pkg` (Title ID: `MCRM00001`, Content ID: `UP0001-MCRM00001_00-MONOCHROMEPS3001`) and deploys to RPCS3 virtual HDD.

---

## Execution Commands
- **Run Windows Desktop Player**:
  ```cmd
  Monochrome.bat
  ```
  or
  ```cmd
  run_pc.bat
  ```

- **Run PS3 Homebrew in RPCS3**:
  ```cmd
  run_rpcs3.bat
  ```

- **Run Web Client in Browser**:
  ```cmd
  run_web.bat
  ```
