/**
 * Monochrome Music Player — PlayStation 3 Platform Implementation
 * Target: Cell B.E. (PPU) / GameOS / RPCS3 emulator
 *
 * Strategy: Zero PSL1GHT dependency — all syscalls via inline LV2 syscall
 * wrappers defined in lv2.c.  RSX framebuffer is a software-render in main
 * memory that is DMA-copied to RSX local memory every frame via GCM.
 *
 * Compilation: zig cc -target powerpc64-linux-musl
 */

#if defined(__powerpc__) || defined(__CELLOS_LV2__) || defined(TARGET_PS3)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "platform.h"
#include "monochrome.h"   /* TrackMetadata, AppState, etc. */

/* ─────────────────────────────────────────────────────────────────────────
 * lv2.c exports (implemented in lv2.c; no separate header needed here)
 * ─────────────────────────────────────────────────────────────────────── */
extern int  ps3_sys_tty_write(int ch, const void *buf, uint32_t len, uint32_t *pwdata);
extern int  ps3_sys_timer_usleep(uint64_t usec);
extern void ps3_sys_process_exit(int status);

/* Convenience wrappers */
static void tty(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        uint32_t w = 0;
        ps3_sys_tty_write(0, buf, (uint32_t)n, &w);
    }
}

static void ps3_sleep_ms(uint32_t ms) {
    ps3_sys_timer_usleep((uint64_t)ms * 1000ULL);
}

static uint64_t ps3_time_ms(void) {
    /* PowerPC Timebase (~79.8 MHz) */
    uint64_t tb;
    asm volatile("mfspr %0, 268" : "=r"(tb));
    return tb / 79800ULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 1 — LIFECYCLE & TIMERS
 * ═══════════════════════════════════════════════════════════════════════════ */

int platform_init(void) {
    tty("[PS3] Hardware Abstraction Layer initialized for Cell B.E.\n");
    return 0;
}

void platform_shutdown(void) {
    tty("[PS3] Hardware Abstraction Layer shutdown.\n");
}

void platform_sleep_ms(uint32_t ms) {
    ps3_sleep_ms(ms);
}

uint64_t platform_get_time_ms(void) {
    return ps3_time_ms();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 2 — RSX FRAMEBUFFER GRAPHICS
 *
 *  A 1280×720 ARGB8 software framebuffer in main XDR memory is rendered
 *  each frame and copied to RSX local memory via gcmMapMainMemory +
 *  gcmSetDisplayBuffer + gcmSetFlip.
 *
 *  All PSL1GHT / GCM functions are declared __attribute__((weak)) so the
 *  ELF links cleanly with zig cc musl.  Under RPCS3 HLE these resolve to
 *  the emulator's own implementations.  Every call is guarded with a NULL
 *  check so a missing HLE stub causes silent degradation, not a crash.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define FB_WIDTH    1280
#define FB_HEIGHT    720
#define FB_PITCH    (FB_WIDTH * 4)
#define FB_SIZE     (FB_PITCH * FB_HEIGHT)
#define NUM_BUFS    2

/* Minimal GCM context type — enough for the function pointers we need */
typedef struct { uint32_t opaque[4]; } gcmContextData;

/* Weak declarations — resolve to NULL if PSL1GHT is absent */
__attribute__((weak)) int  gcmInit(uint32_t cmdSize, uint32_t ioSize);
__attribute__((weak)) int  gcmSetFlipMode(uint32_t mode);
__attribute__((weak)) void gcmResetFlipStatus(void);
__attribute__((weak)) int  gcmGetFlipStatus(void);
__attribute__((weak)) int  gcmSetFlip(gcmContextData *ctx, uint32_t id);
__attribute__((weak)) int  gcmFlush(gcmContextData *ctx);
__attribute__((weak)) gcmContextData *gcmGetCurrentBuffer(void);
__attribute__((weak)) int  gcmSetDisplayBuffer(uint32_t id, uint32_t offset,
                                                uint32_t pitch, uint32_t w, uint32_t h);
__attribute__((weak)) int  gcmMapMainMemory(void *addr, uint32_t size, uint32_t *offset);
__attribute__((weak)) void *rsxMemalign(uint32_t align, uint32_t size);
__attribute__((weak)) void  rsxFree(void *ptr);

#define GCM_FLIP_VSYNC 1

static gcmContextData *s_gcm        = NULL;
static uint32_t        s_cur_buf    = 0;
static int             s_gfx_ready  = 0;

/* ARGB8 software framebuffer — written in main memory, copied to RSX */
static uint32_t s_fb[FB_HEIGHT][FB_WIDTH];

typedef struct { uint32_t *ptr; uint32_t offset; } RsxBuf;
static RsxBuf s_rsx[NUM_BUFS];

/* ── 8×8 Bitmap Font (ASCII 0x20–0x7E) ─────────────────────────────────── */
static const uint8_t FONT8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 20 SPC */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* 21 !   */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* 22 "   */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* 23 #   */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* 24 $   */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* 25 %   */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* 26 &   */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* 27 '   */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* 28 (   */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* 29 )   */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 2A *   */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* 2B +   */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* 2C ,   */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* 2D -   */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* 2E .   */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* 2F /   */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 30 0   */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 31 1   */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 32 2   */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 33 3   */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 34 4   */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 35 5   */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 36 6   */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 37 7   */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 38 8   */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 39 9   */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* 3A :   */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* 3B ;   */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* 3C <   */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* 3D =   */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* 3E >   */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* 3F ?   */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* 40 @   */
    /* A-Z (41-5A) */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00},
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00},
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00},
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00},
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00},
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00},
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00},
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00},
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},
    /* [ \ ] ^ _ ` (5B-60) */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},
    /* a-z (61-7A) */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00},
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00},
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00},
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00},
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00},
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00},
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},
    /* { | } ~ (7B-7E) */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},
};

