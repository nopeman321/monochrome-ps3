/*
 * Monochrome PS3 - UI & Application Logic (PSL1GHT)
 * Pure dynamic data from Monochrome REST API (tracks.monochrome.st)
 * Strictly zero hardcoded mock metadata (Rule: metadatos SOLO de la API)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "monochrome.h"
#include "platform.h"
#include "ui.h"
#include "http_client.h"
#include "audio_engine.h"
#include "cover_art.h"

/* Real search results directly from tracks.monochrome.st */
static TrackMetadata s_display_tracks[MAX_SEARCH_RESULTS];
static int s_display_count = 0;
static AlbumMetadata s_display_albums[MAX_SEARCH_RESULTS];
static int s_display_album_count = 0;
static ArtistMetadata s_display_artists[MAX_SEARCH_RESULTS];
static int s_display_artist_count = 0;
static PlaylistMetadata s_display_playlists[MAX_SEARCH_RESULTS];
static int s_display_playlist_count = 0;
static int s_cursor = 0;

/* Playback state */
static int s_current_track_idx = -1;
static PlaybackState s_playback_state = PLAYBACK_STOPPED;
static float s_playback_progress = 0.0f;
static RepeatMode s_repeat_mode = REPEAT_ALL;
static bool s_shuffle = false;

/* Navigation & Tabs (Monochrome Ecosystem) */
#define NAV_TABS_COUNT 8
static const char *s_nav_tab_names[NAV_TABS_COUNT] = {
    "HOME", "SEARCH", "LIBRARY", "RECENT", "UNRELEASED", "DONATE", "SETTINGS", "ABOUT"
};
static const AppState s_nav_tab_states[NAV_TABS_COUNT] = {
    STATE_HOME, STATE_SEARCH, STATE_LIBRARY, STATE_RECENT, STATE_UNRELEASED, STATE_DONATE, STATE_SETTINGS, STATE_ABOUT
};
static int s_active_nav_tab = 0;

static const char *s_search_tab_names[SEARCH_TAB_COUNT] = {
    "TRACKS", "ALBUMS", "ARTISTS", "PLAYLISTS", "PODCASTS"
};
static SearchTab s_search_active_tab = SEARCH_TAB_TRACKS;

/* Screen cursors */
static int s_library_cursor = 0;
static int s_recent_cursor = 0;
static int s_unreleased_cursor = 0;
static int s_settings_cursor = 0;

#define MAX_RECENT_TRACKS 15
static TrackMetadata s_recent_tracks[MAX_RECENT_TRACKS];
static int s_recent_count = 0;

static void add_recent_track(const TrackMetadata *t)
{
    if (!t || t->id[0] == '\0') return;
    for (int i = 0; i < s_recent_count; i++) {
        if (strcmp(s_recent_tracks[i].id, t->id) == 0) {
            TrackMetadata found = s_recent_tracks[i];
            for (int j = i; j > 0; j--) s_recent_tracks[j] = s_recent_tracks[j - 1];
            s_recent_tracks[0] = found;
            return;
        }
    }
    int move_cnt = (s_recent_count < MAX_RECENT_TRACKS) ? s_recent_count : MAX_RECENT_TRACKS - 1;
    for (int i = move_cnt; i > 0; i--) {
        s_recent_tracks[i] = s_recent_tracks[i - 1];
    }
    s_recent_tracks[0] = *t;
    if (s_recent_count < MAX_RECENT_TRACKS) s_recent_count++;
}

/* Search state */
static char s_search_query[160] = "daft";
static int  s_search_len = 4;
static int  s_osk_row = 1;
static int  s_osk_col = 0;

static SearchStatus s_search_status = SEARCH_STATUS_IDLE;
static int s_last_http_code = 0;
static int s_last_errno = 0;

/* Screen adjustment mode */
static bool s_screen_adjust_mode = false;
static int s_adjust_offset_x = 0;
static int s_adjust_offset_y = 0;
static float s_adjust_scale = 1.0f;
static float s_adjust_font_scale = 1.0f;

/* Menu state */
static bool s_menu_open = false;
static int s_menu_cursor = 0;
static bool s_exit_requested = false;

#define MENU_ITEMS 4
static const char *s_menu_items[] = {
    "Screen Adjust",
    "Network Settings",
    "System Info",
    "Exit"
};

/* Network settings */
static int s_network_timeout = 30;  /* seconds */
static int s_max_retries = 8;

/* System info display */
static bool s_show_system_info = false;
static bool s_show_network_settings = false;
static int s_network_cursor = 0;

