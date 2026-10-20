# AUDIT_REPORT.md — Monochrome PS3 (nemotron-3.5 N2/N3 Auditor)

**Repo:** `C:\Users\Juanjo\.gemini\antigravity\scratch\ps3-monochrome` (rama main, remote nopeman321/monochrome-ps3)  
**Ámbito:** read-only sobre `src/` y `include/` (NO se editó nada).  
**Build:** `build_homebrew.bat` → `build/EBOOT.ELF` + `pkg_build/USRDIR/EBOOT.BIN` + `MCRM00001.pkg` (Zig cc powerpc64, scetool, pkg_builder).  
**Estado conocido:** arranca estable a 60 FPS (State:1 HOME), búsqueda "trending" devuelve 4 tracks reales, sin traps ni ENOENT. NO hay evidencia de input de pad ni de reproducción/streaming aún.

---

## A) INPUT / PAD — `src/platform_ps3.c`

### Qué hay implementado
- `platform_poll_input()` (líneas 1336–1403): inicializa perezosamente `cellPadInit` (versión 7) si el slot de importación está bound (`SCE_BOUND(cellPadInit)`, línea 1338), luego llama `cellPadGetData(0, &pd)` (línea 1355).
- Estructura `CellPadData` declarada localmente (líneas 1317–1320): `int32_t len; uint16_t button[64];`.
- Slots de importación declarados (líneas 1322–1323):
  - `SCE_SLOT(cellPadInit);`
  - `SCE_SLOT(cellPadGetData);`
- Mapeo de botones (líneas 1361–1400):
  - D-Pad: `b2` 0x10=Up, 0x40=Down, 0x80=Left, 0x20=Right (líneas 1362–1365).
  - Cross (✕) = `b3` 0x40 → `INPUT_SELECT` (línea 1368).
  - Circle (○) = `b3` 0x20 → `INPUT_CANCEL` (línea 1371).
  - Square (□) = `b3` 0x80 → `INPUT_ACTION_SEARCH` (línea 1374).
  - Triangle (△) = `b3` 0x10 → `INPUT_PLAY_PAUSE` (línea 1377).
  - R1 = `b3` 0x08 → `INPUT_NEXT_TRACK` (línea 1380).
  - L1 = `b3` 0x04 → `INPUT_PREV_TRACK` (línea 1381).
  - Select = `b2` 0x01 → `INPUT_SHUFFLE` (línea 1384).
  - L2 = `b3` 0x01 → `INPUT_REPEAT` (línea 1387).
  - R2 = `b3` 0x02 → volumen +5% (líneas 1390–1393).
  - L3 = `b2` 0x02 → volumen -5% (líneas 1394–1397).
  - Start = `b2` 0x08 → `INPUT_QUIT` (línea 1400).
- Detección de flanco: `n2 = b2 & ~s_prev2`, `n3 = b3 & ~s_prev3` (líneas 1357–1359). Solo eventos de presión (edge), sin repetición por retención.
- **No hay deadzone analógico** (los sticks no se leen; solo botones digitales).
- Fallback auto-demo (líneas 1343–1351): si `cellPadInit`/`cellPadGetData` no están bound, se genera una secuencia sintética de inputs cada ~90 frames (tk). Esto enmascara la ausencia de pad real.

### Integración en `ui.c`
- `pad_update()` en `src/ui.c` líneas 221–398 consume `platform_poll_input()` y traduce `InputAction` a transiciones de estado (STATE_SEARCH, navegación, play/pause, next/prev, shuffle, repeat, quit).
- En `STATE_SEARCH` (líneas 225–298) el OSK propio usa D-Pad + Cross/Circle/Triangle/Square para navegar la grid `s_osk_grid[4][10]` y escribir `g_search_query`.

