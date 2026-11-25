# Design Specification: Monochrome PS3 Minimalist Pure-Black Background System & UI Adaptation

**Date:** 2026-11-24  
**Target Project:** `E:\DSH\Monochrome-PS3\monochrome-ps3-2.4.0` (`MONO00024` / PSL1GHT v2)  
**Status:** Ready for user approval (builder check resolved)

---

## 1. Intent & Why

Monochrome PS3's current UI (`ui_shell.c`, `ui_theme.h`) covers the entire 1920×1080 canvas in opaque blue-tinted slate (`#12141C`), completely hiding `#000000` (`TH_BG`), while `gfx_load_texture_png()` is a stub and `gfx_draw_rect()` ignores alpha.

The goal is to introduce a cohesive, minimalist pure-black (`#000000`) visual identity inspired by three Y2K/abstract sculptural references—using strict neutral grayscale and crisp white contrast—that serves both as an embedded background asset system and as an adapted translucent monochrome UI across the entire application and XMB.

---

## 2. Goals, Non-Goals & Anti-Goals

### Goals
- Provide **4 selectable background modes** in Settings (`ui_settings.c`), persisted in `/dev_hdd0/game/MONO00024/USRDIR/config.cfg` (`app_state.c`):
  1. **Fractura & Micro-HUD** (`BG_THEME_FRACTURE_HUD`, **default**) — dark smoky crystalline depth on `#000000` with restrained diagonal light wisps and ultra-thin technical vector hairlines.
  2. **Planos Esculturales Mate** (`BG_THEME_MATTE_SLABS`) — overlapping curved brutalist matte-metallic slabs with subtle film grain and white rim highlight (attenuated ~15% on mid-high luminance so right-aligned track durations remain crisp).
  3. **Curvas Líquidas** (`BG_THEME_LIQUID_CHROME`) — sleek obsidian/liquid-chrome folded ribbons with thin specular crests.
  4. **Negro Puro** (`BG_THEME_PURE_BLACK`) — zero-texture `#000000` void.
- Adapt the **UI palette & surfaces** (`ui_theme.h` and all UI modules) to pure black and neutral greys, reserving violet (`#7B2CBF` / `#9D4EDD`) **exclusively** for live playback signals (currently playing track, timeline/scrubber, EQ/spectrum bars, and a 1px splash divider accent).
- Enable **real translucent surfaces** and **alpha blending** at stable **60 FPS** on PS3 hardware / RPCS3 across 1080p, 720p, and 480p with overscan support.
- Replace the existing boxed splash screen (`ui_render_splash()` in `source/ui_v3.c`) with a **boxless fade-in minimalist splash**.
- Apply the active background consistently across main screens (`Home`, `Search`, `Library`, `Playlists`, `Instances`, `Settings`), overlays (`Queue`), `Fullscreen Visualizer` (dimmed), and update `pkgfiles/PIC1.PNG` for the PS3 XMB with the default Fractura & Micro-HUD artwork.

### Non-Goals
- Do not modify `source/audio_engine.c`, `include/audio_engine.h`, `source/http_client.c`, or `include/http_client.h`.
- Do not migrate the video backend to `libtiny3d` 3D shaders or rewrite `rsxutil.c` scanout initialization.
- Do not add animated shaders or video backgrounds; backgrounds are static high-precision 1920×1080 8-bit grayscale PNGs with dynamic brightness multipliers (for splash fade-in and visualizer dim).
- Do not alter the legacy neutralized scripts (`legacy_unused/`) or retired Title ID (`MCRM00001`).

### Anti-Goals
- **No RSX VRAM reads** during CPU alpha blending (FlexIO PPU->VRAM reads stall the bus and drop FPS).
- **No JPEG compression** for background assets (8×8 macroblocking/banding is highly visible in near-black gradients on `#000000`).
- **No opaque blue-slate fills (`#12141C`)** hiding the background canvas.
- **No unrequested clutter** on the left/upper-center negative space of the background textures that would compromise the 8×8 bitmap font readability.

---

## 3. Technical Constraints (Verified in Repository)

