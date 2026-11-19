#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "monochrome.h"

/* ── Track metadata (lightweight copy for the app context) ───────── */
typedef struct {
    char     id[64];
    char     title[128];
    char     artist[128];
    char     album[128];
    char     cover_url[256];
    char     stream_url[512];
    uint32_t duration_sec;
    uint32_t sample_rate;
    uint8_t  bit_depth;
    bool     is_hi_res;
} AppTrack;

/* ── Application context: pure state, no graphics calls ──────────── */
typedef struct {
    /* Navigation */
    MainSection      active_section;
    TopFilterTab     active_top_tab;
    FocusZone        focus_zone;
    bool             left_rail_expanded;
    bool             right_drawer_open;
    bool             visualizer_fullscreen;

    /* Dynamic catalog from the API */
    AppTrack         tracks[MAX_SEARCH_RESULTS];
    uint32_t         track_count;
    AppTrack         albums[MAX_SEARCH_RESULTS];
    uint32_t         album_count;
    AppTrack         artists[MAX_SEARCH_RESULTS];
    uint32_t         artist_count;
    AppTrack         playlists[MAX_SEARCH_RESULTS];
    uint32_t         playlist_count;
    AppTrack         recent_tracks[MAX_SEARCH_RESULTS];
    uint32_t         recent_count;

    /* Playback queue (real playback queue) */
    AppTrack         queue[MAX_SEARCH_RESULTS];
    uint32_t         queue_count;
    uint32_t         queue_index;

    /* Current track & playback */
    AppTrack         current_track;
    bool             has_current_track;
    RepeatMode       repeat_mode;
    bool             shuffle;

    /* Search */
    char             search_query[128];
    bool             search_active;
    bool             search_error;

    /* Persistent configuration */
    CoverArtMode     cover_mode;
    int              overscan_x;
    int              overscan_y;
    float            overscan_scale;
    uint32_t         http_timeout_ms;
} AppContext;

/* ── Public API ──────────────────────────────────────────────────── */
extern AppContext g_app_state;

void app_state_init(void);
bool app_state_load_config(void);
bool app_state_save_config(void);
void app_state_add_recent(const char *track_id, const char *title);
bool app_state_queue_push(const AppTrack *track);
bool app_state_queue_next(AppTrack *out);
bool app_state_queue_prev(AppTrack *out);
void app_state_set_section(MainSection section);
void app_state_set_top_tab(TopFilterTab tab);
void app_state_set_focus_zone(FocusZone zone);

#endif /* APP_STATE_H */
