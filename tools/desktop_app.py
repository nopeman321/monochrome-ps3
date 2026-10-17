#!/usr/bin/env python3
"""
MONOCHROME — Desktop Native Web Client Launcher
Serves the web/ directory locally and opens it in dedicated standalone app mode.
"""

import os
import sys
import time
import socket
import threading
import subprocess
import urllib.request
import urllib.error
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
WEB_DIR = os.path.join(PROJECT_DIR, "web")
PORT = 7890

class MonochromeHTTPHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEB_DIR, **kwargs)

    def end_headers(self):
        # Enable CORS and caching headers
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.end_headers()

def is_port_in_use(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        return s.connect_ex(('127.0.0.1', port)) == 0

def find_edge_path():
    candidates = [
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
        os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\Edge\Application\msedge.exe")
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    return None

def start_server():
    server = ThreadingHTTPServer(('127.0.0.1', PORT), MonochromeHTTPHandler)
    print(f"[SERVER] Monochrome local daemon running on http://127.0.0.1:{PORT}")
    server.serve_forever()

def main():
    print("=" * 60)
    print("  MONOCHROME · Native Desktop Music Client (Tidal Hi-Res)")
    print("=" * 60)

    # 1. Start server in background thread if not already running
    if not is_port_in_use(PORT):
        t = threading.Thread(target=start_server, daemon=True)
        t.start()
        time.sleep(0.5)
    else:
        print(f"[SERVER] Daemon already listening on port {PORT}")

    # 2. Launch Dedicated Desktop Window
    edge_exe = find_edge_path()
    app_url = f"http://127.0.0.1:{PORT}"
    user_data_dir = os.path.join(PROJECT_DIR, "bin", "edge_user_data")

    if edge_exe:
        print(f"[APP] Launching Monochrome Dedicated Desktop Window...")
        cmd = [
            edge_exe,
            f"--app={app_url}",
            f"--user-data-dir={user_data_dir}",
            "--window-size=1280,820",
            "--disable-features=TranslateUI",
            "--enable-features=WebAssembly",
            "--autoplay-policy=no-user-gesture-required"
        ]
        proc = subprocess.Popen(cmd)
        print(f"[APP] Monochrome active (PID: {proc.pid}). Press Ctrl+C in console to stop.")
        try:
            proc.wait()
        except KeyboardInterrupt:
            proc.terminate()
    else:
        print(f"[APP] Opening default browser at {app_url}...")
        import webbrowser
        webbrowser.open(app_url)
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass

if __name__ == '__main__':
    main()
