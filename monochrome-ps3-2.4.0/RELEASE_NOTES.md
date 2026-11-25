# Monochrome PS3 — Release Notes

## v2.4.11 — Red PS3 Blindada, Pestañas Activas y Categoría Música en XMB

**Title ID:** `MONO00024` · **Categoría XMB:** Música (`AM`) · **Formato:** Free NPDRM CEX PKG (`monochrome_ps3.pkg`) · **Destino:** PS3 (PS3HEN / CFW) y RPCS3

---

### Parte 1 · Novedades para Usuarios

- **Ahora en la columna Música del XMB (`AM`):** Monochrome PS3 pasa de la columna *Juego* a aparecer directamente en **Música** junto a tu biblioteca multimedia en PS3. *(Si actualizas sobre una instalación anterior en Juego, reinicia el XMB o reinstala el `.pkg` para que el menú mueva el icono.)*
- **Conexión en PS3 real corregida:** se resolvió el fallo que impedía conectar desde consola física tras la actualización de red; la búsqueda inicial, las carátulas y el catálogo cargan automáticamente al obtener IP.
- **Pestañas `L1 / R1` funcionando en toda la app:** ya puedes alternar entre **ALL**, **ALBUMS**, **ARTISTS**, **PLAYLISTS** y **TRACKS** desde *Search*, *Home* y *Library*, abrir álbumes/artistas/playlists con `✕` y explorar sus pistas.
- **Estabilidad de reproducción continua:** reproducción FLAC más estable sin pausas prematuras, con soporte para pistas Hi-Res y gestión automática de caché para no llenar el disco de la consola.
- **Nueva galería Tiny3D v3:** capturas actualizadas tomadas sobre la interfaz actual en `README.md`.

---

### Parte 2 · Detalles Técnicos de la Release (`v2.4.9` → `v2.4.11`)

1. **Correcciones críticas de red en PSL1GHT (`net_hal.c`, `http_client.c`):**
   - Reemplazo de `memset` sobre `sys_mutex_t` por `sysMutexAttrInitialize` en `init_mutex()` para evitar fallos de inicialización de primitivas LV2 en hardware real.
   - Sustitución de `select()` / `FD_SET` por `poll()` (`<net/poll.h>`) debido a que PSL1GHT asigna descriptores con `SOCKET_FD_MASK = 0x40000000`, evitando errores/corrupción fuera de rango en `fd_set`.
   - Corrección de lectura de errores (`errno` priorizado sobre `net_errno`) y registro explícito de transiciones de estado en `NetCtl` y cliente HTTP.
   - Hardening completo previo (`v2.4.9`): capa `net_hal` con caché DNS (TTL 60 s), worker `mono_search` con `sysCond`, `cJSON` acotado a `CJSON_MAX_DEPTH = 64`, redirecciones acotadas y logger rotativo en `/dev_hdd0/tmp/mono_stage6.log`.

2. **Enrutamiento de pestañas y sincronización de catálogo (`ui_v3.c`, `ui_search.c`, `ui_home.c`, `ui_library.c`, `app_state.c`):**
   - `ui_handle_input()` en `ui_v3.c` ahora evalúa `visualizer_fullscreen` y `right_drawer_open` antes del bloque de `SEC_SEARCH`, y permite que `ACT_L1` / `ACT_R1` alcancen `shell_handle_input()`.
   - `ui_search_update()` sincroniza `artists` y `playlists` además de `tracks` y `albums` hacia `g_app_state`.
   - Nueva función compartida `app_state_get_tab_items()` consumida por `ui_home` y `ui_library`, junto con helpers `ui_search_open_album()`, `ui_search_open_artist()` y `ui_search_open_playlist()` para drill-down seguro sin intentar reproducir IDs no reproducibles como audio.

3. **Audio Engine, RSX y XMB Packaging (`audio_engine.c`, `cover_art.c`, `rsxutil.h`, `sfo.xml`, `Makefile`):**
   - `PREBUFFER_THRESHOLD` de 2 MB, reanudación HTTP `Range` acotada, downsampling automático de flujos Hi-Res, caché en `/dev_hdd0/tmp/monochrome/cache/` limitada a `25` archivos / `512 MB`, `HOST_SIZE = 8 MB` en RSX y optimización `-O2`.
   - `CATEGORY` cambiado a `AM` en `sfo.xml` y `tools/sfo_builder.py`, enlazando explícitamente `SFOXML := $(CURDIR)/sfo.xml` en `Makefile` (corrigiendo el fallback previo a `$(PS3DEV)/bin/sfo.xml`).

---

## 🚀 v2.4.0 — Actualización Oficial de Estabilidad y Navegación

**Title ID**: `MONO00024`  
**Display Version**: `v2.4.0`  
**Package Format**: Free NPDRM CEX PKG (Signed)  
**Target Hardware**: Sony PlayStation 3 (Super Slim / Slim / FAT) con PS3HEN v3.x o CFW Evilnat 4.88-4.92+  

---

### 🌟 Novedades y Correcciones Críticas en v2.4.0

1. **Resolución Definitiva de Salida Limpia al XMB (Fix Triple Pitido y Watchdog)**:
   - Integración nativa del bucle de eventos de GameOS (`sysUtilRegisterCallback` en SLOT0 y `sysUtilCheckCallback` por frame).
   - Intercepción de `SYSUTIL_EXIT_GAME` (`0x0101`) del botón PS y desacoplamiento mediante bandera atómica latente.
   - Teardown de audio determinista: corrección del centinela de puerto `0` en `libaudio`, desmontaje de la cola de eventos y llamada a `audioQuit()` para liberar canales IPC del demonio `audiod`.
   - Eliminación de bloqueos `rsxFinish(1)` y `sysThreadJoin()` en hilos de red, retornando al XMB en menos de 15 ms sin reinicios forzados.

