"""
Sony PlayStation 3 Official Firmware (PUP) Downloader
Downloads official PS3UPDAT.PUP v4.91 from Sony CDN into emulator folder.
"""

import urllib.request
import os
import sys

FIRMWARE_URL = "http://dus01.ps3.update.playstation.net/update/ps3/image/us/2024_0227_3694eb3fb8d9915c112e6ab41a60c69f/PS3UPDAT.PUP"

def download_firmware(destination):
    os.makedirs(os.path.dirname(os.path.abspath(destination)), exist_ok=True)
    print(f"[1/2] Connecting to official Sony CDN...")
    print(f"      URL: {FIRMWARE_URL}")
    print(f"      Target: {destination}")
    
    def reporthook(count, block_size, total_size):
        percent = int(count * block_size * 100 / total_size) if total_size > 0 else 0
        mb_downloaded = (count * block_size) // (1024 * 1024)
        total_mb = total_size // (1024 * 1024)
        sys.stdout.write(f"\r      Progress: {percent}% ({mb_downloaded}/{total_mb} MB)")
        sys.stdout.flush()

    urllib.request.urlretrieve(FIRMWARE_URL, destination, reporthook)
    print(f"\n[2/2] Download completed successfully ({os.path.getsize(destination)} bytes).")

if __name__ == '__main__':
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    target = os.path.join(base_dir, 'emulator', 'rpcs3', 'PS3UPDAT.PUP')
    download_firmware(target)
