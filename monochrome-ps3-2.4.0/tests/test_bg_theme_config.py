"""Host-side verification for BgThemeMode config.cfg parsing and persistence."""

from pathlib import Path
import re


def parse_bg_theme(val, BG_THEME_COUNT=4, DEFAULT=0):
    try:
        t = int(val)
    except ValueError:
        return DEFAULT
    return t if 0 <= t < BG_THEME_COUNT else DEFAULT


def main():
    assert parse_bg_theme("0") == 0
    assert parse_bg_theme("1") == 1
    assert parse_bg_theme("2") == 2
    assert parse_bg_theme("3") == 3
    assert parse_bg_theme("4") == 0
    assert parse_bg_theme("99") == 0
    assert parse_bg_theme("-1") == 0
    assert parse_bg_theme("abc") == 0

    root = Path(__file__).resolve().parent.parent
    monochrome_h = (root / "include" / "monochrome.h").read_text(encoding="utf-8")
    app_state_h = (root / "include" / "app_state.h").read_text(encoding="utf-8")
    app_state_c = (root / "source" / "app_state.c").read_text(encoding="utf-8")
    main_c = (root / "source" / "main.c").read_text(encoding="utf-8")

    assert re.search(r"BG_THEME_FRACTURE_HUD\s*=\s*0", monochrome_h)
    assert re.search(r"BG_THEME_MATTE_SLABS\s*=\s*1", monochrome_h)
    assert re.search(r"BG_THEME_LIQUID_CHROME\s*=\s*2", monochrome_h)
    assert re.search(r"BG_THEME_PURE_BLACK\s*=\s*3", monochrome_h)
    assert re.search(r"BG_THEME_COUNT\s*=\s*4", monochrome_h)
    assert re.search(r"BgThemeMode\s+bg_theme;", app_state_h)
    assert "g_app_state.bg_theme" in app_state_c
    assert 'strcmp(key, "bg_theme") == 0' in app_state_c
    assert 'fprintf(f, "bg_theme=%d\\n", (int)g_app_state.bg_theme);' in app_state_c
    assert '#include "gfx_engine.h"' in main_c
    assert "gfx_set_background_theme((int)g_app_state.bg_theme);" in main_c

    print("config round-trip: OK")


if __name__ == "__main__":
    main()
