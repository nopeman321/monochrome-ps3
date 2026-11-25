#include "ui_queue.h"
#include <stdio.h>
#include <string.h>

#include "monochrome.h"
#include "app_state.h"
#include "gfx_engine.h"
#include "ui_theme.h"
#include "cover_art.h"
#include "audio_engine.h"

static int s_queue_cursor = 0;

void ui_queue_init(void)
{
    s_queue_cursor = 0;
}

bool ui_queue_is_open(void)
{
    return g_app_state.right_drawer_open;
}

void ui_queue_set_open(bool open)
{
    g_app_state.right_drawer_open = open;
}

bool ui_queue_handle_input(Action act)
{
    if (!g_app_state.right_drawer_open) {
        if (act == ACT_TRIANGLE) {
            g_app_state.right_drawer_open = true;
            return true;
        }
        return false;
    }

    /* Modal abierto: cerrable con ACT_TRIANGLE o ACT_CIRCLE */
    if (act == ACT_TRIANGLE || act == ACT_CIRCLE) {
        g_app_state.right_drawer_open = false;
        return true;
    }

    /* Navegacion vertical con D-Pad Up / Down */
    if (act == ACT_UP) {
        if (s_queue_cursor > 0) {
            s_queue_cursor--;
        }
        return true;
    }

    if (act == ACT_DOWN) {
        if (s_queue_cursor + 1 < (int)g_app_state.queue_count) {
            s_queue_cursor++;
        }
        return true;
    }

    /* Reproducir pista seleccionada con ACT_CROSS */
    if (act == ACT_CROSS) {
        if (g_app_state.queue_count > 0 &&
            s_queue_cursor >= 0 &&
            s_queue_cursor < (int)g_app_state.queue_count) {
            AppTrack *t = &g_app_state.queue[s_queue_cursor];
            audio_engine_play_track(t->id, t->title);
            memcpy(&g_app_state.current_track, t, sizeof(AppTrack));
            g_app_state.has_current_track = true;
            g_app_state.queue_index = (uint32_t)s_queue_cursor;
            if (t->cover_url[0]) {
                cover_art_load(t->cover_url, g_app_state.cover_mode);
            }
        }
        return true;
    }

    return true;
}