/* ARGB colour constants */
#define COL_BG      0xFF0A0A0Au
#define COL_FG      0xFFE8E8E8u
#define COL_ACCENT  0xFF4FC3F7u
#define COL_DIM     0xFF606060u
#define COL_SELECT  0xFF1A3A5Au
#define COL_BAR_BG  0xFF1C1C1Cu
#define COL_BAR_FG  0xFF29B6F6u
#define COL_GREEN   0xFF6EC472u
#define COL_HEAD    0xFF121212u

/* ── Pixel / rect / text helpers ───────────────────────────────────────── */
static inline void fb_px(int x, int y, uint32_t c) {
    if ((unsigned)x < FB_WIDTH && (unsigned)y < FB_HEIGHT)
        s_fb[y][x] = c;
}

static void fb_rect(int x, int y, int w, int h, uint32_t c) {
    for (int r = y; r < y + h; r++)
        for (int cx = x; cx < x + w; cx++)
            fb_px(cx, r, c);
}

static void fb_glyph(int x, int y, unsigned char ch, uint32_t fg, int sc) {
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8_t *g = FONT8[ch - 0x20];
    for (int r = 0; r < 8; r++) {
        uint8_t bits = g[r];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80u >> col)) {
                for (int sy = 0; sy < sc; sy++)
                    for (int sx = 0; sx < sc; sx++)
                        fb_px(x + col*sc + sx, y + r*sc + sy, fg);
            }
        }
    }
}

static int fb_str(int x, int y, const char *s, uint32_t fg, int sc) {
    int cx = x;
    while (*s) { fb_glyph(cx, y, (unsigned char)*s++, fg, sc); cx += 8*sc; }
    return cx - x;
}

static void fb_str_r(int rx, int y, const char *s, uint32_t fg, int sc) {
    int len = (int)strlen(s) * 8 * sc;
    fb_str(rx - len, y, s, fg, sc);
}

