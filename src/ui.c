/**
 * Monochrome PS3 — RSX graphics & DualShock 3 input
 *
 * Renders a minimalist black-and-white UI inspired by the Monochrome
 * web client.  On real hardware this would use Tiny3D / libgcm for
 * RSX command lists and FreeType2 for glyph rendering.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "monochrome.h"

/* ── Cached search results & Presets ─────────────────────────────── */
static TrackMetadata g_tracks[MAX_SEARCH_RESULTS];
static int  g_track_count  = 0;
static int  g_cursor       = 0;
static bool g_initial_load = false;

static const char *s_search_presets[] = {
    "trending",
    "Daft Punk",
    "The Weeknd",
    "Synthwave",
    "Lossless",
    "Electronic"
};
#define NUM_SEARCH_PRESETS (sizeof(s_search_presets) / sizeof(s_search_presets[0]))
static int s_preset_idx = 0;

/* ── Queue, Repeat & Shuffle Management ───────────────────────────── */
static RepeatMode s_repeat_mode  = REPEAT_ALL;
static bool       s_shuffle_mode = false;

int  ui_get_repeat_mode(void) { return (int)s_repeat_mode; }
bool ui_get_shuffle_mode(void) { return s_shuffle_mode; }
void ui_set_repeat_mode(int mode) { s_repeat_mode = (RepeatMode)(mode % 3); }
void ui_set_shuffle_mode(bool shuffle) { s_shuffle_mode = shuffle; }

static void ui_play_track_at(int idx) {
    if (idx < 0 || idx >= g_track_count) return;
    g_cursor = idx;
    TrackMetadata *cur = app_get_current_track();
    if (cur) {
        audio_reset_progress();
        *cur = g_tracks[g_cursor];
        char url[512];
        net_resolve_stream(cur->id, url, sizeof(url));
        net_start_stream(url, app_get_ring());
        cover_art_request(cur->id, cur->cover_url);
        printf("[UI] Playing [%d/%d]: \"%s\" by %s\n",
               g_cursor + 1, g_track_count, cur->title, cur->artist);
    }
}

void ui_track_next(void) {
    if (g_track_count <= 0) return;
    if (s_repeat_mode == REPEAT_ONE) {
        ui_play_track_at(g_cursor);
        return;
    }
    if (s_shuffle_mode) {
        int next_idx = (g_track_count > 1) ? ((g_cursor + 1 + (rand() % (g_track_count - 1))) % g_track_count) : 0;
        ui_play_track_at(next_idx);
        return;
    }
    if (s_repeat_mode == REPEAT_ALL) {
        int next_idx = (g_cursor + 1) % g_track_count;
        ui_play_track_at(next_idx);
        return;
    }
    /* REPEAT_OFF */
    if (g_cursor + 1 < g_track_count) {
        ui_play_track_at(g_cursor + 1);
    } else {
        audio_set_state(PLAYBACK_STOPPED);
        printf("[UI] End of queue reached (Repeat OFF).\n");
    }
}

void ui_track_prev(void) {
    if (g_track_count <= 0) return;
    if (s_shuffle_mode) {
        int prev_idx = (g_track_count > 1) ? ((g_cursor + 1 + (rand() % (g_track_count - 1))) % g_track_count) : 0;
        ui_play_track_at(prev_idx);
        return;
    }
    int prev_idx = (g_cursor - 1 + g_track_count) % g_track_count;
    ui_play_track_at(prev_idx);
}

/* ── Init / Shutdown ─────────────────────────────────────────────── */
int gfx_init(void) {
    platform_gfx_init(SCREEN_WIDTH, SCREEN_HEIGHT, "Monochrome — Lossless Music Player");

    printf("[RSX] Framebuffer: %dx%d @ 60 Hz (double-buffered)\n",
           SCREEN_WIDTH, SCREEN_HEIGHT);
    printf("[RSX] VRAM allocated: 2× %.1f MB (framebuffers) + 16 MB (textures)\n",
           (float)(SCREEN_WIDTH * SCREEN_HEIGHT * 4) / (1024.0f * 1024.0f));

    /* Pre-load initial catalogue immediately so it is available before first input tick */
    if (!g_initial_load) {
        g_track_count = net_search("trending", g_tracks, MAX_SEARCH_RESULTS);
        g_initial_load = true;
        if (g_track_count > 0) {
            cover_art_request(g_tracks[0].id, g_tracks[0].cover_url);
        }
    }
    return 0;
}

void gfx_shutdown(void) {
    platform_gfx_shutdown();
    printf("[RSX] Context released.\n");
}

static char g_search_query[64] = "Daft Punk";
static int  g_search_query_len = 9;
static int  g_osk_row = 1;
static int  g_osk_col = 0;