### Por qué podría no llegar input en RPCS3
1. **Vínculo de importación no verificado.** `platform_poll_input()` comprueba `SCE_BOUND(cellPadInit)` y `SCE_BOUND(cellPadGetData)` (líneas 1338, 1342), pero **nunca imprime un mensaje de fallo si los slots no están bound**. Simplemente activa el fallback auto-demo (línea 1343). Si RPCS3 no expone `cellPadInit`/`cellPadGetData` o el ELF no tiene las entradas de importación correctamente resueltas por `tools/sce_imports.py`, el pad queda silencioso y el auto-demo lo oculta.
2. **No se llama `cellPadInit` en ningún lado fuera de `platform_poll_input`.** No hay inicialización explícita de `sys_io`/`ioPad` en el arranque (ni `cellSysmoduleLoadModule` de `sys_io` ni `ioPadInit` por nombre). El lazy init depende de que el usuario presione algo antes de que `s_pad_ready` se ponga a 1.
3. **RPCS3 HLE de cellPad puede requerir un pad virtual asignado.** Si no hay un controlador mapeado en la configuración de RPCS3 (Settings → Pads → Pad 1), `cellPadGetData` devuelve error o ceros, y `platform_poll_input()` retorna `INPUT_NONE` (línea 1355).
4. **Solo detección de flanco.** Si RPCS3 entrega el estado del pad de forma sostenida (level) y no se detecta el flanco correctamente por timing de emulación, algunos eventos pueden perderse.
5. **No hay L3/R3 como botones de acción independientes** (solo volumen), ni flechas analógicas. Está acorde al `InputAction` de `platform.h` (líneas 27–42), pero R3/L3 no aparecen como navegación.

---

## B) OSK — `cellOskDialog` vs OSK propio

### Búsqueda de `cellOskDialog`
- **No existe ninguna llamada a `cellOskDialog`** en `src/`, `include/`, `tools/` ni en ningún archivo del repo (grep devolvió 0 matches para `cellOskDialog|cellOsk` fuera de `platform_pc.c`, que es PC-only y está prohibido editar).
- `grep` confirmó 0 coincidencias de `cellOskDialog` en todo el árbol.

### OSK propio (`src/ui.c` y `src/platform_ps3.c`)
- `src/ui.c` líneas 118–172: estado del OSK (`g_search_query[64]`, `g_osk_row`, `g_osk_col`, grid `s_osk_grid[4][10]`).
- `ui_get_osk_state()` (líneas 141–145), `ui_osk_input_char()` (líneas 147–167), `ui_osk_set_cursor()` (líneas 169–172).
- `src/platform_ps3.c` líneas 1044–1090: render del modal OSK dentro de `platform_gfx_render()` cuando `state == STATE_SEARCH`.
- El OSK propio es **funcional visualmente**: navegación con D-Pad, selección con Cross, borrado con Circle/Triangle, GO con la tecla GO, presets con Square.

### Gap y plan mínimo (sin aplicar)
**Gap:** El goal exige `cellOskDialog` (diálogo de teclado nativo de Sony/PS3). La app usa un OSK de software dibujado en el framebuffer RSX. No hay integración con el sistema de OSK del firmware/GameOS.

**Plan mínimo propuesto (parche, no aplicado):**
1. Añadir slot de importación `SCE_SLOT(cellOskDialog)` en `src/platform_ps3.c` (junto a los existentes `cellPad*`).
2. Declarar wrapper `ps3_osk_dialog_open(const char *title, const char *initial_text, char *out_buf, size_t out_len)` que llame a `sce_opd_call6(sce_slot_cellOskDialog, ...)` con los parámetros de la API de Sony (tipo de diálogo, idioma, longitud máxima, título, buffer de salida).
3. Añadir función `platform_osk_open(...)` en el HAL (`include/platform.h` + implementación en `src/platform_ps3.c`) y un fallback en `src/platform_pc.c` (solo referencia, no editar).
4. En `src/ui.c`, cuando se entra a `STATE_SEARCH` (línea 385: `current = STATE_SEARCH;`), llamar a `platform_osk_open()` en lugar de (o además de) render el grid propio. Si `cellOskDialog` está bound, usar el diálogo nativo y poblar `g_search_query` con el resultado; si no, degradar al OSK propio (ya implementado).
5. Manejar el botón "GO" / cancel del diálogo nativo para disparar `net_search_async_query()` y volver a `STATE_HOME`.

