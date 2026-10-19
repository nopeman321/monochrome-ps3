# Auditoría Técnica y Especificación de Diseño UI/UX para PC (Monochrome Lossless)

**Documento:** `docs/PC_FRONTEND_UI_UX_AUDIT_DESIGN.md`  
**Subagente:** Subagente 3 — Ingeniero de Frontend y UI/UX en PC  
**Fase:** FASE 1 — Auditoría Integral y Arquitectura Visual de Alta Fidelidad (1280x720)  
**Fecha:** 19 de Octubre de 2026  

---

## 1. Resumen Ejecutivo

La presente auditoría evalúa el estado actual de la interfaz de usuario del cliente PC de **Monochrome** implementada en `src/ui.c`, `src/platform_pc.c`, `src/main.c`, `include/monochrome.h` y `include/platform.h`, comparándola contra los estándares visuales y de interacción del cliente web oficial ubicado en `upstream/src/` (`AppHeader.tsx`, `AppNavigation.tsx`, `NowPlayingBar.tsx` y `upstream/styles.css`).

A pesar de que el cliente PC cuenta con un backend de audio de alta fidelidad (waveOut 48 kHz LPCM) y un lienzo GDI funcional a 1280x720 con doble búfer (`CreateCompatibleDC` / `BitBlt`), la interfaz sufre de severas limitaciones estructurales:
1. **Ausencia de navegación moderna:** Falta de una barra lateral (Sidebar) persistente, concentrando todo en una lista monolítica.
2. **Entrada de búsqueda anacrónica:** Uso de un teclado en pantalla (On-Screen Keyboard - OSK) de 4x10 heredado del gamepad de PS3 en lugar de un campo de búsqueda interactivo con teclado físico.
3. **Catálogo limitado y estático:** Restringido a 10 elementos visibles sin soporte de scroll con rueda de ratón (`WM_MOUSEWHEEL`) ni carátulas integradas en la lista.
4. **Dock de reproducción tosco:** Barra inferior con controles de texto plano, sin scrubber interactivo en la barra de progreso, sin slider de volumen visual y con un espectrómetro estático sin estética moderna.
5. **Interacción con mouse deficiente:** Cero respuesta de hover, cursor de sistema plano (sin `IDC_HAND`), y zonas de clic rígidas y limitadas.

Este documento establece la **arquitectura visual refactorizada** para transformar el cliente en una aplicación de escritorio moderna, fluida y de grado audiófilo, manteniendo un 100% de paridad con la capa de abstracción de hardware (HAL) compartida con PS3.

---

## 2. Auditoría Detallada del Frontend Actual

### 2.1. Análisis Componente por Componente

| Componente | Implementación Actual (`src/platform_pc.c` / `src/ui.c`) | Deficiencias Críticas | Referencia Oficial Web (`upstream/`) |
| :--- | :--- | :--- | :--- |
| **Estructura Global** | Lienzo fijo 1280x720. Solo franja superior (80px), lista central y dock inferior (125px). | No existe barra lateral (Sidebar). No hay segmentación entre vistas (Home, Search, Queue, Library). | Layout modular de 3 columnas/áreas: Sidebar lateral (240px), Main Content con Header dinámico, y Player Bar fija (90px). |
| **Buscador & Input** | Modal `STATE_SEARCH` con cuadrícula de 40 teclas virtuales (OSK). Se ciclan presets con la tecla `S`. | En PC con teclado físico es sumamente frustrante no poder escribir directamente. Bloquea la pantalla completa con un teclado virtual. | Input interactivo en el header con icono de lupa, placeholder reactivo, botón de limpieza `[×]` y búsqueda en vivo sin bloquear la vista. |
| **Lista de Canciones** | Filas alternadas (`RGB(17,18,22)` y `RGB(22,23,29)`). Límite rígido de 10 canciones en pantalla. | Desperdicia el espacio horizontal. No hay carátulas en las pistas. Si hay 20 resultados, las últimas 10 nunca se pueden ver porque no hay scroll. | Lista con carátula en miniatura o número, título destacado, artista, álbum, badge "HI-RES" / "FLAC", duración y scroll suave. |
| **Now Playing Dock** | Carátula 80x80 a la izquierda. Texto "PLAYING (48,000 Hz)". Barra de progreso fija blanca. | Controles de texto no clickeables en su mayoría. No se puede arrastrar ni hacer clic en la barra de progreso (sin seeking). No hay barra de volumen. | Controles de transporte con iconos limpios (Shuffle, Prev, Play/Pause central prominente, Next, Repeat), scrubber interactivo y control de volumen. |
| **Visualizador de Audio**| 16 rectángulos blancos dibujados en una esquina (30px de alto máx). | Gráficos toscos, sin gradiente, sin decaimiento balístico ni sensación de analizador de espectro profesional. | Espectrómetro estilizado con peak-hold y gradiente acorde a la paleta monocromática. |
| **Soporte de Mouse** | Detección rudimentaria de `WM_LBUTTONDOWN` sobre filas 0-9 y una caja para play/pause. | Sin `WM_MOUSEMOVE` (sin hover), sin cambio de cursor a `IDC_HAND`, sin `WM_MOUSEWHEEL` para scroll, sin interacción con volumen ni scrubber. | Experiencia de usuario de escritorio completa con hover reactivo, cursor dinámico, drag & drop en barras y scroll de catálogo. |

