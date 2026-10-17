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
static uint8_t         s_ring_storage[STREAM_RING_BUFFER_SIZE] __attribute__((aligned(128)));
static float          g_progress = 0.0f;

void app_request_exit(void) { g_running = false; }
AudioRingBuffer *app_get_ring(void) { return &g_ring; }
TrackMetadata   *app_get_current_track(void) { return &g_now; }

/* ── Lifecycle ───────────────────────────────────────────────────── */
int app_init(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    platform_init();

    printf("[SYS] ══════════════════════════════════════════\n");
    printf("[SYS]  MONOCHROME · Music Client v%s\n", APP_VERSION);
    printf("[SYS]  Target Architecture: %s\n", PLATFORM_NAME);
    printf("[SYS] ══════════════════════════════════════════\n");

    /* Statically assigned ring buffer in memory — 100% reliable */
    g_ring.capacity = STREAM_RING_BUFFER_SIZE;
    g_ring.buffer   = s_ring_storage;
    g_ring.write_pos  = 0;
    g_ring.read_pos   = 0;
    g_ring.stream_eof = false;
    printf("[SYS] Ring buffer: %zu MB @ %p\n",
           g_ring.capacity / (1024*1024), (void *)g_ring.buffer);

    /* Network subsystem */
    if (net_init() != 0) {
        printf("[SYS] FATAL: net_init failed\n");
        return -1;
    }

    /* Audio subsystem */
    if (audio_init(&g_ring) != 0) {
        printf("[SYS] FATAL: audio_init failed\n");
        return -1;
    }

    /* Graphics / UI context */
    if (gfx_init() != 0) {
        printf("[SYS] FATAL: gfx_init failed\n");
        return -1;
    }

    g_state = STATE_HOME;
    return 0;
}

void app_shutdown(void) {
    printf("[SYS] Shutting down subsystems…\n");
    net_stop_stream();
    audio_shutdown();
    net_shutdown();
    gfx_shutdown();
    platform_shutdown();

    g_ring.buffer = NULL;
    printf("[SYS] Clean exit completed.\n");
}

/* ── Main loop ───────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    if (app_init() != 0) {
        return 1;
    }

    while (g_running) {
        /* 1 ── Read input (DualShock 3 on PS3 / Keyboard on PC) */
        g_state = pad_update(g_state);

        /* 2 ── Pump decoded audio to hardware DAC (waveOut / sys_audio_out) */
        audio_tick();

        /* 3 ── Render frame and VSync-flip */
        gfx_render(g_state, &g_now, audio_get_state(), g_progress);

        /* 4 ── 60 FPS pacing */
        platform_sleep_ms(16);
    }

    app_shutdown();
    return 0;
}
