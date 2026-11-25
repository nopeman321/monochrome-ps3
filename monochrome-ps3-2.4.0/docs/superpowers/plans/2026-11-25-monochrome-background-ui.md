# Monochrome PS3 Background System & UI Adaptation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce a selectable 4-mode pure-black background system with XDR-shadow-framebuffer compositing, real alpha blending, a neutral-monochrome UI palette reserving violet exclusively for live playback signals, and a boxless fade-in splash screen across the entire Monochrome PS3 application.

**Architecture:** Three calibrated 8-bit grayscale PNGs are embedded in `.rodata` via the existing `bin2o` pipeline and decoded once per theme change by `libpng` into an overscan-aware XDR pre-baked buffer (`s_baked_bg_xdr`). All drawing primitives composite onto a per-frame XDR shadow framebuffer (`s_shadow_fb`), which is flushed to RSX VRAM in a single 64-bit aligned burst per frame, keeping zero VRAM reads and stable ~60 FPS. UI tokens are replaced with pure-black neutral greys and translucent surfaces; violet (`#7B2CBF` / `#9D4EDD`) appears only on live-playback signals.

**Tech Stack:** PSL1GHT v2 / Cell PPU C, `libpng14`, `libz`, `libjpeg`, `librsx`, `libgcm_sys`, Python 3 + Pillow (asset pipeline), Docker (`ghcr.io/altps3/ps3dev:latest`), RPCS3 for verification.

**Spec:** `docs/superpowers/specs/2026-11-24-monochrome-background-ui-design.md`

## Global Constraints

- Build target: `monochrome_ps3.elf` / `.self` / `.pkg` (`APPID := MONO00024`), built via `tools/run_in_docker.ps1` → `make clean && make pkg` with zero errors.
- Framebuffer format: `VIDEO_BUFFER_FORMAT_XRGB` — `uint32_t` `0xAARRGGBB` Big-Endian on Cell PPU; `blit_rgba` / `GfxTexture` byte order is `[R, G, B, A]`.
- No RSX VRAM reads during CPU alpha blending (FlexIO PPU→VRAM reads stall the bus).
- No JPEG compression for background assets; output must be 8-bit grayscale PNG.
- No animated shaders or video backgrounds; backgrounds are static PNGs with dynamic brightness multiplier.
- No opaque `#12141C` fills hiding the background canvas.
- Background PNGs: `data/bg_fracture_hud.png`, `data/bg_matte_slabs.png`, `data/bg_liquid_chrome.png` — all `1920×1080`, mode `L` (8-bit grayscale), true `#000000` floor. Auto-embedded by existing `Makefile:77,88,98` via `bin2o`; no Makefile changes required.
- `libpng14` available at `/usr/local/ps3dev/portlibs/ppu/include/png.h`, already on `INCLUDE` and `-lpng -lz` linked.
- Virtual coordinate space: `VIRT_W×VIRT_H` (`1920×1080`); native via `vx()`, `vy()`, `vw()`, `vh()`, `s_scale_x/y`, `s_offset_x/y`. Overscan range `0.70..1.00`.
- Do NOT modify `source/audio_engine.c`, `include/audio_engine.h`, `source/http_client.c`, `include/http_client.h`, `source/rsxutil.c` scanout init, or `legacy_unused/`.
- All documentation and identifiers: 100% English. UI strings remain in Spanish per existing convention.
- Violet tokens (`TH_ACCENT` / `TH_ACCENT_LT`) appear ONLY on live-playback state: currently-playing track rows, Hero `[ EN REPRODUCCION ]` badge, playing EQ bars, Dock timeline fill/scrubber/elapsed time, and Fullscreen Visualizer spectrum and progress bar.

## Review Focus

The five failure modes most likely to bite a person using this software:

1. **Corrupt `config.cfg` `bg_theme` value** — an out-of-range integer (`-1`, `99`, non-numeric) must silently clamp to `BG_THEME_FRACTURE_HUD` without crashing, misreporting, or leaving an invalid decode state. Test pinned to **Task 2, Step 5**: `assert app_state_load_config("bg_theme=99") → g_app_state.bg_theme == BG_THEME_FRACTURE_HUD` and `assert app_state_load_config("bg_theme=-1") → BG_THEME_FRACTURE_HUD`.

2. **Overscan < 1.0 out-of-bounds read in `bake_background()`** — inverse-mapped source coordinates outside `[0..1920) × [0..1080)` must write `0xFF000000` without reading beyond the decoded buffer. Test pinned to **Task 3, Step 8**: in Docker Python test, call `gfx_set_overscan(0.70f)` and verify no access violation or PPU trap in RPCS3 TTY log while all 3 textured themes display correctly at the screen edges.

3. **`BG_THEME_PURE_BLACK` path leaks or crashes** — selecting mode `3` must free or skip `s_baked_bg_xdr`, never attempt a `decode_png_memory()` call, and produce a solid `0xFF000000` `s_shadow_fb` from `gfx_begin_frame()`. Test pinned to **Task 3, Step 8**: RPCS3 `TTY.log` shows zero `Access violation` / `PPU Trap` after cycling to `Negro Puro` and back.

4. **Non-PNG or truncated embedded buffer passed to `gfx_decode_png_memory()`** — must `longjmp` via `png_jmpbuf`, free all temporary allocations, and return `-1` without undefined behavior. Test pinned to **Task 3, Step 3 & Step 8**: add `tests/test_decode_png_bad.c` that passes a 16-byte garbage buffer to `gfx_decode_png_memory()` and asserts return `-1` with `out_rgba == NULL`, compiled in Docker via `ppu-gcc`.

