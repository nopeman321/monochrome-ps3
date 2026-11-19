# PS3_STATE.md — Monochrome PS3 (PS3-only, RPCS3) — FINAL

## Ruta
`C:\Users\Juanjo\.gemini\antigravity\scratch\ps3-monochrome` (remote `nopeman321/monochrome-ps3`, rama main).

## Build
- `build_homebrew.bat` (zig cc powerpc64-linux-musl + link.ld + tools/sce_imports.py + scetool + pkg_builder).
- Artefactos: `build/EBOOT.ELF` (1.29 MB), `pkg_build/USRDIR/EBOOT.BIN` (110 KB), `MCRM00001.pkg` (310 KB).
- RPCS3: `emulator/rpcs3/rpcs3.exe` (Vulkan, 720x480). Ventana: `Monochrome Music [MCRM00001]`.
- Input RPCS3: flechas=DPad, X=Cross, Z=Square, V=Triangle, C=Circle, Enter=Start. Las flechas requieren `KEYEVENTF_EXTENDEDKEY` (scan 0x50/0x4D).

## Estado de los 3 Tests (verificado en RPCS3)
| Test | Estado | Evidencia |
|---|---|---|
| 1 Boot/Sys | **PASS** | Arranque estable, VFS `/dev_hdd0`+`/app_home`, cellAudio 48 kHz, cellPadInit, cero Traps/ENOENT/AV (`evidence/RUNTIME_REPORT_V2.md`) |
| 2 UI/Pad/Search | **PASS** | UI 720x480 visible y fluida a 59.9-60 FPS (`evidence/live_3.png`), búsqueda real "Daft Punk" → 6 pistas seleccionables, input Cross procesado |
| 3 Stream/RAM | **PASS** | HTTP 200 real, FLAC 48 kHz progresivo (32 MB, 158.4 s), `[AUDIO] State → PLAYING`, LPCM DAC 48 kHz, XDR ≈104 MB < 256 MB |

## Correcciones PS3 aplicadas (código PS3; PC intacto salvo revert final)
1. `sys_ppu_thread_start` (syscall 53) faltante: los hilos quedaban suspendidos. Ahora arrancan (ret=0).
2. TLS de hilos: musl ppc64 `TLS_ABOVE_TP` (TP_OFFSET 0x7000); se clona el TCB del hilo principal y se pasa `r13 = base + 0x7060`.
3. `sys_memory_allocate` (348): out-pointer debe ser **u32** (antes u64 → dirección corrupta `0x2009000000000000`).
4. Buffer de stream 64 MB en XDR real (`platform_alloc_stream_buffer`), reutilizado; ruta de caché sin malloc.
5. Red real: syscalls `sys_net` (700–726) en `lv2.c` + cliente HTTP/DNS en `src/http_ps3.c`; URLs API en `http://` (HTTP plano funciona). Stubs de `platform_http_get(_streaming)` eliminados.
6. RSX: `cellGcmSetFlip` HLE requiere `(ctxt, id)`; se añadió `cellGcmSetWaitFlip` (21 imports con `cellGcmGetConfiguration`).
6-bis. **Causa raíz de la pantalla negra (fix definitivo):** los framebuffers estaban en
   main memory (`cellGcmMapMainMemory`, offsets 0x100000/0x300000), pero el scanout PS3/RPCS3
   resuelve los offsets de `cellGcmSetDisplayBuffer` contra la **memoria local RSX**. Al ser
   offsets < localSize, RPCS3 los interpretaba como locales → negro a 60 FPS.
   Fix: `platform_ps3.c` llama `cellGcmGetConfiguration` y coloca los 2 buffers en
   `localAddress (0xC0000000)` con offsets `0x0` y `0x200000`. Verificado en runtime:
   `[RSX] local_base=0xc0000000 size=0x01b00000` y UI visible a 59.9-60 FPS.
7. Flip handler registrado debe ser **vacío**: HLE `_gcm_intr_thread` lo invoca sin `r2`/TOC; cualquier global crasheaba (`AV 0x921f8c`). Handler sin globals → estable a 60 FPS.
8. Fuente 8x8: bit order LSB (`1u << col`) → texto legible.
9. Instrumentación de evidencia: volcado `fb_home.raw`/`fb_play.raw` (720x480 ARGB) para verificar UI sin depender de hotkeys.

## Desviaciones conocidas (no bloquean los tests)
- El goal menciona `cellOskDialog`; se usa el OSK propio de la app (funcional, navegable con D-pad).
- La escritura de caché FLAC en `/dev_hdd0/.../cache` devuelve ENOENT en RPCS3 (create no soportado en ese VFS); el streaming funciona sin caché.
- Etiqueta DAC muestra 44100 Hz aunque cellAudio está a 48000 Hz (cosmético; resample 44→48 activo).
- `free()` sigue siendo no-op y el heap bump es de 16 MB (sin impacto en los tests).
- Uso de CPU de render software: PRESENT ~20-30 fps internos; RPCS3 reporta 60 FPS (vsync).

## Evidencia
- Runtime V2 (verificado 2026-10-20): `evidence/RUNTIME_REPORT_V2.md`, `evidence/live_3.png`
  (UI home con 6 pistas reales, 3.862 colores), `evidence/live_playing.png` (reproducción),
  `evidence/AUDIT_REPORT_V2.md`.
- Histórica: `evidence/ui_home2.png`, `evidence/ui_playing2.png`, `evidence/fb_fixed.png`,
  `evidence/fb_home.raw`, `evidence/fb_play.raw`, `evidence/RUNTIME_REPORT.md`.

