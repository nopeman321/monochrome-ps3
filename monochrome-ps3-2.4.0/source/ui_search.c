#include "ui_search.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "monochrome.h"
#include "app_state.h"
#include "gfx_engine.h"
#include "ui_theme.h"
#include "ui_components.h"
#include "ui_shell.h"
#include "audio_engine.h"
#include "cover_art.h"
#include "http_client.h"
#include "platform.h"

static char s_query[128] = "";
static TrackMetadata s_results[MAX_SEARCH_RESULTS];
static int s_count = 0;
static int s_cursor = 0;
static bool s_busy = false;
static bool s_focus_in_results = false;
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

static int  s_last_played_idx = -1;
static bool s_result_failed[MAX_SEARCH_RESULTS];
static bool s_fallback_exhausted = false;

/* El teclado permanece siempre visible en la pantalla Search (no se puede ocultar) */
bool ui_search_is_keyboard_open(void) {
    return true;
}

void ui_search_init(void) {
    memset(s_results, 0, sizeof(s_results));
    memset(s_result_failed, 0, sizeof(s_result_failed));
    s_last_played_idx = -1;
    s_fallback_exhausted = false;
    s_count = 0; s_cursor = 0; s_busy = false; s_error = false;
    s_query[0] = '\0';
    s_focus_in_results = false;
    kb_row = 0; kb_col = 0;
    s_album_count = 0; s_artist_count = 0; s_playlist_count = 0;
    s_last_tab = TAB_ALL;
    s_drilldown_tracks = false;
    s_drilldown_title[0] = '\0';
}

void ui_search_start_initial(const char *query) {
    if (!query || query[0] == '\0') query = "daft";
    s_busy = true;
    s_error = false;
    s_fallback_exhausted = false;
    s_drilldown_tracks = false;
    s_drilldown_title[0] = '\0';
    http_search_async_start(query);
}

static void do_search(void) {
    if (s_query[0] == '\0') return;
    s_busy = true;
    s_count = 0;
    s_cursor = 0;
    s_error = false;
    s_fallback_exhausted = false;
    s_focus_in_results = true;
    s_drilldown_tracks = false;
    s_drilldown_title[0] = '\0';
    s_last_played_idx = -1;
    memset(s_result_failed, 0, sizeof(s_result_failed));
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
        at.sample_rate = t->sample_rate;
        at.bit_depth = t->bit_depth;
        at.is_hi_res = t->is_hi_res;
        memcpy(&g_app_state.current_track, &at, sizeof(AppTrack));
        g_app_state.has_current_track = true;
        app_state_add_recent(t->id, t->title);
        app_state_queue_push(&at);
        if (t->cover_url[0]) cover_art_load(t->cover_url, g_app_state.cover_mode);
    }
}

static void play_track_at_index(int idx) {
    if (idx < 0 || idx >= s_count) return;
    s_fallback_exhausted = false;
    s_last_played_idx = idx;
    TrackMetadata *t = &s_results[idx];
    play_track(t);
}

static int get_active_results_count(void) {
    if (s_drilldown_tracks) return s_count;
    if (g_app_state.active_top_tab == TAB_ALBUMS) return s_album_count;
    if (g_app_state.active_top_tab == TAB_ARTISTS) return s_artist_count;
    if (g_app_state.active_top_tab == TAB_PLAYLISTS) return s_playlist_count;
    return s_count;
}

