# Monochrome PS3 — Architecture & Agent Reference Guide

## Overview
**Monochrome PS3** is an ultra-low-latency, lossless FLAC audio streaming client built exclusively for the **Sony PlayStation 3 (Cell B.E. / GameOS / RSX)** and tested on the **RPCS3** emulator.

---

## Workspace Directory & Key Files
- `source/main.c`: Core application lifecycle, loop timing and adaptive frame pacing.
- `source/platform_psl1ght.c`: GameOS / Cell B.E. HAL (RSX framebuffer in local memory `0xC0000000`, `cellPad`, `cellAudio`).
- `source/http_client.c`: HTTP/1.0 client for streaming audio.
- `source/audio_engine.c`: Audio engine (ring buffer + playback) — modificado solo en Fase 0B y Fase 6.
- `source/cover_art.c`: Async album artwork downloader and 80×80 downsampler.
- `source/ui.c`: Controller input state machine, track navigation, and On-Screen Keyboard.
- `source/rsxutil.c`: RSX utility helpers.
- `source/cJSON.c`: JSON parser.
- `include/monochrome.h`: Central header with app constants, Title ID, and build stamp fallbacks.
- `include/platform.h`: Platform abstraction header.
- `include/ui.h`: UI state machine header.
- `include/http_client.h`: HTTP client header.
- `include/audio_engine.h`: Audio engine header.
- `include/cover_art.h`: Cover art header.
- `include/rsxutil.h`: RSX utils header.
- `pkgfiles/ICON0.PNG`, `pkgfiles/PIC1.PNG`: PKG icons.
- `Makefile`: Root build file (PSL1GHT v2).
- `tools/run_in_docker.ps1`: Docker build script using `ghcr.io/altps3/ps3dev:latest`.

---

## Build & Test Pipeline
- **Build PS3 Homebrew PKG & ELF (Docker)**:
  ```pwsh
  tools/run_in_docker.ps1
  ```
  Runs `make clean && make pkg` inside `ghcr.io/altps3/ps3dev:latest` and verifies
  `monochrome_ps3.pkg` exists and was recently modified.

- **Manual build**:
  ```sh
  make clean && make pkg
  ```

- **Title ID (unique, official)**: `MONO00024`
- **Output binary**: `monochrome_ps3.elf` / `monochrome_ps3.self` / `monochrome_ps3.pkg`
- **Source tree**: `source/` + `include/` only. No `src/` directory is used.

---

## Build Stamp
Each build injects `APP_BUILD_SHA` (short git SHA) and `APP_BUILD_TIME` (UTC) via `CFLAGS`.
`source/main.c` prints at startup, right after `platform_init()`:

```
[BUILD] Monochrome PS3 <version> | SHA: <sha> | Built: <time>
```

Fallbacks are defined in `include/monochrome.h` when the macros are not provided.

---

## Legacy (DO NOT USE)
The following are intentionally neutralized and must never be executed:
- `build_homebrew.bat` (legacy, produced `MCRM00001.pkg`)
- `scripts/build_and_pkg.sh` (legacy, produced `MCRM00001.pkg`)

Both have been moved to `legacy_unused/` and are excluded from the build pipeline.
The only official Title ID is `MONO00024`. `MCRM00001` is retired.
