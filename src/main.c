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

    /* Cover art caching subsystem */
    cover_art_init();

    g_state = STATE_HOME;
    return 0;
}

void app_shutdown(void) {
    printf("[SYS] Shutting down subsystems…\n");
    cover_art_shutdown();
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

    /* Performance tracking / loop timing */
    uint64_t last_perf_time = platform_get_time_ms();
    uint32_t loop_count = 0;
    uint64_t accumulated_ms = 0;

    while (g_running) {
        uint64_t loop_start = platform_get_time_ms();

        /* 1 ── Read input (DualShock 3 on PS3 / Keyboard on PC) */
        g_state = pad_update(g_state);

        /* 2 ── Pump decoded audio to hardware DAC (waveOut / sys_audio_out) */
        audio_tick();

        /* 3 ── Render frame and VSync-flip */
        g_progress = audio_get_progress(g_now.duration_sec);
        gfx_render(g_state, &g_now, audio_get_state(), g_progress);

        uint64_t loop_end = platform_get_time_ms();
        uint64_t elapsed = loop_end - loop_start;
        accumulated_ms += elapsed;
        loop_count++;

        /* Every 120 iterations, output [PERF] metrics for objective verification */
        if (loop_count >= 120) {
            uint64_t total_time = loop_end - last_perf_time;
            double loops_per_sec = (total_time > 0) ? ((double)loop_count * 1000.0 / (double)total_time) : 60.0;
            double avg_ms = (double)accumulated_ms / (double)loop_count;
            printf("[PERF] loops/s=%.1f avg_ms=%.2f\n", loops_per_sec, avg_ms);
            last_perf_time = loop_end;
            loop_count = 0;
            accumulated_ms = 0;
        }

        /* 4 ── Adaptive pacing: yield only if frame took less than 16ms */
        if (elapsed < 16) {
            platform_sleep_ms(16 - (uint32_t)elapsed);
        }
    }

    app_shutdown();
    return 0;
}
