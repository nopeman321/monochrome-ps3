# Monochrome PS3 — Sony PlayStation 3 Homebrew Port

A native, high-resolution music streaming homebrew client for the **Sony PlayStation 3** (PS3HEN & Custom Firmware), ported from the open-source [monochrome-music/monochrome](https://github.com/monochrome-music/monochrome) project.

---

## 🚀 Key Features

- **Cell Broadband Engine Architecture**:
  - **PPE Core 0**: Main loop running at 60 FPS VSync with DualShock 3 input polling and state machine.
  - **PPE Core 1**: Asynchronous background network daemon streaming lossless FLAC via HTTPS.
  - **SPU DSP Core**: Real-time 44.1 kHz $\to$ 48.0 kHz fractional resampler (ratio 160:147) and audio processing.
- **Microkernel LV2 Integration**:
  - Native `sys_timer_usleep` (`syscall 141`) for efficient PPU thread yielding (< 1% CPU utilization).
  - Native `sys_tty_write` (`syscall 403`) routing console diagnostics to TTY.
- **PS3HEN / CFW Compatibility**:
  - Signed as **Free NPDRM SELF** (`SCE\0`) using SCETool with Sony root keysets.
  - **No `.rap` license files** or PSN account required.
  - Installs cleanly via **Package Manager** on all PS3HEN versions (4.8x / 4.9x) and CFW (Evilnat, Rebug, Cobra).
- **Upstream Alignment**:
  - Integrated with upstream `tracks.monochrome.st` proxy and `api.monochrome.tf` failover cluster.
  - Full lossless 16-bit / 24-bit FLAC audio decoding.

---

## 📁 Repository Structure

```text
├── build_homebrew.bat     # One-click build script (compile, sign, package)
├── link.ld                # GNU/LLD Linker script with strict 64 KB alignment
├── include/
│   ├── monochrome.h       # System architecture and memory map header
│   ├── lv2.h              # GameOS LV2 syscall interface
│   └── cJSON.h            # Lightweight JSON parser header
├── src/
│   ├── main.c             # Application lifecycle & main loop
│   ├── ui.c               # RSX graphics rendering & DualShock 3 input
│   ├── audio.c            # Audio engine, lock-free ring buffer, SPU resampler
│   ├── network.c          # Upstream API client & TLS stream fetcher
│   ├── lv2.c              # GameOS LV2 inline assembly & libc hooks
│   └── cJSON.c            # cJSON implementation
├── tools/
│   ├── pkg_builder.py     # Sony PKG packaging utility
│   ├── sfo_builder.py     # PARAM.SFO generator
│   └── scetool/           # SCETool signing suite
└── pkg_build/             # PKG source staging directory
    ├── PARAM.SFO          # Title ID MCRM00001, Category HG
    ├── ICON0.PNG          # 320x176 XMB Icon
    ├── PIC1.PNG           # 1920x1080 XMB Wallpaper
    └── USRDIR/
        └── certs/         # Root CA certificate bundle
```

---

## 🛠️ Building the Homebrew

### Prerequisites
- Windows 10/11 (or Linux via cross-compilation)
- Python 3.8+

### Build in 1-Click
Execute the bundled batch script:
```cmd
build_homebrew.bat
```
This performs:
1. Cross-compilation to 64-bit Big-Endian PowerPC (`powerpc64-linux-musl`) with 64 KB page alignment.
2. Cryptographic signing with SCETool into a genuine `SCE\0` Free NPDRM SELF.
3. Packaging into `MCRM00001.pkg` ready for PS3 installation.

---

## 🎮 Installation on PlayStation 3 (HEN / CFW)

1. Format a USB drive as **FAT32** (MBR partition scheme).
2. Copy `MCRM00001.pkg` to the root of the USB drive.
3. Plug the USB drive into the rightmost USB port of your PS3.
4. If on HEN: Run **Enable HEN** from the XMB.
5. Navigate to **Game** $\to$ **Package Manager** $\to$ **Install Package Files** $\to$ **Standard**.
6. Select and install **Monochrome Music**.
7. Launch the app directly from your XMB Game list!

---

## ⚖️ License & Attribution

- Ported from [monochrome-music/monochrome](https://github.com/monochrome-music/monochrome) (licensed under MIT).
- PS3 Homebrew architecture and Cell B.E. implementation released under the MIT License.