2. **Arranque Inmediato sin Pantalla en Negro (Fix Cabeceras SELF)**:
   - Declaración de `SYS_PROCESS_PARAM(1001, 0x100000)` para generar la cabecera `PT_SCE_PROC_PARAM` (0x60000001) válida de 0x20 bytes.
   - Prevención de rechazos del cargador ELF de GameOS al iniciar la app desde el XMB.

3. **Drill-down Completo de Álbumes, Artistas y Playlists**:
   - Corrección de consultas REST con protocolo forzado `HTTP/1.0` y `Accept-Encoding: identity`, evitando fragmentación *chunked* de Cloudflare que provocaba listas vacías ("0 pistas").
   - Aislamiento del cursor al abrir álbumes, permitiendo navegar y reproducir álbumes completos ordenados por `trackNumber` (1..N).
   - Implementación del endpoint `/artists/<id>` para desplegar directamente las canciones más populares del artista (`topTracks`).

4. **Mejoras Ergonómicas de Control y Limpieza Visual**:
   - Botón **`SELECT`**: Abre y cierra el menú contextual del sistema de forma global.
   - Botón **`START`**: Actúa exclusivamente como **GO** (ejecutar búsqueda inmediata) en la pantalla de búsqueda OSK.
   - Botones **`L1 / R1`**: Navegan fluidamente entre las 8 pestañas principales del sistema en todas las pantallas.
   - Botones **`L2 / R2`**: Conmutan exclusivamente las sub-pestañas de categoría (Tracks, Albums, Artists, Playlists).
   - Eliminación del número de versión del HUD superior para una interfaz más limpia y minimalista.

---

## 🚀 v2.3.0 — Versión Final Oficial para PlayStation 3

**Title ID**: `MONO00024`  
**Display Version**: `v2.3.0`  
**Package Format**: Free NPDRM CEX PKG (Signed)  
**Target Hardware**: Sony PlayStation 3 (Super Slim / Slim / FAT) con PS3HEN v3.x o CFW Evilnat 4.88-4.92+  

---

### 🎵 Novedades y Mejoras Clave

1. **Streaming y Reproducción Lossless FLAC**:
   - Streaming continuo y fluido desde `tracks.monochrome.st`.
   - Pipeline de audio en tiempo real con hilo DMA a prioridad 100 de GameOS.
   - Socket buffer TCP de 512 KB y llamadas no bloqueantes para eliminar microcortes o lags en la reproducción de pistas Hi-Res.
   - Prebuffering ampliado a 4 MB para máxima estabilidad de red.

2. **Calibración y Ajuste de Pantalla (Screen Adjust)**:
   - Ajuste ultra-fino de posición píxel a píxel (paso 1px).
   - Ajuste ultra-fino de escala global (paso 0.005 con L2/R2).
   - Ajuste fino del tamaño de fuente (paso 0.02 con L1/R1).
   - Persistencia automática en disco de los valores elegidos (`screen_config.txt`), aplicándose automáticamente desde el primer frame de inicio.

3. **Interfaz y Arte Minimalista Metalheart**:
   - Splash screen de inicio con banner ASCII y especificaciones del motor LPCM 48kHz.
   - Fondo técnico con marcas y cuadrículas tenues sobre negro puro (`0x00000000`).
   - Esquema de colores sobrio: líneas blancas puras, texto de alto contraste y detalles en carmesí web Monochrome (`#C83838`).

4. **Integración Total con el XMB**:
   - Icono oficial `ICON0.PNG` adaptado a la relación de aspecto nativa (320×176).
   - Fondo panorámico oscuro `PIC1.PNG` (1920×1080) asegurando que el texto blanco del XMB mantenga total legibilidad.

5. **Navegación Web Completa (8 Pestañas EGM)**:
   - Cobertura 100% de la web oficial `monochrome.st`: Home, Search (con sub-pestañas Tracks, Albums, Artists, Playlists, Podcasts y OSK interactivo), Library, Recent, Unreleased, Donate, Settings y About.
   - Navegación global inmediata mediante gatillos L1/R1 y alternancia de filtros con L2/R2.

6. **Subsistema Asíncrono de Carátulas (`mono_cover`)**:
   - Descarga y descompresión JPEG delegada a un hilo PPU independiente en segundo plano (prioridad 1500, stack 128 KB).
   - Protección contra fallos y corrupción de datos con manejador `custom_jpeg_error_mgr` (`setjmp`/`longjmp`), erradicando cualquier riesgo de cuelgue o caída al XMB.
   - Enrutamiento dinámico de URLs HTTP/HTTPS respetando host y puerto asignado.

7. **Renderizado de 60 FPS y Prevención de Desbordamiento**:
   - Eliminación de 102.400 divisiones escalares por cuadro con la tabla precalculada `tx_lut[1920]`.
   - Ráfagas alineadas de 64 bits hacia la VRAM de la RSX para descongestionar el bus FlexIO.
   - Sistema de recorte de texto adaptativo (`platform_draw_text_clipped`) con elipsis automática (`...`) que garantiza que ningún título, URL o ficha técnica desborde los marcos o se salga de la pantalla.
