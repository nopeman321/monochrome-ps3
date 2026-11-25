/*
 * Monochrome PS3 - Unified Network Hardware Abstraction Layer (PSL1GHT v2)
 * Consolidates libnet / libnetctl lifecycle, thread-safe DNS with 60s TTL cache,
 * non-blocking socket primitives (select in 250ms slices), protocol sanitization,
 * unified buffered logging with 2MB rotation, and live network telemetry.
 */

#include <ppu-lv2.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

#include <sysmodule/sysmodule.h>
#include <sys/mutex.h>
#include <net/net.h>
#include <net/netctl.h>
#include <net/netdb.h>
#include <net/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "net_hal.h"

#ifndef ECANCELED
#define ECANCELED EINTR
#endif
#ifndef ENETDOWN
#define ENETDOWN EIO
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT EAGAIN
#endif

#define NET_HAL_LOG_PATH      "/dev_hdd0/tmp/mono_stage6.log"
#define NET_HAL_LOG_OLD_PATH  "/dev_hdd0/tmp/mono_stage6.log.old"
#define NET_HAL_LOG_MAX_BYTES (2u * 1024u * 1024u)
#define NET_HAL_SLICE_MS      250
#define NET_HAL_DNS_TTL_MS    60000ULL
#define NET_HAL_DNS_CACHE_SZ  8
#define NET_HAL_POLL_MS       200ULL

typedef struct {
    bool           valid;
    char           host[128];
    struct in_addr addr;
    uint64_t       expires_ms;
} DnsCacheEntry;

/* ── Global state ── */
static sys_mutex_t s_state_mutex;
static bool        s_state_mutex_inited = false;
static sys_mutex_t s_dns_mutex;
static bool        s_dns_mutex_inited = false;
static sys_mutex_t s_log_mutex;
static bool        s_log_mutex_inited = false;

static int         s_init_refcount = 0;
static bool        s_mod_net_loaded = false;
static bool        s_mod_netctl_loaded = false;
static bool        s_net_inited = false;
static bool        s_netctl_inited = false;
static volatile bool s_menu_paused = false;
static uint64_t    s_last_poll_ms = 0;

static NetHalTelemetry s_telemetry;
static DnsCacheEntry   s_dns_cache[NET_HAL_DNS_CACHE_SZ];

/* ── Unified buffered logger state ── */
static FILE    *s_log_fp = NULL;
static char     s_log_io_buf[4096];
static size_t   s_log_bytes = 0;
static uint32_t s_log_unflushed = 0;
static uint64_t s_log_last_flush_ms = 0;

static void init_mutex(sys_mutex_t *m, bool *inited_flag)
{
    if (*inited_flag) return;
    sys_mutex_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
    attr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    if (sysMutexCreate(m, &attr) == 0) {
        *inited_flag = true;
    }
}

static int read_net_errno(void)
{
#ifdef net_errno
    int ne = net_errno;
    if (ne != 0) return ne;
#endif
    return errno;
}

static int read_h_errno(void)
{
#ifdef net_h_errno
    int he = net_h_errno;
    if (he != 0) return he;
#endif
    return h_errno;
}

static void invalidate_dns_cache_locked(void)
{
    memset(s_dns_cache, 0, sizeof(s_dns_cache));
}

static NetHalState map_netctl_state(s32 ctl_state)
{
    /* PSL1GHT net/netctl.h enum:
     * 0 = Disconnected, 1 = Connecting, 2 = Connected, 3 = IPObtained */
    if (ctl_state == NET_CTL_STATE_IPObtained) {
        return NET_HAL_IP_OBTAINED;
    }
    if (ctl_state == 1 || ctl_state == 2) {
        return NET_HAL_CONNECTING;
    }
    return NET_HAL_DISCONNECTED;
}

