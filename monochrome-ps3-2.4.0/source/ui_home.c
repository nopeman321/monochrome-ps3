#include "ui_home.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "monochrome.h"
#include "app_state.h"
#include "gfx_engine.h"
#include "ui_theme.h"
#include "ui_components.h"
#include "ui_shell.h"
#include "ui_search.h"
#include "cover_art.h"
#include "audio_engine.h"

static int s_lane_cursor = 0;
static int s_hero_index = 0;
static TopFilterTab s_last_tab = TAB_ALL;

void ui_home_init(void) {
    s_lane_cursor = 0;
    s_hero_index = 0;
    s_last_tab = TAB_ALL;
}

static void render_hero(int x, int y, int w, int h, const AppTrack *track) {
    gfx_draw_rect(x, y, w, h, TH_SURFACE);
    gfx_draw_rect_outline(x, y, w, h, TH_BORDER, 1.0f);
    gfx_draw_text(x + 20, y + 14, "DESTACADO //", 13.0f, TH_ACCENT);

    int cx = x + 24;
    int cy = y + 36;
    int csize = 180;
    if (track && track->cover_url[0]) {
        GfxTexture tex;
        if (cover_art_get_texture(track->cover_url, &tex) && tex.uploaded) {
            gfx_draw_sprite(&tex, cx, cy, csize, csize, 0, 0, 1, 1, 0xFFFFFFFF);
            gfx_draw_rect_outline(cx, cy, csize, csize, TH_BORDER, 1.0f);
        } else {
            gfx_draw_rect(cx, cy, csize, csize, TH_SURFACE_2);
            gfx_draw_rect_outline(cx, cy, csize, csize, TH_BORDER, 1.0f);
            char init[4] = {0};
            if (track->title[0]) init[0] = (char)toupper((unsigned char)track->title[0]);
            else init[0] = '?';
            int init_size = 48;
            gfx_draw_text(cx + (csize - init_size) / 2, cy + (csize - init_size) / 2, init, (float)init_size, TH_TEXT_DIM);
            cover_art_load(track->cover_url, g_app_state.cover_mode);
        }
    } else {
        gfx_draw_rect(cx, cy, csize, csize, TH_SURFACE_2);
        gfx_draw_rect_outline(cx, cy, csize, csize, TH_BORDER, 1.0f);
        int mc_size = 48;
        gfx_draw_text(cx + (csize - mc_size * 2) / 2, cy + (csize - mc_size) / 2, "MC", (float)mc_size, TH_TEXT_DIM);
    }

    const char *badge = audio_engine_is_playing() ? "[ EN REPRODUCCION ]" : "[ LISTO // FLAC ]";
    uint32_t badge_color = audio_engine_is_playing() ? TH_ACCENT_LT : TH_TEXT_DIM;
    gfx_draw_text(cx + csize + 28, cy + 8, badge, 12.0f, badge_color);

    const char *title = (track && track->title[0]) ? track->title : "Monochrome Music";
    const char *artist = (track && track->artist[0]) ? track->artist : "PS3 High Resolution Audio";
    const char *album = (track && track->album[0]) ? track->album : "Lossless FLAC Streamer";

    gfx_draw_text_clipped(cx + csize + 28, cy + 32, (float)(w - csize - 80), title, 22.0f, TH_TEXT);
    gfx_draw_text_clipped(cx + csize + 28, cy + 64, (float)(w - csize - 80), artist, 16.0f, TH_TEXT_DIM);
    gfx_draw_text_clipped(cx + csize + 28, cy + 92, (float)(w - csize - 80), album, 14.0f, TH_BORDER_LT);

    /* Badges tecnicos */
    int bx = cx + csize + 28;
    int by = cy + 126;

    int b1_w = 256;
    gfx_draw_rect(bx, by, b1_w, 26, TH_SURFACE_2);
    gfx_draw_rect_outline(bx, by, b1_w, 26, TH_BORDER, 1.0f);
    gfx_draw_text(bx + 14, by + 6, "FLAC 48kHz / 24-bit", 12.0f, TH_ACCENT_LT);

    int b2_x = bx + b1_w + 16;
    int b2_w = 160;
    gfx_draw_rect(b2_x, by, b2_w, 26, TH_SURFACE_2);
    gfx_draw_rect_outline(b2_x, by, b2_w, 26, TH_BORDER, 1.0f);
    gfx_draw_text(b2_x + 14, by + 6, "BIT-PERFECT", 12.0f, TH_TEXT_DIM);

    /* Mini barras reactivas en el Hero con contenedor dedicado */
    int spec_box_x = b2_x + b2_w + 16;
    int spec_box_w = 120;
    gfx_draw_rect(spec_box_x, by, spec_box_w, 26, TH_SURFACE_2);
    gfx_draw_rect_outline(spec_box_x, by, spec_box_w, 26, TH_BORDER, 1.0f);

    bool is_playing = audio_engine_is_playing();
    float raw_vis[16] = {0.0f};
    if (is_playing) {
        audio_engine_get_vis(raw_vis, 16);
    }

    int spec_x = spec_box_x + 13;
    int spec_y = by + 4;
    for (int b = 0; b < 16; b++) {
        float band = raw_vis[b];
        if (band > 1.0f) band = 1.0f;
        if (band < 0.0f) band = 0.0f;

        int bar_h = is_playing ? ((int)(band * 18.0f) + 2) : 2;
        if (bar_h > 18) bar_h = 18;
        if (bar_h < 2) bar_h = 2;

        uint32_t bar_col = is_playing ? TH_ACCENT : TH_TEXT_DIM;
        gfx_draw_rect(spec_x + b * 6, spec_y + (18 - bar_h), 4, bar_h, bar_col);
        if (is_playing && bar_h > 4) {
            gfx_draw_rect(spec_x + b * 6, spec_y + (18 - bar_h), 4, 2, TH_ACCENT_LT);
        }
    }
}

