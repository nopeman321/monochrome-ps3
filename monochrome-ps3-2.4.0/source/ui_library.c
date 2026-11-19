#include "ui_library.h"
#include <stdio.h>
#include <string.h>

#include "monochrome.h"
#include "app_state.h"
#include "gfx_engine.h"
#include "ui_theme.h"
#include "ui_components.h"
#include "audio_engine.h"
#include "cover_art.h"

static int s_cursor = 0;

void ui_library_init(void) { s_cursor = 0; }

void ui_library_render(void) {
    int x0 = 260, y0 = 64, x1 = VIRT_W, y1 = VIRT_H - 96;
    int mw = x1 - x0;
    int cx = x0 + 30;
    int cy = y0 + 20;

    if (g_app_state.active_section == SEC_LIBRARY) {
        gfx_draw_text(cx, cy, "BIBLIOTECA", 18.0f, TH_TEXT);
        cy += 32;

        int count = (int)g_app_state.track_count;
        if (count <= 0) {
            gfx_draw_text(cx, cy, "Sin pistas en biblioteca", 14.0f, TH_TEXT_DIM);
            return;
        }
        int row_h = 52;
        for (int i = 0; i < count && i < 10; i++) {
            AppTrack *t = &g_app_state.tracks[i];
            UiComponentState st = UI_COMPONENT_STATE_DEFAULT;
            if (i == s_cursor) st = UI_COMPONENT_STATE_FOCUSED;
            if (g_app_state.has_current_track && strcmp(t->id, g_app_state.current_track.id) == 0)
                st = UI_COMPONENT_STATE_PLAYING;
            track_row_render((float)cx, (float)(cy + i * row_h), (float)(mw - 60), (float)row_h, (uint32_t)(i + 1), t, st);
            if (t->cover_url[0]) cover_art_load(t->cover_url, g_app_state.cover_mode);
        }
    } else if (g_app_state.active_section == SEC_PLAYLISTS) {
        gfx_draw_text(cx, cy, "PLAYLISTS", 18.0f, TH_TEXT);
        cy += 32;

        int count = (int)g_app_state.playlist_count;
        if (count <= 0) {
            gfx_draw_text(cx, cy, "Sin playlists disponibles", 14.0f, TH_TEXT_DIM);
            return;
        }
        int row_h = 52;
        for (int i = 0; i < count && i < 10; i++) {
            AppTrack *t = &g_app_state.playlists[i];
            UiComponentState st = UI_COMPONENT_STATE_DEFAULT;
            if (i == s_cursor) st = UI_COMPONENT_STATE_FOCUSED;
            if (g_app_state.has_current_track && strcmp(t->id, g_app_state.current_track.id) == 0)
                st = UI_COMPONENT_STATE_PLAYING;
            track_row_render((float)cx, (float)(cy + i * row_h), (float)(mw - 60), (float)row_h, (uint32_t)(i + 1), t, st);
            if (t->cover_url[0]) cover_art_load(t->cover_url, g_app_state.cover_mode);
        }
    } else if (g_app_state.active_section == SEC_INSTANCES) {
        gfx_draw_text(cx, cy, "INSTANCES", 18.0f, TH_TEXT);
        cy += 32;
        gfx_draw_text(cx, cy, "Sesiones exclusivas (sin datos)", 14.0f, TH_TEXT_DIM);
    }
}

void ui_library_handle_input(InputAction act) {
    int count = 0;
    if (g_app_state.active_section == SEC_LIBRARY) count = (int)g_app_state.track_count;
    else if (g_app_state.active_section == SEC_PLAYLISTS) count = (int)g_app_state.playlist_count;

    switch (act) {
        case ACT_UP:    if (s_cursor > 0) s_cursor--; break;
        case ACT_DOWN:  if (count > 0 && s_cursor + 1 < count) s_cursor++; break;
        case ACT_CROSS: {
            AppTrack *t = NULL;
            if (g_app_state.active_section == SEC_LIBRARY && s_cursor < (int)g_app_state.track_count)
                t = &g_app_state.tracks[s_cursor];
            else if (g_app_state.active_section == SEC_PLAYLISTS && s_cursor < (int)g_app_state.playlist_count)
                t = &g_app_state.playlists[s_cursor];
            if (t) {
                if (g_app_state.has_current_track && strcmp(g_app_state.current_track.id, t->id) == 0) {
                    if (audio_engine_is_playing()) audio_engine_pause();
                    else audio_engine_resume();
                } else {
                    audio_engine_play_track(t->id, t->title);
                    memcpy(&g_app_state.current_track, t, sizeof(AppTrack));
                    g_app_state.has_current_track = true;
                    if (t->cover_url[0]) cover_art_load(t->cover_url, g_app_state.cover_mode);
                }
            }
            break;
        }
        default: break;
    }
}
