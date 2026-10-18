---
name: rpcs3-debugger
description: RPCS3 emulator profiling, configuration management, log inspection, virtual HDD deployment, and regression testing for PS3 Homebrew.
---

# RPCS3 Debugger & Emulation Suite

Esta skill describe los estándares y herramientas para desplegar, ejecutar y depurar **Monochrome PS3** en el emulador **RPCS3**.

---

## 1. Estructura de Directorios en RPCS3

El emulador autónomo del proyecto reside en `emulator/rpcs3/`:
- **Binario**: `emulator/rpcs3/rpcs3.exe`
- **Disco Duro Virtual**: `emulator/rpcs3/dev_hdd0/game/MCRM00001/`
  - `USRDIR/EBOOT.BIN`: Ejecutable firmado Free NPDRM SELF.
  - `USRDIR/certs/cacert.pem`: Certificados SSL/TLS para peticiones HTTPS directas desde GameOS.
  - `PARAM.SFO`: Metadatos del juego (Title ID `MCRM00001`, Resolución 720x480).
- **Perfiles de Configuración**:
  - Global: `emulator/rpcs3/config/config.yml`
  - Juego específico: `emulator/rpcs3/config/custom_configs/config_MCRM00001.yml`

---

## 2. Parámetros Críticos de Emulación para Monochrome

El script `tools/configure_rpcs3.py` automatiza la sincronización de estas opciones:

```yaml
Core:
  PPU Decoder: Recompiler (LLVM)
  SPU Decoder: Recompiler (ASMJIT)
  Lower SPU thread priority: false
  SPU loop detection: false

Audio:
  Renderer: Cubeb / XAudio2
  Audio Buffer Duration: 100ms   # Previene tartamudeos o clicks en streams continuos
  Dump to file: false

Video:
  Renderer: Vulkan
  Resolution: 720x480 (480p)     # Optimizado para rendimiento continuo
  Aspect ratio: 16:9
  Framerate Limit: 60

Network:
  Network Status: Connected
  IP / DNS: Auto
```

---

## 3. Comandos de Lanzamiento y Pruebas

- **Ejecutar Monochrome en RPCS3**:
  ```cmd
  run_rpcs3.bat
  ```
  *(Verifica automáticamente si `EBOOT.BIN` existe; si no, invoca `build_homebrew.bat` y arranca RPCS3 cargando el juego directamente).*

- **Inspección de Logs de Error**:
  - En caso de fallo o cuelgue, consultar:
    - `emulator/rpcs3/RPCS3.log`
    - `emulator/rpcs3/TTY.log`
    - `rpcs3_err.txt` (si fue redirigido en modo headless)
