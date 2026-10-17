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
        : "r0", "r2", "r7", "r8", "r9", "r10", "r12", "lr", "ctr", "xer", "cr0", "cr1", "cr5", "cr6", "cr7", "memory"
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
        : "r0", "r2", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r12", "lr", "ctr", "xer", "cr0", "cr1", "cr5", "cr6", "cr7", "memory"
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