static NetHalState refresh_link_state_locked(bool force)
{
    if (!s_netctl_inited) {
        return s_telemetry.state;
    }

    uint64_t now = net_hal_time_ms();
    if (!force && s_last_poll_ms != 0 && (now - s_last_poll_ms) < NET_HAL_POLL_MS) {
        return s_telemetry.state;
    }
    s_last_poll_ms = now;

    s32 ctl_state = 0;
    s32 rc = netCtlGetState(&ctl_state);
    NetHalState prev = s_telemetry.state;
    NetHalState next = (rc == 0) ? map_netctl_state(ctl_state) : NET_HAL_ERROR;
    s_telemetry.state = next;

    if (next == NET_HAL_IP_OBTAINED && s_telemetry.ip_str[0] == '\0') {
        snprintf(s_telemetry.ip_str, sizeof(s_telemetry.ip_str), "IP Obtained");
    } else if (next != NET_HAL_IP_OBTAINED) {
        s_telemetry.ip_str[0] = '\0';
    }

    if (prev == NET_HAL_IP_OBTAINED && next != NET_HAL_IP_OBTAINED) {
        if (s_dns_mutex_inited) {
            sysMutexLock(s_dns_mutex, 0);
            invalidate_dns_cache_locked();
            sysMutexUnlock(s_dns_mutex);
        }
    }
    return next;
}

static int set_socket_nonblocking(int sock)
{
    int nb = 1;
#ifdef SO_NBIO
    if (setsockopt(sock, SOL_SOCKET, SO_NBIO, &nb, sizeof(nb)) == 0) {
        return 0;
    }
#endif
#ifdef O_NONBLOCK
    {
        int flags = fcntl(sock, F_GETFL, 0);
        if (flags >= 0 && fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0) {
            return 0;
        }
    }
#endif
    (void)nb;
    return -1;
}

/* ── Clock ── */

uint64_t net_hal_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

/* ── Unified Logger ── */

static bool tag_needs_immediate_flush(const char *tag)
{
    if (!tag) return false;
    return (strstr(tag, "ERR") != NULL ||
            strstr(tag, "FATAL") != NULL ||
            strstr(tag, "WARN") != NULL ||
            strstr(tag, "EXIT") != NULL ||
            strstr(tag, "SHUTDOWN") != NULL ||
            strstr(tag, "CLEANUP") != NULL);
}

static void open_log_if_needed_locked(void)
{
    if (s_log_fp) return;

    struct stat st;
    if (stat(NET_HAL_LOG_PATH, &st) == 0) {
        if ((size_t)st.st_size >= NET_HAL_LOG_MAX_BYTES) {
            remove(NET_HAL_LOG_OLD_PATH);
            rename(NET_HAL_LOG_PATH, NET_HAL_LOG_OLD_PATH);
            s_log_bytes = 0;
        } else {
            s_log_bytes = (size_t)st.st_size;
        }
    } else {
        s_log_bytes = 0;
    }

    s_log_fp = fopen(NET_HAL_LOG_PATH, "a");
    if (s_log_fp) {
        setvbuf(s_log_fp, s_log_io_buf, _IOFBF, sizeof(s_log_io_buf));
    }
}

void net_hal_log(const char *tag, const char *fmt, ...)
{
    if (!fmt) return;

    char msg[896];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char line[1024];
    unsigned long ts = (unsigned long)time(NULL);
    const char *safe_tag = (tag && tag[0]) ? tag : "LOG";
    int n = snprintf(line, sizeof(line), "[%lu][%s] %s\n", ts, safe_tag, msg);
    if (n <= 0) return;
    size_t write_len = ((size_t)n >= sizeof(line)) ? (sizeof(line) - 1) : (size_t)n;

    if (s_log_mutex_inited) {
        sysMutexLock(s_log_mutex, 0);
    }

    open_log_if_needed_locked();
    if (s_log_fp) {
        size_t written = fwrite(line, 1, write_len, s_log_fp);
        s_log_bytes += written;
        s_log_unflushed++;

        uint64_t now = net_hal_time_ms();
        if (tag_needs_immediate_flush(safe_tag) ||
            s_log_unflushed >= 16 ||
            (now - s_log_last_flush_ms) >= 2000ULL) {
            fflush(s_log_fp);
            s_log_unflushed = 0;
            s_log_last_flush_ms = now;
        }

        if (s_log_bytes >= NET_HAL_LOG_MAX_BYTES) {
            fflush(s_log_fp);
            fclose(s_log_fp);
            s_log_fp = NULL;
            s_log_bytes = 0;
            s_log_unflushed = 0;
            remove(NET_HAL_LOG_OLD_PATH);
            rename(NET_HAL_LOG_PATH, NET_HAL_LOG_OLD_PATH);
        }
    }

    if (s_log_mutex_inited) {
        sysMutexUnlock(s_log_mutex);
    }
}