void ui_search_update(void) {
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

            /* Sincronizar catálogo global / buffer de pistas para Home y Library */
            int sync_tracks = (s_count > MAX_SEARCH_RESULTS) ? MAX_SEARCH_RESULTS : (s_count < 0 ? 0 : s_count);
            g_app_state.track_count = (uint32_t)sync_tracks;
            for (int i = 0; i < sync_tracks; i++) {
                memset(&g_app_state.tracks[i], 0, sizeof(AppTrack));
                strncpy(g_app_state.tracks[i].id, s_results[i].id, sizeof(g_app_state.tracks[i].id)-1);
                strncpy(g_app_state.tracks[i].title, s_results[i].title, sizeof(g_app_state.tracks[i].title)-1);
                strncpy(g_app_state.tracks[i].artist, s_results[i].artist, sizeof(g_app_state.tracks[i].artist)-1);
                strncpy(g_app_state.tracks[i].album, s_results[i].album, sizeof(g_app_state.tracks[i].album)-1);
                strncpy(g_app_state.tracks[i].cover_url, s_results[i].cover_url, sizeof(g_app_state.tracks[i].cover_url)-1);
                g_app_state.tracks[i].duration_sec = s_results[i].duration_sec;
                g_app_state.tracks[i].sample_rate = s_results[i].sample_rate;
                g_app_state.tracks[i].bit_depth = s_results[i].bit_depth;
                g_app_state.tracks[i].is_hi_res = s_results[i].is_hi_res;
            }

            if (!s_drilldown_tracks && s_album_count > 0) {
                int sync_albums = (s_album_count > MAX_SEARCH_RESULTS) ? MAX_SEARCH_RESULTS : s_album_count;
                g_app_state.album_count = (uint32_t)sync_albums;
                for (int i = 0; i < sync_albums; i++) {
                    memset(&g_app_state.albums[i], 0, sizeof(AppTrack));
                    strncpy(g_app_state.albums[i].id, s_albums[i].id, sizeof(g_app_state.albums[i].id)-1);
                    strncpy(g_app_state.albums[i].title, s_albums[i].title, sizeof(g_app_state.albums[i].title)-1);
                    strncpy(g_app_state.albums[i].artist, s_albums[i].artist, sizeof(g_app_state.albums[i].artist)-1);
                    strncpy(g_app_state.albums[i].cover_url, s_albums[i].cover_url, sizeof(g_app_state.albums[i].cover_url)-1);
                }
            }
        } else if (st == SEARCH_STATUS_NO_RESULTS || st == SEARCH_STATUS_ERROR) {
            s_busy = false;
            s_count = 0;
            s_error = (st == SEARCH_STATUS_ERROR);
        }
    }

    /* Auto-fallback si la pista seleccionada devolvio 404 */
    if (s_last_played_idx >= 0 && s_last_played_idx < s_count &&
        audio_engine_has_error() && audio_engine_get_last_error() == 404) {
        s_result_failed[s_last_played_idx] = true;
        const char *failed_title = s_results[s_last_played_idx].title;
        const char *failed_artist = s_results[s_last_played_idx].artist;

        int fallback_idx = -1;
        /* 1. Buscar coincidencia exacta de titulo y artista */
        for (int i = 0; i < s_count; i++) {
            if (i != s_last_played_idx && !s_result_failed[i]) {
                if (strcasecmp(s_results[i].title, failed_title) == 0 &&
                    strcasecmp(s_results[i].artist, failed_artist) == 0) {
                    fallback_idx = i;
                    break;
                }
            }
        }
        /* 2. Si no hubo coincidencia de artista, buscar coincidencia de titulo */
        if (fallback_idx < 0) {
            for (int i = 0; i < s_count; i++) {
                if (i != s_last_played_idx && !s_result_failed[i]) {
                    if (strcasecmp(s_results[i].title, failed_title) == 0) {
                        fallback_idx = i;
                        break;
                    }
                }
            }
        }

        if (fallback_idx >= 0) {
            platform_log("[AUTO-FALLBACK] Pista #%d ('%s' [ID: %s]) devolvio 404. Probando alternativa #%d ('%s' [ID: %s])...",
                         s_last_played_idx + 1, s_results[s_last_played_idx].title, s_results[s_last_played_idx].id,
                         fallback_idx + 1, s_results[fallback_idx].title, s_results[fallback_idx].id);
            s_cursor = fallback_idx;
            play_track_at_index(fallback_idx);
        } else {
            platform_log("[AUTO-FALLBACK] No se encontraron versiones alternativas para '%s'", failed_title);
            s_last_played_idx = -1;
            audio_engine_clear_error();
            g_app_state.search_error = false;
            s_fallback_exhausted = true;
        }
    }
}

