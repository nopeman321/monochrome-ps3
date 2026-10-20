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

int  ps3_sys_ppu_thread_create(uint64_t *thread_id, void (*entry)(uint64_t), uint64_t arg, int32_t prio, uint64_t stack_size, uint64_t flags, const char *thread_name, void *tls);
int  ps3_sys_ppu_thread_join(uint64_t thread_id, uint64_t *val);
int  ps3_sys_ppu_thread_start(uint64_t thread_id);
int  ps3_sys_memory_allocate(uint32_t size, uint64_t flags, uint32_t *addr);
int  ps3_sys_net_socket(int domain, int type, int protocol);
int  ps3_sys_net_connect(int s, const void *addr, uint32_t addrlen);
int  ps3_sys_net_sendto(int s, const void *buf, uint32_t len, int flags, const void *addr, uint32_t addrlen);
int  ps3_sys_net_recvfrom(int s, void *buf, uint32_t len, int flags, void *addr, uint32_t *addrlen);
int  ps3_sys_net_close(int s);
int  ps3_sys_net_shutdown(int s, int how);
int  ps3_sys_net_setsockopt(int s, int level, int optname, const void *optval, uint32_t optlen);

void ps3_sys_ppu_thread_exit(uint64_t val);

/* ── PS3 GameOS LV2 Filesystem Syscalls ──────────────────────────── */
int  ps3_sys_fs_open(const char *path, int flags, int *fd, int mode, const void *arg, uint64_t size);
int  ps3_sys_fs_read(int fd, void *buf, uint64_t nbytes, uint64_t *nread);
int  ps3_sys_fs_write(int fd, const void *buf, uint64_t nbytes, uint64_t *nwritten);
int  ps3_sys_fs_close(int fd);
int  ps3_sys_fs_stat(const char *path, void *sb);
int  ps3_sys_fs_mkdir(const char *path, int mode);

#ifdef __cplusplus
}
#endif

#endif /* MONOCHROME_LV2_H */
