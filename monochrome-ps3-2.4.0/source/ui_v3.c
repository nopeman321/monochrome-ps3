/*
 * Monochrome PS3 - UI v3 Shell (Phase 3 & Phase 6)
 * Implementa include/ui.h.
 * Estructura base: Left Rail + Top Tabs + Bottom Player Dock.
 * Integracion de Fullscreen Visualizer (SQUARE) y Cola de reproduccion (TRIANGLE).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sysutil/sysutil.h>

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
#include "net_hal.h"
#include "ui_shell.h"
#include "ui_player_dock.h"
#include "ui_home.h"
#include "ui_search.h"
#include "ui_settings.h"
#include "ui_library.h"
#include "ui_queue.h"
#include "ui_visualizer.h"

/* ── Estado de UI v3 ─────────────────────────────────────────────── */
static bool s_exit_requested = false;
static long s_frame = 0;
static bool s_initial_search_started = false;

/* ── ui_init ─────────────────────────────────────────────────────── */
void ui_init(void)
{
    s_exit_requested = false;
    s_frame = 0;
    s_initial_search_started = false;

    ui_home_init();
    ui_search_init();
    ui_library_init();
    ui_settings_init();
    ui_queue_init();
    ui_visualizer_init();

    platform_log("[UI V3] Shell inicializado (esperando enlace NetCtl para catalogo inicial)");
}

/* ── ui_render_splash ────────────────────────────────────────────── */
void ui_render_splash(void)
{
    int sw = VIRT_W;
    uint64_t t_start = platform_get_time_usec();
    const char *footer = APP_DISPLAY_VERSION "  //  Pulsa cualquier boton para iniciar...";
    float footer_x = (float)((sw - (int)strlen(footer) * 12) / 2);

    while ((platform_get_time_usec() - t_start) < 2200000ULL) {
        sysUtilCheckCallback();
        if (platform_should_exit()) {
            s_exit_requested = true;
            break;
        }

        float fade = fminf(1.0f, (float)(platform_get_time_usec() - t_start) / 1200000.0f);
        uint8_t a = (uint8_t)(fade * 255.0f);

        gfx_set_background_dim(fade);
        platform_prepare_frame();

        /* Boxless centered splash over the active persisted background */
        gfx_draw_text((float)((sw - 10 * 40) / 2), 430.0f,
                      "MONOCHROME", 40.0f, th_color(0xFF, 0xFF, 0xFF, a));
        gfx_draw_rect((float)((sw - 420) / 2), 492.0f, 420.0f, 1.0f,
                      th_color(0x88, 0x88, 0x88, a));
        gfx_draw_rect((float)((sw - 64) / 2), 492.0f, 64.0f, 2.0f,
                      th_color(0x9D, 0x4E, 0xDD, a));
        gfx_draw_text((float)((sw - 20 * 14) / 2), 514.0f,
                      "PS3 // LOSSLESS FLAC", 14.0f, th_color(0x9A, 0x9A, 0x9A, a));
        gfx_draw_text(footer_x, 940.0f,
                      footer, 12.0f, th_color(0x88, 0x88, 0x88, a));

        platform_flip();

        InputAction act = platform_poll_input();
        if (act != ACT_NONE) break;
        usleep(16000);
    }

    /* Always restore full background brightness before entering main UI */
    gfx_set_background_dim(1.0f);
}

/* ── ui_handle_input ─────────────────────────────────────────────── */
AppState ui_handle_input(AppState current, InputAction act)
{
    if (act == ACT_NONE) return current;

    /* Si visualizador fullscreen activo, delegar todo a ui_visualizer */
    if (g_app_state.visualizer_fullscreen) {
        ui_visualizer_handle_input(act);
        return current;
    }

    /* Si cola abierta, priorizar su input */
    if (g_app_state.right_drawer_open) {
        ui_queue_handle_input(act);
        return current;
    }

    /* En la pantalla de búsqueda con foco en contenido principal:
       delegar prioritariamente a ui_search (START = buscar, CUADRADO = borrar,
       TRIÁNGULO = espacio, SELECT = limpiar, etc.) antes de interceptores globales,
       pero permitir que L1/R1 roten las pestañas superiores en shell_handle_input */
    if (g_app_state.active_section == SEC_SEARCH && g_app_state.focus_zone == ZONE_MAIN_CONTENT &&
        act != ACT_L1 && act != ACT_R1) {
        ui_search_handle_input(act);
        return current;
    }

    /* START: en ningún contexto sale al XMB; si hay pista, conmuta reproducción/pausa */
    if (act == ACT_START) {
        if (g_app_state.has_current_track) {
            if (audio_engine_is_playing()) audio_engine_pause();
            else audio_engine_resume();
        }
        return current;
    }

    /* ACT_TRIANGLE: abrir/cerrar cola de reproduccion */
    if (act == ACT_TRIANGLE) {
        ui_queue_handle_input(act);
        return current;
    }

    /* ACT_SQUARE: toggle visualizador fullscreen */
    if (act == ACT_SQUARE) {
        ui_visualizer_handle_input(act);
        return current;
    }

    /* Delegar navegacion por zonas al shell */
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
    (void)sw;
    (void)sh;
    (void)frame;
    int x0, y0, x1, y1;
    shell_get_main_area(&x0, &y0, &x1, &y1);
    int mw = x1 - x0;
    int mh = y1 - y0;
    (void)mw;
    (void)mh;
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
    (void)state;
    s_frame = frame;
    int sw = VIRT_W;
    int sh = VIRT_H;

    /* Disparar busqueda inicial de catalogo de forma no bloqueante cuando NetCtl obtenga IP */
    if (!s_initial_search_started && net_hal_is_connected()) {
        s_initial_search_started = true;
        ui_search_start_initial("daft");
    }

    /* Si visualizador fullscreen activo, renderizar exclusivamente visualizador */
    if (g_app_state.visualizer_fullscreen) {
        ui_visualizer_render();

        /* Overlay de salida */
        if (s_exit_requested) {
            gfx_draw_rect(0, sh / 2 - 60, sw, 120, TH_ACTIVE_BG);
            gfx_draw_rect_outline(0, sh / 2 - 60, sw, 120, TH_BORDER_LT, 3);
            gfx_draw_text(sw / 2 - 120, sh / 2 - 20, "SALIENDO AL XMB...", 16.0f, TH_ACTIVE_TEXT);
        }
        return;
    }

    /* Actualizar catálogo / búsqueda asíncrona en cada frame */
    ui_search_update();

    /* Render shell (fondo + top bar + left rail + area central limpia) */
    shell_render(sw, sh, frame);

    /* Render area central (contenido de la seccion activa) */
    render_main_content(sw, sh, frame);

    /* Render Bottom Player Dock */
    player_dock_render(sw, sh, frame);

    /* Render cola de reproduccion overlay si esta abierta */
    if (g_app_state.right_drawer_open) {
        ui_queue_render();
    }

    /* Overlay de salida */
    if (s_exit_requested) {
        gfx_draw_rect(0, sh / 2 - 60, sw, 120, TH_ACTIVE_BG);
        gfx_draw_rect_outline(0, sh / 2 - 60, sw, 120, TH_BORDER_LT, 3);
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
