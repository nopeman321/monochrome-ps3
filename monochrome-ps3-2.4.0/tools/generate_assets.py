"""
PS3 XMB Asset Generator for Monochrome
Converts Monochrome branding assets into exact Sony PS3 XMB specifications:
- ICON0.PNG: 320x176 (XMB icon)
- PIC1.PNG: 1920x1080 (XMB wallpaper background)
"""

from PIL import Image, ImageOps
import os

Image.MAX_IMAGE_PIXELS = None

def generate_xmb_assets(upstream_dir, output_dir):
    os.makedirs(output_dir, exist_ok=True)
    
    # 1. ICON0.PNG (320x176)
    icon_src = os.path.join(upstream_dir, 'assets', 'icon-only.png')
    if not os.path.exists(icon_src):
        icon_src = os.path.join(upstream_dir, 'assets', 'icon-foreground.png')
        
    icon_dst = os.path.join(output_dir, 'ICON0.PNG')
    if os.path.exists(icon_src):
        img = Image.open(icon_src).convert('RGBA')
        # Create 320x176 canvas (black/transparent background)
        canvas = Image.new('RGBA', (320, 176), (0, 0, 0, 0))
        # Resize maintaining aspect ratio to fit inside 150x150
        img.thumbnail((150, 150), Image.Resampling.LANCZOS)
        # Center in 320x176
        x = (320 - img.width) // 2
        y = (176 - img.height) // 2
        canvas.paste(img, (x, y), img)
        canvas.save(icon_dst, 'PNG')
        print(f"Generated {icon_dst} (320x176)")
        
    # 2. PIC1.PNG (1920x1080)
    splash_src = os.path.join(upstream_dir, 'assets', 'splash-dark.png')
    if not os.path.exists(splash_src):
        splash_src = os.path.join(upstream_dir, 'assets', 'splash.png')
        
    pic1_dst = os.path.join(output_dir, 'PIC1.PNG')
    if os.path.exists(splash_src):
        img = Image.open(splash_src).convert('RGB')
        # Resize/fit to 1920x1080
        img_resized = ImageOps.fit(img, (1920, 1080), Image.Resampling.LANCZOS)
        img_resized.save(pic1_dst, 'PNG')
        print(f"Generated {pic1_dst} (1920x1080)")

if __name__ == '__main__':
    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    upstream = os.path.join(base, 'upstream')
    pkg_build = os.path.join(base, 'pkg_build')
    generate_xmb_assets(upstream, pkg_build)
