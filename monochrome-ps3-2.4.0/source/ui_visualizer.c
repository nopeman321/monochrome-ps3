#include "ui_visualizer.h"
#include <stdio.h>
#include <string.h>

#include "monochrome.h"
#include "app_state.h"
#include "gfx_engine.h"
#include "ui_theme.h"
#include "cover_art.h"
#include "audio_engine.h"

#define VIS_SPECTRUM_BARS 32

void ui_visualizer_init(void)
{
}

bool ui_visualizer_is_fullscreen(void)
{
    return g_app_state.visualizer_fullscreen;
}

void ui_visualizer_set_fullscreen(bool fullscreen)
{
    g_app_state.visualizer_fullscreen = fullscreen;
    gfx_set_background_dim(fullscreen ? 0.60f : 1.0f);
}

bool ui_visualizer_handle_input(Action act)
{
    if (!g_app_state.visualizer_fullscreen) {
        if (act == ACT_SQUARE) {
            g_app_state.visualizer_fullscreen = true;
            gfx_set_background_dim(0.60f);
            return true;
        }
        return false;
    }

    /* ACT_SQUARE or ACT_CIRCLE close the visualizer */
    if (act == ACT_SQUARE || act == ACT_CIRCLE) {
        g_app_state.visualizer_fullscreen = false;
        gfx_set_background_dim(1.0f);
        return true;
    }

    /* ACT_CROSS alterna play/pause */
    if (act == ACT_CROSS) {
        if (audio_engine_is_playing()) {
            audio_engine_pause();
        } else {
            audio_engine_resume();
        }
        return true;
    }

    /* ACT_L2 / ACT_R2 hacen scrub hacia atras / adelante */
    if (act == ACT_L2) {
        float p = audio_engine_get_progress();
        p -= 0.05f;
        if (p < 0.0f) {
            p = 0.0f;
        }
        audio_engine_seek(p);
        return true;
    }

    if (act == ACT_R2) {
        float p = audio_engine_get_progress();
        p += 0.05f;
        if (p > 1.0f) {
            p = 1.0f;
        }
        audio_engine_seek(p);
        return true;
    }

    return true;
}

