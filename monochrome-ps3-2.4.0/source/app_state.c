/*
 * Monochrome PS3 - Application State (pure state, no graphics calls)
 * Phase 1: State decoupling + 1-bit asset pipeline preparation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "app_state.h"
#include "monochrome.h"
#include "http_client.h"

#define CONFIG_PATH "/dev_hdd0/game/MONO00024/USRDIR/config.cfg"

AppContext g_app_state;

/* ── Helpers internos ────────────────────────────────────────────── */

static bool track_is_empty(const AppTrack *t)
{
    return t->id[0] == '\0';
}

static void track_clear(AppTrack *t)
{
    memset(t, 0, sizeof(*t));
}

static int find_recent_by_id(const char *track_id)
{
    for (uint32_t i = 0; i < g_app_state.recent_count; i++) {
        if (strcmp(g_app_state.recent_tracks[i].id, track_id) == 0)
            return (int)i;
    }
    return -1;
}

/* ── Inicialización ──────────────────────────────────────────────── */

void app_state_init(void)
{
    memset(&g_app_state, 0, sizeof(g_app_state));

    /* Navegación por defecto */
    g_app_state.active_section   = SEC_HOME;
    g_app_state.active_top_tab   = TAB_ALL;
    g_app_state.focus_zone       = ZONE_MAIN_CONTENT;
    g_app_state.left_rail_expanded = true;
    g_app_state.right_drawer_open  = false;
    g_app_state.visualizer_fullscreen = false;

    /* Catálogo vacío */
    g_app_state.track_count  = 0;
    g_app_state.album_count  = 0;
    g_app_state.artist_count = 0;
    g_app_state.playlist_count = 0;
    g_app_state.recent_count = 0;

    /* Cola de reproducción vacía */
    g_app_state.queue_count = 0;
    g_app_state.queue_index = 0;

    /* Sin pista actual */
    g_app_state.has_current_track = false;
    track_clear(&g_app_state.current_track);

    /* Búsqueda inactiva */
    g_app_state.search_query[0] = '\0';
    g_app_state.search_active   = false;
    g_app_state.search_error    = false;

    /* Configuración por defecto */
    g_app_state.cover_mode       = COVER_MODE_ORIGINAL;
    g_app_state.bg_theme         = BG_THEME_FRACTURE_HUD;
    g_app_state.overscan_x       = 0;
    g_app_state.overscan_y       = 0;
    g_app_state.overscan_scale   = 1.0f;
    g_app_state.http_timeout_ms  = 10000;
    http_set_timeout_ms((int)g_app_state.http_timeout_ms);
}

/* ── Persistencia de configuración ───────────────────────────────── */

bool app_state_load_config(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f)
        return false;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* Saltar comentarios y líneas vacías */
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '#' || line[0] == ';')
            continue;

        char key[64] = {0};
        char val[192] = {0};
        if (sscanf(line, "%63[^=]=%191[^\n\r]", key, val) != 2)
            continue;

        if (strcmp(key, "cover_mode") == 0) {
            g_app_state.cover_mode = COVER_MODE_ORIGINAL;
        } else if (strcmp(key, "bg_theme") == 0) {
            int t = atoi(val);
            g_app_state.bg_theme = (t < 0 || t >= BG_THEME_COUNT)
                ? BG_THEME_FRACTURE_HUD : (BgThemeMode)t;
        } else if (strcmp(key, "overscan_x") == 0) {
            g_app_state.overscan_x = atoi(val);
        } else if (strcmp(key, "overscan_y") == 0) {
            g_app_state.overscan_y = atoi(val);
        } else if (strcmp(key, "overscan_scale") == 0) {
            g_app_state.overscan_scale = (float)atof(val);
            if (g_app_state.overscan_scale <= 0.0f) g_app_state.overscan_scale = 1.0f;
        } else if (strcmp(key, "http_timeout_ms") == 0) {
            int t = atoi(val);
            if (t < 2000) t = 10000;
            if (t > 60000) t = 60000;
            g_app_state.http_timeout_ms = (uint32_t)t;
        }
    }

    fclose(f);
    http_set_timeout_ms((int)g_app_state.http_timeout_ms);
    return true;
}

bool app_state_save_config(void)
{
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f)
        return false;

    fprintf(f, "# Monochrome PS3 - Configuracion persistente\n");
    fprintf(f, "cover_mode=%d\n", (int)g_app_state.cover_mode);
    fprintf(f, "bg_theme=%d\n", (int)g_app_state.bg_theme);
    fprintf(f, "overscan_x=%d\n", g_app_state.overscan_x);
    fprintf(f, "overscan_y=%d\n", g_app_state.overscan_y);
    fprintf(f, "overscan_scale=%.3f\n", g_app_state.overscan_scale);
    fprintf(f, "http_timeout_ms=%u\n", g_app_state.http_timeout_ms);

    fclose(f);
    return true;
}