static void trunc(char *dst, size_t dsz, const char *src, int maxch) {
    int n = (int)strlen(src);
    if (n <= maxch) { strncpy(dst, src, dsz-1); dst[dsz-1]='\0'; return; }
    int k = maxch - 3; if (k < 0) k = 0;
    strncpy(dst, src, (size_t)k);
    dst[k]='.'; dst[k+1]='.'; dst[k+2]='.'; dst[k+3]='\0';
}

/* ── RSX flip ───────────────────────────────────────────────────────────── */
static void rsx_flip(void) {
    if (!s_gfx_ready) return;

    /* Wait for previous flip */
    if (gcmGetFlipStatus) {
        int tries = 200;
        while (tries-- > 0 && gcmGetFlipStatus() != 0)
            ps3_sleep_ms(1);
    }
    if (gcmResetFlipStatus) gcmResetFlipStatus();

    /* Copy software FB to RSX memory */
    if (s_rsx[s_cur_buf].ptr)
        memcpy(s_rsx[s_cur_buf].ptr, s_fb, FB_SIZE);

    /* Issue GCM flip */
    if (gcmSetFlip && s_gcm) gcmSetFlip(s_gcm, s_cur_buf);
    if (gcmFlush   && s_gcm) gcmFlush(s_gcm);

    s_cur_buf ^= 1u;
}

/* ── GFX init ────────────────────────────────────────────────────────────  */
int platform_gfx_init(int width, int height, const char *title) {
    (void)width; (void)height; (void)title;
    tty("[RSX] Initialising %dx%d double-buffered framebuffer\n", FB_WIDTH, FB_HEIGHT);

    if (!gcmInit) {
        tty("[RSX] gcmInit not available (HLE?) — running TTY-only\n");
        return 0;
    }
    if (gcmInit(256*1024, 1024*1024) != 0) {
        tty("[RSX] gcmInit failed — TTY-only mode\n");
        return 0;
    }
    if (gcmSetFlipMode) gcmSetFlipMode(GCM_FLIP_VSYNC);

    for (int i = 0; i < NUM_BUFS; i++) {
        /* Prefer RSX local memory; fall back to main memory */
        if (rsxMemalign)
            s_rsx[i].ptr = (uint32_t *)rsxMemalign(64, FB_SIZE);
        else
            s_rsx[i].ptr = (uint32_t *)malloc(FB_SIZE);

        if (!s_rsx[i].ptr) { tty("[RSX] buf%d alloc failed\n", i); return 0; }
        memset(s_rsx[i].ptr, 0, FB_SIZE);

        if (gcmMapMainMemory)
            gcmMapMainMemory(s_rsx[i].ptr, FB_SIZE, &s_rsx[i].offset);
        else
            s_rsx[i].offset = (uint32_t)((uintptr_t)s_rsx[i].ptr & 0xFFFFFFFFu);

        if (gcmSetDisplayBuffer)
            gcmSetDisplayBuffer((uint32_t)i, s_rsx[i].offset, FB_PITCH, FB_WIDTH, FB_HEIGHT);
    }

    s_gcm       = gcmGetCurrentBuffer ? gcmGetCurrentBuffer() : NULL;
    s_cur_buf   = 0;
    s_gfx_ready = 1;
    tty("[RSX] Framebuffer ready (%d KB per buffer)\n", (int)(FB_SIZE/1024));
    return 0;
}

void platform_gfx_shutdown(void) {
    s_gfx_ready = 0;
    for (int i = 0; i < NUM_BUFS; i++) {
        if (s_rsx[i].ptr) {
            if (rsxFree) rsxFree(s_rsx[i].ptr); else free(s_rsx[i].ptr);
            s_rsx[i].ptr = NULL;
        }
    }
    tty("[RSX] Framebuffer released.\n");
}

/* ── Render frame ────────────────────────────────────────────────────────  */
/*
 * TrackMetadata fields (from monochrome.h):
 *   id[64], title[128], artist[128], album[128], cover_url[256],
 *   stream_url[512], duration_sec, sample_rate, bit_depth, is_hi_res
 */