/* ── Lifecycle & Link State ── */

int net_hal_init(void)
{
    init_mutex(&s_log_mutex, &s_log_mutex_inited);
    init_mutex(&s_state_mutex, &s_state_mutex_inited);
    init_mutex(&s_dns_mutex, &s_dns_mutex_inited);

    if (s_state_mutex_inited) {
        sysMutexLock(s_state_mutex, 0);
    }

    if (s_init_refcount > 0) {
        s_init_refcount++;
        refresh_link_state_locked(true);
        if (s_state_mutex_inited) {
            sysMutexUnlock(s_state_mutex);
        }
        return 0;
    }

    memset(&s_telemetry, 0, sizeof(s_telemetry));
    s_telemetry.state = NET_HAL_DISCONNECTED;
    s_last_poll_ms = 0;

    if (s_dns_mutex_inited) {
        sysMutexLock(s_dns_mutex, 0);
        invalidate_dns_cache_locked();
        sysMutexUnlock(s_dns_mutex);
    }

    int rc = sysModuleLoad(SYSMODULE_NET);
    if (rc != 0) {
        s_telemetry.state = NET_HAL_ERROR;
        s_telemetry.last_net_errno = rc;
        if (s_state_mutex_inited) sysMutexUnlock(s_state_mutex);
        net_hal_log("NET_HAL ERR", "sysModuleLoad(SYSMODULE_NET) failed: 0x%x", rc);
        return -1;
    }
    s_mod_net_loaded = true;

    rc = sysModuleLoad(SYSMODULE_NETCTL);
    if (rc == 0) {
        s_mod_netctl_loaded = true;
    } else {
        net_hal_log("NET_HAL WARN", "sysModuleLoad(SYSMODULE_NETCTL) returned 0x%x", rc);
    }

    rc = netInitialize();
    if (rc < 0) {
        s_telemetry.state = NET_HAL_ERROR;
        s_telemetry.last_net_errno = rc;
        if (s_state_mutex_inited) sysMutexUnlock(s_state_mutex);
        net_hal_log("NET_HAL ERR", "netInitialize() failed: %d", rc);
        return -1;
    }
    s_net_inited = true;

    rc = netCtlInit();
    if (rc < 0) {
        s_telemetry.state = NET_HAL_ERROR;
        s_telemetry.last_net_errno = rc;
        if (s_state_mutex_inited) sysMutexUnlock(s_state_mutex);
        net_hal_log("NET_HAL ERR", "netCtlInit() failed: %d", rc);
        return -1;
    }
    s_netctl_inited = true;

    s_init_refcount = 1;
    NetHalState st = refresh_link_state_locked(true);

    if (s_state_mutex_inited) {
        sysMutexUnlock(s_state_mutex);
    }

    net_hal_log("NET_HAL", "Initialized (libnet + libnetctl), link state=%d", (int)st);
    return 0;
}

void net_hal_shutdown(void)
{
    if (s_state_mutex_inited) {
        sysMutexLock(s_state_mutex, 0);
    }

    if (s_init_refcount <= 0) {
        if (s_state_mutex_inited) sysMutexUnlock(s_state_mutex);
        return;
    }

    s_init_refcount--;
    if (s_init_refcount > 0) {
        if (s_state_mutex_inited) sysMutexUnlock(s_state_mutex);
        return;
    }

    if (s_state_mutex_inited) {
        sysMutexUnlock(s_state_mutex);
    }

    net_hal_log("NET_HAL CLEANUP", "Shutting down network HAL...");

    if (s_netctl_inited) {
        netCtlTerm();
        s_netctl_inited = false;
    }
    if (s_net_inited) {
        netDeinitialize();
        s_net_inited = false;
    }
    if (s_mod_netctl_loaded) {
        sysModuleUnload(SYSMODULE_NETCTL);
        s_mod_netctl_loaded = false;
    }
    if (s_mod_net_loaded) {
        sysModuleUnload(SYSMODULE_NET);
        s_mod_net_loaded = false;
    }

    if (s_log_mutex_inited) {
        sysMutexLock(s_log_mutex, 0);
        if (s_log_fp) {
            fflush(s_log_fp);
            fclose(s_log_fp);
            s_log_fp = NULL;
        }
        s_log_mutex_inited = false;
        sysMutexUnlock(s_log_mutex);
        sysMutexDestroy(s_log_mutex);
    }

    if (s_dns_mutex_inited) {
        s_dns_mutex_inited = false;
        sysMutexDestroy(s_dns_mutex);
    }

    if (s_state_mutex_inited) {
        s_state_mutex_inited = false;
        sysMutexDestroy(s_state_mutex);
    }
}