---

## 3. Especificación de Diseño y Arquitectura Visual (1280 x 720)

### 3.1. Paleta de Colores Oficial (Tema Monochrome)

Basado en la especificación de diseño oficial de `upstream/styles.css` (`:root[data-theme='monochrome']`):

```
Fondo Principal (Canvas):       #0A0A0C  -> RGB( 10,  10,  12)
Fondo Paneles (Sidebar/Dock):   #111216  -> RGB( 17,  18,  22)
Fondo Tarjetas / Filas Par:     #14161C  -> RGB( 20,  22,  28)
Fondo Filas Impar:              #181A22  -> RGB( 24,  26,  34)
Fondo Hover / Resaltado:        #222530  -> RGB( 34,  37,  48)
Selección Activa / Acento:      #2A2E3D  -> RGB( 42,  46,  61)
Bordes y Divisores:             #252834  -> RGB( 37,  40,  52)
Borde Enfocado / Glow:          #4B5268  -> RGB( 75,  82, 104)

Texto Primario (Títulos):       #F5F5F7  -> RGB(245, 245, 247)
Texto Secundario (Artistas):    #A0A5B5  -> RGB(160, 165, 181)
Texto Muted (Detalles/Álbum):   #6E7385  -> RGB(110, 115, 133)
Acento Blanco Puro:             #FFFFFF  -> RGB(255, 255, 255)

Badge Hi-Res (Oro / Ámbar):     #FFD166  -> RGB(255, 209, 102)
Badge Lossless (Cian / Azul):   #64C8FF  -> RGB(100, 200, 255)
Estado Online (Verde Esmeralda):#10B981  -> RGB( 16, 185, 129)
Estado Pausa (Ámbar Cálido):    #F59E0B  -> RGB(245, 158,  11)
```

### 3.2. Tipografía y Jerarquía Visual
- **Logotipo y Título Principal:** `Segoe UI`, 22pt, `FW_BOLD`, Kerning ampliado.
- **Títulos de Canción (Now Playing):** `Segoe UI`, 15pt, `FW_SEMIBOLD`.
- **Títulos en Catálogo:** `Segoe UI`, 14pt, `FW_SEMIBOLD`.
- **Artistas y Navegación:** `Segoe UI`, 13pt, `FW_NORMAL`.
- **Metadatos y Badges:** `Segoe UI`, 11pt, `FW_NORMAL`.
- **Tiempos y Frecuencias (DAC/Reloj):** `Consolas`, 12pt, `FW_NORMAL` (ancho fijo para evitar jitter numérico).

---

## 4. Mockup y Wireframe Textual del Layout (1280 x 720)

