#!/usr/bin/env python3
"""
Monochrome PS3 - UI Asset Packer (Phase 1)
-------------------------------------------
Script offline (Python estandar, sin dependencias externas) que prepara assets
1-bit (blanco puro + canal alpha) para tinte por GPU en Tiny3D. Genera PNGs
optimizados RGBA en data/ usando un escritor PNG nativo (zlib + struct).

Uso:
    python3 tools/pack_ui_assets.py --generate-placeholders
        Genera PNGs validos 1-bit geometricos en data/, permitiendo compilar
        inmediatamente sin necesidad de imagenes finales en assets_raw/.

    python3 tools/pack_ui_assets.py
        Procesa imagenes desde assets_raw/ (si existen) hacia data/.
"""

import argparse
import os
import struct
import zlib

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS_RAW = os.path.join(BASE_DIR, "assets_raw")
DATA_DIR = os.path.join(BASE_DIR, "data")

OUTPUT_PNGS = {
    "ui_frame_9slice.png":    (128, 128),
    "ui_glyphs_ds3.png":      (256, 256),
    "ui_icons_iso.png":       (256, 256),
    "ui_radar_rings.png":     (256, 256),
}

WHITE = (255, 255, 255, 255)
TRANSPARENT = (0, 0, 0, 0)


# ── Escritor PNG nativo (solo stdlib) ─────────────────────────────

def _chunk(kind, data):
    crc = zlib.crc32(kind + data) & 0xffffffff
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", crc)


def _ihdr(w, h):
    # Bit depth 8, color type 6 (RGBA), compression 0, filter 0, interlace 0
    return struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)


def _idat(raw):
    compressed = zlib.compress(raw, 6)
    return compressed


def write_png(path, width, height, pixels):
    """Escribe un archivo PNG RGBA. pixels es lista de filas; cada fila es
    una lista de tuplas (R,G,B,A)."""
    # Cada fila tiene un byte de filtro (0 = ninguno) delante
    row_bytes = width * 4
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filtro "none"
        for x in range(width):
            r, g, b, a = pixels[y][x]
            raw.append(r)
            raw.append(g)
            raw.append(b)
            raw.append(a)

    idat = _idat(bytes(raw))
    png = bytearray()
    png += b"\x89PNG\r\n\x1a\n"  # signature
    png += _chunk(b"IHDR", _ihdr(width, height))
    png += _chunk(b"IDAT", idat)
    png += _chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    print(f"[PACK] {path} ({width}x{height})")


# ── Helpers de dibujo 1-bit ───────────────────────────────────────

def blank(w, h):
    return [[TRANSPARENT for _ in range(w)] for _ in range(h)]


def set_pixel(px, x, y, color):
    if 0 <= y < len(px) and 0 <= x < len(px[0]):
        px[y][x] = color


def hline(px, x0, x1, y, color):
    for x in range(x0, x1 + 1):
        set_pixel(px, x, y, color)


def vline(px, y0, y1, x, color):
    for y in range(y0, y1 + 1):
        set_pixel(px, x, y, color)


def rect_outline(px, x0, y0, x1, y1, color, width=1):
    for w in range(width):
        hline(px, x0 + w, x1 - w, y0 + w, color)
        hline(px, x0 + w, x1 - w, y1 - w, color)
        vline(px, y0 + w, y1 - w, x0 + w, color)
        vline(px, y0 + w, y1 - w, x1 - w, color)


def line(px, x0, y0, x1, y1, color, width=1):
    """Linea simple Bresenham (1 pixel de ancho)."""
    dx = abs(x1 - x0)
    dy = abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx - dy
    x, y = x0, y0
    while True:
        set_pixel(px, x, y, color)
        if x == x1 and y == y1:
            break
        e2 = 2 * err
        if e2 > -dy:
            err -= dy
            x += sx
        if e2 < dx:
            err += dx
            y += sy


def circle_outline(px, cx, cy, r, color, width=1):
    """Circulo Bresenham (contorno)."""
    for w in range(width):
        rr = r - w
        x = rr
        y = 0
        err = 0
        while x >= y:
            for dx, dy in ((x, y), (y, x), (-y, x), (-x, y),
                           (-x, -y), (-y, -x), (y, -x), (x, -y)):
                set_pixel(px, cx + dx, cy + dy, color)
            y += 1
            err += 1 + 2 * y
            if 2 * (err - x) + 1 > 0:
                x -= 1
                err += 1 - 2 * x