static const char *s_osk[4][10] = {
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
    { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" },
    { "A", "S", "D", "F", "G", "H", "J", "K", "L", "-" },
    { "Z", "X", "C", "V", "B", "N", "M", "SPC", "DEL", "GO" }
};

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

bool ui_should_exit(void)
{
    return s_exit_requested;
}

/* Helper: Play track and load cover art */
static void play_track_with_cover(int track_idx)
{
    if (track_idx < 0 || track_idx >= s_display_count) return;
    
    TrackMetadata *t = &s_display_tracks[track_idx];
    
    /* Record in recent history */
    add_recent_track(t);

    /* Start audio playback */
    audio_engine_play_track(t->id, t->title);
    
    /* Load cover art asynchronously (non-blocking) */
    if (t->cover_url[0] != '\0') {
        cover_art_load(t->cover_url);
    } else {
        cover_art_clear();
    }
}

void ui_render_splash(void)
{
    int sw = platform_get_width();
    int sh = platform_get_height();

    const char *banner[] = {
        "+-----------------------------------------------------------------------------+",
        "|  __  __  ___  _  _  ___   ____ _  _ ___  ___  __  __ ___                    |",
        "| |  \\/  |/ _ \\| \\| |/ _ \\ / ___| || | _ \\/ _ \\|  \\/  | __|                   |",
        "| | |\\/| | (_) | .` | (_) | |   | __ |   / (_) | |\\/| | _|                    |",
        "| |_|  |_|\\___/|_|\\_|\\___/ \\____|_||_|_|_\\\\___/|_|  |_|___|                   |",
        "+-----------------------------------------------------------------------------+",
        NULL
    };

    uint64_t t_start = platform_get_time_usec();
    /* Display for 2.2 seconds or until any button is pressed */
    while ((platform_get_time_usec() - t_start) < 2200000ULL) {
        platform_clear(0x00000000);

        /* Minimalist technical borders */
        platform_draw_text(60, 80, "+ [ CORE // MONOCHROME AUDIO ENGINE ] +", 0x00C83838, 2);
        platform_draw_text_r(sw - 60, 80, "LOC: 0x40100000 // CELL B.E.", 0x00666666, 1);

        int line_h = 28;
        int y_art = (sh / 2) - 100;
        for (int i = 0; banner[i] != NULL; i++) {
            platform_draw_text(60, y_art + (i * line_h), banner[i], 0xFFFFFFFF, 2);
        }

        platform_draw_text(60, y_art + 190, ":: 48kHz / 32-bit FLOAT LPCM RESAMPLER :: LOW-LATENCY STREAMING ::", 0x00C83838, 1);
        platform_draw_text(60, y_art + 215, ":: METALHEART MINIMALIST PS3 KERNEL INITIALIZED ::", 0x00888888, 1);
        platform_draw_text_r(sw - 60, sh - 60, "[ PRESS ANY BUTTON TO SKIP ]", 0x00555555, 1);

        platform_flip();

        InputAction act = platform_poll_input();
        if (act != ACT_NONE) break;
        usleep(16000);
    }
}

void ui_init(void)
{
    memset(s_display_tracks, 0, sizeof(s_display_tracks));
    s_display_count = 0;
    s_cursor = 0;
    s_current_track_idx = -1;
    s_playback_state = PLAYBACK_STOPPED;
    s_playback_progress = 0.0f;

    /* Initialize Network */
    http_net_init();

    /* Launch initial real background search for "daft" (no auto-play on startup) */
    strncpy(s_search_query, "daft", sizeof(s_search_query) - 1);
    s_search_len = 4;
    http_search_async_start(s_search_query);
    s_search_status = SEARCH_STATUS_BUSY;
}

AppState ui_handle_input(AppState current, InputAction act)
{
    if (act == ACT_NONE) return current;

    /* Global Nav Tab Cycling with L1 / R1 across main screens (except search OSK) */
    if (current != STATE_PLAYING && current != STATE_SEARCH && !s_screen_adjust_mode && !s_menu_open && !s_show_system_info && !s_show_network_settings) {
        for (int i = 0; i < NAV_TABS_COUNT; i++) {
            if (s_nav_tab_states[i] == current) {
                s_active_nav_tab = i;
                break;
            }
        }
        if (act == ACT_L1) {
            s_active_nav_tab = (s_active_nav_tab - 1 + NAV_TABS_COUNT) % NAV_TABS_COUNT;
            return s_nav_tab_states[s_active_nav_tab];
        }
        if (act == ACT_R1) {
            s_active_nav_tab = (s_active_nav_tab + 1) % NAV_TABS_COUNT;
            return s_nav_tab_states[s_active_nav_tab];
        }
    }

    /* ── MENU STATE ── */
    if (s_menu_open) {
        switch (act) {
            case ACT_UP:
                if (s_menu_cursor > 0) s_menu_cursor--;
                break;
            case ACT_DOWN:
                if (s_menu_cursor < MENU_ITEMS - 1) s_menu_cursor++;
                break;
            case ACT_CROSS:
                /* Execute menu action */
                if (s_menu_cursor == 0) {
                    /* Screen Adjust */
                    s_menu_open = false;
                    s_screen_adjust_mode = true;
                    platform_get_screen_adjust(&s_adjust_offset_x, &s_adjust_offset_y, &s_adjust_scale);
                    s_adjust_font_scale = platform_get_font_scale();
                } else if (s_menu_cursor == 1) {
                    /* Network Settings */
                    s_menu_open = false;
                    /* TODO: Implement network settings UI */
                } else if (s_menu_cursor == 2) {
                    /* System Info */
                    s_menu_open = false;
                    s_show_system_info = true;
                } else if (s_menu_cursor == 3) {
                    /* Exit */
                    s_exit_requested = true;
                    s_menu_open = false;
                }
                break;
            case ACT_CIRCLE:
            case ACT_START:
                /* Close menu */
                s_menu_open = false;
                break;
            default:
                break;
        }
        return current;
    }

    /* ── SCREEN ADJUST MODE ── */
    if (s_screen_adjust_mode) {
        switch (act) {
            case ACT_UP:
                s_adjust_offset_y -= 1;
                platform_set_screen_adjust(s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale);
                break;
            case ACT_DOWN:
                s_adjust_offset_y += 1;
                platform_set_screen_adjust(s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale);
                break;
            case ACT_LEFT:
                s_adjust_offset_x -= 1;
                platform_set_screen_adjust(s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale);
                break;
            case ACT_RIGHT:
                s_adjust_offset_x += 1;
                platform_set_screen_adjust(s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale);
                break;
            case ACT_L2: /* Decrease scale (fine step 0.005) */
                s_adjust_scale -= 0.005f;
                if (s_adjust_scale < 0.5f) s_adjust_scale = 0.5f;
                platform_set_screen_adjust(s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale);
                break;
            case ACT_R2: /* Increase scale (fine step 0.005) */
                s_adjust_scale += 0.005f;
                if (s_adjust_scale > 1.5f) s_adjust_scale = 1.5f;
                platform_set_screen_adjust(s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale);
                break;
            case ACT_L1: /* Decrease font scale (fine step 0.02) */
                s_adjust_font_scale -= 0.02f;
                if (s_adjust_font_scale < 0.5f) s_adjust_font_scale = 0.5f;
                platform_set_font_scale(s_adjust_font_scale);
                break;
            case ACT_R1: /* Increase font scale (fine step 0.02) */
                s_adjust_font_scale += 0.02f;
                if (s_adjust_font_scale > 2.0f) s_adjust_font_scale = 2.0f;
                platform_set_font_scale(s_adjust_font_scale);
                break;
            case ACT_START: /* Save and exit */
                platform_set_font_scale(s_adjust_font_scale);
                platform_save_screen_config();
                s_screen_adjust_mode = false;
                break;
            case ACT_SELECT: /* Reset to defaults */
                s_adjust_offset_x = 0;
                s_adjust_offset_y = 0;
                s_adjust_scale = 1.0f;
                s_adjust_font_scale = 1.0f;
                platform_set_screen_adjust(s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale);
                platform_set_font_scale(s_adjust_font_scale);
                break;
            case ACT_CIRCLE: /* Cancel without saving */
                s_screen_adjust_mode = false;
                break;
            default:
                break;
        }
        return current;
    }

    /* ── SYSTEM INFO MODE ── */
    if (s_show_system_info) {
        switch (act) {
            case ACT_CIRCLE:
            case ACT_START:
                s_show_system_info = false;
                break;
            default:
                break;
        }
        return current;
    }

    /* ── NETWORK SETTINGS MODE ── */
    if (s_show_network_settings) {
        switch (act) {
            case ACT_UP:
                s_network_cursor = (s_network_cursor - 1 + 2) % 2;
                break;
            case ACT_DOWN:
                s_network_cursor = (s_network_cursor + 1) % 2;
                break;
            case ACT_LEFT:
                if (s_network_cursor == 0) { /* Timeout */
                    int timeout = http_get_timeout_ms() / 1000;
                    if (timeout > 5) {
                        timeout -= 5;
                        http_set_timeout_ms(timeout * 1000);
                    }
                } else if (s_network_cursor == 1) { /* Max retries */
                    int retries = http_get_max_retries();
                    if (retries > 1) {
                        http_set_max_retries(retries - 1);
                    }
                }
                break;
            case ACT_RIGHT:
                if (s_network_cursor == 0) { /* Timeout */
                    int timeout = http_get_timeout_ms() / 1000;
                    if (timeout < 60) {
                        timeout += 5;
                        http_set_timeout_ms(timeout * 1000);
                    }
                } else if (s_network_cursor == 1) { /* Max retries */
                    int retries = http_get_max_retries();
                    if (retries < 15) {
                        http_set_max_retries(retries + 1);
                    }
                }
                break;
            case ACT_CIRCLE:
            case ACT_START:
                s_show_network_settings = false;
                break;
            default:
                break;
        }
        return current;
    }

    /* Open menu with START */
    if (act == ACT_START) {
        s_menu_open = true;
        s_menu_cursor = 0;
        return current;
    }

    /* ── SEARCH OSK STATE ── */
    if (current == STATE_SEARCH) {
        switch (act) {
            case ACT_UP:
                s_osk_row = (s_osk_row - 1 + 4) % 4;
                break;
            case ACT_DOWN:
                s_osk_row = (s_osk_row + 1) % 4;
                break;
            case ACT_LEFT:
                s_osk_col = (s_osk_col - 1 + 10) % 10;
                break;
            case ACT_RIGHT:
                s_osk_col = (s_osk_col + 1) % 10;
                break;
            case ACT_CROSS: {
                const char *k = s_osk[s_osk_row][s_osk_col];
                if (strcmp(k, "GO") == 0) {
                    if (s_search_len > 0) {
                        /* Trigger real async HTTP search */
                        s_display_count = 0;
                        s_cursor = 0;
                        http_search_async_start(s_search_query);
                        s_search_status = SEARCH_STATUS_BUSY;
                    }
                    return STATE_HOME;
                } else if (strcmp(k, "DEL") == 0) {
                    if (s_search_len > 0) {
                        s_search_query[--s_search_len] = '\0';
                    }
                } else if (strcmp(k, "SPC") == 0) {
                    if (s_search_len < (int)sizeof(s_search_query) - 1) {
                        s_search_query[s_search_len++] = ' ';
                        s_search_query[s_search_len] = '\0';
                    }
                } else {
                    if (s_search_len < (int)sizeof(s_search_query) - 1) {
                        s_search_query[s_search_len++] = k[0];
                        s_search_query[s_search_len] = '\0';
                    }
                }
                break;
            }
            case ACT_L1:
            case ACT_L2: /* Cycle search sub-tab left */
                s_search_active_tab = (SearchTab)((s_search_active_tab - 1 + SEARCH_TAB_COUNT) % SEARCH_TAB_COUNT);
                break;
            case ACT_R1:
            case ACT_R2: /* Cycle search sub-tab right */
                s_search_active_tab = (SearchTab)((s_search_active_tab + 1) % SEARCH_TAB_COUNT);
                break;
            case ACT_TRIANGLE: /* Quick delete in OSK */
                if (s_search_len > 0) {
                    s_search_query[--s_search_len] = '\0';
                }
                break;
            case ACT_CIRCLE: /* Cancel search */
                return STATE_HOME;
            default:
                break;
        }
        return current;
    }

    /* ── NOW PLAYING STATE ── */
    if (current == STATE_PLAYING) {
        switch (act) {
            case ACT_CROSS:
                /* If error (format not supported), skip to next track */
                if (audio_engine_has_error()) {
                    if (s_display_count > 0) {
                        s_current_track_idx = (s_current_track_idx + 1) % s_display_count;
                        s_playback_progress = 0.0f;
                        play_track_with_cover(s_current_track_idx);
                    }
                } else if (s_playback_state == PLAYBACK_PLAYING) {
                    s_playback_state = PLAYBACK_PAUSED;
                    audio_engine_pause();
                } else {
                    s_playback_state = PLAYBACK_PLAYING;
                    audio_engine_resume();
                }
                break;
            case ACT_CIRCLE:
                return STATE_HOME;
            case ACT_L1: /* Prev track in search results */
                if (s_display_count > 0) {
                    if (s_current_track_idx > 0) s_current_track_idx--;
                    else s_current_track_idx = s_display_count - 1;
                    s_playback_progress = 0.0f;
                    play_track_with_cover(s_current_track_idx);
                }
                break;
            case ACT_R1: /* Next track in search results */
                if (s_display_count > 0) {
                    s_current_track_idx = (s_current_track_idx + 1) % s_display_count;
                    s_playback_progress = 0.0f;
                    play_track_with_cover(s_current_track_idx);
                }
                break;
            case ACT_UP:
                audio_engine_set_volume(audio_engine_get_volume() + 0.05f);
                break;
            case ACT_DOWN:
                audio_engine_set_volume(audio_engine_get_volume() - 0.05f);
                break;
            case ACT_LEFT:
                s_playback_progress -= 0.05f;
                if (s_playback_progress < 0.0f) s_playback_progress = 0.0f;
                audio_engine_seek(s_playback_progress);
                break;
            case ACT_RIGHT:
                s_playback_progress += 0.05f;
                if (s_playback_progress > 1.0f) s_playback_progress = 1.0f;
                audio_engine_seek(s_playback_progress);
                break;
            case ACT_L2:
                s_repeat_mode = (RepeatMode)((s_repeat_mode + 1) % 3);
                break;
            case ACT_SELECT:
                s_shuffle = !s_shuffle;
                break;
            case ACT_TRIANGLE:
                return STATE_QUEUE;
            default:
                break;
        }
        return current;
    }

    /* ── QUEUE STATE ── */
    if (current == STATE_QUEUE) {
        switch (act) {
            case ACT_CIRCLE:
            case ACT_TRIANGLE:
                return STATE_HOME;
            case ACT_CROSS:
                if (s_display_count > 0 && s_cursor >= 0 && s_cursor < s_display_count) {
                    s_current_track_idx = s_cursor;
                    s_playback_state = PLAYBACK_PLAYING;
                    s_playback_progress = 0.0f;
                    play_track_with_cover(s_current_track_idx);
                    return STATE_PLAYING;
                }
                break;
            case ACT_UP:
                if (s_cursor > 0) s_cursor--;
                break;
            case ACT_DOWN:
                if (s_cursor + 1 < s_display_count) s_cursor++;
                break;
            default:
                break;
        }
        return current;
    }

    /* ── LIBRARY STATE ── */
    if (current == STATE_LIBRARY) {
        switch (act) {
            case ACT_UP:
                if (s_library_cursor > 0) s_library_cursor--;
                break;
            case ACT_DOWN:
                if (s_library_cursor < 4) s_library_cursor++;
                break;
            case ACT_CROSS:
                /* Trigger library action / search */
                http_search_async_start("electronic");
                s_search_status = SEARCH_STATUS_BUSY;
                return STATE_HOME;
            case ACT_SQUARE:
                return STATE_SEARCH;
            case ACT_TRIANGLE:
                return STATE_QUEUE;
            case ACT_CIRCLE:
                return STATE_HOME;
            default: break;
        }
        return current;
    }

    /* ── RECENT STATE ── */
    if (current == STATE_RECENT) {
        switch (act) {
            case ACT_UP:
                if (s_recent_cursor > 0) s_recent_cursor--;
                break;
            case ACT_DOWN:
                if (s_recent_cursor + 1 < s_recent_count) s_recent_cursor++;
                break;
            case ACT_CROSS:
                if (s_recent_count > 0 && s_recent_cursor >= 0 && s_recent_cursor < s_recent_count) {
                    audio_engine_play_track(s_recent_tracks[s_recent_cursor].id, s_recent_tracks[s_recent_cursor].title);
                    if (s_recent_tracks[s_recent_cursor].cover_url[0] != '\0') {
                        cover_art_load(s_recent_tracks[s_recent_cursor].cover_url);
                    } else {
                        cover_art_clear();
                    }
                    s_playback_state = PLAYBACK_PLAYING;
                    s_playback_progress = 0.0f;
                    return STATE_PLAYING;
                }
                break;
            case ACT_SQUARE:
                return STATE_SEARCH;
            case ACT_TRIANGLE:
                return STATE_QUEUE;
            case ACT_CIRCLE:
                return STATE_HOME;
            default: break;
        }
        return current;
    }

    /* ── UNRELEASED STATE ── */
    if (current == STATE_UNRELEASED) {
        switch (act) {
            case ACT_UP:
                if (s_unreleased_cursor > 0) s_unreleased_cursor--;
                break;
            case ACT_DOWN:
                if (s_unreleased_cursor < 4) s_unreleased_cursor++;
                break;
            case ACT_CROSS:
                /* Search live bootlegs */
                http_search_async_start("alive 2007");
                s_search_status = SEARCH_STATUS_BUSY;
                return STATE_HOME;
            case ACT_SQUARE:
                return STATE_SEARCH;
            case ACT_TRIANGLE:
                return STATE_QUEUE;
            case ACT_CIRCLE:
                return STATE_HOME;
            default: break;
        }
        return current;
    }

    /* ── SETTINGS STATE ── */
    if (current == STATE_SETTINGS) {
        switch (act) {
            case ACT_UP:
                if (s_settings_cursor > 0) s_settings_cursor--;
                break;
            case ACT_DOWN:
                if (s_settings_cursor < 3) s_settings_cursor++;
                break;
            case ACT_CROSS:
                if (s_settings_cursor == 0) {
                    s_screen_adjust_mode = true;
                    platform_get_screen_adjust(&s_adjust_offset_x, &s_adjust_offset_y, &s_adjust_scale);
                    s_adjust_font_scale = platform_get_font_scale();
                } else if (s_settings_cursor == 1) {
                    s_show_network_settings = true;
                } else if (s_settings_cursor == 2) {
                    s_show_system_info = true;
                } else if (s_settings_cursor == 3) {
                    s_adjust_offset_x = 0;
                    s_adjust_offset_y = 0;
                    s_adjust_scale = 1.0f;
                    s_adjust_font_scale = 1.0f;
                    platform_set_screen_adjust(0, 0, 1.0f);
                    platform_set_font_scale(1.0f);
                    platform_save_screen_config();
                }
                break;
            case ACT_SQUARE:
                return STATE_SEARCH;
            case ACT_TRIANGLE:
                return STATE_QUEUE;
            case ACT_CIRCLE:
                return STATE_HOME;
            default: break;
        }
        return current;
    }

    /* ── ABOUT & DONATE STATES ── */
    if (current == STATE_ABOUT || current == STATE_DONATE) {
        switch (act) {
            case ACT_CIRCLE:
                return STATE_HOME;
            case ACT_SQUARE:
                return STATE_SEARCH;
            case ACT_TRIANGLE:
                return STATE_QUEUE;
            default: break;
        }
        return current;
    }

    /* ── HOME STATE (Default) ── */
    switch (act) {
        case ACT_L2:
            s_search_active_tab = (SearchTab)((s_search_active_tab - 1 + SEARCH_TAB_COUNT) % SEARCH_TAB_COUNT);
            s_cursor = 0;
            break;
        case ACT_R2:
            s_search_active_tab = (SearchTab)((s_search_active_tab + 1) % SEARCH_TAB_COUNT);
            s_cursor = 0;
            break;
        case ACT_UP: {
            int max_c = (s_search_active_tab == SEARCH_TAB_TRACKS) ? s_display_count :
                        (s_search_active_tab == SEARCH_TAB_ALBUMS) ? s_display_album_count :
                        (s_search_active_tab == SEARCH_TAB_ARTISTS) ? s_display_artist_count :
                        (s_search_active_tab == SEARCH_TAB_PLAYLISTS) ? s_display_playlist_count : 0;
            if (max_c > 0) {
                if (s_cursor > 0) s_cursor--;
                else s_cursor = max_c - 1;
            }
            break;
        }
        case ACT_DOWN: {
            int max_c = (s_search_active_tab == SEARCH_TAB_TRACKS) ? s_display_count :
                        (s_search_active_tab == SEARCH_TAB_ALBUMS) ? s_display_album_count :
                        (s_search_active_tab == SEARCH_TAB_ARTISTS) ? s_display_artist_count :
                        (s_search_active_tab == SEARCH_TAB_PLAYLISTS) ? s_display_playlist_count : 0;
            if (max_c > 0) {
                if (s_cursor + 1 < max_c) s_cursor++;
                else s_cursor = 0;
            }
            break;
        }
        case ACT_CROSS:
            if (s_search_active_tab == SEARCH_TAB_TRACKS) {
                if (s_display_count > 0 && s_cursor >= 0 && s_cursor < s_display_count) {
                    s_current_track_idx = s_cursor;
                    s_playback_state = PLAYBACK_PLAYING;
                    s_playback_progress = 0.0f;
                    /* Play the EXACT ID selected and load cover */
                    play_track_with_cover(s_current_track_idx);
                    return STATE_PLAYING;
                }
            } else if (s_search_active_tab == SEARCH_TAB_ALBUMS) {
                if (s_display_album_count > 0 && s_cursor >= 0 && s_cursor < s_display_album_count) {
                    /* Drill down into album songs list directly via /releases/<id> */
                    snprintf(s_search_query, sizeof(s_search_query), "Album: %s", s_display_albums[s_cursor].title);
                    s_search_len = (int)strlen(s_search_query);
                    s_display_count = 0;
                    s_cursor = 0;
                    http_fetch_album_async_start(s_display_albums[s_cursor].id, s_display_albums[s_cursor].title);
                    s_search_status = SEARCH_STATUS_BUSY;
                    s_search_active_tab = SEARCH_TAB_TRACKS;
                }
            } else if (s_search_active_tab == SEARCH_TAB_ARTISTS) {
                if (s_display_artist_count > 0 && s_cursor >= 0 && s_cursor < s_display_artist_count) {
                    /* Drill down into artist tracks */
                    strncpy(s_search_query, s_display_artists[s_cursor].name, sizeof(s_search_query) - 1);
                    s_search_len = (int)strlen(s_search_query);
                    s_display_count = 0;
                    s_cursor = 0;
                    http_search_async_start(s_search_query);
                    s_search_status = SEARCH_STATUS_BUSY;
                    s_search_active_tab = SEARCH_TAB_TRACKS;
                }
            } else if (s_search_active_tab == SEARCH_TAB_PLAYLISTS) {
                if (s_display_playlist_count > 0 && s_cursor >= 0 && s_cursor < s_display_playlist_count) {
                    /* Drill down into playlist tracks */
                    strncpy(s_search_query, s_display_playlists[s_cursor].title, sizeof(s_search_query) - 1);
                    s_search_len = (int)strlen(s_search_query);
                    s_display_count = 0;
                    s_cursor = 0;
                    http_search_async_start(s_search_query);
                    s_search_status = SEARCH_STATUS_BUSY;
                    s_search_active_tab = SEARCH_TAB_TRACKS;
                }
            }
            break;
        case ACT_SQUARE:
            return STATE_SEARCH;
        case ACT_TRIANGLE:
            return STATE_QUEUE;
        default:
            break;
    }

    return current;
}

/* ── Screen Renderers ── */

static void render_ascii_metalheart_background(int sw, int sh)
{
    /* Dark subtle metalheart technical markings (pure black 0x00000000 dominates) */
    platform_draw_text(25, 62, "+ [SYS: AUDIO_CORE // PPU_DMA]", 0x00222222, 1);
    platform_draw_text_r(sw - 25, 62, "STATUS: ONLINE // 48kHz LPCM +", 0x00222222, 1);

    platform_draw_text(35, sh / 3, "+ .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  +", 0x00151515, 1);
    platform_draw_text(35, (sh * 2) / 3, "+ .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  +", 0x00151515, 1);

    platform_draw_text(25, sh - 52, "// MONOCHROME.ST // LOSSLESS FLAC // CELL-BE //", 0x001C1C1C, 1);
    platform_draw_text_r(sw - 25, sh - 52, "LOC: 0x40100000 // BUFFER: 48MB //", 0x001C1C1C, 1);
}

static void render_header(int sw, AppState current_state, const char *right_hint)
{
    int header_h = 90;
    platform_draw_rect(0, 0, sw, header_h, 0x000B0E14);
    platform_draw_rect(0, header_h, sw, 3, 0x00C83838);

    /* Brand logo & version */
    platform_draw_text(25, 10, "MONOCHROME", 0xFFFFFFFF, 3);
    platform_draw_text(250, 16, APP_DISPLAY_VERSION, 0x00758595, 2);

    /* Top right status/hint */
    if (right_hint) {
        int hint_len = (int)strlen(right_hint);
        int hint_scale = (hint_len * 16 > sw - 380) ? 1 : 2;
        int hint_y = (hint_scale == 1) ? 18 : 14;
        platform_draw_text_r(sw - 25, hint_y, right_hint, 0x0090A4AE, hint_scale);
    }

    /* 8 Navigation tabs row: Grandes, legibles y adaptables a escala */
    int tab_y = 44;
    int tab_h = 36;
    int tab_font_scale = (sw >= 1150) ? 2 : 1;

    int tab_x = 25;
    for (int i = 0; i < NAV_TABS_COUNT; i++) {
        bool is_active = (s_nav_tab_states[i] == current_state);
        int text_w = (int)strlen(s_nav_tab_names[i]) * 8 * tab_font_scale;
        int tw = text_w + (tab_font_scale == 2 ? 24 : 16);

        if (is_active) {
            /* Pestaña activa: fondo rojo brillante y realce inferior blanco */
            platform_draw_rect(tab_x, tab_y, tw, tab_h, 0x00C83838);
            platform_draw_rect(tab_x, tab_y + tab_h - 4, tw, 4, 0xFFFFFFFF);
            int tx = tab_x + (tw - text_w) / 2;
            int ty = tab_y + (tab_h - (8 * tab_font_scale)) / 2;
            platform_draw_text(tx, ty, s_nav_tab_names[i], 0xFFFFFFFF, tab_font_scale);
        } else {
            /* Pestaña inactiva: fondo oscuro estilizado con borde */
            platform_draw_rect(tab_x, tab_y, tw, tab_h, 0x00141C2B);
            platform_draw_rect(tab_x, tab_y, tw, 1, 0x002B374A);
            int tx = tab_x + (tw - text_w) / 2;
            int ty = tab_y + (tab_h - (8 * tab_font_scale)) / 2;
            platform_draw_text(tx, ty, s_nav_tab_names[i], 0x00A0B0C0, tab_font_scale);
        }
        tab_x += tw + 8;
    }
    platform_draw_text_r(sw - 25, tab_y + 10, "[L1/R1] Pestañas | [] Buscar", 0x00667788, 1);
}

static void render_now_playing_mini_bar(int sw, int sh)
{
    int bar_y = sh - 90;
    int bar_h = 50;

    platform_draw_rect(0, bar_y, sw, bar_h, 0x00131B28);
    platform_draw_rect(0, bar_y, sw, 1, 0xFFFFFFFF);

    if (s_current_track_idx >= 0 && s_current_track_idx < s_display_count) {
        TrackMetadata *t = &s_display_tracks[s_current_track_idx];
        const char *icon = (s_playback_state == PLAYBACK_PLAYING) ? ">" : "||";
        platform_draw_text(30, bar_y + 15, icon, 0x00C83838, 3);

        char dac[64];
        snprintf(dac, sizeof(dac), "%s | %uHz %dbit", t->is_hi_res ? "HI-RES" : "FLAC", t->sample_rate, t->bit_depth);
        int dac_w = (int)strlen(dac) * 8 * 2;
        platform_draw_text_r(sw - 30, bar_y + 16, dac, t->is_hi_res ? 0xFFFFFFFF : 0x00C83838, 2);

        char info[260];
        snprintf(info, sizeof(info), "%s - %s", t->title, t->artist);
        int max_info_w = (sw - 30 - dac_w - 20) - 65;
        if (max_info_w < 100) max_info_w = 100;
        platform_draw_text_clipped(65, bar_y + 12, max_info_w, info, 0xFFFFFFFF, 2);

        char id_str[80];
        snprintf(id_str, sizeof(id_str), "ID: %s", t->id);
        platform_draw_text_clipped(65, bar_y + 32, max_info_w, id_str, 0xFFFFFFFF, 1);
    } else {
        platform_draw_text(30, bar_y + 16, "-- Ningun tema seleccionado (Pulsa X en la lista) --", 0x00808080, 2);
    }

    float real_p = audio_engine_get_progress();
    if (real_p > 0.0f) {
        s_playback_progress = real_p;
    }
    int prog_w = (int)(sw * s_playback_progress);
    platform_draw_rect(0, bar_y + bar_h - 3, prog_w, 3, 0xFFFFFFFF);
}

static void render_footer(int sw, int sh, const char *hints)
{
    platform_draw_rect(0, sh - 40, sw, 40, 0x000F121B);
    int h_len = (int)strlen(hints);
    int f_scale = (h_len * 16 > sw - 60) ? 1 : 2;
    int f_y = (f_scale == 1) ? sh - 24 : sh - 28;
    platform_draw_text_clipped(30, f_y, sw - 60, hints, 0x00B0BEC5, f_scale);
}

static void render_home_screen(int sw, int sh)
{
    render_header(sw, STATE_HOME, "[L2/R2] Filtro Categoria | [X] Seleccionar");

    /* Search Sub-Tabs Row (Grandes, 36px, fuente escala 2) */
    int subtab_y = 94;
    int subtab_h = 36;
    int subtab_font_scale = (sw >= 1150) ? 2 : 1;
    platform_draw_rect(0, subtab_y, sw, subtab_h + 4, 0x000E1420);
    platform_draw_rect(0, subtab_y + subtab_h + 3, sw, 1, 0x002B374A);

    int subtab_x = 25;
    for (int t = 0; t < SEARCH_TAB_COUNT; t++) {
        bool is_sub_active = (t == (int)s_search_active_tab);
        int text_w = (int)strlen(s_search_tab_names[t]) * 8 * subtab_font_scale;
        int tw = text_w + (subtab_font_scale == 2 ? 26 : 16);

        if (is_sub_active) {
            platform_draw_rect(subtab_x, subtab_y + 2, tw, subtab_h, 0x00C83838);
            platform_draw_rect(subtab_x, subtab_y + subtab_h - 2, tw, 4, 0xFFFFFFFF);
            int tx = subtab_x + (tw - text_w) / 2;
            int ty = subtab_y + 2 + (subtab_h - (8 * subtab_font_scale)) / 2;
            platform_draw_text(tx, ty, s_search_tab_names[t], 0xFFFFFFFF, subtab_font_scale);
        } else {
            platform_draw_rect(subtab_x, subtab_y + 2, tw, subtab_h, 0x00141C2B);
            platform_draw_rect(subtab_x, subtab_y + 2, tw, 1, 0x002B374A);
            int tx = subtab_x + (tw - text_w) / 2;
            int ty = subtab_y + 2 + (subtab_h - (8 * subtab_font_scale)) / 2;
            platform_draw_text(tx, ty, s_search_tab_names[t], 0x00A0B0C0, subtab_font_scale);
        }
        subtab_x += tw + 10;
    }
    platform_draw_text_r(sw - 25, subtab_y + 11, "[L2/R2] Filtro Categoria", 0x00788898, 1);

    /* Dynamic Subheader */
    int stat_y = 138;
    platform_draw_rect(0, stat_y, sw, 32, 0x00141824);

    if (s_search_status == SEARCH_STATUS_BUSY) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Buscando '%s' en tracks.monochrome.st...", s_search_query);
        platform_draw_text_clipped(30, stat_y + 8, sw - 60, msg, 0x00C83838, 2);
    } else if (s_search_status == SEARCH_STATUS_NO_RESULTS) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Sin resultados para '%s' (Pulsa [] para buscar de nuevo)", s_search_query);
        platform_draw_text_clipped(30, stat_y + 8, sw - 60, msg, 0x00C83838, 2);
    } else if (s_search_status == SEARCH_STATUS_ERROR) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Error de red al consultar API (HTTP %d, errno %d)", s_last_http_code, s_last_errno);
        platform_draw_text_clipped(30, stat_y + 8, sw - 60, msg, 0x00C83838, 2);
    } else {
        char f_info[160];
        if (s_search_active_tab == SEARCH_TAB_TRACKS) {
            snprintf(f_info, sizeof(f_info), "Canciones: \"%s\" (%d temas disponibles) | [X] Reproducir", s_search_query, s_display_count);
        } else if (s_search_active_tab == SEARCH_TAB_ALBUMS) {
            snprintf(f_info, sizeof(f_info), "Albumes: \"%s\" (%d albumes disponibles) | [X] Abrir canciones del album", s_search_query, s_display_album_count);
        } else if (s_search_active_tab == SEARCH_TAB_ARTISTS) {
            snprintf(f_info, sizeof(f_info), "Artistas: \"%s\" (%d artistas encontrados) | [X] Ver canciones del artista", s_search_query, s_display_artist_count);
        } else if (s_search_active_tab == SEARCH_TAB_PLAYLISTS) {
            snprintf(f_info, sizeof(f_info), "Playlists: \"%s\" (%d listas encontradas) | [X] Cargar canciones", s_search_query, s_display_playlist_count);
        } else {
            snprintf(f_info, sizeof(f_info), "Podcasts & Sesiones Exclusivas");
        }
        int online_w = 30 * 8 * 2;
        int max_finfo_w = sw - 30 - online_w - 40;
        if (max_finfo_w < 100) max_finfo_w = 100;
        platform_draw_text_clipped(30, stat_y + 8, max_finfo_w, f_info, 0xFFFFFFFF, 2);
        platform_draw_text_r(sw - 30, stat_y + 8, "[ONLINE: tracks.monochrome.st]", 0x00C83838, 2);
    }

    /* List items */
    int list_y = 176;
    int row_h = 55;
    int max_rows = (sh - 176 - 100) / row_h;

    if (s_search_active_tab == SEARCH_TAB_TRACKS) {
        if (s_display_count == 0 && s_search_status != SEARCH_STATUS_BUSY) {
            platform_draw_text(sw / 2 - 180, sh / 2, "No hay canciones para esta busqueda", 0x00808080, 2);
        } else {
            for (int i = 0; i < max_rows && i < s_display_count; i++) {
                int ry = list_y + i * row_h;
                int is_sel = (i == s_cursor);
                TrackMetadata *t = &s_display_tracks[i];

                if (is_sel) {
                    platform_draw_rect(20, ry, sw - 40, row_h - 4, 0x00243450);
                    platform_draw_rect(20, ry, 6, row_h - 4, 0xFFFFFFFF);
                    platform_draw_text(36, ry + 16, ">", 0xFFFFFFFF, 3);
                } else {
                    platform_draw_rect(20, ry, sw - 40, row_h - 4, (i % 2 == 0) ? 0x00111722 : 0x00161E2C);
                }

                char num[8];
                snprintf(num, sizeof(num), "%02d.", i + 1);
                platform_draw_text(60, ry + 16, num, is_sel ? 0xFFFFFFFF : 0x00707070, 2);

                int max_row_w = sw - 120 - 20 - 110;
                if (max_row_w < 100) max_row_w = 100;
                platform_draw_text_clipped(110, ry + 10, max_row_w, t->title, is_sel ? 0xFFFFFFFF : 0x00E0E0E0, 2);

                char subline[384];
                if (t->album[0]) {
                    snprintf(subline, sizeof(subline), "%s  |  [Album: %s]", t->artist, t->album);
                } else {
                    snprintf(subline, sizeof(subline), "%s  |  [ID: %s]", t->artist, t->id);
                }
                platform_draw_text_clipped(110, ry + 32, max_row_w, subline, is_sel ? 0xFFFFFFFF : 0x00808080, 1);

                char dur[16];
                snprintf(dur, sizeof(dur), "%d:%02d", t->duration_sec / 60, t->duration_sec % 60);
                platform_draw_text_r(sw - 120, ry + 18, dur, 0x00909090, 2);

                const char *badge = t->is_hi_res ? "HI-RES" : "FLAC";
                platform_draw_text_r(sw - 40, ry + 18, badge, t->is_hi_res ? 0xFFFFFFFF : 0x00C83838, 2);
            }
        }
    } else if (s_search_active_tab == SEARCH_TAB_ALBUMS) {
        if (s_display_album_count == 0 && s_search_status != SEARCH_STATUS_BUSY) {
            platform_draw_text(sw / 2 - 200, sh / 2, "No hay albumes para esta busqueda", 0x00808080, 2);
        } else {
            for (int i = 0; i < max_rows && i < s_display_album_count; i++) {
                int ry = list_y + i * row_h;
                int is_sel = (i == s_cursor);
                AlbumMetadata *alb = &s_display_albums[i];

                if (is_sel) {
                    platform_draw_rect(20, ry, sw - 40, row_h - 4, 0x00243450);
                    platform_draw_rect(20, ry, 6, row_h - 4, 0xFFFFFFFF);
                    platform_draw_text(36, ry + 16, ">", 0xFFFFFFFF, 3);
                } else {
                    platform_draw_rect(20, ry, sw - 40, row_h - 4, (i % 2 == 0) ? 0x00111722 : 0x00161E2C);
                }

                char num[8];
                snprintf(num, sizeof(num), "%02d.", i + 1);
                platform_draw_text(60, ry + 16, num, is_sel ? 0xFFFFFFFF : 0x00707070, 2);

                int max_row_w = sw - 140 - 20 - 110;
                if (max_row_w < 100) max_row_w = 100;
                platform_draw_text_clipped(110, ry + 10, max_row_w, alb->title, is_sel ? 0xFFFFFFFF : 0x00E0E0E0, 2);

                char subline[256];
                snprintf(subline, sizeof(subline), "%s  |  [ALBUM]  |  Ano: %s  |  [Pulsa X para ver canciones]",
                         alb->artist[0] ? alb->artist : "Varios Artistas", alb->year[0] ? alb->year : "2024");
                platform_draw_text_clipped(110, ry + 32, max_row_w, subline, is_sel ? 0xFFFFFFFF : 0x0090A4AE, 1);

                platform_draw_text_r(sw - 40, ry + 18, "[VER ALBUM]", is_sel ? 0xFFFFFFFF : 0x00C83838, 2);
            }
        }
    } else if (s_search_active_tab == SEARCH_TAB_ARTISTS) {
        if (s_display_artist_count == 0 && s_search_status != SEARCH_STATUS_BUSY) {
            platform_draw_text(sw / 2 - 200, sh / 2, "No hay artistas para esta busqueda", 0x00808080, 2);
        } else {
            for (int i = 0; i < max_rows && i < s_display_artist_count; i++) {
                int ry = list_y + i * row_h;
                int is_sel = (i == s_cursor);
                ArtistMetadata *art = &s_display_artists[i];

                if (is_sel) {
                    platform_draw_rect(20, ry, sw - 40, row_h - 4, 0x00243450);
                    platform_draw_rect(20, ry, 6, row_h - 4, 0xFFFFFFFF);
                    platform_draw_text(36, ry + 16, ">", 0xFFFFFFFF, 3);
                } else {
                    platform_draw_rect(20, ry, sw - 40, row_h - 4, (i % 2 == 0) ? 0x00111722 : 0x00161E2C);
                }

                char num[8];
                snprintf(num, sizeof(num), "%02d.", i + 1);
                platform_draw_text(60, ry + 16, num, is_sel ? 0xFFFFFFFF : 0x00707070, 2);

                int max_row_w = sw - 140 - 20 - 110;
                if (max_row_w < 100) max_row_w = 100;
                platform_draw_text_clipped(110, ry + 10, max_row_w, art->name, is_sel ? 0xFFFFFFFF : 0x00E0E0E0, 2);

                char subline[256];
                if (art->bio[0]) {
                    snprintf(subline, sizeof(subline), "%s  |  [Pulsa X para ver discografia completa]", art->bio);
                } else {
                    snprintf(subline, sizeof(subline), "Artista verificado  |  [Pulsa X para ver canciones del artista]");
                }
                platform_draw_text_clipped(110, ry + 32, max_row_w, subline, is_sel ? 0xFFFFFFFF : 0x0090A4AE, 1);

                platform_draw_text_r(sw - 40, ry + 18, "[ARTISTA]", is_sel ? 0xFFFFFFFF : 0x00C83838, 2);
            }
        }
    } else if (s_search_active_tab == SEARCH_TAB_PLAYLISTS) {
        if (s_display_playlist_count == 0 && s_search_status != SEARCH_STATUS_BUSY) {
            platform_draw_text(sw / 2 - 200, sh / 2, "No hay playlists para esta busqueda", 0x00808080, 2);
        } else {
            for (int i = 0; i < max_rows && i < s_display_playlist_count; i++) {
                int ry = list_y + i * row_h;
                int is_sel = (i == s_cursor);
                PlaylistMetadata *pl = &s_display_playlists[i];

                if (is_sel) {
                    platform_draw_rect(20, ry, sw - 40, row_h - 4, 0x00243450);
                    platform_draw_rect(20, ry, 6, row_h - 4, 0xFFFFFFFF);
                    platform_draw_text(36, ry + 16, ">", 0xFFFFFFFF, 3);
                } else {
                    platform_draw_rect(20, ry, sw - 40, row_h - 4, (i % 2 == 0) ? 0x00111722 : 0x00161E2C);
                }

                char num[8];
                snprintf(num, sizeof(num), "%02d.", i + 1);
                platform_draw_text(60, ry + 16, num, is_sel ? 0xFFFFFFFF : 0x00707070, 2);

                int max_row_w = sw - 140 - 20 - 110;
                if (max_row_w < 100) max_row_w = 100;
                platform_draw_text_clipped(110, ry + 10, max_row_w, pl->title, is_sel ? 0xFFFFFFFF : 0x00E0E0E0, 2);

                char subline[256];
                snprintf(subline, sizeof(subline), "Curador: %s  |  %s  |  [Pulsa X para cargar canciones]",
                         pl->curator[0] ? pl->curator : "Monochrome",
                         pl->description[0] ? pl->description : "Playlist Curada");
                platform_draw_text_clipped(110, ry + 32, max_row_w, subline, is_sel ? 0xFFFFFFFF : 0x0090A4AE, 1);

                char tc[32];
                snprintf(tc, sizeof(tc), "[%u TEMAS]", pl->track_count);
                platform_draw_text_r(sw - 40, ry + 18, tc, is_sel ? 0xFFFFFFFF : 0x00C83838, 2);
            }
        }
    } else {
        platform_draw_text(sw / 2 - 200, sh / 2, "Transmisiones y Podcasts Lossless disponibles proximamente", 0x00808080, 2);
    }

    render_now_playing_mini_bar(sw, sh);
    render_footer(sw, sh, "D-PAD: Mover | X: Seleccionar/Reproducir | L2/R2: Categoria | []: Buscar | START: Menu");
}

