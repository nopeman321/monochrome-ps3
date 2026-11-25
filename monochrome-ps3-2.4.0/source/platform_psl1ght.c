/*
 * Monochrome PS3 - Platform Implementation for PSL1GHT (Phase 2)
 * Capa de plataforma reducida: input (ioPad), sysUtil, tiempo, logging y
 * configuración de pantalla. El vídeo se delega a gfx_tiny3d (Tiny3D/RSX).
 */

#include <ppu-lv2.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

#include <sysutil/sysutil.h>
#include <sysmodule/sysmodule.h>
#include <io/pad.h>

#include "platform.h"
#include "gfx_engine.h"
#include "net_hal.h"

#define SCREEN_CONFIG_PATH "/dev_hdd0/tmp/monochrome/screen_config.txt"

/* GameOS System Event Handling & XMB Overlay State */
static volatile bool s_platform_exit_requested = false;
static volatile bool s_platform_overlay_open = false;

static void sysutil_event_callback(u64 status, u64 param, void *usrdata)
{
    (void)param;
    (void)usrdata;
    switch (status) {
        case SYSUTIL_EXIT_GAME:
            platform_log("[SYSUTIL] SYSUTIL_EXIT_GAME recibido (0x%llx) -> Salida al XMB solicitada por VSH", (unsigned long long)status);
            s_platform_exit_requested = true;
            break;
        case SYSUTIL_MENU_OPEN:
            platform_log("[SYSUTIL] SYSUTIL_MENU_OPEN recibido -> Overlay XMB abierto");
            s_platform_overlay_open = true;
            net_hal_set_menu_paused(true);
            break;
        case SYSUTIL_MENU_CLOSE:
            platform_log("[SYSUTIL] SYSUTIL_MENU_CLOSE recibido -> Overlay XMB cerrado");
            s_platform_overlay_open = false;
            net_hal_set_menu_paused(false);
            break;
        case SYSUTIL_DRAW_BEGIN:
        case SYSUTIL_DRAW_END:
            /* VSH renderiza el overlay sobre la pantalla */
            break;
        default:
            platform_log("[SYSUTIL] Evento recibido: status=0x%llx, param=0x%llx", (unsigned long long)status, (unsigned long long)param);
            break;
    }
}

bool platform_should_exit(void)
{
    return s_platform_exit_requested;
}

void platform_set_exit_requested(bool req)
{
    s_platform_exit_requested = req;
}

bool platform_is_overlay_open(void)
{
    return s_platform_overlay_open;
}

/* ── Logging ───────────────────────────────────────────────────── */

void platform_log(const char *fmt, ...)
{
    if (!fmt) return;
    char buf[896];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    net_hal_log("PLAT", "%s", buf);
}

uint64_t platform_get_time_usec(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* ── Inicialización / apagado (delega vídeo a gfx_tiny3d) ──────── */

int platform_init(void)
{
    platform_log("=== MONOCHROME MUSIC STREAMER: INICIO (Phase 2 Tiny3D) ===");

    int gret = gfx_init();
    if (gret != 0) {
        platform_log("[ERROR] gfx_init fallo");
        return -1;
    }
    platform_log("[GFX] gfx_init OK (Tiny3D backend)");

    int pret = ioPadInit(7);
    platform_log("[PAD] ioPadInit(7) retorno %d", pret);

    int sret = sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_event_callback, NULL);
    platform_log("[SYSUTIL] sysUtilRegisterCallback(SLOT0) retorno %d", sret);

    /* Load saved screen scale/offset before first render */
    platform_load_screen_config();
    return 0;
}

void platform_shutdown(void)
{
    platform_log("[CLEANUP] Cerrando subsistemas de plataforma...");

    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
    platform_log("[SYSUTIL] sysUtilUnregisterCallback OK");

    ioPadEnd();
    platform_log("[CLEANUP] ioPadEnd OK");

    gfx_shutdown();
    platform_log("[GFX] gfx_shutdown OK");

    platform_log("[CLEANUP] Recursos listos para retorno inmediato al XMB");
}

/* ── Ciclo de frame (delega a gfx_tiny3d) ──────────────────────── */

void platform_prepare_frame(void)
{
    gfx_begin_frame(0x00000000);
}

void platform_flip(void)
{
    gfx_end_frame(); /* tiny3d_Flip() */
}

/* ── Input ─────────────────────────────────────────────────────── */

static uint16_t s_old_buttons = 0;

