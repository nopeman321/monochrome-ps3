# Plan de Implementación: Fail-Fast y Auto-Fallback en Streaming (HTTP 404)

> **Fecha:** 2026-11-23  
> **Target:** PS3HEN / CFW (`monochrome_ps3.pkg` / `MONO00024`)  
> **Directorio de trabajo:** `e:\DSH\Monochrome-PS3\monochrome-ps3-2.4.0`

---

## 1. Contexto y Problema

Cuando el usuario busca canciones en Monochrome PS3, el backend remoto (`tracks.monochrome.st/search`) devuelve pistas de catálogo con `"playable": true`. Sin embargo, algunos IDs específicos (como `154008218635341824` - versión de single viejo de *"Like That"*) han sido deslistados o eliminados del CDN de streaming, respondiendo con **HTTP 404 Not Found**.

### Deficiencias actuales en el cliente:
1. **Reintentos lentos e inútiles en 404:** `stream_connect_http` trata el código 404 como un error transitorio y reintenta 8 veces con retroceso exponencial (2s, 4s, 8s, 15s...), congelando el pipeline de audio durante más de 70 segundos.
2. **Timeout de prebuffer desorientador:** Mientras la red reintenta, el hilo decodificador espera 25 segundos y luego emite `[DECODE ERROR] magic no es fLaC` y `"Formato no soportado"`, un mensaje engañoso ya que el problema real fue un 404.
3. **Falta de visibilidad en UI:** El dock inferior no refleja el error HTTP ni avisa al usuario qué ocurrió.
4. **Falta de Auto-Fallback:** Si en la misma lista de resultados de búsqueda existe otra versión válida de la misma canción (ej. opción [4] de *"Like That"* con ID `157679656464437248`), el sistema no la intenta automáticamente.

---

## 2. Objetivos de la Mejora

1. **Fail-Fast en Errores Permanentes (404 / 403 / 410):**
   - Cortar inmediatamente la conexión sin reintentar.
   - Abortar de inmediato el prebuffer del decodificador (reduciendo el tiempo de espera de 25s a <200ms).
   - Registrar el código exacto de error (`s_last_error_code = 404`) y estado `"Pista no disponible (404)"`.
2. **Visualización en UI:**
   - Mostrar el estado de error en el dock inferior (`ui_player_dock.c`) en color de advertencia (`0xFFFF4444`).
3. **Auto-Fallback Inteligente en Búsqueda (`ui_search.c`):**
   - Si la pista seleccionada devuelve 404, buscar automáticamente en los resultados actuales otra versión con el mismo título/artista.
   - Si existe una alternativa, reproducirla de inmediato y registrar el fallback.
   - Si no hay alternativa, detenerse limpiamente y mostrar `"Pista no disponible (404)"`.

---

## 3. Plan de Tareas

### Tarea 1: Motor de Audio — Fail-Fast y Señalización de Errores HTTP
- **Archivos:**
  - `include/audio_engine.h`
  - `source/audio_engine.c`
- **Acciones:**
  1. Declarar `int audio_engine_get_last_error(void);` y `void audio_engine_clear_error(void);` en `include/audio_engine.h`.
  2. En `stream_connect_http()`: Si `http_code == 404 || http_code == 403 || http_code == 410`, registrar `s_last_error_code = http_code`, cerrar socket y retornar `-http_code` de inmediato sin reintentar.
  3. En `mono_net_worker_func()`: Si `sock < 0` con código negativo, asignar mensaje descriptivo (`"Pista no disponible (404)"`), marcar `s_has_error = true`, `s_is_playing = false`, `s_stream_ctx.abort = true`.
  4. En `stream_decode_worker_func()`: Comprobar `s_stream_ctx.abort || s_has_error` en el bucle de espera de prebuffer para salir inmediatamente. Omitir el log de error de magic FLAC si la sesión fue abortada por red.
  5. En `audio_engine_play_track()`: Restablecer limpiamente `s_has_error = false`, `s_last_error_code = 0` y `s_status_message = "Conectando..."`.

### Tarea 2: Feedback Visual en el Dock Persistente
- **Archivo:** `source/ui_player_dock.c`
- **Acciones:**
  1. En `player_dock_render()`: Si `has_track` es verdadero y `audio_engine_has_error()` es `true`, dibujar el texto de `audio_engine_get_status_message()` en lugar de la onda/progreso en color de advertencia `0xFFFF4444`.

### Tarea 3: Auto-Fallback Inteligente en la Vista de Búsqueda
- **Archivo:** `source/ui_search.c`
- **Acciones:**
  1. Mantener un registro del índice reproducido `s_last_played_idx` y un arreglo booleano de pistas fallidas `s_failed_results[MAX_SEARCH_RESULTS]`.
  2. Al detectar error 404 en el frame:
     - Marcar el índice actual como fallido.
     - Recorrer `s_results` buscando otra pista no fallida con el mismo título (comparación case-insensitive).
     - Si se encuentra, invocar `play_track()` con la alternativa y mover el cursor a ella.
     - Si no se encuentra, dejar de buscar y mostrar el error.
  3. Al iniciar una nueva búsqueda (`http_search_async_start`), limpiar `s_failed_results`.

### Tarea 4: Compilación y Despliegue en PS3
- **Acciones:**
  1. Ejecutar `tools/run_in_docker.ps1` usando el contenedor `ghcr.io/altps3/ps3dev:latest`.
  2. Verificar 0 warnings y 0 errores.
  3. Validar generación de `monochrome_ps3.pkg` con timestamp actualizado y hash SHA256.
  4. Subir por FTP a `/dev_hdd0/packages/` e instalar vía webMAN MOD en la PS3 (`http://192.168.18.103/install.ps3/...`).