NetHalState net_hal_get_state(void)
{
    if (!s_state_mutex_inited) {
        return NET_HAL_DISCONNECTED;
    }
    sysMutexLock(s_state_mutex, 0);
    NetHalState st = refresh_link_state_locked(false);
    sysMutexUnlock(s_state_mutex);
    return st;
}

bool net_hal_is_connected(void)
{
    return (net_hal_get_state() == NET_HAL_IP_OBTAINED);
}

void net_hal_set_menu_paused(bool paused)
{
    s_menu_paused = paused;
}

bool net_hal_is_menu_paused(void)
{
    return s_menu_paused;
}

/* ── Thread-Safe DNS with 60s TTL Cache ── */

int net_hal_resolve(const char *host, struct in_addr *out_ip, int *out_h_errno)
{
    if (out_h_errno) *out_h_errno = 0;
    if (!host || !host[0] || !out_ip) {
        if (out_h_errno) *out_h_errno = NO_RECOVERY;
        return -1;
    }

    if (!net_hal_is_connected()) {
        if (out_h_errno) *out_h_errno = TRY_AGAIN;
        return -1;
    }

    /* Fast path for numeric IPv4 addresses */
    struct in_addr literal_addr;
    if (inet_aton(host, &literal_addr) != 0) {
        *out_ip = literal_addr;
        if (s_state_mutex_inited) {
            sysMutexLock(s_state_mutex, 0);
            snprintf(s_telemetry.last_dns_host, sizeof(s_telemetry.last_dns_host), "%s", host);
            snprintf(s_telemetry.last_dns_ip, sizeof(s_telemetry.last_dns_ip), "%s", host);
            s_telemetry.last_h_errno = 0;
            sysMutexUnlock(s_state_mutex);
        }
        return 0;
    }

    if (s_dns_mutex_inited) {
        sysMutexLock(s_dns_mutex, 0);
    }

    uint64_t now = net_hal_time_ms();
    int free_slot = -1;
    int oldest_slot = 0;
    uint64_t oldest_exp = UINT64_MAX;

    for (int i = 0; i < NET_HAL_DNS_CACHE_SZ; i++) {
        if (s_dns_cache[i].valid && now < s_dns_cache[i].expires_ms) {
            if (strcmp(s_dns_cache[i].host, host) == 0) {
                struct in_addr hit = s_dns_cache[i].addr;
                char ip_buf[32];
                snprintf(ip_buf, sizeof(ip_buf), "%s", inet_ntoa(hit));
                if (s_dns_mutex_inited) sysMutexUnlock(s_dns_mutex);

                *out_ip = hit;
                if (s_state_mutex_inited) {
                    sysMutexLock(s_state_mutex, 0);
                    snprintf(s_telemetry.last_dns_host, sizeof(s_telemetry.last_dns_host), "%s", host);
                    snprintf(s_telemetry.last_dns_ip, sizeof(s_telemetry.last_dns_ip), "%s", ip_buf);
                    s_telemetry.last_h_errno = 0;
                    sysMutexUnlock(s_state_mutex);
                }
                return 0;
            }
            if (s_dns_cache[i].expires_ms < oldest_exp) {
                oldest_exp = s_dns_cache[i].expires_ms;
                oldest_slot = i;
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }

    struct hostent *he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        int herr = read_h_errno();
        if (herr == 0) herr = HOST_NOT_FOUND;
        if (s_dns_mutex_inited) sysMutexUnlock(s_dns_mutex);

        if (out_h_errno) *out_h_errno = herr;
        if (s_state_mutex_inited) {
            sysMutexLock(s_state_mutex, 0);
            snprintf(s_telemetry.last_dns_host, sizeof(s_telemetry.last_dns_host), "%s", host);
            s_telemetry.last_dns_ip[0] = '\0';
            s_telemetry.last_h_errno = herr;
            sysMutexUnlock(s_state_mutex);
        }
        net_hal_log("DNS ERR", "gethostbyname(%s) failed: h_errno=%d", host, herr);
        return -1;
    }

    struct in_addr resolved;
    memcpy(&resolved, he->h_addr_list[0], sizeof(struct in_addr));
    char ip_buf[32];
    snprintf(ip_buf, sizeof(ip_buf), "%s", inet_ntoa(resolved));

    int slot = (free_slot >= 0) ? free_slot : oldest_slot;
    s_dns_cache[slot].valid = true;
    snprintf(s_dns_cache[slot].host, sizeof(s_dns_cache[slot].host), "%s", host);
    s_dns_cache[slot].addr = resolved;
    s_dns_cache[slot].expires_ms = net_hal_time_ms() + NET_HAL_DNS_TTL_MS;

    if (s_dns_mutex_inited) {
        sysMutexUnlock(s_dns_mutex);
    }

    *out_ip = resolved;
    if (s_state_mutex_inited) {
        sysMutexLock(s_state_mutex, 0);
        snprintf(s_telemetry.last_dns_host, sizeof(s_telemetry.last_dns_host), "%s", host);
        snprintf(s_telemetry.last_dns_ip, sizeof(s_telemetry.last_dns_ip), "%s", ip_buf);
        s_telemetry.last_h_errno = 0;
        sysMutexUnlock(s_state_mutex);
    }
    return 0;
}

/* ── Non-blocking Socket Operations ── */

int net_hal_connect(const char *host, uint16_t port, int timeout_ms,
                    volatile bool *abort_flag, int *out_sock, int *out_errno)
{
    if (out_sock) *out_sock = -1;
    if (out_errno) *out_errno = 0;

    if (!host || !host[0] || port == 0 || !out_sock) {
        if (out_errno) *out_errno = EINVAL;
        return -1;
    }

    if (abort_flag && *abort_flag) {
        if (out_errno) *out_errno = ECANCELED;
        return -1;
    }

    if (!net_hal_is_connected()) {
        if (out_errno) *out_errno = ENETDOWN;
        return -1;
    }

    struct in_addr addr;
    int herr = 0;
    if (net_hal_resolve(host, &addr, &herr) != 0) {
        if (out_errno) *out_errno = EHOSTUNREACH;
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        int err = read_net_errno();
        if (out_errno) *out_errno = err;
        net_hal_log("NET ERR", "socket() failed: errno=%d", err);
        return -1;
    }

    set_socket_nonblocking(sock);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr = addr;

    if (timeout_ms <= 0) timeout_ms = 10000;
    uint64_t deadline = net_hal_time_ms() + (uint64_t)timeout_ms;

    int rc = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0) {
        int err = read_net_errno();
        if (err != EINPROGRESS && err != EALREADY && err != EWOULDBLOCK) {
            close(sock);
            if (out_errno) *out_errno = err;
            return -1;
        }

        for (;;) {
            if (abort_flag && *abort_flag) {
                close(sock);
                if (out_errno) *out_errno = ECANCELED;
                return -1;
            }
            if (!net_hal_is_connected()) {
                close(sock);
                if (out_errno) *out_errno = ENETDOWN;
                return -1;
            }

            uint64_t now = net_hal_time_ms();
            if (now >= deadline) {
                close(sock);
                if (out_errno) *out_errno = ETIMEDOUT;
                return -1;
            }

            uint64_t rem = deadline - now;
            int slice_ms = (rem > NET_HAL_SLICE_MS) ? NET_HAL_SLICE_MS : (int)rem;
            struct timeval tv;
            tv.tv_sec = slice_ms / 1000;
            tv.tv_usec = (slice_ms % 1000) * 1000;

            fd_set wfds, efds;
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            FD_SET(sock, &wfds);
            FD_SET(sock, &efds);

            int sret = select(sock + 1, NULL, &wfds, &efds, &tv);
            if (sret < 0) {
                err = read_net_errno();
                if (err == EINTR) continue;
                close(sock);
                if (out_errno) *out_errno = err;
                return -1;
            }
            if (sret == 0) {
                continue;
            }

            int so_err = 0;
            socklen_t slen = sizeof(so_err);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &slen) < 0) {
                so_err = read_net_errno();
            }
            if (so_err != 0) {
                close(sock);
                if (out_errno) *out_errno = so_err;
                return -1;
            }
            break;
        }
    }

    /* Record local interface IP if available */
    struct sockaddr_in local_sa;
    socklen_t lslen = sizeof(local_sa);
    if (getsockname(sock, (struct sockaddr *)&local_sa, &lslen) == 0 &&
        local_sa.sin_addr.s_addr != 0) {
        if (s_dns_mutex_inited) sysMutexLock(s_dns_mutex, 0);
        char local_ip[32];
        snprintf(local_ip, sizeof(local_ip), "%s", inet_ntoa(local_sa.sin_addr));
        if (s_dns_mutex_inited) sysMutexUnlock(s_dns_mutex);

        if (s_state_mutex_inited) {
            sysMutexLock(s_state_mutex, 0);
            snprintf(s_telemetry.ip_str, sizeof(s_telemetry.ip_str), "%s", local_ip);
            sysMutexUnlock(s_state_mutex);
        }
    }

    *out_sock = sock;
    return 0;
}