void platform_gfx_render(int state, const struct TrackMetadata *track,
                          int pstate, float progress,
                          const struct TrackMetadata *tracks, int track_count,
                          int cursor, const char *search_query) {
    (void)state;

    /* ── Clear ─────────────────────────────────────────────────────────── */
    for (int y = 0; y < FB_HEIGHT; y++)
        for (int x = 0; x < FB_WIDTH; x++)
            s_fb[y][x] = COL_BG;

    /* ── Header (0–39) ──────────────────────────────────────────────────  */
    fb_rect(0, 0, FB_WIDTH, 40, COL_HEAD);
    fb_str(20, 12, "MONOCHROME", COL_ACCENT, 2);
    fb_str(192, 18, "Music Client  v01.00", COL_DIM, 1);
    fb_str_r(FB_WIDTH - 20, 14,
             "[X]Play  [O]Back  [/\\]Pause  [SQ]Search  [Start]Quit",
             COL_DIM, 1);

    /* ── Now-playing strip (41–109) ─────────────────────────────────────  */
    fb_rect(0, 40, FB_WIDTH, 1, COL_DIM);
    fb_rect(0, 41, FB_WIDTH, 68, 0xFF0D1B2Au);

    if (track && track->title[0]) {
        const char *icon = (pstate == (int)PLAYBACK_PLAYING) ? "> " : "|| ";
        char ttl[64]; trunc(ttl, sizeof(ttl), track->title, 38);
        fb_str(20, 52, icon, COL_GREEN, 2);
        fb_str(20 + (int)strlen(icon)*16, 52, ttl, COL_FG, 2);

        char meta[128];
        snprintf(meta, sizeof(meta), "%s  |  %s  |  %u Hz  %u-bit",
                 track->artist[0] ? track->artist : "Unknown",
                 track->album[0]  ? track->album  : "Unknown",
                 track->sample_rate ? track->sample_rate : 48000u,
                 track->bit_depth   ? (unsigned)track->bit_depth : 16u);
        fb_str(20, 78, meta, COL_DIM, 1);
    } else {
        fb_str(20, 58, "-- No track selected --", COL_DIM, 2);
    }

    /* ── Progress bar (98–108) ───────────────────────────────────────────  */
    int bx = 20, by = 98, bw = FB_WIDTH - 40, bh = 6;
    fb_rect(bx, by, bw, bh, COL_BAR_BG);
    if (progress > 0.0f) {
        int fill = (int)(bw * (progress > 1.0f ? 1.0f : progress));
        if (fill > 0) fb_rect(bx, by, fill, bh, COL_BAR_FG);
    }

    /* ── Search line ──────────────────────────────────────────────────── */
    fb_rect(0, 109, FB_WIDTH, 1, COL_DIM);
    if (search_query && search_query[0]) {
        char sq[80]; snprintf(sq, sizeof(sq), "Filter: %s", search_query);
        fb_str(20, 114, sq, COL_ACCENT, 1);
    }

    /* ── Track list ───────────────────────────────────────────────────── */
    int list_top = 128;
    int item_h   = 36;
    int visible  = (FB_HEIGHT - list_top - 20) / item_h;
    if (visible < 1) visible = 1;

    int scroll = 0;
    if (cursor >= visible) scroll = cursor - visible + 1;

    for (int i = 0; i < visible && (scroll + i) < track_count; i++) {
        int idx = scroll + i;
        int ty  = list_top + i * item_h;
        const TrackMetadata *t = &tracks[idx];

        fb_rect(0, ty, FB_WIDTH, item_h - 1, (idx == cursor) ? COL_SELECT : COL_BG);

        /* Row number */
        char num[6]; snprintf(num, sizeof(num), "%2d.", idx+1);
        fb_str(20, ty+4, num, (idx==cursor) ? COL_ACCENT : COL_DIM, 1);

        /* Title */
        char ttl[56]; trunc(ttl, sizeof(ttl), t->title, 52);
        fb_str(56, ty+4, ttl, (idx==cursor) ? COL_FG : 0xFFCCCCCCu, 1);

        /* Artist */
        char art[48]; trunc(art, sizeof(art), t->artist, 44);
        fb_str(56, ty+18, art, COL_DIM, 1);

        /* Duration */
        char dur[12];
        uint32_t d = t->duration_sec;
        snprintf(dur, sizeof(dur), "%u:%02u", d/60u, d%60u);
        fb_str_r(FB_WIDTH-20, ty+4, dur, COL_DIM, 1);

        /* Quality badge */
        const char *qlabel = t->is_hi_res ? "HI-RES" : "LOSSLESS";
        uint32_t qcol      = t->is_hi_res ? COL_ACCENT : COL_GREEN;
        fb_str_r(FB_WIDTH-80, ty+4, qlabel, qcol, 1);

        /* Separator */
        fb_rect(0, ty+item_h-1, FB_WIDTH, 1, 0xFF1A1A1Au);
    }

    /* ── Scrollbar ───────────────────────────────────────────────────── */
    if (track_count > visible) {
        int sbh = FB_HEIGHT - list_top - 20;
        fb_rect(FB_WIDTH-6, list_top, 4, sbh, COL_BAR_BG);
        int th = sbh * visible / track_count; if (th<8) th=8;
        int ty2 = list_top + scroll * (sbh - th) / (track_count - visible);
        fb_rect(FB_WIDTH-6, ty2, 4, th, COL_ACCENT);
    }

    /* ── Status bar ──────────────────────────────────────────────────── */
    fb_rect(0, FB_HEIGHT-16, FB_WIDTH, 16, 0xFF080808u);
    char st[80]; snprintf(st, sizeof(st), "%d tracks  |  MONOCHROME PS3 Homebrew", track_count);
    fb_str(20, FB_HEIGHT-12, st, COL_DIM, 1);

    /* ── Flush to RSX ─────────────────────────────────────────────────── */
    rsx_flip();

    static uint32_t s_frames = 0;
    s_frames++;
    if (s_frames % 60 == 0)
        tty("[RSX] Frame %u | tracks=%d cursor=%d\n", s_frames, track_count, cursor);
}