void ui_visualizer_render(void)
{
    if (!g_app_state.visualizer_fullscreen) {
        return;
    }

    gfx_set_background_dim(0.60f);

    /* Caratula grande */
    float cover_size = 380.0f;
    float cover_x = ((float)VIRT_W - cover_size) * 0.5f;
    float cover_y = 60.0f;

    bool cover_drawn = false;
    if (g_app_state.has_current_track && g_app_state.current_track.cover_url[0]) {
        GfxTexture tex;
        if (cover_art_get_texture(g_app_state.current_track.cover_url, &tex) && tex.uploaded) {
            gfx_draw_sprite(&tex, cover_x, cover_y, cover_size, cover_size, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF);
            gfx_draw_rect_outline(cover_x, cover_y, cover_size, cover_size, TH_BORDER, 1.0f);
            cover_drawn = true;
        }
    }

    if (!cover_drawn) {
        gfx_draw_rect(cover_x, cover_y, cover_size, cover_size, TH_SURFACE_2);
        gfx_draw_rect_outline(cover_x, cover_y, cover_size, cover_size, TH_BORDER, 1.0f);
        gfx_draw_text(cover_x + cover_size * 0.45f, cover_y + cover_size * 0.4f, "?", 48.0f, TH_TEXT_DIM);
    }

    /* Titulo y artista en ASCII */
    const char *title = (g_app_state.has_current_track && g_app_state.current_track.title[0])
                        ? g_app_state.current_track.title : "Sin reproduccion";
    const char *artist = (g_app_state.has_current_track && g_app_state.current_track.artist[0])
                         ? g_app_state.current_track.artist : "Monochrome PS3";

    float text_max_w = 900.0f;
    float text_x = ((float)VIRT_W - text_max_w) * 0.5f;
    float text_y = cover_y + cover_size + 24.0f;

    gfx_draw_text_clipped(text_x, text_y, text_max_w, title, 22.0f, TH_TEXT);
    gfx_draw_text_clipped(text_x, text_y + 30.0f, text_max_w, artist, 15.0f, TH_TEXT_DIM);

    /* 32 barras verticales de espectro alimentadas por audio_engine_get_vis()
     * dibujadas con gfx_draw_rect dentro de un area acotada (sin grillas ni lineas de perspectiva)
     */
    float box_w = 760.0f;
    float box_h = 160.0f;
    float box_x = ((float)VIRT_W - box_w) * 0.5f;
    float box_y = text_y + 65.0f;

    gfx_draw_rect(box_x, box_y, box_w, box_h, TH_SURFACE);
    gfx_draw_rect_outline(box_x, box_y, box_w, box_h, TH_BORDER, 1.0f);

    float raw_vis[16] = {0.0f};
    if (audio_engine_is_playing()) {
        audio_engine_get_vis(raw_vis, 16);
    }

    int num_bars = VIS_SPECTRUM_BARS;
    float bar_w = 16.0f;
    float bar_gap = 6.0f;
    float total_bars_w = (float)num_bars * bar_w + (float)(num_bars - 1) * bar_gap;
    float start_bars_x = box_x + (box_w - total_bars_w) * 0.5f;
    float base_y = box_y + box_h - 12.0f;
    float max_bar_h = box_h - 24.0f;

    for (int i = 0; i < num_bars; i++) {
        float t = (float)i * 15.0f / (float)(num_bars - 1);
        int i0 = (int)t;
        int i1 = (i0 + 1 < 16) ? i0 + 1 : 15;
        float frac = t - (float)i0;
        float val = raw_vis[i0] * (1.0f - frac) + raw_vis[i1] * frac;

        float bh = val * max_bar_h;
        if (bh < 3.0f) {
            bh = 3.0f;
        }
        if (bh > max_bar_h) {
            bh = max_bar_h;
        }

        float bx = start_bars_x + (float)i * (bar_w + bar_gap);
        float by = base_y - bh;

        /* Barra de espectro de dos tonos: cuerpo violeta + pico lilac */
        gfx_draw_rect(bx, by, bar_w, bh, TH_ACCENT);
        if (bh > 6.0f) {
            gfx_draw_rect(bx, by, bar_w, 3.0f, TH_ACCENT_LT);
        }
    }

    /* Barra de progreso real */
    float prog_w = box_w;
    float prog_h = 6.0f;
    float prog_x = box_x;
    float prog_y = box_y + box_h + 30.0f;

    gfx_draw_rect(prog_x, prog_y, prog_w, prog_h, TH_SURFACE_2);

    float progress = audio_engine_get_progress();
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    gfx_draw_rect(prog_x, prog_y, prog_w * progress, prog_h, TH_ACCENT);

    /* Cabezal de reproduccion / scrubber */
    float scrub_x = prog_x + prog_w * progress - 4.0f;
    if (scrub_x < prog_x) scrub_x = prog_x;
    if (scrub_x > prog_x + prog_w - 8.0f) scrub_x = prog_x + prog_w - 8.0f;
    gfx_draw_rect(scrub_x, prog_y - 2.0f, 8.0f, prog_h + 4.0f, TH_ACCENT_LT);

    /* Tiempos MM:SS */
    uint32_t dur_total = g_app_state.has_current_track ? g_app_state.current_track.duration_sec : 0;
    uint32_t dur_cur = (uint32_t)((float)dur_total * progress);
    char cur_str[16];
    char tot_str[16];
    snprintf(cur_str, sizeof(cur_str), "%u:%02u", (unsigned int)(dur_cur / 60), (unsigned int)(dur_cur % 60));
    snprintf(tot_str, sizeof(tot_str), "%u:%02u", (unsigned int)(dur_total / 60), (unsigned int)(dur_total % 60));

    gfx_draw_text(prog_x, prog_y + 14.0f, cur_str, 13.0f, TH_TEXT_DIM);
    gfx_draw_text(prog_x + prog_w - 50.0f, prog_y + 14.0f, tot_str, 13.0f, TH_TEXT_DIM);

    /* Hint inferior discreto y seguro */
    const char *hint = "[X] PLAY/PAUSE   [L2/R2] SCRUB   [] / [O] CERRAR";
    float hint_x = ((float)VIRT_W - 480.0f) * 0.5f;
    float hint_y = 1000.0f;
    gfx_draw_text(hint_x, hint_y, hint, 13.0f, TH_TEXT_DIM);
}