- **Build & Packaging:** Root `Makefile` builds `monochrome_ps3.elf` / `.self` / `.pkg` (`APPID := MONO00024`) via `tools/run_in_docker.ps1` (`ghcr.io/altps3/ps3dev:latest`). Files placed in `data/` are embedded into `.rodata` via `bin2o`, generating `<name>_png.h` headers in `build/` (e.g., `extern const u8 <name>_png[]; extern const u32 <name>_png_size;`).
- **Linked Libraries:** `-lpng -lz -ljpeg -lrsx -lgcm_sys` are already linked in `Makefile`.
- **Virtual vs Native Coordinates:** UI layouts target `VIRT_W x VIRT_H` (`1920x1080`) and scale via `vx()`, `vy()`, `vw()`, `vh()` (`s_scale_x`, `s_scale_y`, `s_offset_x`, `s_offset_y`) to native `s_screen_w x s_screen_h` (`getResolution`) and user overscan (`0.70..1.00`).
- **Framebuffer Format:** `rsxutil.c` configures `VIDEO_BUFFER_FORMAT_XRGB` (`X8R8G8B8`, 32-bit `0xAARRGGBB` Big-Endian on Cell PPU).
- **Memory Budget:** 256 MB XDR RAM holds 48 MB stream buffer + 4.5 MB PCM ring (`audio_engine.c`) + 32 MB RSX host command buffer (`HOST_SIZE`) + ~8 MB cover art cache. Embedded 8-bit grayscale PNGs for all 3 1920×1080 textures total ~835 KB in `.rodata`; keeping one baked native-res background buffer + one native-res XDR shadow framebuffer adds ~7 MB at 720p (~15.8 MB at 1080p), comfortably within budget.

---

## 4. Architectural Approaches Considered

1. **Approach A — Pre-Baked XDR Background + Cached XDR Shadow Framebuffer Compositing *(Picked)***
   - **How:** Embed the 3 calibrated 8-bit grayscale PNGs in `data/`. Decode only the active theme via `libpng` in memory (`png_set_read_fn`) and bilinearly pre-bake it once at startup (or on theme/overscan change) into a native-resolution XDR buffer (`s_baked_bg_xdr`), freeing the temporary 1920×1080 decoded buffer immediately. All frame rendering (`gfx_begin_frame` background copy/dim + `gfx_draw_rect` alpha blending + `blit_rgba` alpha blending + text/lines) operates inside a cached XDR shadow framebuffer (`s_shadow_fb`), and `gfx_end_frame()` flushes `s_shadow_fb` to RSX VRAM (`b->ptr`) in a single sequential 64-bit aligned burst before `flip()`.
   - **Trade-offs:** Nested translucent layers (e.g., `Background -> Hero -> Badges` and `Main UI -> Queue Scrim`) composite accurately with zero VRAM reads and minimal CPU overhead (~60 FPS preserved). Uses ~7–16 MB XDR RAM depending on output resolution.
   - **Why picked:** Zero risk to RSX/GameOS scanout stability (`rsxutil.c`), honors `gfx_set_overscan()` identically across 480p/720p/1080p, and eliminates FlexIO bus stalls.

2. **Approach B — Full RSX / Tiny3D Hardware Textured Quads *(Rejected)***
   - **Trade-offs:** Hardware blending, but `libtiny3d` and compiled RSX shaders are not present in the repository/toolchain setup, and altering the RSX command/flip pipeline carries high regression risk (black screens / HLE vs real PS3 mismatches).

3. **Approach C — Runtime PNG Loading from `/dev_hdd0/game/MONO00024/USRDIR/` *(Rejected)***
   - **Trade-offs:** Saves ~835 KB in `EBOOT.BIN`, but introduces disk I/O latency on theme switches and packaging fragility compared to `Makefile`'s native `bin2o` `data/` embedding.

---

## 5. Detailed Design

### 5.1 Asset Pipeline (`assets_raw/`, `data/`, `pkgfiles/PIC1.PNG`, `tools/pack_ui_assets.py`)
- Stage the 3 approved 1920×1080 source textures generated during design exploration (`raw_3_fracture_hud.png`, `raw_1_supermodified.png`, `raw_2_liquid_chrome.png` from the session artifact directory) into `assets_raw/` as canonical master PNGs:
  - `assets_raw/bg_fracture_hud.png`
  - `assets_raw/bg_matte_slabs.png`
  - `assets_raw/bg_liquid_chrome.png`
- Extend `tools/pack_ui_assets.py` so it converts those three `assets_raw/bg_*.png` masters into calibrated **8-bit grayscale (`L`) PNGs** with true `#000000` black floor (and ~15% highlight attenuation on `bg_matte_slabs.png` for optimal right-edge track duration legibility):
  - `data/bg_fracture_hud.png` (~202 KB)
  - `data/bg_matte_slabs.png` (~364 KB)
  - `data/bg_liquid_chrome.png` (~269 KB)
- Also export `assets_raw/bg_fracture_hud.png` as a 1920×1080 RGB PNG to `pkgfiles/PIC1.PNG` (PS3 XMB wallpaper).
- Keep existing `data/ui_*.png` 1-bit sprite assets intact.