```
0,0 ─────────────────────────────────────────────────────────────────────────────────────────────────── 1280,0
│  SIDEBAR (0 to 230)   │                      MAIN HEADER & SEARCH BAR (230 to 1280)                      │
│ ┌───────────────────┐ │ ┌───────┐ ┌───────────────────────────────────────────────┐ ┌──────────────────┐ │
│ │ ◈  M O N O C H R. │ │ │ <   > │ │ 🔍 Buscar canciones, artistas o álbumes...   | │ │ [24-BIT/96k] [●] │ │
│ └───────────────────┘ │ └───────┘ └───────────────────────────────────────────────┘ └──────────────────┘ │
│                       ├──────────────────────────────────────────────────────────────────────────────────┤ Y: 75
│  EXPLORAR             │  CATÁLOGO: Tendencias Globales  (20 canciones cargadas)    Filtro: [Lossless ▼]  │
│  [◈] Inicio           │ ──────────────────────────────────────────────────────────────────────────────── │ Y: 125
│  [🔍] Búsqueda        │  #   PORTADA  TÍTULO                   ARTISTA         ÁLBUM          CALIDAD   │
│  [☰] Cola             │ ──────────────────────────────────────────────────────────────────────────────── │ Y: 155
│  [♡] Biblioteca       │  01  [ART]    Get Lucky (feat. Phar.)  Daft Punk       Random Access  [HI-RES] 04:08  │
│                       │  02  [ART]    Starboy (feat. Daft P.)  The Weeknd      Starboy        [HI-RES] 03:50  │
│  GÉNEROS RÁPIDOS      │ ▶03  [ART]    One More Time            Daft Punk       Discovery      [FLAC]   05:20  │  ◄ Hover / Sel
│  • Trending           │  04  [ART]    Blinding Lights          The Weeknd      After Hours    [FLAC]   03:20  │
│  • Synthwave          │  05  [ART]    Instant Crush            Daft Punk       Random Access  [HI-RES] 05:37  │
│  • Electronic         │  06  [ART]    Midnight City            M83             Hurry Up...    [FLAC]   04:03  │
│  • Lossless Hi-Fi     │  07  [ART]    Around the World         Daft Punk       Homework       [FLAC]   07:09  │
│                       │  08  [ART]    Harder, Better, Faster   Daft Punk       Discovery      [FLAC]   03:44  │
│  ESTADO DE RED        │  09  [ART]    Resonance                HOME            Odyssey        [FLAC]   03:32  │
│  ● Tidal CDN Activo   │  10  [ART]    Nightcall                Kavinsky        OutRun         [FLAC]   04:19  │
│  DAC: 48kHz LPCM      │  [ ▲▼ Scroll disponible con rueda del ratón: 10 de 20 visibles ]                 │
├───────────────────────┴──────────────────────────────────────────────────────────────────────────────────┤ Y: 625
│ NOW PLAYING ELEVATED DOCK (Y: 625 to 720)                                                                │
│ ┌─────┐ One More Time                 [🔀]  [⏮]   ( ▶ )   [⏭]  [🔁]      AUDIO VISUALIZER   VOL: 100%    │
│ │ 80x │ Daft Punk — Discovery        01:45 ━━━━━━━●──────────── 05:20    ılılıııılılııı     🔊 ━━━━━━━●  │
│ │ 80  │ Lossless FLAC 24b/96k         [Shuf: OFF]  [Rep: ALL]            16-Band Real-Time  Mute: [M]    │
│ └─────┘                                                                                                  │
0,720 ──────────────────────────────────────────────────────────────────────────────────────────────── 1280,720
```

---

## 5. Distribución de Coordenadas y Geometría en Pantalla

### 5.1. Barra Lateral (Sidebar): `X: 0 → 230`, `Y: 0 → 625`
- **Fondo:** `#111216`, Borde derecho separador `#252834` (1px).
- **Logotipo:** `X: 25, Y: 22`. Isotipo gráfico estilizado `◈` y texto `M O N O C H R O M E`.
- **Items de Navegación (`Y: 90 → 230`, Altura 36px por item):**
  - Item 0: *Inicio* (`Y: 90`)
  - Item 1: *Búsqueda* (`Y: 130`)
  - Item 2: *Cola de Reproducción* (`Y: 170`)
  - Item 3: *Biblioteca* (`Y: 210`)
  - *Estado hover:* Fondo `#1A1C24` con radio de 6px y texto en blanco brillante.
- **Sección Presets Rápidos:** `Y: 280 → 460`. Tags clickeables con géneros comunes.
- **Panel Inferior de Diagnóstico:** `Y: 550 → 615`. Muestra `● ONLINE: tracks.monochrome.st` y estadísticas del DAC.

### 5.2. Barra Superior (Header & Search): `X: 230 → 1280`, `Y: 0 → 75`
- **Fondo:** `#0E0F12`, Borde inferior `#252834` (1px).
- **Controles de Historial:** Botones Atrás `<` (`X: 245`) y Adelante `>` (`X: 285`).
- **Campo de Búsqueda Activo:**
  - `X: 335 → 780` (Ancho: 445px), `Y: 18 → 56` (Alto: 38px).
  - Fondo: `#181A22`, Borde `#323646` (cambia a `#FFFFFF` cuando tiene foco).
  - Icono de lupa `🔍` en `X: 348, Y: 28`.
  - Placeholder: `"Buscar canciones, artistas o álbumes..."` en `#6E7385`.
  - Cursor interactivo `|`: Parpadeo cada 500ms utilizando `(GetTickCount64() / 500) % 2`.
  - Botón de limpieza `[×]` en `X: 755`: Borra la cadena instantáneamente.