void ui_search_render(void) {
    int x0, y0, x1, y1;
    shell_get_main_area(&x0, &y0, &x1, &y1);
    int mw = x1 - x0;
    int cx = x0 + 24;
    int cy = y0 + 10;
    int content_w = mw - 48;

    /* Detección de cambio de pestaña superior */
    if (g_app_state.active_top_tab != s_last_tab) {
        s_last_tab = g_app_state.active_top_tab;
        s_cursor = 0;
        s_drilldown_tracks = false;
        s_drilldown_title[0] = '\0';
    }

    gfx_draw_text(cx, cy, "BUSQUEDA //", 15.0f, TH_ACCENT);
    if (s_fallback_exhausted) {
        gfx_draw_text(cx + 180, cy + 2, "[!] Pista no disponible (404) - selecciona otra", 12.0f, TH_ACCENT_LT);
    }
    cy += 24;

    /* Barra de búsqueda estilo terminal */
    uint32_t border = (!s_focus_in_results) ? TH_ACCENT_LT : TH_BORDER;
    int bar_h = 36;
    gfx_draw_rect(cx, cy, content_w, bar_h, TH_SURFACE_2);
    gfx_draw_rect_outline(cx, cy, content_w, bar_h, border, 1.0f);
    char qbuf[256];
    if (s_query[0]) {
        snprintf(qbuf, sizeof(qbuf), "> %s_", s_query);
    } else {
        snprintf(qbuf, sizeof(qbuf), "> Escribe un termino y presiona [START] para buscar...");
    }
    uint32_t qcolor = s_query[0] ? TH_TEXT : TH_TEXT_DIM;
    gfx_draw_text(cx + 16, cy + 10, qbuf, 14.0f, qcolor);
    cy += bar_h + 8;

    /* ── Teclado QWERTY Permanente (no se oculta jamás) ── */
    int key_w = 88, key_h = 32, gap = 8;
    int kb_total_w = 10 * key_w + 9 * gap; /* 952 px */
    int kb_x = cx + (content_w - kb_total_w) / 2;
    if (kb_x < cx) kb_x = cx;
    int kb_y = cy;

    int row_offsets[4] = { 0, 0, (kb_total_w - (9 * key_w + 8 * gap)) / 2, (kb_total_w - (7 * key_w + 6 * gap)) / 2 };

    for (int r = 0; r < 4; r++) {
        const char *row = KB_ROWS[r];
        int rowlen = (int)strlen(row);
        int rx = kb_x + row_offsets[r];
        for (int c = 0; c < rowlen; c++) {
            char ch[2] = { row[c], '\0' };
            int kx = rx + c * (key_w + gap);
            int ky = kb_y + r * (key_h + 4);
            bool focused = (!s_focus_in_results && kb_row == r && kb_col == c);
            uint32_t bg = focused ? TH_ACTIVE_BG : TH_SURFACE_2;
            uint32_t fg = focused ? TH_ACTIVE_TEXT : TH_TEXT;
            gfx_draw_rect(kx, ky, key_w, key_h, bg);
            gfx_draw_rect_outline(kx, ky, key_w, key_h, focused ? TH_ACCENT_LT : TH_BORDER, 1.0f);
            gfx_draw_text(kx + (key_w - 14) / 2, ky + (key_h - 14) / 2, ch, 14.0f, fg);
        }
    }

    /* Fila de comandos con asignaciones oficiales de mandos (12px por carácter en bitmap font) */
    int by = kb_y + 4 * (key_h + 4) + 4;
    const char *btns[4] = { "[/\\] ESPACIO", "[[]] BORRAR", "[SELECT] LIMPIAR", "[START] BUSCAR" };
    int btn_gap = 12;
    int bw = (kb_total_w - 3 * btn_gap) / 4; /* 229 px por botón: sobra margen para "[SELECT] LIMPIAR" (192 px) */
    int bh = 34;
    for (int i = 0; i < 4; i++) {
        int bx = kb_x + i * (bw + btn_gap);
        bool focused = (!s_focus_in_results && kb_row == 4 && kb_col == i);
        uint32_t bg = focused ? TH_ACTIVE_BG : TH_SURFACE_2;
        uint32_t fg = focused ? TH_ACTIVE_TEXT : (i == 3 ? TH_ACCENT_LT : TH_TEXT);
        gfx_draw_rect(bx, by, bw, bh, bg);
        gfx_draw_rect_outline(bx, by, bw, bh, focused ? TH_ACCENT_LT : (i == 3 ? TH_ACCENT : TH_BORDER), 1.0f);
        int btn_text_w = (int)strlen(btns[i]) * 12;
        gfx_draw_text(bx + (bw - btn_text_w) / 2, by + (bh - 12) / 2, btns[i], 12.0f, fg);
    }

    /* Barra de ayuda / leyenda (sin opción de ocultar teclado) */
    int leg_y = by + bh + 8;
    const char *legend = "[D-PAD] MOVER  [X] ESCRIBIR  [START] BUSCAR  [[]] BORRAR  [/\\] ESPACIO  [SELECT] LIMPIAR  [O] MENU";
    int leg_w = (int)strlen(legend) * 12;
    int leg_x = (leg_w < content_w) ? (cx + (content_w - leg_w) / 2) : cx;
    gfx_draw_text_clipped(leg_x, leg_y, (float)content_w, legend, 12.0f, TH_TEXT_DIM);

    /* Separador horizontal */
    int div_y = leg_y + 18;
    gfx_draw_rect(cx, div_y, content_w, 1, TH_BORDER);

    /* ── Área de Resultados (siempre visible bajo el teclado) ── */
    int res_y = div_y + 8;
    int res_h = (y1 - 10) - res_y;

    if (s_busy) {
        gfx_draw_rect(cx, res_y, content_w, 70, TH_SURFACE);
        gfx_draw_rect_outline(cx, res_y, content_w, 70, TH_BORDER, 1.0f);
        gfx_draw_text(cx + 24, res_y + 26, "CONSULTANDO CATALOGO FLAC EN TIEMPO REAL...", 14.0f, TH_ACCENT_LT);
        return;
    }

    if (s_error) {
        gfx_draw_rect(cx, res_y, content_w, 80, TH_SURFACE);
        gfx_draw_rect_outline(cx, res_y, content_w, 80, TH_BORDER, 1.0f);
        gfx_draw_text(cx + 24, res_y + 20, "[X] ERROR DE CONEXION CON EL SERVIDOR TRACKS", 14.0f, TH_ACCENT_LT);
        gfx_draw_text(cx + 24, res_y + 46, "Verifica tu conexion o presiona [START] para reintentar.", 12.0f, TH_TEXT_DIM);
        return;
    }

    if (s_drilldown_tracks) {
        char sub[256];
        snprintf(sub, sizeof(sub), "ALBUM: %s  (CIRCULO: VOLVER A ALBUMES)", s_drilldown_title);
        gfx_draw_text(cx, res_y, sub, 13.0f, TH_ACCENT_LT);
        res_y += 20;
        res_h -= 20;

        if (s_count == 0) {
            gfx_draw_rect(cx, res_y, content_w, 60, TH_SURFACE);
            gfx_draw_rect_outline(cx, res_y, content_w, 60, TH_BORDER, 1.0f);
            gfx_draw_text(cx + 24, res_y + 22, "Sin pistas en este album", 13.0f, TH_TEXT_DIM);
        } else {
            int row_h = 46;
            int visible = res_h / row_h;
            if (visible < 1) visible = 1;
            int start = 0;
            if (s_cursor >= visible) start = s_cursor - visible + 1;
            if (start + visible > s_count) start = s_count - visible;
            if (start < 0) start = 0;

            for (int i = 0; i < visible && (start + i) < s_count; i++) {
                int idx = start + i;
                TrackMetadata *t = &s_results[idx];
                UiComponentState st = UI_COMPONENT_STATE_DEFAULT;
                if (s_focus_in_results && idx == s_cursor) st = UI_COMPONENT_STATE_FOCUSED;
                if (g_app_state.has_current_track && strcmp(t->id, g_app_state.current_track.id) == 0)
                    st = UI_COMPONENT_STATE_PLAYING;

                AppTrack at;
                memset(&at, 0, sizeof(at));
                strncpy(at.id, t->id, sizeof(at.id)-1);
                if (s_result_failed[idx]) {
                    snprintf(at.title, sizeof(at.title), "%.118s [404]", t->title);
                } else {
                    strncpy(at.title, t->title, sizeof(at.title)-1);
                }
                strncpy(at.artist, t->artist, sizeof(at.artist)-1);
                strncpy(at.album, t->album, sizeof(at.album)-1);
                strncpy(at.cover_url, t->cover_url, sizeof(at.cover_url)-1);
                at.duration_sec = t->duration_sec;
                track_row_render((float)cx, (float)(res_y + i * row_h), (float)content_w, (float)row_h, (uint32_t)(idx + 1), &at, st);
            }
        }
    } else if (g_app_state.active_top_tab == TAB_ALBUMS) {
        if (s_album_count == 0) {
            gfx_draw_rect(cx, res_y, content_w, 60, TH_SURFACE);
            gfx_draw_rect_outline(cx, res_y, content_w, 60, TH_BORDER, 1.0f);
            gfx_draw_text(cx + 24, res_y + 22, "Sin albumes para esta busqueda", 13.0f, TH_TEXT_DIM);
        } else {
            int row_h = 42;
            int visible = res_h / row_h;
            if (visible < 1) visible = 1;
            int start = 0;
            if (s_cursor >= visible) start = s_cursor - visible + 1;
            if (start + visible > s_album_count) start = s_album_count - visible;
            if (start < 0) start = 0;

            for (int i = 0; i < visible && (start + i) < s_album_count; i++) {
                int idx = start + i;
                AlbumMetadata *a = &s_albums[idx];
                bool is_foc = (s_focus_in_results && idx == s_cursor);
                uint32_t bg = is_foc ? TH_SURFACE_2 : TH_SURFACE;
                gfx_draw_rect(cx, res_y + i*row_h, content_w, row_h - 2, bg);
                if (is_foc) {
                    gfx_draw_rect_outline(cx, res_y + i*row_h, content_w, row_h - 2, TH_BORDER_LT, 1.0f);
                    gfx_draw_rect(cx, res_y + i*row_h, 4, row_h - 2, TH_ACCENT_LT);
                } else {
                    gfx_draw_rect(cx, res_y + i*row_h + row_h - 2, content_w, 1, TH_BORDER);
                }
                char line[320];
                snprintf(line, sizeof(line), "%s - %s (%s)", a->title, a->artist, a->year);
                gfx_draw_text_clipped(cx + 16, res_y + i*row_h + 11, (float)(content_w - 32), line, 13.0f, is_foc ? TH_TEXT : TH_TEXT_DIM);
            }
        }
    } else if (g_app_state.active_top_tab == TAB_ARTISTS) {
        if (s_artist_count == 0) {
            gfx_draw_rect(cx, res_y, content_w, 60, TH_SURFACE);
            gfx_draw_rect_outline(cx, res_y, content_w, 60, TH_BORDER, 1.0f);
            gfx_draw_text(cx + 24, res_y + 22, "Sin artistas para esta busqueda", 13.0f, TH_TEXT_DIM);
        } else {
            int row_h = 42;
            int visible = res_h / row_h;
            if (visible < 1) visible = 1;
            int start = 0;
            if (s_cursor >= visible) start = s_cursor - visible + 1;
            if (start + visible > s_artist_count) start = s_artist_count - visible;
            if (start < 0) start = 0;

            for (int i = 0; i < visible && (start + i) < s_artist_count; i++) {
                int idx = start + i;
                ArtistMetadata *a = &s_artists[idx];
                bool is_foc = (s_focus_in_results && idx == s_cursor);
                uint32_t bg = is_foc ? TH_SURFACE_2 : TH_SURFACE;
                gfx_draw_rect(cx, res_y + i*row_h, content_w, row_h - 2, bg);
                if (is_foc) {
                    gfx_draw_rect_outline(cx, res_y + i*row_h, content_w, row_h - 2, TH_BORDER_LT, 1.0f);
                    gfx_draw_rect(cx, res_y + i*row_h, 4, row_h - 2, TH_ACCENT_LT);
                } else {
                    gfx_draw_rect(cx, res_y + i*row_h + row_h - 2, content_w, 1, TH_BORDER);
                }
                gfx_draw_text_clipped(cx + 16, res_y + i*row_h + 11, (float)(content_w - 32), a->name, 13.0f, is_foc ? TH_TEXT : TH_TEXT_DIM);
            }
        }
    } else if (g_app_state.active_top_tab == TAB_PLAYLISTS) {
        if (s_playlist_count == 0) {
            gfx_draw_rect(cx, res_y, content_w, 60, TH_SURFACE);
            gfx_draw_rect_outline(cx, res_y, content_w, 60, TH_BORDER, 1.0f);
            gfx_draw_text(cx + 24, res_y + 22, "Sin playlists para esta busqueda", 13.0f, TH_TEXT_DIM);
        } else {
            int row_h = 42;
            int visible = res_h / row_h;
            if (visible < 1) visible = 1;
            int start = 0;
            if (s_cursor >= visible) start = s_cursor - visible + 1;
            if (start + visible > s_playlist_count) start = s_playlist_count - visible;
            if (start < 0) start = 0;

            for (int i = 0; i < visible && (start + i) < s_playlist_count; i++) {
                int idx = start + i;
                PlaylistMetadata *p = &s_playlists[idx];
                bool is_foc = (s_focus_in_results && idx == s_cursor);
                uint32_t bg = is_foc ? TH_SURFACE_2 : TH_SURFACE;
                gfx_draw_rect(cx, res_y + i*row_h, content_w, row_h - 2, bg);
                if (is_foc) {
                    gfx_draw_rect_outline(cx, res_y + i*row_h, content_w, row_h - 2, TH_BORDER_LT, 1.0f);
                    gfx_draw_rect(cx, res_y + i*row_h, 4, row_h - 2, TH_ACCENT_LT);
                } else {
                    gfx_draw_rect(cx, res_y + i*row_h + row_h - 2, content_w, 1, TH_BORDER);
                }
                char line[256];
                snprintf(line, sizeof(line), "%s (%u tracks)", p->title, p->track_count);
                gfx_draw_text_clipped(cx + 16, res_y + i*row_h + 11, (float)(content_w - 32), line, 13.0f, is_foc ? TH_TEXT : TH_TEXT_DIM);
            }
        }
    } else {
        /* TAB_ALL / TAB_TRACKS */
        if (s_count == 0) {
            gfx_draw_rect(cx, res_y, content_w, 70, TH_SURFACE);
            gfx_draw_rect_outline(cx, res_y, content_w, 70, TH_BORDER, 1.0f);
            char no_res[256];
            if (s_query[0]) {
                snprintf(no_res, sizeof(no_res), "[!] NO SE ENCONTRARON PISTAS PARA \"%s\"", s_query);
            } else {
                snprintf(no_res, sizeof(no_res), "Escribe un termino y presiona [START] para buscar musica.");
            }
            gfx_draw_text_clipped(cx + 24, res_y + 18, (float)(content_w - 48), no_res, 14.0f, TH_TEXT);
            gfx_draw_text(cx + 24, res_y + 42, "Explora millones de canciones en calidad lossless FLAC.", 12.0f, TH_TEXT_DIM);
        } else {
            int row_h = 46;
            int visible = res_h / row_h;
            if (visible < 1) visible = 1;
            int start = 0;
            if (s_cursor >= visible) start = s_cursor - visible + 1;
            if (start + visible > s_count) start = s_count - visible;
            if (start < 0) start = 0;

            for (int i = 0; i < visible && (start + i) < s_count; i++) {
                int idx = start + i;
                TrackMetadata *t = &s_results[idx];
                UiComponentState st = UI_COMPONENT_STATE_DEFAULT;
                if (s_focus_in_results && idx == s_cursor) st = UI_COMPONENT_STATE_FOCUSED;
                if (g_app_state.has_current_track && strcmp(t->id, g_app_state.current_track.id) == 0)
                    st = UI_COMPONENT_STATE_PLAYING;

                AppTrack at;
                memset(&at, 0, sizeof(at));
                strncpy(at.id, t->id, sizeof(at.id)-1);
                if (s_result_failed[idx]) {
                    snprintf(at.title, sizeof(at.title), "%.118s [404]", t->title);
                } else {
                    strncpy(at.title, t->title, sizeof(at.title)-1);
                }
                strncpy(at.artist, t->artist, sizeof(at.artist)-1);
                strncpy(at.album, t->album, sizeof(at.album)-1);
                strncpy(at.cover_url, t->cover_url, sizeof(at.cover_url)-1);
                at.duration_sec = t->duration_sec;
                track_row_render((float)cx, (float)(res_y + i * row_h), (float)content_w, (float)row_h, (uint32_t)(idx + 1), &at, st);
            }
        }
    }
}

