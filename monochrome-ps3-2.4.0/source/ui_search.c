#include "ui_search.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "monochrome.h"
#include "app_state.h"
#include "gfx_engine.h"
#include "ui_theme.h"
#include "ui_components.h"
#include "audio_engine.h"
#include "cover_art.h"
#include "http_client.h"

static char s_query[128] = "";
static TrackMetadata s_results[MAX_SEARCH_RESULTS];
static int s_count = 0;
static int s_cursor = 0;
static bool s_busy = false;
static bool s_kb_open = true;
static bool s_error = false;

static AlbumMetadata s_albums[32];
static int s_album_count = 0;
static ArtistMetadata s_artists[32];
static int s_artist_count = 0;
static PlaylistMetadata s_playlists[32];
static int s_playlist_count = 0;

static TopFilterTab s_last_tab = TAB_ALL;
static bool s_drilldown_tracks = false;
static char s_drilldown_title[128] = "";

/* Teclado QWERTY */
static const char *KB_ROWS[4] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL",
    "ZXCVBNM"
};
static int kb_row = 0;
static int kb_col = 0;

bool ui_search_is_keyboard_open(void) { return s_kb_open; }

void ui_search_init(void) {
    memset(s_results, 0, sizeof(s_results));
    s_count = 0; s_cursor = 0; s_busy = false; s_error = false;
    s_query[0] = '\0';
    s_kb_open = true;
    kb_row = 0; kb_col = 0;
    s_album_count = 0; s_artist_count = 0; s_playlist_count = 0;
    s_last_tab = TAB_ALL;
    s_drilldown_tracks = false;
    s_drilldown_title[0] = '\0';
}

static void do_search(void) {
    if (s_query[0] == '\0') return;
    s_busy = true;
    s_count = 0;
    s_cursor = 0;
    s_error = false;
    s_kb_open = false;
    s_drilldown_tracks = false;
    s_drilldown_title[0] = '\0';
    http_search_async_start(s_query);
}

static void play_track(TrackMetadata *t) {
    if (g_app_state.has_current_track &&
        strcmp(g_app_state.current_track.id, t->id) == 0) {
        if (audio_engine_is_playing()) audio_engine_pause();
        else audio_engine_resume();
    } else {
        audio_engine_play_track(t->id, t->title);
        AppTrack at;
        memset(&at, 0, sizeof(at));
        strncpy(at.id, t->id, sizeof(at.id)-1);
        strncpy(at.title, t->title, sizeof(at.title)-1);
        strncpy(at.artist, t->artist, sizeof(at.artist)-1);
        strncpy(at.album, t->album, sizeof(at.album)-1);
        strncpy(at.cover_url, t->cover_url, sizeof(at.cover_url)-1);
        at.duration_sec = t->duration_sec;
        memcpy(&g_app_state.current_track, &at, sizeof(AppTrack));
        g_app_state.has_current_track = true;
        if (t->cover_url[0]) cover_art_load(t->cover_url, g_app_state.cover_mode);
    }
}

