/**
 * Monochrome Music Player - PlayStation 3 Port (PSL1GHT)
 */

#ifndef MONOCHROME_PS3_H
#define MONOCHROME_PS3_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define APP_TITLE          "Monochrome"
#define APP_VERSION        "02.40"
#define APP_DISPLAY_VERSION "v2.4.0"
#define APP_TITLEID        "MONO00024"

#define MAX_SEARCH_RESULTS 20
#define VIS_BANDS          16

/* ── Application states ──────────────────────────────────────────── */
typedef enum {
    STATE_HOME,
    STATE_SEARCH,
    STATE_LIBRARY,
    STATE_RECENT,
    STATE_UNRELEASED,
    STATE_SETTINGS,
    STATE_ABOUT,
    STATE_DONATE,
    STATE_PLAYING,
    STATE_QUEUE
} AppState;

/* ── Search Sub-Tabs (Monochrome web ecosystem) ─────────────────── */
typedef enum {
    SEARCH_TAB_TRACKS = 0,
    SEARCH_TAB_ALBUMS,
    SEARCH_TAB_ARTISTS,
    SEARCH_TAB_PLAYLISTS,
    SEARCH_TAB_PODCASTS,
    SEARCH_TAB_COUNT
} SearchTab;

/* ── Playback states ─────────────────────────────────────────────── */
typedef enum {
    PLAYBACK_STOPPED,
    PLAYBACK_BUFFERING,
    PLAYBACK_PLAYING,
    PLAYBACK_PAUSED
} PlaybackState;

/* ── Repeat & Shuffle Modes ──────────────────────────────────────── */
typedef enum {
    REPEAT_OFF = 0,
    REPEAT_ALL = 1,
    REPEAT_ONE = 2
} RepeatMode;

/* ── Track metadata ──────────────────────────────────────────────── */
typedef struct TrackMetadata {
    char     id[64];
    char     title[128];
    char     artist[128];
    char     album[128];
    char     cover_url[256];
    char     stream_url[512];
    uint32_t duration_sec;
    uint32_t sample_rate;      /* 44100, 48000, 96000, 192000 */
    uint8_t  bit_depth;        /* 16 or 24 */
    bool     is_hi_res;
} TrackMetadata;

/* Album / Release metadata */
typedef struct AlbumMetadata {
    char     id[64];
    char     title[128];
    char     artist[128];
    char     cover_url[256];
    char     year[16];
    char     type[32];
    uint32_t track_count;
} AlbumMetadata;

/* Artist metadata */
typedef struct ArtistMetadata {
    char id[64];
    char name[128];
    char avatar_url[256];
    char bio[128];
} ArtistMetadata;

/* Playlist metadata */
typedef struct PlaylistMetadata {
    char     id[64];
    char     title[128];
    char     curator[64];
    char     description[128];
    uint32_t track_count;
} PlaylistMetadata;

/* ── Controller Input Actions ────────────────────────────────────── */
typedef enum {
    ACT_NONE = 0,
    ACT_UP,
    ACT_DOWN,
    ACT_LEFT,
    ACT_RIGHT,
    ACT_CROSS,     /* Confirm / Play / Select */
    ACT_CIRCLE,    /* Cancel / Back */
    ACT_SQUARE,    /* Search / Filter */
    ACT_TRIANGLE,  /* Queue / Delete in OSK */
    ACT_L1,        /* Previous track */
    ACT_R1,        /* Next track */
    ACT_L2,        /* Repeat mode toggle */
    ACT_R2,        /* Screen adjust scale+ */
    ACT_SELECT,    /* Shuffle toggle */
    ACT_START      /* Exit cleanly */
} InputAction;

#endif /* MONOCHROME_PS3_H */
