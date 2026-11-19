/*
 * Monochrome PS3 - UI v3 Shell (Phase 3)
 * Nuevo punto de entrada que implementa include/ui.h.
 * Estructura base: Left Rail + Top Tabs + Bottom Player Dock.
 * Las vistas de contenido se completan en fases posteriores.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "monochrome.h"
#include "ui_theme.h"
#include "ui.h"
#include "platform.h"
#include "platform_gfx.h"
#include "gfx_engine.h"
#include "app_state.h"
#include "http_client.h"
#include "audio_engine.h"
#include "cover_art.h"
#include "ui_shell.h"
#include "ui_player_dock.h"
#include "ui_home.h"

/* ── Estado de UI v3 ─────────────────────────────────────────────── */
static bool s_exit_requested = false;
static long s_frame = 0;

/* Datos de búsqueda desde la API (vista simplificada) */
static TrackMetadata s_search_tracks[MAX_SEARCH_RESULTS];
static int s_search_count = 0;
static int s_search_cursor = 0;
static char s_search_query[128] = "daft";
static bool s_search_busy = false;

/* ── ui_init ─────────────────────────────────────────────────────── */
void ui_init(void)
{
    memset(s_search_tracks, 0, sizeof(s_search_tracks));
    s_search_count = 0;
    s_search_cursor = 0;
    s_exit_requested = false;
    s_frame = 0;

    /* Inicializar red y lanzar búsqueda inicial */
    http_net_init();
    strncpy(s_search_query, "daft", sizeof(s_search_query) - 1);
    http_search_async_start(s_search_query);
    s_search_busy = true;

    platform_log("[UI V3] Shell inicializado (Left Rail + Top Tabs + Bottom Dock)");
}

/* ── ui_render_splash ────────────────────────────────────────────── */
void ui_render_splash(void)
{
    int sw = VIRT_W;
    int sh = VIRT_H;
    uint64_t t_start = platform_get_time_usec();

    while ((platform_get_time_usec() - t_start) < 2000000ULL) {
        gfx_draw_rect(0, 0, sw, sh, TH_BG);

        /* Marco 9-slice centrado */
        int frame_w = 700;
        int frame_h = 400;
        int frame_x = (sw - frame_w) / 2;
        int frame_y = (sh - frame_h) / 2;

        /* Usar textura 9-slice si está cargada, sino rect plano */
        gfx_draw_rect(frame_x, frame_y, frame_w, frame_h, TH_SURFACE);
        gfx_draw_rect_outline(frame_x, frame_y, frame_w, frame_h, TH_ACCENT, 3);

        gfx_draw_text(frame_x + 40, frame_y + 60,
                      "MONOCHROME PS3", 24.0f, 0xFFFFFFFF);
        gfx_draw_text(frame_x + 40, frame_y + 100,
                      "Shell v3 — Left Rail + Top Tabs + Bottom Dock",
                      16.0f, 0x008A2BE2);
        gfx_draw_text(frame_x + 40, frame_y + 140,
                      APP_DISPLAY_VERSION, 16.0f, 0x00FFFFFF);
        gfx_draw_text(frame_x + 40, frame_y + 170,
                      "SHA: " APP_BUILD_SHA, 8.0f, 0x00666666);
        gfx_draw_text(frame_x + 40, frame_y + 200,
                      "Pulsa cualquier botón para continuar", 8.0f, 0x00C83838);

        platform_flip();

        InputAction act = platform_poll_input();
        if (act != ACT_NONE) break;
        usleep(16000);
    }
}

/* ── ui_handle_input ─────────────────────────────────────────────── */
AppState ui_handle_input(AppState current, InputAction act)
{
    if (act == ACT_NONE) return current;

    /* START: salida limpia */
    if (act == ACT_START) {
        s_exit_requested = true;
        return current;
    }

    /* Delegar navegación por zonas al shell */
    shell_handle_input(act);

    /* Delegar input a ui_home solo cuando el foco está en contenido */
    if ((g_app_state.active_section == SEC_HOME || g_app_state.active_section == SEC_LIBRARY || g_app_state.active_section == SEC_PLAYLISTS) &&
        g_app_state.focus_zone == ZONE_MAIN_CONTENT) {
        ui_home_handle_input(act);
    }

    /* Navegación dentro del contenido principal */
    if (g_app_state.focus_zone == ZONE_MAIN_CONTENT) {
        switch (act) {
            case ACT_CROSS:
                if (s_search_count > 0 && s_search_cursor >= 0 &&
                    s_search_cursor < s_search_count) {
                    TrackMetadata *t = &s_search_tracks[s_search_cursor];
                    audio_engine_play_track(t->id, t->title);
                    memcpy(&g_app_state.current_track, t, sizeof(AppTrack));
                    g_app_state.has_current_track = true;
                    if (t->cover_url[0] != '\0') {
                        cover_art_load(t->cover_url, g_app_state.cover_mode);
                    }
                    return STATE_PLAYING;
                }
                break;
            case ACT_UP:
                if (s_search_cursor > 0) s_search_cursor--;
                break;
            case ACT_DOWN:
                if (s_search_cursor + 1 < s_search_count) s_search_cursor++;
                break;
            case ACT_SQUARE:
                return STATE_SEARCH;
            default: break;
        }
    }

    return current;
}

