/*
 * Monochrome PS3 - UI Shell v3 (Phase 3 & Phase 6)
 * Top Tabs + Left Nav Rail + coordinador de zonas
 * Layout virtual 1920x1080 con paleta monocroma + acento seleccionable
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "monochrome.h"
#include "ui_theme.h"
#include "gfx_engine.h"
#include "app_state.h"
#include "ui_search.h"
#include "platform.h"

/* ── Estado del shell ────────────────────────────────────────────── */
static float s_rail_target_w = (float)LEFT_RAIL_COLLAPSED_W;
static float s_rail_current_w = (float)LEFT_RAIL_COLLAPSED_W;
static bool  s_rail_init = false;
static uint64_t s_shell_last_usec = 0ULL;
static float s_shell_dt = 1.0f / 60.0f;

static float s_tab_pill_x = 0.0f;
static float s_tab_pill_w = 0.0f;
static bool  s_tab_pill_init = false;

static float s_rail_active_y = 0.0f;
static bool  s_rail_active_init = false;

/* Nombres de items del rail */
static const char *s_nav_item_names[NAV_ITEM_COUNT] = {
    "SEARCH", "HOME", "LIBRARY", "PLAYLISTS", "INSTANCES", "SETTINGS"
};

/* Nombres de pestanas superiores */
static const char *s_top_tab_names[TOP_TAB_COUNT] = {
    "ALL", "ALBUMS", "ARTISTS", "PLAYLISTS", "TRACKS"
};

/* ── Helper: mapear MainSection -> indice del rail ───────────────── */
static int section_to_nav_idx(MainSection sec)
{
    switch (sec) {
        case SEC_SEARCH:   return NAV_ITEM_SEARCH;
        case SEC_HOME:     return NAV_ITEM_HOME;
        case SEC_LIBRARY:  return NAV_ITEM_LIBRARY;
        case SEC_PLAYLISTS:return NAV_ITEM_PLAYLISTS;
        case SEC_INSTANCES:return NAV_ITEM_INSTANCES;
        case SEC_SETTINGS: return NAV_ITEM_SETTINGS;
        default:           return NAV_ITEM_HOME;
    }
}

/* ── Helper: mapear TopFilterTab -> indice de pestana superior ───── */
static int top_tab_to_idx(TopFilterTab tab)
{
    switch (tab) {
        case TAB_ALL:      return TOP_TAB_ALL;
        case TAB_ALBUMS:   return TOP_TAB_ALBUMS;
        case TAB_ARTISTS:  return TOP_TAB_ARTISTS;
        case TAB_PLAYLISTS:return TOP_TAB_PLAYLISTS;
        case TAB_TRACKS:   return TOP_TAB_TRACKS;
        default:           return TOP_TAB_ALL;
    }
}

/* ── Animacion del rail (expansion/colapso suave) ────────────────── */
static void rail_update_animation(float dt)
{
    s_rail_target_w = g_app_state.left_rail_expanded
        ? (float)LEFT_RAIL_EXPANDED_W
        : (float)LEFT_RAIL_COLLAPSED_W;

    if (!s_rail_init) {
        s_rail_current_w = s_rail_target_w;
        s_rail_init = true;
    } else {
        s_rail_current_w = ui_anim_exp_lerp(s_rail_current_w, s_rail_target_w, 14.0f, dt, 0.5f);
    }
}

static int rail_width(void)
{
    return (int)(s_rail_current_w + 0.5f);
}