---

## C) STREAMING / CONCURRENCIA — `src/network.c`, `src/main.c`

### Ring buffer (`src/main.c` y `include/monochrome.h`)
- `STREAM_RING_BUFFER_SIZE` = 8 MB (línea 32 de `include/monochrome.h`).
- `s_ring_storage[STREAM_RING_BUFFER_SIZE]` alineado a 128 bytes, estático en BSS (línea 21 de `src/main.c`).
- `AudioRingBuffer` (líneas 95–101 de `include/monochrome.h`): `buffer`, `capacity`, `volatile size_t write_pos` (productor = red), `volatile size_t read_pos` (consumidor = audio), `volatile bool stream_eof`.
- Helpers `ring_available()` (líneas 116–120) y `ring_free()` (líneas 121–123) con `CELL_LWSYNC()` (lwsync en PowerPC, MemoryBarrier en MSVC, `__atomic_thread_fence` en otro).
- Diseño SPS (single-producer single-consumer), lock-free.

### `net_start_stream` / `net_stop_stream` (`src/network.c` líneas 550–589)
- `net_start_stream(url, rb)`: reseta ring (líneas 555–557), establece `s_worker`, llama `platform_thread_create(stream_worker_func, &s_worker)` (línea 566).
- `net_stop_stream()`: pone `abort_requested = true` (línea 581) y hace `platform_thread_join(s_worker.thread)` (línea 583). **Bloqueante**: si el worker está dentro de `drflac_read_pcm_frames_s16` esperando datos o en `platform_thread_join` del hilo de descarga, el hilo principal se queda esperando hasta que el worker termine.
- No hay mutexes `sys_lwmutex`/`sys_mutex` en el código. La sincronización es puramente `volatile` + `CELL_LWSYNC()` (barreras de memoria PowerPC).

### `stream_worker_func` (`src/network.c` líneas 371–548)
- Descarga FLAC en `ctx.data` (malloc de `STREAM_BUFFER_CAPACITY` = 16 MB en PS3, 48 MB en PC, líneas 312–316).
- Pre-buffer: espera hasta ≥64 KB o EOF (líneas 447–449).
- Abre FLAC con `drflac_open_relaxed` usando callbacks `flac_stream_read`/`flac_stream_seek`/`flac_stream_tell` (líneas 463–464).
- Bucle de decode progresivo (líneas 493–526): verifica `ring_free(worker->rb) < max_bytes` (línea 496) → backpressure: `platform_sleep_ms(10)` y `continue`. Decodifica chunks de 2048 frames, resamplea si es 44.1 kHz o 96 kHz, escribe al ring con `ring_write_pcm()`.
- `audio_set_state(PLAYBACK_PLAYING)` inmediato (línea 488) antes de que haya datos suficientes en el ring.

### Riesgos de bloqueo del hilo RSX / audio
1. **RSX flip puede bloquear.** `rsx_wait_flip()` en `src/platform_ps3.c` líneas 749–758: espera `gcmGetFlipStatus() != 0` con timeout de 250 × 200 µs = 50 ms. Si el flip handler no está registrado o `cellGcmGetFlipStatus` no está bound, el timeout se agota pero no bloquea indefinidamente. Sin embargo, si `gcmSetFlip` espera vsync y el display no avanza, el frame se retrasa.
2. **`net_stop_stream()` bloqueante.** `platform_thread_join` espera la salida del worker. Si el worker está bloqueado en `drflac_read_pcm_frames_s16` esperando más datos de red que nunca llegan (sin EOF ni abort detectado a tiempo), el hilo principal se cuelga al detener el stream.
3. **No hay `sys_lwmutex`/`sys_mutex`.** El código usa `volatile` + barreras `lwsync`, que es suficiente para SPS en PowerPC pero no protege contra races múltiples-productor o consumidores múltiples. El ring buffer asume un solo productor (net thread) y un solo consumidor (audio tick). Si algún otro camino escribe al ring, hay race.
4. **`stream_worker_func` no se une al hilo de descarga en todos los caminos de error.** Por ejemplo, si `drflac_open_relaxed` falla (línea 465), sí hace `platform_thread_join(ctx.dl_thread)` (línea 468), pero en otros caminos tempranos podría faltar.
5. **Audio write polling.** `platform_audio_write()` en `src/platform_ps3.c` líneas 1250–1297: busy-wait con `ps3_sys_timer_usleep(200)` hasta 10000 µs (líneas 1260–1268) esperando que el bloque DMA no esté siendo leído por hardware. Si el hardware DMA no avanza (RPCS3 HLE de cellAudio defectuoso), este bucle consume CPU del hilo principal (que es también el hilo RSX).

