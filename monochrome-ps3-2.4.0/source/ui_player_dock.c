/*
 * Monochrome PS3 - UI Player Dock v3 (Phase 3)
 * Bottom Persistent Player Dock de 96px (y: 984..1080)
 * Muestra estado real del motor de audio: carátula, artista/título,
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
    (void)frame;
    int dock_y = sh - BOTTOM_DOCK_H;   /* 984 */
    int dock_h = BOTTOM_DOCK_H;        /* 96 */

    /* Fondo del dock */
    gfx_draw_rect(0, dock_y, sw, dock_h, TH_SURFACE);
    gfx_draw_rect(0, dock_y, sw, 2, TH_BORDER);

    /* ── Izquierda: carátula 72x72 ─────────────────────────────── */
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
        /* Placeholder de carátula */
        gfx_draw_rect(cover_x + 12, cover_y + 12, cover_size - 24, cover_size - 24, TH_BORDER);
        gfx_draw_text(cover_x + 18, cover_y + 28, "MC", 16.0f, TH_TEXT_DIM);
    }

    /* ── Centro: ARTISTA - TÍTULO + tiempo + barra de onda ─────── */
    int info_x = cover_x + cover_size + 24;
    int info_y = dock_y + 14;
    int max_info_w = sw - info_x - 260;  /* dejar espacio a la derecha para glifos */

    if (has_track) {
        /* Artista - Título */
        char line1[256];
        snprintf(line1, sizeof(line1), "%s - %s",
                 g_app_state.current_track.artist,
                 g_app_state.current_track.title);
        int max_line1_w = max_info_w - 120;  /* dejar espacio para tiempo */
        if (max_line1_w < 100) max_line1_w = 100;
        gfx_draw_text_clipped(info_x, info_y, (float)max_line1_w, line1, 18.0f, TH_TEXT);

        /* Tiempo MM:SS en violeta */
        int cur_sec = (int)(g_app_state.current_track.duration_sec *
                           audio_engine_get_progress());
        char time_str[16];
        snprintf(time_str, sizeof(time_str), "%d:%02d", cur_sec / 60, cur_sec % 60);
        int time_w = (int)strlen(time_str) * 8 * 1;
        gfx_draw_text(sw - 260 + (120 - time_w) / 2, info_y + 4, time_str, 8.0f, TH_ACCENT);

        info_y += 24;

        /* Barra de progreso con forma de onda 1-bit */
        int wave_x = info_x;
        int wave_y = info_y + 6;
        int wave_w = max_info_w > 200 ? max_info_w : 200;
        int wave_h = 14;

        /* Fondo de la onda */
        gfx_draw_rect(wave_x, wave_y, wave_w, wave_h, TH_BORDER);

        /* Barras verticales reactivas alimentadas por audio_engine_get_vis() */
        float vis_bands[VIS_BANDS];
        audio_engine_get_vis(vis_bands, VIS_BANDS);

        int bar_count = 16;
        int bar_w = 4;
        int bar_gap = 4;
        int total_bar_w = bar_count * bar_w + (bar_count - 1) * bar_gap;
        int start_x = wave_x + (wave_w - total_bar_w) / 2;

        float progress = audio_engine_get_progress();
        for (int b = 0; b < bar_count; b++) {
            float band = vis_bands[b % VIS_BANDS];
            int bar_h = (int)(band * (float)wave_h * 0.8f) + 2;
            if (bar_h > wave_h) bar_h = wave_h;
            if (bar_h < 2) bar_h = 2;

            /* Solo rellenar hasta el progreso actual (onda reactiva) */
            float fill_x = progress * (float)total_bar_w;
            float bar_left = (float)(b * (bar_w + bar_gap));
            float bar_right = bar_left + (float)bar_w;

            uint32_t bar_color;
            if (bar_right <= fill_x) {
                bar_color = TH_ACCENT;   /* ya reproducido: violeta */
            } else if (bar_left < fill_x) {
                bar_color = TH_ACCENT_LT; /* parcialmente: violeta claro */
            } else {
                bar_color = TH_TEXT_DIM; /* pendiente: gris oscuro */
            }

            gfx_draw_rect(wave_x + (int)bar_left, wave_y + (wave_h - bar_h),
                          bar_w, bar_h, bar_color);
        }

        info_y += wave_h + 10;
    } else {
        gfx_draw_text(info_x, info_y, "Ningun tema seleccionado", 8.0f, TH_TEXT_DIM);
        info_y += 24;
        gfx_draw_text(info_x, info_y, "Pulsa X en la lista para reproducir", 8.0f, TH_TEXT_DIM);
    }

    /* ── Derecha: leyendas de glifos DualShock 3 ───────────────── */
    int glyph_x = sw - 250;
    int glyph_y = dock_y + 14;
    int glyph_line_h = 16;

    gfx_draw_text(glyph_x, glyph_y, "L1/R1 TABS", 8.0f, TH_TEXT_DIM);
    glyph_y += glyph_line_h;
    gfx_draw_text(glyph_x, glyph_y, "L2/R2 SCRUB", 8.0f, TH_TEXT_DIM);
    glyph_y += glyph_line_h;
    gfx_draw_text(glyph_x, glyph_y, "X PLAY/PAUSE", 8.0f, TH_TEXT_DIM);
    glyph_y += glyph_line_h;
    gfx_draw_text(glyph_x, glyph_y, "[] VISUALIZER", 8.0f, TH_TEXT_DIM);
    glyph_y += glyph_line_h;
    gfx_draw_text(glyph_x, glyph_y, "TRIANGLE QUEUE", 8.0f, TH_TEXT_DIM);
}