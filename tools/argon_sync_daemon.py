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
OPENCODE_CLI = r"C:\Users\Juanjo\AppData\Roaming\ai.opencode.desktop\cli\2.0.32-x64\opencode-cli.exe"
GDRIVE_CMD = r"C:\Users\Juanjo\.gemini\config\plugins\gdrive\skills\gdrive\scripts\gdrive.cmd"
BUILD_BAT = os.path.join(PROJECT_DIR, "build_homebrew.bat")
PKG_PATH = os.path.join(PROJECT_DIR, "MCRM00001.pkg")
LOCAL_SYNC_FILE = os.path.join(PROJECT_DIR, "PS3_DEV_SYNC.md")

POLL_INTERVAL_SECONDS = 30

def run_cmd(cmd, cwd=PROJECT_DIR, timeout=300):
    try:
        res = subprocess.run(cmd, cwd=cwd, shell=True, capture_output=True, text=True, timeout=timeout, encoding="utf-8", errors="ignore")
        return res.returncode, res.stdout, res.stderr
    except Exception as e:
        return -1, "", str(e)

DEFAULT_FILE_ID = "1HDZIfa5SBU-Yuaymgr3n45nJOGJtFNEqY2Q0boXZM_M"

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
    if "[ESTADO: PENDIENTE]" not in content:
        return None
    
    task_match = re.search(r"##\s*\[TAREA PENDIENTE\](.*?)(##|\Z)", content, re.DOTALL)
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
    if task["subagent"] == "@opencode_runner":
        cmd = f'"{OPENCODE_CLI}" run --continue --auto "{task["body"]}"'
    else:
        cmd = f'npx.cmd @deepseek-ai/dsh headless "{task["body"]}"'
    
    code, out, err = run_cmd(cmd)
    return code == 0, out, err

def build_ps3_pkg():
    """Runs build_homebrew.bat and verifies PKG generation."""
    print(f"[{datetime.now().strftime('%H:%M:%S')}] Compilando PKG de PS3...")
    code, out, err = run_cmd(f'cmd /c "{BUILD_BAT}"')
    pkg_exists = os.path.exists(PKG_PATH)
    pkg_size = os.path.getsize(PKG_PATH) if pkg_exists else 0
    return code == 0 and pkg_exists, out, err, pkg_size

def process_loop():
    print("=" * 60)
    print("  ARGON SYNC DAEMON — Monochrome PS3 Automation Loop")
    print("  Supervisando Google Drive para tareas de Argon (Gemini Web)...")
    print("=" * 60)
    
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
                    print(task["body"][:300] + "...")
                    print("-" * 50)
                    
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
{sub_out[-1000:] if sub_out else sub_err[-500:]}
- **Log de Compilador PPU/SCETool:**
```
{b_out[-1200:] if b_out else b_err[-500:]}
```
"""
                    # Actualizar estado a COMPLETADO
                    updated = content.replace("[ESTADO: PENDIENTE]", "[ESTADO: COMPLETADO - ESPERANDO REVISIÓN DE ARGON]")
                    updated = re.sub(
                        r"(##\s*\[HISTORIAL DE RESULTADOS Y LOGS\])",
                        r"\1\n" + report_entry,
                        updated
                    )
                    
                    # Sincronizar de vuelta a Drive
                    sync_to_drive(file_id, updated)
                    print(f"[{datetime.now().strftime('%H:%M:%S')}] Reporte publicado exitosamente en Google Drive.")
            
        except Exception as e:
            print(f"[LOOP ERROR] {e}")
        
        time.sleep(POLL_INTERVAL_SECONDS)

if __name__ == "__main__":
    process_loop()