- **Badges de Audio y Calidad:**
  - Badge 1 (`X: 880 → 1050`, `Y: 22 → 52`): `24-BIT / 96 KHZ · LOSSLESS`.
  - Badge 2 (`X: 1065 → 1245`, `Y: 22 → 52`): `● TIDAL HI-RES CDN`.

### 5.3. Catálogo y Lista de Pistas: `X: 230 → 1280`, `Y: 75 → 625`
- **Fondo:** `#0A0A0C`.
- **Encabezado de Sección:** `X: 250, Y: 92`. Título dinámico (e.g., `CATÁLOGO: Tendencias` o `BÚSQUEDA: Daft Punk`).
- **Encabezados de Columna:** `Y: 130`.
  - `#`: `X: 255`
  - `TÍTULO`: `X: 310`
  - `ARTISTA`: `X: 680`
  - `ÁLBUM`: `X: 910`
  - `CALIDAD`: `X: 1100`
  - `TIEMPO`: `X: 1210`
- **Filas de Pistas (`Y: 155 → 595`):**
  - Altura por fila: 44px. 10 filas visibles a la vez.
  - **Soporte de Scroll:** `s_scroll_offset` (0 a `track_count - 10`). Al girar la rueda del mouse, se actualiza el offset suavemente.
  - **Estados:**
    - *Hover:* Resaltado `#20232E`.
    - *Seleccionada:* Resaltado `#282C3A`, borde perimetral blanco y barra vertical indicadora `X: 245 → 248`.

### 5.4. Barra Inferior Now Playing: `X: 0 → 1280`, `Y: 625 → 720`
- **Fondo:** `#111216`, Borde superior `#2A2D38` (1px). Altura total: 95px.
- **Sección Izquierda (Track Meta):**
  - Carátula: `X: 20, Y: 635`, tamaño `70x70` px, renderizada mediante `StretchDIBits` a 32 bits nítido.
  - Título: `X: 102, Y: 642`, Segoe UI 15pt bold con elipsis.
  - Artista: `X: 102, Y: 666`, Segoe UI 13pt regular.
  - Álbum / Badge: `X: 102, Y: 686`, Segoe UI 11pt muted.
- **Sección Central (Transporte & Scrubber):**
  - **Botones de Control (`Y: 636`, Centrados):**
    - Shuffle (`X: 520`): Alterna aleatorio, se ilumina en verde `#10B981` al activarse.
    - Previous (`X: 565`): Salta a pista anterior.
    - **Play / Pause (`X: 615 → 655`):** Botón circular prominente de 38x38 px en blanco o azul acento.
    - Next (`X: 675`): Salta a siguiente pista.
    - Repeat (`X: 720`): Cicla Off / All / One con indicador de modo.
  - **Scrubber y Barra de Progreso (`Y: 680 → 690`):**
    - Tiempo actual: `X: 375, Y: 680` (ej: `01:45`).
    - Barra canal: `X: 425 → 835` (410px de ancho), altura 6px. Fondo `#252834`.
    - Barra progreso: Relleno `#FFFFFF` o degradado cian.
    - **Scrubber interactivo:** Círculo deslizante de 12px de diámetro. Al arrastrar o hacer clic en cualquier parte de la barra, calcula `progress = (mouseX - 425) / 410.0f` y aplica seek directo.
    - Tiempo total: `X: 845, Y: 680` (ej: `05:20`).
- **Sección Derecha (Visualizador & Control de Volumen):**
  - **Espectrómetro de 16 bandas:** `X: 930 → 1070`, `Y: 640 → 695`. Barras de 7px de ancho, espaciado de 2px, con degradado de color y caída balística suave.
  - **Control de Volumen:** `X: 1090 → 1250`.
    - Icono `🔊` en `X: 1090, Y: 665`.
    - Barra de volumen: `X: 1115 → 1205` (Ancho: 90px), altura 6px.
    - Relleno de volumen proporcional (0 a 100%).
    - Clic/arrastre interactivo para regular el volumen de hardware vía `platform_audio_set_volume`.
    - Porcentaje: `X: 1215, Y: 663` (ej: `100%`).

---

## 6. Arquitectura de Interacción y Manejo de Eventos Win32

### 6.1. Máquina de Estados y Foco de Entrada
Se definen dos modos de foco sin romper la arquitectura general:
1. **Modo Catálogo / Normal:**
   - Flechas `▲ / ▼` o teclas `W / S`: Navegan la lista con autoscroll si se sale de la ventana visible.
   - `Enter`: Reproduce la pista resaltada.
   - `Espacio`: Alterna Play/Pause.
   - `N / P`: Pista siguiente / anterior.
   - `H`: Alterna Shuffle.
   - `R`: Cicla Repeat.
   - Tecla `/` o clic en la barra superior: Transfiere el foco al campo de búsqueda (`s_search_focused = true`).