static void render_playing_screen(int sw, int sh, long frame)
{
    render_header(sw, STATE_PLAYING, "EN REPRODUCCION (LOSSLESS FLAC)");

    if (s_current_track_idx < 0 || s_current_track_idx >= s_display_count) {
        platform_draw_text(sw / 2 - 150, sh / 2, "Ningun tema en reproduccion", 0xFFFFFFFF, 2);
        render_footer(sw, sh, "O: Volver a la lista");
        return;
    }

    TrackMetadata *t = &s_display_tracks[s_current_track_idx];

    /* Card Left: Cover Art Box */
    int cover_x = 80;
    int cover_y = 106;
    int cover_size = 320;
    platform_draw_rect(cover_x - 3, cover_y - 3, cover_size + 6, cover_size + 6, 0xFFFFFFFF);
    platform_draw_rect(cover_x, cover_y, cover_size, cover_size, 0x001A2436);

    /* Draw cover art if loaded, otherwise show placeholder */
    if (cover_art_is_ready()) {
        platform_draw_texture(cover_art_get_texture(), 
                             cover_art_get_width(), cover_art_get_height(),
                             cover_x, cover_y, cover_size, cover_size);
    } else {
        platform_draw_rect(cover_x + 30, cover_y + 30, cover_size - 60, cover_size - 60, 0x000F1520);
        platform_draw_text(cover_x + 60, cover_y + 130, "MONOCHROME", 0xFFFFFFFF, 3);
        platform_draw_text(cover_x + 100, cover_y + 170, "CARGANDO...", 0x00C83838, 2);
    }

    /* Card Right: Track Details */
    int info_x = cover_x + cover_size + 50;
    int info_y = 105;
    int max_detail_w = sw - info_x - 40;
    if (max_detail_w < 200) max_detail_w = 200;

    int t_len = (int)strlen(t->title);
    int title_scale = (t_len * 24 > max_detail_w) ? 2 : 3;
    platform_draw_text_clipped(info_x, info_y, max_detail_w, t->title, 0xFFFFFFFF, title_scale);
    platform_draw_text_clipped(info_x, info_y + 35, max_detail_w, t->artist, 0xFFFFFFFF, 2);
    if (t->album[0]) {
        platform_draw_text_clipped(info_x, info_y + 65, max_detail_w, t->album, 0x0090A4AE, 2);
    }

    /* Track ID verification box */
    char id_box[256];
    snprintf(id_box, sizeof(id_box), "ID: %s  |  tracks.monochrome.st/track/%s", t->id, t->id);
    int box_w = max_detail_w;
    if (box_w > 640) box_w = 640;
    platform_draw_rect(info_x, info_y + 95, box_w, 24, 0x00101824);
    platform_draw_text_clipped(info_x + 10, info_y + 101, box_w - 20, id_box, 0xFFFFFFFF, 1);

    /* Audio spec badges */
    char dac_spec[128];
    snprintf(dac_spec, sizeof(dac_spec), "FORMATO: LOSSLESS FLAC STREAM  |  SALIDA: 48000 Hz Float Stereo");
    platform_draw_rect(info_x, info_y + 130, box_w, 26, 0x00141C2B);
    platform_draw_text_clipped(info_x + 15, info_y + 137, box_w - 30, dac_spec, 0x00C83838, 1);

    /* 16-Band Visualizer */
    int vis_x = info_x;
    int vis_y = info_y + 200;
    platform_draw_text(vis_x, vis_y - 25, "ANALIZADOR DE ESPECTRO REAL-TIME (16 BANDAS):", 0x00808080, 1);
    float vis_bands[VIS_BANDS];
    audio_engine_get_vis(vis_bands, VIS_BANDS);
    for (int b = 0; b < VIS_BANDS; b++) {
        int bar_h = (int)(vis_bands[b] * 55.0f) + 4;
        if (bar_h > 58) bar_h = 58;
        int bar_x = vis_x + b * 28;
        int bar_y_pos = vis_y + 60 - bar_h;
        platform_draw_rect(bar_x, bar_y_pos, 20, bar_h, 0xFFFFFFFF);
        platform_draw_rect(bar_x, bar_y_pos - 3, 20, 2, 0xFFFFFFFF);
    }

    /* Live Volume & Audio Status */
    char vol_info[64];
    snprintf(vol_info, sizeof(vol_info), "VOLUMEN: %d%%  |  D-PAD ARRIBA/ABAJO: Cambiar  |  UNDERRUNS: %u",
             (int)(audio_engine_get_volume() * 100.0f), audio_engine_get_underruns());
    platform_draw_text(vis_x, vis_y + 75, vol_info, 0x00C83838, 1);

    /* Status message (shows errors like non-FLAC format) */
    const char *status_msg = audio_engine_get_status_message();
    if (status_msg && status_msg[0] != '\0') {
        int status_y = vis_y + 95;
        uint32_t status_color = audio_engine_has_error() ? 0x00C83838 : 0xFFFFFFFF;
        platform_draw_text(vis_x, status_y, status_msg, status_color, 1);
    }

    /* Progress bar */
    int bar_x = 80;
    int bar_y = sh - 150;
    int bar_w = sw - 160;
    int bar_h = 10;
    platform_draw_rect(bar_x, bar_y, bar_w, bar_h, 0x001C1C1C);

    float real_p = audio_engine_get_progress();
    if (real_p > 0.0f) {
        s_playback_progress = real_p;
    }
    int fill_w = (int)(bar_w * s_playback_progress);
    platform_draw_rect(bar_x, bar_y, fill_w, bar_h, 0xFFFFFFFF);

    /* Progress times */
    int cur_sec = (int)(t->duration_sec * s_playback_progress);
    char cur_t[16], tot_t[16];
    snprintf(cur_t, sizeof(cur_t), "%d:%02d", cur_sec / 60, cur_sec % 60);
    snprintf(tot_t, sizeof(tot_t), "%d:%02d", t->duration_sec / 60, t->duration_sec % 60);
    platform_draw_text(bar_x, bar_y + 18, cur_t, 0x00E0E0E0, 2);
    platform_draw_text_r(bar_x + bar_w, bar_y + 18, tot_t, 0x00E0E0E0, 2);

    const char *state_str = (s_playback_state == PLAYBACK_PLAYING) ? "[PLAYING]" : "[PAUSED]";
    platform_draw_text(bar_x + (bar_w / 2) - 50, bar_y + 18, state_str, (s_playback_state == PLAYBACK_PLAYING) ? 0x00C83838 : 0x00C83838, 2);

    render_footer(sw, sh, "X: Pausa/Reanudar  |  < >: Seek  |  L1/R1: Skip  |  O: Volver a Lista");
}