def fill_circle(px, cx, cy, r, color):
    for yy in range(cy - r, cy + r + 1):
        for xx in range(cx - r, cx + r + 1):
            if (xx - cx) ** 2 + (yy - cy) ** 2 <= r * r:
                set_pixel(px, xx, yy, color)


# ── Generadores de placeholders geometricos 1-bit ─────────────────

def gen_frame_9slice():
    """Marco biselado HUD 1-bit para 9-slice: borde blanco de 16px, centro transparente."""
    w, h = OUTPUT_PNGS["ui_frame_9slice.png"]
    px = blank(w, h)
    border = 16
    rect_outline(px, 0, 0, w - 1, h - 1, WHITE, width=border)
    # Esquinas biseladas
    line(px, border, 0, border * 2, 0, WHITE, 4)
    line(px, 0, border, 0, border * 2, WHITE, 4)
    line(px, w - border, 0, w - border * 2, 0, WHITE, 4)
    line(px, w - 1, border, w - 1, border * 2, WHITE, 4)
    line(px, border, h - 1, border * 2, h - 1, WHITE, 4)
    line(px, 0, h - border, 0, h - border * 2, WHITE, 4)
    line(px, w - border, h - 1, w - border * 2, h - 1, WHITE, 4)
    line(px, w - 1, h - border, w - 1, h - border * 2, WHITE, 4)
    return px