int net_hal_send_all(int sock, const void *buf, size_t len, int timeout_ms,
                     volatile bool *abort_flag, int *out_errno)
{
    if (out_errno) *out_errno = 0;
    if (sock < 0 || (!buf && len > 0)) {
        if (out_errno) *out_errno = EINVAL;
        return -1;
    }
    if (len == 0) return 0;

    if (timeout_ms <= 0) timeout_ms = 10000;
    uint64_t deadline = net_hal_time_ms() + (uint64_t)timeout_ms;

    const uint8_t *p = (const uint8_t *)buf;
    size_t sent_total = 0;

    while (sent_total < len) {
        if (abort_flag && *abort_flag) {
            if (out_errno) *out_errno = ECANCELED;
            return -1;
        }
        if (!net_hal_is_connected()) {
            if (out_errno) *out_errno = ENETDOWN;
            return -1;
        }

        uint64_t now = net_hal_time_ms();
        if (now >= deadline) {
            if (out_errno) *out_errno = ETIMEDOUT;
            return -1;
        }

        uint64_t rem = deadline - now;
        int slice_ms = (rem > NET_HAL_SLICE_MS) ? NET_HAL_SLICE_MS : (int)rem;
        struct timeval tv;
        tv.tv_sec = slice_ms / 1000;
        tv.tv_usec = (slice_ms % 1000) * 1000;

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);

        int sret = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (sret < 0) {
            int err = read_net_errno();
            if (err == EINTR) continue;
            if (out_errno) *out_errno = err;
            return -1;
        }
        if (sret == 0) {
            continue;
        }

        ssize_t n = send(sock, p + sent_total, len - sent_total, 0);
        if (n > 0) {
            sent_total += (size_t)n;
        } else if (n == 0) {
            if (out_errno) *out_errno = EPIPE;
            return -1;
        } else {
            int err = read_net_errno();
            if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK) {
                continue;
            }
            if (out_errno) *out_errno = err;
            return -1;
        }
    }

    return 0;
}

