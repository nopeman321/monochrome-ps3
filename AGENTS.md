# Monochrome PS3 — Architecture & Agent Reference Guide

## Overview
**Monochrome PS3** is an ultra-low-latency, lossless FLAC audio streaming client built exclusively for the **Sony PlayStation 3 (Cell B.E. / GameOS / RSX)** and tested on the **RPCS3** emulator.

---

## Workspace Directory & Key Files
- `src/main.c`: Core application lifecycle, loop timing and adaptive frame pacing.
- `src/platform_ps3.c`: GameOS / Cell B.E. HAL (RSX framebuffer in local memory `0xC0000000`, `cellPad`, `cellAudio`).
- `src/http_ps3.c`: Direct LV2 `sys_net` HTTP/1.0 and DNS A-record client with chunked/progressive stream support.
- `src/lv2.c`: Inline PowerPC64 assembly wrappers for LV2 microkernel syscalls.
- `src/audio.c`: 48 kHz stereo LPCM pumping, lock-free ring buffer, and 16-band spectrum visualizer.
- `src/network.c`: Streaming pipeline, track resolving, real-time seek, and persistent disk caching.
- `src/ui.c`: Controller input state machine, track navigation, and On-Screen Keyboard.
- `src/cover.c`: Async album artwork downloader and 80×80 downsampler.
- `src/flac_decoder.c`: Single-header dr_flac audio decoder backend.
- `tools/pkg_builder.py`: Debug PKG builder with 16-byte aligned filename offsets for RPCS3 and PS3HEN.
- `tools/sce_imports.py`: Binary patcher for SCE dynamic import tables (`cellGcmSys`, `cellAudio`, `sys_io`).
- `tools/configure_rpcs3.py`: Automated configuration synchronizer for RPCS3 (Vulkan, 720x480, Bilinear scaling).
- `tools/verify_runtime.ps1`: Automated runtime test harness for RPCS3 with image analysis and TTY verification.

---

## Build & Test Pipeline
- **Build PS3 Homebrew PKG & ELF**:
  ```cmd
  build_homebrew.bat
  ```
  Generates `build/EBOOT.ELF`, signs `pkg_build/USRDIR/EBOOT.BIN`, and builds `MCRM00001.pkg`.

- **Run in RPCS3**:
  ```cmd
  run_rpcs3.bat
  ```
  Launches the emulator with Vulkan 60 FPS rendering and XInput controller support.
