#include "ui_components.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static uint32_t state_bg(UiComponentState s) {
    switch (s) {
        case UI_COMPONENT_STATE_FOCUSED:  return TH_ACTIVE_BG;
        case UI_COMPONENT_STATE_PLAYING: return TH_ACCENT;
        default: return TH_SURFACE;
    }
}

static uint32_t state_text(UiComponentState s) {
    switch (s) {
        case UI_COMPONENT_STATE_FOCUSED:  return TH_ACTIVE_TEXT;
        case UI_COMPONENT_STATE_PLAYING: return TH_TEXT;
        default: return TH_TEXT;
    }
}

static void draw_cover_placeholder(float x, float y, float size, const AppTrack *track) {
    /* Recuadro monocromático con iniciales/placeholder limpio */
    gfx_draw_rect(x, y, size, size, TH_SURFACE_2);
    gfx_draw_rect_outline(x, y, size, size, TH_BORDER, 1.0f);
    char initials[4] = {0};
    if (track && track->title[0]) {
        initials[0] = (char)toupper((unsigned char)track->title[0]);
        if (track->title[1]) initials[1] = (char)toupper((unsigned char)track->title[1]);
    } else {
        initials[0] = '?';
    }
    gfx_draw_text(x + size * 0.35f, y + size * 0.3f, initials, size * 0.4f, TH_TEXT_DIM);
}

void album_card_render(float x, float y, float w, float h,
                       const AppTrack *track,
                       UiComponentState state) {
    /* Fondo de la tarjeta */
    gfx_draw_rect(x, y, w, h, state_bg(state));
    gfx_draw_rect_outline(x, y, w, h, TH_BORDER, 1.0f);

    /* Carátula (lado izquierdo cuadrado) */
    float cover_size = h - 16.0f;
    float cx = x + 8.0f;
    float cy = y + 8.0f;
    if (track && track->cover_url[0]) {
        GfxTexture tex;
        if (cover_art_get_texture(track->cover_url, &tex) && tex.uploaded) {
            gfx_draw_sprite(&tex, cx, cy, cover_size, cover_size, 0,0,1,1, 0xFFFFFFFF);
        } else {
            draw_cover_placeholder(cx, cy, cover_size, track);
        }
    } else {
        draw_cover_placeholder(cx, cy, cover_size, track);
    }

    /* Texto: título y artista */
    float text_x = cx + cover_size + 12.0f;
    float text_max = x + w - text_x - 8.0f;
    uint32_t tc = state_text(state);
    const char *title = (track && track->title[0]) ? track->title : "Unknown";
    const char *artist = (track && track->artist[0]) ? track->artist : "";
    gfx_draw_text_clipped(text_x, y + 12.0f, text_max, title, 18.0f, tc);
    gfx_draw_text_clipped(text_x, y + 36.0f, text_max, artist, 14.0f, TH_TEXT_DIM);
}

void track_row_render(float x, float y, float w, float h,
                      uint32_t index, const AppTrack *track,
                      UiComponentState state) {
    /* Fondo */
    gfx_draw_rect(x, y, w, h, state_bg(state));
    if (state == UI_COMPONENT_STATE_PLAYING) {
        gfx_draw_rect(x, y, 4.0f, h, TH_ACCENT_LT);
    }

    uint32_t tc = state_text(state);
    /* Índice */
    char idxbuf[16];
    snprintf(idxbuf, sizeof(idxbuf), "%u", index);
    gfx_draw_text(x + 8.0f, y + (h - 14.0f) * 0.5f, idxbuf, 14.0f, TH_TEXT_DIM);

    float col_x = x + 48.0f;
    /* Título */
    const char *title = (track && track->title[0]) ? track->title : "Unknown";
    gfx_draw_text_clipped(col_x, y + 6.0f, w * 0.45f, title, 16.0f, tc);

    /* Artista / Álbum */
    char sub[256];
    if (track) {
        snprintf(sub, sizeof(sub), "%s - %s", track->artist, track->album);
    } else {
        sub[0] = 0;
    }
    gfx_draw_text_clipped(col_x, y + 26.0f, w * 0.45f, sub, 12.0f, TH_TEXT_DIM);

    /* Duración (derecha) */
    char dur[32];
    if (track) {
        uint32_t s = track->duration_sec;
        snprintf(dur, sizeof(dur), "%u:%02u", s / 60, s % 60);
    } else {
        dur[0] = 0;
    }
    gfx_draw_text(x + w - 70.0f, y + (h - 14.0f) * 0.5f, dur, 14.0f, TH_TEXT_DIM);
}