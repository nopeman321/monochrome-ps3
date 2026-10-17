#!/usr/bin/env python3
"""
MONOCHROME PS3 — RPCS3 Real-Time Debugger & Log Streamer
Author: Senior Cell B.E. Systems Engineer
Usage: python tools/debug_rpcs3.py [--duration SECONDS] [--no-build]
"""

import sys
import os
import time
import subprocess
import argparse

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
RPCS3_EXE = os.path.join(PROJECT_DIR, "emulator", "rpcs3", "rpcs3.exe")
TTY_LOG = os.path.join(PROJECT_DIR, "emulator", "rpcs3", "log", "TTY.log")
RPCS3_LOG = os.path.join(PROJECT_DIR, "emulator", "rpcs3", "log", "RPCS3.log")
EBOOT_BIN = os.path.join(PROJECT_DIR, "emulator", "rpcs3", "dev_hdd0", "game", "MCRM00001", "USRDIR", "EBOOT.BIN")

# ANSI color escape sequences
RESET   = "\033[0m"
BOLD    = "\033[1m"
RED     = "\033[31m"
GREEN   = "\033[32m"
YELLOW  = "\033[33m"
BLUE    = "\033[34m"
MAGENTA = "\033[35m"
CYAN    = "\033[36m"
WHITE   = "\033[37m"

def colorize(line):
    if "[PPE]" in line:
        return f"{CYAN}{BOLD}{line}{RESET}"
    elif "[NET]" in line:
        return f"{BLUE}{line}{RESET}"
    elif "[AUDIO]" in line:
        return f"{MAGENTA}{line}{RESET}"
    elif "[RSX]" in line:
        return f"{GREEN}{line}{RESET}"
    elif "[PAD]" in line:
        return f"{YELLOW}{BOLD}{line}{RESET}"
    elif "[NOW PLAYING]" in line:
        return f"{WHITE}{BOLD}{line}{RESET}"
    elif "ERROR" in line or "FATAL" in line or "SIG:" in line or "Access violation" in line:
        return f"{RED}{BOLD}{line}{RESET}"
    return line

def build_project():
    print(f"{CYAN}[BUILD]{RESET} Compiling and packaging Monochrome PS3...")
    build_bat = os.path.join(PROJECT_DIR, "build_homebrew.bat")
    res = subprocess.run([build_bat], cwd=PROJECT_DIR, shell=True, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"{RED}[BUILD ERROR]{RESET}\n{res.stderr}\n{res.stdout}")
        return False
    print(f"{GREEN}[BUILD SUCCESS]{RESET} EBOOT.BIN deployed to RPCS3 virtual HDD.")
    return True

def main():
    parser = argparse.ArgumentParser(description="Monochrome PS3 RPCS3 Live Debugger")
    parser.add_argument("--duration", "-d", type=float, default=10.0, help="Duration to run emulation in seconds (default: 10s)")
    parser.add_argument("--no-build", action="store_true", help="Skip compilation before launch")
    args = parser.parse_args()

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    if not args.no_build:
        if not build_project():
            sys.exit(1)

    if not os.path.exists(RPCS3_EXE):
        print(f"{RED}[ERROR]{RESET} RPCS3 executable not found at: {RPCS3_EXE}")
        sys.exit(1)

    # Truncate old TTY log
    if os.path.exists(TTY_LOG):
        try:
            with open(TTY_LOG, "w", encoding="utf-8") as f:
                f.truncate(0)
        except Exception:
            pass

    print(f"{YELLOW}[RPCS3]{RESET} Launching PlayStation 3 Cell B.E. emulation for {args.duration:.1f}s...")
    print(f"{YELLOW}[RPCS3]{RESET} Target: {EBOOT_BIN}\n" + "-" * 70)

    rel_eboot = "dev_hdd0/game/MCRM00001/USRDIR/EBOOT.BIN"
    proc = subprocess.Popen([RPCS3_EXE, rel_eboot], cwd=os.path.join(PROJECT_DIR, "emulator", "rpcs3"))

    start_time = time.time()
    last_pos = 0

    try:
        while time.time() - start_time < args.duration:
            if proc.poll() is not None:
                print(f"{YELLOW}[RPCS3]{RESET} Process exited early with code {proc.returncode}")
                break

            if os.path.exists(TTY_LOG):
                try:
                    with open(TTY_LOG, "r", encoding="utf-8", errors="replace") as f:
                        f.seek(last_pos)
                        new_lines = f.readlines()
                        last_pos = f.tell()
                        for line in new_lines:
                            line_stripped = line.rstrip()
                            if line_stripped:
                                print(colorize(line_stripped))
                except Exception:
                    pass

            time.sleep(0.05)

    except KeyboardInterrupt:
        print(f"\n{YELLOW}[RPCS3]{RESET} Stopping emulation requested by user...")
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                proc.kill()

        # Flush any remaining logs
        if os.path.exists(TTY_LOG):
            try:
                with open(TTY_LOG, "r", encoding="utf-8", errors="replace") as f:
                    f.seek(last_pos)
                    for line in f.readlines():
                        line_stripped = line.rstrip()
                        if line_stripped:
                            print(colorize(line_stripped))
            except Exception:
                pass

    print("-" * 70)
    print(f"{GREEN}[RPCS3]{RESET} Emulation session finished cleanly.")

if __name__ == "__main__":
    main()