static void render_search_screen(int sw, int sh)
{
    render_header(sw, STATE_SEARCH, "[L1/R1] Filtro | [GO] Buscar");

    int card_w = 860;
    int card_h = 510;
    int card_x = (sw - card_w) / 2;
    int card_y = 70;

    platform_draw_rect(card_x - 3, card_y - 3, card_w + 6, card_h + 6, 0xFFFFFFFF);
    platform_draw_rect(card_x, card_y, card_w, card_h, 0x00141C2B);

    /* Search Sub-Tabs (Tracks, Albums, Artists, Playlists, Podcasts): Grandes y adaptables (36px, scale 2) */
    int subtab_h = 36;
    int subtab_font_scale = 2;
    int subtab_x = card_x + 30;
    int subtab_y = card_y + 16;
    for (int t = 0; t < SEARCH_TAB_COUNT; t++) {
        bool is_sub_active = (t == (int)s_search_active_tab);
        int text_w = (int)strlen(s_search_tab_names[t]) * 8 * subtab_font_scale;
        int tw = text_w + 22;
        if (is_sub_active) {
            platform_draw_rect(subtab_x, subtab_y, tw, subtab_h, 0x00C83838);
            platform_draw_rect(subtab_x, subtab_y + subtab_h - 4, tw, 4, 0xFFFFFFFF);
            int tx = subtab_x + (tw - text_w) / 2;
            int ty = subtab_y + (subtab_h - (8 * subtab_font_scale)) / 2;
            platform_draw_text(tx, ty, s_search_tab_names[t], 0xFFFFFFFF, subtab_font_scale);
        } else {
            platform_draw_rect(subtab_x, subtab_y, tw, subtab_h, 0x00141C2B);
            platform_draw_rect(subtab_x, subtab_y, tw, 1, 0x002B374A);
            int tx = subtab_x + (tw - text_w) / 2;
            int ty = subtab_y + (subtab_h - (8 * subtab_font_scale)) / 2;
            platform_draw_text(tx, ty, s_search_tab_names[t], 0x00A0B0C0, subtab_font_scale);
        }
        subtab_x += tw + 8;
    }
    platform_draw_text_r(card_x + card_w - 30, subtab_y + 10, "[L1/R1] Filtro", 0x00788898, 1);

    /* Query Box */
    int qbox_y = card_y + 60;
    platform_draw_rect(card_x + 30, qbox_y, card_w - 60, 48, 0x000F1520);
    platform_draw_rect(card_x + 30, qbox_y, card_w - 60, 1, 0xFFFFFFFF);

    char q_display[80];
    snprintf(q_display, sizeof(q_display), "Buscar en Monochrome: %s_", s_search_query);
    platform_draw_text_clipped(card_x + 45, qbox_y + 12, card_w - 90, q_display, 0xFFFFFFFF, 2);

    /* OSK Grid */
    int key_w = 68;
    int key_h = 55;
    int grid_x = card_x + 45;
    int grid_y = card_y + 122;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 10; c++) {
            int kx = grid_x + c * (key_w + 10);
            int ky = grid_y + r * (key_h + 10);
            bool is_sel = (r == s_osk_row && c == s_osk_col);

            uint32_t bg = is_sel ? 0xFFFFFFFF : 0x001E2B3E;
            uint32_t fg = is_sel ? 0x00000000 : 0x00E0E0E0;

            platform_draw_rect(kx, ky, key_w, key_h, bg);
            if (is_sel) {
                platform_draw_rect(kx, ky, key_w, 2, 0xFFFFFFFF);
            }

            const char *label = s_osk[r][c];
            int text_scale = (strlen(label) > 1) ? 2 : 3;
            int text_offset_x = (strlen(label) == 3) ? 12 : (strlen(label) == 2) ? 18 : 24;
            platform_draw_text(kx + text_offset_x, ky + 16, label, fg, text_scale);
        }
    }

    platform_draw_text(card_x + 45, card_y + card_h - 38, "D-PAD: Navegar  |  X: Pulsar  |  /\\: Borrar  |  GO: Buscar en vivo  |  O: Cancelar", 0x00A0A0A0, 1);
    render_footer(sw, sh, "Escribe una palabra o artista y pulsa GO para buscar en la API");
}

