# Monochrome PS3 — Release Notes

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