/* ── Console stubs ───────────────────────────────────────────────────── */
void platform_clear_screen(void) { /* visual — handled by render */ }
void platform_set_cursor(int r, int c) { (void)r; (void)c; }
int  platform_get_mouse_track_selection(void) { return -1; }

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 3 — AUDIO OUTPUT  (audioPortOpen / audioPortStart / audioPortWrite)
 *
 *  RPCS3 HLE provides these functions. We declare them weak and guard every
 *  call, so silence is the fallback when they are absent.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Minimal GameOS audio structures */
typedef struct {
    uint32_t type;      /* 0 = AUDIO_PORT_OUT */
    uint32_t channel;   /* 2 or 8 */
    uint32_t latency;   /* number of DMA blocks */
    uint32_t attr;      /* 0x10 = AUDIO_PORT_PERSONAL */
    uint32_t level;
} AudioPortParam;

typedef struct {
    uint32_t portNum;
    uint64_t portSize;
    uint32_t portType;
    uint32_t channelCount;
    uint64_t startTime;
    uint64_t flags;
    volatile uint64_t *readIndexAddr;
    float    *audioDataStart;
} AudioPortConfig;

__attribute__((weak)) int audioInit(void);
__attribute__((weak)) int audioPortOpen(const AudioPortParam *p, uint32_t *num);
__attribute__((weak)) int audioPortStart(uint32_t num);
__attribute__((weak)) int audioPortStop(uint32_t num);
__attribute__((weak)) int audioPortClose(uint32_t num);
__attribute__((weak)) int audioGetPortConfig(uint32_t num, AudioPortConfig *cfg);
__attribute__((weak)) int audioPortSetVolume(uint32_t num, float vol);

#define AUDIO_BLOCK_SAMPLES 256u
#define AUDIO_PORT_NBLOCKS   8u