### Backpressure y EOF
- Backpressure: `ring_free() < max_bytes` → sleep 10 ms (línea 496–499). `max_bytes` = 2500 frames (línea 495).
- EOF: `ctx.eof` se pone en `true` cuando `download_worker_func` termina (línea 337). El loop de decode termina cuando `drflac_read_pcm_frames_s16` devuelve 0 y `ctx.eof` es true (líneas 502–506), poniendo `rb->stream_eof = true` (línea 505).
- `audio_tick()` detecta `stream_eof` y pone `PLAYBACK_STOPPED` (líneas 138–140 de `src/audio.c`).
- Race potencial: `stream_eof` se escribe en el worker y se lee en `audio_tick`/`pad_update` sin lock, solo `volatile`. En PowerPC con `lwsync` en los accesos al ring, el booleano `stream_eof` no tiene barrera explícita aparte de las del ring.

---

## D) SEARCH — `net_search_async_query` / `net_search_async_poll` e integración en `ui.c`

### Implementación (`src/network.c` líneas 591–751)
- Worker de búsqueda en hilo PPU separado (`search_worker_func`, líneas 626–700), creado en `net_search_async_init()` (línea 702, llamado desde `net_init()` línea 58).
- Debounce de 300 ms (línea 638: `s_async_query_time + 300`).
- L1 LRU cache de 16 entradas (`s_search_cache[SEARCH_CACHE_MAX]`, líneas 600–601).
- `net_search_async_query()` (líneas 717–734): copia query, incrementa `s_async_query_seq`, pone `s_async_is_searching = true`.
- `net_search_async_poll()` (líneas 738–750): si `s_async_results_seq != s_last_polled_seq`, copia resultados y retorna count. Retorna -1 si no hay nuevos resultados.

### Integración en `ui.c`
- `gfx_init()` (líneas 103–109): busca "trending" de forma síncrona con `net_search("trending", ...)` antes de renderizar el primer frame.
- `gfx_render()` (líneas 175–218): llama `net_search_async_poll()` cada frame (línea 185). Si `async_count >= 0`, reemplaza `g_tracks` y `g_track_count`, resetea cursor si fuera de rango, y pide cover art del cursor (líneas 189–191).
- `pad_update()` en `STATE_SEARCH` (líneas 225–298): Cross sobre GO llama `net_search_async_query(g_search_query)` (línea 245) y vuelve a `STATE_HOME`. Cada pulsación de tecla en el OSK llama `net_search_async_query()` (líneas 158, 164, 252, 258, 264).
- `ui_play_track_at()` (líneas 41–55): resuelve stream con `net_resolve_stream()` y arranca `net_start_stream()`.

### ¿Resultados reales seleccionables?
- Sí: `net_search_async_poll` devuelve tracks y `gfx_render` los muestra en la lista. El cursor se mueve con D-Pad y Cross selecciona (líneas 336–347 de `ui.c`).
- Estado conocido: búsqueda "trending" devuelve 4 tracks reales (según contexto).

