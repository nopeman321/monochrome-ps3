/*
 * Monochrome PS3 - UI Shell v3 (Phase 3 & Phase 6)
 * Top Tabs + Left Nav Rail + coordinador de zonas
 * Layout virtual 1920x1080 con paleta monocroma + acento violeta
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
static int s_shell_frame = 0;
static int s_rail_target_w = LEFT_RAIL_COLLAPSED_W;
static int s_rail_current_w = LEFT_RAIL_COLLAPSED_W;
static bool s_rail_animating = false;

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
static void rail_update_animation(void)
{
    if (g_app_state.left_rail_expanded) {
        s_rail_target_w = LEFT_RAIL_EXPANDED_W;
    } else {
        s_rail_target_w = LEFT_RAIL_COLLAPSED_W;
    }

    if (s_rail_current_w != s_rail_target_w) {
        s_rail_animating = true;
        int step = (s_rail_target_w > s_rail_current_w) ? 12 : -12;
        s_rail_current_w += step;
        if ((step > 0 && s_rail_current_w >= s_rail_target_w) ||
            (step < 0 && s_rail_current_w <= s_rail_target_w)) {
            s_rail_current_w = s_rail_target_w;
            s_rail_animating = false;
        }
    }
}

static int rail_width(void)
{
    rail_update_animation();
    return s_rail_current_w;
}

/* ── Top Bar (y: 0..64) ──────────────────────────────────────────── */
static void render_top_bar(int sw, int sh)
{
    (void)sw;
    (void)sh;
    int virt_w = VIRT_W;
    int bar_h = TOP_BAR_H;

    /* Top bar background */
    gfx_draw_rect(0, 0, virt_w, bar_h, TH_BAR_BG);
    gfx_draw_rect(0, bar_h, virt_w, 2, TH_BORDER);

    int x = 24;
    int tab_y = 16;
    int tab_h = 36;

    /* Indicador L1 a la izquierda */
    gfx_draw_text(x, tab_y + 11, "L1", 14.0f, TH_TEXT_DIM);
    x += 44;

    /* Pestanas superiores */
    int active_tab_idx = top_tab_to_idx(g_app_state.active_top_tab);
    for (int t = 0; t < TOP_TAB_COUNT; t++) {
        int text_w = (int)strlen(s_top_tab_names[t]) * 14;
        int tw = text_w + 28;
        bool is_active = (t == active_tab_idx);

        if (is_active) {
            gfx_draw_rect(x, tab_y, tw, tab_h, TH_ACTIVE_BG);
            int ty = tab_y + (tab_h - 14) / 2;
            int tx = x + (tw - text_w) / 2;
            gfx_draw_text(tx, ty, s_top_tab_names[t], 14.0f, TH_ACTIVE_TEXT);
        } else {
            gfx_draw_rect(x, tab_y, tw, tab_h, TH_SURFACE_2);
            gfx_draw_rect_outline(x, tab_y, tw, tab_h, TH_BORDER, 1.0f);
            int ty = tab_y + (tab_h - 14) / 2;
            int tx = x + (tw - text_w) / 2;
            gfx_draw_text(tx, ty, s_top_tab_names[t], 14.0f, TH_TEXT_DIM);
        }
        x += tw + 12;
    }

    /* Indicador R1 a la derecha de las pestanas */
    gfx_draw_text(x + 4, tab_y + 11, "R1", 14.0f, TH_TEXT_DIM);

    /* Badge derecho: "PLAYSTATION 3 // MONOCHROME" */
    const char *badge_right = "PLAYSTATION 3  //  MONOCHROME";
    int badge_w = (int)strlen(badge_right) * 12;
    gfx_draw_text(virt_w - 40 - badge_w, tab_y + 4, badge_right, 12.0f, TH_TEXT_DIM);

    /* Mini-etiqueta discreta con build SHA debajo del badge */
    char sha_label[64];
    snprintf(sha_label, sizeof(sha_label), "build %s", APP_BUILD_SHA);
    int sha_w = (int)strlen(sha_label) * 12;
    gfx_draw_text(virt_w - 40 - sha_w, tab_y + 20, sha_label, 12.0f, TH_BORDER_LT);
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

    int item_y = bar_h + 20;
    int item_h = 44;
    int active_nav_idx = section_to_nav_idx(g_app_state.active_section);
    if (active_nav_idx < 0 || active_nav_idx >= NAV_ITEM_COUNT) {
        active_nav_idx = NAV_ITEM_HOME;
    }

    for (int i = 0; i < NAV_ITEM_COUNT; i++) {
        bool is_active = (i == active_nav_idx);
        bool is_focused = (g_app_state.focus_zone == ZONE_LEFT_RAIL);

        /* Fondo del item */
        if (is_active) {
            gfx_draw_rect(4, item_y, rw - 8, item_h, TH_ACTIVE_BG);
            /* Left accent bar */
            gfx_draw_rect(4, item_y, 4, item_h, TH_BORDER_LT);

            if (rw > LEFT_RAIL_COLLAPSED_W) {
                gfx_draw_rect(rw - 8, item_y + 6, 4, item_h - 12, TH_ACTIVE_TEXT);
                gfx_draw_text(24, item_y + 14, s_nav_item_names[i], 16.0f, TH_ACTIVE_TEXT);
            } else {
                char icon[2] = { s_nav_item_names[i][0], '\0' };
                gfx_draw_text(34, item_y + 14, icon, 16.0f, TH_ACTIVE_TEXT);
            }
        } else if (is_focused) {
            gfx_draw_rect(4, item_y, rw - 8, item_h, TH_SURFACE_2);
            gfx_draw_rect_outline(4, item_y, rw - 8, item_h, TH_BORDER_LT, 1.0f);
            if (rw > LEFT_RAIL_COLLAPSED_W) {
                gfx_draw_text(24, item_y + 14, s_nav_item_names[i], 16.0f, TH_TEXT);
            } else {
                char icon[2] = { s_nav_item_names[i][0], '\0' };
                gfx_draw_text(34, item_y + 14, icon, 16.0f, TH_TEXT);
            }
        } else {
            if (rw > LEFT_RAIL_COLLAPSED_W) {
                gfx_draw_text(24, item_y + 14, s_nav_item_names[i], 16.0f, TH_TEXT_DIM);
            } else {
                char icon[2] = { s_nav_item_names[i][0], '\0' };
                gfx_draw_text(34, item_y + 14, icon, 16.0f, TH_TEXT_DIM);
            }
        }

        item_y += item_h + 8;
    }

    /* Indicador de ayuda al fondo del rail */
    if (rw <= LEFT_RAIL_COLLAPSED_W) {
        gfx_draw_text(34, VIRT_H - BOTTOM_DOCK_H - 40, "<", 16.0f, TH_TEXT_DIM);
    } else {
        gfx_draw_text(24, VIRT_H - BOTTOM_DOCK_H - 40, "D-PAD > ABRIR", 12.0f, TH_TEXT_DIM);
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
    gfx_set_background_dim(1.0f);
    s_shell_frame = (int)frame;

    /* Top Bar */
    render_top_bar(VIRT_W, VIRT_H);

    /* Left Nav Rail */
    render_left_rail(VIRT_W, VIRT_H);
}
