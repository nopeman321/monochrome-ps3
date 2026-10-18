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
extern int  ps3_sys_ppu_thread_create(uint64_t *thread_id, void (*entry)(uint64_t), uint64_t arg, int32_t prio, uint64_t stack_size, uint64_t flags, const char *thread_name);
extern int  ps3_sys_ppu_thread_join(uint64_t thread_id, uint64_t *val);
extern void ps3_sys_ppu_thread_exit(uint64_t val);

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
 *  SECTION 1.5 — PPU THREADING (Cell B.E. / GameOS sys_ppu_thread)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_PLATFORM_THREADS 8

typedef struct {
    uint64_t thread_id;
    PlatformThreadFunc func;
    void *arg;
    volatile int in_use;
} __attribute__((aligned(16))) Ps3ThreadContext;

static Ps3ThreadContext s_thread_pool[MAX_PLATFORM_THREADS];

static void ps3_thread_entry_stub(uint64_t raw_arg) {
    Ps3ThreadContext *ctx = (Ps3ThreadContext *)(uintptr_t)raw_arg;
    if (ctx && ctx->func) {
        PlatformThreadFunc f = ctx->func;
        void *a = ctx->arg;
        f(a);
    }
    ps3_sys_ppu_thread_exit(0);
}

PlatformThreadHandle platform_thread_create(PlatformThreadFunc func, void *arg) {
    if (!func) return NULL;

    Ps3ThreadContext *ctx = NULL;
    for (int i = 0; i < MAX_PLATFORM_THREADS; i++) {
        if (!s_thread_pool[i].in_use) {
            ctx = &s_thread_pool[i];
            ctx->in_use = 1;
            break;
        }
    }

    if (!ctx) {
        tty("[THREAD] Error: No free thread slot in pool\n");
        return NULL;
    }

    ctx->func = func;
    ctx->arg = arg;
    ctx->thread_id = 0;

    /*
     * sys_ppu_thread_create:
     * - entry: ps3_thread_entry_stub (OPD descriptor under PPC64 ELFv1)
     * - arg: ctx pointer passed in r3
     * - prio: 1000 (standard priority)
     * - stack size: 64 KB (65536 bytes)
     * - flags: SYS_PPU_THREAD_CREATE_JOINABLE (1)
     */
    uint64_t tid = 0;
    int ret = ps3_sys_ppu_thread_create(&tid, ps3_thread_entry_stub, (uint64_t)(uintptr_t)ctx,
                                        1000, 65536, 1, "monochrome_stream");
    if (ret != 0 || tid == 0) {
        tty("[THREAD] sys_ppu_thread_create failed (code %d)\n", ret);
        ctx->in_use = 0;
        return NULL;
    }

    ctx->thread_id = tid;
    tty("[THREAD] PPU Thread %llu created successfully (entry=%p, stack=64KB)\n",
        (unsigned long long)tid, (void *)ps3_thread_entry_stub);
    return (PlatformThreadHandle)ctx;
}

