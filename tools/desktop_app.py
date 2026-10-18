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

# Ensure stdout and stderr are available even when launched via pythonw or CREATE_NO_WINDOW
if sys.stdout is None or not hasattr(sys.stdout, 'write'):
    sys.stdout = open(os.devnull, 'w', encoding='utf-8')
if sys.stderr is None or not hasattr(sys.stderr, 'write'):
    sys.stderr = open(os.devnull, 'w', encoding='utf-8')

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
WEB_DIR = os.path.join(PROJECT_DIR, "web")
PORT = 7890

class ReusableThreadingServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True

class MonochromeHTTPHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEB_DIR, **kwargs)

    def do_GET(self):
        # Handle favicon without 404
        if self.path == '/favicon.ico':
            icon_path = os.path.join(PROJECT_DIR, "pkg_build", "ICON0.PNG")
            if os.path.exists(icon_path):
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Cache-Control", "public, max-age=86400")
                self.end_headers()
                with open(icon_path, "rb") as f:
                    self.wfile.write(f.read())
                return
            else:
                self.send_response(204)
                self.end_headers()
                return
        super().do_GET()

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.end_headers()

    def log_message(self, format, *args):
        # Quiet logger when running in background
        try:
            super().log_message(format, *args)
        except Exception:
            pass

def is_port_in_use(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.5)
        return s.connect_ex(('127.0.0.1', port)) == 0

def find_browser_app_mode():
    candidates = [
        # Microsoft Edge
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
        os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\Edge\Application\msedge.exe"),
        # Google Chrome
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
        os.path.expandvars(r"%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe"),
        # Brave Browser
        r"C:\Program Files\BraveSoftware\Brave-Browser\Application\brave.exe",
        os.path.expandvars(r"%LOCALAPPDATA%\BraveSoftware\Brave-Browser\Application\brave.exe"),
    ]
    for c in candidates:
        if c and os.path.exists(c):
            return c
    return None

def start_server():
    server = ReusableThreadingServer(('127.0.0.1', PORT), MonochromeHTTPHandler)
    try:
        print(f"[SERVER] Monochrome local daemon running on http://127.0.0.1:{PORT}")
    except Exception:
        pass
    server.serve_forever()

def main():
    try:
        print("=" * 60)
        print("  MONOCHROME · Native Desktop Music Client (Tidal Hi-Res)")
        print("=" * 60)
    except Exception:
        pass

    # 1. Start server in background thread if not already running
    if not is_port_in_use(PORT):
        t = threading.Thread(target=start_server, daemon=True)
        t.start()
        time.sleep(0.4)
    else:
        try:
            print(f"[SERVER] Daemon already listening on port {PORT}")
        except Exception:
            pass

    # 2. Launch Dedicated Desktop Window
    browser_exe = find_browser_app_mode()
    app_url = f"http://127.0.0.1:{PORT}"
    user_data_dir = os.path.join(PROJECT_DIR, "bin", "browser_user_data")

    if browser_exe:
        try:
            print(f"[APP] Launching Monochrome Dedicated Desktop Window via {os.path.basename(browser_exe)}...")
        except Exception:
            pass
        # Resolution: minimum compact desktop window (800x500 default)
        win_size = "800,500"
        for arg in sys.argv[1:]:
            if "," in arg or "x" in arg:
                win_size = arg.replace("x", ",")
                break
        if "MONOCHROME_WINDOW_SIZE" in os.environ:
            win_size = os.environ["MONOCHROME_WINDOW_SIZE"].replace("x", ",")

        cmd = [
            browser_exe,
            f"--app={app_url}",
            f"--user-data-dir={user_data_dir}",
            f"--window-size={win_size}",
            "--disable-features=TranslateUI",
            "--enable-features=WebAssembly",
            "--autoplay-policy=no-user-gesture-required"
        ]
        proc = subprocess.Popen(cmd)
        try:
            print(f"[APP] Monochrome active (PID: {proc.pid}).")
        except Exception:
            pass
        try:
            proc.wait()
        except KeyboardInterrupt:
            proc.terminate()
    else:
        try:
            print(f"[APP] Opening default browser at {app_url}...")
        except Exception:
            pass
        import webbrowser
        webbrowser.open(app_url)
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass

if __name__ == '__main__':
    main()
