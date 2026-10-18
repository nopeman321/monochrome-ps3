#ifndef MONOCHROME_LV2_H
#define MONOCHROME_LV2_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── PS3 GameOS LV2 Syscalls ─────────────────────────────────────── */
int  ps3_sys_tty_write(int ch, const void *buf, uint32_t len, uint32_t *pwdata);
int  ps3_sys_timer_usleep(uint64_t usec);
int  ps3_sys_timer_sleep(uint64_t sec);
void ps3_sys_process_exit(int status);

int  ps3_sys_ppu_thread_create(uint64_t *thread_id, void (*entry)(uint64_t), uint64_t arg, int32_t prio, uint64_t stack_size, uint64_t flags, const char *thread_name);
int  ps3_sys_ppu_thread_join(uint64_t thread_id, uint64_t *val);
void ps3_sys_ppu_thread_exit(uint64_t val);

#ifdef __cplusplus
}
#endif

#endif /* MONOCHROME_LV2_H */
