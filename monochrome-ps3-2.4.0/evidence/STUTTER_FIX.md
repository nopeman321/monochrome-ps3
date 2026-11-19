# STUTTER_FIX — Causas del stuttering y correcciones aplicadas

Fecha: 2026-10-20. Build final: `EBOOT.ELF` 1.352.756 B @ 19:27:32.

## Diagnóstico (medido, no supuesto)

Se instrumentó el bucle principal con desglose por sección (`[PERF] ... | in= au= rd=`).
Con reproducción activa, ANTES de las correcciones:

```
[PERF] loops/s=46.8 avg_ms=21.31     <-- el frame duraba 21.3 ms (60 FPS = 16.6 ms)
[PERF] loops/s=45.4 avg_ms=21.31
[PERF] loops/s=43.6 avg_ms=22.73
```

`21.33 ms` = exactamente el tiempo de los 4 bloques de audio (4 × 256 / 48000 s) que
`audio_tick()` bombeaba a la fuerza en cada vuelta. El resto de hallazgos:

| # | Causa | Evidencia |
|---|---|---|
| 1 | `rsx_flip()` descartaba 1 de cada 2 frames (`s_flip_div`) | 25-30 FPS irregulares sobre VSync de 60 Hz → judder |
| 2 | `platform_audio_write()` bloqueaba hasta 10 ms esperando hueco en el anillo DMA de cellAudio | bucle clavado en 21.3 ms |
| 3 | Copia completa de 1.38 MB al framebuffer RSX en cada frame | **~7 ms** de los 19.4 ms del render |
| 4 | `fb_rect()` pintaba pixel a pixel (con comprobación de límites por pixel) | clear + fondos costosos |
| 5 | FSR activo en Intel UHD integrada + Write Color Buffers en un juego 2D por software | saturación de GPU/memoria compartida |

## Correcciones

### 1. Se presenta cada fotograma (`src/platform_ps3.c`)
Eliminado el divisor `s_flip_div`. Ahora cada frame llega al RSX.

### 2. Audio no bloqueante (`src/audio.c` + `src/platform_ps3.c`)
- Nueva `platform_audio_free_blocks()`: huecos libres reales en el anillo DMA
  (distancia entre el puntero de escritura y el índice que consume el hardware, con
  1 bloque de margen).
- `platform_audio_write()` ya **no espera**: si no hay hueco, retorna y el llamador
  decide. Eliminado el `ps3_sys_timer_usleep(200)` en bucle (hasta 10 ms/bloque).
- `audio_tick()` bombea exactamente lo que el DMA acepta, no 4 bloques fijos.
  Rellena con silencio los huecos si el ring no tiene datos (mantiene el DAC vivo).
- Coste medido del audio tras el cambio: **0.4-0.9 ms** (antes bloqueaba el frame).

### 3. Copia a RSX solo de filas sucias (`src/platform_ps3.c`)
- `s_dirty[2][480]` por buffer (doble búfer) + `s_drawn`/`s_prev_drawn`.
- Se marca la fila en `fb_px`/`fb_rect`/`fb_glyph`; en `rsx_flip()` solo se copian
  las filas marcadas del buffer que toca presentar.
- El "clear" ya no repinta los 480 rows: solo limpia las filas que se dibujaron en
  el frame anterior (y la pantalla completa solo la primera vez).

### 4. `fb_rect()` optimizado
Escritura de 2 píxeles a la vez (uint64), con recorte previo de coordenadas en vez de
comprobar límites por pixel.

### 5. Configuración de RPCS3 (`tools/configure_rpcs3.py`)
- `Output Scaling Mode: FidelityFX Super Resolution` → **`Bilinear`** (FSR son pasadas
  de compute shader por frame; en una Intel UHD de 15 W provoca picos).
- `Write Color Buffers: true` → **`false`** (el framebuffer se pinta por software; activarlo
  solo añade sincronización GPU→CPU en cada flip).

### 6. Extra
- Cabecera del ahora-en-reproducción: título truncado a 15 caracteres (antes 20) para que
  no se solape con el visualizador de espectro.
- `tools/verify_runtime.ps1`: reintenta localizar la ventana hasta 24 s (el primer arranque
  compila PPU/SPU y tardaba más de los 4 s que esperaba).

## Resultado medido (mismo hardware, misma pista en streaming)

```
ANTES  [PERF] loops/s=43.6-46.9  avg_ms=21.3-22.7   (render bloqueado por audio)
DESPUÉS[PERF] loops/s=52.7-59.9  avg_ms=15.0-19.3   | in=0.02 au=0.5 rd=15-19
```

- Desglose final típico: input **0.02-0.06 ms**, audio **0.4-0.9 ms**, render **15-19 ms**.
- `loops/s` sostenido ≈ **57-60** con FLAC 48 kHz sonando (antes 46).
- Ventana de RPCS3: `FPS: 60.06`, sin `SIG`/`Access Violation`/`Guru Meditation`.
- Confirmado en la captura: portada, visualizador, VU, DMA 5/8 y la fila seleccionada
  se actualizan sin ghosting (la UI por filas sucias funciona).

## Honestidad sobre los límites
- Siguen existiendo picos puntuales a ~40 loops/s (24 ms) durante **cambios de pista**:
  en ese momento coinciden descarga HTTP (hasta 48 MB), decodificación FLAC y decodificación
  de portada en hilos secundarios, y el host es un i5-1035G1 de 15 W con 4 núcleos.
  Es contención de CPU del host, no un bloqueo del bucle.
- Si se quiere margen extra, las palancas restantes serían: reducir la resolución interna
  del framebuffer software, o repartir el decode de portada antes de empezar la pista.