/* ── Render del área central según sección activa ────────────────── */
static void render_main_content(int sw, int sh, long frame)
{
    (void)frame;
    int x0, y0, x1, y1;
    shell_get_main_area(&x0, &y0, &x1, &y1);
    int mw = x1 - x0;
    int mh = y1 - y0;
    int cx = x0 + 30;
    int cy = y0 + 20;

    switch (g_app_state.active_section) {
        case SEC_HOME:
            ui_home_render();
            break;
        case SEC_LIBRARY:
        case SEC_PLAYLISTS:
            ui_home_render();
            break;
        case SEC_SEARCH: {
            gfx_draw_text(cx, cy, "BUSQUEDA EN MONOCHROME", 16.0f, TH_TEXT);
            cy += 30;

            /* Query box */
            gfx_draw_rect(cx, cy, mw - 60, 40, TH_SURFACE_2);
            gfx_draw_rect_outline(cx, cy, mw - 60, 40, TH_BORDER, 1);
            char qbuf[256];
            snprintf(qbuf, sizeof(qbuf), "> %s_", s_search_query);
            gfx_draw_text(cx + 15, cy + 12, qbuf, 16.0f, TH_TEXT);
            cy += 60;

            /* Resultados */
            if (s_search_busy) {
                gfx_draw_text(cx, cy, "Buscando en tracks.monochrome.st...", 8.0f, TH_ACCENT);
            } else if (s_search_count == 0) {
                gfx_draw_text(cx, cy, "Sin resultados. Usa L1/R1 para cambiar pestaña.", 8.0f, TH_TEXT_DIM);
            } else {
                int row_y = cy;
                for (int i = 0; i < s_search_count && i < 8; i++) {
                    bool is_sel = (i == s_search_cursor);
                    TrackMetadata *t = &s_search_tracks[i];
                    int ry = row_y + i * 44;
                    if (is_sel) {
                        gfx_draw_rect(cx - 10, ry - 2, mw - 40, 40, TH_ACTIVE_BG);
                        gfx_draw_rect(cx - 10, ry - 2, 6, 40, TH_ACTIVE_TEXT);
                    }
                    char line[256];
                    snprintf(line, sizeof(line), "%02d  %s - %s", i + 1,
                             t->artist, t->title);
                    gfx_draw_text(cx, ry, line, 16.0f, is_sel ? TH_ACTIVE_TEXT : TH_TEXT);
                }
            }
            break;
        }
        case SEC_INSTANCES:
            gfx_draw_text(cx, cy, "INSTANCES", 16.0f, TH_TEXT);
            cy += 30;
            gfx_draw_text(cx, cy, "Sesiones exclusivas (Fase 4)", 8.0f, TH_TEXT_DIM);
            break;
        case SEC_SETTINGS:
            gfx_draw_text(cx, cy, "AJUSTES", 16.0f, TH_TEXT);
            cy += 30;
            gfx_draw_text(cx, cy, "Configuración del sistema (Fase 4)", 8.0f, TH_TEXT_DIM);
            break;
        default:
            gfx_draw_text(cx, cy, "Pantalla no disponible", 8.0f, TH_TEXT_DIM);
            break;
    }
}

/* ── ui_render ───────────────────────────────────────────────────── */
void ui_render(AppState state, long frame)
{
    s_frame = frame;
    int sw = VIRT_W;
    int sh = VIRT_H;

    /* Poll búsqueda asíncrona */
    int count = 0, hcode = 0, herrn = 0;
    SearchStatus st = http_search_async_poll(s_search_tracks, &count,
                                              MAX_SEARCH_RESULTS, &hcode, &herrn);
    if (st == SEARCH_STATUS_OK) {
        s_search_count = count;
        s_search_busy = false;
        if (s_search_cursor >= s_search_count) s_search_cursor = 0;
    } else if (st == SEARCH_STATUS_NO_RESULTS) {
        s_search_count = 0;
        s_search_busy = false;
    } else if (st == SEARCH_STATUS_ERROR) {
        s_search_count = 0;
        s_search_busy = false;
    }

    /* Render shell (fondo + top bar + left rail + área central limpia) */
    shell_render(sw, sh, frame);

    /* Render área central (contenido de la sección activa) */
    render_main_content(sw, sh, frame);

    /* Render Bottom Player Dock */
    player_dock_render(sw, sh, frame);

    /* Overlay de salida */
    if (s_exit_requested) {
        gfx_draw_rect(0, sh / 2 - 60, sw, 120, TH_ACTIVE_BG);
        gfx_draw_rect_outline(0, sh / 2 - 60, sw, 120, TH_ACCENT, 3);
        gfx_draw_text(sw / 2 - 120, sh / 2 - 20, "SALIENDO AL XMB...", 16.0f, TH_ACTIVE_TEXT);
    }
}

/* ── ui_get_state_name ───────────────────────────────────────────── */
const char *ui_get_state_name(AppState state)
{
    switch (state) {
        case STATE_HOME:        return "STATE_HOME (Home)";
        case STATE_SEARCH:      return "STATE_SEARCH (Search)";
        case STATE_LIBRARY:     return "STATE_LIBRARY (Library)";
        case STATE_RECENT:      return "STATE_RECENT (Recent)";
        case STATE_UNRELEASED:  return "STATE_UNRELEASED (Unreleased)";
        case STATE_SETTINGS:    return "STATE_SETTINGS (Settings)";
        case STATE_ABOUT:       return "STATE_ABOUT (About)";
        case STATE_DONATE:      return "STATE_DONATE (Donate)";
        case STATE_PLAYING:     return "STATE_PLAYING (Reproduciendo)";
        case STATE_QUEUE:       return "STATE_QUEUE (Cola de Reproduccion)";
        default:                return "STATE_UNKNOWN";
    }
}

/* ── ui_should_exit ──────────────────────────────────────────────── */
bool ui_should_exit(void)
{
    return s_exit_requested;
}