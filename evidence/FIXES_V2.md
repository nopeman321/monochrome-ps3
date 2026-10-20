# FIXES_V2 — Errores restantes encontrados y corregidos

Fecha: 2026-10-20. Build final: `build/EBOOT.ELF` 1.295.412 B, mtime 01:17:33.

## Resumen: 3 errores reales corregidos + 1 error de herramienta de test

| # | Error | Tipo | Estado | Verificación |
|---|---|---|---|---|
| E1 | `VSync Mode: Enabled` rechazado por RPCS3 (`invalid enum or integer`) | Config | FIXED | log ya no emite el error; config escribe `Full` |
| E2 | `sys_fs_open` con create devolvía `CELL_ENOENT` → caché de portadas/FLAC nunca se guardaba | Código PS3 | FIXED | ficheros creados (25 KB cover, 32 MB FLAC) |
| E3 | TTY/UI mostraban `DAC: 44100 Hz` y luego `24-bit` cuando el pipeline entrega 48 kHz/16-bit | Código PS3 | FIXED | `DAC: 48000 Hz 16-bit` |
| E4 | `tools/verify_runtime.ps1` perdía teclas (foco/ventana minimizada) | Herramienta | FIXED | controles verificados en prueba aislada |

## E1 — Enum de VSync inválido
RPCS3 solo acepta `vsync_mode` ∈ {`Disabled`, `Adaptive`, `Full`}; nuestro `configure_rpcs3.py`
escribía `Enabled`.
- `tools/configure_rpcs3.py:66` → `VSync Mode: Full`
- `tools/configure_rpcs3.py:106` → reemplazo `("VSync Mode: Disabled", "VSync Mode: Full")`
- Corregidos también `config.yml` y `custom_configs/config_MCRM00001.yml` ya escritos.
Verificación: `RPCS3.log` ya no contiene `cfg::try_to_enum_value('Enabled', 'VSync Mode')`.

## E2 — Flags de cellFs en octal (raíz del CELL_ENOENT)
`sys_fs.h` de PS3 define los flags en **octal**:
```
O_RDONLY=000000  O_WRONLY=000001  O_CREAT=000100  O_TRUNC=001000  O_APPEND=002000
```
`platform_file_open_write()` probaba `0x601/0x401/0x201`: ninguno incluía `O_CREAT (0x40)`,
así que RPCS3 devolvía `CELL_ENOENT` al crear y la caché nunca se escribía.
- `src/platform_ps3.c:130` → `int flags_seq[3] = { 0x241, 0x201, 0x001 };` (CREAT|TRUNC|WRONLY).
Verificación (RPCS3.log):
```
sys_fs_open(path=".../cache/covers/192484768301232128.bin", flags=01101 ...)
sys_fs_open(): fd=3, Regular file, ..., Flags: 0x241, Pos/Size: 25KB/25KB
sys_tty: "[COVER] Saved 80x80 decoded texture to cache: .../covers/192484768301232128.bin"
```
Y en TTY tras descarga larga:
```
[STREAM] Download complete: Saved 31374 KB to persistent cache: .../cache/tracks/192484768301232128.flac
```
Segunda ejecución (caché caliente):
```
[STREAM] Persistent Cache HIT: .../cache/tracks/192484768301232128.flac (31374 KB, instant zero-network playback)
[COVER]  Persistent Cache HIT: .../cache/covers/192484768301232128.bin (instant 0 ms load)
```
Ficheros en host: `192484768301232128.flac` = 32.127.641 B, `192484768301232128.bin` = 25.600 B.

## E3 — Formato de audio mal reportado
El catálogo solo estima 44.1/96 kHz y el render imprimía eso; después el `bitsPerSample` del FLAC
(24) se propagaba aunque el pipeline decodifica a LPCM s16.
- `src/network.c` (tras abrir el FLAC): fija `now->sample_rate` real, `now->bit_depth = 16`
  (formato de salida real) e `is_hi_res` según la fuente (24-bit/96 kHz).
Verificación:
```
[STREAM] FLAC Format: 48000 Hz | 2 ch | 7600825 frames (158.4 sec)
[STREAM] Metadata updated: 48000 Hz source/24-bit -> LPCM 48000 Hz/16-bit
[NOW PLAYING] ▶ Trending — Dei V [] | DAC: 48000 Hz 16-bit | XDR Buf: 456 KB
```

## E4 — Pérdida de teclas en el verificador
Dos causas acumuladas, ambas corregidas en `tools/verify_runtime.ps1`:
1. Las flechas del D-pad exigen **VK real + scan code + KEYEVENTF_EXTENDEDKEY**
   (UP 0x26/0x48, DOWN 0x28/0x50, LEFT 0x25/0x4B, RIGHT 0x27/0x4D). Con `vk=0` no llegan.
2. `AttachThreadInput` en `ForceForeground` rompía la entrega de teclas; se volvió al método
   simple (`ShowWindow` + `SetWindowPos TOPMOST` + `SetForegroundWindow`) y se re-estabiliza
   antes de **cada** tecla.

## Verificación final (independiente, hecha por el Lead)

### Estabilidad (100 s de escucha, `evidence/tty_soak_100s.log`)
```
[PERF] loops/s=44.7..47.2 avg_ms≈21   (tras el arranque; sin degradación)
[PERF] loops/s=55.1 avg_ms=16.40      (arranque)
LPCM DAC activo: 69 muestras
underruns/stalls: 0
fallos (fail/ERROR/SIG): 0
```
Ventana de RPCS3 muestreada 6 veces: `FPS: 60.11 / 60.07 / 60.11 / 60.09 / 60.02 / 60.02`.

### Controles (`evidence/tty_controls_proof.log`)
```
[INPUT] Previous Track (⏮)  → [UI] Playing [6/6]: "Trending Topic" by Rell Vert
[INPUT] Next Track (⏭)      → [UI] Playing [1/6]: "Trending" by Dei V
[INPUT] Playback Paused (⏸)  → [AUDIO] State → PAUSED
[INPUT] Playback Resumed (▶) → [AUDIO] State → PLAYING
[INPUT] Volume 100% / 95%   (D-pad arriba/abajo en reproducción)
```

### Logs limpios
`RPCS3.log` ya NO contiene: `CELL_ENOENT`, `cfg::try_to_enum_value`, `[FS] open_write fail`,
`SIG:`, `Access violation`, `Guru Meditation`, `PPU Trap`.
Solo quedan mensajes internos de RPCS3 con nivel `·E` (`ppu_loader: Skipping PRX name
registeration`, `PPU: Function analysis: 0 functions`), ajenos al homebrew.

## Descartado como error (comprobado)
- `[HTTP] 200 len=-1 chunked=0` en la búsqueda: la petición es HTTP/1.0, por lo que el servidor
  responde con `Connection: close` y sin `Content-Length` ni `Transfer-Encoding`. Replicado a
  mano: cuerpo JSON limpio de 8.420 B con framing por cierre. No es un fallo del cliente.
- `sys_fs_stat ... CELL_ENOENT` en la primera reproducción era la comprobación de caché fría;
  desaparece cuando la caché existe (verificado).

## Pendiente (no es un error, es una limitación conocida)
- Seek real: requiere `Range` + reinicio de stream/decoder. Hoy se informa por TTY sin falsear.
- El entorno minimiza la ventana de RPCS3 de forma intermitente; no es un fallo del homebrew.
