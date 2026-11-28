/*
 * Monochrome PS3 - UI Player Dock v3 (Phase 3 & Phase 6)
 * Bottom Persistent Player Dock de 96px (y: 984..1080)
 * Muestra estado real del motor de audio: caratula, artista/titulo,
 * progreso, barra de onda 1-bit y leyendas de glifos DualShock 3.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "monochrome.h"
#include "ui_theme.h"
#include "gfx_engine.h"
#include "app_state.h"
#include "audio_engine.h"
#include "cover_art.h"
#include "platform.h"

/* ── Render del dock inferior ────────────────────────────────────── */
void player_dock_render(int sw, int sh, long frame)
{
    (void)sw;
    (void)sh;
    (void)frame;

    int virt_w = VIRT_W;
    int virt_h = VIRT_H;
    int dock_y = virt_h - BOTTOM_DOCK_H;   /* 984 */
    int dock_h = BOTTOM_DOCK_H;            /* 96 */

    /* Dock background */
    gfx_draw_rect(0, dock_y, virt_w, dock_h, TH_BAR_BG);
    gfx_draw_rect(0, dock_y, virt_w, 2, TH_BORDER);

    /* ── Izquierda: caratula 72x72 ─────────────────────────────── */
    int cover_size = 72;
    int cover_x = 20;
    int cover_y = dock_y + (dock_h - cover_size) / 2;   /* centrado vertical */
    gfx_draw_rect(cover_x - 3, cover_y - 3, cover_size + 6, cover_size + 6, TH_BORDER);
    gfx_draw_rect(cover_x, cover_y, cover_size, cover_size, TH_SURFACE_2);

    bool has_track = g_app_state.has_current_track;
    bool cover_ready = false;

    if (has_track && g_app_state.current_track.cover_url[0] != '\0') {
        cover_ready = cover_art_is_cached(g_app_state.current_track.cover_url);
    }

    if (cover_ready) {
        GfxTexture tex;
        if (cover_art_get_texture(g_app_state.current_track.cover_url, &tex)) {
            gfx_draw_sprite(&tex,
                            (float)cover_x, (float)cover_y,
                            (float)cover_size, (float)cover_size,
                            0.0f, 0.0f, 1.0f, 1.0f,
                            0xFFFFFFFF);
        }
    } else {
        /* Placeholder de caratula */
        gfx_draw_rect(cover_x + 8, cover_y + 8, cover_size - 16, cover_size - 16, TH_BORDER);
        int mc_x = cover_x + (cover_size - 18 * 2) / 2;
        int mc_y = cover_y + (cover_size - 18) / 2;
        gfx_draw_text(mc_x, mc_y, "MC", 18.0f, TH_TEXT_DIM);
    }

    /* ── Derecha: leyendas de glifos DualShock 3 ───────────────── */
    int glyph_x = virt_w - 240;            /* 1680 */
    int glyph_y = dock_y + 11;
    int glyph_line_h = 15;

    /* Separador vertical entre reproductor y controles */
    gfx_draw_rect(glyph_x - 24, dock_y + 12, 1, dock_h - 24, TH_BORDER);

    gfx_draw_text(glyph_x, glyph_y, "L1/R1 TABS", 11.0f, TH_TEXT_DIM);
    glyph_y += glyph_line_h;
    gfx_draw_text(glyph_x, glyph_y, "L2/R2 SCRUB", 11.0f, TH_TEXT_DIM);
    glyph_y += glyph_line_h;
    gfx_draw_text(glyph_x, glyph_y, "X PLAY/PAUSE", 11.0f, TH_TEXT_DIM);
    glyph_y += glyph_line_h;
    gfx_draw_text(glyph_x, glyph_y, "[] VISUALIZER", 11.0f, TH_TEXT_DIM);
    glyph_y += glyph_line_h;
    gfx_draw_text(glyph_x, glyph_y, "TRIANGLE QUEUE", 11.0f, TH_TEXT_DIM);

    /* ── Centro: ARTISTA - TITULO + animacion EQ + linea de tiempo ─── */
    int info_x = cover_x + cover_size + 24;  /* 116 */
    int content_max_x = glyph_x - 40;       /* 1640 - margen antes del separador */

    if (has_track) {
        int row1_y = dock_y + 16;
        int row2_y = dock_y + 54;

        /* Fila 1 Derecha: Animación de espectro / ecualizador (16 bandas) */
        int bar_count = 16;
        int bar_w = 4;
        int bar_gap = 3;
        int total_vis_w = bar_count * bar_w + (bar_count - 1) * bar_gap; /* 109px */
        int vis_x = content_max_x - total_vis_w;
        int vis_y = row1_y - 2;
        int vis_h = 22;

        float vis_bands[VIS_BANDS];
        audio_engine_get_vis(vis_bands, VIS_BANDS);
        bool is_playing = audio_engine_is_playing();

        for (int b = 0; b < bar_count; b++) {
            float band = (b < VIS_BANDS) ? vis_bands[b] : 0.0f;
            if (band > 1.0f) band = 1.0f;
            if (band < 0.0f) band = 0.0f;

            int bh = is_playing ? ((int)(band * (float)vis_h) + 2) : 2;
            if (bh > vis_h) bh = vis_h;
            if (bh < 2) bh = 2;

            int bx = vis_x + b * (bar_w + bar_gap);
            int by = vis_y + (vis_h - bh);
            uint32_t bar_col = is_playing ? TH_ACCENT : TH_TEXT_DIM;
            gfx_draw_rect(bx, by, bar_w, bh, bar_col);
            if (is_playing && bh > 4) {
                gfx_draw_rect(bx, by, bar_w, 2, TH_ACCENT_LT);
            }
        }

        /* Fila 1 Izquierda: Artista - Titulo */
        char line1[512];
        snprintf(line1, sizeof(line1), "%s - %s",
                 g_app_state.current_track.artist,
                 g_app_state.current_track.title);
        int max_title_w = vis_x - info_x - 20;
        if (max_title_w < 100) max_title_w = 100;
        gfx_draw_text_clipped(info_x, row1_y, (float)max_title_w, line1, 16.0f, TH_TEXT);

        /* Fila 2: Estado de error O Linea de tiempo con tiempos */
        if (audio_engine_has_error()) {
            const char *msg = audio_engine_get_status_message();
            char err_str[256];
            snprintf(err_str, sizeof(err_str), "[!] %s", msg);
            gfx_draw_text_clipped(info_x, row2_y, (float)(content_max_x - info_x), err_str, 13.0f, 0xFFFF4444);
        } else {
            float progress = audio_engine_get_progress();
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;

            uint32_t dur_total = g_app_state.current_track.duration_sec;
            uint32_t dur_cur = (uint32_t)((float)dur_total * progress);

            char cur_time_str[16];
            char tot_time_str[16];
            snprintf(cur_time_str, sizeof(cur_time_str), "%u:%02u",
                     (unsigned int)(dur_cur / 60), (unsigned int)(dur_cur % 60));
            snprintf(tot_time_str, sizeof(tot_time_str), "%u:%02u",
                     (unsigned int)(dur_total / 60), (unsigned int)(dur_total % 60));

            /* Tiempo actual (izquierda de la barra) */
            int time_cur_w = gfx_draw_text(info_x, row2_y, cur_time_str, 12.0f, TH_ACCENT_LT);

            /* Tiempo total (derecha de la barra, dentro del area de contenido) */
            int time_tot_box_w = (int)(60.0f * gfx_get_font_scale());
            int time_tot_x = content_max_x - time_tot_box_w;
            gfx_draw_text(time_tot_x, row2_y, tot_time_str, 12.0f, TH_TEXT_DIM);

            /* Linea de tiempo horizontal */
            int bar_x = info_x + (time_cur_w > 0 ? time_cur_w : 50) + 16;
            int bar_end_x = time_tot_x - 16;
            int bar_w = bar_end_x - bar_x;
            int bar_h = 6;
            int bar_y = row2_y + 3;

            if (bar_w > 50) {
                /* Fondo y ranura de la barra */
                gfx_draw_rect(bar_x, bar_y, bar_w, bar_h, TH_SURFACE_2);
                gfx_draw_rect(bar_x, bar_y, bar_w, 1, TH_BORDER);

                /* Progreso reproducido */
                int fill_w = (int)(progress * (float)bar_w);
                if (fill_w > 0) {
                    gfx_draw_rect(bar_x, bar_y, fill_w, bar_h, TH_ACCENT);
                }

                /* Cabezal / cursor de reproduccion */
                int pip_x = bar_x + fill_w - 3;
                if (pip_x < bar_x) pip_x = bar_x;
                if (pip_x > bar_x + bar_w - 6) pip_x = bar_x + bar_w - 6;
                gfx_draw_rect(pip_x, bar_y - 3, 6, bar_h + 6, TH_ACCENT);
                gfx_draw_rect(pip_x + 1, bar_y - 2, 4, bar_h + 4, TH_ACCENT_LT);
            }
        }
    } else {
        gfx_draw_text(info_x, dock_y + 24, "Ningun tema seleccionado", 16.0f, TH_TEXT);
        gfx_draw_text(info_x, dock_y + 54, "Pulsa X en la lista para reproducir", 13.0f, TH_TEXT_DIM);
    }
}