static uint32_t   s_aport  = 0;
static int        s_aready = 0;
static float     *s_adma   = NULL;
static volatile uint64_t *s_aridx = NULL;
static uint64_t   s_awrite = 0;
static uint32_t   s_anblk  = AUDIO_PORT_NBLOCKS;
static uint32_t   s_ach    = 2;

int platform_audio_init(uint32_t sample_rate, uint8_t channels) {
    (void)sample_rate;
    s_ach = (channels >= 2) ? 2u : 2u;
    tty("[PS3] sys_audio_out initialising: 48000 Hz, %u ch\n", s_ach);

    if (!audioInit || audioInit() != 0) {
        tty("[PS3] audioInit unavailable — silent mode\n");
        return 0;
    }
    AudioPortParam p = {0, s_ach, s_anblk, 0x10,
                        AUDIO_BLOCK_SAMPLES * s_ach * 4u};
    uint32_t port = 0;
    if (!audioPortOpen || audioPortOpen(&p, &port) != 0) {
        tty("[PS3] audioPortOpen failed\n");
        return 0;
    }
    s_aport = port;

    if (audioGetPortConfig) {
        AudioPortConfig cfg = {0};
        if (audioGetPortConfig(s_aport, &cfg) == 0) {
            s_adma  = cfg.audioDataStart;
            s_aridx = cfg.readIndexAddr;
            if (cfg.portSize > 0)
                s_anblk = (uint32_t)(cfg.portSize /
                           (AUDIO_BLOCK_SAMPLES * s_ach * sizeof(float)));
        }
    }
    if (audioPortStart) audioPortStart(s_aport);
    s_aready = 1;
    tty("[PS3] audioPortStart OK — port %u, %u blocks\n", s_aport, s_anblk);
    return 0;
}

void platform_audio_shutdown(void) {
    if (s_aready) {
        if (audioPortStop)  audioPortStop(s_aport);
        if (audioPortClose) audioPortClose(s_aport);
        s_aready = 0;
    }
    tty("[PS3] sys_audio_out released.\n");
}

void platform_audio_write(const int16_t *samples, size_t frames) {
    if (!s_aready || !s_adma || !frames) return;

    size_t done = 0;
    while (done < frames) {
        /* Wait if ring buffer is full */
        uint64_t ridx = s_aridx ? *s_aridx : s_awrite;
        int spin = 100;
        while (spin-- > 0 && (s_awrite - ridx) >= (uint64_t)s_anblk) {
            ps3_sleep_ms(1);
            ridx = s_aridx ? *s_aridx : s_awrite;
        }

        uint32_t bpos = (uint32_t)(s_awrite % s_anblk);
        float   *dst  = s_adma + bpos * AUDIO_BLOCK_SAMPLES * s_ach;

        size_t chunk = AUDIO_BLOCK_SAMPLES;
        if (chunk > frames - done) chunk = frames - done;

        const int16_t *src = samples + done * s_ach;
        for (size_t i = 0; i < chunk * s_ach; i++)
            dst[i] = (float)src[i] / 32768.0f;
        for (size_t i = chunk * s_ach; i < AUDIO_BLOCK_SAMPLES * s_ach; i++)
            dst[i] = 0.0f;

        s_awrite++;
        done += chunk;
    }
}

void platform_audio_set_volume(float vol) {
    if (s_aready && audioPortSetVolume) audioPortSetVolume(s_aport, vol);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 4 — INPUT  (DualShock 3 via ioPadGetData)
 *
 *  button[2] bits:  0x08=Up  0x04=Right  0x02=Down  0x01=Left  0x10=Start
 *  button[3] bits:  0x08=Triangle  0x04=Circle  0x02=Cross  0x01=Square
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t len;
    uint16_t button[8];
    uint8_t  analog_rx, analog_ry, analog_lx, analog_ly;
    uint8_t  button_pressure[12];
    uint8_t  pad[4];
} ioPadData;

