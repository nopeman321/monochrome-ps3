#include "ui_components.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static void draw_cover_placeholder(float x, float y, float size, const AppTrack *track) {
    /* Recuadro monocromatico de alta precision con grid Y2K */
    gfx_draw_rect(x, y, size, size, TH_SURFACE_2);
    gfx_draw_rect_outline(x, y, size, size, TH_BORDER, 1.0f);
    /* Reticula interior sutil */
    gfx_draw_rect_outline(x + 4.0f, y + 4.0f, size - 8.0f, size - 8.0f, TH_BORDER, 1.0f);

    char initials[8] = {0};
    if (track && track->title[0]) {
        initials[0] = (char)toupper((unsigned char)track->title[0]);
        if (track->title[1]) initials[1] = (char)toupper((unsigned char)track->title[1]);
    } else {
        initials[0] = 'M';
        initials[1] = 'C';
    }
    float font_size = size * 0.30f;
    if (font_size < 12.0f) font_size = 12.0f;
    gfx_draw_text(x + size * 0.28f, y + size * 0.34f, initials, font_size, TH_TEXT_DIM);
}

void album_card_render(float x, float y, float w, float h,
                       const AppTrack *track,
                       UiComponentState state) {
    bool is_focused = (state == UI_COMPONENT_STATE_FOCUSED);
    bool is_playing = (state == UI_COMPONENT_STATE_PLAYING);

    /* Fondo de la tarjeta */
    uint32_t bg = is_focused ? TH_SURFACE_2 : TH_SURFACE;
    gfx_draw_rect(x, y, w, h, bg);

    /* Borde segun estado */
    if (is_focused) {
        gfx_draw_rect_outline(x, y, w, h, TH_BORDER_LT, 2.0f);
    } else if (is_playing) {
        gfx_draw_rect_outline(x, y, w, h, TH_ACCENT, 1.0f);
    } else {
        gfx_draw_rect_outline(x, y, w, h, TH_BORDER, 1.0f);
    }

    /* Carátula (lado izquierdo cuadrado o centrada) */
    float cover_size = h - 16.0f;
    float cx = x + 8.0f;
    float cy = y + 8.0f;
    if (track && track->cover_url[0]) {
        GfxTexture tex;
        if (cover_art_get_texture(track->cover_url, &tex) && tex.uploaded) {
            gfx_draw_sprite(&tex, cx, cy, cover_size, cover_size, 0,0,1,1, 0xFFFFFFFF);
            gfx_draw_rect_outline(cx, cy, cover_size, cover_size, TH_BORDER, 1.0f);
        } else {
            draw_cover_placeholder(cx, cy, cover_size, track);
        }
    } else {
        draw_cover_placeholder(cx, cy, cover_size, track);
    }

    /* Texto: título y artista */
    float text_x = cx + cover_size + 14.0f;
    float text_max = x + w - text_x - 12.0f;
    if (text_max < 60.0f) text_max = 60.0f;

    uint32_t title_color = is_focused ? TH_TEXT : (is_playing ? TH_ACCENT_LT : TH_TEXT);
    uint32_t sub_color = is_focused ? TH_TEXT_DIM : TH_TEXT_DIM;

    const char *title = (track && track->title[0]) ? track->title : "Sin titulo";
    const char *artist = (track && track->artist[0]) ? track->artist : "Desconocido";

    gfx_draw_text_clipped(text_x, y + 14.0f, text_max, title, 16.0f, title_color);
    gfx_draw_text_clipped(text_x, y + 36.0f, text_max, artist, 13.0f, sub_color);
}

void track_row_render(float x, float y, float w, float h,
                      uint32_t index, const AppTrack *track,
                      UiComponentState state) {
    bool is_focused = (state == UI_COMPONENT_STATE_FOCUSED);
    bool is_playing = (state == UI_COMPONENT_STATE_PLAYING);

    /* Fondo del renglon */
    if (is_focused) {
        gfx_draw_rect(x, y, w, h - 2.0f, TH_SURFACE_2);
        gfx_draw_rect_outline(x, y, w, h - 2.0f, TH_BORDER_LT, 1.0f);
        /* Left 4px accent bar */
        gfx_draw_rect(x, y, 4.0f, h - 2.0f, TH_TEXT);
    } else if (is_playing) {
        gfx_draw_rect(x, y, w, h - 2.0f, TH_SURFACE);
        /* Barra de acento tenue izquierda */
        gfx_draw_rect(x, y, 4.0f, h - 2.0f, TH_ACCENT);
        gfx_draw_rect(x, y + h - 2.0f, w, 1.0f, TH_BORDER);
    } else {
        gfx_draw_rect(x, y, w, h - 2.0f, TH_SURFACE);
        gfx_draw_rect(x, y + h - 2.0f, w, 1.0f, TH_BORDER);
    }

    /* Colores del texto con alto contraste garantizado */
    uint32_t idx_color = is_focused ? TH_TEXT : (is_playing ? TH_ACCENT : TH_TEXT_DIM);
    uint32_t title_color = is_focused ? TH_TEXT : (is_playing ? TH_ACCENT_LT : TH_TEXT);
    uint32_t sub_color = is_focused ? TH_TEXT_DIM : TH_TEXT_DIM;
    uint32_t dur_color = is_focused ? TH_TEXT : TH_TEXT_DIM;

    /* Indice numerico o glifo de reproduccion */
    char idxbuf[16];
    if (is_playing) {
        snprintf(idxbuf, sizeof(idxbuf), ">%02u", index);
    } else {
        snprintf(idxbuf, sizeof(idxbuf), "%02u", index);
    }
    gfx_draw_text(x + 12.0f, y + (h - 16.0f) * 0.5f, idxbuf, 14.0f, idx_color);

    float col_x = x + 54.0f;
    float max_title_w = (w - 180.0f) * 0.52f;
    float max_sub_w = (w - 180.0f) * 0.44f;
    if (max_title_w < 120.0f) max_title_w = 120.0f;
    if (max_sub_w < 100.0f) max_sub_w = 100.0f;

    /* Titulo */
    const char *title = (track && track->title[0]) ? track->title : "Sin titulo";
    gfx_draw_text_clipped(col_x, y + 8.0f, max_title_w, title, 16.0f, title_color);

    /* Artista • Album */
    char sub[320];
    if (track && (track->artist[0] || track->album[0])) {
        if (track->artist[0] && track->album[0]) {
            snprintf(sub, sizeof(sub), "%s - %s", track->artist, track->album);
        } else if (track->artist[0]) {
            snprintf(sub, sizeof(sub), "%s", track->artist);
        } else {
            snprintf(sub, sizeof(sub), "%s", track->album);
        }
    } else {
        sub[0] = '\0';
    }
    gfx_draw_text_clipped(col_x, y + 28.0f, max_sub_w, sub, 13.0f, sub_color);

    /* Duracion (derecha con margen seguro) */
    char dur[32];
    if (track && track->duration_sec > 0) {
        uint32_t s = track->duration_sec;
        snprintf(dur, sizeof(dur), "%u:%02u", s / 60, s % 60);
    } else {
        snprintf(dur, sizeof(dur), "--:--");
    }
    gfx_draw_text(x + w - 80.0f, y + (h - 16.0f) * 0.5f, dur, 14.0f, dur_color);
}