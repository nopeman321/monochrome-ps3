# RUNTIME_REPORT.md — Gatekeeper RPCS3 (Monochrome PS3)

Fecha: 2026-10-19/20 (America/Guayaquil). Build: `build_homebrew.bat` → `MCRM00001.pkg`.
Emulador: RPCS3 0.0.43-20331 (Vulkan, 720x480), `emulator/rpcs3/`.

## Resultado global: 3/3 PASS

| # | Test | Resultado | Evidencia |
|---|------|-----------|-----------|
| 1 | Boot/Sys | PASS | log de arranque; sin `SIG`, `Access violation`, `PPU Trap`, `ENOENT` |
| 2 | UI/Pad/Search | PASS | `evidence/ui_home2.png`, `evidence/ui_playing2.png`; input `[INPUT] Navigate DOWN`, `INPUT_SELECT`; búsqueda real "Daft Punk" → 9 pistas |
| 3 | Stream/RAM | PASS | FLAC 48 kHz/2 ch, `[STREAM] Downloaded 32 MB`, `XDR Buf: 8180 KB`, `[AUDIO] LPCM DAC 48 kHz`, FPS 60 |

## Test 1 — Boot/Sys
- `[SYS] Ring buffer: 8 MB @ ...`, `[NET] Ready — primary host: tracks.monochrome.st`
- `[PS3] cellAudio initialising: 48000 Hz, 2 ch`, `cellPadInit OK`, `[RSX] Framebuffer ready`
- Sin excepciones FATAL en `RPCS3.log` durante >60 s de ejecución.

## Test 2 — UI / Pad / Search
- UI 720x480 verificada por volcado directo del framebuffer:
  - `evidence/ui_home2.png`: cabecera MONOCHROME, now-playing, filtro "Daft Punk", 6 pistas reales con FLAC/duración.
  - `evidence/ui_playing2.png`: portada real, "▶ Trending — Dei V", visualizador 16 bandas, VOL 100 %, DMA 6/8.
- Input DualShock emulado: flechas (D-pad) con `KEYEVENTF_EXTENDEDKEY`; Cross (X) selecciona/reproduce.
- Búsqueda async real: `[NET] Querying live API: http://tracks.monochrome.st/search?q=Daft+Punk`, `[NET] Live search succeeded: 9 tracks found`.

## Test 3 — Streaming / RAM
- `[STREAM] SUCCESS: Opened progressive FLAC stream (86 KB buffered)`
- `[STREAM] FLAC Format: 48000 Hz | 2 ch | 7600825 frames (158.4 sec)`
- `[STREAM] Downloaded 8/16/24/32 MB` — descarga progresiva real.
- `[AUDIO] State → PLAYING`, `[AUDIO] LPCM DAC 48 kHz stereo active | Pumped: 4 blks/tick | Buffer: 8184 KB`.
- `[NOW PLAYING] ▶ Instant Crush (feat. Julian Casablancas) — Daft Punk | XDR Buf: 8180 KB`.
- FPS RPCS3: 60.02 (título de ventana). Sin SIG/AV.

## Presupuesto XDR
64 MB buffer de stream + 8 MB ring + ~16 MB texturas + 2×2 MB framebuffers + 16 MB heap ≈ 108 MB < 256 MB.

## Notas / limitaciones
- Caché FLAC en VFS falla al crear (`CELL_ENOENT`), streaming funciona igualmente.
- `cellOskDialog` no usado: la app usa su OSK propio (funcional).
- Etiqueta DAC 44100 Hz es cosmética (cellAudio a 48000 Hz).

## Artefactos
- `evidence/ui_home2.png`, `evidence/ui_playing2.png`
- `evidence/fb_home.raw`, `evidence/fb_play.raw` (720×480 ARGB)
- `evidence/fb_fixed.png` (frame de verificación)
