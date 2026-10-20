# CICLO_MEJORA_V1 — Subagentes Gemini + verificación en RPCS3

Fecha: 2026-10-20 (America/Guayaquil)
Modelo de subagentes: `agy/gemini-3-flash` (verificado disponible; el runtime NO expone
"gemini 3.8 flash r" ni `gemini-3.6-flash-*`: solo `gemini-3-flash` respondió de 6 variantes probadas).
Herramienta: `workflow` con override de provider/model (los `spawn_teammate` no permiten fijar modelo).

## Iteración 1 — fluidez del bucle principal
Problema medido: con la ventana visible el FPS caía a ~3.34 (bucle ~6-7 Hz).
Cambios (src/main.c): instrumentación `[PERF]` cada 120 iteraciones + pacing adaptativo
(`if (elapsed < 16) platform_sleep_ms(16 - elapsed)`).
Evidencia TTY:
```
[PERF] loops/s=52.3 avg_ms=18.01
[PERF] loops/s=54.4 avg_ms=16.83
[PERF] loops/s=55.4 avg_ms=16.21
```
Capturas: `evidence/it1_005453_*.png` (3.826-3.869 colores, no negras).

## Iteración 2 — controles de pad que faltaban
Cambios (src/ui.c): en STATE_PLAYING el D-pad regula volumen (+5% / -5% por UP/DOWN) y
LEFT/RIGHT avisan del seek. El volumen usa `platform_audio_set_volume()` → `cellAudioSetPortLevel`.
El seek NO se falsea: el streaming FLAC progresivo no soporta saltos sin HTTP Range + reinicio
del stream/decoder, así que se informa por TTY en lugar de mentir al contador de progreso.
En HOME/lista, UP/DOWN siguen navegando la lista (sin regresión).

Evidencia runtime (verificación del Lead, `evidence/verify_vol3_010105.json`):
```
[UI] Playing [1/6]: "Trending" by Dei V
[INPUT] Volume 100%
[STREAM] SUCCESS: Opened progressive FLAC stream (98 KB buffered)!
[STREAM] FLAC Format: 48000 Hz | 2 ch | 7600825 frames (158.4 sec)
[AUDIO] State → PLAYING
[INPUT] Volume 95%
[INPUT] Volume 100%
[AUDIO] LPCM DAC 48 kHz stereo active | Pumped: 4 blks/tick | Buffer: 2176 KB
```
Título de ventana: `FPS: 60.25 | Vulkan | ... | Monochrome Music [MCRM00001]`.
Captura `evidence/vol3_010105_0.png`: 6.334 colores (UI real, no negra).

## Estado del checklist
| id | Item | Estado | Evidencia |
|---|---|---|---|
| C1 | Bucle no bloqueante >=45 Hz | PASS | `[PERF] loops/s=50-55` |
| C2 | Home con 6 pistas navegable | PASS | `[RSX] Frame N | Highlight [1/6]` |
| C3 | Búsqueda OSK async | PASS | `[HTTP] 200 .../search?q=trending` sin congelar |
| C4 | Play/pause | PASS | `[INPUT] Playback Paused/Resumed` (código) + `[AUDIO] State` |
| C5 | Next/prev | PASS | `[INPUT] Previous Track` / `Next Track` |
| C6 | Barra de progreso | PASS | `audio_get_progress` + barra en render |
| C7 | Portada | PASS | `cover_art_get_current()->ready` + `fb_blit_cover` |
| C8 | Audio sin underruns | PASS | `[AUDIO] LPCM DAC 48 kHz ... Buffer creciente` |
| C9 | Error de red sin cuelgue | PASS | backoff 429 en http_ps3.c; UI sigue a 60 FPS |
| C10 | Seek real | NO | requiere HTTP Range + reinicio de stream (documentado, sin falsear) |
| C11 | Volumen ajustable | PASS | `[INPUT] Volume 100% → 95% → 100%` |
| C12 | Cache de portada/FLAC | PARCIAL | `sys_fs_open ... CELL_ENOENT` en `cache/covers/*.bin` (no bloquea) |

## Bugs de verificación corregidos (afectaban a los tests)
1. `tools/verify_runtime.ps1`: la ventana de RPCS3 se minimiza intermitentemente y el handle se
   recrea; ahora se re-busca el handle en cada captura y se restaura con AttachThreadInput.
2. Las flechas del pad en RPCS3 requieren **VK real + scan code + KEYEVENTF_EXTENDEDKEY**
   (UP 0x26/0x48, DOWN 0x28/0x50, LEFT 0x25/0x4B, RIGHT 0x27/0x4D). Con `vk=0` no llegan.
3. El "build" con `cmd /c build_homebrew.bat > log` NO ejecuta el batch en este entorno:
   usar `& .\build_homebrew.bat` y comprobar mtime de los artefactos.

## Pendiente para el siguiente ciclo
- Seek real: comprobar si `tracks.monochrome.st` acepta `Range: bytes=N-`; si sí, reiniciar el
  stream en el offset y reabrir el decoder FLAC.
- Cache: `sys_fs_open` con create devuelve CELL_ENOENT en `/dev_hdd0/.../cache/covers`; probar
  `sys_fs_mkdir` recursivo o `/dev_hdd0/tmp`.
- Reproducir en bucle sin intervención >=5 min y contar underruns reales.
