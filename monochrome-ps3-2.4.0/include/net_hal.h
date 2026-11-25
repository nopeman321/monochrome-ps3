/**
 * Monochrome PS3 - Unified Network Hardware Abstraction Layer (PSL1GHT v2)
 */

#ifndef NET_HAL_H
#define NET_HAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_HAL_DISCONNECTED = 0,
    NET_HAL_CONNECTING   = 1,
    NET_HAL_IP_OBTAINED  = 2,
    NET_HAL_ERROR        = 3
} NetHalState;

typedef struct {
    NetHalState state;
    char ip_str[32];
    char last_dns_host[128];
    char last_dns_ip[32];
    int last_http_code;
    int last_net_errno;
    int last_h_errno;
    uint32_t last_api_latency_ms;
    uint32_t stream_kbps;
    uint32_t reconnect_count;
    uint32_t ring_underruns;
} NetHalTelemetry;

/* Lifecycle & link state (safe to call multiple times; idempotent) */
int net_hal_init(void);
void net_hal_shutdown(void);
NetHalState net_hal_get_state(void);
bool net_hal_is_connected(void);

/* XMB overlay cooperative pause state */
void net_hal_set_menu_paused(bool paused);
bool net_hal_is_menu_paused(void);

/* Thread-safe DNS resolution with 60s TTL cache and h_errno capture.
 * Returns 0 on success, -1 on failure (sets *out_h_errno if non-NULL). */
int net_hal_resolve(const char *host, struct in_addr *out_ip, int *out_h_errno);

/* Non-blocking TCP connect with select() in <=250ms slices checking abort_flag & link state.
 * Returns 0 on success (*out_sock set to valid fd), -1 on failure (*out_sock = -1, *out_errno set). */
int net_hal_connect(const char *host, uint16_t port, int timeout_ms,
                    volatile bool *abort_flag, int *out_sock, int *out_errno);

/* Non-blocking send loop handling partial writes & select() slices.
 * Returns 0 when all len bytes are sent, -1 on error/timeout/abort (*out_errno set). */
int net_hal_send_all(int sock, const void *buf, size_t len, int timeout_ms,
                     volatile bool *abort_flag, int *out_errno);

/* Non-blocking recv slice using select() up to timeout_ms.
 * Retries ONLY EINTR; aborts on EAGAIN/EWOULDBLOCK/timeout/abort_flag.
 * Returns >0 bytes read, 0 on clean EOF, -1 on error/timeout/abort (*out_errno set). */
int net_hal_recv_some(int sock, void *buf, size_t len, int timeout_ms,
                      volatile bool *abort_flag, int *out_errno);

/* Input & URL sanitization helpers */
bool net_hal_validate_id(const char *id); /* [A-Za-z0-9_-], len 1..63 */
size_t net_hal_url_encode(const char *src, char *dst, size_t dst_sz); /* RFC 3986 */

/* Unified mutex-protected logger to /dev_hdd0/tmp/mono_stage6.log (2MB rotation) */
void net_hal_log(const char *tag, const char *fmt, ...);

/* Telemetry & monotonic-ish millisecond clock */
void net_hal_record_api_telemetry(int http_code, int net_err, uint32_t latency_ms);
void net_hal_record_stream_telemetry(uint32_t kbps, uint32_t reconnects, uint32_t underruns);
void net_hal_get_telemetry(NetHalTelemetry *out);
uint64_t net_hal_time_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_HAL_H */