2. **Modo Búsqueda Interactiva (`s_search_focused == true`):**
   - El usuario teclea caracteres directamente (`WM_CHAR`).
   - Se procesan `VK_BACK` (borrar carácter), `VK_ESCAPE` (desenfocar/cancelar).
   - `VK_RETURN`: Dispara la búsqueda inmediata a la API de Tidal/Monochrome con la cadena ingresada, actualiza el catálogo, resetea el cursor a 0 y retorna el foco a la lista.

### 6.2. Eventos de Ratón de Alta Fidelidad
- **`WM_MOUSEMOVE`:**
  - Determina si el puntero se encuentra sobre:
    - Barra de búsqueda (`IDC_IBEAM`).
    - Filas de canciones, botones de transporte, scrubber, slider de volumen o sidebar (`IDC_HAND`).
    - Resto de la ventana (`IDC_ARROW`).
  - Guarda la fila en hover (`s_hover_track`) para redibujarla con feedback inmediato.
  - Si se está arrastrando el ratón con botón izquierdo presionado sobre la barra de progreso o volumen, actualiza el valor en tiempo real (scrubbing fluido).
- **`WM_LBUTTONDOWN` / `WM_LBUTTONUP`:**
  - Manejo de clics atómicos en zonas hit-test precisas (Sidebar, Header, Pistas, Transporte, Scrubber, Volumen).
- **`WM_MOUSEWHEEL`:**
  - Extrae `GET_WHEEL_DELTA_WPARAM(wParam)`.
  - Incrementa/decrementa `s_scroll_offset` con límites `[0, max(0, track_count - 10)]`.

---

## 7. Plan de Refactorización Preservando 100% de Paridad con la HAL Compartida

### 7.1. Compatibilidad Estricta con la HAL (`platform.h` y `monochrome.h`)
- La firma de `platform_gfx_render`:
  ```c
  void platform_gfx_render(int state, const struct TrackMetadata *track,
                           int pstate, float progress,
                           const struct TrackMetadata *tracks, int track_count, int cursor,
                           const char *search_query);
  ```
  se preserva intacta.
- En `src/ui.c`, se enriquece la sincronización de búsqueda:
  - Se permite a `platform_pc.c` reportar solicitudes de búsqueda directas o cambios de pista mediante funciones helper o cola de acciones de entrada.
- El código de PS3 (`src/platform_ps3.c`) continuará compilando sin ningún cambio, manteniendo su renderizador RSX y el teclado OSK virtual para DualShock 3, mientras que PC se beneficia del renderizador GDI moderno y su arquitectura responsiva de escritorio.

### 7.2. Tareas de Implementación en Fase 2
1. **Ampliación de recursos GDI en `src/platform_pc.c`:**
   - Nuevos pinceles y plumas para la paleta oficial (Dark Card, Accent Glow, Progress Fill, Hover States).
   - Variables de estado para scroll (`s_scroll_offset`), hover (`s_hover_row`, `s_hover_btn`), foco de búsqueda (`s_search_focused`), y cursor parpadeante.
2. **Reemplazo del layout en `platform_gfx_render`:**
   - Dibujado del Sidebar en `X: 0 → 230`.
   - Dibujado del Header con campo de búsqueda interactivo en `X: 230 → 1280, Y: 0 → 75`.
   - Dibujado del Catálogo con scroll en `Y: 75 → 625`.
   - Dibujado del Now Playing Dock elevado en `Y: 625 → 720` con scrubber circular, controles geométricos, espectrograma reactivo y slider de volumen.
3. **Mapeo completo de `PlatformWndProc`:**
   - Soporte para `WM_MOUSEMOVE`, `WM_MOUSEWHEEL`, `WM_SETCURSOR`, `WM_CHAR` y hit-testing de precisión.
4. **Pruebas de verificación:**
   - Compilación nativa con Zig CC (`build_pc.bat`).
   - Verificación de ausencia de parpadeo (60 FPS estables con `BitBlt`).
   - Prueba interactiva de búsqueda y reproducción.

---

**Conclusión:** Este diseño rescata la esencia minimalista, elegante y audiófila del cliente web oficial de Monochrome, transformando el frontend de PC de una interfaz ASCII rígida a una suite de reproducción de música moderna, intuitiva y fluida.