/* ── Catálogo reciente ───────────────────────────────────────────── */

void app_state_add_recent(const char *track_id, const char *title)
{
    if (!track_id || !title)
        return;

    int idx = find_recent_by_id(track_id);
    if (idx >= 0) {
        /* Mover al frente (más reciente primero) */
        AppTrack tmp = g_app_state.recent_tracks[idx];
        memmove(&g_app_state.recent_tracks[1], &g_app_state.recent_tracks[0],
                (size_t)idx * sizeof(AppTrack));
        g_app_state.recent_tracks[0] = tmp;
        return;
    }

    /* Insertar al frente, desplazando */
    if (g_app_state.recent_count >= MAX_SEARCH_RESULTS) {
        g_app_state.recent_count = MAX_SEARCH_RESULTS - 1;
    }
    memmove(&g_app_state.recent_tracks[1], &g_app_state.recent_tracks[0],
            g_app_state.recent_count * sizeof(AppTrack));
    g_app_state.recent_count++;

    track_clear(&g_app_state.recent_tracks[0]);
    strncpy(g_app_state.recent_tracks[0].id, track_id, sizeof(g_app_state.recent_tracks[0].id) - 1);
    strncpy(g_app_state.recent_tracks[0].title, title, sizeof(g_app_state.recent_tracks[0].title) - 1);
}

/* ── Cola de reproducción ────────────────────────────────────────── */

bool app_state_queue_push(const AppTrack *track)
{
    if (!track || track_is_empty(track))
        return false;
    if (g_app_state.queue_count >= MAX_SEARCH_RESULTS)
        return false;

    g_app_state.queue[g_app_state.queue_count] = *track;
    g_app_state.queue_count++;
    return true;
}

bool app_state_queue_next(AppTrack *out)
{
    if (!out || g_app_state.queue_count == 0)
        return false;

    g_app_state.queue_index++;
    if (g_app_state.repeat_mode == REPEAT_ONE) {
        /* Misma pista */
    } else if (g_app_state.queue_index >= g_app_state.queue_count) {
        if (g_app_state.repeat_mode == REPEAT_ALL)
            g_app_state.queue_index = 0;
        else
            g_app_state.queue_index = g_app_state.queue_count - 1;
    }

    if (g_app_state.queue_index >= g_app_state.queue_count)
        return false;

    *out = g_app_state.queue[g_app_state.queue_index];
    g_app_state.current_track = *out;
    g_app_state.has_current_track = true;
    return true;
}

bool app_state_queue_prev(AppTrack *out)
{
    if (!out || g_app_state.queue_count == 0)
        return false;

    if (g_app_state.queue_index == 0) {
        if (g_app_state.repeat_mode == REPEAT_ALL)
            g_app_state.queue_index = g_app_state.queue_count - 1;
        else
            return false;
    } else {
        g_app_state.queue_index--;
    }

    *out = g_app_state.queue[g_app_state.queue_index];
    g_app_state.current_track = *out;
    g_app_state.has_current_track = true;
    return true;
}

/* ── Navegación ──────────────────────────────────────────────────── */

void app_state_set_section(MainSection section)
{
    g_app_state.active_section = section;
}

void app_state_set_top_tab(TopFilterTab tab)
{
    g_app_state.active_top_tab = tab;
}

void app_state_set_focus_zone(FocusZone zone)
{
    g_app_state.focus_zone = zone;
}

const AppTrack *app_state_get_tab_items(TopFilterTab tab, int *out_count, const char **out_heading)
{
    const AppTrack *items = g_app_state.tracks;
    int count = (int)g_app_state.track_count;
    const char *heading = "PISTAS DISPONIBLES //";

    switch (tab) {
        case TAB_ALBUMS:
            items = g_app_state.albums;
            count = (int)g_app_state.album_count;
            heading = "ALBUMES DISPONIBLES //";
            break;
        case TAB_ARTISTS:
            items = g_app_state.artists;
            count = (int)g_app_state.artist_count;
            heading = "ARTISTAS DISPONIBLES //";
            break;
        case TAB_PLAYLISTS:
            items = g_app_state.playlists;
            count = (int)g_app_state.playlist_count;
            heading = "PLAYLISTS DISPONIBLES //";
            break;
        case TAB_TRACKS:
            items = g_app_state.tracks;
            count = (int)g_app_state.track_count;
            heading = "PISTAS DISPONIBLES //";
            break;
        case TAB_ALL:
        default:
            if (g_app_state.track_count > 0) {
                items = g_app_state.tracks;
                count = (int)g_app_state.track_count;
            } else {
                items = g_app_state.albums;
                count = (int)g_app_state.album_count;
            }
            heading = "PISTAS DISPONIBLES //";
            break;
    }

    if (out_count) *out_count = count;
    if (out_heading) *out_heading = heading;
    return items;
}
