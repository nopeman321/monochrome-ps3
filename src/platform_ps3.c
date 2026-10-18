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
#include <unistd.h>

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
 *  A 720×480 ARGB8 software framebuffer in main XDR memory is rendered
 *  each frame and copied into main memory mapped into IO space via
 *  cellGcmMapMainMemory + cellGcmSetDisplayBuffer + cellGcmSetFlip.
 *
 *  All cellGcmSys / cellPad / cellAudio calls go through SCE dynamic
 *  import slots (SCE_SLOT below): the loader binds the firmware/HLE
 *  functions at boot, and a failed binding degrades to TTY-only /
 *  silent / auto-demo mode instead of crashing.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define FB_WIDTH     720
#define FB_HEIGHT    480
#define FB_PITCH    (FB_WIDTH * 4)
#define FB_SIZE     (FB_PITCH * FB_HEIGHT)
#define NUM_BUFS    2

/* ── SCE dynamic import slots (cellGcmSys / cellAudio / cellPad) ─────────
 * tools/sce_imports.py appends import tables + a 0x60000002 program header
 * to the ELF.  On boot the loader resolves each NID against the firmware /
 * HLE module and writes the 32-bit OPD address into the LOW half of the
 * matching 64-bit slot below (big-endian: slot + 4).  Calls go through the
 * slot value as an ELFv1 function pointer.  volatile is essential: zig cc
 * links with full LTO + --gc-sections, which would otherwise internalize
 * the slots, fold them to the constant 1 and delete every guarded call.
 */
#define SCE_SLOT(sym) \
    volatile uint64_t sce_slot_##sym __attribute__((used, aligned(8))) = 1
#define SCE_BOUND(sym) (sce_slot_##sym > 0x1000u)

/* ELFv1 call trampoline (src/entry.s).  The slot holds the OPD address
 * bound by the loader: [func_word, rtoc_word].  Calling through the raw
 * func word (the old SCE_FN macro) enters firmware code with our own
 * .TOC. in r2, which makes rtoc-relative loads resolve to garbage — the
 * LLE cellGcmSys path died with "Access violation reading 0xa0".
 * sce_opd_call6 instead loads r2 = rtoc_word and r12 = OPD before the
 * target runs, and restores the caller's r2/LR afterwards.  Arguments
 * travel as 64-bit registers; targets read the low 32 bits of u32
 * parameters.  sce_opd_call6f is the same body for a trailing float
 * (passed in f1, untouched by the trampoline). */
extern uint64_t sce_opd_call6(uint64_t opd, uint64_t a0, uint64_t a1,
                              uint64_t a2, uint64_t a3, uint64_t a4,
                              uint64_t a5);
extern uint64_t sce_opd_call6f(uint64_t opd, uint64_t a0, float a1);

SCE_SLOT(_cellGcmInitBody);
SCE_SLOT(cellGcmSetFlipMode);
SCE_SLOT(cellGcmSetFlip);
SCE_SLOT(cellGcmSetDisplayBuffer);
SCE_SLOT(cellGcmMapMainMemory);
SCE_SLOT(cellGcmGetFlipStatus);
SCE_SLOT(cellGcmResetFlipStatus);
SCE_SLOT(cellGcmSetFlipHandler);
SCE_SLOT(cellGcmSetFlipImmediate);
SCE_SLOT(cellGcmGetLastFlipTime);

static inline int gcm_init_body(uint32_t *ctx, uint32_t cmd, uint32_t io,
                                uint32_t ea) {
    return (int)sce_opd_call6(sce_slot__cellGcmInitBody,
                              (uint64_t)(uintptr_t)ctx, cmd, io, ea, 0, 0);
}
static inline int gcm_set_flip_mode(uint32_t mode) {
    return (int)sce_opd_call6(sce_slot_cellGcmSetFlipMode,
                              mode, 0, 0, 0, 0, 0);
}
static inline int gcm_set_flip(uint32_t ctx, uint32_t id) {
    return (int)sce_opd_call6(sce_slot_cellGcmSetFlip,
                              ctx, id, 0, 0, 0, 0);
}
static inline int gcm_set_flip_immediate(uint32_t ctx, uint32_t id) {
    return (int)sce_opd_call6(sce_slot_cellGcmSetFlipImmediate,
                              ctx, id, 0, 0, 0, 0);
}
static inline int gcm_set_display_buf(uint32_t id, uint32_t offset,
                                      uint32_t r, uint32_t w, uint32_t h) {
    return (int)sce_opd_call6(sce_slot_cellGcmSetDisplayBuffer,
                              id, offset, r, w, h, 0);
}
static inline int gcm_map_main_memory(uint32_t offset, uint32_t size,
                                      uint32_t *ea) {
    return (int)sce_opd_call6(sce_slot_cellGcmMapMainMemory,
                              offset, size, (uint64_t)(uintptr_t)ea, 0, 0, 0);
}
static inline int gcm_get_flip_status(void) {
    return (int)sce_opd_call6(sce_slot_cellGcmGetFlipStatus,
                              0, 0, 0, 0, 0, 0);
}
static inline int gcm_reset_flip_status(void) {
    return (int)sce_opd_call6(sce_slot_cellGcmResetFlipStatus,
                              0, 0, 0, 0, 0, 0);
}