### 5.2 Application State & Persistence (`include/monochrome.h`, `include/app_state.h`, `source/app_state.c`)
- Define `BgThemeMode` in `include/monochrome.h`:
  ```c
  typedef enum {
      BG_THEME_FRACTURE_HUD  = 0, /* Default */
      BG_THEME_MATTE_SLABS   = 1,
      BG_THEME_LIQUID_CHROME = 2,
      BG_THEME_PURE_BLACK    = 3,
      BG_THEME_COUNT         = 4
  } BgThemeMode;
  ```
- Add `BgThemeMode bg_theme;` to `AppContext` (`include/app_state.h`).
- In `app_state_init()`, set `g_app_state.bg_theme = BG_THEME_FRACTURE_HUD;`.
- In `app_state_load_config()` and `app_state_save_config()`, read/write `bg_theme=%d` in `/dev_hdd0/game/MONO00024/USRDIR/config.cfg` (clamping invalid values to `BG_THEME_FRACTURE_HUD`).
- In `main.c`, immediately after `app_state_load_config()` and screen overscan initialization, apply `gfx_set_background_theme((int)g_app_state.bg_theme);` (or call inside `ui_init()` / `main.c` right before `ui_render_splash()` so the splash uses the persisted theme).

### 5.3 Graphics Engine (`include/gfx_engine.h`, `source/gfx_tiny3d.c`)
- **New public APIs in `include/gfx_engine.h`:**
  - `int gfx_set_background_theme(int theme_id);`
  - `void gfx_set_background_dim(float brightness);` (`0.0f` = pure black, `1.0f` = full texture brightness)
- **XDR Shadow Framebuffer (`s_shadow_fb`):**
  - Allocated via `memalign(64, (size_t)s_screen_w * s_screen_h * 4)` in `gfx_init()`, freed in `gfx_shutdown()`.
  - All drawing primitives (`gfx_begin_frame`, `gfx_draw_rect`, `gfx_draw_rect_outline`, `blit_rgba`, `draw_glyph_box`, `draw_line_sw`) target `s_shadow_fb` instead of writing scattered 32-bit values directly to `b->ptr`.
  - `gfx_end_frame()` copies `s_shadow_fb` to `s_buffers[s_cur_buffer].ptr` using an unrolled 64-bit copy loop, then calls `flip()` and `waitFlip()`.
- **Embedded PNG Decoding & Pre-Baking:**
  - Implement real PNG decoding via `libpng` (`png_create_read_struct`, `png_set_read_fn` reading from memory buffer) for both embedded background PNGs and `gfx_load_texture_png()` (converting `GRAY` / `GRAY_ALPHA` / `RGB` / `RGBA` to 8-bit RGBA/XRGB).
  - `bake_background()`:
    - If `s_bg_theme == BG_THEME_PURE_BLACK`, free or zero `s_baked_bg_xdr` (no decode needed).
    - Otherwise, select the matching `bin2o` buffer (`bg_fracture_hud_png`, `bg_matte_slabs_png`, or `bg_liquid_chrome_png`), decode to temporary 1920×1080 buffer, bilinearly sample into `s_baked_bg_xdr` (`s_screen_w x s_screen_h` `uint32_t` `0xFFRRGGBB`) mapping each native pixel `(px, py)` back through the inverse overscan transform (`vx_inv = (px - s_offset_x) / s_scale_x`, `vy_inv = (py - s_offset_y) / s_scale_y`; pixels outside `[0..1920) x [0..1080)` are filled `#000000`), and immediately `free()` the temporary 1920×1080 decoded buffer.
    - Re-run `bake_background()` whenever `gfx_set_background_theme()` or `gfx_set_overscan()` changes parameters.
- **Frame Begin & Alpha Blending:**
  - `gfx_begin_frame(uint32_t clear_rgba)`:
    - If `s_bg_theme == BG_THEME_PURE_BLACK` or `s_baked_bg_xdr == NULL` or `s_bg_brightness <= 0.001f`, fill `s_shadow_fb` with `clear_rgba` (or `0xFF000000`) in 64-bit bursts.
    - If `s_bg_brightness >= 0.999f`, `memcpy` / 64-bit copy `s_baked_bg_xdr` -> `s_shadow_fb`.
    - Otherwise, scale RGB channels by fixed-point integer `scale = (uint32_t)(s_bg_brightness * 256.0f)` into `s_shadow_fb`.
  - `gfx_draw_rect()`:
    - `alpha == 255`: fast 64-bit solid fill on `s_shadow_fb`.
    - `alpha == 0`: no-op.
    - `1 <= alpha <= 254`: integer blend per pixel (`inv = 255 - a`, `out_c = (src_c * a + dst_c * inv + 127) / 255` with `out_a = 0xFF`) directly in `s_shadow_fb`.
  - `blit_rgba()`: update to properly alpha-blend (`s_a < 255`) against `s_shadow_fb` when `s_a > 0`.

