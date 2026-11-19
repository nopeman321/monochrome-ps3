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
#include "ui_search.h"
#include "ui_settings.h"
#include "ui_library.h"

/* ── Estado de UI v3 ─────────────────────────────────────────────── */
static bool s_exit_requested = false;
static long s_frame = 0;

/* ── ui_init ─────────────────────────────────────────────────────── */
void ui_init(void)
{
    s_exit_requested = false;
    s_frame = 0;

    ui_home_init();
    ui_search_init();
    ui_library_init();
    ui_settings_init();

    /* Inicializar red */
    http_net_init();

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

    /* Si teclado de busqueda abierto, delegar todo a ui_search antes del shell */
    if (g_app_state.active_section == SEC_SEARCH && g_app_state.focus_zone == ZONE_MAIN_CONTENT && ui_search_is_keyboard_open()) {
        ui_search_handle_input(act);
        return current;
    }

    /* Delegar navegación por zonas al shell */
    shell_handle_input(act);

    /* Delegar input a las vistas de contenido solo con foco en MAIN_CONTENT */
    if (g_app_state.focus_zone == ZONE_MAIN_CONTENT) {
        if (g_app_state.active_section == SEC_HOME) ui_home_handle_input(act);
        else if (g_app_state.active_section == SEC_SEARCH) ui_search_handle_input(act);
        else if (g_app_state.active_section == SEC_SETTINGS) ui_settings_handle_input(act);
        else if (g_app_state.active_section == SEC_LIBRARY || g_app_state.active_section == SEC_PLAYLISTS || g_app_state.active_section == SEC_INSTANCES) ui_library_handle_input(act);
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
        case SEC_INSTANCES:
            ui_library_render();
            break;
        case SEC_SEARCH: {
            ui_search_render();
            break;
        }
        case SEC_SETTINGS:
            ui_settings_render();
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