/* cellGcmSetFlipHandler(fn) is REQUIRED on the LLE path.  Firmware's
 * _gcm_intr_thread only updates flip status (driverInfo.head[].flipFlags
 * bit 0) when it receives the flip event, and sys_rsx's send_event()
 * filters that event by driverInfo.handlers -- which is only set when a
 * user flip handler is registered.  Without it, cellGcmGetFlipStatus()
 * stays at 1 forever (observed: every flip wait exhausted 200 ms).  The
 * handler runs in the gcm interrupt thread; keep it trivial. */
static volatile uint32_t s_flip_cb_count = 0;
static void flip_handler_cb(uint32_t head) {
    (void)head;
    s_flip_cb_count++;
}
static inline uint64_t gcm_set_flip_handler(uint64_t fn_opd) {
    return sce_opd_call6(sce_slot_cellGcmSetFlipHandler,
                         fn_opd, 0, 0, 0, 0, 0);
}
/* u64 return: full r3 (driverInfo.head[head].lastFlipTime; nonzero once
 * a flip has completed -- 0xFEC in sys_rsx writes it). */
static inline uint64_t gcm_get_last_flip_time(void) {
    return sce_opd_call6(sce_slot_cellGcmGetLastFlipTime,
                         0, 0, 0, 0, 0, 0);
}

/* Official values: CELL_GCM_DISPLAY_HSYNC = 1, CELL_GCM_DISPLAY_VSYNC = 2.
 * We previously passed 1, i.e. HSYNC (tearing on real HW). */
#define GCM_FLIP_VSYNC 2
#define GCM_FB_ALLOC_SIZE 0x200000u /* MapMainMemory requires 1MB granularity */

static uint32_t s_ctx_addr = 0;   /* CellGcmContextData* from _cellGcmInitBody */
static uint32_t s_io_ea    = 0;   /* command ring EA (1MB aligned) */
static uint32_t s_cur_buf  = 0;
static int      s_gfx_ready = 0;

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
/* TEMP DIAG: count flip-wait exhaustion to find the frame-time sink. */
static int s_flip_diag = 20;

/* TEMP DIAG: guest-side probes into firmware + RPCS3 RSX state.
 *   FW_RTOC - 32752  -> firmware state pointer; its +8 field is the "head"
 *     argument the cellGcmSetFlipHandler thunk reads (helper 0x10061090).
 *   driverInfo base = value returned by cellGcmSetFlipHandler (0x40200000);
 *     +0x10C0/+0x1100 = head[0]/head[1] flipFlags, +0x10F8 lastFlipTime,
 *     +0x12C0 = handlers bitmask (RPCS3 sys_rsx.h layout). */
#define FW_RTOC 0x100788D0u
static inline uint32_t rd32(uint32_t a) {
    return *(volatile uint32_t *)(uintptr_t)a;
}
static uint32_t fw_flip_head(void) {
    uint32_t sp = rd32(FW_RTOC - 32752u);
    return sp ? rd32(sp + 8u) : 0xFFFFFFFFu;
}
static uint32_t s_di_base = 0;
static inline uint32_t di_rd(uint32_t off) {
    return s_di_base ? rd32(s_di_base + off) : 0u;
}

