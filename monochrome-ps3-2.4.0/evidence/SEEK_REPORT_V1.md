# SEEK_REPORT — Seek real + soak de escucha 5+ minutos

Fecha: 2026-10-20. Build: `build/EBOOT.ELF` mtime 07:52:44, `MCRM00001.pkg` 317.632 B.

## Qué se implementó (seek real, sin falsear)

1. `include/monochrome.h`
   - `void net_seek_to_sec(int seconds); int net_seek_is_pending(void);`
   - `void audio_seek_to_frames(uint64_t frames); uint64_t audio_get_elapsed_frames(void);`
   - `uint32_t audio_get_elapsed_sec(void);` (faltaba la declaración)
2. `src/audio.c`: `audio_seek_to_frames()` sincroniza el contador real de reproducción y
   reinicia las bandas del visualizador.
3. `src/network.c`
   - Petición de seek asíncrona (`s_seek_req_sec` / `s_seek_pending`).
   - En el bucle de decodificación: al haber buffer completo (`ctx.eof`), reabre el decoder con
     `drflac_open_memory(ctx.data, ctx.received)` y se posiciona con
     `drflac_seek_to_pcm_frame(frame)`; después descarta el audio pendiente del ring de forma
     atómica (`write_pos = read_pos`), actualiza el contador y continúa decodificando.
   - Si el buffer aún no está completo, el seek queda pendiente y se aplica al terminar la
     descarga (se informa por TTY, no se miente al usuario).
   - Limpieza de un bug latente en la ruta de caché: `free(ctx.data)` liberaba el buffer XDR que
     después se volvía a asignar (use-after-free en PC; no-op en PS3). Eliminado.
4. `src/ui.c` (STATE_PLAYING): D-pad IZQUIERDA = −10 s, DERECHA = +10 s, con clamp a
   `[0, duración−1]`.
5. `src/platform_ps3.c`: la cabecera muestra `[<][>]Seek`.

## Verificación funcional (RPCS3, build 07:52:44)
```
[INPUT] Seek +10s -> 0:11
[SEEK] Solicitado -> 0:11
[SEEK] -> 0:11 (frame 528000/7600825)
[INPUT] Seek +10s -> 0:22
[SEEK] -> 0:22 (frame 1056000/7600825)
[INPUT] Seek -10s -> 0:13
[SEEK] -> 0:13 (frame 624000/7600825)
[INPUT] Seek -10s -> 0:04
[SEEK] -> 0:04 (frame 192000/7600825)
fallos: 0
```
Los frames son exactos: 11 s × 48000 = 528.000; 22 s = 1.056.000; 13 s = 624.000.

## Soak de escucha (5 min 40 s, `evidence/tty_soak_seek.log`)
- **5 seeks** solicitados y **5 aplicados** a frames correctos.
- 248 muestras `[AUDIO] LPCM DAC 48 kHz` → audio ininterrumpido.
- **0 underruns / 0 stalls / 0 fallos** en TTY.
- FPS de ventana: 59.80-60.12 en 21 de 22 muestras (la única anomalía, 0.59, coincide con la
  ventana minimizada por el entorno, no con el homebrew).
- `[PERF]` estable: 46.5-56.7 loops/s (sin degradación con el tiempo).
- Auto-avance de cola observado: tras un seek cerca del final, la pista terminó y pasó sola a la
  siguiente (totales de frames distintos: 10807423 y 9130166).

## Notas
- El seek depende de tener el buffer completo. Con caché persistente (caso normal tras la primera
  escucha) es inmediato. En primera escucha se aplica al completarse la descarga (~10-20 s).
- Se comprobó que el endpoint `/track/:id` soporta `Range: bytes=N-` (HTTP 206 + `content-range`),
  pero la vía implementada evita descargar de nuevo: reutiliza el buffer ya cargado.
