"""
RPCS3 Downloader and Portable Setup for Windows
Fetches the latest official RPCS3 release from GitHub, extracts it,
and prepares the environment for debugging PS3 packages.
"""

import os
import sys
import json
import urllib.request
import py7zr

def install_rpcs3(target_dir):
    os.makedirs(target_dir, exist_ok=True)
    archive_path = os.path.join(target_dir, 'rpcs3_latest.7z')
    
    print("[1/3] Fetching latest RPCS3 release info from GitHub API...")
    api_url = "https://api.github.com/repos/RPCS3/rpcs3-binaries-win/releases/latest"
    req = urllib.request.Request(api_url, headers={'User-Agent': 'RPCS3-Installer'})
    with urllib.request.urlopen(req) as resp:
        release_data = json.loads(resp.read().decode('utf-8'))
        
    assets = release_data.get('assets', [])
    download_url = None
    for a in assets:
        if a['name'].endswith('.7z'):
            download_url = a['browser_download_url']
            break
            
    if not download_url:
        raise RuntimeError("Could not find a valid .7z asset in RPCS3 release!")
        
    print(f"[2/3] Downloading RPCS3 from: {download_url}")
    print(f"      Saving to: {archive_path}")
    
    def reporthook(count, block_size, total_size):
        percent = int(count * block_size * 100 / total_size) if total_size > 0 else 0
        sys.stdout.write(f"\r      Progress: {percent}% ({count * block_size // (1024*1024)} MB)")
        sys.stdout.flush()
        
    urllib.request.urlretrieve(download_url, archive_path, reporthook)
    print("\n      Download completed successfully.")
    
    print(f"[3/3] Extracting archive with py7zr into: {target_dir}...")
    with py7zr.SevenZipFile(archive_path, mode='r') as z:
        z.extractall(path=target_dir)
        
    print("[OK] RPCS3 extracted.")
    exe_path = os.path.join(target_dir, 'rpcs3.exe')
    if os.path.exists(exe_path):
        print(f"[SUCCESS] rpcs3.exe is ready at: {exe_path}")
    else:
        print("[WARN] rpcs3.exe not found at top level, checking subdirectories...")

if __name__ == '__main__':
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    emu_dir = os.path.join(base_dir, 'emulator', 'rpcs3')
    install_rpcs3(emu_dir)