static void rsx_flip(void) {
    if (!s_gfx_ready) return;

    /* Wait for the previous flip to complete (vsync) */
    int waited = 0;
    while (waited < 200 && gcm_get_flip_status() != 0) {
        ps3_sleep_ms(1);
        waited++;
    }
    if (s_flip_diag > 0) {
        s_flip_diag--;
        tty("[RSX] flip w=%d st=%d cb=%u head=%u lft=%u hnd=%08x f0=%08x f1=%08x lt=%08x%08x\n",
            waited, (int)gcm_get_flip_status(), s_flip_cb_count, fw_flip_head(),
            (uint32_t)gcm_get_last_flip_time(),
            di_rd(0x12C0), di_rd(0x10C0), di_rd(0x1100),
            di_rd(0x10F8), di_rd(0x10FC));
    }
    gcm_reset_flip_status();

    /* Copy software FB to main memory mapped into IO */
    if (s_rsx[s_cur_buf].ptr)
        memcpy(s_rsx[s_cur_buf].ptr, s_fb, FB_SIZE);

    /* Issue the GCM flip.  cellGcmSetFlip only writes the flip semaphore
     * (CPU-polling part); cellGcmSetFlipImmediate additionally triggers the
     * actual flip on the RSX side (GCM_FLIP_HEAD -> request_emu_flip ->
     * handle_emu_flip -> 0xFEC).  Without it, cellGcmGetFlipStatus() stays
     * at WAITING forever. */
    gcm_set_flip_immediate(s_ctx_addr, s_cur_buf);

    s_cur_buf ^= 1u;
}

/* ── GFX init ────────────────────────────────────────────────────────────  */
int platform_gfx_init(int width, int height, const char *title) {
    (void)width; (void)height; (void)title;
    tty("[RSX] Initialising %dx%d double-buffered framebuffer\n", FB_WIDTH, FB_HEIGHT);

    if (!SCE_BOUND(_cellGcmInitBody) || !SCE_BOUND(cellGcmSetFlip)) {
        tty("[RSX] cellGcmSys imports not bound — running TTY-only\n");
        return 0;
    }

    /* Command ring: ioSize bytes, EA 1MB-aligned (GCM IO map granularity) */
    s_io_ea = (uint32_t)(uintptr_t)memalign(0x100000, 0x100000);
    if (!s_io_ea) { tty("[RSX] command buffer alloc failed\n"); return 0; }
    memset((void *)(uintptr_t)s_io_ea, 0, 0x100000);

    {
        /* TEMP DIAG: dump the bound OPD before the first GCM call. */
        volatile uint32_t *opd =
            (volatile uint32_t *)(uintptr_t)(uint32_t)sce_slot__cellGcmInitBody;
        tty("[RSX] dbg slot=%08x%08x opd@%08x fn=%08x rtoc=%08x\n",
            (uint32_t)(sce_slot__cellGcmInitBody >> 32),
            (uint32_t)sce_slot__cellGcmInitBody,
            (uint32_t)(uintptr_t)opd, opd[0], opd[1]);
    }

    int r = gcm_init_body(&s_ctx_addr, 256 * 1024, 1024 * 1024, s_io_ea);
    if (r != 0 || !s_ctx_addr) {
        tty("[RSX] _cellGcmInitBody failed (%d) — TTY-only mode\n", r);
        return 0;
    }
    gcm_set_flip_mode(GCM_FLIP_VSYNC);
    tty("[RSX] di: head-after-init=%u\n", fw_flip_head());

    /* Register the flip handler — REQUIRED for flip status to ever clear
     * on the LLE path (see gcm_set_flip_handler comment above). */
    if (SCE_BOUND(cellGcmSetFlipHandler)) {
        uint64_t hret = gcm_set_flip_handler((uint64_t)(uintptr_t)&flip_handler_cb);
        uint32_t hb = (uint32_t)hret;
        if ((hb >> 24) == 0x40u) s_di_base = hb; /* driverInfo base */
        tty("[RSX] SetFlipHandler -> %08x head=%u hnd=%08x\n",
            hb, fw_flip_head(), di_rd(0x12C0));

        /* TEMP DIAG EXPERIMENT: the firmware picked head 0 (handlers bit 3),
         * but sys_rsx sends flip events as FLIP_BASE<<1 (bit 4) and
         * send_event() filters by driverInfo.handlers -> event dropped ->
         * intr thread never updates flip status.  Force the flip-head state
         * to 1 and re-register to set bit 4 as well. */
        uint32_t sp = rd32(FW_RTOC - 32752u);
        if (sp && rd32(sp + 8u) == 0u) {
            *(volatile uint32_t *)(uintptr_t)(sp + 8u) = 1u;
            hret = gcm_set_flip_handler((uint64_t)(uintptr_t)&flip_handler_cb);
            tty("[RSX] di2: poked head=1 ret=%08x head=%u hnd=%08x\n",
                (uint32_t)hret, fw_flip_head(), di_rd(0x12C0));
        }
    }

    for (int i = 0; i < NUM_BUFS; i++) {
        /* Framebuffers live in main memory mapped into RSX IO space;
         * cellGcmMapMainMemory enforces 1MB alignment/granularity. */
        s_rsx[i].ptr = (uint32_t *)memalign(0x100000, GCM_FB_ALLOC_SIZE);
        if (!s_rsx[i].ptr) { tty("[RSX] buf%d alloc failed\n", i); return 0; }
        memset(s_rsx[i].ptr, 0, GCM_FB_ALLOC_SIZE);

        r = gcm_map_main_memory((uint32_t)(uintptr_t)s_rsx[i].ptr,
                                GCM_FB_ALLOC_SIZE, &s_rsx[i].offset);
        if (r != 0) {
            tty("[RSX] cellGcmMapMainMemory buf%d failed (%d) — TTY-only\n", i, r);
            return 0;
        }

        r = gcm_set_display_buf((uint32_t)i, s_rsx[i].offset,
                                FB_PITCH, FB_WIDTH, FB_HEIGHT);
        if (r != 0) {
            tty("[RSX] cellGcmSetDisplayBuffer buf%d failed (%d) — TTY-only\n", i, r);
            return 0;
        }
    }

    s_cur_buf   = 0;
    s_gfx_ready = 1;
    tty("[RSX] Framebuffer ready (%d KB per buffer, ctx=0x%08x)\n",
        (int)(GCM_FB_ALLOC_SIZE / 1024), s_ctx_addr);
    return 0;
}

