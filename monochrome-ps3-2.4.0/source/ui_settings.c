#include "ui_settings.h"
#include <stdio.h>
#include <string.h>

#include "monochrome.h"
#include "app_state.h"
#include "gfx_engine.h"
#include "ui_theme.h"
#include "ui_shell.h"
#include "cover_art.h"

static int s_sel = 0;

void ui_settings_init(void) {
    s_sel = 0;
}

void ui_settings_render(void) {
    int x0, y0, x1, y1;
    shell_get_main_area(&x0, &y0, &x1, &y1);
    int mw = x1 - x0;
    int mh = y1 - y0;
    (void)mh;
    int cx = x0 + 24;
    int cy = y0 + 16;
    int content_w = mw - 48;

    gfx_draw_text(cx, cy, "AJUSTES //", 15.0f, TH_ACCENT);
    cy += 36;

    /* Cover Art Mode Row */
    const char *mode_str = (g_app_state.cover_mode == COVER_MODE_1BIT_DITHER) ? "1-BIT B/W DITHERED" : "ORIGINAL COLOR";
    int row_h = 44;

    bool foc0 = (s_sel == 0);
    gfx_draw_rect(cx, cy, content_w, row_h, foc0 ? TH_SURFACE_2 : TH_SURFACE);
    if (foc0) {
        gfx_draw_rect_outline(cx, cy, content_w, row_h, TH_BORDER_LT, 1.0f);
        gfx_draw_rect(cx, cy, 4, row_h, TH_ACCENT_LT);
    } else {
        gfx_draw_rect(cx, cy + row_h - 1, content_w, 1, TH_BORDER);
    }
    char line[256];
    snprintf(line, sizeof(line), "Modo de caratula: %s", mode_str);
    gfx_draw_text(cx + 16, cy + 14, line, 14.0f, foc0 ? TH_TEXT : TH_TEXT_DIM);
    cy += row_h + 10;

    /* Overscan / Safe Area Row */
    bool foc1 = (s_sel == 1);
    gfx_draw_rect(cx, cy, content_w, row_h, foc1 ? TH_SURFACE_2 : TH_SURFACE);
    if (foc1) {
        gfx_draw_rect_outline(cx, cy, content_w, row_h, TH_BORDER_LT, 1.0f);
        gfx_draw_rect(cx, cy, 4, row_h, TH_ACCENT_LT);
    } else {
        gfx_draw_rect(cx, cy + row_h - 1, content_w, 1, TH_BORDER);
    }
    snprintf(line, sizeof(line), "Calibracion Overscan: X=%d Y=%d Scale=%.2f", g_app_state.overscan_x, g_app_state.overscan_y, g_app_state.overscan_scale);
    gfx_draw_text(cx + 16, cy + 14, line, 14.0f, foc1 ? TH_TEXT : TH_TEXT_DIM);
    cy += row_h + 24;

    /* Info de sistema (box estructurado) */
    int box_h = 160;
    gfx_draw_rect(cx, cy, content_w, box_h, TH_SURFACE);
    gfx_draw_rect_outline(cx, cy, content_w, box_h, TH_BORDER, 1.0f);

    gfx_draw_text(cx + 20, cy + 14, "DIAGNOSTICO DEL SISTEMA // MONOCHROME PS3", 13.0f, TH_ACCENT);

    int sy = cy + 42;
    snprintf(line, sizeof(line), "BUILD SHA:      %s", APP_BUILD_SHA);
    gfx_draw_text(cx + 20, sy, line, 12.0f, TH_TEXT_DIM); sy += 22;

    snprintf(line, sizeof(line), "COMPILACION:    %s", APP_BUILD_TIME);
    gfx_draw_text(cx + 20, sy, line, 12.0f, TH_TEXT_DIM); sy += 22;

    snprintf(line, sizeof(line), "TITLE ID:       MONO00024 (CONTENT ID: UP0001-MONO00024_00)");
    gfx_draw_text(cx + 20, sy, line, 12.0f, TH_TEXT_DIM); sy += 22;

    snprintf(line, sizeof(line), "HTTP TIMEOUT:   %u ms  |  SERVIDOR: tracks.monochrome.st:80", g_app_state.http_timeout_ms);
    gfx_draw_text(cx + 20, sy, line, 12.0f, TH_TEXT_DIM);

    gfx_draw_text(cx, y1 - 32, "[D-PAD] NAVEGAR   [X] CAMBIAR MODO   [L1/R1] PESTAÑAS", 12.0f, TH_TEXT_DIM);
}

void ui_settings_handle_input(InputAction act) {
    switch (act) {
        case ACT_UP:
            if (s_sel > 0) s_sel--;
            break;
        case ACT_DOWN:
            if (s_sel < 1) s_sel++;
            break;
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