void ui_search_handle_input(InputAction act) {
    /* ── Comandos asignados a botones directos del mando (activos desde teclado o resultados) ── */
    /* 1. START: Buscar */
    if (act == ACT_START) {
        do_search();
        return;
    }

    /* 2. CUADRADO: Borrar carácter (Backspace) */
    if (act == ACT_SQUARE) {
        int len = (int)strlen(s_query);
        if (len > 0) {
            s_query[len - 1] = '\0';
        }
        return;
    }

    /* 3. TRIÁNGULO: Insertar Espacio */
    if (act == ACT_TRIANGLE) {
        int len = (int)strlen(s_query);
        if (len < 127) {
            s_query[len] = ' ';
            s_query[len + 1] = '\0';
        }
        return;
    }

    /* 4. SELECT: Limpiar texto de búsqueda */
    if (act == ACT_SELECT) {
        s_query[0] = '\0';
        return;
    }

    int active_count = get_active_results_count();

    if (!s_focus_in_results) {
        /* Foco en el teclado virtual */
        int rowlens[4] = { 10, 10, 9, 7 };
        switch (act) {
            case ACT_UP:
                if (kb_row > 0) kb_row--;
                else kb_row = 4;
                if (kb_row < 4 && kb_col >= rowlens[kb_row]) kb_col = rowlens[kb_row] - 1;
                break;

            case ACT_DOWN:
                if (kb_row < 4) {
                    kb_row++;
                    if (kb_row < 4 && kb_col >= rowlens[kb_row]) kb_col = rowlens[kb_row] - 1;
                } else {
                    /* Desde los botones, bajar el foco a los resultados si hay elementos */
                    if (active_count > 0) {
                        s_focus_in_results = true;
                        s_cursor = 0;
                    } else {
                        kb_row = 0;
                    }
                }
                break;

            case ACT_LEFT:
                if (kb_row < 4) {
                    if (kb_col > 0) kb_col--;
                    else {
                        /* Volver al Left Rail */
                        app_state_set_focus_zone(ZONE_LEFT_RAIL);
                        g_app_state.left_rail_expanded = true;
                    }
                } else {
                    if (kb_col > 0) kb_col--;
                    else {
                        app_state_set_focus_zone(ZONE_LEFT_RAIL);
                        g_app_state.left_rail_expanded = true;
                    }
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
                    if (len < 127) {
                        s_query[len] = (char)tolower((unsigned char)ch);
                        s_query[len + 1] = '\0';
                    }
                } else {
                    if (kb_col == 0) {
                        /* ESPACIO */
                        int len = (int)strlen(s_query);
                        if (len < 127) { s_query[len] = ' '; s_query[len + 1] = '\0'; }
                    } else if (kb_col == 1) {
                        /* BORRAR */
                        int len = (int)strlen(s_query);
                        if (len > 0) s_query[len - 1] = '\0';
                    } else if (kb_col == 2) {
                        /* LIMPIAR */
                        s_query[0] = '\0';
                    } else if (kb_col == 3) {
                        /* BUSCAR */
                        do_search();
                    }
                }
                break;
            }

            case ACT_CIRCLE:
                /* Círculo no oculta el teclado; vuelve al menú Left Rail */
                app_state_set_focus_zone(ZONE_LEFT_RAIL);
                g_app_state.left_rail_expanded = true;
                break;

            default: break;
        }
    } else {
        /* Foco en los resultados inferiores */
        switch (act) {
            case ACT_UP:
                if (s_cursor > 0) {
                    s_cursor--;
                } else {
                    /* Subir desde el primer resultado de vuelta al teclado */
                    s_focus_in_results = false;
                    kb_row = 4;
                }
                break;

            case ACT_DOWN:
                if (active_count > 0 && s_cursor + 1 < active_count) {
                    s_cursor++;
                }
                break;

            case ACT_LEFT:
                app_state_set_focus_zone(ZONE_LEFT_RAIL);
                g_app_state.left_rail_expanded = true;
                break;

            case ACT_CIRCLE:
                if (s_drilldown_tracks) {
                    s_drilldown_tracks = false;
                    s_drilldown_title[0] = '\0';
                    s_cursor = 0;
                } else {
                    /* Volver foco al teclado */
                    s_focus_in_results = false;
                    kb_row = 4;
                }
                break;

            case ACT_CROSS: {
                if (s_drilldown_tracks) {
                    if (s_count > 0 && s_cursor >= 0 && s_cursor < s_count) {
                        play_track_at_index(s_cursor);
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
                        play_track_at_index(s_cursor);
                    }
                }
                break;
            }

            default: break;
        }
    }
}