5. **Scrubber cap rendered black after `TH_ACTIVE_TEXT` becomes `#000000`** — `ui_player_dock.c` line 185 outer cap must use `TH_ACCENT`, not `TH_ACTIVE_TEXT`. Test pinned to **Task 5, Step 5**: `rg -n "TH_ACTIVE_TEXT" source/ui_player_dock.c` → 0 matches; RPCS3 screenshot of Dock with playing track confirms visible violet scrubber caps.

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `assets_raw/bg_fracture_hud.png` | Create | Canonical master — Fractura & Micro-HUD |
| `assets_raw/bg_matte_slabs.png` | Create | Canonical master — Planos Esculturales Mate |
| `assets_raw/bg_liquid_chrome.png` | Create | Canonical master — Curvas Líquidas |
| `tools/pack_ui_assets.py` | Modify | Adds `pack_backgrounds()`: L-mode conversion, black floor, highlight attenuation, PIC1 export |
| `data/bg_fracture_hud.png` | Generate | 8-bit L PNG, embedded via `bin2o` as `bg_fracture_hud_png` |
| `data/bg_matte_slabs.png` | Generate | 8-bit L PNG, 15% highlight attenuation, embedded as `bg_matte_slabs_png` |
| `data/bg_liquid_chrome.png` | Generate | 8-bit L PNG, embedded as `bg_liquid_chrome_png` |
| `pkgfiles/PIC1.PNG` | Modify | 1920×1080 RGB PNG for PS3 XMB wallpaper |
| `include/monochrome.h` | Modify | Add `BgThemeMode` enum |
| `include/app_state.h` | Modify | Add `BgThemeMode bg_theme` to `AppContext` |
| `source/app_state.c` | Modify | Init, load, save `bg_theme` |
| `source/main.c` | Modify | Call `gfx_set_background_theme()` after overscan init |
| `include/gfx_engine.h` | Modify | Declare `gfx_set_background_theme()`, `gfx_set_background_dim()`, `gfx_decode_png_memory()` |
| `source/gfx_tiny3d.c` | Modify | XDR shadow FB, libpng decoder, bake, alpha blend, 64-bit flush |
| `include/ui_theme.h` | Modify | Replace blue-slate tokens; add `TH_BAR_BG`, `TH_MODAL_BG`, `TH_SCRIM` |
| `source/ui_shell.c` | Modify | Canvas reveal, bar translucency, dim call |
| `source/ui_player_dock.c` | Modify | `TH_BAR_BG` dock bg, fix scrubber outer cap |
| `source/ui_components.c` | Modify | Playback-only violet in card/row render |
| `source/ui_home.c` | Modify | Section header and badge tokens |
| `source/ui_search.c` | Modify | Section header, keyboard, drilldown tokens |
| `source/ui_library.c` | Modify | Section header tokens |
| `source/ui_queue.c` | Modify | Drawer background (`TH_MODAL_BG`, `TH_SCRIM`), section header tokens |
| `source/ui_settings.c` | Modify | 4-row selector (Row 0 = theme), remove non-playback violet, `NETCTL` white |
| `source/ui_visualizer.c` | Modify | 60% dim on enter, restore 1.0f on exit |
| `source/ui_v3.c` | Modify | Boxless fade-in splash |

---

### Task 1: Background Asset Pipeline & XMB PIC1 Export

**Files:**
- Create: `assets_raw/bg_fracture_hud.png` (copy of `raw_3_fracture_hud.png`)
- Create: `assets_raw/bg_matte_slabs.png` (copy of `raw_1_supermodified.png`)
- Create: `assets_raw/bg_liquid_chrome.png` (copy of `raw_2_liquid_chrome.png`)
- Modify: `tools/pack_ui_assets.py:18-32,294-339`
- Generate: `data/bg_fracture_hud.png`, `data/bg_matte_slabs.png`, `data/bg_liquid_chrome.png`
- Modify: `pkgfiles/PIC1.PNG`