static void render_queue_screen(int sw, int sh)
{
    render_header(sw, STATE_QUEUE, "COLA DE REPRODUCCION (QUEUE)");

    platform_draw_rect(0, 95, sw, 34, 0x00141824);
    platform_draw_text(30, 58, "Pistas en cola de streaming:", 0xFFFFFFFF, 2);

    int list_y = 95;
    int row_h = 60;
    for (int i = 0; i < s_display_count; i++) {
        int ry = list_y + i * row_h;
        TrackMetadata *t = &s_display_tracks[i];
        bool is_now = (i == s_current_track_idx);

        if (is_now) {
            platform_draw_rect(30, ry, sw - 60, row_h - 6, 0x001B4D2E);
            platform_draw_text(45, ry + 20, "STREAMING >", 0x00C83838, 2);
        } else {
            platform_draw_rect(30, ry, sw - 60, row_h - 6, 0x00141C2B);
            char q_num[16];
            snprintf(q_num, sizeof(q_num), "#%d", i + 1);
            platform_draw_text(45, ry + 20, q_num, 0x00757575, 2);
        }

        int max_q_w = sw - 180 - 100;
        if (max_q_w < 100) max_q_w = 100;
        platform_draw_text_clipped(180, ry + 12, max_q_w, t->title, 0xFFFFFFFF, 2);

        char sub[256];
        snprintf(sub, sizeof(sub), "%s | ID: %s", t->artist, t->id);
        platform_draw_text_clipped(180, ry + 34, max_q_w, sub, 0x00909090, 1);

        char dur[16];
        snprintf(dur, sizeof(dur), "%d:%02d", t->duration_sec / 60, t->duration_sec % 60);
        platform_draw_text_r(sw - 50, ry + 20, dur, 0x00E0E0E0, 2);
    }

    render_now_playing_mini_bar(sw, sh);
    render_footer(sw, sh, "O: Volver  |  X: Saltar a esta pista  |  START: Salir");
}


