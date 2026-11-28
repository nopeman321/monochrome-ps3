#ifndef UI_THEME_H
#define UI_THEME_H

#include <stdint.h>
#include <math.h>
#include "monochrome.h"
#include "platform.h"

/* ── Neutral Monochrome Palette + Selectable 16-Color Accent ─────── */
#define COLOR_BG          0x00000000   /* #000000 pure black */
#define COLOR_BAR_BG      0x00000000   /* #000000 translucent bar backdrop */
#define COLOR_SURFACE     0x00080808   /* #080808 dark neutral surface */
#define COLOR_SURFACE_2   0x00161616   /* #161616 elevated neutral surface */
#define COLOR_MODAL_BG    0x00060606   /* #060606 translucent modal panel */
#define COLOR_SCRIM       0x00000000   /* #000000 modal backdrop scrim */
#define COLOR_BORDER      0x002E2E2E   /* #2E2E2E subtle hairline border */
#define COLOR_BORDER_LT   0x00888888   /* #888888 highlighted border */
#define COLOR_TEXT        0x00FFFFFF   /* #FFFFFF primary text */
#define COLOR_TEXT_DIM    0x009A9A9A   /* #9A9A9A secondary dim text */
#define COLOR_ACTIVE_BG   0x00FFFFFF   /* #FFFFFF solid white active block */
#define COLOR_ACTIVE_TEXT 0x00000000   /* #000000 black text on white active block */

/* ── Medidas virtuales 1920x1080 ─────────────────────────────────── */
#define VIRT_W 1920
#define VIRT_H 1080

#define TOP_BAR_H        68
#define LEFT_RAIL_COLLAPSED_W   80
#define LEFT_RAIL_EXPANDED_W   260
#define BOTTOM_DOCK_H     96
#define MAIN_AREA_Y0      TOP_BAR_H      /* 68 */
#define MAIN_AREA_Y1      (VIRT_H - BOTTOM_DOCK_H)  /* 984 */

/* ── Ítems del Left Nav Rail ─────────────────────────────────────── */
#define NAV_ITEM_COUNT 6
typedef enum {
    NAV_ITEM_SEARCH = 0,
    NAV_ITEM_HOME,
    NAV_ITEM_LIBRARY,
    NAV_ITEM_PLAYLISTS,
    NAV_ITEM_INSTANCES,
    NAV_ITEM_SETTINGS
} NavItem;

/* ── Top Filter Tabs ─────────────────────────────────────────────── */
#define TOP_TAB_COUNT 5
typedef enum {
    TOP_TAB_ALL = 0,
    TOP_TAB_ALBUMS,
    TOP_TAB_ARTISTS,
    TOP_TAB_PLAYLISTS,
    TOP_TAB_TRACKS
} TopTab;

/* ── Colores en formato uint32_t RGBA para gfx ───────────────────── */
static inline uint32_t th_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

#define TH_BG          th_color(0x00, 0x00, 0x00, 0xFF)
#define TH_BAR_BG      th_color(0x00, 0x00, 0x00, 0xCC)
#define TH_SURFACE     th_color(0x08, 0x08, 0x08, 0xA8)
#define TH_SURFACE_2   th_color(0x16, 0x16, 0x16, 0xC4)
#define TH_MODAL_BG    th_color(0x06, 0x06, 0x06, 0xEA)
#define TH_SCRIM       th_color(0x00, 0x00, 0x00, 0xA0)
#define TH_BORDER      th_color(0x2E, 0x2E, 0x2E, 0xFF)
#define TH_BORDER_LT   th_color(0x88, 0x88, 0x88, 0xFF)
#define TH_TEXT        th_color(0xFF, 0xFF, 0xFF, 0xFF)
#define TH_TEXT_DIM    th_color(0x9A, 0x9A, 0x9A, 0xFF)
#define TH_ACTIVE_BG   th_color(0xFF, 0xFF, 0xFF, 0xFF)
#define TH_ACTIVE_TEXT th_color(0x00, 0x00, 0x00, 0xFF)

uint32_t    ui_theme_accent(void);
uint32_t    ui_theme_accent_lt(void);
const char *ui_theme_accent_name(int idx);
uint32_t    ui_theme_accent_color(int idx);
float       ui_theme_font_scale(int idx);
const char *ui_theme_font_label(int idx);

#define TH_ACCENT      ui_theme_accent()
#define TH_ACCENT_LT   ui_theme_accent_lt()

/* ── Smooth frame-rate-independent animation helpers ─────────────── */
static inline float ui_anim_dt(uint64_t *last_usec)
{
    uint64_t now = platform_get_time_usec();
    if (*last_usec == 0ULL) {
        *last_usec = now;
        return 1.0f / 60.0f;
    }
    uint64_t diff = (now >= *last_usec) ? (now - *last_usec) : 0ULL;
    *last_usec = now;
    float dt = (float)diff / 1000000.0f;
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.05f)  dt = 0.05f;
    return dt;
}

static inline float ui_anim_exp_lerp(float current, float target, float speed, float dt, float eps)
{
    float diff = target - current;
    if (fabsf(diff) <= eps) {
        return target;
    }
    return current + diff * (1.0f - expf(-speed * dt));
}

#endif /* UI_THEME_H */