static const char *s_osk_grid[4][10] = {
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
    { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" },
    { "A", "S", "D", "F", "G", "H", "J", "K", "L", "-" },
    { "Z", "X", "C", "V", "B", "N", "M", "SPC", "DEL", "GO" }
};

void ui_set_search_query(const char *query) {
    if (!query) return;
    snprintf(g_search_query, sizeof(g_search_query), "%s", query);
    g_search_query_len = (int)strlen(g_search_query);
    net_search_async_query(g_search_query);
}

const char *ui_get_search_query(void) {
    return g_search_query;
}

void ui_get_osk_state(int *row, int *col, const char **query) {
    if (row)   *row   = g_osk_row;
    if (col)   *col   = g_osk_col;
    if (query) *query = g_search_query;
}

void ui_osk_input_char(char ch) {
    if (ch == '\b' || ch == 127) {
        if (g_search_query_len > 0) {
            g_search_query[--g_search_query_len] = '\0';
            net_search_async_query(g_search_query);
        }
    } else if (ch >= 32 && ch < 127) {
        if (g_search_query_len < (int)sizeof(g_search_query) - 1) {
            g_search_query[g_search_query_len++] = ch;
            g_search_query[g_search_query_len] = '\0';
            net_search_async_query(g_search_query);
        }
    }
}

void ui_osk_set_cursor(int row, int col) {
    if (row >= 0 && row < 4)  g_osk_row = row;
    if (col >= 0 && col < 10) g_osk_col = col;
}

/* ── Render one frame ────────────────────────────────────────────── */
void gfx_render(AppState state, const TrackMetadata *track,
                PlaybackState pstate, float progress) {
    /* Lazy-load initial catalogue on first render */
    if (!g_initial_load) {
        g_track_count = net_search("trending", g_tracks, MAX_SEARCH_RESULTS);
        g_initial_load = true;
    }

    /* Poll background async search worker */
    bool is_searching = false;
    int async_count = net_search_async_poll(g_tracks, MAX_SEARCH_RESULTS, &is_searching);
    if (async_count >= 0) {
        g_track_count = async_count;
        if (g_cursor >= g_track_count) g_cursor = 0;
        if (g_track_count > 0) {
            cover_art_request(g_tracks[g_cursor].id, g_tracks[g_cursor].cover_url);
        }
    }

    static uint32_t frame_count = 0;
    frame_count++;

    if (state == STATE_PLAYING && track && track->title[0] != '\0') {
        if (frame_count % 60 == 0) {
            AudioRingBuffer *rb = app_get_ring();
            size_t avail = rb ? ring_available(rb) : 0;
            printf("[NOW PLAYING] ▶ %s — %s [%s] | DAC: %u Hz %d-bit | XDR Buf: %zu KB\n",
                   track->title, track->artist, track->album,
                   track->sample_rate, (int)track->bit_depth, avail / 1024);
        }
    } else {
        if (frame_count == 1 || frame_count % 60 == 0) {
            const char *tname = (g_track_count > 0) ? g_tracks[g_cursor].title : "None";
            const char *aname = (g_track_count > 0) ? g_tracks[g_cursor].artist : "None";
            printf("[RSX] Frame %5u (60.0 FPS) | State: %d | Highlight [%d/%d]: %s - %s\n",
                   frame_count, (int)state, g_cursor + 1, g_track_count, tname, aname);
        }
    }

    /* Hardware Abstraction Layer Visual Renderer (Win32 GDI / PS3 RSX) */
    platform_gfx_render((int)state, track, (int)pstate, progress,
                        g_tracks, g_track_count, g_cursor,
                        g_search_query);
}

/* ── Input polling → state transitions ───────────────────────────── */
AppState pad_update(AppState current) {
    InputAction act = platform_poll_input();

    /* ── SEARCH STATE (On-Screen Keyboard Overlay) ─────────────────── */
    if (current == STATE_SEARCH) {
        switch (act) {
            case INPUT_UP:
                g_osk_row = (g_osk_row - 1 + 4) % 4;
                break;
            case INPUT_DOWN:
                g_osk_row = (g_osk_row + 1) % 4;
                break;
            case INPUT_LEFT:
                g_osk_col = (g_osk_col - 1 + 10) % 10;
                break;
            case INPUT_RIGHT:
                g_osk_col = (g_osk_col + 1) % 10;
                break;

            case INPUT_SELECT: {
                const char *key = s_osk_grid[g_osk_row][g_osk_col];
                if (strcmp(key, "GO") == 0) {
                    if (g_search_query_len > 0) {
                        printf("[UI] Triggering async search for: '%s'...\n", g_search_query);
                        net_search_async_query(g_search_query);
                        g_cursor = 0;
                    }
                    current = STATE_HOME;
                } else if (strcmp(key, "DEL") == 0) {
                    if (g_search_query_len > 0) {
                        g_search_query[--g_search_query_len] = '\0';
                        net_search_async_query(g_search_query);
                    }
                } else if (strcmp(key, "SPC") == 0) {
                    if (g_search_query_len < (int)sizeof(g_search_query) - 1) {
                        g_search_query[g_search_query_len++] = ' ';
                        g_search_query[g_search_query_len] = '\0';
                        net_search_async_query(g_search_query);
                    }
                } else {
                    if (g_search_query_len < (int)sizeof(g_search_query) - 1) {
                        g_search_query[g_search_query_len++] = key[0];
                        g_search_query[g_search_query_len] = '\0';
                        net_search_async_query(g_search_query);
                    }
                }
                break;
            }

            case INPUT_PLAY_PAUSE: /* Triangle / Space shortcut for Delete in OSK */
                if (g_search_query_len > 0) {
                    g_search_query[--g_search_query_len] = '\0';
                    net_search_async_query(g_search_query);
                }
                break;

            case INPUT_ACTION_SEARCH: /* Cycle quick suggestions */
                s_preset_idx = (s_preset_idx + 1) % NUM_SEARCH_PRESETS;
                snprintf(g_search_query, sizeof(g_search_query), "%s", s_search_presets[s_preset_idx]);
                g_search_query_len = (int)strlen(g_search_query);
                printf("[UI] Quick preset: '%s'\n", g_search_query);
                net_search_async_query(g_search_query);
                break;

            case INPUT_CANCEL:
                printf("[UI] Search cancelled.\n");
                current = STATE_HOME;
                break;

            case INPUT_QUIT:
                app_request_exit();
                break;

            default:
                break;
        }
        return current;
    }

    /* ── HOME & PLAYING STATES ─────────────────────────────────────── */
    /* Auto-advance queue when current track finishes */
    if (current == STATE_PLAYING && audio_get_state() == PLAYBACK_STOPPED) {
        AudioRingBuffer *rb = app_get_ring();
        if (rb && rb->stream_eof) {
            printf("[UI] Track complete, auto-advancing queue...\n");
            ui_track_next();
            if (audio_get_state() == PLAYBACK_STOPPED) {
                current = STATE_HOME;
            }
        }
    }

    int mouse_sel = platform_get_mouse_track_selection();
    if (mouse_sel >= 0 && mouse_sel < g_track_count) {
        g_cursor = mouse_sel;
        act = INPUT_SELECT;
    }

    switch (act) {
        case INPUT_UP:
            if (g_track_count > 1) {
                g_cursor = (g_cursor - 1 + g_track_count) % g_track_count;
                printf("[INPUT] Navigate UP: [%d/%d] \"%s\" by %s\n",
                       g_cursor + 1, g_track_count, g_tracks[g_cursor].title, g_tracks[g_cursor].artist);
            }
            break;

        case INPUT_DOWN:
            if (g_track_count > 1) {
                g_cursor = (g_cursor + 1) % g_track_count;
                printf("[INPUT] Navigate DOWN: [%d/%d] \"%s\" by %s\n",
                       g_cursor + 1, g_track_count, g_tracks[g_cursor].title, g_tracks[g_cursor].artist);
            }
            break;

        case INPUT_SELECT:
            if (g_track_count > 0) {
                TrackMetadata *cur = app_get_current_track();
                if (cur && strcmp(cur->id, g_tracks[g_cursor].id) == 0 &&
                    (audio_get_state() == PLAYBACK_PLAYING || audio_get_state() == PLAYBACK_BUFFERING)) {
                    /* Already streaming this track */
                    break;
                }
                ui_play_track_at(g_cursor);
                current = STATE_PLAYING;
            }
            break;

        case INPUT_NEXT_TRACK:
            printf("[INPUT] Next Track (⏭)\n");
            ui_track_next();
            current = STATE_PLAYING;
            break;

        case INPUT_PREV_TRACK:
            printf("[INPUT] Previous Track (⏮)\n");
            ui_track_prev();
            current = STATE_PLAYING;
            break;

        case INPUT_SHUFFLE:
            s_shuffle_mode = !s_shuffle_mode;
            printf("[INPUT] Shuffle Mode: %s (🔀)\n", s_shuffle_mode ? "ON" : "OFF");
            break;

        case INPUT_REPEAT: {
            s_repeat_mode = (RepeatMode)((s_repeat_mode + 1) % 3);
            static const char *s_rep_names[] = { "OFF", "ALL", "ONE" };
            printf("[INPUT] Repeat Mode: %s (🔁)\n", s_rep_names[s_repeat_mode]);
            break;
        }

        case INPUT_PLAY_PAUSE:
            if (audio_get_state() == PLAYBACK_PLAYING) {
                audio_set_state(PLAYBACK_PAUSED);
                printf("[INPUT] Playback Paused (⏸)\n");
            } else if (audio_get_state() == PLAYBACK_PAUSED) {
                audio_set_state(PLAYBACK_PLAYING);
                printf("[INPUT] Playback Resumed (▶)\n");
            }
            break;

        case INPUT_ACTION_SEARCH:
            printf("[INPUT] Opening Search On-Screen Keyboard (OSK)...\n");
            current = STATE_SEARCH;
            break;

        case INPUT_QUIT:
            printf("[INPUT] Exit requested.\n");
            app_request_exit();
            break;

        default:
            break;
    }

    return current;
}