**Interfaces:**
- Consumes: 1920×1080 RGB source PNGs in `C:\Users\Juanjo\.gemini\antigravity\brain\d514a632-44b5-4498-af6f-982537de24ba\` (`raw_3_fracture_hud.png`, `raw_1_supermodified.png`, `raw_2_liquid_chrome.png`)
- Produces:
  - `data/bg_fracture_hud.png` → `bg_fracture_hud_png[]` / `bg_fracture_hud_png_size` (via `Makefile` `bin2o`)
  - `data/bg_matte_slabs.png` → `bg_matte_slabs_png[]` / `bg_matte_slabs_png_size`
  - `data/bg_liquid_chrome.png` → `bg_liquid_chrome_png[]` / `bg_liquid_chrome_png_size`
  - `pkgfiles/PIC1.PNG` — 1920×1080 RGB

- [ ] **Step 1: Stage source PNGs into `assets_raw/`**

  Copy the three approved master images:
  - `raw_3_fracture_hud.png` → `assets_raw/bg_fracture_hud.png`
  - `raw_1_supermodified.png` → `assets_raw/bg_matte_slabs.png`
  - `raw_2_liquid_chrome.png` → `assets_raw/bg_liquid_chrome.png`

  Source directory: `C:\Users\Juanjo\.gemini\antigravity\brain\d514a632-44b5-4498-af6f-982537de24ba\`

- [ ] **Step 2: Extend `tools/pack_ui_assets.py` with `pack_backgrounds()`**

  At lines 18-32: add `import sys` (line 20). Define constants:
  ```python
  PKGFILES_DIR = os.path.join(BASE_DIR, "pkgfiles")
  BG_SOURCES = {
      "bg_fracture_hud.png":  {"highlight_scale": 1.00, "black_floor": 3},
      "bg_matte_slabs.png":   {"highlight_scale": 0.85, "black_floor": 3},
      "bg_liquid_chrome.png": {"highlight_scale": 1.00, "black_floor": 3},
  }
  ```

  Add `pack_backgrounds()` at lines 294-339: for each entry in `BG_SOURCES`, load `assets_raw/<name>` with Pillow, assert `size == (1920, 1080)`, convert to `"L"`, clamp pixels `<= black_floor` to `0`, apply `highlight_scale` (multiply each non-zero pixel by `highlight_scale`, round to `int`, clamp to `255`), save to `data/<name>` with `optimize=True`. After processing all three, convert `assets_raw/bg_fracture_hud.png` to `"RGB"` and save to `pkgfiles/PIC1.PNG`. Call `pack_backgrounds()` from `process_from_assets_raw()` and when invoked as `python3 tools/pack_ui_assets.py` or with `--backgrounds-only`. Keep `OUTPUT_PNGS` and `--generate-placeholders` logic for `data/ui_*.png` intact.

- [ ] **Step 3: Run the asset pipeline and verify outputs**

  Run:
  ```bash
  python3 tools/pack_ui_assets.py
  ```

  Then verify with Pillow:
  ```python
  from PIL import Image
  for name in ["bg_fracture_hud", "bg_matte_slabs", "bg_liquid_chrome"]:
      img = Image.open(f"data/{name}.png")
      assert img.size == (1920, 1080), f"{name}: wrong size"
      assert img.mode == "L",          f"{name}: not grayscale"
      assert img.getextrema()[0] == 0, f"{name}: black floor missing"
  pic1 = Image.open("pkgfiles/PIC1.PNG")
  assert pic1.size == (1920, 1080) and pic1.mode == "RGB"
  for ui in ["ui_frame_9slice", "ui_glyphs_ds3", "ui_icons_iso", "ui_radar_rings"]:
      assert Image.open(f"data/{ui}.png"), f"{ui} vanished"
  print("Asset pipeline: OK")
  ```

  Expected: `Asset pipeline: OK`

- [ ] **Step 4: Commit**

  ```bash
  git add assets_raw/ tools/pack_ui_assets.py data/bg_*.png pkgfiles/PIC1.PNG
  git commit -m "feat(assets): add bg master PNGs, pack_backgrounds() L-mode pipeline, and PIC1 XMB export"
  ```

---

### Task 2: BgThemeMode Enum & config.cfg Persistence

**Files:**
- Modify: `include/monochrome.h:28-33`
- Modify: `include/app_state.h:62-68`
- Modify: `source/app_state.c:74-81,102-117,124-139`
- Modify: `source/main.c:17-24,43-72`

**Interfaces:**
- Consumes: `int gfx_set_background_theme(int theme_id);` (produced by Task 3; `main.c` call site compiles only after Task 3 declares it in `include/gfx_engine.h`)
- Produces:
  - `typedef enum { BG_THEME_FRACTURE_HUD=0, BG_THEME_MATTE_SLABS=1, BG_THEME_LIQUID_CHROME=2, BG_THEME_PURE_BLACK=3, BG_THEME_COUNT=4 } BgThemeMode;` in `include/monochrome.h`
  - `BgThemeMode bg_theme;` in `AppContext` (`include/app_state.h`)
  - `g_app_state.bg_theme` initialized to `BG_THEME_FRACTURE_HUD`, loaded from and saved to `bg_theme=%d` in `config.cfg`, invalid values clamped to `BG_THEME_FRACTURE_HUD`

- [ ] **Step 1: Add `BgThemeMode` enum to `include/monochrome.h:28-33`**

  Insert after the `CoverArtMode` enum (line 28):
  ```c
  typedef enum {
      BG_THEME_FRACTURE_HUD  = 0, /* Default */
      BG_THEME_MATTE_SLABS   = 1,
      BG_THEME_LIQUID_CHROME = 2,
      BG_THEME_PURE_BLACK    = 3,
      BG_THEME_COUNT         = 4
  } BgThemeMode;
  ```

- [ ] **Step 2: Add `bg_theme` field to `AppContext` in `include/app_state.h:62-68`**

  Inside the `AppContext` persistent configuration block (line 62-68), add:
  ```c
  BgThemeMode bg_theme;
  ```

- [ ] **Step 3: Implement init, load, save in `source/app_state.c`**

  - `app_state_init()` (line 75): add `g_app_state.bg_theme = BG_THEME_FRACTURE_HUD;`
  - `app_state_load_config()` (lines 102-117): add an `else if` branch:
    ```c
    else if (strcmp(key, "bg_theme") == 0) {
        int t = atoi(val);
        g_app_state.bg_theme = (t < 0 || t >= BG_THEME_COUNT)
            ? BG_THEME_FRACTURE_HUD : (BgThemeMode)t;
    }
    ```
  - `app_state_save_config()` (lines 130-136): add:
    ```c
    fprintf(f, "bg_theme=%d\n", (int)g_app_state.bg_theme);
    ```

- [ ] **Step 4: Wire `gfx_set_background_theme()` in `source/main.c`**

  - Lines 17-24: add `#include "gfx_engine.h"`.
  - Lines 46-47 (startup log): extend to include `bg_theme=%d` from `g_app_state.bg_theme`.
  - After overscan setup and before `ui_render_splash()` (line 60 area): call `gfx_set_background_theme((int)g_app_state.bg_theme);`

- [ ] **Step 5: Verify config round-trip logic**

  Write a host-side Python test (run outside Docker, exercising the parse logic textually):
  ```python
  # tests/test_bg_theme_config.py
  def parse_bg_theme(val, BG_THEME_COUNT=4, DEFAULT=0):
      try:
          t = int(val)
      except ValueError:
          return DEFAULT
      return t if 0 <= t < BG_THEME_COUNT else DEFAULT

  assert parse_bg_theme("0") == 0
  assert parse_bg_theme("3") == 3
  assert parse_bg_theme("99") == 0
  assert parse_bg_theme("-1") == 0
  assert parse_bg_theme("abc") == 0
  print("config round-trip: OK")
  ```

  Run: `python tests/test_bg_theme_config.py` → `config round-trip: OK`

  *(Full compile verification deferred to Task 3 build step, since `gfx_engine.h` declaration is needed.)*

- [ ] **Step 6: Commit**

  ```bash
  git add include/monochrome.h include/app_state.h source/app_state.c source/main.c
  git commit -m "feat(state): add BgThemeMode enum and config.cfg bg_theme persistence"
  ```

---

### Task 3: GFX Engine — XDR Shadow FB, libpng Decoder, Overscan Pre-Bake & Alpha Blending

**Files:**
- Modify: `include/gfx_engine.h:47-51`
- Modify: `source/gfx_tiny3d.c:10-42,155-237,241-315,319-380,405-524,595-631,702-757,764-775`

**Interfaces:**
- Consumes:
  - `BgThemeMode` / `BG_THEME_*` / `BG_THEME_COUNT` (`include/monochrome.h`, Task 2)
  - `bg_fracture_hud_png[]`, `bg_fracture_hud_png_size`, `bg_matte_slabs_png[]`, `bg_matte_slabs_png_size`, `bg_liquid_chrome_png[]`, `bg_liquid_chrome_png_size` (auto-generated `build/bg_*_png.h`, Task 1)
- Produces:
  - `int  gfx_set_background_theme(int theme_id);` — clamps invalid to `BG_THEME_FRACTURE_HUD`, triggers `bake_background()`, returns applied theme
  - `void gfx_set_background_dim(float brightness);` — clamps to `[0.0f, 1.0f]`, sets `s_bg_brightness`
  - `int  gfx_decode_png_memory(const uint8_t *buf, size_t len, uint8_t **out_rgba, int *out_w, int *out_h);` — in-memory `libpng` decoder returning `[R, G, B, A]` or `-1` on invalid/corrupt input
  - Real alpha-blended `gfx_draw_rect()`, `blit_rgba()`, and all draw primitives compositing on `s_shadow_fb`, flushed 64-bit to RSX VRAM in `gfx_end_frame()`
  - Real `libpng`-based `gfx_load_texture_png()` (fixes existing `bool ok = ...` bug at line 372)
  - `static void blend_pixel_xrgb(uint32_t *dst, uint32_t rgba);`