int net_hal_recv_some(int sock, void *buf, size_t len, int timeout_ms,
                      volatile bool *abort_flag, int *out_errno)
{
    if (out_errno) *out_errno = 0;
    if (sock < 0 || (!buf && len > 0)) {
        if (out_errno) *out_errno = EINVAL;
        return -1;
    }
    if (len == 0) return 0;

    if (timeout_ms <= 0) timeout_ms = 10000;
    uint64_t deadline = net_hal_time_ms() + (uint64_t)timeout_ms;

    for (;;) {
        if (abort_flag && *abort_flag) {
            if (out_errno) *out_errno = ECANCELED;
            return -1;
        }
        if (!net_hal_is_connected()) {
            if (out_errno) *out_errno = ENETDOWN;
            return -1;
        }

        uint64_t now = net_hal_time_ms();
        if (now >= deadline) {
            if (out_errno) *out_errno = ETIMEDOUT;
            return -1;
        }

        uint64_t rem = deadline - now;
        int slice_ms = (rem > NET_HAL_SLICE_MS) ? NET_HAL_SLICE_MS : (int)rem;
        struct timeval tv;
        tv.tv_sec = slice_ms / 1000;
        tv.tv_usec = (slice_ms % 1000) * 1000;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);

        int sret = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (sret < 0) {
            int err = read_net_errno();
            if (err == EINTR) continue;
            if (out_errno) *out_errno = err;
            return -1;
        }
        if (sret == 0) {
            continue;
        }

        /* Socket is readable: retry ONLY EINTR; abort on EAGAIN/EWOULDBLOCK/other */
        for (;;) {
            if (abort_flag && *abort_flag) {
                if (out_errno) *out_errno = ECANCELED;
                return -1;
            }
            ssize_t n = recv(sock, buf, len, 0);
            if (n > 0) {
                return (int)n;
            }
            if (n == 0) {
                return 0;
            }
            int err = read_net_errno();
            if (err == EINTR) {
                continue;
            }
            if (out_errno) *out_errno = err;
            return -1;
        }
    }
}