/* ── Library Screen (Playlists & Offline Cache) ── */
static void render_library_screen(int sw, int sh)
{
    render_header(sw, STATE_LIBRARY, "BIBLIOTECA & COLECCIONES");

    platform_draw_rect(0, 95, sw, 34, 0x00141824);
    platform_draw_text(30, 103, "+ TUS COLECCIONES, PLAYLISTS & DESCARGAS LOCALES +", 0xFFFFFFFF, 2);

    int card_w = (sw - 80) / 2;
    int card_h = sh - 245;

    /* Card 1: Playlists & Stations */
    platform_draw_rect(30, 138, card_w, card_h, 0x00141C2B);
    platform_draw_rect(30, 110, card_w, 2, 0xFFFFFFFF);
    platform_draw_text(45, 125, "PLAYLISTS Y RADIOS DINAMICAS", 0x00C83838, 2);

    const char *playlists[] = {
        "1. Infinite Radio (Mix automatico en vivo)",
        "2. Daft Punk - Complete Discography FLAC",
        "3. High-Resolution Audio Masterworks (96kHz)",
        "4. French House & Electronic Essentials",
        "5. Lo-Fi & Ambient Study Sessions"
    };
    for (int i = 0; i < 5; i++) {
        bool sel = (i == s_library_cursor);
        int py = 160 + i * 45;
        if (sel) {
            platform_draw_rect(40, py, card_w - 20, 36, 0x00243450);
            platform_draw_rect(40, py, 4, 36, 0xFFFFFFFF);
            platform_draw_text_clipped(50, py + 10, card_w - 40, playlists[i], 0xFFFFFFFF, 2);
        } else {
            platform_draw_text_clipped(50, py + 10, card_w - 40, playlists[i], 0x00A0A0A0, 2);
        }
    }

    /* Card 2: Offline Cache & HDD Storage */
    int card2_x = 30 + card_w + 20;
    platform_draw_rect(card2_x, 138, card_w, card_h, 0x00141C2B);
    platform_draw_rect(card2_x, 110, card_w, 2, 0xFFFFFFFF);
    platform_draw_text(card2_x + 15, 125, "CACHE PERSISTENTE /dev_hdd0", 0x00C83838, 2);

    platform_draw_text(card2_x + 15, 165, "Ruta: /dev_hdd0/tmp/monochrome/cache/", 0x00788898, 1);
    platform_draw_text(card2_x + 15, 185, "Capacidad asignada: Ilimitada (Segun HDD)", 0x00788898, 1);
    platform_draw_text(card2_x + 15, 215, "Las pistas reproducidas se almacenan", 0xFFFFFFFF, 2);
    platform_draw_text(card2_x + 15, 240, "automaticamente en disco para", 0xFFFFFFFF, 2);
    platform_draw_text(card2_x + 15, 265, "reproduccion instantanea offline", 0xFFFFFFFF, 2);
    platform_draw_text(card2_x + 15, 290, "sin consumo de ancho de banda.", 0xFFFFFFFF, 2);

    render_now_playing_mini_bar(sw, sh);
    render_footer(sw, sh, "D-PAD: Navegar | X: Reproducir Playlist | L1/R1: Pestañas | [] Buscar");
}