### Condiciones de carrera
1. **`g_tracks` se reemplaza sin sincronización con el render.** `net_search_async_poll` copia a `s_async_results` (en network.c) y luego `ui.c` hace `memcpy(out, s_async_results, ...)` dentro de `gfx_render`. Como todo corre en el hilo PPU principal (search worker escribe `s_async_results`, main loop lee en `gfx_render`), y el search worker duerme si `s_async_is_searching` es false o seq igual, no hay concurrencia real entre hilos para `s_async_results` en PS3 (solo un worker). Pero `g_tracks` en `ui.c` se modifica en `gfx_render` mientras `pad_update` puede leer `g_track_count` y `g_tracks[g_cursor]` — ambos en el mismo hilo, así que no es race de hilos, pero sí puede haber inconsistencia visual si el poll ocurre entre frames.
2. **`s_async_query_seq` vs `s_last_polled_seq`.** Si dos queries se envían rápido, el segundo incrementa `s_async_query_seq` antes de que el poll lea el primero. El worker solo procesa el seq actual (línea 632: `s_async_query_seq == last_handled_seq`). Se pierden queries intermedias (comportamiento aceptable de "último query gana").
3. **Race entre `net_stop_stream()` y `ui_play_track_at()`.** `ui_play_track_at` llama `net_start_stream` que llama `net_stop_stream` internamente (línea 553). Si se presiona Cross rápido sobre distintas pistas, `net_stop_stream` hace `platform_thread_join` bloqueante hasta que el worker anterior termine. En RPCS3, si el worker anterior está bloqueado en red, se congela el hilo principal (y por tanto RSX).
4. **Cover art request asíncrono sin sincronización con el track actual.** `cover_art_request()` se llama desde `gfx_render` (línea 190) y desde `ui_play_track_at` (línea 51). El worker de cover (`cover_worker_func`) usa `s_pending_id`/`s_pending_url` compartidos sin mutex — si dos requests se solapan en frames consecutivos, el segundo sobrescribe el primero antes de que el worker lo procese.

---

## E) BOOT / VFS — `src/platform_ps3.c`, `src/lv2.c`, `src/entry.s`

### cellSysmodule / inicialización de módulos
- **No hay llamada a `cellSysmoduleLoadModule` ni a `sys_net_initialize_network` real.** En `net_init()` (`src/network.c` línea 36) solo hay un comentario: `/* Real PS3: sys_net_initialize_network() */`. No se carga ningún módulo sysmodule (net, io, audio, sysutil, gcm).
- `platform_init()` (líneas 69–72) solo imprime TTY y retorna 0. No inicializa pads, audio, ni módulos de sistema.
- `cellPadInit` se llama lazy en `platform_poll_input()` (línea 1339).
- `cellAudioInit` se llama en `platform_audio_init()` (línea 1205).
- `cellGcmSys` (`_cellGcmInitBody`) se llama en `platform_gfx_init()` (línea 795).

### Rutas VFS
- `/dev_hdd0/game/MCRM00001/USRDIR/cache` y subdirectorios (líneas 87–89 de `platform_ps3.c`).
- `CERT_BUNDLE_PATH` = `/dev_hdd0/game/MCRM00001/USRDIR/certs/cacert.pem` (línea 44 de `include/monochrome.h`).
- `FONT_PATH` = `/dev_hdd0/game/MCRM00001/USRDIR/assets/font.ttf` (línea 45 de `include/monochrome.h`).
- No se usa `/app_home` en ningún lado (grep 0 matches para `/app_home`).
- `platform_file_open_read` (líneas 116–122) abre con flags 0 (`CELL_FS_O_RDONLY`).
- `platform_file_open_write` (líneas 124–132) usa flags `0x601` (WRONLY | CREAT | TRUNC).
- `platform_file_size` (líneas 158–172) lee el `st_size` del `CellFsStat` crudo en offsets 40–47 (big-endian).

### Apertura de assets / certs
- En `net_init()` (líneas 42–50) se abre `CERT_BUNDLE_PATH` con `platform_file_open_read`. Si falla, imprime WARN y continúa con verificación HTTPS deshabilitada.
- `FONT_PATH` está definido pero **no se usa en ningún lado** (no hay llamada a `platform_file_open_read(FONT_PATH)` ni carga de fuente). El render usa la bitmap font `FONT8` embebida (líneas 417–517 de `platform_ps3.c`).
- No hay código que abra assets embebidos del EBOOT (no hay `fopen` sobre recursos internos; todo va por sys_fs sobre `/dev_hdd0`).