/* ── Top Bar (y: 0..64) ──────────────────────────────────────────── */
static void render_top_bar(int sw, int sh)
{
    (void)sw;
    (void)sh;
    int virt_w = VIRT_W;
    int bar_h = TOP_BAR_H;
    float fscale = gfx_get_font_scale();

    /* Top bar background */
    gfx_draw_rect(0, 0, virt_w, bar_h, TH_BAR_BG);
    gfx_draw_rect(0, bar_h, virt_w, 2, TH_BORDER);

    int x = 24;
    int tab_y = 16;
    int tab_h = 36;
    int scaled_text_h = (int)(14.0f * fscale);
    int label_ty = tab_y + (tab_h - scaled_text_h) / 2;

    /* Indicador L1 a la izquierda */
    gfx_draw_text(x, label_ty, "L1", 14.0f, TH_TEXT_DIM);
    x += (int)(44.0f * fscale);

    /* Calcular posiciones de pestanas superiores */
    int active_tab_idx = top_tab_to_idx(g_app_state.active_top_tab);
    int tab_x[TOP_TAB_COUNT];
    int tab_w[TOP_TAB_COUNT];
    int text_w_arr[TOP_TAB_COUNT];
    for (int t = 0; t < TOP_TAB_COUNT; t++) {
        text_w_arr[t] = (int)((float)strlen(s_top_tab_names[t]) * 14.0f * fscale);
        tab_w[t] = text_w_arr[t] + 28;
        tab_x[t] = x;
        x += tab_w[t] + 12;
    }

    /* Animar pildora activa suavemente */
    float target_px = (float)tab_x[active_tab_idx];
    float target_pw = (float)tab_w[active_tab_idx];
    if (!s_tab_pill_init) {
        s_tab_pill_x = target_px;
        s_tab_pill_w = target_pw;
        s_tab_pill_init = true;
    } else {
        s_tab_pill_x = ui_anim_exp_lerp(s_tab_pill_x, target_px, 18.0f, s_shell_dt, 0.5f);
        s_tab_pill_w = ui_anim_exp_lerp(s_tab_pill_w, target_pw, 18.0f, s_shell_dt, 0.5f);
    }

    /* Pass 1: fondos inactivos */
    for (int t = 0; t < TOP_TAB_COUNT; t++) {
        gfx_draw_rect(tab_x[t], tab_y, tab_w[t], tab_h, TH_SURFACE_2);
        gfx_draw_rect_outline(tab_x[t], tab_y, tab_w[t], tab_h, TH_BORDER, 1.0f);
    }

    /* Pildora activa deslizante */
    gfx_draw_rect(s_tab_pill_x, (float)tab_y, s_tab_pill_w, (float)tab_h, TH_ACTIVE_BG);

    /* Pass 2: etiquetas de texto */
    for (int t = 0; t < TOP_TAB_COUNT; t++) {
        bool is_active = (t == active_tab_idx);
        int tx = tab_x[t] + (tab_w[t] - text_w_arr[t]) / 2;
        gfx_draw_text(tx, label_ty, s_top_tab_names[t], 14.0f, is_active ? TH_ACTIVE_TEXT : TH_TEXT_DIM);
    }

    /* Indicador R1 a la derecha de las pestanas */
    gfx_draw_text(x + 4, label_ty, "R1", 14.0f, TH_TEXT_DIM);

    /* Badge derecho: "PLAYSTATION 3 // MONOCHROME" */
    const char *badge_right = "PLAYSTATION 3  //  MONOCHROME";
    int badge_w = (int)((float)strlen(badge_right) * 12.0f * fscale);
    gfx_draw_text(virt_w - 40 - badge_w, tab_y + 2, badge_right, 12.0f, TH_TEXT_DIM);

    /* Mini-etiqueta discreta con build SHA debajo del badge */
    char sha_label[64];
    snprintf(sha_label, sizeof(sha_label), "build %s", APP_BUILD_SHA);
    int sha_w = (int)((float)strlen(sha_label) * 12.0f * fscale);
    gfx_draw_text(virt_w - 40 - sha_w, tab_y + 22, sha_label, 12.0f, TH_BORDER_LT);
}

