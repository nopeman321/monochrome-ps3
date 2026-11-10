/*
 * Monochrome PS3 - Stage 6: Real API Search & Lossless Streaming
 * Pure PSL1GHT v2 standard implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/process.h>
#include <sysutil/sysutil.h>

#include "monochrome.h"

SYS_PROCESS_PARAM(1001, 0x100000);
#include "platform.h"
#include "ui.h"
#include "http_client.h"
#include "audio_engine.h"
#include "cover_art.h"

int main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;

    if (platform_init() != 0) {
        platform_log("[FATAL] platform_init fallo; abortando");
        return -1;
    }

    /* Load screen adjustment config if saved */
    if (platform_load_screen_config() == 0) {
        int ox, oy;
        float sc;
        platform_get_screen_adjust(&ox, &oy, &sc);
        platform_set_screen_adjust(ox, oy, sc);
    }

    cover_art_init();

    if (audio_engine_init() != 0) {
        platform_log("[WARN] audio_engine_init fallo; continuando");
    }

    ui_init();

    /* Minimalist ASCII splash screen */
    ui_render_splash();
    platform_log("[SPLASH] Splash screen displayed");

    platform_log("[MEM] Presupuesto de memoria reservado:");
    platform_log("  - Host IO Ring: 32 MB XDR (memalign 1MB)");
    platform_log("  - Display buffers: 2x ~8.29 MB (1080p XRGB) en RSX VRAM");
    platform_log("  - Application heap: ~4 MB estatico");

    AppState state = STATE_HOME;
    FILE *f_init = fopen("/dev_hdd0/game/MONO00024/USRDIR/initial_state.txt", "r");
    if (!f_init) f_init = fopen("/dev_hdd0/game/MCRM00001/USRDIR/initial_state.txt", "r");
    if (f_init) {
        char init_name[32] = {0};
        if (fscanf(f_init, "%31s", init_name) == 1) {
            if (strcmp(init_name, "SEARCH") == 0) state = STATE_SEARCH;
            else if (strcmp(init_name, "LIBRARY") == 0) state = STATE_LIBRARY;
            else if (strcmp(init_name, "RECENT") == 0) state = STATE_RECENT;
            else if (strcmp(init_name, "UNRELEASED") == 0) state = STATE_UNRELEASED;
            else if (strcmp(init_name, "DONATE") == 0) state = STATE_DONATE;
            else if (strcmp(init_name, "SETTINGS") == 0) state = STATE_SETTINGS;
            else if (strcmp(init_name, "ABOUT") == 0) state = STATE_ABOUT;
            else if (strcmp(init_name, "PLAYING") == 0) state = STATE_PLAYING;
        }
        fclose(f_init);
        unlink("/dev_hdd0/game/MONO00024/USRDIR/initial_state.txt");
        unlink("/dev_hdd0/game/MCRM00001/USRDIR/initial_state.txt");
    }
    platform_log("[NAV] Pantalla inicial: %s", ui_get_state_name(state));

    long frame = 0;
    uint64_t total_render_time_usec = 0;
    uint64_t sample_frames = 0;
    uint64_t sample_time_usec = 0;

    platform_log("[LOOP] Entrando en bucle de interfaz interactiva...");

    while (1) {
        uint64_t t0 = platform_get_time_usec();

        /* 0. Pump GameOS System Event Callback (PS Button / XMB overlay intercept) */
        sysUtilCheckCallback();
        if (platform_should_exit() || ui_should_exit()) {
            platform_log("[EXIT] Salida detectada (sysutil=%d, menu=%d) -> Retorno inmediato al XMB",
                         platform_should_exit(), ui_should_exit());
            break;
        }

        /* 1. Poll input actions */
        InputAction act = platform_poll_input();
        
        /* 2. Process UI State Machine */
        AppState next_state = ui_handle_input(state, act);
        if (next_state != state) {
            platform_log("[NAV] Transicion de pantalla: %s -> %s",
                         ui_get_state_name(state), ui_get_state_name(next_state));
            state = next_state;
        }
        
        /* Check if user wants to exit from menu */
        if (ui_should_exit()) {
            platform_log("[USER] Salida solicitada desde menu");
            break;
        }

        /* 3. Wait for hardware VSync flip, then render and flip */
        platform_prepare_frame();
        ui_render(state, frame);
        platform_flip();

        uint64_t t1 = platform_get_time_usec();
        uint64_t frame_dt = (t1 > t0) ? (t1 - t0) : 0;
        total_render_time_usec += frame_dt;
        sample_time_usec += frame_dt;
        sample_frames++;

        /* Periodic performance statistics (to detect stutter) */
        if (sample_frames >= 300) {
            float avg_ms = (float)sample_time_usec / (float)sample_frames / 1000.0f;
            float fps = (avg_ms > 0.0f) ? (1000.0f / avg_ms) : 60.0f;
            platform_log("[PERF] Frame %ld | Estado: %s | Tiempo medio por frame: %.2f ms (%.1f FPS)",
                         frame, ui_get_state_name(state), avg_ms, fps);
            sample_frames = 0;
            sample_time_usec = 0;
        }

        frame++;
    }

    float overall_avg_ms = (frame > 0) ? ((float)total_render_time_usec / (float)frame / 1000.0f) : 0.0f;
    platform_log("[PERF] Resumen total: %ld frames presentados, tiempo medio: %.2f ms",
                 frame, overall_avg_ms);

    audio_engine_shutdown();
    http_net_shutdown();
    cover_art_shutdown();
    platform_shutdown();
    platform_log("[EXIT] Saliendo limpiamente al XMB via sysProcessExit(0)...");
    sysProcessExit(0);
    return 0;
}
