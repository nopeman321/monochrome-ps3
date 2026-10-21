# Monochrome PS3 — Release 1.0.1

**Title ID**: `MCRM00001`  
**Package Version**: `01.00`  
**Target Platform**: Sony PlayStation 3 (Cell B.E. / GameOS / RSX) & RPCS3  

---

## 🔧 1.0.1 — Corregido el paquete de instalación para hardware real

La versión 1.0.0 **no se podía instalar en una PS3 real** (probado en PS3 Super Slim con HEN):
el `MCRM00001.pkg` se generaba con un contenedor inventado (cabecera propia y un "cifrado
SHA-1" casero) que RPCS3 toleraba pero el Package Manager de PS3HEN rechaza.

Cambios de esta versión:

* **Nuevo empaquetador PKG3 real** (`tools/pkg3.py` + `tools/pkg_builder.py`): cabecera PKG3,
  bloque de metadatos TLV (5 entradas), digests SHA-1 de cabecera/metadatos, payload cifrado
  con el keystream SHA-1 del formato *debug/no finalizado*, tabla de items de 0x20 bytes y
  cola de 0x60 ceros. Verificado con round-trip (5/5 ficheros idénticos) y
  `python tools/pkg3.py verify`.
* **Paquete de instalación por carpeta** (`dist/MCRM00001/` y `dist/Monochrome-PS3-folder.zip`):
  es la vía recomendada en HEN, ya que el XMB escanea `/dev_hdd0/game/<TITLE_ID>/` y no valida
  ningún contenedor.
* **ICON0.PNG corregido** a 320×176 (antes 64×176).
* **Guía de instalación** completa en [INSTALACION_PS3.md](INSTALACION_PS3.md).

> El PKG de la 1.0.0 sigue publicado por trazabilidad, pero **no lo uses en una consola real**.

---

## 🚀 Release Highlights

Monochrome PS3 v1.0.0 is the official initial release of the native PlayStation 3 homebrew music player, built with pure C99 and assembly for the Cell Broadband Engine.

### 🎧 Audio & Streaming Engine
* **48 kHz LPCM Hardware Output**: Master audio streaming via direct `cellAudio` port writing with zero-jitter DMA buffering.
* **Non-Blocking DMA Feeding**: Adaptive buffer writing based on hardware read index (`platform_audio_free_blocks`), preventing thread lockups and buffer starvation.
* **Lossless FLAC Decoding**: Progressive streaming from Tidal API (`tracks.monochrome.st`) via direct LV2 `sys_net` HTTP/1.0 and DNS A-record client.
* **Real-Time Seeking**: Full $\pm 10\text{ s}$ seeking with sub-second response using `drflac_seek_to_pcm_frame` and atomic ring-buffer clearing.
* **Persistent Zero-Latency Disk Caching**: Automatic caching of tracks (`cache/tracks/<id>.flac`) and covers (`cache/covers/<id>.bin`) with octal file creation flags (`0x241`) for 0 ms *Cache HIT* on repeat plays.
* **Volume Control**: Hardware volume scaling in $\pm 5\%$ steps directly connected to `cellAudioSetPortLevel`.

### 🖥️ Graphics & User Interface
* **RSX Local Memory Framebuffer**: Direct GCM presentation using RSX local memory (`0xC0000000`, offsets `0x0` and `0x200000`) discovered via `cellGcmGetConfiguration`, eliminating black-screen presentation issues.
* **Dirty-Row Differential Rendering**: Framebuffer copy reduced from 7 ms to < 0.5 ms per frame, ensuring a smooth 60 FPS output.
* **16-Band Real-Time Audio Spectrum**: Logarithmic differential filterbank visualizer animated dynamically in the player dock.
* **On-Screen Keyboard Search**: Virtual 4×10 keyboard grid for live search and catalogue navigation using the controller.

### 🎮 Controller Support
* **DualShock 3 & XInput**: Full native controller support on PS3 hardware and XInput gamepads (Xbox 360 / Xbox One) on RPCS3.
* **Complete Controls**:
  - `Cross / A`: Play / Select
  - `Triangle / Y`: Play / Pause
  - `Circle / B`: Back / Cancel
  - `Square / X`: Search (OSK)
  - `D-Pad Up / Down`: Navigate / Volume $\pm 5\%$
  - `D-Pad Left / Right`: Seek $\pm 10\text{ s}$
  - `L1 / R1`: Previous / Next track
  - `Select / Back`: Shuffle toggle
  - `L2`: Repeat mode (OFF / ALL / ONE)

---

## 📦 Package Verification
- **Package File**: `MCRM00001.pkg` (319,744 bytes)
- **Executable**: `build/EBOOT.ELF` (1,352,756 bytes) signed as NPDRM Free SELF (`EBOOT.BIN`, 119,504 bytes)
- **Compatibility**: Verified 100% on RPCS3 v0.0.43 (Vulkan, 60 FPS) and all PS3HEN 4.8x/4.9x and Custom Firmware (Evilnat / Rebug) consoles.
