# RUNTIME_REPORT_V2 — Monochrome PS3 en RPCS3 (verificación real)

Fecha: 2026-10-20 00:2x (America/Guayaquil)
Build verificado: `build/EBOOT.ELF` 1.291.316 B, `pkg_build/USRDIR/EBOOT.BIN` 110.608 B,
`MCRM00001.pkg` 310.848 B, todos con mtime 2026-10-20 00:23:42.
Emulador: RPCS3 0.0.43-20331 (Vulkan, 720x480, VSync on).

## Resultado global: 3/3 PASS

| Test | Veredicto | Evidencia |
|---|---|---|
| 1. CORE/BOOT | **PASS** | build exit 0, arranque estable, cero SIG/AV/Guru/ENOENT/PPU Trap, 60 FPS |
| 2. RSX/UI/PAD | **PASS** | UI 720x480 visible y fluida (59.90-60.17 FPS), input Cross procesado |
| 3. NET/STREAMING | **PASS** | HTTP 200 real, FLAC 48 kHz progresivo, AUDIO PLAYING, sin fugas |

## Test 1 — CORE/BOOT
Build: `& .\build_homebrew.bat` → exit 0; `[SUCCESS] PS3 Debug PKG generated (310848 bytes)`;
`sce_imports: patched ... 21 imports in 3 modules, 9 phdrs, blob at index 4`.

Arranque (TTY.log):
```
[RSX] local_base=0xc0000000 size=0x01b00000 io=0x0f900000
[RSX] buf0 ea=0xc0000000 offset=0x00000000
[RSX] buf1 ea=0xc0200000 offset=0x00200000
[RSX] flipstat=0 p0=ff121212 fb0=ff121212
[RSX] Frame   780 (60.0 FPS) | State: 1 | Highlight [1/6]: Trending - Dei V
[RSX] PRESENT frames=1080 flip_cb=0
```
RPCS3.log: sin `SIG:`, `Access violation`, `Guru Meditation`, `CELL_ENOENT` ni `PPU Trap`.

## Test 2 — RSX/UI/PAD
Ventana: `FPS: 59.90 | Vulkan | 0.0.43-20331 | Monochrome Music [MCRM00001]`.

Captura real: `evidence/live_3.png` (1366x768, zona de juego con 3.862 colores,
mean RGB ≈ (12.9, 16.8, 20.0), stddev ≈ (19.7, 26.1, 31.5) → NO es una imagen negra).
Contenido verificado visualmente: cabecera `MONOCHROME v01.00`, icono/portada,
VU meter `VOL 100%`, `DMA 2/8`, barra `Filter: Daft Punk`, y 6 pistas reales
(`Trending — Dei V`, `Trending Remix (with Myke Towers)`, `Mrs. Trendsetter — Lil Baby`,
`Trending Topic — Rels B`, ...) con duración y badge FLAC.
Input: la pulsación de Cross (X) fue procesada por el juego (ver Test 3).

Nota de entorno: la ventana de RPCS3 se minimiza intermitentemente por causas ajenas al
juego (el título pasa a `FPS: 3.29` con la ventana icónica). En ese estado Vulkan deja de
presentar y la captura sale negra; NO es un fallo del homebrew. Con la ventana restaurada
el FPS vuelve a ~60 y la UI se muestra correctamente (`evidence/live_2.png`,
`evidence/live_3.png`, `evidence/live_4.png`).

## Test 3 — NET/STREAMING
Búsqueda real + reproducción (TTY.log, tras pulsar Cross):
```
[NOW PLAYING] ▶ Trending — Dei V [] | DAC: 44100 Hz 16-bit | XDR Buf: 0 KB
[HTTP] 200 len=142775 chunked=0 http://tracks.monochrome.st/proxy/mi/...jpg
[HTTP] 200 len=32127641 chunked=0 http://tracks.monochrome.st/track/192484768301232128
[STREAM] SUCCESS: Opened progressive FLAC stream (65 KB buffered)!
[STREAM] FLAC Format: 48000 Hz | 2 ch | 7600825 frames (158.4 sec)
[AUDIO] State → PLAYING
[AUDIO] LPCM DAC 48 kHz stereo active | Pumped: 4 blks/tick | Buffer: 1776 KB
[NOW PLAYING] ▶ Trending — Dei V [] | DAC: 44100 Hz 16-bit | XDR Buf: 4652 KB
[STREAM] Downloaded 8 MB
```
XDR: 64 MB buffer de stream + 8 MB ring + ~16 MB texturas + 16 MB heap ≈ **104 MB** < 256 MB.
Los framebuffers ya NO viven en XDR: están en memoria local RSX (`cellGcmGetConfiguration`).

## Diferencias respecto a RUNTIME_REPORT.md (24 h antes)
- Informe anterior daba PASS con framebuffers en main memory, pero la pantalla era NEGRA:
  el scanout PS3 lee de memoria local RSX. Este informe corrige aquel falso positivo.

## Artefactos
`evidence/live_2.png`, `evidence/live_3.png`, `evidence/live_4.png` (home UI, no negras),
`evidence/live_playing.png`, `evidence/desktop_live3.png`, `evidence/shot_*.png` (traza).
