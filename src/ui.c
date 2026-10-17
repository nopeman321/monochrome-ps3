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

/* ── Init / Shutdown ─────────────────────────────────────────────── */
int gfx_init(void) {
    platform_gfx_init(SCREEN_WIDTH, SCREEN_HEIGHT, "Monochrome — Lossless Music Player");

    printf("[RSX] Framebuffer: %dx%d @ 60 Hz (double-buffered)\n",
           SCREEN_WIDTH, SCREEN_HEIGHT);
    printf("[RSX] VRAM allocated: 2× %.1f MB (framebuffers) + 16 MB (textures)\n",
           (float)(SCREEN_WIDTH * SCREEN_HEIGHT * 4) / (1024.0f * 1024.0f));
    return 0;
}

void gfx_shutdown(void) {
    platform_gfx_shutdown();
    printf("[RSX] Context released.\n");
}

/* ── Render one frame ────────────────────────────────────────────── */
void gfx_render(AppState state, const TrackMetadata *track,
                PlaybackState pstate, float progress) {
    /* Lazy-load initial catalogue on first render */
    if (!g_initial_load) {
        g_track_count = net_search("trending", g_tracks, MAX_SEARCH_RESULTS);
        g_initial_load = true;
    }

    /*
     * Real RSX pipeline:
     *   1. gcmSetClearColor(0x00000000)      — pure black
     *   2. gcmSetClearSurface(COLOR|DEPTH)
     *   3. Draw header bar:  "MONOCHROME" in white sans-serif
     *   4. Draw track list:  white text, highlighted row has #FFFFFF border
     *   5. Draw now-playing bar at bottom:
     *        ▶ Title — Artist — Album       [HI-RES 24/96]
     *        ████████████░░░░░░░░░░░░░░░░  2:15 / 3:50
     *   6. gcmSetFlip(buffer_id)             — VSync
     */

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
                        s_search_presets[s_preset_idx]);
}

/* ── Input polling → state transitions ───────────────────────────── */
AppState pad_update(AppState current) {
    InputAction act = platform_poll_input();

    /* Direct mouse track selection support */
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
                printf("[INPUT] Select: Playing [%d/%d] \"%s\"\n",
                       g_cursor + 1, g_track_count, g_tracks[g_cursor].title);
                TrackMetadata *cur = app_get_current_track();
                if (cur) {
                    audio_reset_progress();
                    *cur = g_tracks[g_cursor];
                    char url[512];
                    net_resolve_stream(cur->id, url, sizeof(url));
                    net_start_stream(url, app_get_ring());
                    audio_set_state(PLAYBACK_PLAYING);
                    current = STATE_PLAYING;
                }
            }
            break;

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
            s_preset_idx = (s_preset_idx + 1) % NUM_SEARCH_PRESETS;
            printf("[INPUT] Search triggered for genre/query: '%s'...\n", s_search_presets[s_preset_idx]);
            current = STATE_SEARCH;
            g_track_count = net_search(s_search_presets[s_preset_idx], g_tracks, MAX_SEARCH_RESULTS);
            g_cursor = 0;
            current = STATE_HOME;
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