/* ── Recent History Screen ── */
static void render_recent_screen(int sw, int sh)
{
    render_header(sw, STATE_RECENT, "HISTORIAL RECIENTE");

    platform_draw_rect(0, 95, sw, 34, 0x00141824);
    platform_draw_text(30, 103, "+ PISTAS ESCUCHADAS RECIENTEMENTE +", 0xFFFFFFFF, 2);

    int list_y = 138;
    int row_h = 55;
    int max_rows = (sh - 138 - 100) / row_h;

    if (s_recent_count == 0) {
        platform_draw_text(sw / 2 - 200, sh / 2, "Aun no has reproducido ninguna pista", 0x00808080, 2);
        platform_draw_text(sw / 2 - 160, sh / 2 + 30, "Pulsa [] para buscar musica", 0xFFFFFFFF, 2);
    } else {
        for (int i = 0; i < max_rows && i < s_recent_count; i++) {
            int ry = list_y + i * row_h;
            bool is_sel = (i == s_recent_cursor);
            TrackMetadata *t = &s_recent_tracks[i];

            if (is_sel) {
                platform_draw_rect(20, ry, sw - 40, row_h - 4, 0x00243450);
                platform_draw_rect(20, ry, 6, row_h - 4, 0xFFFFFFFF);
                platform_draw_text(36, ry + 16, ">", 0xFFFFFFFF, 3);
            } else {
                platform_draw_rect(20, ry, sw - 40, row_h - 4, (i % 2 == 0) ? 0x00111722 : 0x00161E2C);
            }

            char num[8];
            snprintf(num, sizeof(num), "%02d.", i + 1);
            platform_draw_text(60, ry + 16, num, is_sel ? 0xFFFFFFFF : 0x00707070, 2);

            int max_rec_w = sw - 120 - 20 - 110;
            if (max_rec_w < 100) max_rec_w = 100;
            platform_draw_text_clipped(110, ry + 10, max_rec_w, t->title, is_sel ? 0xFFFFFFFF : 0x00E0E0E0, 2);

            char subline[256];
            snprintf(subline, sizeof(subline), "%s  |  [ID: %s]", t->artist, t->id);
            platform_draw_text_clipped(110, ry + 32, max_rec_w, subline, is_sel ? 0xFFFFFFFF : 0x00808080, 1);

            char dur[16];
            snprintf(dur, sizeof(dur), "%d:%02d", t->duration_sec / 60, t->duration_sec % 60);
            platform_draw_text_r(sw - 120, ry + 18, dur, 0x00909090, 2);
            platform_draw_text_r(sw - 40, ry + 18, "FLAC", 0x00C83838, 2);
        }
    }

    render_now_playing_mini_bar(sw, sh);
    render_footer(sw, sh, "D-PAD: Navegar | X: Reanudar Pista | L1/R1: Pestañas | [] Buscar");
}

/* ── Unreleased Projects Screen ── */
static void render_unreleased_screen(int sw, int sh)
{
    render_header(sw, STATE_UNRELEASED, "PROYECTOS EXCLUSIVOS");

    platform_draw_rect(0, 95, sw, 34, 0x00141824);
    platform_draw_text(30, 103, ":: ARCHIVO EXCLUSIVO & SESIONES EN VIVO LOSSLESS ::", 0x00C83838, 2);

    const char *unreleased_items[][3] = {
        { "Alive 2007 (Soundboard 48kHz Master Direct Feed)", "Daft Punk", "84:12" },
        { "Justice Live at Accor Arena 2024 (FLAC Archive)", "Justice", "72:45" },
        { "Starboy Studio Outtakes & Unreleased B-Sides", "The Weeknd & Daft Punk", "45:10" },
        { "Tron: Legacy Translucence Sessions (Modular Synth)", "Daft Punk", "58:30" },
        { "Monochrome Community Mixtape #01 (Lossless)", "Various Artists", "63:20" }
    };

    int list_y = 138;
    int row_h = 60;
    for (int i = 0; i < 5; i++) {
        int ry = list_y + i * row_h;
        bool is_sel = (i == s_unreleased_cursor);

        if (is_sel) {
            platform_draw_rect(20, ry, sw - 40, row_h - 4, 0x00243450);
            platform_draw_rect(20, ry, 6, row_h - 4, 0x00C83838);
            platform_draw_text(36, ry + 18, "*", 0x00C83838, 3);
        } else {
            platform_draw_rect(20, ry, sw - 40, row_h - 4, (i % 2 == 0) ? 0x00111722 : 0x00161E2C);
        }

        int max_unr_w = sw - 120 - 20 - 60;
        if (max_unr_w < 100) max_unr_w = 100;
        platform_draw_text_clipped(60, ry + 12, max_unr_w, unreleased_items[i][0], is_sel ? 0xFFFFFFFF : 0x00E0E0E0, 2);
        platform_draw_text_clipped(60, ry + 34, max_unr_w, unreleased_items[i][1], is_sel ? 0x00C83838 : 0x00808080, 1);
        platform_draw_text_r(sw - 120, ry + 20, unreleased_items[i][2], 0x00909090, 2);
        platform_draw_text_r(sw - 40, ry + 20, "EXCLUSIVE", 0x00C83838, 1);
    }

    render_now_playing_mini_bar(sw, sh);
    render_footer(sw, sh, "D-PAD: Seleccionar | X: Transmitir Sesion | L1/R1: Pestañas | START: Menú");
}

/* ── Donate Screen ── */
static void render_donate_screen(int sw, int sh)
{
    render_header(sw, STATE_DONATE, "APOYO AL PROYECTO");

    int card_w = 900;
    int card_h = sh - 200;
    int card_x = (sw - card_w) / 2;
    int card_y = 104;

    platform_draw_rect(card_x - 3, card_y - 3, card_w + 6, card_h + 6, 0x00C83838);
    platform_draw_rect(card_x, card_y, card_w, card_h, 0x00121722);

    platform_draw_text(card_x + 40, card_y + 30, "MONOCHROME MUSIC: STREAMING SIN ANUNCIOS NI DRM", 0xFFFFFFFF, 2);
    platform_draw_text(card_x + 40, card_y + 65, "Un ecosistema de streaming abierto, gratuito y lossless,", 0x00CCCCCC, 2);
    platform_draw_text(card_x + 40, card_y + 90, "creado por la comunidad y portado de forma nativa a la PS3.", 0x00CCCCCC, 2);

    platform_draw_rect(card_x + 40, card_y + 125, card_w - 80, 2, 0x00243450);

    platform_draw_text(card_x + 40, card_y + 145, "ENLACES OFICIALES:", 0x00C83838, 2);
    platform_draw_text(card_x + 40, card_y + 175, "Web oficial:    https://monochrome.st", 0xFFFFFFFF, 2);
    platform_draw_text(card_x + 40, card_y + 205, "Donaciones:     https://monochrome.st/donate", 0xFFFFFFFF, 2);
    platform_draw_text(card_x + 40, card_y + 235, "Repo Web:       github.com/monochrome-music/monochrome", 0x0090A4AE, 2);
    platform_draw_text(card_x + 40, card_y + 265, "Port PS3:       github.com/nopeman321/monochrome-ps3", 0x0090A4AE, 2);

    platform_draw_rect(card_x + 40, card_y + 315, card_w - 80, 45, 0x001B2433);
    platform_draw_text(card_x + 55, card_y + 330, "Agradecimientos a la escena homebrew de PS3 y a los desarrolladores de PSL1GHT.", 0x00788898, 1);

    render_footer(sw, sh, "L1/R1: Pestañas | [] Buscar | /\\ Cola | START: Menú de Sistema");
}

/* ── Settings Screen ── */
static void render_settings_screen(int sw, int sh)
{
    render_header(sw, STATE_SETTINGS, "CONFIGURACION DEL SISTEMA");

    platform_draw_rect(0, 95, sw, 34, 0x00141824);
    platform_draw_text(30, 103, "+ AJUSTES DE AUDIO, PANTALLA Y RED +", 0xFFFFFFFF, 2);

    const char *settings_items[] = {
        "1. Calibracion de Pantalla (Overscan, Offset X/Y y Escala)",
        "2. Configuracion de Red (Timeouts HTTP y Reintentos 429)",
        "3. Diagnostico del Sistema (Cell B.E., RSX y Memoria XDR)",
        "4. Restablecer Pantalla a Valores de Fabrica"
    };

    int card_w = 940;
    int card_h = sh - 250;
    int card_x = (sw - card_w) / 2;
    int card_y = 140;

    platform_draw_rect(card_x, card_y, card_w, card_h, 0x00141C2B);
    platform_draw_rect(card_x, card_y, card_w, 2, 0xFFFFFFFF);

    for (int i = 0; i < 4; i++) {
        int py = card_y + 30 + i * 65;
        bool sel = (i == s_settings_cursor);
        if (sel) {
            platform_draw_rect(card_x + 20, py, card_w - 40, 50, 0x00243450);
            platform_draw_rect(card_x + 20, py, 6, 50, 0x00C83838);
            platform_draw_text_clipped(card_x + 35, py + 16, card_w - 60, settings_items[i], 0xFFFFFFFF, 2);
        } else {
            platform_draw_rect(card_x + 20, py, card_w - 40, 50, 0x00101520);
            platform_draw_text_clipped(card_x + 35, py + 16, card_w - 60, settings_items[i], 0x00A0B0C0, 2);
        }
    }

    render_footer(sw, sh, "D-PAD: Navegar | X: Abrir Ajuste | L1/R1: Pestañas | START: Guardar");
}

/* ── About Screen ── */
static void render_about_screen(int sw, int sh)
{
    render_header(sw, STATE_ABOUT, "ACERCA DE MONOCHROME");

    int card_w = 920;
    int card_h = sh - 200;
    int card_x = (sw - card_w) / 2;
    int card_y = 104;

    platform_draw_rect(card_x - 3, card_y - 3, card_w + 6, card_h + 6, 0xFFFFFFFF);
    platform_draw_rect(card_x, card_y, card_w, card_h, 0x00141C2B);

    platform_draw_text(card_x + 35, card_y + 25, "MONOCHROME PS3 CLIENT", 0xFFFFFFFF, 3);
    platform_draw_text(card_x + 460, card_y + 31, APP_DISPLAY_VERSION, 0x00C83838, 2);

    int y = card_y + 70;
    platform_draw_text(card_x + 35, y, "Title ID:          MONO00024 (Free NPDRM CEX / CFW / HEN)", 0x00CCCCCC, 1); y += 24;
    platform_draw_text(card_x + 35, y, "Target Hardware:   Sony PlayStation 3 (Cell B.E. + RSX)", 0x00CCCCCC, 1); y += 24;
    platform_draw_text(card_x + 35, y, "SDK Toolchain:     PSL1GHT v2 (GCC 7.2.0 PPU / newlib)", 0x00CCCCCC, 1); y += 24;
    platform_draw_text(card_x + 35, y, "Audio Architecture: GameOS 48.000 Hz Float32 DMA (256 samples/block)", 0x00CCCCCC, 1); y += 24;
    platform_draw_text(card_x + 35, y, "FLAC Decoding:     dr_flac progressive non-blocking engine", 0x00CCCCCC, 1); y += 24;
    platform_draw_text(card_x + 35, y, "Resampling:        Exact 147:160 linear interpolation", 0x00CCCCCC, 1); y += 24;
    platform_draw_text(card_x + 35, y, "Cover Art Engine:  libjpeg asincrono (hilo mono_cover)", 0x00CCCCCC, 1); y += 24;
    platform_draw_text(card_x + 35, y, "Official API:      tracks.monochrome.st (Port 80 HTTP/1.1)", 0x00C83838, 1); y += 35;

    platform_draw_rect(card_x + 35, y, card_w - 70, 35, 0x000F1520);
    platform_draw_text(card_x + 45, y + 10, "Desarrollado para disfrute de musica libre y audio de alta fidelidad.", 0x00808080, 1);

    render_footer(sw, sh, "L1/R1: Pestañas | [] Buscar | /\\ Cola | START: Menú");
}