### Entry point y OPD
- `src/entry.s`: define `ps3_entry_opd`, `ps3_thread_entry_stub_opd`, `ps3_flip_handler_cb_opd` como descriptores OPD oficiales de 32 bits [func, .TOC.].
- `_start_ps3` carga `.TOC.` desde OPD, alloca stack de 128 bytes, llama `ps3_main_entry` (que llama `main`).
- `sce_opd_call6` / `sce_opd_call6f` (líneas 66–93 de `entry.s`): trampolín ELFv1 que carga r2 = rtoc, r12 = OPD, hace `bctrl`, y restaura r2/LR del caller.

### Riesgos de boot/VFS
1. **No se carga `sys_net` módulo.** En PS3 real, sin `cellSysmoduleLoadModule(CELL_SYSMODULE_NET)` el socket layer no funciona. En RPCS3 HLE puede estar implícito, pero es un gap para hardware real.
2. **`cellPadInit` lazy sin verificación de módulo IO.** Si `sys_io` no está cargado, `cellPadInit` puede fallar y el pad queda inactivo (ver sección A).
3. **`FONT_PATH` y `CERT_BUNDLE_PATH` apuntan a `/dev_hdd0/game/MCRM00001/USRDIR/...`.** Si el PKG no instala `certs/cacert.pem` ni `assets/font.ttf`, la apertura falla silenciosamente (cert) o la fuente no se carga (aunque no se usa).
4. **No hay montaje de `/app_home`.** Si el flujo de boot espera leer algo desde `/app_home`, no está soportado.

---

## F) RAM XDR < 256MB — allocations estáticas y dinámicas

### Presupuesto declarado (`include/monochrome.h` líneas 12–12)
- Ring buffer FLAC: 8 MB XDR.
- PCM output double-buffer: 2 MB XDR.
- Cover art texture cache: 16 MB GDDR3.
- RSX framebuffers (720p x2): 7.2 MB GDDR3.
- App heap + stack: ~32 MB XDR.
- GameOS kernel reservation: ~48 MB XDR.

### Allocations estáticas reales (PS3)
| Archivo:línea | Objeto | Tamaño aproximado |
|---|---|---|
| `src/main.c:21` | `s_ring_storage[STREAM_RING_BUFFER_SIZE]` | 8 MB (BSS, alineado 128) |
| `src/platform_ps3.c:200` | `s_thread_pool[MAX_PLATFORM_THREADS]` | 8 × ~48 bytes ≈ 384 B (BSS) |
| `src/platform_ps3.c:411` | `s_fb[FB_HEIGHT][FB_WIDTH]` (720×480 uint32) | 1.3824 MB (BSS) |
| `src/platform_ps3.c:414` | `s_rsx[NUM_BUFS]` | 2 × sizeof(RsxBuf) ≈ 32 B |
| `src/platform_ps3.c:417–517` | `FONT8[95][8]` | 760 B (rodata) |
| `src/platform_ps3.c:1410–1426` | `k_catalog[]` | ~600 B (rodata) |
| `src/lv2.c:303` | `s_heap_pool[HEAP_POOL_SIZE]` | 16 MB (BSS) |
| `src/network.c:600` | `s_search_cache[SEARCH_CACHE_MAX]` | 16 × (128 + 20×sizeof(TrackMetadata) + 8 + 8) ≈ 16 × ~5.3 KB ≈ 85 KB (BSS) |
| `src/network.c:612` | `s_async_results[MAX_SEARCH_RESULTS]` | 20 × sizeof(TrackMetadata) ≈ 20 × ~900 B ≈ 18 KB (BSS) |
| `src/network.c:20` | `g_api_hosts[]` | punteros, despreciable |
| `src/ui.c:16` | `g_tracks[MAX_SEARCH_RESULTS]` | 20 × sizeof(TrackMetadata) ≈ 18 KB (BSS) |
| `src/ui.c:118` | `g_search_query[64]` | 64 B |
| `src/audio.c:26` | `g_pcm_block[AUDIO_BLOCK_FRAMES * 2]` | 256 × 2 × 2 B = 1 KB (BSS) |
| `src/audio.c:111` | `s_vis_bands[VIS_BANDS]` | 16 × 4 B = 64 B |
| `src/cover.c:22` | `s_current_cover` | 80 × 80 × 4 B + overhead ≈ 25.6 KB (BSS) |
| `src/cover.c:27–28` | `s_pending_id[64]`, `s_pending_url[256]` | 320 B |