void platform_gfx_shutdown(void) {
    s_gfx_ready = 0;
    for (int i = 0; i < NUM_BUFS; i++) {
        if (s_rsx[i].ptr) {
            free(s_rsx[i].ptr);
            s_rsx[i].ptr = NULL;
        }
    }
    if (s_io_ea) { free((void *)(uintptr_t)s_io_ea); s_io_ea = 0; }
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
    fb_str(188, 18, "v01.00", COL_DIM, 1);
    fb_str_r(FB_WIDTH - 20, 14,
             "[X]Play  [O]Back  [/\\ ]Pause",
             COL_DIM, 1);

    /* ── Now-playing strip (41–109) ─────────────────────────────────────  */
    fb_rect(0, 40, FB_WIDTH, 1, COL_DIM);
    fb_rect(0, 41, FB_WIDTH, 68, 0xFF0D1B2Au);

    if (track && track->title[0]) {
        const char *icon = (pstate == (int)PLAYBACK_PLAYING) ? "> " : "|| ";
        char ttl[64]; trunc(ttl, sizeof(ttl), track->title, 26);
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
        char ttl[56]; trunc(ttl, sizeof(ttl), t->title, 38);
        fb_str(56, ty+4, ttl, (idx==cursor) ? COL_FG : 0xFFCCCCCCu, 1);

        /* Artist */
        char art[48]; trunc(art, sizeof(art), t->artist, 32);
        fb_str(56, ty+18, art, COL_DIM, 1);

        /* Duration */
        char dur[12];
        uint32_t d = t->duration_sec;
        snprintf(dur, sizeof(dur), "%u:%02u", d/60u, d%60u);
        fb_str_r(FB_WIDTH-16, ty+4, dur, COL_DIM, 1);

        /* Quality badge */
        const char *qlabel = t->is_hi_res ? "HI-RES" : "FLAC";
        uint32_t qcol      = t->is_hi_res ? COL_ACCENT : COL_GREEN;
        fb_str_r(FB_WIDTH-64, ty+4, qlabel, qcol, 1);

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
 *  SECTION 3 — AUDIO OUTPUT  (cellAudioInit / cellAudioPortOpen / cellAudioPortStart)
 *
 *  Bound at load time through the SCE dynamic import tables written by
 *  tools/sce_imports.py.  Silence is the fallback if binding failed.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* cellAudio SDK structures (big-endian, native to the PPU) */
typedef struct {
    uint64_t nChannel;   /* 2 or 8 */
    uint64_t nBlock;     /* 2, 4, 8, 16 or 32 */
    uint64_t attr;       /* CELL_AUDIO_PORTATTR_INITLEVEL applies 'level' */
    float    level;
} CellAudioPortParam;

typedef struct {
    uint32_t readIndexAddr;  /* EA of a u64 consumed-block counter */
    uint32_t status;
    uint64_t nChannel;
    uint64_t nBlock;
    uint32_t portSize;
    uint32_t portAddr;       /* EA of the port ring buffer */
} CellAudioPortConfig;

#define CELL_AUDIO_PORTATTR_INITLEVEL 0x1000ULL

SCE_SLOT(cellAudioInit);
SCE_SLOT(cellAudioPortOpen);
SCE_SLOT(cellAudioGetPortConfig);
SCE_SLOT(cellAudioPortStart);
SCE_SLOT(cellAudioPortStop);
SCE_SLOT(cellAudioPortClose);
SCE_SLOT(cellAudioSetPortLevel);

static inline int aud_init(void) {
    return (int)sce_opd_call6(sce_slot_cellAudioInit, 0, 0, 0, 0, 0, 0);
}
static inline int aud_port_open(const CellAudioPortParam *p, uint32_t *id) {
    return (int)sce_opd_call6(sce_slot_cellAudioPortOpen,
                              (uint64_t)(uintptr_t)p,
                              (uint64_t)(uintptr_t)id, 0, 0, 0, 0);
}
static inline int aud_get_cfg(uint32_t id, CellAudioPortConfig *cfg) {
    return (int)sce_opd_call6(sce_slot_cellAudioGetPortConfig,
                              id, (uint64_t)(uintptr_t)cfg, 0, 0, 0, 0);
}
static inline int aud_port_start(uint32_t id) {
    return (int)sce_opd_call6(sce_slot_cellAudioPortStart,
                              id, 0, 0, 0, 0, 0);
}
static inline int aud_port_stop(uint32_t id) {
    return (int)sce_opd_call6(sce_slot_cellAudioPortStop,
                              id, 0, 0, 0, 0, 0);
}
static inline int aud_port_close(uint32_t id) {
    return (int)sce_opd_call6(sce_slot_cellAudioPortClose,
                              id, 0, 0, 0, 0, 0);
}
static inline int aud_set_level(uint32_t id, float lvl) {
    return (int)sce_opd_call6f(sce_slot_cellAudioSetPortLevel, id, lvl);
}

#define AUDIO_BLOCK_SAMPLES 256u

static uint32_t   s_aport  = 0;
static int        s_aready = 0;
static float     *s_adma   = NULL;
static volatile uint64_t *s_aridx = NULL;
static uint64_t   s_awrite = 0;
static uint32_t   s_anblk  = 8;
static uint32_t   s_astride = AUDIO_BLOCK_SAMPLES * 2; /* floats per block */
static uint32_t   s_ach    = 2;

int platform_audio_init(uint32_t sample_rate, uint8_t channels) {
    (void)sample_rate;
    s_ach = 2u;
    tty("[PS3] cellAudio initialising: 48000 Hz, %u ch\n", s_ach);

    if (!SCE_BOUND(cellAudioInit) || !SCE_BOUND(cellAudioPortOpen)) {
        tty("[PS3] cellAudio imports not bound — silent mode\n");
        return 0;
    }
    if (aud_init() != 0) {
        tty("[PS3] cellAudioInit failed — silent mode\n");
        return 0;
    }

    CellAudioPortParam p;
    memset(&p, 0, sizeof(p));
    p.nChannel = s_ach;
    p.nBlock   = 8;
    p.attr     = CELL_AUDIO_PORTATTR_INITLEVEL;
    p.level    = 1.0f;

    uint32_t port = 0;
    if (aud_port_open(&p, &port) != 0) {
        tty("[PS3] cellAudioPortOpen failed\n");
        return 0;
    }
    s_aport = port;

    CellAudioPortConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (SCE_BOUND(cellAudioGetPortConfig) && aud_get_cfg(s_aport, &cfg) == 0) {
        s_adma  = (float *)(uintptr_t)cfg.portAddr;
        s_aridx = (volatile uint64_t *)(uintptr_t)cfg.readIndexAddr;
        if (cfg.nBlock) s_anblk = (uint32_t)cfg.nBlock;
        if (cfg.portSize && s_anblk)
            s_astride = (uint32_t)(cfg.portSize / s_anblk / sizeof(float));
    }
    if (SCE_BOUND(cellAudioPortStart)) aud_port_start(s_aport);
    s_aready = 1;
    platform_audio_set_volume(1.0f); /* also keeps this call path alive under LTO */
    tty("[PS3] cellAudioPortStart OK — port %u, %u blocks, stride %u floats\n",
        s_aport, s_anblk, s_astride);
    return 0;
}

void platform_audio_shutdown(void) {
    if (s_aready) {
        if (SCE_BOUND(cellAudioPortStop))  aud_port_stop(s_aport);
        if (SCE_BOUND(cellAudioPortClose)) aud_port_close(s_aport);
        s_aready = 0;
    }
    tty("[PS3] cellAudio released.\n");
}

void platform_audio_write(const int16_t *samples, size_t frames) {
    if (!s_aready || !s_adma || !frames) return;

    size_t done = 0;
    while (done < frames) {
        /* Wait while every free block would still be overwritten */
        uint64_t ridx = s_aridx ? *s_aridx : s_awrite;
        int spin = 100;
        while (spin-- > 0 && (s_awrite - ridx) >= (uint64_t)s_anblk) {
            ps3_sleep_ms(1);
            ridx = s_aridx ? *s_aridx : s_awrite;
        }

        uint32_t bpos = (uint32_t)(s_awrite % s_anblk);
        float   *dst  = s_adma + (size_t)bpos * s_astride;

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

/* kept alive via 'used': volume control will be wired to the UI later,
 * and LTO would otherwise drop this (and its import slot) unused. */
__attribute__((used))
void platform_audio_set_volume(float vol) {
    if (s_aready && SCE_BOUND(cellAudioSetPortLevel)) aud_set_level(s_aport, vol);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 4 — INPUT  (DualShock 3 via cellPadGetData)
 *
 *  CellPadData.button[] u16 slots:
 *    [2] digital1: 0x10=Up  0x20=Right  0x40=Down  0x80=Left  0x08=Start
 *    [3] digital2: 0x10=Triangle  0x20=Circle  0x40=Cross  0x80=Square
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    int32_t  len;
    uint16_t button[64];
} CellPadData;

SCE_SLOT(cellPadInit);
SCE_SLOT(cellPadGetData);

static inline int pad_init(uint32_t ver) {
    return (int)sce_opd_call6(sce_slot_cellPadInit, ver, 0, 0, 0, 0, 0);
}
static inline int pad_get_data(uint32_t id, CellPadData *d) {
    return (int)sce_opd_call6(sce_slot_cellPadGetData,
                              id, (uint64_t)(uintptr_t)d, 0, 0, 0, 0);
}

static int      s_pad_ready = 0;
static uint16_t s_prev2 = 0, s_prev3 = 0;

InputAction platform_poll_input(void) {
    /* Lazy init */
    if (!s_pad_ready && SCE_BOUND(cellPadInit)) {
        if (pad_init(7) == 0) { s_pad_ready = 1; tty("[PS3] cellPadInit OK\n"); }
    }

    if (!s_pad_ready || !SCE_BOUND(cellPadGetData)) {
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

    CellPadData pd; memset(&pd, 0, sizeof(pd));
    if (pad_get_data(0, &pd) != 0) return INPUT_NONE;

    uint16_t b2 = pd.button[2], b3 = pd.button[3];
    uint16_t n2 = (uint16_t)(b2 & ~s_prev2), n3 = (uint16_t)(b3 & ~s_prev3);
    s_prev2 = b2; s_prev3 = b3;

    if (n2 & 0x10) return INPUT_UP;
    if (n2 & 0x40) return INPUT_DOWN;
    if (n2 & 0x80) return INPUT_LEFT;
    if (n2 & 0x20) return INPUT_RIGHT;
    if (n3 & 0x40) return INPUT_SELECT;        /* Cross     */
    if (n3 & 0x20) return INPUT_CANCEL;        /* Circle    */
    if (n3 & 0x80) return INPUT_ACTION_SEARCH; /* Square    */
    if (n3 & 0x10) return INPUT_PLAY_PAUSE;    /* Triangle  */
    if (n2 & 0x08) return INPUT_QUIT;          /* Start     */
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