/* ── Left Nav Rail (x: 0..rail_w) ────────────────────────────────── */
static void render_left_rail(int sw, int sh)
{
    (void)sw;
    (void)sh;
    int rw = rail_width();
    int bar_h = TOP_BAR_H;

    /* Rail background */
    gfx_draw_rect(0, bar_h, rw, VIRT_H - bar_h - BOTTOM_DOCK_H, TH_BAR_BG);
    gfx_draw_rect(rw, bar_h, 2, VIRT_H - bar_h - BOTTOM_DOCK_H, TH_BORDER);

    int item_y0 = bar_h + 20;
    int item_h = 44;
    int item_step = item_h + 8;
    int active_nav_idx = section_to_nav_idx(g_app_state.active_section);
    if (active_nav_idx < 0 || active_nav_idx >= NAV_ITEM_COUNT) {
        active_nav_idx = NAV_ITEM_HOME;
    }

    float target_ay = (float)(item_y0 + active_nav_idx * item_step);
    if (!s_rail_active_init) {
        s_rail_active_y = target_ay;
        s_rail_active_init = true;
    } else {
        s_rail_active_y = ui_anim_exp_lerp(s_rail_active_y, target_ay, 18.0f, s_shell_dt, 0.5f);
    }

    bool show_labels = (rw >= 180);
    bool is_focused = (g_app_state.focus_zone == ZONE_LEFT_RAIL);

    /* Fondo de items inactivos con foco en rail */
    for (int i = 0; i < NAV_ITEM_COUNT; i++) {
        if (i != active_nav_idx && is_focused) {
            int iy = item_y0 + i * item_step;
            gfx_draw_rect(4, iy, rw - 8, item_h, TH_SURFACE_2);
            gfx_draw_rect_outline(4, iy, rw - 8, item_h, TH_BORDER_LT, 1.0f);
        }
    }

    /* Pildora activa deslizante en rail */
    gfx_draw_rect(4.0f, s_rail_active_y, (float)(rw - 8), (float)item_h, TH_ACTIVE_BG);
    gfx_draw_rect(4.0f, s_rail_active_y, 4.0f, (float)item_h, TH_BORDER_LT);
    if (show_labels) {
        gfx_draw_rect((float)(rw - 8), s_rail_active_y + 6.0f, 4.0f, (float)(item_h - 12), TH_ACTIVE_TEXT);
    }

    /* Etiquetas e iconos */
    for (int i = 0; i < NAV_ITEM_COUNT; i++) {
        int iy = item_y0 + i * item_step;
        bool is_active = (i == active_nav_idx);
        uint32_t col = is_active ? TH_ACTIVE_TEXT : (is_focused ? TH_TEXT : TH_TEXT_DIM);

        if (show_labels) {
            gfx_draw_text_clipped(24, iy + 14, (float)(rw - 36), s_nav_item_names[i], 16.0f, col);
        } else {
            char icon[2] = { s_nav_item_names[i][0], '\0' };
            gfx_draw_text(34, iy + 14, icon, 16.0f, col);
        }
    }

    /* Indicador de ayuda al fondo del rail */
    if (!show_labels) {
        gfx_draw_text(34, VIRT_H - BOTTOM_DOCK_H - 40, "<", 16.0f, TH_TEXT_DIM);
    } else {
        gfx_draw_text_clipped(24, VIRT_H - BOTTOM_DOCK_H - 40, (float)(rw - 32), "D-PAD > ABRIR", 12.0f, TH_TEXT_DIM);
    }
}

/* ── Area central (recorte segun zonas) ──────────────────────────── */
void shell_get_main_area(int *x0, int *y0, int *x1, int *y1)
{
    int rw = rail_width();
    *x0 = rw;
    *y0 = MAIN_AREA_Y0;
    *x1 = VIRT_W;
    *y1 = MAIN_AREA_Y1;
}