**Total estático BSS/rodata estimado en PS3:** ~8 MB (ring) + 16 MB (heap lv2.c) + 1.38 MB (framebuffer software) + 25.6 KB (cover) + ~120 KB (caches/resultados) ≈ **~25.5 MB estáticos**.

### Allocations dinámicas (heap de bump 16 MB en `src/lv2.c:303–352`)
- `malloc`/`calloc`/`realloc`/`memalign` son un **bump allocator** sobre `s_heap_pool` de 16 MB. `free()` es no-op (línea 319–321). No hay reutilización de memoria liberada: cada `malloc` avanza el bump y nunca retrocede.
- `network.c:380`: `malloc(ctx.capacity)` = 16 MB (STREAM_BUFFER_CAPACITY en PS3) para `ctx.data` del stream worker. Esto **agota casi todo el heap de 16 MB de una sola asignación**.
- `network.c:411`: `malloc(c_size)` para cached FLAC (tamaño variable del archivo cacheado).
- `platform_ps3.c:791`: `memalign(0x100000, 0x100000)` = 1 MB para command ring RSX.
- `platform_ps3.c:818–821`: `rsxMemalign(64, buf_size)` por framebuffer (2 × 720×480×4 = 2.76 MB, pero cada uno se reserva a 1 MB granularidad por `GCM_FB_ALLOC_SIZE` = 0x200000 = 2 MB cada uno → 4 MB total reservados).
- `platform_ps3.c:1431`: `malloc(n + 1)` para respuesta HTTP fallback (pequeño).
- `cover.c`: `stbi_load_from_memory` asigna internamente (stb_image usa `malloc` del heap bump), `stbi_image_free` no libera nada (free es no-op).

### Riesgos de RAM
1. **Heap bump de 16 MB sin free real.** `free()` es no-op. Cada asignación es permanente hasta el reinicio. El stream worker reserva 16 MB (`ctx.data`) — eso consume prácticamente todo el heap disponible. Cualquier `malloc` posterior (JSON parsing, cover download buffer, caché de FLAC) puede fallar por `s_heap_used + aligned_size > HEAP_POOL_SIZE` (línea 310 de `lv2.c`).
2. **Framebuffer software `s_fb[480][720]` = 1.38 MB en BSS** más 2 framebuffers RSX de ~2 MB cada uno (reservados con 1 MB granularidad). Total GPU-related ~5.4 MB.
3. **Ring buffer 8 MB estático** es aceptable dentro de 256 MB, pero junto con 16 MB heap + 1.38 MB FB software + cover cache 25.6 KB, el consumo estático es ~25.5 MB, dejando ~230 MB teóricos. El problema real es el heap bump de 16 MB sin reciclaje.

---

## Hallazgos priorizados (P0/P1/P2)

### P0 — Bloqueantes funcionales
1. **Input de pad ausente en RPCS3.** `platform_poll_input()` depende de `SCE_BOUND(cellPadInit)`/`SCE_BOUND(cellPadGetData)` pero nunca informa si fallan; activa fallback auto-demo que oculta el problema. No hay `cellSysmoduleLoadModule` de `sys_io` ni verificación de que RPCS3 exponga cellPad. → Revisar imports de `sce_imports.py`, agregar trazas de fallo y asegurar inicialización de `sys_io` antes de `cellPadInit`.
2. **`cellOskDialog` no usado.** El goal exige `cellOskDialog`; la app usa OSK propio de software. No hay slot de importación ni wrapper de `cellOskDialog`. → Agregar slot, wrapper y fallback degradado.
3. **Heap bump de 16 MB con `free()` no-op y asignación de 16 MB para `ctx.data` del stream.** Riesgo alto de fallo de asignación tras el primer stream; sin reciclaje, el heap se agota. → Implementar `free` real o usar un pool fijo para `ctx.data`.