### 5.4 Monochrome + Violet Micro-Accent Theme (`include/ui_theme.h`)
Replace the blue-slate tokens (`#12141C`, `#1A1E2A`, `#383D50`, `#4A5268`, `#8890A4`) with pure neutral monochrome tokens and translucent surfaces:
- `TH_BG`: `th_color(0x00, 0x00, 0x00, 0xFF)` (`#000000`)
- `TH_BAR_BG`: `th_color(0x00, 0x00, 0x00, 0xCC)` (Top Bar, Left Rail, Bottom Dock ~80% pure black)
- `TH_SURFACE`: `th_color(0x08, 0x08, 0x08, 0xA8)` (Hero, idle cards/rows, boxes ~66% translucent near-black)
- `TH_SURFACE_2`: `th_color(0x16, 0x16, 0x16, 0xC4)` (Focused rows, OSK keys, badges ~77% charcoal)
- `TH_MODAL_BG`: `th_color(0x06, 0x06, 0x06, 0xEA)` (Queue drawer ~92% dark)
- `TH_SCRIM`: `th_color(0x00, 0x00, 0x00, 0xA0)` (Queue backdrop scrim)
- `TH_BORDER`: `th_color(0x2E, 0x2E, 0x2E, 0xFF)` (1px neutral hairline)
- `TH_BORDER_LT`: `th_color(0x88, 0x88, 0x88, 0xFF)` (1px neutral silver focus border)
- `TH_TEXT`: `th_color(0xFF, 0xFF, 0xFF, 0xFF)` (`#FFFFFF`)
- `TH_TEXT_DIM`: `th_color(0x9A, 0x9A, 0x9A, 0xFF)` (Neutral silver secondary text)
- `TH_ACTIVE_BG`: `th_color(0xFF, 0xFF, 0xFF, 0xFF)` (Focused/active solid white pill)
- `TH_ACTIVE_TEXT`: `th_color(0x00, 0x00, 0x00, 0xFF)` (Pure black text on white active pill)
- `TH_ACCENT`: `th_color(0x7B, 0x2C, 0xBF, 0xFF)` (Violet — **playback-only** micro-accent)
- `TH_ACCENT_LT`: `th_color(0x9D, 0x4E, 0xDD, 0xFF)` (Light violet — **playback-only** highlight)

### 5.5 UI Adaptation Across Modules
- **`source/ui_shell.c`:**
  - `shell_render()`: Remove `gfx_draw_rect(x0, y0, mw, mh, TH_SURFACE);` so the active background shows through the entire central canvas. Ensure `gfx_set_background_dim(1.0f)` is active for normal shell rendering.
  - `render_top_bar()` & `render_left_rail()` & `source/ui_player_dock.c`: Draw backgrounds with `TH_BAR_BG` and `TH_BORDER`. Left rail active item uses `TH_ACTIVE_BG` (`#FFFFFF`), `TH_ACTIVE_TEXT` (`#000000`), and neutral/silver accent bar (`TH_BORDER_LT` instead of violet when merely navigating).
- **Playback-Only Violet Rule across `ui_home.c`, `ui_search.c`, `ui_library.c`, `ui_settings.c`, `ui_queue.c`, `ui_components.c`, `ui_player_dock.c`, `ui_visualizer.c`:**
  - Section headers (`DESTACADO //`, `BUSQUEDA //`, `BIBLIOTECA //`, `PLAYLISTS //`, `INSTANCIAS //`, `AJUSTES //`, `COLA DE REPRODUCCION //`) and idle UI chrome use `TH_TEXT` / `TH_TEXT_DIM` instead of `TH_ACCENT`.
  - Focused rows (`UI_COMPONENT_STATE_FOCUSED`, Search/Settings/Queue selection) use `TH_SURFACE_2`, `TH_BORDER_LT` outline, and `TH_TEXT` (`#FFFFFF`) 4px left indicator bar.
  - Playing track rows (`UI_COMPONENT_STATE_PLAYING`, active track in Queue), Hero `[ EN REPRODUCCION ]` badge & playing EQ bars, Dock timeline fill/scrubber/elapsed time/playing EQ bars, and Fullscreen Visualizer spectrum & progress bar retain `TH_ACCENT` / `TH_ACCENT_LT`.
  - Note: in `ui_player_dock.c` line 185, the scrubber outer cap previously used `TH_ACTIVE_TEXT` (which was violet `#7B2CBF`); update it to `TH_ACCENT` outer / `TH_ACCENT_LT` inner so the new `#000000` `TH_ACTIVE_TEXT` does not turn the scrubber cap black.
