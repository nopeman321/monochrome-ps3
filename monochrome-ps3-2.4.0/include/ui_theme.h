#ifndef UI_THEME_H
#define UI_THEME_H

#include <stdint.h>

/* ── Neutral Monochrome Palette + Playback-Only Violet Accent ────── */
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
#define COLOR_ACCENT      0x007B2CBF   /* #7B2CBF violet (playback-only) */
#define COLOR_ACCENT_LT   0x009D4EDD   /* #9D4EDD light violet (playback-only) */

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
#define TH_ACCENT      th_color(0x7B, 0x2C, 0xBF, 0xFF) /* playback-only */
#define TH_ACCENT_LT   th_color(0x9D, 0x4E, 0xDD, 0xFF) /* playback-only */

#endif /* UI_THEME_H */
