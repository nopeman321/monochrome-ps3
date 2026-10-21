# Guía del Proyecto: Monochrome PS3

**Ubicación de esta carpeta:** `C:\Users\Juanjo\Documents\monochrome-ps3`

Esta carpeta contiene el proyecto completo, autosuficiente e independiente de Monochrome Music para PlayStation 3 (Cell B.E. / LV2 / RSX) y su entorno de emulación en RPCS3.

---

## 📁 Estructura del Proyecto

- **`build_homebrew.bat`**: Script para compilar el código fuente en PowerPC64, parchear las tablas de importación dinámica de SCE (`cellGcmSys`, `cellAudio`, `cellPad`), firmar con SCETool y empaquetar el PKG instalable.
- **`run_rpcs3.bat`**: Lanzador directo del emulador RPCS3 con el juego cargado en primer plano.
- **`src/` e `include/`**: Código fuente C y ensamblador PowerPC (`platform_ps3.c`, `http_ps3.c`, `lv2.c`, `audio.c`, `network.c`, `ui.c`, etc.).
- **`tools/`**: Cadena de compilación completa:
  - Compilador cruzado Zig (`powerpc64-linux-musl`).
  - SCETool con claves raíz de Sony para firma NPDRM.
  - Script de importaciones dinámicas (`tools/sce_imports.py`).
  - Empaquetador de paquetes PKG (`tools/pkg_builder.py`).
  - Scripts de verificación y diagnóstico (`tools/verify_runtime.ps1`, `tools/configure_rpcs3.py`).
- **`emulator/rpcs3/`**: Emulador RPCS3 completamente preconfigurado:
  - Renderizador Vulkan a 720x480 con sincronización vertical (VSync Full) y escalado Bilinear optimizado para gráficas Intel UHD.
  - VFS `/dev_hdd0` con el juego desplegado en `/dev_hdd0/game/MCRM00001/USRDIR/EBOOT.BIN`.
  - Mapeo de mando Xbox 360 configurado vía XInput en `config/input_configs/global/Default.yml`.
- **`MCRM00001.pkg`**: Paquete debug listo para instalar en una consola PS3 real con PS3HEN o Custom Firmware.
- **`build/`**: Binarios intermedios (`EBOOT.ELF`).
- **`evidence/`**: Informes de rendimiento, diagnósticos de stuttering, auditorías estáticas y capturas de pantalla de la interfaz en ejecución.
- **`restore_point_backup/`**: Copia de seguridad autónoma en formato Git Bundle (`.bundle`), binarios y script de restauración de emergencia (`restore.ps1`).

---

## 🎮 Controles

### Mando Xbox 360 (XInput - Jugador 1)
| Botón Xbox | Botón PS3 | Función en Monochrome |
|---|---|---|
| **A** | Cruz ($\times$) | Reproducir / Seleccionar pista o letra en teclado |
| **B** | Círculo ($\bigcirc$) | Cancelar / Volver |
| **X** | Cuadrado ($\square$) | Abrir teclado en pantalla (búsqueda OSK) |
| **Y** | Triángulo ($\triangle$) | Pausa / Reanudar reproducción |
| **D-Pad ↑ / ↓** | D-Pad ↑ / ↓ | Navegar lista · En reproducción: Volumen $\pm 5\%$ |
| **D-Pad ← / →** | D-Pad ← / → | Seek instantáneo $\pm 10\text{ s}$ en la pista |
| **LB / RB** | L1 / R1 | Pista anterior / Pista siguiente |
| **Back** | Select | Alternar modo Shuffle (aleatorio) |
| **LT** | L2 | Ciclar modo Repeat (OFF / ALL / ONE) |
| **RT / RS** | R2 / R3 | Control de volumen secundario |
| **Start** | Start | Salir de la aplicación |
| **Botón Guía** | Botón PS | Menú del sistema |

### Teclado (modo alternativo)
- **Flechas**: D-Pad (Navegar / Volumen / Seek)
- **X**: Play / Seleccionar
- **V**: Pausa / Reanudar
- **Q / E**: Pista anterior / siguiente
- **Z**: Búsqueda
- **Espacio**: Shuffle
- **R**: Repeat
- *(Nota: Enter activa Start/Salir)*

---

## 🛠️ Cómo Compilar y Ejecutar

1. **Compilar**:
   Ejecuta `build_homebrew.bat` haciendo doble clic o desde la terminal:
   ```cmd
   .\build_homebrew.bat
   ```
2. **Ejecutar en PC (RPCS3)**:
   Haz doble clic en `run_rpcs3.bat`.
3. **Instalar en PS3 física**:
   Copia `MCRM00001.pkg` a la raíz de un pendrive USB formateado en FAT32 e instálalo desde *Package Manager* en tu PS3 con HEN o CFW activo.