void ui_render(AppState state, long frame)
{
    /* Poll background async search */
    int count = 0, hcode = 0, herrn = 0;
    SearchStatus st = http_search_async_poll(s_display_tracks, &count, MAX_SEARCH_RESULTS, &hcode, &herrn);
    if (st == SEARCH_STATUS_OK) {
        s_display_count = count;
        s_display_album_count = http_search_get_albums(s_display_albums, MAX_SEARCH_RESULTS);
        s_display_artist_count = http_search_get_artists(s_display_artists, MAX_SEARCH_RESULTS);
        s_display_playlist_count = http_search_get_playlists(s_display_playlists, MAX_SEARCH_RESULTS);
        s_search_status = SEARCH_STATUS_OK;
        s_last_http_code = hcode;
        s_cursor = 0;
        if (state == STATE_PLAYING && s_current_track_idx < 0 && count > 0) {
            s_current_track_idx = 0;
            s_playback_state = PLAYBACK_PLAYING;
            s_playback_progress = 0.0f;
            play_track_with_cover(0);
        }
    } else if (st == SEARCH_STATUS_NO_RESULTS) {
        s_display_count = 0;
        s_display_album_count = 0;
        s_display_artist_count = 0;
        s_display_playlist_count = 0;
        s_search_status = SEARCH_STATUS_NO_RESULTS;
        s_last_http_code = hcode;
    } else if (st == SEARCH_STATUS_ERROR) {
        s_display_count = 0;
        s_display_album_count = 0;
        s_display_artist_count = 0;
        s_display_playlist_count = 0;
        s_search_status = SEARCH_STATUS_ERROR;
        s_last_http_code = hcode;
        s_last_errno = herrn;
    }

    int sw = platform_get_width();
    int sh = platform_get_height();

    platform_clear(0x00000000);

    /* Minimalist metalheart ASCII background */
    render_ascii_metalheart_background(sw, sh);

    switch (state) {
        case STATE_HOME:
            render_home_screen(sw, sh);
            break;
        case STATE_SEARCH:
            render_search_screen(sw, sh);
            break;
        case STATE_LIBRARY:
            render_library_screen(sw, sh);
            break;
        case STATE_RECENT:
            render_recent_screen(sw, sh);
            break;
        case STATE_UNRELEASED:
            render_unreleased_screen(sw, sh);
            break;
        case STATE_DONATE:
            render_donate_screen(sw, sh);
            break;
        case STATE_SETTINGS:
            render_settings_screen(sw, sh);
            break;
        case STATE_ABOUT:
            render_about_screen(sw, sh);
            break;
        case STATE_PLAYING:
            render_playing_screen(sw, sh, frame);
            break;
        case STATE_QUEUE:
            render_queue_screen(sw, sh);
            break;
        default:
            render_home_screen(sw, sh);
            break;
    }

    /* Show error message overlay if track is not available in HiFi format */
    if (audio_engine_has_error() && s_playback_state == PLAYBACK_PLAYING) {
        const char *err_msg = audio_engine_get_status_message();
        if (err_msg && strstr(err_msg, "no disponible en FLAC")) {
            /* Draw error banner at bottom of screen */
            platform_draw_rect(0, sh - 60, sw, 60, 0xE0000000);
            platform_draw_text(20, sh - 40, "No disponible en calidad HiFi (FLAC)", 0x00C83838, 2);
            platform_draw_text(20, sh - 20, "Presiona X para intentar siguiente track", 0x00CCCCCC, 1);
        }
    }

    /* Screen adjustment overlay */
    if (s_screen_adjust_mode) {
        /* Draw semi-transparent overlay */
        platform_draw_rect(0, 0, sw, 100, 0x80000000);
        
        /* Draw instructions */
        char buf[128];
        snprintf(buf, sizeof(buf), "SCREEN ADJUST");
        platform_draw_text(20, 15, buf, 0xFFFFFF, 2);
        
        snprintf(buf, sizeof(buf), "D-Pad=Move | L2/R2=Scale | L1/R1=Font | START=Save | SELECT=Reset");
        platform_draw_text(20, 40, buf, 0xFFFFFFFF, 1);
        
        snprintf(buf, sizeof(buf), "Offset: (%d, %d) | Scale: %.2f | Font: %.2f",
                 s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale, s_adjust_font_scale);
        platform_draw_text(20, 65, buf, 0xFFFF00, 2);
    }

    /* Menu overlay */
    if (s_menu_open) {
        /* Draw semi-transparent background */
        int menu_w = 400;
        int menu_h = 300;  /* Increased for 4 items */
        int menu_x = (sw - menu_w) / 2;
        int menu_y = (sh - menu_h) / 2;
        
        platform_draw_rect(menu_x, menu_y, menu_w, menu_h, 0xE0000000);
        platform_draw_rect(menu_x + 2, menu_y + 2, menu_w - 4, menu_h - 4, 0x202030);
        
        /* Draw title */
        platform_draw_text(menu_x + 20, menu_y + 20, "MENU", 0xFFFFFFFF, 3);
        
        /* Draw menu items */
        for (int i = 0; i < MENU_ITEMS; i++) {
            uint32_t color = (i == s_menu_cursor) ? 0x00C83838 : 0xFFFFFF;
            char item_text[64];
            snprintf(item_text, sizeof(item_text), "> %s", s_menu_items[i]);
            platform_draw_text(menu_x + 40, menu_y + 80 + (i * 40), item_text, color, 2);
        }
        
        /* Draw instructions */
        platform_draw_text(menu_x + 20, menu_y + menu_h - 30, "D-Pad=Navigate | X=Select | O=Close", 0x808080, 1);
    }

    /* System Info overlay */
    if (s_show_system_info) {
        /* Draw semi-transparent background */
        int info_w = 600;
        int info_h = 400;
        int info_x = (sw - info_w) / 2;
        int info_y = (sh - info_h) / 2;
        
        platform_draw_rect(info_x, info_y, info_w, info_h, 0xE0000000);
        platform_draw_rect(info_x + 2, info_y + 2, info_w - 4, info_h - 4, 0x202030);
        
        /* Draw title */
        platform_draw_text(info_x + 20, info_y + 20, "SYSTEM INFO", 0xFFFFFFFF, 3);
        
        /* Draw system information */
        char buf[256];
        int y_offset = info_y + 80;
        
        snprintf(buf, sizeof(buf), "App: Monochrome PS3 %s", APP_DISPLAY_VERSION);
        platform_draw_text(info_x + 40, y_offset, buf, 0xFFFFFF, 2);
        y_offset += 40;
        
        snprintf(buf, sizeof(buf), "Title ID: %s", APP_TITLEID);
        platform_draw_text(info_x + 40, y_offset, buf, 0xFFFFFF, 2);
        y_offset += 40;
        
        snprintf(buf, sizeof(buf), "Screen: %dx%d", sw, sh);
        platform_draw_text(info_x + 40, y_offset, buf, 0xFFFFFF, 2);
        y_offset += 40;
        
        snprintf(buf, sizeof(buf), "Adjust: offset=(%d, %d) scale=%.2f", 
                 s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale);
        platform_draw_text(info_x + 40, y_offset, buf, 0xFFFFFF, 2);
        y_offset += 40;
        
        snprintf(buf, sizeof(buf), "Network timeout: %ds", s_network_timeout);
        platform_draw_text(info_x + 40, y_offset, buf, 0xFFFFFF, 2);
        y_offset += 40;
        
        snprintf(buf, sizeof(buf), "Max retries: %d", s_max_retries);
        platform_draw_text(info_x + 40, y_offset, buf, 0xFFFFFF, 2);
        y_offset += 40;
        
        /* Draw instructions */
        platform_draw_text(info_x + 20, info_y + info_h - 30, "Press O or START to close", 0x808080, 1);
    }

    /* Network Settings overlay */
    if (s_show_network_settings) {
        /* Draw semi-transparent background */
        int net_w = 600;
        int net_h = 300;
        int net_x = (sw - net_w) / 2;
        int net_y = (sh - net_h) / 2;
        
        platform_draw_rect(net_x, net_y, net_w, net_h, 0xE0000000);
        platform_draw_rect(net_x + 2, net_y + 2, net_w - 4, net_h - 4, 0x202030);
        
        /* Draw title */
        platform_draw_text(net_x + 20, net_y + 20, "NETWORK SETTINGS", 0xFFFFFFFF, 3);
        
        /* Draw settings */
        char buf[256];
        int y_offset = net_y + 80;
        
        int timeout = http_get_timeout_ms() / 1000;
        int retries = http_get_max_retries();
        
        /* Timeout setting */
        uint32_t color0 = (s_network_cursor == 0) ? 0x00C83838 : 0xFFFFFF;
        snprintf(buf, sizeof(buf), "> Timeout: %d seconds  (LEFT/RIGHT to adjust)", timeout);
        platform_draw_text(net_x + 40, y_offset, buf, color0, 2);
        y_offset += 50;
        
        /* Max retries setting */
        uint32_t color1 = (s_network_cursor == 1) ? 0x00C83838 : 0xFFFFFF;
        snprintf(buf, sizeof(buf), "> Max retries: %d  (LEFT/RIGHT to adjust)", retries);
        platform_draw_text(net_x + 40, y_offset, buf, color1, 2);
        y_offset += 50;
        
        /* Instructions */
        platform_draw_text(net_x + 20, net_y + net_h - 50, "D-Pad=Navigate & Adjust", 0x808080, 1);
        platform_draw_text(net_x + 20, net_y + net_h - 30, "Press O or START to save & close", 0x808080, 1);
    }
}
