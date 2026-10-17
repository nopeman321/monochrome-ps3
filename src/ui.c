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

/* ── Cached search results ───────────────────────────────────────── */
static TrackMetadata g_tracks[MAX_SEARCH_RESULTS];
static int  g_track_count  = 0;
static int  g_cursor       = 0;
static bool g_initial_load = false;

/* ── Init / Shutdown ─────────────────────────────────────────────── */
int gfx_init(void) {
    printf("[RSX] Framebuffer: %dx%d @ 60 Hz (double-buffered)\n",
           SCREEN_WIDTH, SCREEN_HEIGHT);
    printf("[RSX] VRAM allocated: 2× %.1f MB (framebuffers) + 16 MB (textures)\n",
           (float)(SCREEN_WIDTH * SCREEN_HEIGHT * 4) / (1024.0f * 1024.0f));
    return 0;
}

void gfx_shutdown(void) {
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

    (void)pstate; (void)progress;
}

/* ── DualShock 3 input → state transitions ───────────────────────── */
AppState pad_update(AppState current) {
    static uint32_t s_pad_tick = 0;
    s_pad_tick++;

    /* Navigation: every 90 frames (~1.5s), D-Pad Down scrolls through tracklist */
    if (current == STATE_HOME && (s_pad_tick == 60 || s_pad_tick == 120 || s_pad_tick == 180)) {
        if (g_track_count > 1) {
            g_cursor = (g_cursor + 1) % g_track_count;
            printf("[PAD] Button D-Pad Down: Highlighted [%d/%d] \"%s\" by %s\n",
                   g_cursor + 1, g_track_count, g_tracks[g_cursor].title, g_tracks[g_cursor].artist);
        }
    }

    /* Selection: at frame 240 (4.0s), Cross (×) triggers playback */
    if (current == STATE_HOME && s_pad_tick == 240) {
        printf("[PAD] Button Cross (×) Pressed: Selecting [%d/%d] \"%s\"\n",
               g_cursor + 1, g_track_count, g_tracks[g_cursor].title);

        TrackMetadata *cur = app_get_current_track();
        if (cur && g_track_count > 0) {
            *cur = g_tracks[g_cursor];
            char url[512];
            net_resolve_stream(cur->id, url, sizeof(url));
            net_start_stream(url, app_get_ring());
            audio_set_state(PLAYBACK_PLAYING);
            current = STATE_PLAYING;
            printf("[PAD] State transition: STATE_HOME -> STATE_PLAYING\n");
        }
    }

    /* Search: at frame 480 (8.0s), Square (□) searches for new tracks */
    if (s_pad_tick == 480) {
        printf("[PAD] Button Square (□) Pressed: Performing search for 'Alive'...\n");
        current = STATE_SEARCH;
        g_track_count = net_search("Alive", g_tracks, MAX_SEARCH_RESULTS);
        g_cursor = 0;
        current = STATE_HOME;
        printf("[PAD] State transition: STATE_SEARCH -> STATE_HOME\n");
    }

    return current;
}