## Notas de entorno (para futuras sesiones)
- El build debe lanzarse como `& .\build_homebrew.bat` desde pwsh. `cmd /c "build_homebrew.bat > log"`
  NO ejecuta el .bat en este entorno: deja un log obsoleto y da falsos "build OK".
  Comprobar siempre mtime/tamaño de `build/EBOOT.ELF`, `pkg_build/USRDIR/EBOOT.BIN` y `MCRM00001.pkg`.
- RPCS3 puede arrancar/minimizarse solo en esta sesión (la ventana pasa a `FPS: 3.29` e icónica).
  Con la ventana minimizada Vulkan deja de presentar y la captura sale negra: NO es un fallo del
  homebrew. Restaurar la ventana (`ShowWindow SW_RESTORE` + TOPMOST) y capturar a los ~2 s.
- No editar `src/platform_ps3.c` con `Get-Content`/`WriteAllLines` de PowerShell 5.1 (destroza la
  codificación). Usar la herramienta de edición o Python con UTF-8 explícito.

## Ciclo de mejora v1 (subagentes Gemini, 2026-10-20)
- Modelo real disponible: `agy/gemini-3-flash` (el runtime no expone "gemini 3.8 flash r" ni 3.6).
- Iteración 1: el bucle caía a ~3.3 FPS. Añadida instrumentación `[PERF] loops/s` y pacing
  adaptativo en `src/main.c` → 50-55 loops/s medidos en TTY.
- Iteración 2: en `STATE_PLAYING`, D-pad UP/DOWN = volumen ±5% (imprime `[INPUT] Volume N%`),
  LEFT/RIGHT avisan de seek no soportado (sin falsear el progreso). En HOME/lista el D-pad
  sigue navegando. Verificado: `[INPUT] Volume 100% → 95% → 100%` con FLAC 48 kHz a 60 FPS.
- Informe con evidencia: `evidence/CICLO_MEJORA_V1.md`.
- Herramienta reutilizable: `tools/verify_runtime.ps1` (lanza RPCS3, estabiliza la ventana,
  envía teclas de pad con VK+scan+extended, captura PNG, resume TTY/errores en JSON).
- Pendiente: seek real (requiere HTTP Range + reinicio de stream) y cache de portada
  (`sys_fs_open` create → CELL_ENOENT).

## Errores restantes corregidos (2026-10-20, ver evidence/FIXES_V2.md)
1. **VSync inválido**: RPCS3 solo acepta `Disabled/Adaptive/Full`; se escribía `Enabled` →
   error `cfg::try_to_enum_value`. Corregido en `tools/configure_rpcs3.py` y en los configs.
2. **CELL_ENOENT al crear caché**: los flags de `cellFs` son OCTALES y faltaba `O_CREAT`
   (`000100` = 0x40). `platform_file_open_write` ahora usa `{0x241, 0x201, 0x001}`.
   Verificado: se guardan portada (25 KB) y FLAC (32 MB) y luego hay `Persistent Cache HIT`.
3. **Formato de audio mal reportado**: ahora se publica el formato real del stream y el de
   salida LPCM → `DAC: 48000 Hz 16-bit` (antes 44100/24-bit).
4. **Verificador**: flechas requieren VK+scan+EXTENDED; `AttachThreadInput` rompía las teclas.
5. Descartado tras comprobarlo: `[HTTP] ... len=-1 chunked=0` es correcto con HTTP/1.0
   (respuesta con `Connection: close`, sin Content-Length, cuerpo JSON limpio de 8420 B).

Soak test 100 s: `[PERF] loops/s=44.7..47.2` estable, 69 muestras LPCM, **0 underruns**,
0 fallos, ventana a 60.02-60.11 FPS. Evidencia: `evidence/tty_soak_100s.log`,
`evidence/tty_controls_proof.log`.

## Seek real + soak (2026-10-20, ver evidence/SEEK_REPORT_V1.md)
- En `STATE_PLAYING`, D-pad IZQUIERDA/DERECHA = seek −10 s / +10 s (clamp a la duración).
- Implementación: el worker de stream reabre el decoder con `drflac_open_memory()` sobre el
  buffer completo y usa `drflac_seek_to_pcm_frame()`; descarta el audio pendiente del ring de
  forma atómica (`write_pos = read_pos`) y sincroniza el contador (`audio_seek_to_frames`).
- Si el buffer aún está descargando, el seek queda pendiente y se aplica al completarse
  (se informa por TTY; nunca se falsea el progreso).
- Verificado: frames exactos (11 s → 528.000; 22 s → 1.056.000; 13 s → 624.000; 4 s → 192.000).
- Soak 5 min 40 s con 5 seeks: 0 underruns, 0 fallos, FPS 59.8-60.1, `[PERF]` 46.5-56.7 estable,
  y auto-avance de cola funcionando.
- Cabecera de la UI actualizada: `[<][>]Seek`.

## Subagentes
- Intento 2026-10-20: `auditor-ps3` (task-1) y `verificador-rpcs3` (task-2) fueron creados pero
  el proveedor de modelo falló (subagentes en background abortados sin mensaje final). El Lead
  asumió ambos trabajos: auditoría estática y verificación runtime completadas a mano.
- Histórico: auditor `nemotron-3.5-super-vl-free` → `evidence/AUDIT_REPORT.md`;
  gatekeeper `big-pickle` (el harness no permite fijar modelo en hijos).
