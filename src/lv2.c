/**
 * PS3 GameOS LV2 Syscall Layer & Libc Bridge
 *
 * Implements genuine LV2 system calls on the Cell Broadband Engine PPE:
 *   - Syscall 403 (0x193): sys_tty_write (channel 0 = TTY console)
 *   - Syscall 141 (0x8D):  sys_timer_usleep (PPE thread suspension in usec)
 *   - Syscall 142 (0x8E):  sys_timer_sleep (PPE thread suspension in sec)
 *   - Syscall 3   (0x03):  sys_process_exit (clean process termination)
 *
 * Overrides libc write() and usleep() so all standard I/O (printf, puts)
 * and thread pacing route directly through Sony GameOS.
 */

#include "lv2.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>

static inline int64_t lv2_sc4(uint64_t num, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6) {
    register uint64_t _r11 asm("r11") = num;
    register uint64_t _r3  asm("r3")  = r3;
    register uint64_t _r4  asm("r4")  = r4;
    register uint64_t _r5  asm("r5")  = r5;
    register uint64_t _r6  asm("r6")  = r6;
    asm volatile(
        "sc\n"
        : "+r"(_r3)
        : "r"(_r11), "r"(_r4), "r"(_r5), "r"(_r6)
        : "r0", "r7", "r8", "r9", "r10", "r12", "lr", "ctr", "xer", "cr0", "cr1", "cr5", "cr6", "cr7", "memory"
    );
    return (int64_t)_r3;
}

static inline int64_t lv2_sc1(uint64_t num, uint64_t r3) {
    register uint64_t _r11 asm("r11") = num;
    register uint64_t _r3  asm("r3")  = r3;
    asm volatile(
        "sc\n"
        : "+r"(_r3)
        : "r"(_r11)
        : "r0", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r12", "lr", "ctr", "xer", "cr0", "cr1", "cr5", "cr6", "cr7", "memory"
    );
    return (int64_t)_r3;
}

static inline int64_t lv2_sc2(uint64_t num, uint64_t r3, uint64_t r4) {
    register uint64_t _r11 asm("r11") = num;
    register uint64_t _r3  asm("r3")  = r3;
    register uint64_t _r4  asm("r4")  = r4;
    asm volatile(
        "sc\n"
        : "+r"(_r3)
        : "r"(_r11), "r"(_r4)
        : "r0", "r5", "r6", "r7", "r8", "r9", "r10", "r12", "lr", "ctr", "xer", "cr0", "cr1", "cr5", "cr6", "cr7", "memory"
    );
    return (int64_t)_r3;
}

static inline int64_t lv2_sc7(uint64_t num, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8, uint64_t r9) {
    register uint64_t _r11 asm("r11") = num;
    register uint64_t _r3  asm("r3")  = r3;
    register uint64_t _r4  asm("r4")  = r4;
    register uint64_t _r5  asm("r5")  = r5;
    register uint64_t _r6  asm("r6")  = r6;
    register uint64_t _r7  asm("r7")  = r7;
    register uint64_t _r8  asm("r8")  = r8;
    register uint64_t _r9  asm("r9")  = r9;
    asm volatile(
        "sc\n"
        : "+r"(_r3)
        : "r"(_r11), "r"(_r4), "r"(_r5), "r"(_r6), "r"(_r7), "r"(_r8), "r"(_r9)
        : "r0", "r10", "r12", "lr", "ctr", "xer", "cr0", "cr1", "cr5", "cr6", "cr7", "memory"
    );
    return (int64_t)_r3;
}

static inline int64_t lv2_sc8(uint64_t num, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8, uint64_t r9, uint64_t r10) {
    register uint64_t _r11 asm("r11") = num;
    register uint64_t _r3  asm("r3")  = r3;
    register uint64_t _r4  asm("r4")  = r4;
    register uint64_t _r5  asm("r5")  = r5;
    register uint64_t _r6  asm("r6")  = r6;
    register uint64_t _r7  asm("r7")  = r7;
    register uint64_t _r8  asm("r8")  = r8;
    register uint64_t _r9  asm("r9")  = r9;
    register uint64_t _r10 asm("r10") = r10;
    asm volatile(
        "sc\n"
        : "+r"(_r3)
        : "r"(_r11), "r"(_r4), "r"(_r5), "r"(_r6), "r"(_r7), "r"(_r8), "r"(_r9), "r"(_r10)
        : "r0", "r12", "lr", "ctr", "xer", "cr0", "cr1", "cr5", "cr6", "cr7", "memory"
    );
    return (int64_t)_r3;
}

int ps3_sys_tty_write(int ch, const void *buf, uint32_t len, uint32_t *pwdata) {
    return (int)lv2_sc4(403, (uint64_t)ch, (uint64_t)buf, (uint64_t)len, (uint64_t)pwdata);
}

int ps3_sys_timer_usleep(uint64_t usec) {
    return (int)lv2_sc1(141, usec);
}

int ps3_sys_timer_sleep(uint64_t sec) {
    return (int)lv2_sc1(142, sec);
}

void ps3_sys_process_exit(int status) {
    lv2_sc1(3, (uint64_t)status);
    while (1) {
        asm volatile("nop");
    }
}

typedef struct {
    uint32_t entry;
    uint32_t tls;
} PpuThreadParam;