/* ── Sanitization & URL Encoding ── */

bool net_hal_validate_id(const char *id)
{
    if (!id || id[0] == '\0') return false;
    size_t len = 0;
    for (const unsigned char *p = (const unsigned char *)id; *p != '\0'; p++) {
        unsigned char c = *p;
        bool ok = ((c >= 'A' && c <= 'Z') ||
                   (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') ||
                   c == '_' || c == '-');
        if (!ok) return false;
        len++;
        if (len > 63) return false;
    }
    return (len >= 1 && len <= 63);
}

size_t net_hal_url_encode(const char *src, char *dst, size_t dst_sz)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!dst || dst_sz == 0) return 0;
    dst[0] = '\0';
    if (!src) return 0;

    size_t out = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
        unsigned char c = *p;
        bool unreserved = ((c >= 'A' && c <= 'Z') ||
                           (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') ||
                           c == '-' || c == '.' || c == '_' || c == '~');
        if (unreserved) {
            if (out + 1 >= dst_sz) break;
            dst[out++] = (char)c;
        } else {
            if (out + 3 >= dst_sz) break;
            dst[out++] = '%';
            dst[out++] = hex[(c >> 4) & 0x0F];
            dst[out++] = hex[c & 0x0F];
        }
    }
    dst[out] = '\0';
    return out;
}

/* ── Telemetry ── */

void net_hal_record_api_telemetry(int http_code, int net_err, uint32_t latency_ms)
{
    if (!s_state_mutex_inited) return;
    sysMutexLock(s_state_mutex, 0);
    s_telemetry.last_http_code = http_code;
    s_telemetry.last_net_errno = net_err;
    s_telemetry.last_api_latency_ms = latency_ms;
    sysMutexUnlock(s_state_mutex);
}

void net_hal_record_stream_telemetry(uint32_t kbps, uint32_t reconnects, uint32_t underruns)
{
    if (!s_state_mutex_inited) return;
    sysMutexLock(s_state_mutex, 0);
    s_telemetry.stream_kbps = kbps;
    s_telemetry.reconnect_count = reconnects;
    s_telemetry.ring_underruns = underruns;
    sysMutexUnlock(s_state_mutex);
}

void net_hal_get_telemetry(NetHalTelemetry *out)
{
    if (!out) return;
    if (!s_state_mutex_inited) {
        memset(out, 0, sizeof(*out));
        return;
    }
    sysMutexLock(s_state_mutex, 0);
    refresh_link_state_locked(false);
    *out = s_telemetry;
    sysMutexUnlock(s_state_mutex);
}
