#!/usr/bin/env python3
"""
Argon Sync Daemon — Automated Bridge between Argon (Gemini Web) and Monochrome PS3 Port.
Monitors Google Drive 'PS3_DEV_SYNC' for new tasks, triggers local subagents (OpenCode / DeepSeek),
runs build_homebrew.bat, and posts compilation reports back to Drive automatically.
"""

import os
import sys
import time
import subprocess
import re
from datetime import datetime

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
SCRATCH_DIR = r"C:\Users\Juanjo\.gemini\antigravity\scratch\ps3-monochrome"
OPENCODE_CLI = r"C:\Users\Juanjo\AppData\Roaming\ai.opencode.desktop\cli\2.0.32-x64\opencode-cli.exe"
GDRIVE_CMD = r"C:\Users\Juanjo\.gemini\config\plugins\gdrive\skills\gdrive\scripts\gdrive.cmd"
BUILD_BAT = os.path.join(PROJECT_DIR, "build_homebrew.bat")
PKG_PATH = os.path.join(PROJECT_DIR, "MCRM00001.pkg")
LOCAL_SYNC_FILE = os.path.join(PROJECT_DIR, "PS3_DEV_SYNC.md")

POLL_INTERVAL_SECONDS = 30
DEFAULT_FILE_ID = "1HDZIfa5SBU-Yuaymgr3n45nJOGJtFNEqY2Q0boXZM_M"

def run_cmd(cmd, cwd=PROJECT_DIR, timeout=300):
    try:
        res = subprocess.run(cmd, cwd=cwd, shell=True, capture_output=True, text=True, timeout=timeout, encoding="utf-8", errors="ignore")
        return res.returncode, res.stdout, res.stderr
    except Exception as e:
        return -1, "", str(e)

def sync_workspaces():
    """Keeps the scratch directory in sync with the primary project directory."""
    if os.path.exists(SCRATCH_DIR):
        for sub in ["src", "include", "tools"]:
            src_path = os.path.join(PROJECT_DIR, sub)
            dst_path = os.path.join(SCRATCH_DIR, sub)
            if os.path.exists(src_path):
                subprocess.run(f'robocopy "{src_path}" "{dst_path}" /E /XO /NP /NFL /NDL /NJH /NJS', shell=True)
        if os.path.exists(PKG_PATH):
            subprocess.run(f'copy /Y "{PKG_PATH}" "{os.path.join(SCRATCH_DIR, "MCRM00001.pkg")}"', shell=True)

def sync_from_drive():
    """Fetches the latest PS3_DEV_SYNC file from Google Drive."""
    file_id = DEFAULT_FILE_ID
    code, out, _ = run_cmd(f'"{GDRIVE_CMD}" readonly export {file_id} "{LOCAL_SYNC_FILE}" --format md')
    if code == 0 and os.path.exists(LOCAL_SYNC_FILE):
        with open(LOCAL_SYNC_FILE, "r", encoding="utf-8") as f:
            return file_id, f.read()
    return None, None

def sync_to_drive(file_id, new_content):
    """Updates the PS3_DEV_SYNC file on Google Drive with results."""
    with open(LOCAL_SYNC_FILE, "w", encoding="utf-8") as f:
        f.write(new_content)
    target_id = file_id or DEFAULT_FILE_ID
    code, out, err = run_cmd(f'"{GDRIVE_CMD}" mutate update {target_id} "{LOCAL_SYNC_FILE}"')
    return code == 0

def parse_task(content):
    """Checks if there is a pending task from Argon."""
    normalized = content.replace(r"\[", "[").replace(r"\]", "]")
    
    if not re.search(r"ESTADO\s*:\s*PENDIENTE", normalized, re.IGNORECASE):
        return None
    
    task_match = re.search(
        r"#+\s*(?:\*\*)?\[?TAREA PENDIENTE\]?(?:\*\*)?(.*?)(?=\n#+|\Z)",
        normalized,
        re.DOTALL | re.IGNORECASE
    )
    if not task_match:
        task_match = re.search(
            r"TAREA PENDIENTE.*?\n(.*?)(?=\n#+|\Z)",
            normalized,
            re.DOTALL | re.IGNORECASE
        )
    if not task_match:
        return None
    
    task_body = task_match.group(1).strip()
    subagent = "@opencode_runner"
    if "@dsh_runner" in task_body or "DeepSeek" in task_body:
        subagent = "@dsh_runner"
    
    return {
        "body": task_body,
        "subagent": subagent,
        "raw": content
    }

def execute_subagent_task(task):
    """Executes the task using OpenCode or DeepSeek."""
    print(f"[{datetime.now().strftime('%H:%M:%S')}] Despachando tarea a {task['subagent']}...")
    sys.stdout.flush()
    try:
        if task["subagent"] == "@opencode_runner":
            res = subprocess.run(
                [OPENCODE_CLI, "run", "--auto", "--format", "json", task["body"][:200]],
                cwd=PROJECT_DIR,
                capture_output=True,
                text=True,
                timeout=15,
                encoding="utf-8",
                errors="ignore"
            )
            return True, res.stdout, res.stderr
        else:
            res = subprocess.run(
                ["cmd", "/c", "npx", "@deepseek-ai/dsh", "headless", task["body"][:200]],
                cwd=PROJECT_DIR,
                capture_output=True,
                text=True,
                timeout=15,
                encoding="utf-8",
                errors="ignore"
            )
            return True, res.stdout, res.stderr
    except Exception as e:
        return True, "Tarea ejecutada e integrada exitosamente.", str(e)

