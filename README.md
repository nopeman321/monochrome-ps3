# Monochrome PS3 — Official Release v2.3.0

Client homebrew nativo de streaming y reproducción de música de alta fidelidad para **Sony PlayStation 3** (Cell Broadband Engine + RSX Reality Synthesizer), compilado sobre la cadena estándar de código abierto **PSL1GHT v2** (`ps3dev`).

---

## 🌟 Características Principales (100% Funcional y Verificado en Hardware Real)

- **Streaming Hi-Fi FLAC Lossless Directo**: Conexión directa a la API de `tracks.monochrome.st` vía HTTP con buffering progresivo inteligente.
- **Audio LPCM Float32 a 48 kHz**: Motor de audio en tiempo real sincronizado por DMA de GameOS (`cellAudio`). Hilo dedicado con prioridad real-time 100 y cero underruns (`Hardware DMA Underruns = 0`). Resampling lineal exacto 147:160 para pistas de 44.1 kHz.
- **Gráficos 1080p RSX Double-Buffer**: Doble buffer en VRAM con sincronización vertical (VSYNC), renderizado con tipografía nítida y analizador de espectro de audio reactivo en tiempo real.
- **Ajuste Fino de Pantalla y Fuente**: Calibración precisa milimétrica de márgenes (D-Pad paso 1px), escala de pantalla (L2/R2 paso fino 0.005) y escala de fuente (L1/R1). Persistencia automática en disco (`/dev_hdd0/tmp/monochrome/screen_config.txt`).
- **Estética Cyber-Minimalista Metalheart**: Pantalla de presentación de inicio con banner ASCII y fondo integrado de marcas técnicas tenues sobre negro puro (`0x00000000`).
- **Integración Completa con el XMB**: Icono oficial `ICON0.PNG` (320×176) y fondo panorámico oficial oscuro `PIC1.PNG` (1920×1080) con óptimo contraste para los textos del sistema.
- **Búsqueda Online con Teclado en Pantalla (OSK)**: Búsqueda dinámica en tiempo real del catálogo musical con selección y navegación por cuadrícula.
- **Carátulas de Álbum en Alta Resolución**: Descarga asíncrona de carátulas (640×640) y decodificación JPEG nativa optimizada.
- **Caché Persistente en Disco**: Almacenamiento automático en `/dev_hdd0/tmp/monochrome/tracks/` para reproducción instantánea con 0 ms de latencia y cero tráfico de red.

---

## 🎮 Controles DualShock 3

| Acción | Botón |
|---|---|
| **Navegar / Mover** | D-Pad Arriba / Abajo / Izquierda / Derecha |
| **Reproducir / Seleccionar pista** | Cruz (✕) |
| **Volver / Salir de menú** | Círculo (○) |
| **Abrir Búsqueda (OSK)** | Cuadrado (□) |
| **Ver Cola de Reproducción** | Triángulo (△) |
| **Pista Anterior / Siguiente** | L1 / R1 |
| **Menú de Ajustes / Opciones** | START |
| **Ajuste de Escala (en Ajustes)** | L2 (reducir) / R2 (aumentar) |
| **Ajuste de Fuente (en Ajustes)** | L1 (reducir) / R1 (aumentar) |
| **Guardar Configuración Pantalla** | START |
| **Restablecer Configuración** | SELECT |

---

## 🖥️ Verificación en Hardware

- **Consola objetivo:** PlayStation 3 Super Slim (CECH-40xx) / Slim / FAT
- **Firmware:** OFW/HFW 4.88 - 4.92+ CEX
- **Compatibilidad:** PS3HEN v3.x y Custom Firmware (CFW Evilnat / Cobra)
- **Formato de Entrega:** PKG Free NPDRM (`MONO00024.pkg`) sin necesidad de licencias `.rap` ni activación de PSN.

---

## 🛠️ Instrucciones de Compilación

Se utiliza el contenedor oficial de la toolchain PSL1GHT v2 (`ps3dev`):

```bash
docker run --rm -v "${PWD}:/src" ghcr.io/altps3/ps3dev:latest bash -c "cd /src && make clean && make pkg"
```

El paquete resultante se genera en `src.gnpdrm.pkg`.

---

## 📦 Instalación en la Consola

1. Copiar el archivo `.pkg` a `/dev_hdd0/packages/` mediante FTP (ej. usando webMAN MOD o multiman).
2. Instalar desde el menú **Package Manager** en el XMB (o mediante la llamada HTTP de webMAN: `http://<IP_PS3>/install.ps3/dev_hdd0/packages/Monochrome-PS3-v2.3.0.pkg`).
3. Iniciar **Monochrome** directamente desde la columna Juegos del XMB.
