#include "ui_settings.h"
#include <stdio.h>
#include <string.h>

#include "monochrome.h"
#include "app_state.h"
#include "gfx_engine.h"
#include "ui_theme.h"
#include "cover_art.h"

static int s_sel = 0;

void ui_settings_init(void) { s_sel = 0; }

void ui_settings_render(void) {
    int x0 = 260, y0 = 64, x1 = VIRT_W, y1 = VIRT_H - 96;
    int mw = x1 - x0;
    int cx = x0 + 30;
    int cy = y0 + 20;

    gfx_draw_text(cx, cy, "AJUSTES", 18.0f, TH_TEXT);
    cy += 40;

    /* Cover Art Mode */
    const char *mode_str = (g_app_state.cover_mode == COVER_MODE_1BIT_DITHER) ? "1-BIT B/W DITHERED" : "ORIGINAL COLOR";
    char line[256];
    if (s_sel == 0) gfx_draw_rect(cx - 8, cy - 4, mw - 60, 28, TH_ACTIVE_BG);
    snprintf(line, sizeof(line), "Cover Art Mode: %s", mode_str);
    gfx_draw_text(cx, cy, line, 14.0f, s_sel==0 ? TH_ACTIVE_TEXT : TH_TEXT);
    cy += 36;

    /* Overscan / Safe Area */
    if (s_sel == 1) gfx_draw_rect(cx - 8, cy - 4, mw - 60, 28, TH_ACTIVE_BG);
    snprintf(line, sizeof(line), "Overscan: X=%d Y=%d Scale=%.2f", g_app_state.overscan_x, g_app_state.overscan_y, g_app_state.overscan_scale);
    gfx_draw_text(cx, cy, line, 14.0f, s_sel==1 ? TH_ACTIVE_TEXT : TH_TEXT);
    cy += 36;

    /* Info de sistema (solo lectura) */
    gfx_draw_text(cx, cy, "Info de sistema (solo lectura):", 14.0f, TH_TEXT_DIM);
    cy += 24;
    snprintf(line, sizeof(line), "Build SHA: %s", APP_BUILD_SHA);
    gfx_draw_text(cx, cy, line, 12.0f, TH_TEXT_DIM); cy += 20;
    snprintf(line, sizeof(line), "Build Time: %s", APP_BUILD_TIME);
    gfx_draw_text(cx, cy, line, 12.0f, TH_TEXT_DIM); cy += 20;
    snprintf(line, sizeof(line), "Title ID: MONO00024");
    gfx_draw_text(cx, cy, line, 12.0f, TH_TEXT_DIM); cy += 20;
    snprintf(line, sizeof(line), "HTTP Timeout: %u ms", g_app_state.http_timeout_ms);
    gfx_draw_text(cx, cy, line, 12.0f, TH_TEXT_DIM); cy += 20;

    gfx_draw_text(cx, cy + 20, "Up/Down navegar - Cross toggle modo", 10.0f, TH_TEXT_DIM);
}

void ui_settings_handle_input(InputAction act) {
    switch (act) {
        case ACT_UP:   if (s_sel > 0) s_sel--; break;
        case ACT_DOWN: if (s_sel < 1) s_sel++; break;
        case ACT_CROSS:
        case ACT_LEFT:
        case ACT_RIGHT: {
            if (s_sel == 0) {
                g_app_state.cover_mode = (g_app_state.cover_mode == COVER_MODE_1BIT_DITHER)
                    ? COVER_MODE_ORIGINAL : COVER_MODE_1BIT_DITHER;
                cover_art_set_mode(g_app_state.cover_mode);
                cover_art_clear_all();
            }
            break;
        }
        default: break;
    }
}
