#!/usr/bin/env python3
"""
MONOCHROME PS3 — RPCS3 Automated Configuration Tool
Configures RPCS3 settings optimally for Monochrome Music (MCRM00001):
- Low-latency uncompressed LPCM audio buffer (100ms, no sample-skipping, no time-stretching)
- Internet connection enabled for streaming API (sys_net HTTP / TLS)
- RSX Vulkan 720x480 60Hz double-buffered VSync, Bilinear scaling (sin FSR) y
  Write Color Buffers desactivado (framebuffer 2D por software: no hace falta)
- Persistent input polling and DualShock 3 / Keyboard mapping
"""

import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
RPCS3_DIR = os.path.join(PROJECT_DIR, "emulator", "rpcs3")
CONFIG_DIR = os.path.join(RPCS3_DIR, "config")
CUSTOM_CONFIG_DIR = os.path.join(CONFIG_DIR, "custom_configs")

CUSTOM_CONFIG_YAML = """Audio:
  Audio Channel Layout: Stereo
  Audio Device: "@@@default@@@"
  Audio Format: Stereo
  Audio Provider: CellAudio
  Desired Audio Buffer Duration: 100
  Disable Sampling Skip: true
  Enable Buffering: true
  Enable Time Stretching: false
  Master Volume: 100
  Renderer: Cubeb

Core:
  PPU Decoder: Recompiler (LLVM)
  SPU Decoder: Recompiler (LLVM)
  PPU Threads: 2
  Preferred SPU Threads: 0
  SPU Block Size: Safe
  SPU Loop Detection: false
  LLVM Precompilation: true
  Clocks scale: 100
  Hook static functions: false

Input/Output:
  Background input enabled: true
  Keep pads connected: true
  Keyboard: Basic
  Mouse: Basic

Net:
  Internet enabled: Connected
  PSN status: Simulated
  DNS address: 8.8.8.8
  Bind address: 0.0.0.0
  IP address: 0.0.0.0
  UPNP Enabled: false

System:
  Enter button assignment: Enter with cross
  Language: English (US)
  License Area: SCEA

Video:
  Renderer: Vulkan
  Resolution: 720x480
  Aspect ratio: 16:9
  VSync Mode: Full
  Frame limit: 60
  Multithreaded RSX: true
  Write Color Buffers: false
  Read Color Buffers: false
  Strict Rendering Mode: false
  Shader Mode: Async Recompiler (multi-threaded)
  Shader Precision: High
  Use full RGB output range: true
  Output Scaling Mode: Bilinear
"""

def configure():
    print("[RPCS3 CONFIG] Applying optimal profile for Monochrome PS3 (MCRM00001)...")
    
    if not os.path.exists(RPCS3_DIR):
        print(f"[ERROR] RPCS3 directory not found at: {RPCS3_DIR}")
        return False

    os.makedirs(CUSTOM_CONFIG_DIR, exist_ok=True)
    custom_cfg_path = os.path.join(CUSTOM_CONFIG_DIR, "config_MCRM00001.yml")
    with open(custom_cfg_path, "w", encoding="utf-8") as f:
        f.write(CUSTOM_CONFIG_YAML)
    print(f"[OK] Custom game configuration written to: {custom_cfg_path}")

    # Patch global config.yml if present
    global_cfg_path = os.path.join(CONFIG_DIR, "config.yml")
    if os.path.exists(global_cfg_path):
        with open(global_cfg_path, "r", encoding="utf-8") as f:
            content = f.read()

        replacements = [
            ("Desired Audio Buffer Duration: 34", "Desired Audio Buffer Duration: 100"),
            ("Disable Sampling Skip: false", "Disable Sampling Skip: true"),
            ("Keep pads connected: false", "Keep pads connected: true"),
            ('Keyboard: "Null"', 'Keyboard: Basic'),
            ("Internet enabled: Disconnected", "Internet enabled: Connected"),
            ("PSN status: Disconnected", "PSN status: Simulated"),
            ("UPNP Enabled: true", "UPNP Enabled: false"),
            ("Multithreaded RSX: false", "Multithreaded RSX: true"),
            ("VSync Mode: Disabled", "VSync Mode: Full"),
            # Write Color Buffers solo hace falta para juegos que renderizan por
            # RSX; aqui el framebuffer es 2D por software, asi que activarlo solo
            # anade sincronizacion GPU->CPU en cada flip (stutter en GPU integrada).
            ("Write Color Buffers: true", "Write Color Buffers: false"),
            # FSR son pasadas de compute shaders por frame: en una Intel UHD de
            # 15W satura la memoria compartida y provoca tirones. Bilinear es gratis.
            ("Output Scaling Mode: FidelityFX Super Resolution", "Output Scaling Mode: Bilinear"),
        ]

        for old, new in replacements:
            content = content.replace(old, new)

        with open(global_cfg_path, "w", encoding="utf-8") as f:
            f.write(content)
        print(f"[OK] Global configuration updated at: {global_cfg_path}")

    print("[SUCCESS] RPCS3 is configured 100% correctly for Monochrome PS3!")
    return True

if __name__ == "__main__":
    configure()
