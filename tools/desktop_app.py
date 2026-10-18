#!/usr/bin/env python3
"""
MONOCHROME — Desktop Native Web Client Launcher & API Proxy
Serves the web/ directory locally, proxies Tidal/Monochrome API requests,
and opens the dedicated standalone desktop app window.
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

USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"

class ReusableThreadingServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True

class MonochromeHTTPHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEB_DIR, **kwargs)

    def do_GET(self):
        # 1. Handle Favicon
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

        # 2. Local Proxy for Search
        if self.path.startswith('/api/search'):
            query_str = self.path[len('/api/search'):]
            target_url = f"https://tracks.monochrome.st/search{query_str}"
            self.proxy_remote(target_url, default_content_type="application/json")
            return

        # 3. Local Proxy for Track Audio Streams
        if self.path.startswith('/api/track/'):
            track_id = self.path[len('/api/track/'):].split('?')[0]
            target_url = f"https://tracks.monochrome.st/track/{track_id}"
            self.proxy_remote(target_url, default_content_type="audio/flac")
            return

        # 4. Local Proxy for Cover Artwork
        if self.path.startswith('/api/proxy/mi/'):
            img_id = self.path[len('/api/proxy/mi/'):].split('?')[0]
            target_url = f"https://tracks.monochrome.st/proxy/mi/{img_id}"
            self.proxy_remote(target_url, default_content_type="image/jpeg")
            return

        # Standard static file serving
        super().do_GET()

    def proxy_remote(self, url, default_content_type="application/octet-stream"):
        try:
            req = urllib.request.Request(url)
            req.add_header('User-Agent', USER_AGENT)
            if 'Range' in self.headers:
                req.add_header('Range', self.headers['Range'])

            with urllib.request.urlopen(req, timeout=25) as resp:
                code = resp.getcode()
                content_len = resp.getheader('Content-Length')

                if default_content_type in ("application/json", "image/jpeg") or not content_len:
                    data = resp.read()
                    self.send_response(code)
                    self.send_header('Content-Type', default_content_type)
                    self.send_header('Content-Length', str(len(data)))
                    self.send_header('Cache-Control', 'public, max-age=3600')
                    self.end_headers()
                    self.wfile.write(data)
                else:
                    self.send_response(code)
                    for header, val in resp.getheaders():
                        hl = header.lower()
                        if hl in ('content-length', 'content-range', 'accept-ranges', 'cache-control', 'etag', 'last-modified'):
                            self.send_header(header, val)
                    self.send_header('Content-Type', default_content_type)
                    self.end_headers()

                    # Stream large audio data in chunks
                    while True:
                        chunk = resp.read(65536)
                        if not chunk:
                            break
                        self.wfile.write(chunk)
        except Exception as e:
            try:
                self.send_response(502)
                self.send_header('Content-Type', 'text/plain')
                self.end_headers()
                self.wfile.write(f"Proxy error: {e}".encode('utf-8'))
            except Exception:
                pass

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
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
            f"--window-size={win_size}",
            "--disable-features=TranslateUI",
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