void platform_thread_join(PlatformThreadHandle thread) {
    if (!thread) return;

    Ps3ThreadContext *ctx = (Ps3ThreadContext *)thread;
    if (ctx->thread_id != 0) {
        uint64_t exit_val = 0;
        int ret = ps3_sys_ppu_thread_join(ctx->thread_id, &exit_val);
        tty("[THREAD] PPU Thread %llu joined (code %d, exit_val=%llu)\n",
            (unsigned long long)ctx->thread_id, ret, (unsigned long long)exit_val);
        ctx->thread_id = 0;
    }
    ctx->func = NULL;
    ctx->arg = NULL;
    ctx->in_use = 0;
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
#ifndef GCM_FLIP_VSYNC
#define GCM_FLIP_VSYNC 2
#endif
#define GCM_FB_ALLOC_SIZE 0x200000u /* MapMainMemory requires 1MB granularity */

extern void *memalign(size_t boundary, size_t size);

/* Minimal GCM context type — enough for the function pointers we need */
typedef struct { uint32_t opaque[4]; } gcmContextData;

static gcmContextData *s_gcm        = NULL;
static uint32_t        s_ctx_addr   = 0;   /* CellGcmContextData* from _cellGcmInitBody */
static uint32_t        s_io_ea      = 0;   /* command ring EA (1MB aligned) */
static uint32_t        curr_fb      = 0;
#define s_cur_buf curr_fb
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
#define COL_CARD    0xFF141824u

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

static void fb_blit_cover(int dst_x, int dst_y, int dst_w, int dst_h,
                          const uint32_t *src_pixels, int src_w, int src_h) {
    if (!src_pixels || src_w <= 0 || src_h <= 0) return;
    for (int y = 0; y < dst_h; y++) {
        int sy = (y * src_h) / dst_h;
        for (int x = 0; x < dst_w; x++) {
            int sx = (x * src_w) / dst_w;
            fb_px(dst_x + x, dst_y + y, src_pixels[sy * src_w + sx]);
        }
    }
}

/* ── RSX / GCM Abstraction ──────────────────────────────────────────────── */
__attribute__((weak)) int gcmGetFlipStatus(void) {
    if (SCE_BOUND(cellGcmGetFlipStatus))
        return gcm_get_flip_status();
    return 0;
}

__attribute__((weak)) void gcmResetFlipStatus(void) {
    if (SCE_BOUND(cellGcmResetFlipStatus))
        gcm_reset_flip_status();
}

__attribute__((weak)) int gcmSetFlip(gcmContextData *ctx, uint32_t id) {
    if (SCE_BOUND(cellGcmSetFlipImmediate))
        return gcm_set_flip_immediate((uint32_t)(uintptr_t)ctx, id);
    if (SCE_BOUND(cellGcmSetFlip))
        return gcm_set_flip((uint32_t)(uintptr_t)ctx, id);
    return 0;
}

__attribute__((weak)) void rsxFlushBuffer(gcmContextData *ctx) {
    (void)ctx;
}

__attribute__((weak)) void gcmSetWaitFlip(gcmContextData *ctx) {
    (void)ctx;
}

__attribute__((weak)) void sysUtilCheckCallback(void) {
}

__attribute__((weak)) int gcmSetDisplayBuffer(uint32_t id, uint32_t offset,
                                              uint32_t pitch, uint32_t w, uint32_t h) {
    if (SCE_BOUND(cellGcmSetDisplayBuffer))
        return gcm_set_display_buf(id, offset, pitch, w, h);
    return 0;
}

__attribute__((weak)) int gcmSetFlipMode(uint32_t mode) {
    if (SCE_BOUND(cellGcmSetFlipMode)) {
        uint32_t m = (mode == 1) ? 2 : mode;
        return gcm_set_flip_mode(m);
    }
    return 0;
}

__attribute__((weak)) void *rsxMemalign(uint32_t align, uint32_t size) {
    uint32_t a = (align < 0x100000u) ? 0x100000u : align;
    uint32_t s = (size < GCM_FB_ALLOC_SIZE) ? GCM_FB_ALLOC_SIZE : size;
    return memalign(a, s);
}

__attribute__((weak)) void rsxFree(void *ptr) {
    if (ptr) free(ptr);
}

__attribute__((weak)) int32_t rsxAddressToOffset(void *ptr, uint32_t *offset) {
    if (offset) {
        if (SCE_BOUND(cellGcmMapMainMemory)) {
            return gcm_map_main_memory((uint32_t)(uintptr_t)ptr, GCM_FB_ALLOC_SIZE, offset);
        }
        *offset = (uint32_t)((uintptr_t)ptr & 0xFFFFFFFFu);
    }
    return 0;
}

/* ── RSX synchronization & presentation ──────────────────────────────────── */
static void rsx_wait_flip(void) {
    if (gcmGetFlipStatus) {
        while (gcmGetFlipStatus() != 0) {
            usleep(200);
        }
    }
    if (gcmResetFlipStatus) gcmResetFlipStatus();
}

static void rsx_flip(void) {
    if (!s_gfx_ready) return;

    /* Copy software FB to RSX memory backbuffer */
    if (s_rsx[curr_fb].ptr)
        memcpy(s_rsx[curr_fb].ptr, s_fb, FB_SIZE);

    /* Frame presentation sequence */
    rsx_wait_flip();
    if (gcmSetFlip && s_gcm) gcmSetFlip(s_gcm, curr_fb);
    if (rsxFlushBuffer && s_gcm) rsxFlushBuffer(s_gcm);
    if (gcmSetWaitFlip && s_gcm) gcmSetWaitFlip(s_gcm);
    curr_fb ^= 1;
    if (sysUtilCheckCallback) sysUtilCheckCallback();
}

void platform_present_frame(void) {
    rsx_flip();
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

    int r = gcm_init_body(&s_ctx_addr, 256 * 1024, 1024 * 1024, s_io_ea);
    if (r != 0 || !s_ctx_addr) {
        tty("[RSX] _cellGcmInitBody failed (%d) — TTY-only mode\n", r);
        return 0;
    }
    s_gcm = (gcmContextData *)(uintptr_t)s_ctx_addr;

    /* Register the flip handler — REQUIRED for flip status to ever clear
     * on the LLE path (see gcm_set_flip_handler comment above). */
    if (SCE_BOUND(cellGcmSetFlipHandler)) {
        gcm_set_flip_handler((uint64_t)(uintptr_t)&flip_handler_cb);
    }
    (void)gcm_get_last_flip_time(); /* keep slot alive under LTO */

    uint32_t fb_width = (uint32_t)FB_WIDTH;
    uint32_t fb_height = (uint32_t)FB_HEIGHT;
    uint32_t color_pitch = (uint32_t)FB_PITCH;
    uint32_t color_offset[NUM_BUFS] = {0, 0};

    /* Reserva de 2 framebuffers alineados a 64 bytes en memoria local RSX */
    for (int i = 0; i < NUM_BUFS; i++) {
        uint32_t buf_size = color_pitch * fb_height;
        if (rsxMemalign) {
            s_rsx[i].ptr = (uint32_t *)rsxMemalign(64, buf_size);
        } else {
            s_rsx[i].ptr = (uint32_t *)malloc(buf_size);
        }

        if (!s_rsx[i].ptr) {
            tty("[RSX] buf%d alloc failed\n", i);
            return 0;
        }
        memset(s_rsx[i].ptr, 0, buf_size);

        color_offset[i] = 0;
        if (rsxAddressToOffset) {
            rsxAddressToOffset((void *)s_rsx[i].ptr, &color_offset[i]);
        } else {
            color_offset[i] = (uint32_t)((uintptr_t)s_rsx[i].ptr & 0xFFFFFFFFu);
        }
        s_rsx[i].offset = color_offset[i];
    }

    /* Registro de los buffers mediante gcmSetDisplayBuffer */
    if (gcmSetDisplayBuffer) {
        gcmSetDisplayBuffer(0, color_offset[0], color_pitch, fb_width, fb_height);
        gcmSetDisplayBuffer(1, color_offset[1], color_pitch, fb_width, fb_height);
    }

    /* Configurar gcmSetFlipMode(GCM_FLIP_VSYNC) */
    if (gcmSetFlipMode) {
        gcmSetFlipMode(GCM_FLIP_VSYNC);
    }

    curr_fb     = 0;
    s_gfx_ready = 1;
    tty("[RSX] Framebuffer ready (%d KB per buffer, ctx=0x%08x)\n",
        (int)(GCM_FB_ALLOC_SIZE / 1024), s_ctx_addr);
    return 0;
}

void platform_gfx_shutdown(void) {
    s_gfx_ready = 0;
    for (int i = 0; i < NUM_BUFS; i++) {
        if (s_rsx[i].ptr) {
            if (rsxFree) rsxFree((void *)s_rsx[i].ptr); else free(s_rsx[i].ptr);
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
             "[X]Play/Pause  [^/v]Vol  [L1/R1]Track  [O]Back",
             COL_DIM, 1);

    /* ── Now-playing strip (41–109) ─────────────────────────────────────  */
    fb_rect(0, 40, FB_WIDTH, 1, COL_DIM);
    fb_rect(0, 41, FB_WIDTH, 68, 0xFF0D1B2Au);

    /* 40x40 Album Cover Art Thumbnail */
    const CoverArtImage *cover = cover_art_get_current();
    if (cover && cover->ready) {
        fb_blit_cover(16, 48, 42, 42, cover->pixels, cover->width, cover->height);
        fb_rect(15, 47, 44, 1, COL_ACCENT);
        fb_rect(15, 90, 44, 1, COL_ACCENT);
        fb_rect(15, 47, 1, 44, COL_ACCENT);
        fb_rect(59, 47, 1, 44, COL_ACCENT);
    } else {
        fb_rect(16, 48, 42, 42, COL_CARD);
        fb_str(30, 60, ">", COL_FG, 2);
    }

    if (track && track->title[0]) {
        const char *icon = (pstate == (int)PLAYBACK_PLAYING) ? "> " : "|| ";
        char ttl[64]; trunc(ttl, sizeof(ttl), track->title, 20);
        fb_str(68, 52, icon, COL_GREEN, 2);
        fb_str(68 + (int)strlen(icon)*16, 52, ttl, COL_FG, 2);

        char meta[128];
        snprintf(meta, sizeof(meta), "%s  |  %s",
                 track->artist[0] ? track->artist : "Unknown",
                 track->album[0]  ? track->album  : "Unknown");
        fb_str(68, 78, meta, COL_DIM, 1);
    } else {
        fb_str(68, 58, "-- No track selected --", COL_DIM, 2);
    }

    /* ── 16-Band Real-Time Audio Spectrum Visualizer (x: 360–504, y: 56–88) ── */
    float vis_bands[VIS_BANDS];
    audio_get_visualization(vis_bands, VIS_BANDS);
    for (int b = 0; b < VIS_BANDS; b++) {
        int bar_x = 360 + b * 9;
        int bar_h = (int)(vis_bands[b] * 28.0f);
        if (bar_h < 2) bar_h = 2;
        if (bar_h > 28) bar_h = 28;
        int bar_y = 86 - bar_h;
        fb_rect(bar_x, bar_y, 6, bar_h, COL_ACCENT);
        fb_rect(bar_x, bar_y - 2, 6, 2, COL_FG);
    }

    /* ── Volume HUD & DMA Ring-Buffer Occupancy ───────────────────────── */
    extern float platform_audio_get_volume_hud(void);
    extern uint32_t platform_audio_get_dma_occ(void);
    extern uint32_t platform_audio_get_nblk(void);

    int vx = FB_WIDTH - 180, vy = 52, vw = 80, vh = 8;
    fb_str(vx - 32, vy, "VOL", COL_DIM, 1);
    fb_rect(vx, vy, vw, vh, COL_BAR_BG);
    float cur_vol = platform_audio_get_volume_hud();
    int vfill = (int)(vw * cur_vol);
    if (vfill > 0) fb_rect(vx, vy, vfill, vh, COL_ACCENT);
    char vstr[16]; snprintf(vstr, sizeof(vstr), "%d%%", (int)(cur_vol * 100.0f + 0.5f));
    fb_str(vx + vw + 8, vy, vstr, COL_FG, 1);

    int dx = FB_WIDTH - 180, dy = 72, dw = 80, dh = 8;
    fb_str(dx - 32, dy, "DMA", COL_DIM, 1);
    fb_rect(dx, dy, dw, dh, COL_BAR_BG);
    uint32_t occ = platform_audio_get_dma_occ();
    uint32_t nblk = platform_audio_get_nblk();
    int dfill = (int)(dw * ((float)occ / (float)(nblk ? nblk : 8)));
    if (dfill > 0) fb_rect(dx, dy, dfill, dh, COL_GREEN);
    char dstr[16]; snprintf(dstr, sizeof(dstr), "%u/%u", occ, nblk ? nblk : 8);
    fb_str(dx + dw + 8, dy, dstr, COL_DIM, 1);

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
    /* ── On-Screen Keyboard Modal Overlay (when state == STATE_SEARCH) ── */
    if (state == (int)STATE_SEARCH) {
        int osk_r = 0, osk_c = 0;
        const char *osk_q = NULL;
        ui_get_osk_state(&osk_r, &osk_c, &osk_q);

        /* Modal Background Box (centered: 720x480 screen) */
        int mx = 40, my = 50, mw = FB_WIDTH - 80, mh = 370;
        fb_rect(mx, my, mw, mh, 0xFF0D1B2Au);
        fb_rect(mx, my, mw, 2, COL_ACCENT);
        fb_rect(mx, my + mh - 2, mw, 2, COL_ACCENT);
        fb_rect(mx, my, 2, mh, COL_ACCENT);
        fb_rect(mx + mw - 2, my, 2, mh, COL_ACCENT);

        fb_str(mx + 20, my + 15, "SEARCH TIDAL / MONOCHROME", COL_FG, 2);
        char qdisp[80];
        snprintf(qdisp, sizeof(qdisp), "Query: %s_", osk_q ? osk_q : "");
        fb_str(mx + 20, my + 45, qdisp, COL_ACCENT, 2);

        static const char *k_grid[4][10] = {
            { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
            { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" },
            { "A", "S", "D", "F", "G", "H", "J", "K", "L", "-" },
            { "Z", "X", "C", "V", "B", "N", "M", "SPC", "DEL", "GO" }
        };

        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 10; c++) {
                int kx = mx + 20 + c * 60;
                int ky = my + 85 + r * 55;
                int kw = 52, kh = 42;
                bool is_sel = (r == osk_r && c == osk_c);
                uint32_t bg_col = is_sel ? COL_ACCENT : 0xFF1C2541u;
                uint32_t fg_col = is_sel ? COL_FG : COL_DIM;
                fb_rect(kx, ky, kw, kh, bg_col);
                if (is_sel) {
                    fb_rect(kx, ky, kw, 1, COL_FG);
                    fb_rect(kx, ky + kh - 1, kw, 1, COL_FG);
                    fb_rect(kx, ky, 1, kh, COL_FG);
                    fb_rect(kx + kw - 1, ky, 1, kh, COL_FG);
                }
                fb_str(kx + 14, ky + 12, k_grid[r][c], fg_col, 2);
            }
        }

        fb_str(mx + 20, my + mh - 35, "[D-Pad] Navigate  [X] Select  [Triangle] Delete  [O] Cancel", COL_DIM, 1);
        fb_str(mx + 20, my + mh - 18, "[Square] Quick Preset  Select [GO] to search live API", COL_ACCENT, 1);
    }

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
static uint32_t   s_audio_write_block = 0;
static uint32_t   s_anblk  = 8;
static uint32_t   s_astride = AUDIO_BLOCK_SAMPLES * 2; /* floats per block */
static uint32_t   s_ach    = 2;
static float      s_master_volume = 1.0f;
static int        s_audio_playing = 1;

float platform_audio_get_volume_hud(void) {
    return s_master_volume;
}

uint32_t platform_audio_get_nblk(void) {
    return s_anblk;
}

uint32_t platform_audio_get_dma_occ(void) {
    uint64_t hw_read_idx = s_aridx ? *s_aridx : 0;
    uint32_t hw_blk = (uint32_t)(hw_read_idx % s_anblk);
    uint32_t wr_blk = s_audio_write_block % s_anblk;
    return (wr_blk >= hw_blk) ? (wr_blk - hw_blk) : (s_anblk - (hw_blk - wr_blk));
}

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
        /* Read current hardware consumed block counter from EA (big-endian PPU64) */
        uint64_t hw_read_idx = s_aridx ? *s_aridx : 0;
        uint32_t hw_read_block = (uint32_t)(hw_read_idx % s_anblk);

        /* Only write when target block is not currently being read by hardware DMA */
        int timeout_us = 10000;
        while ((s_audio_write_block % s_anblk) == hw_read_block && timeout_us > 0) {
            ps3_sys_timer_usleep(200);
            timeout_us -= 200;
            if (s_aridx) {
                hw_read_idx = *s_aridx;
                hw_read_block = (uint32_t)(hw_read_idx % s_anblk);
            }
        }

        uint32_t cur_block = s_audio_write_block % s_anblk;
        float *dst = s_adma + (size_t)cur_block * s_astride;

        size_t chunk = AUDIO_BLOCK_SAMPLES;
        if (chunk > frames - done) chunk = frames - done;

        if (!s_audio_playing) {
            /* Paused: fill DMA blocks with 0.0f */
            for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES * s_ach; i++) {
                dst[i] = 0.0f;
            }
        } else {
            const int16_t *src = samples + done * s_ach;
            for (size_t i = 0; i < chunk * s_ach; i++) {
                dst[i] = (float)src[i] / 32768.0f;
            }
            for (size_t i = chunk * s_ach; i < AUDIO_BLOCK_SAMPLES * s_ach; i++) {
                dst[i] = 0.0f;
            }
        }

        /* PowerPC memory barrier before advancing write block index */
        __asm__ volatile("lwsync" ::: "memory");

        s_audio_write_block = (s_audio_write_block + 1) % s_anblk;
        done += chunk;
    }
}