InputAction platform_poll_input(void)
{
    if (s_platform_overlay_open) {
        return ACT_NONE;
    }

    padInfo padinfo;
    padData paddata;

    ioPadGetInfo(&padinfo);
    for (int i = 0; i < MAX_PADS; i++) {
        if (padinfo.status[i]) {
            if (ioPadGetData(i, &paddata) == 0 && paddata.len > 0) {
                uint16_t cur = 0;
                if (paddata.BTN_LEFT)     cur |= (1 << 0);
                if (paddata.BTN_RIGHT)    cur |= (1 << 1);
                if (paddata.BTN_UP)       cur |= (1 << 2);
                if (paddata.BTN_DOWN)     cur |= (1 << 3);
                if (paddata.BTN_CROSS)    cur |= (1 << 4);
                if (paddata.BTN_CIRCLE)   cur |= (1 << 5);
                if (paddata.BTN_SQUARE)   cur |= (1 << 6);
                if (paddata.BTN_TRIANGLE) cur |= (1 << 7);
                if (paddata.BTN_START)    cur |= (1 << 8);
                if (paddata.BTN_SELECT)   cur |= (1 << 9);
                if (paddata.BTN_L1)       cur |= (1 << 10);
                if (paddata.BTN_R1)       cur |= (1 << 11);
                if (paddata.BTN_L2)       cur |= (1 << 12);
                if (paddata.BTN_R2)       cur |= (1 << 13);

                uint16_t pressed = cur & ~s_old_buttons;
                s_old_buttons = cur;

                if (pressed & (1 << 2))  return ACT_UP;
                if (pressed & (1 << 3))  return ACT_DOWN;
                if (pressed & (1 << 0))  return ACT_LEFT;
                if (pressed & (1 << 1))  return ACT_RIGHT;
                if (pressed & (1 << 4))  return ACT_CROSS;
                if (pressed & (1 << 5))  return ACT_CIRCLE;
                if (pressed & (1 << 6))  return ACT_SQUARE;
                if (pressed & (1 << 7))  return ACT_TRIANGLE;
                if (pressed & (1 << 10)) return ACT_L1;
                if (pressed & (1 << 11)) return ACT_R1;
                if (pressed & (1 << 12)) return ACT_L2;
                if (pressed & (1 << 13)) return ACT_R2;
                if (pressed & (1 << 9))  return ACT_SELECT;
                if (pressed & (1 << 8))  return ACT_START;

                return ACT_NONE;
            }
        }
    }
    s_old_buttons = 0;
    return ACT_NONE;
}

/* ── Resolución y pantalla ─────────────────────────────────────── */

int platform_get_width(void)  { return gfx_get_width(); }
int platform_get_height(void) { return gfx_get_height(); }

/* Screen adjustment for overscan compensation */
static int s_screen_offset_x = 0;
static int s_screen_offset_y = 0;
static float s_screen_scale = 1.0f;
static float s_font_scale = 1.0f;

void platform_set_screen_adjust(int offset_x, int offset_y, float scale)
{
    s_screen_offset_x = offset_x;
    s_screen_offset_y = offset_y;
    s_screen_scale = scale;
    gfx_set_overscan(offset_x, offset_y, scale);
    platform_log("[SCREEN] Ajuste: offset=(%d, %d), scale=%.2f", offset_x, offset_y, scale);
}

void platform_get_screen_adjust(int *offset_x, int *offset_y, float *scale)
{
    if (offset_x) *offset_x = s_screen_offset_x;
    if (offset_y) *offset_y = s_screen_offset_y;
    if (scale) *scale = s_screen_scale;
}

float platform_get_font_scale(void) { return s_font_scale; }
void platform_set_font_scale(float scale) { s_font_scale = scale; }

int platform_save_screen_config(void)
{
    FILE *f = fopen(SCREEN_CONFIG_PATH, "w");
    if (!f) {
        platform_log("[SCREEN ERROR] No se pudo guardar config en %s", SCREEN_CONFIG_PATH);
        return -1;
    }
    fprintf(f, "%d\n%d\n%.4f\n%.4f\n", s_screen_offset_x, s_screen_offset_y, s_screen_scale, s_font_scale);
    fclose(f);
    platform_log("[SCREEN] Config guardada: offset=(%d, %d), scale=%.4f",
                 s_screen_offset_x, s_screen_offset_y, s_screen_scale);
    return 0;
}

int platform_load_screen_config(void)
{
    mkdir("/dev_hdd0/tmp", 0777);
    mkdir("/dev_hdd0/tmp/monochrome", 0777);
    mkdir("/dev_hdd0/tmp/monochrome/cache", 0777);
    mkdir("/dev_hdd0/tmp/monochrome/tracks", 0777);

    FILE *f = fopen(SCREEN_CONFIG_PATH, "r");
    if (!f) {
        platform_log("[SCREEN] No hay config guardada, creando valores por defecto");
        platform_save_screen_config();
        return 0;
    }
    int ox, oy;
    float sc, fs = 1.0f;
    if (fscanf(f, "%d\n%d\n%f\n%f\n", &ox, &oy, &sc, &fs) == 4) {
        s_screen_offset_x = ox;
        s_screen_offset_y = oy;
        s_screen_scale = sc;
        s_font_scale = fs;
        gfx_set_overscan(ox, oy, sc);
        platform_log("[SCREEN] Config cargada: offset=(%d, %d), scale=%.4f, font=%.4f",
                     ox, oy, sc, fs);
        fclose(f);
        return 0;
    }
    if (fscanf(f, "%d\n%d\n%f\n", &ox, &oy, &sc) == 3) {
        s_screen_offset_x = ox;
        s_screen_offset_y = oy;
        s_screen_scale = sc;
        gfx_set_overscan(ox, oy, sc);
        platform_log("[SCREEN] Config cargada: offset=(%d, %d), scale=%.4f", ox, oy, sc);
        fclose(f);
        return 0;
    }
    fclose(f);
    platform_log("[SCREEN ERROR] Config invalida, usando valores por defecto");
    return -1;
}