### P1 — Riesgos de bloqueo y concurrencia
4. **`net_stop_stream()` bloqueante con `platform_thread_join`.** Si el stream worker está bloqueado en red/FLAC, el hilo principal (RSX) se cuelga al detener. → Hacer `net_stop_stream` no bloqueante o con timeout.
5. **`platform_audio_write()` busy-wait hasta 10 ms esperando DMA.** Si cellAudio HLE no avanza, el hilo principal se bloquea. → Añadir timeout menor y no bloquear el frame.
6. **Race de cover art:** `s_pending_id`/`s_pending_url` compartidos sin protección entre `gfx_render` y `cover_art_request`. → Serializar requests de cover.

### P2 — Pulido y robustez
7. **Sin deadzone / repetición de botones.** Solo detección de flanco; sin repetición por retención para navegación. → Añadir temporizador de repetición.
8. **`FONT_PATH` definido pero no usado; `/app_home` no soportado.** → O usar la fuente TTF o eliminar la definición.
9. **EOF de `stream_eof` sin barrera explícita aparte del ring.** → Añadir `lwsync` al leer `stream_eof` desde el hilo principal.
10. **Trayectoria de flac_stream_seek puede bloquear indefinidamente** si `new_pos > ctx->received` y nunca llega EOF (líneas 296–299 de `network.c`). → Añadir timeout.

---

## Parches mínimos propuestos (sin aplicar)

### Parche A1 — Input: traza y verificación de `cellPad` (P0)
En `src/platform_ps3.c`, `platform_poll_input()`:
```c
if (!s_pad_ready && SCE_BOUND(cellPadInit)) {
    if (pad_init(7) == 0) { s_pad_ready = 1; tty("[PS3] cellPadInit OK\n"); }
    else { tty("[PS3] cellPadInit FAILED (%d)\n", ret); }  // traza de fallo
}
if (!s_pad_ready || !SCE_BOUND(cellPadGetData)) {
    tty("[PS3] Pad unavailable — auto-demo fallback active\n");  // traza
    ... fallback auto-demo ...
}
```
También asegurar inicialización de `sys_io` antes de `cellPadInit` en `platform_init()` o al inicio de `main()`.

### Parche B1 — OSK: `cellOskDialog` (P0)
1. `include/platform.h`: añadir `int platform_osk_open(const char *title, const char *initial, char *out, size_t out_len);`
2. `src/platform_ps3.c`: añadir `SCE_SLOT(cellOskDialog);` y wrapper con `sce_opd_call6`.
3. `src/ui.c`: en `STATE_SEARCH` (línea 385), intentar `platform_osk_open`; si falla o no está bound, usar OSK propio actual.

### Parche C1 — Heap / stream: `free` real o pool fijo (P0)
En `src/lv2.c`: implementar `free()` que marque bloques como libres (o al menos reduzca `s_heap_used` si es el último allocations), o cambiar `ctx.data` en `network.c` a un buffer estático de 16 MB en BSS para no consumir el heap bump.

### Parche D1 — `net_stop_stream` no bloqueante (P1)
En `src/network.c`, `net_stop_stream()`: no hacer `platform_thread_join` en el hilo principal; marcar `abort_requested` y dejar que el worker termine solo, o hacer join con timeout.

### Parche D2 — Cover art serializado (P1)
En `src/cover.c`: usar una cola de requests o un flag "busy" para que `cover_art_request` no sobrescriba un request pendiente sin procesar.

---

## Conclusión
El código compila y arranca a 60 FPS en RPCS3 con búsqueda funcional, pero **no hay evidencia real de input de pad** (fallback auto-demo ocultando la falta de `cellPad`), **no hay `cellOskDialog`** (se requiere por goal), y **el streaming tiene riesgos de bloqueo** (`net_stop_stream` join bloqueante, `platform_audio_write` busy-wait, heap bump sin `free` real). Los parches P0 anteriores son los mínimos para lograr input real, OSK nativo y streaming estable sin corromper la memoria de 256 MB XDR.
