/**
 * Monochrome PS3 — Main entry point & Cell B.E. system controller
 *
 * PPE Thread 0: main loop — pad input → state machine → RSX render → VSync
 * PPE Thread 1: network daemon (launched inside net_init)
 * SPU 0-1:      FLAC decode + resample (dispatched by audio_tick)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "monochrome.h"

/* ── Globals ─────────────────────────────────────────────────────── */
static volatile bool  g_running = true;
static AppState       g_state   = STATE_SPLASH;
static TrackMetadata  g_now     = {0};
static AudioRingBuffer g_ring   = {0};
static float          g_progress = 0.0f;

void app_request_exit(void) { g_running = false; }

/* ── Lifecycle ───────────────────────────────────────────────────── */
int app_init(void) {
    printf("[PPE] ══════════════════════════════════════════\n");
    printf("[PPE]  MONOCHROME · PS3 · Cell B.E. v%s\n", APP_VERSION);
    printf("[PPE] ══════════════════════════════════════════\n");

    /* Allocate the shared ring buffer FIRST — everything depends on it */
    g_ring.capacity = STREAM_RING_BUFFER_SIZE;
    g_ring.buffer   = (uint8_t *)malloc(g_ring.capacity);
    if (!g_ring.buffer) {
        printf("[PPE] FATAL: cannot allocate %zu bytes for ring buffer\n",
               g_ring.capacity);
        return -1;
    }
    g_ring.write_pos  = 0;
    g_ring.read_pos   = 0;
    g_ring.stream_eof = false;
    printf("[PPE] Ring buffer: %zu MB @ %p\n",
           g_ring.capacity / (1024*1024), (void *)g_ring.buffer);

    /* Network (sys_net + mbedTLS) */
    if (net_init() != 0) {
        printf("[PPE] FATAL: net_init failed\n");
        return -1;
    }

    /* Audio (sys_audio_out + SPU task queue) */
    if (audio_init(&g_ring) != 0) {
        printf("[PPE] FATAL: audio_init failed\n");
        return -1;
    }

    /* RSX graphics context */
    if (gfx_init() != 0) {
        printf("[PPE] FATAL: gfx_init failed\n");
        return -1;
    }

    g_state = STATE_HOME;
    return 0;
}

void app_shutdown(void) {
    printf("[PPE] Shutting down subsystems…\n");
    net_stop_stream();
    audio_shutdown();
    net_shutdown();
    gfx_shutdown();

    if (g_ring.buffer) {
        free(g_ring.buffer);
        g_ring.buffer = NULL;
    }
    printf("[PPE] Clean exit.\n");
}

/* ── Main loop ───────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    if (app_init() != 0) {
        return 1;
    }

    while (g_running) {
        /* 1 ── Read DualShock 3 and advance state machine */
        g_state = pad_update(g_state);

        /* 2 ── Pump decoded audio to sys_audio_out */
        audio_tick();

        /* 3 ── Render frame and VSync-flip */
        gfx_render(g_state, &g_now, audio_get_state(), g_progress);

        /* 4 ── 60 FPS VSync pacing (yield PPU core to avoid 100% CPU lock) */
        usleep(16666);
    }

    app_shutdown();
    return 0;
}
