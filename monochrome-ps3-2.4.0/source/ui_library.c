#include "ui_library.h"
#include <stdio.h>
#include <string.h>

#include "monochrome.h"
#include "app_state.h"
#include "gfx_engine.h"
#include "ui_theme.h"
#include "ui_components.h"
#include "ui_shell.h"
#include "audio_engine.h"
#include "cover_art.h"

static int s_cursor = 0;

void ui_library_init(void) {
    s_cursor = 0;
}

void ui_library_render(void) {
    int x0, y0, x1, y1;
    shell_get_main_area(&x0, &y0, &x1, &y1);
    int mw = x1 - x0;
    int mh = y1 - y0;
    (void)mh;
    int cx = x0 + 24;
    int cy = y0 + 16;
    int content_w = mw - 48;

    if (g_app_state.active_section == SEC_LIBRARY) {
        gfx_draw_text(cx, cy, "BIBLIOTECA //", 15.0f, TH_ACCENT);
        cy += 28;

        int count = (int)g_app_state.track_count;
        if (count <= 0) {
            gfx_draw_rect(cx, cy, content_w, 110, TH_SURFACE);
            gfx_draw_rect_outline(cx, cy, content_w, 110, TH_BORDER, 1.0f);
            gfx_draw_text(cx + 24, cy + 34, "[!] NO HAY PISTAS REGISTRADAS EN TU BIBLIOTECA", 15.0f, TH_TEXT);
            gfx_draw_text(cx + 24, cy + 62, "Reproduce musica desde BUSCAR para guardarla en tu historial.", 13.0f, TH_TEXT_DIM);
            return;
        }

        int row_h = 50;
        int visible = 10;
        int start = 0;
        if (s_cursor >= visible) start = s_cursor - visible + 1;
        if (start + visible > count) start = count - visible;
        if (start < 0) start = 0;

        for (int i = 0; i < visible && (start + i) < count; i++) {
            int idx = start + i;
            AppTrack *t = &g_app_state.tracks[idx];
            UiComponentState st = UI_COMPONENT_STATE_DEFAULT;
            if (idx == s_cursor) st = UI_COMPONENT_STATE_FOCUSED;
            if (g_app_state.has_current_track && strcmp(t->id, g_app_state.current_track.id) == 0) {
                st = UI_COMPONENT_STATE_PLAYING;
            }
            track_row_render((float)cx, (float)(cy + i * row_h), (float)content_w, (float)row_h, (uint32_t)(idx + 1), t, st);
            if (t->cover_url[0] && idx == s_cursor) {
                cover_art_load(t->cover_url, g_app_state.cover_mode);
            }
        }

        gfx_draw_text(cx, y1 - 32, "[D-PAD] NAVEGAR   [X] REPRODUCIR   [L1/R1] PESTAÑAS", 12.0f, TH_TEXT_DIM);
    } else if (g_app_state.active_section == SEC_PLAYLISTS) {
        gfx_draw_text(cx, cy, "PLAYLISTS //", 15.0f, TH_ACCENT);
        cy += 28;

        int count = (int)g_app_state.playlist_count;
        if (count <= 0) {
            gfx_draw_rect(cx, cy, content_w, 110, TH_SURFACE);
            gfx_draw_rect_outline(cx, cy, content_w, 110, TH_BORDER, 1.0f);
            gfx_draw_text(cx + 24, cy + 34, "[!] NO HAY PLAYLISTS GUARDADAS", 15.0f, TH_TEXT);
            gfx_draw_text(cx + 24, cy + 62, "Crea y sincroniza listas de reproduccion desde la API de Monochrome.", 13.0f, TH_TEXT_DIM);
            return;
        }

        int row_h = 50;
        int visible = 10;
        int start = 0;
        if (s_cursor >= visible) start = s_cursor - visible + 1;
        if (start + visible > count) start = count - visible;
        if (start < 0) start = 0;

        for (int i = 0; i < visible && (start + i) < count; i++) {
            int idx = start + i;
            AppTrack *t = &g_app_state.playlists[idx];
            UiComponentState st = UI_COMPONENT_STATE_DEFAULT;
            if (idx == s_cursor) st = UI_COMPONENT_STATE_FOCUSED;
            if (g_app_state.has_current_track && strcmp(t->id, g_app_state.current_track.id) == 0) {
                st = UI_COMPONENT_STATE_PLAYING;
            }
            track_row_render((float)cx, (float)(cy + i * row_h), (float)content_w, (float)row_h, (uint32_t)(idx + 1), t, st);
            if (t->cover_url[0] && idx == s_cursor) {
                cover_art_load(t->cover_url, g_app_state.cover_mode);
            }
        }

        gfx_draw_text(cx, y1 - 32, "[D-PAD] NAVEGAR   [X] REPRODUCIR   [L1/R1] PESTAÑAS", 12.0f, TH_TEXT_DIM);
    } else if (g_app_state.active_section == SEC_INSTANCES) {
        gfx_draw_text(cx, cy, "INSTANCIAS //", 15.0f, TH_ACCENT);
        cy += 28;
        gfx_draw_rect(cx, cy, content_w, 110, TH_SURFACE);
        gfx_draw_rect_outline(cx, cy, content_w, 110, TH_BORDER, 1.0f);
        gfx_draw_text(cx + 24, cy + 34, "NODOS MONOCHROME ACTIVOS", 15.0f, TH_TEXT);
        gfx_draw_text(cx + 24, cy + 62, "tracks.monochrome.st [ONLINE - FLAC LOSSLESS STREAMING]", 13.0f, TH_ACCENT_LT);
    }
}

void ui_library_handle_input(InputAction act) {
    int count = 0;
    if (g_app_state.active_section == SEC_LIBRARY) count = (int)g_app_state.track_count;
    else if (g_app_state.active_section == SEC_PLAYLISTS) count = (int)g_app_state.playlist_count;

    switch (act) {
        case ACT_UP:
            if (s_cursor > 0) s_cursor--;
            break;
        case ACT_DOWN:
            if (count > 0 && s_cursor + 1 < count) s_cursor++;
            break;
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