def gen_glyphs_ds3():
    """Hoja de glifos DualShock 3: 16 glifos blancos en cuadrícula 4x4."""
    w, h = OUTPUT_PNGS["ui_glyphs_ds3.png"]
    px = blank(w, h)
    cols, rows = 4, 4
    gw, gh = w // cols, h // rows
    glyphs = [
        "cross", "circle", "triangle", "square",
        "l1", "r1", "l2", "r2",
        "play", "pause", "stop", "prev",
        "next", "shuffle", "repeat", "lossless",
    ]

    for i, g in enumerate(glyphs):
        cx = (i % cols) * gw + gw // 2
        cy = (i // cols) * gh + gh // 2
        s = min(gw, gh) // 3
        if g == "cross":
            line(px, cx - s, cy - s, cx + s, cy + s, WHITE, 6)
            line(px, cx + s, cy - s, cx - s, cy + s, WHITE, 6)
        elif g == "circle":
            circle_outline(px, cx, cy, s, WHITE, 6)
        elif g == "triangle":
            line(px, cx, cy - s, cx + s, cy + s, WHITE, 6)
            line(px, cx + s, cy + s, cx - s, cy + s, WHITE, 6)
            line(px, cx - s, cy + s, cx, cy - s, WHITE, 6)
        elif g == "square":
            rect_outline(px, cx - s, cy - s, cx + s, cy + s, WHITE, 6)
        elif g in ("l1", "r1"):
            rect_outline(px, cx - s, cy - s, cx + s, cy + s, WHITE, 6)
        elif g in ("l2", "r2"):
            rect_outline(px, cx - s * 2, cy - s, cx + s * 2, cy + s, WHITE, 6)
        elif g == "play":
            fill_circle(px, cx, cy, s, TRANSPARENT)  # limpiar
            line(px, cx - s, cy - s, cx - s, cy + s, WHITE, 6)
            line(px, cx - s, cy + s, cx + s, cy, WHITE, 6)
            line(px, cx + s, cy, cx - s, cy - s, WHITE, 6)
        elif g == "pause":
            rect_outline(px, cx - s, cy - s, cx - s // 2, cy + s, WHITE, 6)
            rect_outline(px, cx + s // 2, cy - s, cx + s, cy + s, WHITE, 6)
        elif g == "stop":
            rect_outline(px, cx - s, cy - s, cx + s, cy + s, WHITE, 6)
            fill_circle(px, cx, cy, s // 2, WHITE)
        elif g == "prev":
            line(px, cx + s, cy - s, cx - s, cy, WHITE, 6)
            line(px, cx - s, cy, cx + s, cy + s, WHITE, 6)
        elif g == "next":
            line(px, cx - s, cy - s, cx + s, cy, WHITE, 6)
            line(px, cx + s, cy, cx - s, cy + s, WHITE, 6)
        elif g == "shuffle":
            line(px, cx - s, cy - s, cx + s, cy + s, WHITE, 5)
            line(px, cx + s, cy - s, cx - s, cy + s, WHITE, 5)
        elif g == "repeat":
            circle_outline(px, cx, cy, s, WHITE, 6)
            line(px, cx, cy - s, cx + s, cy - s // 2, WHITE, 6)
        elif g == "lossless":
            # "FLAC" simple: 4 barras
            line(px, cx - s, cy - s, cx - s, cy + s, WHITE, 4)
            line(px, cx - s + 4, cy - s, cx - s + 4, cy + s // 2, WHITE, 4)
            line(px, cx + s - 4, cy - s, cx + s - 4, cy + s, WHITE, 4)
            line(px, cx + s, cy - s, cx + s, cy + s // 2, WHITE, 4)
    return px


def gen_icons_iso():
    """Iconos isometricos 1-bit: 8 iconos en cuadrícula 4x2."""
    w, h = OUTPUT_PNGS["ui_icons_iso.png"]
    px = blank(w, h)
    cols, rows = 4, 2
    gw, gh = w // cols, h // rows

    for i in range(cols * rows):
        cx = (i % cols) * gw + gw // 2
        cy = (i // cols) * gh + gh // 2
        s = min(gw, gh) // 3
        # Cubo isometrico 1-bit
        line(px, cx, cy - s, cx + s, cy - s // 2, WHITE, 4)
        line(px, cx + s, cy - s // 2, cx, cy, WHITE, 4)
        line(px, cx, cy, cx - s, cy - s // 2, WHITE, 4)
        line(px, cx - s, cy - s // 2, cx, cy - s, WHITE, 4)
        line(px, cx - s, cy - s // 2, cx - s, cy + s // 2, WHITE, 4)
        line(px, cx - s, cy + s // 2, cx, cy, WHITE, 4)
        line(px, cx, cy, cx + s, cy + s // 2, WHITE, 4)
        line(px, cx + s, cy + s // 2, cx + s, cy - s // 2, WHITE, 4)
    return px


def gen_radar_rings():
    """Anillos concen­tricos blancos para el visualizador."""
    w, h = OUTPUT_PNGS["ui_radar_rings.png"]
    px = blank(w, h)
    cx, cy = w // 2, h // 2
    for r in range(30, min(w, h) // 2, 30):
        circle_outline(px, cx, cy, r, WHITE, 3)
    # Ejes cruz
    vline(px, 0, h - 1, cx, WHITE)
    hline(px, 0, w - 1, cy, WHITE)
    return px


PLACEHOLDER_GEN = {
    "ui_frame_9slice.png": gen_frame_9slice,
    "ui_glyphs_ds3.png": gen_glyphs_ds3,
    "ui_icons_iso.png": gen_icons_iso,
    "ui_radar_rings.png": gen_radar_rings,
}


def generate_placeholders():
    os.makedirs(DATA_DIR, exist_ok=True)
    os.makedirs(ASSETS_RAW, exist_ok=True)
    for name, gen_fn in PLACEHOLDER_GEN.items():
        path = os.path.join(DATA_DIR, name)
        px = gen_fn()
        write_png(path, len(px[0]), len(px), px)
    print("[OK] Placeholders 1-bit generados en data/")


def process_from_assets_raw():
    os.makedirs(DATA_DIR, exist_ok=True)
    os.makedirs(ASSETS_RAW, exist_ok=True)
    generated_any = False
    for name, (w, h) in OUTPUT_PNGS.items():
        src = os.path.join(ASSETS_RAW, name)
        if os.path.exists(src):
            # Cargar PNG simple desde archivo (solo RGBA 8-bit)
            try:
                from PIL import Image
            except ImportError:
                print(f"[SKIP] {name}: Pillow no disponible para leer {src}", file=sys.stderr)
                continue
            img = Image.open(src).convert("RGBA")
            px = [[img.getpixel((x, y)) for x in range(img.size[0])] for y in range(img.size[1])]
            # Forzar 1-bit: blanco puro + alpha
            for y in range(len(px)):
                for x in range(len(px[0])):
                    r, g, b, a = px[y][x]
                    if a > 128:
                        px[y][x] = (255, 255, 255, a)
                    else:
                        px[y][x] = (0, 0, 0, 0)
            path = os.path.join(DATA_DIR, name)
            write_png(path, img.size[0], img.size[1], px)
            generated_any = True
        else:
            print(f"[SKIP] {name}: no se encontro {src}")
    if not generated_any:
        print("[WARN] No se encontraron imagenes en assets_raw/. Use --generate-placeholders.")


def main():
    parser = argparse.ArgumentParser(description="Monochrome PS3 UI Asset Packer (1-bit)")
    parser.add_argument("--generate-placeholders", action="store_true",
                        help="Genera PNGs 1-bit geometricos validos en data/")
    args = parser.parse_args()

    if args.generate_placeholders:
        generate_placeholders()
    else:
        process_from_assets_raw()


if __name__ == "__main__":
    main()
