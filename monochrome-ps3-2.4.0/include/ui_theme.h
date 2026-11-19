#ifndef UI_THEME_H
#define UI_THEME_H

#include <stdint.h>

/* ── Paleta Monocroma + Acento Violeta ───────────────────────────── */
#define COLOR_BG          0x00000000   /* #000000 */
#define COLOR_SURFACE     0x00141824   /* gris oscuro superficie */
#define COLOR_SURFACE_2   0x001C2436   /* superficie ligeramente más clara */
#define COLOR_BORDER      0x004A4A4A   /* gris borde */
#define COLOR_TEXT        0x00FFFFFF   /* #FFFFFF */
#define COLOR_TEXT_DIM    0x00666666   /* #666666 */
#define COLOR_ACCENT      0x007B2CBF   /* #7B2CBF violeta */
#define COLOR_ACCENT_LT   0x008A2BE2   /* #8A2BE2 violeta claro */
#define COLOR_ACTIVE_BG   0x00FFFFFF   /* bloque blanco para ítem activo */
#define COLOR_ACTIVE_TEXT 0x007B2CBF   /* texto violeta sobre blanco */

/* ── Medidas virtuales 1920x1080 ─────────────────────────────────── */
#define VIRT_W 1920
#define VIRT_H 1080

#define TOP_BAR_H        64
#define LEFT_RAIL_COLLAPSED_W   80
#define LEFT_RAIL_EXPANDED_W   260
#define BOTTOM_DOCK_H     96
#define MAIN_AREA_Y0      TOP_BAR_H      /* 64 */
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
#define TH_SURFACE     th_color(0x14, 0x18, 0x24, 0xFF)
#define TH_SURFACE_2   th_color(0x1C, 0x24, 0x36, 0xFF)
#define TH_BORDER      th_color(0x4A, 0x4A, 0x4A, 0xFF)
#define TH_TEXT        th_color(0xFF, 0xFF, 0xFF, 0xFF)
#define TH_TEXT_DIM    th_color(0x66, 0x66, 0x66, 0xFF)
#define TH_ACCENT      th_color(0x7B, 0x2C, 0xBF, 0xFF)
#define TH_ACCENT_LT   th_color(0x8A, 0x2B, 0xE2, 0xFF)
#define TH_ACTIVE_BG   th_color(0xFF, 0xFF, 0xFF, 0xFF)
#define TH_ACTIVE_TEXT th_color(0x7B, 0x2C, 0xBF, 0xFF)

#endif /* UI_THEME_H */