void ui_search_render(void) {
    int x0 = 260, y0 = 64, x1 = VIRT_W, y1 = VIRT_H - 96;
    int mw = x1 - x0;
    int cx = x0 + 30;
    int cy = y0 + 20;

    if (s_busy) {
        int http_code = 0, net_err = 0;
        SearchStatus st = http_search_async_poll(s_results, &s_count, MAX_SEARCH_RESULTS, &http_code, &net_err);
        if (st == SEARCH_STATUS_OK) {
            s_busy = false;
            s_cursor = 0;
            if (!s_drilldown_tracks) {
                s_album_count = http_search_get_albums(s_albums, 32);
                s_artist_count = http_search_get_artists(s_artists, 32);
                s_playlist_count = http_search_get_playlists(s_playlists, 32);
            }
        } else if (st == SEARCH_STATUS_NO_RESULTS || st == SEARCH_STATUS_ERROR) {
            s_busy = false;
            s_count = 0;
            s_error = (st == SEARCH_STATUS_ERROR);
        }
    }

    gfx_draw_text(cx, cy, "BUSQUEDA", 18.0f, TH_TEXT);
    cy += 32;

    /* Barra de busqueda */
    uint32_t border = s_kb_open ? TH_ACCENT : TH_BORDER;
    gfx_draw_rect(cx, cy, mw - 60, 40, TH_SURFACE_2);
    gfx_draw_rect_outline(cx, cy, mw - 60, 40, border, 2);
    char qbuf[256];
    snprintf(qbuf, sizeof(qbuf), "> %s_", s_query);
    gfx_draw_text(cx + 15, cy + 12, qbuf, 16.0f, TH_TEXT);
    cy += 50;

    if (s_kb_open) {
        /* Teclado QWERTY */
        int key_w = 70, key_h = 56, gap = 8;
        int kb_x = cx;
        int kb_y = cy + 10;
        for (int r = 0; r < 4; r++) {
            const char *row = KB_ROWS[r];
            int rowlen = (int)strlen(row);
            int rx = kb_x + (4 - r) * 18;
            for (int c = 0; c < rowlen; c++) {
                char ch[2] = { row[c], '\0' };
                int kx = rx + c * (key_w + gap);
                int ky = kb_y + r * (key_h + gap);
                bool focused = (kb_row == r && kb_col == c);
                uint32_t bg = focused ? TH_ACTIVE_BG : TH_SURFACE_2;
                uint32_t fg = focused ? TH_ACTIVE_TEXT : TH_TEXT;
                gfx_draw_rect(kx, ky, key_w, key_h, bg);
                gfx_draw_rect_outline(kx, ky, key_w, key_h, TH_BORDER, 1);
                gfx_draw_text(kx + key_w/2 - 6, ky + key_h/2 - 8, ch, 18.0f, fg);
            }
        }
        int by = kb_y + 4 * (key_h + gap) + 6;
        const char *btns[4] = { "ESPACIO", "BORRAR", "LIMPIAR", "BUSCAR" };
        int bw = 170, bh = 56;
        for (int i = 0; i < 4; i++) {
            int bx = kb_x + i * (bw + 12);
            bool focused = (kb_row == 4 && kb_col == i);
            uint32_t bg = focused ? TH_ACTIVE_BG : TH_SURFACE_2;
            uint32_t fg = focused ? TH_ACTIVE_TEXT : TH_TEXT;
            gfx_draw_rect(bx, by, bw, bh, bg);
            gfx_draw_rect_outline(bx, by, bw, bh, TH_BORDER, 1);
            gfx_draw_text(bx + 12, by + bh/2 - 8, btns[i], 14.0f, fg);
        }
        cy = by + bh + 20;
        gfx_draw_text(cx, cy, "D-Pad mover, X seleccionar, O cerrar", 12.0f, TH_TEXT_DIM);
    } else {
        /* detect tab change */
        if (g_app_state.active_top_tab != s_last_tab) {
            s_last_tab = g_app_state.active_top_tab;
            s_cursor = 0;
            s_drilldown_tracks = false;
            s_drilldown_title[0] = '\0';
        }

        if (s_busy) {
            gfx_draw_text(cx, cy, "Buscando...", 14.0f, TH_ACCENT);
            cy += 30;
        } else if (s_drilldown_tracks) {
            char sub[256];
            snprintf(sub, sizeof(sub), "%s (O volver)", s_drilldown_title);
            gfx_draw_text(cx, cy, sub, 14.0f, TH_ACCENT);
            cy += 24;
            if (s_count == 0) {
                gfx_draw_text(cx, cy, "Sin resultados", 14.0f, TH_TEXT_DIM);
            } else {
                int row_y = cy;
                int row_h = 52;
                int visible = 8;
                int start = 0;
                if (s_cursor >= visible) start = s_cursor - visible + 1;
                if (start + visible > s_count) start = s_count - visible;
                if (start < 0) start = 0;
                for (int i = 0; i < visible && (start + i) < s_count; i++) {
                    int idx = start + i;
                    TrackMetadata *t = &s_results[idx];
                    UiComponentState st = UI_COMPONENT_STATE_DEFAULT;
                    if (idx == s_cursor) st = UI_COMPONENT_STATE_FOCUSED;
                    if (g_app_state.has_current_track && strcmp(t->id, g_app_state.current_track.id) == 0)
                        st = UI_COMPONENT_STATE_PLAYING;
                    AppTrack at;
                    memset(&at, 0, sizeof(at));
                    strncpy(at.id, t->id, sizeof(at.id)-1);
                    strncpy(at.title, t->title, sizeof(at.title)-1);
                    strncpy(at.artist, t->artist, sizeof(at.artist)-1);
                    strncpy(at.album, t->album, sizeof(at.album)-1);
                    strncpy(at.cover_url, t->cover_url, sizeof(at.cover_url)-1);
                    at.duration_sec = t->duration_sec;
                    track_row_render((float)cx, (float)(row_y + i*row_h), (float)(mw-60), (float)row_h, (uint32_t)(idx+1), &at, st);
                }
            }
        } else if (g_app_state.active_top_tab == TAB_ALBUMS) {
            if (s_album_count == 0) {
                gfx_draw_text(cx, cy, "Sin albumes para esta busqueda", 14.0f, TH_TEXT_DIM);
            } else {
                int row_y = cy;
                int row_h = 40;
                int visible = 10;
                int start = 0;
                if (s_cursor >= visible) start = s_cursor - visible + 1;
                if (start + visible > s_album_count) start = s_album_count - visible;
                if (start < 0) start = 0;
                for (int i = 0; i < visible && (start + i) < s_album_count; i++) {
                    int idx = start + i;
                    AlbumMetadata *a = &s_albums[idx];
                    uint32_t bg = (idx == s_cursor) ? TH_ACTIVE_BG : TH_SURFACE_2;
                    uint32_t fg = (idx == s_cursor) ? TH_ACTIVE_TEXT : TH_TEXT;
                    gfx_draw_rect(cx, row_y + i*row_h, mw - 60, row_h - 4, bg);
                    char line[256];
                    snprintf(line, sizeof(line), "%s - %s (%s)", a->title, a->artist, a->year);
                    gfx_draw_text(cx + 8, row_y + i*row_h + 10, line, 14.0f, fg);
                }
            }
        } else if (g_app_state.active_top_tab == TAB_ARTISTS) {
            if (s_artist_count == 0) {
                gfx_draw_text(cx, cy, "Sin artistas para esta busqueda", 14.0f, TH_TEXT_DIM);
            } else {
                int row_y = cy;
                int row_h = 40;
                int visible = 10;
                int start = 0;
                if (s_cursor >= visible) start = s_cursor - visible + 1;
                if (start + visible > s_artist_count) start = s_artist_count - visible;
                if (start < 0) start = 0;
                for (int i = 0; i < visible && (start + i) < s_artist_count; i++) {
                    int idx = start + i;
                    ArtistMetadata *a = &s_artists[idx];
                    uint32_t bg = (idx == s_cursor) ? TH_ACTIVE_BG : TH_SURFACE_2;
                    uint32_t fg = (idx == s_cursor) ? TH_ACTIVE_TEXT : TH_TEXT;
                    gfx_draw_rect(cx, row_y + i*row_h, mw - 60, row_h - 4, bg);
                    gfx_draw_text(cx + 8, row_y + i*row_h + 10, a->name, 14.0f, fg);
                }
            }
        } else if (g_app_state.active_top_tab == TAB_PLAYLISTS) {
            if (s_playlist_count == 0) {
                gfx_draw_text(cx, cy, "Sin playlists para esta busqueda", 14.0f, TH_TEXT_DIM);
            } else {
                int row_y = cy;
                int row_h = 40;
                int visible = 10;
                int start = 0;
                if (s_cursor >= visible) start = s_cursor - visible + 1;
                if (start + visible > s_playlist_count) start = s_playlist_count - visible;
                if (start < 0) start = 0;
                for (int i = 0; i < visible && (start + i) < s_playlist_count; i++) {
                    int idx = start + i;
                    PlaylistMetadata *p = &s_playlists[idx];
                    uint32_t bg = (idx == s_cursor) ? TH_ACTIVE_BG : TH_SURFACE_2;
                    uint32_t fg = (idx == s_cursor) ? TH_ACTIVE_TEXT : TH_TEXT;
                    gfx_draw_rect(cx, row_y + i*row_h, mw - 60, row_h - 4, bg);
                    char line[256];
                    snprintf(line, sizeof(line), "%s (%u tracks)", p->title, p->track_count);
                    gfx_draw_text(cx + 8, row_y + i*row_h + 10, line, 14.0f, fg);
                }
            }
        } else {
            /* TAB_ALL / TAB_TRACKS */
            if (s_count == 0) {
                if (s_error) gfx_draw_text(cx, cy, "Error de red - intenta de nuevo", 14.0f, TH_TEXT_DIM);
                else gfx_draw_text(cx, cy, "Sin resultados", 14.0f, TH_TEXT_DIM);
            } else {
                int row_y = cy;
                int row_h = 52;
                int visible = 8;
                int start = 0;
                if (s_cursor >= visible) start = s_cursor - visible + 1;
                if (start + visible > s_count) start = s_count - visible;
                if (start < 0) start = 0;
                for (int i = 0; i < visible && (start + i) < s_count; i++) {
                    int idx = start + i;
                    TrackMetadata *t = &s_results[idx];
                    UiComponentState st = UI_COMPONENT_STATE_DEFAULT;
                    if (idx == s_cursor) st = UI_COMPONENT_STATE_FOCUSED;
                    if (g_app_state.has_current_track && strcmp(t->id, g_app_state.current_track.id) == 0)
                        st = UI_COMPONENT_STATE_PLAYING;
                    AppTrack at;
                    memset(&at, 0, sizeof(at));
                    strncpy(at.id, t->id, sizeof(at.id)-1);
                    strncpy(at.title, t->title, sizeof(at.title)-1);
                    strncpy(at.artist, t->artist, sizeof(at.artist)-1);
                    strncpy(at.album, t->album, sizeof(at.album)-1);
                    strncpy(at.cover_url, t->cover_url, sizeof(at.cover_url)-1);
                    at.duration_sec = t->duration_sec;
                    track_row_render((float)cx, (float)(row_y + i*row_h), (float)(mw-60), (float)row_h, (uint32_t)(idx+1), &at, st);
                }
            }
        }
    }
}