__attribute__((weak)) int ioPadInit(uint32_t max_connect);
__attribute__((weak)) int ioPadEnd(void);
__attribute__((weak)) int ioPadGetData(uint32_t port, ioPadData *data);

static int     s_pad_ready = 0;
static uint16_t s_prev2 = 0, s_prev3 = 0;

InputAction platform_poll_input(void) {
    /* Lazy init */
    if (!s_pad_ready && ioPadInit) {
        if (ioPadInit(7) == 0) { s_pad_ready = 1; tty("[PS3] ioPadInit OK\n"); }
    }

    if (!s_pad_ready || !ioPadGetData) {
        /* Fallback auto-demo for headless test */
        static uint32_t tk = 0;
        tk++;
        if (tk ==  90) return INPUT_DOWN;
        if (tk == 180) return INPUT_DOWN;
        if (tk == 270) return INPUT_DOWN;
        if (tk == 360) return INPUT_SELECT;
        if (tk == 600) return INPUT_PLAY_PAUSE;
        return INPUT_NONE;
    }

    ioPadData pd; memset(&pd, 0, sizeof(pd));
    if (ioPadGetData(0, &pd) != 0 || pd.len == 0) return INPUT_NONE;

    uint16_t b2 = pd.button[2], b3 = pd.button[3];
    uint16_t n2 = b2 & ~s_prev2,  n3 = b3 & ~s_prev3;
    s_prev2 = b2; s_prev3 = b3;

    if (n2 & 0x08) return INPUT_UP;
    if (n2 & 0x02) return INPUT_DOWN;
    if (n2 & 0x01) return INPUT_LEFT;
    if (n2 & 0x04) return INPUT_RIGHT;
    if (n3 & 0x02) return INPUT_SELECT;       /* Cross     */
    if (n3 & 0x04) return INPUT_CANCEL;       /* Circle    */
    if (n3 & 0x01) return INPUT_ACTION_SEARCH;/* Square    */
    if (n3 & 0x08) return INPUT_PLAY_PAUSE;   /* Triangle  */
    if (n2 & 0x10) return INPUT_QUIT;         /* Start     */
    return INPUT_NONE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 5 — NETWORK (pre-cached fallback catalogue)
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char k_catalog[] =
    "{\"tracks\":["
    "{\"trackId\":\"155102881689636864\",\"title\":\"Harder, Better, Faster, Stronger\","
     "\"artistNames\":[\"Daft Punk\"],\"albumTitle\":\"Discovery\","
     "\"duration\":224,\"audioQuality\":\"LOSSLESS\"},"
    "{\"trackId\":\"155102871061270528\",\"title\":\"One More Time\","
     "\"artistNames\":[\"Daft Punk\"],\"albumTitle\":\"Discovery\","
     "\"duration\":320,\"audioQuality\":\"LOSSLESS\"},"
    "{\"trackId\":\"155142491052445696\",\"title\":\"Instant Crush\","
     "\"artistNames\":[\"Daft Punk\",\"Julian Casablancas\"],"
     "\"albumTitle\":\"Random Access Memories\","
     "\"duration\":337,\"audioQuality\":\"HI_RES\"},"
    "{\"trackId\":\"155142502393843712\",\"title\":\"Get Lucky\","
     "\"artistNames\":[\"Daft Punk\",\"Pharrell Williams\"],"
     "\"albumTitle\":\"Random Access Memories\","
     "\"duration\":248,\"audioQuality\":\"HI_RES\"}"
    "]}";

int platform_http_get(const char *url, char **resp, size_t *sz) {
    tty("[NET] GET %s (PS3 fallback catalogue)\n", url ? url : "(null)");
    size_t n = strlen(k_catalog);
    char  *b = (char *)malloc(n + 1);
    if (!b) return -1;
    memcpy(b, k_catalog, n + 1);
    *resp = b; *sz = n;
    return 0;
}

void platform_http_free(char *r) { if (r) free(r); }

#endif /* TARGET_PS3 */
