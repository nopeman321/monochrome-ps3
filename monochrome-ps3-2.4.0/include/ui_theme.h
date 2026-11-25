#ifndef UI_THEME_H
#define UI_THEME_H

#include <stdint.h>

/* ── Paleta Monocroma + Acento Violeta ───────────────────────────── */
#define COLOR_BG          0x00000000   /* #000000 */
#define COLOR_SURFACE     0x0012141C   /* gris oscuro superficie */
#define COLOR_SURFACE_2   0x001A1E2A   /* superficie ligeramente más clara */
#define COLOR_BORDER      0x00383D50   /* gris borde */
#define COLOR_BORDER_LT   0x004A5268   /* borde resaltado */
#define COLOR_TEXT        0x00FFFFFF   /* #FFFFFF */
#define COLOR_TEXT_DIM    0x008890A4   /* plata secundario legible */
#define COLOR_ACCENT      0x007B2CBF   /* #7B2CBF violeta */
#define COLOR_ACCENT_LT   0x009D4EDD   /* #9D4EDD violeta claro / lilac */
#define COLOR_ACTIVE_BG   0x00FFFFFF   /* bloque blanco para ítem activo */
#define COLOR_ACTIVE_TEXT 0x007B2CBF   /* texto violeta sobre blanco */

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
#define TH_SURFACE     th_color(0x12, 0x14, 0x1C, 0xFF)
#define TH_SURFACE_2   th_color(0x1A, 0x1E, 0x2A, 0xFF)
#define TH_BORDER      th_color(0x38, 0x3D, 0x50, 0xFF)
#define TH_BORDER_LT   th_color(0x4A, 0x52, 0x68, 0xFF)
#define TH_TEXT        th_color(0xFF, 0xFF, 0xFF, 0xFF)
#define TH_TEXT_DIM    th_color(0x88, 0x90, 0xA4, 0xFF)
#define TH_ACCENT      th_color(0x7B, 0x2C, 0xBF, 0xFF)
#define TH_ACCENT_LT   th_color(0x9D, 0x4E, 0xDD, 0xFF)
#define TH_ACTIVE_BG   th_color(0xFF, 0xFF, 0xFF, 0xFF)
#define TH_ACTIVE_TEXT th_color(0x7B, 0x2C, 0xBF, 0xFF)

#endif /* UI_THEME_H */