int ps3_sys_ppu_thread_create(uint64_t *thread_id, void (*entry)(uint64_t), uint64_t arg, int32_t prio, uint64_t stack_size, uint64_t flags, const char *thread_name) {
    PpuThreadParam param;
    param.entry = (uint32_t)(uintptr_t)entry;
    param.tls   = 0;

    return (int)lv2_sc8(52,
                        (uint64_t)(uintptr_t)thread_id,
                        (uint64_t)(uintptr_t)&param,
                        arg,
                        0, /* GameOS unk parameter */
                        (uint64_t)prio,
                        stack_size,
                        flags,
                        (uint64_t)(uintptr_t)thread_name);
}

int ps3_sys_ppu_thread_join(uint64_t thread_id, uint64_t *val) {
    int ret = (int)lv2_sc2(44, thread_id, (uint64_t)(uintptr_t)val);
    if (ret != 0) {
        int ret2 = (int)lv2_sc2(49, thread_id, (uint64_t)(uintptr_t)val);
        if (ret2 == 0) return ret2;
    }
    return ret;
}

void ps3_sys_ppu_thread_exit(uint64_t val) {
    lv2_sc1(41, val);
    while (1) {
        asm volatile("nop");
    }
}

/* ── Overrides for standard C library ────────────────────────────── */

ssize_t write(int fd, const void *buf, size_t count) {
    if (fd == 1 || fd == 2) {
        uint32_t written = 0;
        ps3_sys_tty_write(0, buf, (uint32_t)count, &written);
        return (ssize_t)count;
    }
    return (ssize_t)count;
}

int usleep(useconds_t usec) {
    return ps3_sys_timer_usleep((uint64_t)usec);
}

unsigned int sleep(unsigned int seconds) {
    ps3_sys_timer_sleep((uint64_t)seconds);
    return 0;
}

/* ── Stdio & Console Overrides (Bypasses Linux ioctl/writev) ─────── */
#include <stdarg.h>

int vprintf(const char *fmt, va_list ap) {
    char buf[4096];
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (len > 0) {
        uint32_t written = 0;
        ps3_sys_tty_write(0, buf, (uint32_t)len, &written);
    }
    return len;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vprintf(fmt, ap);
    va_end(ap);
    return ret;
}

int puts(const char *s) {
    if (!s) return 0;
    size_t len = strlen(s);
    uint32_t written = 0;
    ps3_sys_tty_write(0, s, (uint32_t)len, &written);
    ps3_sys_tty_write(0, "\n", 1, &written);
    return (int)len + 1;
}

int putchar(int c) {
    char ch = (char)c;
    uint32_t written = 0;
    ps3_sys_tty_write(0, &ch, 1, &written);
    return c;
}

void abort(void) {
    ps3_sys_process_exit(1);
    while (1) { asm volatile("nop"); }
}

void exit(int status) {
    ps3_sys_process_exit(status);
    while (1) { asm volatile("nop"); }
}

void _exit(int status) {
    ps3_sys_process_exit(status);
    while (1) { asm volatile("nop"); }
}

FILE *fopen(const char *pathname, const char *mode) {
    (void)pathname; (void)mode;
    return NULL;
}

int fclose(FILE *stream) {
    (void)stream;
    return 0;
}

int fseek(FILE *stream, long offset, int whence) {
    (void)stream; (void)offset; (void)whence;
    return -1;
}

long ftell(FILE *stream) {
    (void)stream;
    return -1;
}


/* ── Static Memory Allocator for GameOS (Bypasses Linux mmap/brk) ─── */
#define HEAP_POOL_SIZE  (16 * 1024 * 1024)  /* 16 MB heap pool in BSS */
static uint8_t s_heap_pool[HEAP_POOL_SIZE] __attribute__((aligned(16)));
static size_t  s_heap_used = 0;

void *malloc(size_t size) {
    if (size == 0) return NULL;
    /* 16-byte alignment required by Cell PPE */
    size_t aligned_size = (size + 15) & ~((size_t)15);
    if (s_heap_used + aligned_size > HEAP_POOL_SIZE) {
        return NULL;
    }
    void *ptr = &s_heap_pool[s_heap_used];
    s_heap_used += aligned_size;
    return ptr;
}


void free(void *ptr) {
    (void)ptr;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) return NULL;
    void *new_p = malloc(size);
    if (new_p) {
        memcpy(new_p, ptr, size);
    }
    return new_p;
}

/* memalign must live on the bump pool too: the linker otherwise resolves it
 * to the musl dlmalloc implementation, which faults on its very first call
 * because GameOS has no brk/mmap heap for it to manage.  Used by the RSX
 * layer for 1 MB-aligned command rings and framebuffer mappings. */
void *memalign(size_t alignment, size_t size) {
    if (alignment < 16) alignment = 16;
    size_t base   = (size_t)&s_heap_pool[s_heap_used];
    size_t aligned = (base + alignment - 1) & ~(alignment - 1);
    size_t used   = (aligned - (size_t)s_heap_pool) + ((size + 15) & ~(size_t)15);
    if (used > HEAP_POOL_SIZE) return NULL;
    s_heap_used = used;
    return (void *)aligned;
}

/* ── GameOS PPU Thread Entry Point ───────────────────────────────── */
extern int main(int argc, char *argv[]);

void ps3_main_entry(void) {
    int ret = main(0, NULL);
    ps3_sys_process_exit(ret);
}