- **`source/ui_visualizer.c`:**
  - Remove the opaque `gfx_draw_rect(0, 0, VIRT_W, VIRT_H, TH_BG)` cover-up; instead call `gfx_set_background_dim(0.60f)` at the start of `ui_visualizer_render()` (so `gfx_begin_frame` already provides the dimmed active background) and restore `1.0f` when closing or in `shell_render()`. Use translucent `TH_SURFACE` for the spectrum container.
- **`source/ui_settings.c`:**
  - Preserve all 3 existing settings rows (`overscan_scale`, `http_timeout_ms`, and Reset Safe Area) plus the `248px` live `net_hal` telemetry box, expanding `s_sel` range from `0..2` to **`0..3`**:
    - Row 0 (`s_sel == 0`): `"Fondo de Interfaz: < Fractura HUD >"` cycling `[Fractura HUD | Planos Mate | Cromo Liquido | Negro Puro]` via `ACT_LEFT` / `ACT_RIGHT` / `ACT_CROSS`, live-calling `gfx_set_background_theme((int)g_app_state.bg_theme)` and persisting with `app_state_save_config()`.
    - Row 1 (`s_sel == 1`): `"Escalado de Zona Segura"` (unchanged behavior).
    - Row 2 (`s_sel == 2`): `"Timeout de Red (HTTP)"` (unchanged behavior).
    - Row 3 (`s_sel == 3`): `"Restablecer Zona Segura al 100% (Pantalla Completa)"` (unchanged behavior).
  - Keep `NETCTL` `IP OBTAINED` status in `TH_TEXT` (white) rather than `TH_ACCENT_LT` so violet remains playback-only.
- **New Boxless Fade-In Splash (`source/ui_v3.c`):**
  - Completely remove the old `700x400` `TH_SURFACE` box and `TH_ACCENT` border.
  - Preserve `sysUtilCheckCallback()` / `platform_should_exit()` polling inside the splash loop, and use the proper `platform_prepare_frame()` -> draw -> `platform_flip()` frame cycle (with `gfx_set_background_dim(fade)` smoothly interpolating `0.0f -> 1.0f` over ~1.2s, holding through ~2.2s total or exiting immediately on any button press, then restoring `gfx_set_background_dim(1.0f)`).
  - Centered layout over the fading active background:
    - Large centered `"MONOCHROME"` (`40.0f` size, `TH_TEXT` `#FFFFFF`, fading/revealed cleanly).
    - Centered `1px` horizontal neutral grey divider (`TH_BORDER_LT`, `420px` wide) with a centered `64px x 2px` violet (`TH_ACCENT_LT`) micro-accent segment.
    - Centered subtitle `"PS3 // LOSSLESS FLAC"` (`14.0f`, `TH_TEXT_DIM`).
    - Subtle footer near the bottom (`y = 940`): `APP_DISPLAY_VERSION "  //  Pulsa cualquier boton para iniciar..."` (`12.0f`, `TH_BORDER_LT`).

### 5.6 What Is Left to the Builder
- Exact alpha values (`0xA0..0xCC`) and brightness attenuation percentage for `bg_matte_slabs.png` can be fine-tuned visually after capturing RPCS3 screenshots if any text legibility spot needs adjustment.
- Exact `libpng` memory-reader callback implementation details and `s_shadow_fb` 64-bit aligned copy optimization inside `gfx_tiny3d.c`.
- Exact duration/easing curve for the splash fade-in (`~1.2s` ramp + hold).

---

## 6. Verification Criteria
- Clean Docker build (`tools/run_in_docker.ps1` -> `make clean && make pkg`) succeeds with `0 errors` and produces `monochrome_ps3.pkg` (`MONO00024`).
- RPCS3 / PS3 verification:
  - New boxless fade-in splash renders cleanly with zero flip crashes.
  - All 4 background options (`Fractura HUD` default, `Planos Mate`, `Cromo Liquido`, `Negro Puro`) switch live in Settings, persist across restart via `config.cfg`, and honor overscan adjustments (`75%–100%`).
  - Translucent Top Bar, Left Rail, Hero, Track Rows, Search Keyboard, Queue scrim, Bottom Dock, and dimmed Fullscreen Visualizer maintain crisp 8×8 text readability and ~60 FPS (`[PERF]` logs).
  - `pkgfiles/PIC1.PNG` is 1920×1080 valid PNG matching `Fractura & Micro-HUD`.