void ui_search_handle_input(InputAction act) {
    if (s_kb_open) {
        int rowlens[4] = { 10, 10, 9, 7 };
        switch (act) {
            case ACT_UP:
                if (kb_row > 0) kb_row--;
                else kb_row = 4;
                if (kb_row < 4 && kb_col >= rowlens[kb_row]) kb_col = rowlens[kb_row]-1;
                break;
            case ACT_DOWN:
                if (kb_row < 4) kb_row++;
                else kb_row = 0;
                if (kb_row < 4 && kb_col >= rowlens[kb_row]) kb_col = rowlens[kb_row]-1;
                break;
            case ACT_LEFT:
                if (kb_row < 4) {
                    if (kb_col > 0) kb_col--;
                    else { kb_col = rowlens[kb_row]-1; }
                } else {
                    if (kb_col > 0) kb_col--;
                }
                break;
            case ACT_RIGHT:
                if (kb_row < 4) {
                    if (kb_col + 1 < rowlens[kb_row]) kb_col++;
                    else kb_col = 0;
                } else {
                    if (kb_col + 1 < 4) kb_col++;
                    else kb_col = 0;
                }
                break;
            case ACT_CROSS: {
                if (kb_row < 4) {
                    const char *row = KB_ROWS[kb_row];
                    char ch = row[kb_col];
                    int len = (int)strlen(s_query);
                    if (len < 127) { s_query[len] = (char)tolower((unsigned char)ch); s_query[len+1] = '\0'; }
                } else {
                    if (kb_col == 0) {
                        int len = (int)strlen(s_query);
                        if (len < 127) { s_query[len] = ' '; s_query[len+1] = '\0'; }
                    } else if (kb_col == 1) {
                        int len = (int)strlen(s_query);
                        if (len > 0) s_query[len-1] = '\0';
                    } else if (kb_col == 2) {
                        s_query[0] = '\0';
                    } else if (kb_col == 3) {
                        do_search();
                    }
                }
                break;
            }
            case ACT_CIRCLE:
                s_kb_open = false;
                break;
            default: break;
        }
    } else {
        int active_count = s_count;
        if (s_drilldown_tracks) active_count = s_count;
        else if (g_app_state.active_top_tab == TAB_ALBUMS) active_count = s_album_count;
        else if (g_app_state.active_top_tab == TAB_ARTISTS) active_count = s_artist_count;
        else if (g_app_state.active_top_tab == TAB_PLAYLISTS) active_count = s_playlist_count;
        else active_count = s_count;

        switch (act) {
            case ACT_UP:
                if (s_cursor > 0) s_cursor--;
                else s_kb_open = true;
                break;
            case ACT_DOWN:
                if (active_count > 0 && s_cursor + 1 < active_count) s_cursor++;
                break;
            case ACT_SQUARE:
                s_kb_open = true;
                break;
            case ACT_CIRCLE:
                if (s_drilldown_tracks) {
                    s_drilldown_tracks = false;
                    s_drilldown_title[0] = '\0';
                    s_cursor = 0;
                }
                break;
            case ACT_CROSS: {
                if (s_drilldown_tracks) {
                    if (s_count > 0 && s_cursor >= 0 && s_cursor < s_count) {
                        play_track(&s_results[s_cursor]);
                    }
                    break;
                }
                if (g_app_state.active_top_tab == TAB_ALBUMS) {
                    if (s_album_count > 0 && s_cursor >= 0 && s_cursor < s_album_count) {
                        AlbumMetadata *a = &s_albums[s_cursor];
                        s_busy = true;
                        s_drilldown_tracks = true;
                        strncpy(s_drilldown_title, a->title, sizeof(s_drilldown_title)-1);
                        s_drilldown_title[sizeof(s_drilldown_title)-1] = '\0';
                        s_count = 0;
                        s_cursor = 0;
                        http_fetch_album_async_start(a->id, a->title);
                    }
                } else if (g_app_state.active_top_tab == TAB_ARTISTS) {
                    if (s_artist_count > 0 && s_cursor >= 0 && s_cursor < s_artist_count) {
                        ArtistMetadata *a = &s_artists[s_cursor];
                        s_busy = true;
                        s_drilldown_tracks = true;
                        strncpy(s_drilldown_title, a->name, sizeof(s_drilldown_title)-1);
                        s_drilldown_title[sizeof(s_drilldown_title)-1] = '\0';
                        s_count = 0;
                        s_cursor = 0;
                        http_fetch_artist_async_start(a->id, a->name);
                    }
                } else {
                    if (s_count > 0 && s_cursor >= 0 && s_cursor < s_count) {
                        play_track(&s_results[s_cursor]);
                    }
                }
                break;
            }
            default: break;
        }
    }
}