- [ ] **Step 1: Declare new public APIs in `include/gfx_engine.h:47-51`**

  Add after existing declarations:
  ```c
  int  gfx_set_background_theme(int theme_id);
  void gfx_set_background_dim(float brightness);
  int  gfx_decode_png_memory(const uint8_t *buf, size_t len,
                             uint8_t **out_rgba, int *out_w, int *out_h);
  ```

- [ ] **Step 2: Add includes and static state in `source/gfx_tiny3d.c:10-42`**

  Includes: `<png.h>`, `"bg_fracture_hud_png.h"`, `"bg_matte_slabs_png.h"`, `"bg_liquid_chrome_png.h"`.

  Static state variables:
  ```c
  static uint32_t *s_shadow_fb    = NULL;
  static uint32_t *s_baked_bg_xdr = NULL;
  static int       s_bg_theme     = BG_THEME_FRACTURE_HUD;
  static float     s_bg_brightness = 1.0f;
  static bool      s_bg_dirty     = true;
  ```

- [ ] **Step 3: Implement `gfx_decode_png_memory()` and fix `gfx_load_texture_png()`**

  Implement `PngMemCursor` struct `{ const uint8_t *data; size_t pos; size_t len; }` and `png_mem_read_cb(png_structp, png_bytep, size_t)` reading from it.

  Implement `int gfx_decode_png_memory(const uint8_t *buf, size_t len, uint8_t **out_rgba, int *out_w, int *out_h)`:
  - Initialize `*out_rgba = NULL`; verify `buf != NULL && len >= 8` and PNG signature via `png_sig_cmp(buf, 0, 8)`; return `-1` on mismatch.
  - `png_create_read_struct` + `png_create_info_struct`; `setjmp(png_jmpbuf(png_ptr))` → free any partial allocation, set `*out_rgba = NULL`, and return `-1`.
  - `png_set_read_fn` with `PngMemCursor`.
  - Transformations: 16→8 strip, palette→RGB, expand `<8-bit` gray, `tRNS`→alpha, gray→RGB (`png_set_gray_to_rgb`), opaque filler (`png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER)`). Output: `[R, G, B, A]` bytes.
  - Allocate `memalign(16, w * h * 4)`, read rows, destroy structs, return `0`.

  Fix `gfx_load_texture_png()` (lines 319-380):
  - Read file into temp buffer, decode with `gfx_decode_png_memory()`, call `gfx_upload_rgba_texture(rgba, w, h, out) == 0` (replacing `bool ok = gfx_upload_rgba_texture(...)` to fix zero-is-success inversion), free buffers, return `0` or `-1`.

  Add bad-buffer test `tests/test_decode_png_bad.c` including `"gfx_engine.h"`: pass a 16-byte garbage buffer to `gfx_decode_png_memory(bad, sizeof(bad), &rgba, &w, &h)`, assert return `-1` and `rgba == NULL`.

- [ ] **Step 4: Implement `bake_background()`**

  `static void bake_background(void)` in `source/gfx_tiny3d.c`:
  - Return early (leave `s_bg_dirty = true`) if `s_shadow_fb == NULL`.
  - If `s_bg_theme == BG_THEME_PURE_BLACK`: `free(s_baked_bg_xdr); s_baked_bg_xdr = NULL; s_bg_dirty = false; return;`
  - Select `(png_data, png_size)` by `s_bg_theme`; call `gfx_decode_png_memory()`. On failure: `free(s_baked_bg_xdr); s_baked_bg_xdr = NULL; return;`
  - If `s_baked_bg_xdr == NULL`: allocate via `memalign(64, (size_t)s_screen_w * s_screen_h * 4u)`.
  - For each native pixel `(px, py)`: compute `vx_inv = ((float)px - s_offset_x) / s_scale_x`, `vy_inv = ((float)py - s_offset_y) / s_scale_y`; if outside `[0, sw) × [0, sh)` write `0xFF000000u`; otherwise bilinearly sample the 4 neighboring source texels (gray channel → R=G=B) and pack `0xFF000000u | (r << 16) | (g << 8) | b`.
  - `free(decoded_rgba); s_bg_dirty = false;`

- [ ] **Step 5: Implement `gfx_set_background_theme()`, `gfx_set_background_dim()`, update `gfx_set_overscan()`**

  `int gfx_set_background_theme(int theme_id)`:
  - Clamp `theme_id < 0 || theme_id >= BG_THEME_COUNT` to `BG_THEME_FRACTURE_HUD`.
  - If `theme_id != s_bg_theme || s_bg_dirty`: set `s_bg_theme = theme_id`, call `bake_background()`.
  - Return `s_bg_theme`.

  `void gfx_set_background_dim(float brightness)`:
  - Clamp to `[0.0f, 1.0f]`, set `s_bg_brightness = brightness`.

  `gfx_set_overscan()` (lines 764-775): after updating `s_overscan_*`, `s_scale_*`, `s_offset_*`, if `s_shadow_fb != NULL` and parameters changed, call `bake_background()`.

- [ ] **Step 6: Update `gfx_init()`, `gfx_shutdown()`, `gfx_begin_frame()`, `gfx_end_frame()` (lines 155-237)**

  `gfx_init()`: allocate `s_shadow_fb = memalign(64, (size_t)s_screen_w * s_screen_h * 4u)`; return `-1` if `NULL`; fill with `0xFF000000u`; call `gfx_set_overscan()` and `bake_background()`.

  `gfx_shutdown()`: `free(s_shadow_fb); s_shadow_fb = NULL; free(s_baked_bg_xdr); s_baked_bg_xdr = NULL;`

  `gfx_begin_frame(uint32_t clear_rgba)` — all paths write to `s_shadow_fb`, never `b->ptr`:
  - Pure black path (`s_bg_theme == BG_THEME_PURE_BLACK || s_baked_bg_xdr == NULL || s_bg_brightness <= 0.001f`): 64-bit unrolled burst fill with `0xFF000000u | (clear_rgba & 0x00FFFFFFu)`.
  - Full brightness path (`s_bg_brightness >= 0.999f`): 64-bit unrolled copy `s_baked_bg_xdr → s_shadow_fb`.
  - Dimmed path: fixed-point `uint32_t scale = (uint32_t)(s_bg_brightness * 256.0f)` (clamped `0..256`); per-pixel: scale R, G, B of `s_baked_bg_xdr[i]`, pack with `0xFF` top byte into `s_shadow_fb[i]`.

  `gfx_end_frame()`: 64-bit unrolled copy `s_shadow_fb → s_buffers[s_cur_buffer].ptr` (handle odd trailing pixel); then `flip(s_context, s_buffers[s_cur_buffer].id); s_cur_buffer = (s_cur_buffer + 1) % 2; waitFlip();`