void ui_queue_render(void)
{
    if (!g_app_state.right_drawer_open) {
        return;
    }

    float panel_w = 560.0f;
    float panel_h = (float)VIRT_H;
    float panel_x = (float)VIRT_W - panel_w;

    /* Semi-transparent scrim over main area */
    gfx_draw_rect(0.0f, 0.0f, panel_x, panel_h, TH_SCRIM);

    /* Right side drawer */
    gfx_draw_rect(panel_x, 0.0f, panel_w, panel_h, TH_MODAL_BG);
    gfx_draw_rect(panel_x, 0.0f, 2.0f, panel_h, TH_BORDER);

    /* Header */
    gfx_draw_text(panel_x + 24.0f, 36.0f, "COLA DE REPRODUCCION //", 16.0f, TH_TEXT);

    if (g_app_state.queue_count == 0) {
        gfx_draw_text(panel_x + 24.0f, 64.0f, "0 pistas en lista", 13.0f, TH_TEXT_DIM);
    } else {
        char cnt_str[32];
        snprintf(cnt_str, sizeof(cnt_str), "%u pistas preparadas", (unsigned int)g_app_state.queue_count);
        gfx_draw_text(panel_x + 24.0f, 64.0f, cnt_str, 13.0f, TH_TEXT_DIM);
    }

    gfx_draw_rect(panel_x + 24.0f, 90.0f, panel_w - 48.0f, 1.0f, TH_BORDER);

    /* Si la cola esta vacia: recuadro estructurado */
    if (g_app_state.queue_count == 0) {
        float box_x = panel_x + 24.0f;
        float box_y = 120.0f;
        float box_w = panel_w - 48.0f;
        gfx_draw_rect(box_x, box_y, box_w, 130.0f, TH_SURFACE_2);
        gfx_draw_rect_outline(box_x, box_y, box_w, 130.0f, TH_BORDER, 1.0f);
        gfx_draw_text(box_x + 20.0f, box_y + 24.0f, "[ COLA DE REPRODUCCION VACIA ]", 15.0f, TH_TEXT);
        gfx_draw_text(box_x + 20.0f, box_y + 54.0f, "No hay pistas en la cola de reproduccion.", 13.0f, TH_TEXT_DIM);
        gfx_draw_text(box_x + 20.0f, box_y + 78.0f, "Agrega pistas pulsando X desde Inicio o Busqueda.", 13.0f, TH_TEXT_DIM);
    } else {
        if (s_queue_cursor >= (int)g_app_state.queue_count) {
            s_queue_cursor = (int)g_app_state.queue_count - 1;
        }
        if (s_queue_cursor < 0) {
            s_queue_cursor = 0;
        }

        int max_visible = 14;
        int scroll_top = 0;
        if (s_queue_cursor >= scroll_top + max_visible) {
            scroll_top = s_queue_cursor - max_visible + 1;
        }
        if (s_queue_cursor < scroll_top) {
            scroll_top = s_queue_cursor;
        }

        int end_idx = scroll_top + max_visible;
        if (end_idx > (int)g_app_state.queue_count) {
            end_idx = (int)g_app_state.queue_count;
        }

        float row_y0 = 106.0f;
        float row_h = 56.0f;
        float rx = panel_x + 20.0f;
        float rw = panel_w - 40.0f;

        for (int i = scroll_top; i < end_idx; i++) {
            float ry = row_y0 + (float)(i - scroll_top) * row_h;
            bool is_selected = (i == s_queue_cursor);
            bool is_current = (g_app_state.has_current_track && (uint32_t)i == g_app_state.queue_index);

            if (is_selected) {
                gfx_draw_rect(rx, ry, rw, row_h - 4.0f, TH_SURFACE_2);
                gfx_draw_rect_outline(rx, ry, rw, row_h - 4.0f, TH_BORDER_LT, 1.0f);
                gfx_draw_rect(rx, ry, 4.0f, row_h - 4.0f, TH_TEXT);
            } else if (is_current) {
                gfx_draw_rect(rx, ry, rw, row_h - 4.0f, TH_SURFACE);
                gfx_draw_rect(rx, ry, 4.0f, row_h - 4.0f, TH_ACCENT);
                gfx_draw_rect(rx, ry + row_h - 5.0f, rw, 1.0f, TH_BORDER);
            } else {
                gfx_draw_rect(rx, ry, rw, row_h - 4.0f, TH_SURFACE);
                gfx_draw_rect(rx, ry + row_h - 5.0f, rw, 1.0f, TH_BORDER);
            }

            uint32_t title_color = is_selected ? TH_TEXT : (is_current ? TH_ACCENT_LT : TH_TEXT);
            uint32_t dim_color = TH_TEXT_DIM;
            uint32_t num_color = is_selected ? TH_TEXT : (is_current ? TH_ACCENT : TH_TEXT_DIM);

            char num_str[16];
            if (is_current) {
                snprintf(num_str, sizeof(num_str), ">%02u", (unsigned int)(i + 1));
            } else {
                snprintf(num_str, sizeof(num_str), "%02u", (unsigned int)(i + 1));
            }
            gfx_draw_text(rx + 10.0f, ry + 16.0f, num_str, 13.0f, num_color);

            const AppTrack *trk = &g_app_state.queue[i];
            const char *title_str = trk->title[0] ? trk->title : "Sin titulo";
            const char *artist_str = trk->artist[0] ? trk->artist : "Desconocido";

            gfx_draw_text_clipped(rx + 46.0f, ry + 8.0f, rw - 120.0f, title_str, 15.0f, title_color);
            gfx_draw_text_clipped(rx + 46.0f, ry + 28.0f, rw - 120.0f, artist_str, 12.0f, dim_color);

            if (trk->duration_sec > 0) {
                char dur_str[16];
                snprintf(dur_str, sizeof(dur_str), "%u:%02u",
                         (unsigned int)(trk->duration_sec / 60),
                         (unsigned int)(trk->duration_sec % 60));
                gfx_draw_text(rx + rw - 55.0f, ry + 18.0f, dur_str, 13.0f, dim_color);
            }
        }
    }

    /* Hint inferior con margen seguro */
    float hint_h = 56.0f;
    float hint_y = (float)VIRT_H - hint_h - 16.0f;
    gfx_draw_rect(panel_x, hint_y, panel_w, hint_h, TH_SURFACE_2);
    gfx_draw_rect(panel_x, hint_y, panel_w, 1.0f, TH_BORDER);
    gfx_draw_text(panel_x + 24.0f, hint_y + 20.0f, "[X] REPRODUCIR   [/\\ / O] CERRAR COLA", 12.0f, TH_TEXT_DIM);
}