/* kept alive via 'used': volume control wired to DualShock 3 & HUD */
__attribute__((used))
void platform_audio_set_volume(float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    s_master_volume = vol;
    if (s_aready && SCE_BOUND(cellAudioSetPortLevel)) aud_set_level(s_aport, s_master_volume);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 4 — INPUT  (DualShock 3 via cellPadGetData)
 *
 *  CellPadData.button[] u16 slots:
 *    [2] digital1: 0x10=Up  0x20=Right  0x40=Down  0x80=Left  0x08=Start
 *    [3] digital2: 0x10=Triangle  0x20=Circle  0x40=Cross  0x80=Square
 *                  0x04=L1  0x08=R1  0x01=L2  0x02=R2
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

    /* D-Pad Up / Down / Left / Right: Menu & OSK navigation */
    if (n2 & 0x10) return INPUT_UP;
    if (n2 & 0x40) return INPUT_DOWN;
    if (n2 & 0x80) return INPUT_LEFT;
    if (n2 & 0x20) return INPUT_RIGHT;

    /* Cross (✕): Select track / Confirm OSK key */
    if (n3 & 0x40) return INPUT_SELECT;

    /* Circle (○): Cancel / Back */
    if (n3 & 0x20) return INPUT_CANCEL;

    /* Square (□): Action / Search (OSK) */
    if (n3 & 0x80) return INPUT_ACTION_SEARCH;

    /* Triangle (△): Toggle Play / Pause */
    if (n3 & 0x10) return INPUT_PLAY_PAUSE;

    /* L1 (0x04) / R1 (0x08): Adjust master volume */
    if (n3 & 0x04) {
        platform_audio_set_volume(s_master_volume - 0.05f);
        tty("[PAD] L1: Volume -> %.2f\n", s_master_volume);
    }
    if (n3 & 0x08) {
        platform_audio_set_volume(s_master_volume + 0.05f);
        tty("[PAD] R1: Volume -> %.2f\n", s_master_volume);
    }

    /* Start: Quit */
    if (n2 & 0x08) return INPUT_QUIT;

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

int platform_http_get_streaming(const char *url, PlatformHttpChunkCallback on_chunk, void *userdata) {
    if (!on_chunk) return -1;
    tty("[NET] Streaming %s (PS3 fallback catalogue)\n", url ? url : "(null)");
    size_t n = strlen(k_catalog);
    on_chunk((const uint8_t *)k_catalog, n, userdata);
    return 0;
}

void platform_http_free(char *r) { if (r) free(r); }

#endif /* TARGET_PS3 */