- [ ] **Step 7: Route all drawing primitives to `s_shadow_fb` with real alpha blending**

  Implement shared helper:
  ```c
  static void blend_pixel_xrgb(uint32_t *dst, uint32_t rgba);
  ```
  Blends `rgba` (`0xAARRGGBB`) onto `*dst`: if `a == 0` return; if `a == 255` write opaque; else per-channel `(src_c * a + dst_c * (255 - a) + 127) / 255` with `0xFF` top byte.

  `gfx_draw_rect()` (lines 241-273): `a == 0` → return; `a == 255` → 64-bit solid fill on `s_shadow_fb`; `1 <= a <= 254` → per-pixel `blend_pixel_xrgb` on `s_shadow_fb`.

  `gfx_draw_rect_outline()` (275-315), `draw_glyph_box()` (595-631), `draw_line_sw()` (702-718), `gfx_draw_wireframe_mesh()` (720-757): write/blend onto `s_shadow_fb` via `blend_pixel_xrgb`.

  `blit_rgba()` (405-439): change destination parameter to `uint32_t *dst_buf, int dst_w, int dst_h` (call sites pass `s_shadow_fb, s_screen_w, s_screen_h`). Compute `eff_a = (p[3] * t_a + 127) / 255`; skip `eff_a == 0`; for `eff_a == 255` write opaque tinted pixel; for `1 <= eff_a <= 254` blend tinted RGB via `blend_pixel_xrgb`. Update `gfx_draw_sprite()` / `gfx_draw_9slice()` (441-524) call sites accordingly.

- [ ] **Step 8: Build verification and RPCS3 runtime check**

  Compile the bad-PNG test object in Docker:
  ```bash
  powershell -ExecutionPolicy Bypass -File tools/run_in_docker.ps1 "ppu-gcc -Wall -Werror -Iinclude -I/usr/local/ps3dev/portlibs/ppu/include -c tests/test_decode_png_bad.c -o /tmp/test_decode_png_bad.o"
  ```
  Expected: `0` warnings/errors.

  Run full build:
  ```bash
  powershell -ExecutionPolicy Bypass -File tools/run_in_docker.ps1 "make clean && make pkg"
  ```
  Expected: `0 errors`, `monochrome_ps3.pkg` produced.

  Run: `python tools/verify_pkg.py monochrome_ps3.pkg`
  Expected: `[SUCCESS] All RPCS3 PKG checks passed 100%!`

  Deploy and launch via `tools/verify_runtime.ps1`. Check `TTY.log` / `RPCS3.log` for:
  - Zero `Access violation` / `PPU Trap` / `CELL_ENOENT`
  - `[GFX] Backend RSX inicializado`
  - `[PERF]` logs showing ~60 FPS

  At overscan `0.70f`: confirm all 3 textured themes display without edge artifacts or PPU trap.
  At `BG_THEME_PURE_BLACK`: confirm solid black and no PPU trap on theme cycle.

- [ ] **Step 9: Commit**

  ```bash
  git add include/gfx_engine.h source/gfx_tiny3d.c tests/test_decode_png_bad.c
  git commit -m "feat(gfx): XDR shadow FB, libpng decoder, overscan pre-bake, real alpha blend, 64-bit flush"
  ```

---

### Task 4: Neutral Monochrome Theme & Translucent Surface Tokens

**Files:**
- Modify: `include/ui_theme.h:6-18` (hex constants)
- Modify: `include/ui_theme.h:57-68` (TH_* macros)

**Interfaces:**
- Consumes: `th_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a)` macro at `include/ui_theme.h:52-55`
- Produces (all `TH_*` tokens consumed by Tasks 5–7):
  - `TH_BG`:         `th_color(0x00, 0x00, 0x00, 0xFF)`
  - `TH_BAR_BG`:     `th_color(0x00, 0x00, 0x00, 0xCC)` *(new)*
  - `TH_SURFACE`:    `th_color(0x08, 0x08, 0x08, 0xA8)`
  - `TH_SURFACE_2`:  `th_color(0x16, 0x16, 0x16, 0xC4)`
  - `TH_MODAL_BG`:   `th_color(0x06, 0x06, 0x06, 0xEA)` *(new)*
  - `TH_SCRIM`:      `th_color(0x00, 0x00, 0x00, 0xA0)` *(new)*
  - `TH_BORDER`:     `th_color(0x2E, 0x2E, 0x2E, 0xFF)`
  - `TH_BORDER_LT`:  `th_color(0x88, 0x88, 0x88, 0xFF)`
  - `TH_TEXT`:       `th_color(0xFF, 0xFF, 0xFF, 0xFF)`
  - `TH_TEXT_DIM`:   `th_color(0x9A, 0x9A, 0x9A, 0xFF)`
  - `TH_ACTIVE_BG`:  `th_color(0xFF, 0xFF, 0xFF, 0xFF)`
  - `TH_ACTIVE_TEXT`:`th_color(0x00, 0x00, 0x00, 0xFF)`
  - `TH_ACCENT`:     `th_color(0x7B, 0x2C, 0xBF, 0xFF)` *(playback-only)*
  - `TH_ACCENT_LT`:  `th_color(0x9D, 0x4E, 0xDD, 0xFF)` *(playback-only)*
  - `COLOR_*` constants updated to match (lines 6-18)

- [ ] **Step 1: Replace blue-slate `COLOR_*` hex constants in `include/ui_theme.h:6-18`**

  Update: `COLOR_SURFACE` → `0x00080808`, `COLOR_SURFACE_2` → `0x00161616`, `COLOR_BORDER` → `0x002E2E2E`, `COLOR_BORDER_LT` → `0x00888888`, `COLOR_TEXT_DIM` → `0x009A9A9A`, `COLOR_ACTIVE_TEXT` → `0x00000000`. Add `COLOR_BAR_BG`, `COLOR_MODAL_BG`, `COLOR_SCRIM` constants.