static void render_lanes(int x, int y, int w, int h) {
    int count = 0;
    const char *heading = NULL;
    const AppTrack *items = app_state_get_tab_items(g_app_state.active_top_tab, &count, &heading);

    if (count <= 0) {
        gfx_draw_rect(x, y, w, 110, TH_SURFACE);
        gfx_draw_rect_outline(x, y, w, 110, TH_BORDER, 1.0f);
        gfx_draw_text(x + 24, y + 28, "[ ! ] NO HAY ELEMENTOS EN ESTA PESTANA", 16.0f, TH_TEXT);
        gfx_draw_text(x + 24, y + 64, "Cambia de pestana con [L1/R1] o ve a BUSCAR para explorar catalogo.", 13.0f, TH_TEXT_DIM);
        return;
    }

    if (s_lane_cursor >= count) s_lane_cursor = count - 1;
    if (s_lane_cursor < 0) s_lane_cursor = 0;

    gfx_draw_text(x, y - 24, heading ? heading : "PISTAS DISPONIBLES //", 15.0f, TH_TEXT);

    int row_h = 50;
    int visible = (h - 30) / row_h;
    if (visible < 4) visible = 4;
    if (visible > 10) visible = 10;

    int start = 0;
    if (s_lane_cursor >= visible) start = s_lane_cursor - visible + 1;
    if (start + visible > count) start = count - visible;
    if (start < 0) start = 0;

    for (int i = 0; i < visible && (start + i) < count; i++) {
        int idx = start + i;
        const AppTrack *t = &items[idx];

        UiComponentState st = UI_COMPONENT_STATE_DEFAULT;
        if (idx == s_lane_cursor) st = UI_COMPONENT_STATE_FOCUSED;
        if (g_app_state.has_current_track && t && strcmp(t->id, g_app_state.current_track.id) == 0) {
            st = UI_COMPONENT_STATE_PLAYING;
        }

        track_row_render((float)x, (float)(y + i * row_h), (float)w, (float)row_h, (uint32_t)(idx + 1), t, st);

        if (t && t->cover_url[0] && idx == s_lane_cursor) {
            cover_art_load(t->cover_url, g_app_state.cover_mode);
        }
    }
}

void ui_home_render(void) {
    if (g_app_state.active_top_tab != s_last_tab) {
        s_last_tab = g_app_state.active_top_tab;
        s_lane_cursor = 0;
    }

    int x0, y0, x1, y1;
    shell_get_main_area(&x0, &y0, &x1, &y1);
    int mw = x1 - x0;
    int mh = y1 - y0;

    int cx = x0 + 24;
    int cy = y0 + 16;
    int content_w = mw - 48;

    const AppTrack *hero = NULL;
    if (g_app_state.has_current_track) {
        hero = &g_app_state.current_track;
    } else if (g_app_state.track_count > 0) {
        hero = &g_app_state.tracks[s_hero_index % g_app_state.track_count];
    } else if (g_app_state.album_count > 0) {
        hero = &g_app_state.albums[s_hero_index % g_app_state.album_count];
    }

    render_hero(cx, cy, content_w, 230, hero);

    int lanes_y = cy + 260;
    int lanes_h = mh - 280;
    render_lanes(cx, lanes_y, content_w, lanes_h);
}

void ui_home_handle_input(InputAction act) {
    if (g_app_state.active_top_tab != s_last_tab) {
        s_last_tab = g_app_state.active_top_tab;
        s_lane_cursor = 0;
    }

    int count = 0;
    const AppTrack *items = app_state_get_tab_items(g_app_state.active_top_tab, &count, NULL);

    switch (act) {
        case ACT_UP:
            if (s_lane_cursor > 0) s_lane_cursor--;
            break;
        case ACT_DOWN:
            if (count > 0 && s_lane_cursor + 1 < count) s_lane_cursor++;
            break;
        case ACT_LEFT:
            if (s_lane_cursor >= 5) s_lane_cursor -= 5;
            else s_lane_cursor = 0;
            break;
        case ACT_RIGHT:
            if (count > 0) {
                if (s_lane_cursor + 5 < count) s_lane_cursor += 5;
                else s_lane_cursor = count - 1;
            }
            break;
        case ACT_CROSS: {
            if (count > 0 && s_lane_cursor >= 0 && s_lane_cursor < count) {
                const AppTrack *t = &items[s_lane_cursor];
                TopFilterTab tab = g_app_state.active_top_tab;
                if (tab == TAB_ALBUMS || (tab == TAB_ALL && g_app_state.track_count == 0)) {
                    ui_search_open_album(t->id, t->title);
                } else if (tab == TAB_ARTISTS) {
                    ui_search_open_artist(t->id, t->title);
                } else if (tab == TAB_PLAYLISTS) {
                    ui_search_open_playlist(t->title);
                } else {
                    if (g_app_state.has_current_track && strcmp(g_app_state.current_track.id, t->id) == 0) {
                        if (audio_engine_is_playing()) audio_engine_pause();
                        else audio_engine_resume();
                    } else {
                        audio_engine_play_track(t->id, t->title);
                        memcpy(&g_app_state.current_track, t, sizeof(AppTrack));
                        g_app_state.has_current_track = true;
                        app_state_add_recent(t->id, t->title);
                        app_state_queue_push(t);
                        if (t->cover_url[0]) cover_art_load(t->cover_url, g_app_state.cover_mode);
                    }
                }
            }
            break;
        }
        default: break;
    }
}