/* ── Coordinador de zonas ────────────────────────────────────────── */
void shell_handle_input(InputAction act)
{
    if (act == ACT_NONE) return;

    /* L1/R1: rotar pestanas superiores desde cualquier pantalla (excepto OSK) */
    if (act == ACT_L1) {
        int idx = top_tab_to_idx(g_app_state.active_top_tab);
        idx = (idx - 1 + TOP_TAB_COUNT) % TOP_TAB_COUNT;
        switch (idx) {
            case TOP_TAB_ALL:      app_state_set_top_tab(TAB_ALL); break;
            case TOP_TAB_ALBUMS:   app_state_set_top_tab(TAB_ALBUMS); break;
            case TOP_TAB_ARTISTS:  app_state_set_top_tab(TAB_ARTISTS); break;
            case TOP_TAB_PLAYLISTS:app_state_set_top_tab(TAB_PLAYLISTS); break;
            case TOP_TAB_TRACKS:   app_state_set_top_tab(TAB_TRACKS); break;
        }
        return;
    }
    if (act == ACT_R1) {
        int idx = top_tab_to_idx(g_app_state.active_top_tab);
        idx = (idx + 1) % TOP_TAB_COUNT;
        switch (idx) {
            case TOP_TAB_ALL:      app_state_set_top_tab(TAB_ALL); break;
            case TOP_TAB_ALBUMS:   app_state_set_top_tab(TAB_ALBUMS); break;
            case TOP_TAB_ARTISTS:  app_state_set_top_tab(TAB_ARTISTS); break;
            case TOP_TAB_PLAYLISTS:app_state_set_top_tab(TAB_PLAYLISTS); break;
            case TOP_TAB_TRACKS:   app_state_set_top_tab(TAB_TRACKS); break;
        }
        return;
    }

    /* D-Pad Left desde borde izquierdo -> foco al Left Rail (excepto teclado de busqueda o ajustes) */
    if (act == ACT_LEFT && g_app_state.focus_zone == ZONE_MAIN_CONTENT &&
        !(g_app_state.active_section == SEC_SEARCH && ui_search_is_keyboard_open()) &&
        !(g_app_state.active_section == SEC_SETTINGS)) {
        app_state_set_focus_zone(ZONE_LEFT_RAIL);
        g_app_state.left_rail_expanded = true;
        return;
    }

    /* Circulo desde Main Content -> volver al Left Rail */
    if (act == ACT_CIRCLE && g_app_state.focus_zone == ZONE_MAIN_CONTENT &&
        !(g_app_state.active_section == SEC_SEARCH && ui_search_is_keyboard_open())) {
        app_state_set_focus_zone(ZONE_LEFT_RAIL);
        g_app_state.left_rail_expanded = true;
        return;
    }

    /* D-Pad Up/Down en el rail -> cambiar seccion activa */
    if (g_app_state.focus_zone == ZONE_LEFT_RAIL && (act == ACT_UP || act == ACT_DOWN)) {
        int nav_idx = section_to_nav_idx(g_app_state.active_section);
        if (nav_idx < 0 || nav_idx >= NAV_ITEM_COUNT) nav_idx = NAV_ITEM_HOME;
        if (act == ACT_UP) nav_idx = (nav_idx - 1 + NAV_ITEM_COUNT) % NAV_ITEM_COUNT;
        else nav_idx = (nav_idx + 1) % NAV_ITEM_COUNT;
        MainSection new_sec = SEC_HOME;
        switch (nav_idx) {
            case NAV_ITEM_SEARCH:    new_sec = SEC_SEARCH; break;
            case NAV_ITEM_HOME:      new_sec = SEC_HOME; break;
            case NAV_ITEM_LIBRARY:   new_sec = SEC_LIBRARY; break;
            case NAV_ITEM_PLAYLISTS: new_sec = SEC_PLAYLISTS; break;
            case NAV_ITEM_INSTANCES: new_sec = SEC_INSTANCES; break;
            case NAV_ITEM_SETTINGS:  new_sec = SEC_SETTINGS; break;
        }
        app_state_set_section(new_sec);
        return;
    }

    /* D-Pad Right o X en el rail -> volver a Main Content */
    if ((act == ACT_RIGHT || act == ACT_CROSS || act == ACT_CIRCLE) &&
        g_app_state.focus_zone == ZONE_LEFT_RAIL) {
        app_state_set_focus_zone(ZONE_MAIN_CONTENT);
        g_app_state.left_rail_expanded = false;
        return;
    }

    /* Triangulo: toggle right drawer */
    if (act == ACT_TRIANGLE) {
        g_app_state.right_drawer_open = !g_app_state.right_drawer_open;
        return;
    }

    /* Cuadrado: toggle visualizer fullscreen */
    if (act == ACT_SQUARE) {
        g_app_state.visualizer_fullscreen = !g_app_state.visualizer_fullscreen;
        return;
    }
}

/* ── Render completo del shell (fondo + top bar + left rail) ─────── */
void shell_render(int sw, int sh, long frame)
{
    (void)sw;
    (void)sh;
    (void)frame;
    gfx_set_background_dim(1.0f);
    s_shell_dt = ui_anim_dt(&s_shell_last_usec);
    rail_update_animation(s_shell_dt);

    /* Top Bar */
    render_top_bar(VIRT_W, VIRT_H);

    /* Left Nav Rail */
    render_left_rail(VIRT_W, VIRT_H);
}