- [ ] **Step 2: Replace `TH_*` macros and add `TH_BAR_BG`, `TH_MODAL_BG`, `TH_SCRIM` in `include/ui_theme.h:57-68`**

  Set every `TH_*` macro to the exact RGBA values listed in `Produces` above.

- [ ] **Step 3: Verify no legacy blue-slate tokens remain**

  Run:
  ```bash
  rg -n "12141C|1A1E2A|383D50|4A5268|8890A4" include/ui_theme.h
  ```
  Expected: 0 matches.

  Run:
  ```bash
  powershell -ExecutionPolicy Bypass -File tools/run_in_docker.ps1 "make"
  ```
  Expected: 0 errors.

- [ ] **Step 4: Commit**

  ```bash
  git add include/ui_theme.h
  git commit -m "feat(ui): switch theme tokens to pure-black neutral monochrome and translucent surfaces"
  ```

---

### Task 5: Shell Canvas Reveal, Bar Translucency & Playback-Only Violet Adaptation

**Files:**
- Modify: `source/ui_shell.c:98,155,173,310-333`
- Modify: `source/ui_player_dock.c:34,185-186`
- Modify: `source/ui_components.c:37-43,83-103`
- Modify: `source/ui_home.c:29,75`
- Modify: `source/ui_search.c:321-328,363,378-380,403,410,418,480,511,540`
- Modify: `source/ui_library.c:48,86,121,126`
- Modify: `source/ui_queue.c:91-98,117,155,167`

**Interfaces:**
- Consumes:
  - All `TH_*` tokens (`include/ui_theme.h`, Task 4)
  - `void gfx_set_background_dim(float brightness);` (`include/gfx_engine.h`, Task 3)