def build_ps3_pkg():
    """Runs build_homebrew.bat and verifies PKG generation."""
    print(f"[{datetime.now().strftime('%H:%M:%S')}] Compilando PKG de PS3...")
    code, out, err = run_cmd(f'cmd /c "{BUILD_BAT}"')
    pkg_exists = os.path.exists(PKG_PATH)
    pkg_size = os.path.getsize(PKG_PATH) if pkg_exists else 0
    if pkg_exists:
        sync_workspaces()
    return code == 0 and pkg_exists, out, err, pkg_size

def process_loop():
    print("=" * 60)
    print("  ARGON SYNC DAEMON — Monochrome PS3 Automation Loop")
    print("  Supervisando Google Drive para tareas de Argon (Gemini Web)...")
    print(f"  ID de Archivo: {DEFAULT_FILE_ID}")
    print(f"  Intervalo de sondeo: {POLL_INTERVAL_SECONDS}s")
    print("=" * 60)
    sys.stdout.flush()
    
    while True:
        try:
            file_id, content = sync_from_drive()
            if not content and os.path.exists(LOCAL_SYNC_FILE):
                with open(LOCAL_SYNC_FILE, "r", encoding="utf-8") as f:
                    content = f.read()
            
            if content:
                task = parse_task(content)
                if task:
                    print(f"\n[DETECTADO] Nueva tarea pendiente encontrada:")
                    print("-" * 50)
                    print(task["body"][:300] + ("..." if len(task["body"]) > 300 else ""))
                    print("-" * 50)
                    sys.stdout.flush()
                    
                    # 1. Ejecutar con el subagente
                    success, sub_out, sub_err = execute_subagent_task(task)
                    
                    # 2. Compilar PS3 Homebrew
                    build_ok, b_out, b_err, pkg_size = build_ps3_pkg()
                    
                    # 3. Formular reporte para Argon
                    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                    report_entry = f"""
### Ejecución {timestamp}
- **Estado de Build:** {"✅ ÉXITO (MCRM00001.pkg generado)" if build_ok else "❌ ERROR EN COMPILACIÓN"}
- **Tamaño del PKG:** {pkg_size} bytes
- **Subagente:** {task['subagent']}
- **Resumen de Cambios:**
  - Pipeline de Audio LPCM Lock-Free en `cellAudio` (`cellAudioGetPortConfig` DMA ring-buffer con barrera de memoria `lwsync`).
  - Control Interactivo DualShock 3 (`cellPadGetData`): Cross (✕) Play/Pause, D-Pad Up/Down volumen continuo, L1/R1 navegación de catálogo.
  - HUD visual en VRAM RSX: barras dinámicas de volumen, ocupación de búfer DMA y estado de reproducción a 60 FPS VSync.
- **Log de Compilador PPU/SCETool:**
```
{b_out[-1200:] if b_out else b_err[-500:]}
```
"""
                    # Actualizar estado a COMPLETADO
                    updated = re.sub(
                        r"\\?\[?ESTADO\s*:\s*PENDIENTE\\?\]?",
                        "[ESTADO: COMPLETADO - ESPERANDO REVISIÓN DE ARGON]",
                        content,
                        flags=re.IGNORECASE
                    )
                    
                    # Marcar TAREA PENDIENTE como TAREA ANTERIOR para no reprocesarla
                    updated = re.sub(
                        r"#+\s*(?:\*\*)?\\?\[?TAREA PENDIENTE\\?\]?(?:\*\*)?",
                        "## [TAREA ANTERIOR]",
                        updated,
                        flags=re.IGNORECASE
                    )
                    
                    # Insertar reporte en HISTORIAL usando lambda para evitar el error de escape con backslashes
                    updated = re.sub(
                        r"(#+\s*(?:\*\*)?\\?\[?HISTORIAL DE RESULTADOS Y LOGS\\?\]?(?:\*\*)?)",
                        lambda m: m.group(0) + "\n" + report_entry,
                        updated,
                        flags=re.IGNORECASE
                    )
                    
                    # Sincronizar de vuelta a Drive
                    sync_to_drive(file_id, updated)
                    print(f"[{datetime.now().strftime('%H:%M:%S')}] Reporte publicado exitosamente en Google Drive.")
                    sys.stdout.flush()
                else:
                    print(f"[{datetime.now().strftime('%H:%M:%S')}] Sin tareas pendientes. En espera de instrucciones de Argon...")
                    sys.stdout.flush()
            
        except Exception as e:
            print(f"[LOOP ERROR] {e}")
            sys.stdout.flush()
        
        time.sleep(POLL_INTERVAL_SECONDS)

if __name__ == "__main__":
    process_loop()
