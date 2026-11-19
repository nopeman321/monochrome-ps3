#include "ui_home.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "monochrome.h"
#include "app_state.h"
#include "gfx_engine.h"
#include "ui_theme.h"
#include "cover_art.h"
#include "audio_engine.h"

static int s_lane_cursor = 0;
static int s_hero_index = 0;

void ui_home_init(void) { s_lane_cursor = 0; s_hero_index = 0; }

static void render_hero(int x, int y, int w, int h, AppTrack *track) {
    gfx_draw_rect(x, y, w, h, TH_SURFACE);
    gfx_draw_rect_outline(x, y, w, h, TH_BORDER, 2.0f);
    gfx_draw_text(x + 16, y + 12, "FEATURED //", 14.0f, TH_ACCENT);

    int cx = x + 24;
    int cy = y + 48;
    int csize = 220;
    if (track && track->cover_url[0]) {
        GfxTexture tex;
        if (cover_art_get_texture(track->cover_url, &tex) && tex.uploaded) {
            gfx_draw_sprite(&tex, cx, cy, csize, csize, 0,0,1,1, 0xFFFFFFFF);
        } else {
            gfx_draw_rect(cx, cy, csize, csize, TH_SURFACE_2);
            gfx_draw_rect_outline(cx, cy, csize, csize, TH_BORDER, 1.0f);
            char init[4] = {0};
            if (track->title[0]) init[0] = (char)toupper((unsigned char)track->title[0]);
            else init[0] = '?';
            gfx_draw_text(cx + csize/3, cy + csize/3, init, csize*0.4f, TH_TEXT_DIM);
            cover_art_load(track->cover_url, g_app_state.cover_mode);
        }
    } else {
        gfx_draw_rect(cx, cy, csize, csize, TH_SURFACE_2);
        gfx_draw_rect_outline(cx, cy, csize, csize, TH_BORDER, 1.0f);
    }

    /* visualizador solo si reproduciendo, monocromatico sutil, dentro del hero */
    if (g_app_state.has_current_track && audio_engine_is_playing()) {
        float bands[16];
        audio_engine_get_vis(bands, 16);
        (void)bands; /* wireframe disabled */
    }

    const char *badge = audio_engine_is_playing() ? "NOW PLAYING" : "READY";
    uint32_t badge_color = audio_engine_is_playing() ? TH_ACCENT_LT : TH_TEXT_DIM;
    gfx_draw_text(cx + csize + 24, cy + 20, badge, 12.0f, badge_color);
    const char *title = (track && track->title[0]) ? track->title : "Sin pista";
    const char *artist = (track && track->artist[0]) ? track->artist : "";
    gfx_draw_text_clipped(cx + csize + 24, cy + 44, (float)(w - csize - 80), title, 22.0f, TH_TEXT);
    gfx_draw_text_clipped(cx + csize + 24, cy + 74, (float)(w - csize - 80), artist, 16.0f, TH_TEXT_DIM);
}

static void render_lanes(int x, int y, int w, int h) {
    int count = (int)g_app_state.track_count;
    if (count <= 0) count = (int)g_app_state.album_count;
    if (count <= 0) {
        gfx_draw_text(x, y + 20, "Sin pistas cargadas - abre BUSCAR en el menu izquierdo", 14.0f, TH_TEXT_DIM);
        return;
    }
    int visible = 6;
    int start = s_lane_cursor;
    if (start < 0) start = 0;
    if (start + visible > count) start = count - visible;
    if (start < 0) start = 0;
    int card_w = 180, card_h = 220, gap = 16;
    int cx = x, cy = y;
    gfx_draw_text(cx, cy - 24, "TRACKS", 16.0f, TH_TEXT);
    cy += 4;
    for (int i = 0; i < visible && (start + i) < count; i++) {
        int idx = start + i;
        AppTrack *t = NULL;
        if (idx < (int)g_app_state.track_count) t = &g_app_state.tracks[idx];
        else if (idx < (int)g_app_state.album_count) t = &g_app_state.albums[idx];
        if (t && t->cover_url[0]) {
            cover_art_load(t->cover_url, g_app_state.cover_mode);
            if (i + 2 < visible) {
                int nidx = start + i + 2;
                AppTrack *nt = NULL;
                if (nidx < (int)g_app_state.track_count) nt = &g_app_state.tracks[nidx];
                if (nt && nt->cover_url[0]) cover_art_load(nt->cover_url, g_app_state.cover_mode);
            }
        }
        UiComponentState st = UI_COMPONENT_STATE_DEFAULT;
        if (i == 0) st = UI_COMPONENT_STATE_FOCUSED;
        album_card_render((float)cx, (float)cy, (float)card_w, (float)card_h, t, st);
        cx += card_w + gap;
    }

    int ry = y + card_h + 40;
    gfx_draw_text(x, ry - 24, "LISTA", 16.0f, TH_TEXT);
    ry += 4;
    int row_h = 52;
    for (int i = 0; i < 6 && (start + i) < count; i++) {
        int idx = start + i;
        AppTrack *t = NULL;
        if (idx < (int)g_app_state.track_count) t = &g_app_state.tracks[idx];
        UiComponentState st = UI_COMPONENT_STATE_DEFAULT;
        if (idx == s_lane_cursor) st = UI_COMPONENT_STATE_FOCUSED;
        if (g_app_state.has_current_track && t && strcmp(t->id, g_app_state.current_track.id) == 0)
            st = UI_COMPONENT_STATE_PLAYING;
        track_row_render((float)x, (float)(ry + i * row_h), (float)(w), (float)row_h, (uint32_t)(idx + 1), t, st);
    }
}

void ui_home_render(void) {
    int x0 = 260, y0 = 64, x1 = VIRT_W, y1 = VIRT_H - 96;
    int mw = x1 - x0;
    int mh = y1 - y0;
    int cx = x0 + 20;
    int cy = y0 + 10;

    AppTrack *hero = NULL;
    if (g_app_state.has_current_track) hero = &g_app_state.current_track;
    else if (g_app_state.track_count > 0) hero = &g_app_state.tracks[s_hero_index % g_app_state.track_count];
    else if (g_app_state.album_count > 0) hero = &g_app_state.albums[s_hero_index % g_app_state.album_count];
    render_hero(cx, cy, mw - 40, 260, hero);
    render_lanes(cx, cy + 280, mw - 40, mh - 300);
}

void ui_home_handle_input(InputAction act) {
    int count = (int)g_app_state.track_count;
    if (count <= 0) count = (int)g_app_state.album_count;
    switch (act) {
        case ACT_LEFT:  if (s_lane_cursor > 0) s_lane_cursor--; break;
        case ACT_RIGHT: if (count > 0 && s_lane_cursor + 1 < count) s_lane_cursor++; break;
        case ACT_UP:    if (s_hero_index > 0) s_hero_index--; break;
        case ACT_DOWN:  if (count > 0 && s_hero_index + 1 < count) s_hero_index++; break;
        case ACT_CROSS: {
            AppTrack *t = NULL;
            if (s_lane_cursor < (int)g_app_state.track_count) t = &g_app_state.tracks[s_lane_cursor];
            else if (s_lane_cursor < (int)g_app_state.album_count) t = &g_app_state.albums[s_lane_cursor];
            if (t) {
                audio_engine_play_track(t->id, t->title);
                memcpy(&g_app_state.current_track, t, sizeof(AppTrack));
                g_app_state.has_current_track = true;
                if (t->cover_url[0]) cover_art_load(t->cover_url, g_app_state.cover_mode);
            }
            break;
        }
        default: break;
    }
}