- Produces:
  - Translucent Top Bar (`TH_BAR_BG`) / Left Rail (`TH_BAR_BG`) / Bottom Dock (`TH_BAR_BG`) / Queue Drawer (`TH_MODAL_BG` + `TH_SCRIM`) over live pre-baked background
  - Strict playback-only violet across all browsing modules; `TH_ACTIVE_TEXT` (#000000) never used for scrubber cap

- [ ] **Step 1: Reveal central canvas and apply bar translucency in `source/ui_shell.c`**

  - `render_top_bar()` line 98: `TH_SURFACE` → `TH_BAR_BG`.
  - `render_left_rail()` line 155: `TH_SURFACE` → `TH_BAR_BG`.
  - `render_left_rail()` line 173: active nav item 4px left bar `TH_ACCENT` → `TH_BORDER_LT`.
  - `shell_render()` lines 310-333: add `gfx_set_background_dim(1.0f);` at function entry; remove `gfx_draw_rect(0, 0, VIRT_W, VIRT_H, TH_BG);` (line 317); remove central canvas cover fill `gfx_draw_rect(x0, y0, mw, mh, TH_SURFACE);` (lines 326-332).

- [ ] **Step 2: Adapt `source/ui_player_dock.c` and fix scrubber outer cap**

  - Line 34: dock background `TH_SURFACE` → `TH_BAR_BG`.
  - Line 185: scrubber outer cap `TH_ACTIVE_TEXT` → `TH_ACCENT` (inner cap line 186 stays `TH_ACCENT_LT`).
  - Lines 118, 121 (playing EQ bars), 156 (elapsed time), 178 (progress bar fill): keep `TH_ACCENT` / `TH_ACCENT_LT` unchanged.

- [ ] **Step 3: Enforce playback-only violet in `source/ui_components.c`**

  - `album_card_render()` line 38: focused outline `TH_ACCENT_LT` → `TH_BORDER_LT`; lines 40, 66 (`is_playing`) keep `TH_ACCENT` / `TH_ACCENT_LT`.
  - `track_row_render()` line 87: focused 4px left bar `TH_ACCENT_LT` → `TH_TEXT`; lines 99-102: focused `idx_color` / `dur_color` `TH_ACCENT_LT` → `TH_TEXT`; lines 91, 99-100 (`is_playing`) keep `TH_ACCENT` / `TH_ACCENT_LT`.

- [ ] **Step 4: Remove non-playback violet from `ui_home.c`, `ui_search.c`, `ui_library.c`, `ui_queue.c`**

  `source/ui_home.c`:
  - Line 29 (`"DESTACADO //"`): `TH_ACCENT` → `TH_TEXT_DIM`.
  - Line 75 (`"FLAC 48kHz / 24-bit"` badge): `TH_ACCENT_LT` → `TH_TEXT`.
  - Lines 57 (Hero badge), 106-109 (playing EQ bars): keep violet.

  `source/ui_search.c`:
  - Line 321 (`"BUSQUEDA //"`): `TH_ACCENT` → `TH_TEXT`.
  - Line 323 (404 notice): `TH_ACCENT_LT` → `TH_TEXT_DIM`.
  - Line 328 (search bar border): `TH_ACCENT_LT` → `TH_BORDER_LT`.
  - Line 363 (focused key outline): `TH_ACCENT_LT` → `TH_BORDER_LT`.
  - Lines 378-380 (`[START] BUSCAR` button): `TH_ACCENT_LT` → `TH_TEXT`, `TH_ACCENT` → `TH_BORDER_LT` (when focused).
  - Lines 403, 410, 418 (busy/error/drilldown texts): `TH_ACCENT_LT` → `TH_TEXT`.
  - Lines 480, 511, 540 (focused drilldown 4px bars): `TH_ACCENT_LT` → `TH_TEXT`.

  `source/ui_library.c`:
  - Lines 48, 86, 121 (section headers): `TH_ACCENT` → `TH_TEXT`.
  - Line 126 (node status): `TH_ACCENT_LT` → `TH_TEXT`.

  `source/ui_queue.c`:
  - Line 91: backdrop scrim `th_color(0, 0, 0, 160)` → `TH_SCRIM`.
  - Line 94: drawer background `TH_SURFACE` → `TH_MODAL_BG`.
  - Line 98 (`"COLA DE REPRODUCCION //"`): `TH_ACCENT` → `TH_TEXT`.
  - Line 117 (empty-queue title): `TH_ACCENT_LT` → `TH_TEXT`.
  - Line 155 (selected 4px bar): `TH_ACCENT_LT` → `TH_TEXT`.
  - Line 167 (`is_selected` num_color): `TH_ACCENT_LT` → `TH_TEXT`.
  - Lines 158, 165, 167 (`is_current` playing track): keep `TH_ACCENT` / `TH_ACCENT_LT`.

- [ ] **Step 5: Verify build and playback-only violet audit**

  Run:
  ```bash
  rg -n "TH_ACCENT" source/ui_shell.c source/ui_home.c source/ui_search.c \
      source/ui_library.c source/ui_queue.c source/ui_components.c \
      source/ui_player_dock.c
  ```
  Expected: every remaining `TH_ACCENT` / `TH_ACCENT_LT` occurrence is tied to a live playback state (`is_playing`, `is_current`, EQ bars, scrubber, timeline).

  Run:
  ```bash
  rg -n "TH_ACTIVE_TEXT" source/ui_player_dock.c
  ```
  Expected: 0 matches.

  Run:
  ```bash
  powershell -ExecutionPolicy Bypass -File tools/run_in_docker.ps1 "make"
  ```
  Expected: 0 errors.

  Verify in RPCS3: RPCS3 screenshot of Dock with playing track confirms violet scrubber caps are visible (not black).

- [ ] **Step 6: Commit**

  ```bash
  git add source/ui_shell.c source/ui_player_dock.c source/ui_components.c \
      source/ui_home.c source/ui_search.c source/ui_library.c source/ui_queue.c
  git commit -m "feat(ui): reveal central background, bar translucency, playback-only violet across all modules"
  ```

---

### Task 6: Settings 4-Row Theme Selector & Visualizer 60% Background Dim

**Files:**
- Modify: `source/ui_settings.c:14,30-226`
- Modify: `source/ui_visualizer.c:23-26,38-42,78-107`

**Interfaces:**
- Consumes:
  - `BgThemeMode` / `BG_THEME_*` / `BG_THEME_COUNT` (`include/monochrome.h`, Task 2)
  - `g_app_state.bg_theme`, `app_state_save_config()` (`include/app_state.h`, Task 2)
  - `int gfx_set_background_theme(int theme_id);`, `void gfx_set_background_dim(float brightness);` (`include/gfx_engine.h`, Task 3)
  - `TH_SURFACE`, `TH_SURFACE_2`, `TH_BORDER`, `TH_BORDER_LT`, `TH_TEXT`, `TH_TEXT_DIM` (`include/ui_theme.h`, Task 4)
- Produces:
  - `ui_settings` with `s_sel` range `0..3`: Row 0 = `"Fondo de Interfaz: < %s >"` live-preview + persisted; Row 1 = Safe Area (unchanged); Row 2 = HTTP Timeout (unchanged); Row 3 = Reset Safe Area (unchanged)
  - `NETCTL` `IP OBTAINED` rendered in `TH_TEXT` (white)
  - `ui_visualizer_render()` enters fullscreen at `0.60f` brightness, restores `1.0f` on exit

- [ ] **Step 1: Expand `source/ui_settings.c` to 4 rows**

  `ui_settings_render()` (lines 30-145):
  - Line 40 (`"AJUSTES //"`): `TH_ACCENT` → `TH_TEXT`.
  - Insert Row 0 render block above Safe Area row: draw `"Fondo de Interfaz: < %s >"` using label array `{"Fractura HUD", "Planos Mate", "Cromo Liquido", "Negro Puro"}` indexed by `g_app_state.bg_theme` (clamped `0..BG_THEME_COUNT-1`). Use `TH_SURFACE_2` background, `TH_BORDER_LT` outline, `TH_TEXT` 4px left bar when `s_sel == 0`.
  - Shift Safe Area → Row 1, HTTP Timeout → Row 2, Reset Safe Area → Row 3. Update focus bars from `TH_ACCENT_LT` → `TH_TEXT`. Update Safe Area slider fill (line 72) from violet to `TH_TEXT` / `TH_TEXT_DIM`.
  - Line 110 (`"DIAGNOSTICO DEL SISTEMA & TELEMETRIA // MONOCHROME PS3"`): `TH_ACCENT` → `TH_TEXT`.
  - Line 124 (`NETCTL` / `NET_HAL_IP_OBTAINED`): `TH_ACCENT_LT` → `TH_TEXT`.

  `ui_settings_handle_input()` (lines 147-226):
  - `ACT_DOWN`: change bound from `s_sel < 2` to `s_sel < 3`.
  - Add `s_sel == 0` block: `ACT_LEFT` → `g_app_state.bg_theme = (g_app_state.bg_theme + BG_THEME_COUNT - 1) % BG_THEME_COUNT;`; `ACT_RIGHT` / `ACT_CROSS` → `g_app_state.bg_theme = (g_app_state.bg_theme + 1) % BG_THEME_COUNT;`; both paths call `gfx_set_background_theme((int)g_app_state.bg_theme);` then `app_state_save_config();`.
  - Move Safe Area, HTTP Timeout, Reset logic to `s_sel == 1`, `2`, `3` respectively.

- [ ] **Step 2: Update `source/ui_visualizer.c` for 60% background dim**

  `ui_visualizer_render()` (lines 78-86):
  - Remove `gfx_draw_rect(0.0f, 0.0f, (float)VIRT_W, (float)VIRT_H, TH_BG);` (line 85).
  - Add `gfx_set_background_dim(0.60f);` immediately after the `!g_app_state.visualizer_fullscreen` guard.

  `ui_visualizer_set_fullscreen()` (lines 23-26) and `ui_visualizer_handle_input()` close branch (lines 39-42):
  - Add `gfx_set_background_dim(1.0f);` when `visualizer_fullscreen` becomes `false`.

- [ ] **Step 3: Verify build and RPCS3 behavior**

  Run:
  ```bash
  powershell -ExecutionPolicy Bypass -File tools/run_in_docker.ps1 "make"
  ```
  Expected: 0 errors.

  Verify in RPCS3:
  - Row 0 cycles all 4 themes live with `< >` indicators; persists across app restart in `config.cfg`.
  - Rows 1–3 (Safe Area, HTTP Timeout, Reset) function identically to pre-change behavior.
  - `NETCTL` `IP OBTAINED` renders in white (`TH_TEXT`), not violet.
  - Fullscreen Visualizer shows 60% brightness background; restores 100% on close (no opaque black fill).

- [ ] **Step 4: Commit**

  ```bash
  git add source/ui_settings.c source/ui_visualizer.c
  git commit -m "feat(ui): 4-row theme selector in settings, 60% visualizer dim, remove non-playback violet"
  ```

---

### Task 7: Boxless Fade-In Splash Screen

**Files:**
- Modify: `source/ui_v3.c:58-101,223,249`

**Interfaces:**
- Consumes:
  - `void gfx_set_background_dim(float brightness);` (`include/gfx_engine.h`, Task 3)
  - `void platform_prepare_frame(void);`, `void platform_flip(void);`, `bool platform_should_exit(void);`, `uint64_t platform_get_time_usec(void);`, `InputAction platform_poll_input(void);` (`include/platform.h`)
  - `sysUtilCheckCallback()` (`<sysutil/sysutil.h>`)
  - `th_color()`, `TH_TEXT`, `TH_TEXT_DIM`, `TH_BORDER_LT`, `TH_ACCENT_LT`, `VIRT_W`, `VIRT_H` (`include/ui_theme.h`, Task 4)
  - `APP_DISPLAY_VERSION` (`include/monochrome.h`)
- Produces:
  - `void ui_render_splash(void);` — boxless fade-in over the active persisted background, respecting `sysUtilCheckCallback()` and button-skip; restores `gfx_set_background_dim(1.0f)` before returning

- [ ] **Step 1: Replace `ui_render_splash()` in `source/ui_v3.c:58-101`**

  Remove: the `700×400` `TH_SURFACE` box, `TH_ACCENT` 3px outline, and any `gfx_draw_rect(0, 0, sw, sh, TH_BG)` background fill.

  New implementation structure — loop `while (platform_get_time_usec() - t_start) < 2200000ULL`:
  1. `sysUtilCheckCallback();` — if `platform_should_exit()` set `s_exit_requested = true; break;`
  2. `float fade = fminf(1.0f, (float)(platform_get_time_usec() - t_start) / 1200000.0f);` and `uint8_t a = (uint8_t)(fade * 255.0f);`
  3. `gfx_set_background_dim(fade);` then `platform_prepare_frame();`
  4. Draw centered boxless layout (all using `th_color(r, g, b, a)` for the fade-in alpha):
     - `"MONOCHROME"` at size `40.0f`, color `th_color(0xFF, 0xFF, 0xFF, a)`, centered at `y = 430` (`x = (sw - 10 * 40) / 2`)
     - `1px` horizontal divider `420px` wide at `x = (sw - 420) / 2`, `y = 492`, color `th_color(0x88, 0x88, 0x88, a)`
     - Centered `64px × 2px` violet micro-accent over the divider: `x = (sw - 64) / 2`, `y = 492`, `th_color(0x9D, 0x4E, 0xDD, a)`
     - Subtitle `"PS3 // LOSSLESS FLAC"` at size `14.0f`, `th_color(0x9A, 0x9A, 0x9A, a)`, centered at `y = 514` (`x = (sw - 20 * 14) / 2`)
     - Footer `APP_DISPLAY_VERSION "  //  Pulsa cualquier boton para iniciar..."` at size `12.0f`, `th_color(0x88, 0x88, 0x88, a)`, centered at `y = 940`
  5. `platform_flip();` — poll `platform_poll_input()` and break if `!= ACT_NONE`; `usleep(16000);`
  
  After loop: `gfx_set_background_dim(1.0f);`

  Also in `source/ui_v3.c:223,249` (the `"SALIENDO AL XMB..."` exit overlay): change `TH_ACCENT` border to `TH_BORDER_LT` so `TH_ACCENT` remains 100% playback-only across the entire codebase.

- [ ] **Step 2: Verify build and RPCS3 splash behavior**

  Run:
  ```bash
  powershell -ExecutionPolicy Bypass -File tools/run_in_docker.ps1 "make clean && make pkg"
  ```
  Expected: 0 errors, `monochrome_ps3.pkg` (`MONO00024`) produced.

  Verify in RPCS3:
  - Boxless splash fades in smoothly over the persisted `bg_theme` background over ~1.2s.
  - `TTY.log` shows zero flip crashes during splash.
  - Any button press exits the splash immediately.
  - `ui_render()` enters at full `1.0f` background brightness after the splash.
  - Violet micro-accent segment is visible on the neutral grey divider.

- [ ] **Step 3: Commit**

  ```bash
  git add source/ui_v3.c
  git commit -m "feat(ui): boxless fade-in splash over active background, sysutil preserved, button-skip"
  ```

---

### Task 8: Final Integration Build & Verification

**Files:**
- No new files; validation only.

**Interfaces:**
- Consumes: all outputs from Tasks 1–7.
- Produces: verified `monochrome_ps3.pkg` (`MONO00024`) matching all spec verification criteria.

- [ ] **Step 1: Full clean build**

  ```bash
  powershell -ExecutionPolicy Bypass -File tools/run_in_docker.ps1 "make clean && make pkg"
  ```
  Expected: `0 errors`, `monochrome_ps3.pkg` produced.

  ```bash
  python tools/verify_pkg.py monochrome_ps3.pkg
  ```
  Expected: `[SUCCESS] All RPCS3 PKG checks passed 100%!`

- [ ] **Step 2: RPCS3 full acceptance test**

  Deploy via `tools/verify_runtime.ps1` and confirm all spec criteria:
  - Boxless fade-in splash renders with zero flip crashes.
  - All 4 background options (`Fractura HUD` default, `Planos Mate`, `Cromo Liquido`, `Negro Puro`) switch live in Settings Row 0, persist across restart via `config.cfg`.
  - Overscan adjustments (`75%–100%`) reflow background correctly on all modes.
  - Translucent Top Bar, Left Rail, Bottom Dock, Queue Drawer, dimmed Fullscreen Visualizer (60%) all show crisp `8×8` text.
  - `[PERF]` logs confirm ~60 FPS throughout.
  - `pkgfiles/PIC1.PNG` matches `Fractura & Micro-HUD` artwork at 1920×1080.
  - Violet appears only on live-playback signals; no opaque `#12141C` fills.
  - `NETCTL` `IP OBTAINED` renders in white.
  - `TTY.log`: zero `Access violation`, `PPU Trap`, `CELL_ENOENT`.

- [ ] **Step 3: Final commit**

  ```bash
  git tag v2.4.0-bg-system
  git commit --allow-empty -m "chore: integration verified — Monochrome PS3 background system & UI adaptation complete"
  ```
