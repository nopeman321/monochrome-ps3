/*
 * Monochrome PS3 - Hardened HTTP Client & Async Search Pipeline (PSL1GHT)
 * Uses net_hal for link state, non-blocking sockets, protocol sanitization,
 * unified logging, and API telemetry, plus a single persistent worker thread.
 */

#include <ppu-lv2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include <sys/socket.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/cond.h>

#include "monochrome.h"
#include "net_hal.h"
#include "http_client.h"
#include "cJSON.h"

#define API_HOST               "tracks.monochrome.st"
#define API_PORT               80
#define HTTP_DEFAULT_BUF_SIZE  (256u * 1024u)
#define HTTP_MAX_BUF_SIZE      (512u * 1024u)
#define HTTP_MAX_HEADER_BYTES  (16u * 1024u)
#define HTTP_MAX_REDIRECTS     3
#define HTTP_WORKER_PRIO       1100
#define HTTP_WORKER_STACK      0x20000

#ifndef ECANCELED
#define ECANCELED EINTR
#endif
#ifndef ENETDOWN
#define ENETDOWN EIO
#endif
#ifndef EMSGSIZE
#define EMSGSIZE EINVAL
#endif

typedef struct {
    char  *data;
    size_t cap;
    bool   reusable;
} HttpRecvBuf;

typedef enum {
    ASYNC_REQ_SEARCH = 0,
    ASYNC_REQ_ALBUM,
    ASYNC_REQ_ARTIST
} AsyncReqType;

/* ── Synchronization & Worker State ── */
static sys_mutex_t      s_search_mutex;
static bool             s_search_mutex_inited = false;
static sys_cond_t       s_search_cond;
static bool             s_search_cond_inited = false;
static sys_ppu_thread_t s_search_thread_id;
static bool             s_search_thread_created = false;

static bool             s_net_inited = false;
static bool             s_worker_running = false;
static volatile bool    s_cancel_requested = false;
static int              s_active_sock = -1;

/* Pending request slot (1-deep replace/cancel) */
static bool             s_has_pending_req = false;
static uint32_t         s_pending_seq = 0;
static AsyncReqType     s_pending_req_type = ASYNC_REQ_SEARCH;
static char             s_pending_query[128] = "";
static char             s_pending_release_id[64] = "";
static char             s_pending_album_title[128] = "";
static char             s_pending_artist_id[64] = "";
static char             s_pending_artist_name[128] = "";

/* Published async results (all protected by s_search_mutex) */
static char             s_async_query[128] = "";
static TrackMetadata    s_async_results[MAX_SEARCH_RESULTS];
static int              s_async_result_count = 0;
static AlbumMetadata    s_async_albums[MAX_SEARCH_RESULTS];
static int              s_async_album_count = 0;
static ArtistMetadata   s_async_artists[MAX_SEARCH_RESULTS];
static int              s_async_artist_count = 0;
static PlaylistMetadata s_async_playlists[MAX_SEARCH_RESULTS];
static int              s_async_playlist_count = 0;
static int              s_async_http_code = 0;
static int              s_async_errno = 0;
static SearchStatus     s_async_status = SEARCH_STATUS_IDLE;

/* Worker-owned scratch buffers (avoid large stack frames on 128KB stack) */
static TrackMetadata    s_worker_tracks[MAX_SEARCH_RESULTS];
static AlbumMetadata    s_worker_albums[MAX_SEARCH_RESULTS];
static ArtistMetadata   s_worker_artists[MAX_SEARCH_RESULTS];
static PlaylistMetadata s_worker_playlists[MAX_SEARCH_RESULTS];
static HttpRecvBuf      s_worker_recv_buf = { NULL, 0, true };

static volatile int     g_http_timeout_ms = 30000;
static volatile int     g_http_max_retries = 8;

/* ── Helpers ── */

static void init_sync_primitives(void)
{
    if (!s_search_mutex_inited) {
        sys_mutex_attr_t attr;
        sysMutexAttrInitialize(attr);
        if (sysMutexCreate(&s_search_mutex, &attr) == 0) {
            s_search_mutex_inited = true;
        }
    }
    if (s_search_mutex_inited && !s_search_cond_inited) {
        sys_cond_attr_t cattr;
        sysCondAttrInitialize(cattr);
        if (sysCondCreate(&s_search_cond, s_search_mutex, &cattr) == 0) {
            s_search_cond_inited = true;
        }
    }
}

static void safe_copy_str(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
}

static void set_active_sock(bool track_sock, int sock)
{
    if (!track_sock || !s_search_mutex_inited) return;
    sysMutexLock(s_search_mutex, 0);
    s_active_sock = sock;
    sysMutexUnlock(s_search_mutex);
}

static void close_tracked_sock(bool track_sock, int *sock_ptr)
{
    if (!sock_ptr || *sock_ptr < 0) return;
    int fd = *sock_ptr;
    *sock_ptr = -1;
    set_active_sock(track_sock, -1);
    close(fd);
}

static bool sleep_interruptible_ms(int delay_ms, volatile bool *abort_flag)
{
    int elapsed = 0;
    while (elapsed < delay_ms) {
        if (abort_flag && *abort_flag) return false;
        if (!net_hal_is_connected()) return false;
        int step = (delay_ms - elapsed > 50) ? 50 : (delay_ms - elapsed);
        usleep((useconds_t)step * 1000u);
        elapsed += step;
    }
    return true;
}

static void wait_while_menu_paused(volatile bool *abort_flag)
{
    while (net_hal_is_menu_paused()) {
        if (abort_flag && *abort_flag) break;
        usleep(50000);
    }
}

/* ── HTTP Header & Body Parsing Helpers ── */

static const char *find_header_ci(const char *headers, const char *hdr_end, const char *name)
{
    size_t name_len = strlen(name);
    const char *line = headers;
    while (line < hdr_end) {
        const char *eol = strchr(line, '\n');
        if (!eol || eol > hdr_end) eol = hdr_end;

        if ((size_t)(eol - line) > name_len &&
            strncasecmp(line, name, name_len) == 0 &&
            line[name_len] == ':') {
            const char *val = line + name_len + 1;
            while (val < eol && (*val == ' ' || *val == '\t')) val++;
            return val;
        }
        if (eol >= hdr_end) break;
        line = eol + 1;
    }
    return NULL;
}

static bool parse_content_length(const char *headers, const char *hdr_end, size_t *out_len)
{
    const char *val = find_header_ci(headers, hdr_end, "Content-Length");
    if (!val) return false;

    while (val < hdr_end && (*val == ' ' || *val == '\t')) val++;
    if (val >= hdr_end || !isdigit((unsigned char)*val)) return false;

    size_t len = 0;
    while (val < hdr_end && isdigit((unsigned char)*val)) {
        size_t digit = (size_t)(*val - '0');
        if (len > (SIZE_MAX - digit) / 10u) {
            return false;
        }
        len = len * 10u + digit;
        val++;
    }
    *out_len = len;
    return true;
}

static bool has_chunked_encoding(const char *headers, const char *hdr_end)
{
    const char *val = find_header_ci(headers, hdr_end, "Transfer-Encoding");
    if (!val) return false;

    const char *eol = strchr(val, '\n');
    if (!eol || eol > hdr_end) eol = hdr_end;

    size_t vlen = (size_t)(eol - val);
    char tmp[64];
    if (vlen >= sizeof(tmp)) vlen = sizeof(tmp) - 1;
    memcpy(tmp, val, vlen);
    tmp[vlen] = '\0';

    for (size_t i = 0; i < vlen; i++) {
        tmp[i] = (char)tolower((unsigned char)tmp[i]);
    }
    return (strstr(tmp, "chunked") != NULL);
}

static bool decode_chunked_inplace(char *body, size_t body_len, size_t *out_decoded_len)
{
    char *src = body;
    char *end = body + body_len;
    char *dst = body;

    while (src < end) {
        size_t chunk_sz = 0;
        bool has_hex = false;
        while (src < end) {
            unsigned char c = (unsigned char)*src;
            if (isxdigit(c)) {
                has_hex = true;
                size_t d = (c >= '0' && c <= '9') ? (size_t)(c - '0')
                         : (c >= 'a' && c <= 'f') ? (size_t)(c - 'a' + 10)
                         : (size_t)(c - 'A' + 10);
                if (chunk_sz > (SIZE_MAX - d) / 16u) return false;
                chunk_sz = (chunk_sz << 4) | d;
                src++;
            } else {
                break;
            }
        }
        if (!has_hex) return false;

        /* Skip chunk extensions until CRLF or LF */
        while (src < end && *src != '\n') src++;
        if (src >= end) return false;
        src++; /* past '\n' */

        if (chunk_sz == 0) {
            *dst = '\0';
            *out_decoded_len = (size_t)(dst - body);
            return true;
        }

        if ((size_t)(end - src) < chunk_sz) return false;
        memmove(dst, src, chunk_sz);
        dst += chunk_sz;
        src += chunk_sz;

        if (src < end && *src == '\r') src++;
        if (src < end && *src == '\n') src++;
    }
    return false;
}

static bool parse_redirect_location(const char *headers, const char *hdr_end,
                                    const char *cur_host, uint16_t cur_port,
                                    char *out_host, size_t out_host_sz,
                                    uint16_t *out_port,
                                    char *out_path, size_t out_path_sz)
{
    const char *val = find_header_ci(headers, hdr_end, "Location");
    if (!val) return false;

    const char *eol = strchr(val, '\n');
    if (!eol || eol > hdr_end) eol = hdr_end;
    while (eol > val && (eol[-1] == '\r' || eol[-1] == ' ' || eol[-1] == '\t')) {
        eol--;
    }
    size_t len = (size_t)(eol - val);
    if (len == 0 || len >= 512) return false;

    char loc[512];
    memcpy(loc, val, len);
    loc[len] = '\0';

    /* Reject control characters or whitespace inside URL */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)loc[i];
        if (c <= 0x20 || c == 0x7F) return false;
    }

    if (loc[0] == '/') {
        safe_copy_str(out_host, out_host_sz, cur_host);
        *out_port = cur_port;
        safe_copy_str(out_path, out_path_sz, loc);
        return true;
    }

    if (strncasecmp(loc, "http://", 7) == 0) {
        const char *hp = loc + 7;
        const char *slash = strchr(hp, '/');
        const char *host_end = slash ? slash : (loc + len);
        const char *colon = NULL;
        for (const char *p = hp; p < host_end; p++) {
            if (*p == ':') {
                colon = p;
                break;
            }
        }
        size_t hlen = colon ? (size_t)(colon - hp) : (size_t)(host_end - hp);
        if (hlen == 0 || hlen >= out_host_sz) return false;
        memcpy(out_host, hp, hlen);
        out_host[hlen] = '\0';

        uint16_t port = 80;
        if (colon) {
            unsigned long pval = 0;
            const char *pp = colon + 1;
            if (pp >= host_end) return false;
            while (pp < host_end) {
                if (!isdigit((unsigned char)*pp)) return false;
                pval = pval * 10ul + (unsigned long)(*pp - '0');
                if (pval > 65535ul) return false;
                pp++;
            }
            if (pval == 0) return false;
            port = (uint16_t)pval;
        }
        *out_port = port;
        safe_copy_str(out_path, out_path_sz, slash ? slash : "/");
        return true;
    }

    /* Reject https:// (no TLS backend here) or any non-HTTP scheme */
    return false;
}

static bool ensure_recv_buf(HttpRecvBuf *buf, size_t needed)
{
    if (!buf || needed > HTTP_MAX_BUF_SIZE) return false;
    if (buf->data && buf->cap >= needed) return true;

    size_t new_cap = (buf->cap == 0) ? HTTP_DEFAULT_BUF_SIZE : buf->cap;
    while (new_cap < needed && new_cap < HTTP_MAX_BUF_SIZE) {
        new_cap *= 2u;
    }
    if (new_cap > HTTP_MAX_BUF_SIZE) new_cap = HTTP_MAX_BUF_SIZE;
    if (new_cap < needed) new_cap = needed;

    char *p = (char *)realloc(buf->data, new_cap);
    if (!p) return false;
    buf->data = p;
    buf->cap = new_cap;
    return true;
}

static void release_recv_buf(HttpRecvBuf *buf)
{
    if (!buf || buf->reusable) return;
    free(buf->data);
    buf->data = NULL;
    buf->cap = 0;
}

/* ── Unified HTTP GET with Retries, Redirects & Telemetry ── */

static int http_execute_get(const char *initial_host, uint16_t initial_port,
                            const char *initial_path,
                            HttpRecvBuf *buf,
                            volatile bool *abort_flag,
                            bool track_active_sock,
                            char **out_body, size_t *out_body_len,
                            int *out_http_code, int *out_errno)
{
    if (out_body) *out_body = NULL;
    if (out_body_len) *out_body_len = 0;
    if (out_http_code) *out_http_code = 0;
    if (out_errno) *out_errno = 0;

    if (!ensure_recv_buf(buf, HTTP_DEFAULT_BUF_SIZE)) {
        if (out_errno) *out_errno = ENOMEM;
        return -1;
    }

    int max_retries = http_get_max_retries();
    uint64_t t_total_start = net_hal_time_ms();
    int last_http_code = 0;
    int last_err = 0;

    for (int attempt = 0; attempt < max_retries; attempt++) {
        if (abort_flag && *abort_flag) {
            last_err = ECANCELED;
            break;
        }
        if (!net_hal_is_connected()) {
            last_err = ENETDOWN;
            net_hal_log("HTTP ERR", "Aborted %s: link not connected (state=%d)",
                        initial_path, (int)net_hal_get_state());
            break;
        }
        wait_while_menu_paused(abort_flag);
        if (abort_flag && *abort_flag) {
            last_err = ECANCELED;
            break;
        }

        if (attempt > 0) {
            int shift = (attempt - 1 > 5) ? 5 : (attempt - 1);
            int base_ms = 250 << shift;
            if (base_ms > 8000) base_ms = 8000;
            uint32_t jitter = (uint32_t)((net_hal_time_ms() * 1103515245ULL + 12345ULL) >> 16) % 250u;
            int delay_ms = base_ms + (int)jitter;
            net_hal_log("HTTP", "Retry %d/%d in %d ms for %s",
                        attempt + 1, max_retries, delay_ms, initial_path);
            if (!sleep_interruptible_ms(delay_ms, abort_flag)) {
                last_err = (abort_flag && *abort_flag) ? ECANCELED : ENETDOWN;
                break;
            }
        }

        char cur_host[128];
        uint16_t cur_port = initial_port;
        char cur_path[512];
        safe_copy_str(cur_host, sizeof(cur_host), initial_host);
        safe_copy_str(cur_path, sizeof(cur_path), initial_path);

        bool retry_attempt = false;

        for (int hop = 0; hop <= HTTP_MAX_REDIRECTS; hop++) {
            int sock = -1;
            int timeout_ms = http_get_timeout_ms();
            last_http_code = 0;
            last_err = 0;

            if (net_hal_connect(cur_host, cur_port, timeout_ms, abort_flag, &sock, &last_err) != 0) {
                net_hal_log("HTTP ERR", "connect(%s:%u) failed (errno=%d)", cur_host, (unsigned)cur_port, last_err);
                retry_attempt = true;
                break;
            }

            set_active_sock(track_active_sock, sock);

            char req[768];
            int req_len = snprintf(req, sizeof(req),
                                   "GET %s HTTP/1.0\r\n"
                                   "Host: %s\r\n"
                                   "User-Agent: Monochrome-PS3/2.4 (PSL1GHT; CellOS)\r\n"
                                   "Accept: application/json\r\n"
                                   "Accept-Encoding: identity\r\n"
                                   "Connection: close\r\n\r\n",
                                   cur_path, cur_host);
            if (req_len <= 0 || (size_t)req_len >= sizeof(req)) {
                close_tracked_sock(track_active_sock, &sock);
                last_err = EMSGSIZE;
                retry_attempt = false;
                break;
            }

            if (net_hal_send_all(sock, req, (size_t)req_len, timeout_ms, abort_flag, &last_err) != 0) {
                net_hal_log("HTTP ERR", "send_all(%s) failed (errno=%d)", cur_path, last_err);
                close_tracked_sock(track_active_sock, &sock);
                retry_attempt = true;
                break;
            }

            size_t total_recv = 0;
            char *hdr_end = NULL;
            size_t sep_len = 0;
            bool has_cl = false;
            size_t content_length = 0;
            bool chunked = false;
            int http_code = 0;
            bool recv_failed = false;
            bool non_retryable = false;

            while (1) {
                if (total_recv + 1 >= buf->cap) {
                    if (buf->cap < HTTP_MAX_BUF_SIZE) {
                        if (!ensure_recv_buf(buf, buf->cap * 2u)) {
                            last_err = ENOMEM;
                            recv_failed = true;
                            non_retryable = true;
                            break;
                        }
                    } else {
                        net_hal_log("HTTP ERR", "Response exceeded %u byte cap for %s",
                                    (unsigned)HTTP_MAX_BUF_SIZE, cur_path);
                        last_err = EMSGSIZE;
                        recv_failed = true;
                        non_retryable = true;
                        break;
                    }
                }

                int n = net_hal_recv_some(sock, buf->data + total_recv,
                                          buf->cap - total_recv - 1u,
                                          timeout_ms, abort_flag, &last_err);
                if (n < 0) {
                    net_hal_log("HTTP ERR", "recv_some(%s) failed (errno=%d)", cur_path, last_err);
                    recv_failed = true;
                    break;
                }
                if (n == 0) {
                    break;
                }

                total_recv += (size_t)n;
                buf->data[total_recv] = '\0';

                if (!hdr_end) {
                    char *sep = strstr(buf->data, "\r\n\r\n");
                    if (sep) {
                        hdr_end = sep;
                        sep_len = 4;
                    } else {
                        sep = strstr(buf->data, "\n\n");
                        if (sep) {
                            hdr_end = sep;
                            sep_len = 2;
                        }
                    }

                    if (!hdr_end) {
                        if (total_recv > HTTP_MAX_HEADER_BYTES) {
                            net_hal_log("HTTP ERR", "HTTP headers exceeded %u bytes", (unsigned)HTTP_MAX_HEADER_BYTES);
                            last_err = EMSGSIZE;
                            recv_failed = true;
                            non_retryable = true;
                            break;
                        }
                        continue;
                    }

                    if (sscanf(buf->data, "HTTP/%*s %d", &http_code) != 1) {
                        http_code = 0;
                    }
                    last_http_code = http_code;

                    has_cl = parse_content_length(buf->data, hdr_end, &content_length);
                    chunked = has_chunked_encoding(buf->data, hdr_end);

                    if (has_cl) {
                        size_t hdr_total = (size_t)(hdr_end - buf->data) + sep_len;
                        if (content_length > HTTP_MAX_BUF_SIZE - hdr_total - 1u) {
                            net_hal_log("HTTP ERR", "Content-Length %lu exceeds cap (%u)",
                                        (unsigned long)content_length, (unsigned)HTTP_MAX_BUF_SIZE);
                            last_err = EMSGSIZE;
                            recv_failed = true;
                            non_retryable = true;
                            break;
                        }
                        if (!ensure_recv_buf(buf, hdr_total + content_length + 1u)) {
                            last_err = ENOMEM;
                            recv_failed = true;
                            non_retryable = true;
                            break;
                        }
                        /* Re-locate hdr_end in case realloc moved buf->data */
                        hdr_end = buf->data + (hdr_total - sep_len);
                    }
                }

                if (hdr_end && has_cl) {
                    size_t hdr_total = (size_t)(hdr_end - buf->data) + sep_len;
                    if (total_recv >= hdr_total + content_length) {
                        break;
                    }
                }
            }

            close_tracked_sock(track_active_sock, &sock);

            if (recv_failed) {
                retry_attempt = !non_retryable;
                break;
            }

            if (!hdr_end) {
                net_hal_log("HTTP ERR", "Missing HTTP header terminator from %s", cur_host);
                retry_attempt = true;
                break;
            }

            size_t hdr_total = (size_t)(hdr_end - buf->data) + sep_len;
            size_t body_len = (total_recv > hdr_total) ? (total_recv - hdr_total) : 0;
            if (has_cl && body_len > content_length) {
                body_len = content_length;
            }
            char *body = buf->data + hdr_total;
            body[body_len] = '\0';

            if (has_cl && body_len < content_length) {
                net_hal_log("HTTP ERR", "Truncated body: got %lu / %lu bytes",
                            (unsigned long)body_len, (unsigned long)content_length);
                last_err = EIO;
                retry_attempt = true;
                break;
            }

            if (http_code == 301 || http_code == 302 || http_code == 307 || http_code == 308) {
                if (hop >= HTTP_MAX_REDIRECTS) {
                    net_hal_log("HTTP ERR", "Exceeded max redirects (%d)", HTTP_MAX_REDIRECTS);
                    retry_attempt = false;
                    break;
                }
                char next_host[128];
                uint16_t next_port = 80;
                char next_path[512];
                if (!parse_redirect_location(buf->data, hdr_end, cur_host, cur_port,
                                             next_host, sizeof(next_host), &next_port,
                                             next_path, sizeof(next_path))) {
                    net_hal_log("HTTP ERR", "Rejected invalid/HTTPS redirect on HTTP %d", http_code);
                    retry_attempt = false;
                    break;
                }
                safe_copy_str(cur_host, sizeof(cur_host), next_host);
                cur_port = next_port;
                safe_copy_str(cur_path, sizeof(cur_path), next_path);
                net_hal_log("HTTP", "Redirect %d -> http://%s:%u%s",
                            http_code, cur_host, (unsigned)cur_port, cur_path);
                continue;
            }

            if (http_code == 200) {
                if (chunked) {
                    size_t decoded_len = 0;
                    if (!decode_chunked_inplace(body, body_len, &decoded_len)) {
                        net_hal_log("HTTP ERR", "Malformed chunked body from %s", cur_path);
                        last_err = EIO;
                        retry_attempt = true;
                        break;
                    }
                    body_len = decoded_len;
                }

                uint32_t lat_ms = (uint32_t)(net_hal_time_ms() - t_total_start);
                net_hal_record_api_telemetry(http_code, 0, lat_ms);
                if (out_http_code) *out_http_code = http_code;
                if (out_errno) *out_errno = 0;
                if (out_body) *out_body = body;
                if (out_body_len) *out_body_len = body_len;
                return 0;
            }

            /* 429 & 5xx are transient and eligible for backoff retry */
            if (http_code == 429 || (http_code >= 500 && http_code <= 599)) {
                net_hal_log("HTTP ERR", "Transient HTTP %d for %s", http_code, cur_path);
                retry_attempt = true;
                break;
            }

            /* Non-retryable 4xx / invalid status */
            net_hal_log("HTTP ERR", "HTTP %d for %s", http_code, cur_path);
            retry_attempt = false;
            break;
        }

        if (!retry_attempt) {
            break;
        }
    }

    uint32_t lat_ms = (uint32_t)(net_hal_time_ms() - t_total_start);
    net_hal_record_api_telemetry(last_http_code, last_err, lat_ms);
    if (out_http_code) *out_http_code = last_http_code;
    if (out_errno) *out_errno = last_err;
    return -1;
}

static char *find_json_start(char *body)
{
    if (!body) return NULL;
    char *first_brace = strchr(body, '{');
    char *first_bracket = strchr(body, '[');
    if (first_brace && (!first_bracket || first_brace < first_bracket)) return first_brace;
    if (first_bracket) return first_bracket;
    return body;
}

/* ── Artist Name Aggregation Helper ── */

static void parse_artists_to_string(const cJSON *item, const char *fallback, char *out_str, size_t out_max)
{
    if (!out_str || out_max == 0) return;
    out_str[0] = '\0';
    if (!item) {
        if (fallback) safe_copy_str(out_str, out_max, fallback);
        return;
    }

    size_t written = 0;

    cJSON *jnames = cJSON_GetObjectItemCaseSensitive(item, "artistNames");
    if (jnames && cJSON_IsArray(jnames) && cJSON_GetArraySize(jnames) > 0) {
        int count = cJSON_GetArraySize(jnames);
        for (int i = 0; i < count; i++) {
            cJSON *elem = cJSON_GetArrayItem(jnames, i);
            const char *name = NULL;
            if (elem && cJSON_IsString(elem) && elem->valuestring && elem->valuestring[0]) {
                name = elem->valuestring;
            } else if (elem && cJSON_IsObject(elem)) {
                cJSON *subname = cJSON_GetObjectItemCaseSensitive(elem, "name");
                if (subname && cJSON_IsString(subname) && subname->valuestring) {
                    name = subname->valuestring;
                }
            }
            if (name && name[0]) {
                if (written > 0 && written + 2 < out_max) {
                    int n = snprintf(out_str + written, out_max - written, ", ");
                    if (n > 0) written += (size_t)n;
                }
                if (written < out_max) {
                    int n = snprintf(out_str + written, out_max - written, "%s", name);
                    if (n > 0) written += (size_t)n;
                }
            }
        }
    }

    if (out_str[0] == '\0') {
        cJSON *jartists = cJSON_GetObjectItemCaseSensitive(item, "artists");
        if (jartists && cJSON_IsArray(jartists) && cJSON_GetArraySize(jartists) > 0) {
            int count = cJSON_GetArraySize(jartists);
            for (int i = 0; i < count; i++) {
                cJSON *elem = cJSON_GetArrayItem(jartists, i);
                const char *name = NULL;
                if (elem && cJSON_IsObject(elem)) {
                    cJSON *subname = cJSON_GetObjectItemCaseSensitive(elem, "name");
                    if (subname && cJSON_IsString(subname) && subname->valuestring) {
                        name = subname->valuestring;
                    }
                } else if (elem && cJSON_IsString(elem) && elem->valuestring) {
                    name = elem->valuestring;
                }
                if (name && name[0]) {
                    if (written > 0 && written + 2 < out_max) {
                        int n = snprintf(out_str + written, out_max - written, ", ");
                        if (n > 0) written += (size_t)n;
                    }
                    if (written < out_max) {
                        int n = snprintf(out_str + written, out_max - written, "%s", name);
                        if (n > 0) written += (size_t)n;
                    }
                }
            }
        }
    }

    if (out_str[0] == '\0') {
        cJSON *ja = cJSON_GetObjectItemCaseSensitive(item, "artist");
        if (!ja) ja = cJSON_GetObjectItemCaseSensitive(item, "artistName");
        if (ja && cJSON_IsString(ja) && ja->valuestring && ja->valuestring[0]) {
            safe_copy_str(out_str, out_max, ja->valuestring);
        }
    }

    if (out_str[0] == '\0' && fallback && fallback[0]) {
        safe_copy_str(out_str, out_max, fallback);
    }
}

static uint32_t parse_duration_sec(const cJSON *item)
{
    cJSON *jdur = cJSON_GetObjectItemCaseSensitive(item, "duration");
    if (jdur && cJSON_IsNumber(jdur) && jdur->valuedouble > 0.0) {
        return (uint32_t)(jdur->valuedouble / 1000.0);
    }
    return 210;
}

/* ── Internal Request Implementations ── */

static int search_internal(const char *query,
                           TrackMetadata *out_tracks, int max_tracks,
                           AlbumMetadata *out_albums, int max_albums, int *out_album_count,
                           ArtistMetadata *out_artists, int max_artists, int *out_artist_count,
                           PlaylistMetadata *out_playlists, int max_playlists, int *out_playlist_count,
                           HttpRecvBuf *buf, volatile bool *abort_flag, bool track_sock,
                           int *out_http_code, int *out_errno)
{
    if (out_album_count) *out_album_count = 0;
    if (out_artist_count) *out_artist_count = 0;
    if (out_playlist_count) *out_playlist_count = 0;
    if (out_http_code) *out_http_code = 0;
    if (out_errno) *out_errno = 0;

    if (!query || query[0] == '\0') query = "daft";
    net_hal_log("HTTP", "[BUSQUEDA INICIO] Query: '%s'", query);

    char enc_query[384];
    if (net_hal_url_encode(query, enc_query, sizeof(enc_query)) == 0) {
        if (out_errno) *out_errno = EINVAL;
        return -1;
    }

    char path[448];
    snprintf(path, sizeof(path), "/search?q=%s", enc_query);

    uint64_t t_start = net_hal_time_ms();
    char *body = NULL;
    size_t body_len = 0;
    int http_code = 0;

    if (http_execute_get(API_HOST, API_PORT, path, buf, abort_flag, track_sock,
                         &body, &body_len, &http_code, out_errno) != 0) {
        return -1;
    }
    if (out_http_code) *out_http_code = http_code;

    char *json_start = find_json_start(body);
    const char *json_err = NULL;
    cJSON *root = cJSON_ParseWithError(json_start, &json_err);
    if (!root) {
        long err_off = (json_err && json_start) ? (long)(json_err - json_start) : -1L;
        net_hal_log("HTTP ERR", "[JSON ERROR] cJSON_ParseWithError fallo (offset=%ld)", err_off);
        return -1;
    }

    cJSON *j_releases = cJSON_GetObjectItemCaseSensitive(root, "releases");
    cJSON *j_tracks = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    if (!j_tracks) j_tracks = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!j_tracks && cJSON_IsArray(root)) j_tracks = root;

    /* Releases / Albums */
    int album_cnt = 0;
    if (out_albums && max_albums > 0 && j_releases && cJSON_IsArray(j_releases)) {
        int n_rel = cJSON_GetArraySize(j_releases);
        for (int i = 0; i < n_rel && album_cnt < max_albums; i++) {
            cJSON *item = cJSON_GetArrayItem(j_releases, i);
            if (!item) continue;
            AlbumMetadata *alb = &out_albums[album_cnt];
            memset(alb, 0, sizeof(AlbumMetadata));

            cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "releaseId");
            if (jid && cJSON_IsString(jid) && jid->valuestring) {
                safe_copy_str(alb->id, sizeof(alb->id), jid->valuestring);
            }

            cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(item, "title");
            if (jtitle && cJSON_IsString(jtitle) && jtitle->valuestring) {
                safe_copy_str(alb->title, sizeof(alb->title), jtitle->valuestring);
            }

            parse_artists_to_string(item, NULL, alb->artist, sizeof(alb->artist));

            cJSON *jart = cJSON_GetObjectItemCaseSensitive(item, "artwork");
            if (jart && cJSON_IsString(jart) && jart->valuestring) {
                safe_copy_str(alb->cover_url, sizeof(alb->cover_url), jart->valuestring);
            }

            cJSON *jtype = cJSON_GetObjectItemCaseSensitive(item, "releaseType");
            if (jtype && cJSON_IsString(jtype) && jtype->valuestring) {
                safe_copy_str(alb->type, sizeof(alb->type), jtype->valuestring);
            } else {
                safe_copy_str(alb->type, sizeof(alb->type), "ALBUM");
            }

            cJSON *jdate = cJSON_GetObjectItemCaseSensitive(item, "releaseDate");
            if (jdate && cJSON_IsString(jdate) && jdate->valuestring && strlen(jdate->valuestring) >= 4) {
                memcpy(alb->year, jdate->valuestring, 4);
                alb->year[4] = '\0';
            }

            album_cnt++;
        }
    }
    if (out_album_count) *out_album_count = album_cnt;

    /* Artists */
    int artist_cnt = 0;
    cJSON *j_artists = cJSON_GetObjectItemCaseSensitive(root, "artists");
    if (out_artists && max_artists > 0 && j_artists && cJSON_IsArray(j_artists)) {
        int n_art = cJSON_GetArraySize(j_artists);
        for (int i = 0; i < n_art && artist_cnt < max_artists; i++) {
            cJSON *item = cJSON_GetArrayItem(j_artists, i);
            if (!item) continue;
            ArtistMetadata *art = &out_artists[artist_cnt];
            memset(art, 0, sizeof(ArtistMetadata));

            cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "artistId");
            if (jid && cJSON_IsString(jid) && jid->valuestring) {
                safe_copy_str(art->id, sizeof(art->id), jid->valuestring);
            }

            cJSON *jname = cJSON_GetObjectItemCaseSensitive(item, "name");
            if (!jname) jname = cJSON_GetObjectItemCaseSensitive(item, "displayName");
            if (jname && cJSON_IsString(jname) && jname->valuestring) {
                safe_copy_str(art->name, sizeof(art->name), jname->valuestring);
            }

            cJSON *javatar = cJSON_GetObjectItemCaseSensitive(item, "avatar");
            if (javatar && cJSON_IsString(javatar) && javatar->valuestring) {
                safe_copy_str(art->avatar_url, sizeof(art->avatar_url), javatar->valuestring);
            }

            cJSON *jbio = cJSON_GetObjectItemCaseSensitive(item, "bio");
            if (jbio && cJSON_IsString(jbio) && jbio->valuestring) {
                safe_copy_str(art->bio, sizeof(art->bio), jbio->valuestring);
            }

            artist_cnt++;
        }
    }
    if (out_artist_count) *out_artist_count = artist_cnt;

    /* Playlists */
    int playlist_cnt = 0;
    cJSON *j_playlists = cJSON_GetObjectItemCaseSensitive(root, "playlists");
    if (out_playlists && max_playlists > 0 && j_playlists && cJSON_IsArray(j_playlists)) {
        int n_play = cJSON_GetArraySize(j_playlists);
        for (int i = 0; i < n_play && playlist_cnt < max_playlists; i++) {
            cJSON *item = cJSON_GetArrayItem(j_playlists, i);
            if (!item) continue;
            PlaylistMetadata *pl = &out_playlists[playlist_cnt];
            memset(pl, 0, sizeof(PlaylistMetadata));

            cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "playlistId");
            if (jid && cJSON_IsString(jid) && jid->valuestring) {
                safe_copy_str(pl->id, sizeof(pl->id), jid->valuestring);
            }

            cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(item, "title");
            if (jtitle && cJSON_IsString(jtitle) && jtitle->valuestring) {
                safe_copy_str(pl->title, sizeof(pl->title), jtitle->valuestring);
            }

            cJSON *jowner = cJSON_GetObjectItemCaseSensitive(item, "ownerId");
            if (jowner && cJSON_IsString(jowner) && jowner->valuestring) {
                safe_copy_str(pl->curator, sizeof(pl->curator), jowner->valuestring);
            }

            cJSON *jdesc = cJSON_GetObjectItemCaseSensitive(item, "description");
            if (jdesc && cJSON_IsString(jdesc) && jdesc->valuestring) {
                safe_copy_str(pl->description, sizeof(pl->description), jdesc->valuestring);
            }

            cJSON *jtc = cJSON_GetObjectItemCaseSensitive(item, "trackCount");
            if (jtc && cJSON_IsNumber(jtc) && jtc->valueint >= 0) {
                pl->track_count = (uint32_t)jtc->valueint;
            }

            playlist_cnt++;
        }
    }
    if (out_playlist_count) *out_playlist_count = playlist_cnt;

    /* Tracks */
    int parsed_count = 0;
    if (out_tracks && max_tracks > 0 && j_tracks && cJSON_IsArray(j_tracks)) {
        int n = cJSON_GetArraySize(j_tracks);
        for (int i = 0; i < n && parsed_count < max_tracks; i++) {
            cJSON *item = cJSON_GetArrayItem(j_tracks, i);
            if (!item) continue;

            TrackMetadata *t = &out_tracks[parsed_count];
            memset(t, 0, sizeof(TrackMetadata));

            cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "trackId");
            if (jid && cJSON_IsString(jid) && jid->valuestring) {
                safe_copy_str(t->id, sizeof(t->id), jid->valuestring);
            }

            cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(item, "title");
            if (jtitle && cJSON_IsString(jtitle) && jtitle->valuestring) {
                safe_copy_str(t->title, sizeof(t->title), jtitle->valuestring);
            }

            parse_artists_to_string(item, NULL, t->artist, sizeof(t->artist));

            cJSON *jrel_id = cJSON_GetObjectItemCaseSensitive(item, "releaseId");
            if (jrel_id && cJSON_IsString(jrel_id) && jrel_id->valuestring &&
                j_releases && cJSON_IsArray(j_releases)) {
                int n_rel = cJSON_GetArraySize(j_releases);
                for (int r = 0; r < n_rel; r++) {
                    cJSON *rel = cJSON_GetArrayItem(j_releases, r);
                    cJSON *rid = cJSON_GetObjectItemCaseSensitive(rel, "id");
                    if (!rid) rid = cJSON_GetObjectItemCaseSensitive(rel, "releaseId");
                    if (rid && cJSON_IsString(rid) && rid->valuestring &&
                        strcmp(rid->valuestring, jrel_id->valuestring) == 0) {
                        cJSON *rtitle = cJSON_GetObjectItemCaseSensitive(rel, "title");
                        if (rtitle && cJSON_IsString(rtitle) && rtitle->valuestring) {
                            safe_copy_str(t->album, sizeof(t->album), rtitle->valuestring);
                        }
                        break;
                    }
                }
            }

            t->duration_sec = parse_duration_sec(item);

            cJSON *jartwork = cJSON_GetObjectItemCaseSensitive(item, "artwork");
            if (jartwork && cJSON_IsString(jartwork) && jartwork->valuestring) {
                safe_copy_str(t->cover_url, sizeof(t->cover_url), jartwork->valuestring);
            }

            t->sample_rate = 44100;
            t->bit_depth = 16;
            t->is_hi_res = false;
            parsed_count++;
        }
    }

    cJSON_Delete(root);

    uint64_t total_time = net_hal_time_ms() - t_start;
    net_hal_log("HTTP", "[BUSQUEDA RESULTADO] Query: '%s' | HTTP: %d | Resultados: %d | Tiempo: %llu ms",
                query, http_code, parsed_count, (unsigned long long)total_time);

    return parsed_count;
}

static int fetch_album_internal(const char *release_id, const char *album_title,
                                TrackMetadata *out_tracks, int max_tracks,
                                HttpRecvBuf *buf, volatile bool *abort_flag, bool track_sock,
                                int *out_http_code, int *out_errno)
{
    if (out_http_code) *out_http_code = 0;
    if (out_errno) *out_errno = 0;

    if (!net_hal_validate_id(release_id)) {
        net_hal_log("HTTP ERR", "[ALBUM ERROR] Invalid release_id rejected");
        if (out_errno) *out_errno = EINVAL;
        return -1;
    }

    net_hal_log("HTTP", "[ALBUM INICIO] Release ID: '%s', Titulo: '%s'",
                release_id, album_title ? album_title : "");

    char path[128];
    snprintf(path, sizeof(path), "/releases/%s", release_id);

    char *body = NULL;
    size_t body_len = 0;
    int http_code = 0;

    if (http_execute_get(API_HOST, API_PORT, path, buf, abort_flag, track_sock,
                         &body, &body_len, &http_code, out_errno) != 0) {
        return -1;
    }
    if (out_http_code) *out_http_code = http_code;

    char *json_start = find_json_start(body);
    const char *json_err = NULL;
    cJSON *root = cJSON_ParseWithError(json_start, &json_err);
    if (!root) {
        long err_off = (json_err && json_start) ? (long)(json_err - json_start) : -1L;
        net_hal_log("HTTP ERR", "[ALBUM ERROR] cJSON_ParseWithError fallo para release %s (offset=%ld)",
                    release_id, err_off);
        return -1;
    }

    char album_art[256] = "";
    cJSON *j_art = cJSON_GetObjectItemCaseSensitive(root, "artwork");
    if (j_art && cJSON_IsString(j_art) && j_art->valuestring) {
        safe_copy_str(album_art, sizeof(album_art), j_art->valuestring);
    }

    char default_album_title[128] = "";
    cJSON *j_atitle = cJSON_GetObjectItemCaseSensitive(root, "title");
    if (j_atitle && cJSON_IsString(j_atitle) && j_atitle->valuestring) {
        safe_copy_str(default_album_title, sizeof(default_album_title), j_atitle->valuestring);
    } else if (album_title && album_title[0]) {
        safe_copy_str(default_album_title, sizeof(default_album_title), album_title);
    }

    char default_album_artist[128] = "";
    parse_artists_to_string(root, NULL, default_album_artist, sizeof(default_album_artist));

    cJSON *j_tracks = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    int parsed_count = 0;
    if (out_tracks && max_tracks > 0 && j_tracks && cJSON_IsArray(j_tracks)) {
        int n = cJSON_GetArraySize(j_tracks);
        for (int i = 0; i < n && parsed_count < max_tracks; i++) {
            cJSON *item = cJSON_GetArrayItem(j_tracks, i);
            if (!item) continue;
            TrackMetadata *t = &out_tracks[parsed_count];
            memset(t, 0, sizeof(TrackMetadata));

            cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "trackId");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "id");
            if (jid && cJSON_IsString(jid) && jid->valuestring) {
                safe_copy_str(t->id, sizeof(t->id), jid->valuestring);
            }

            cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(item, "title");
            if (jtitle && cJSON_IsString(jtitle) && jtitle->valuestring) {
                safe_copy_str(t->title, sizeof(t->title), jtitle->valuestring);
            }

            parse_artists_to_string(item, default_album_artist, t->artist, sizeof(t->artist));
            safe_copy_str(t->album, sizeof(t->album), default_album_title);

            t->duration_sec = parse_duration_sec(item);

            cJSON *jtart = cJSON_GetObjectItemCaseSensitive(item, "artwork");
            if (jtart && cJSON_IsString(jtart) && jtart->valuestring && jtart->valuestring[0]) {
                safe_copy_str(t->cover_url, sizeof(t->cover_url), jtart->valuestring);
            } else if (album_art[0]) {
                safe_copy_str(t->cover_url, sizeof(t->cover_url), album_art);
            }

            t->sample_rate = 44100;
            t->bit_depth = 16;
            t->is_hi_res = false;
            parsed_count++;
        }
    }

    cJSON_Delete(root);

    net_hal_log("HTTP", "[ALBUM RESULTADO] Release ID: %s | Titulo: \"%s\" | Pistas ordenadas: %d",
                release_id, default_album_title, parsed_count);

    return parsed_count;
}

static int fetch_artist_internal(const char *artist_id, const char *artist_name,
                                 TrackMetadata *out_tracks, int max_tracks,
                                 HttpRecvBuf *buf, volatile bool *abort_flag, bool track_sock,
                                 int *out_http_code, int *out_errno)
{
    if (out_http_code) *out_http_code = 0;
    if (out_errno) *out_errno = 0;

    if (!net_hal_validate_id(artist_id)) {
        net_hal_log("HTTP ERR", "[ARTIST ERROR] Invalid artist_id rejected");
        if (out_errno) *out_errno = EINVAL;
        return -1;
    }

    net_hal_log("HTTP", "[ARTIST INICIO] Artist ID: '%s', Nombre: '%s'",
                artist_id, artist_name ? artist_name : "");

    char path[128];
    snprintf(path, sizeof(path), "/artists/%s", artist_id);

    char *body = NULL;
    size_t body_len = 0;
    int http_code = 0;

    if (http_execute_get(API_HOST, API_PORT, path, buf, abort_flag, track_sock,
                         &body, &body_len, &http_code, out_errno) != 0) {
        return -1;
    }
    if (out_http_code) *out_http_code = http_code;

    char *json_start = find_json_start(body);
    const char *json_err = NULL;
    cJSON *root = cJSON_ParseWithError(json_start, &json_err);
    if (!root) {
        long err_off = (json_err && json_start) ? (long)(json_err - json_start) : -1L;
        net_hal_log("HTTP ERR", "[ARTIST ERROR] cJSON_ParseWithError fallo para artist %s (offset=%ld)",
                    artist_id, err_off);
        return -1;
    }

    char default_artist_name[128] = "";
    cJSON *j_name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (j_name && cJSON_IsString(j_name) && j_name->valuestring) {
        safe_copy_str(default_artist_name, sizeof(default_artist_name), j_name->valuestring);
    } else if (artist_name && artist_name[0]) {
        safe_copy_str(default_artist_name, sizeof(default_artist_name), artist_name);
    }

    cJSON *j_tracks = cJSON_GetObjectItemCaseSensitive(root, "topTracks");
    int parsed_count = 0;
    if (out_tracks && max_tracks > 0 && j_tracks && cJSON_IsArray(j_tracks)) {
        int n = cJSON_GetArraySize(j_tracks);
        for (int i = 0; i < n && parsed_count < max_tracks; i++) {
            cJSON *item = cJSON_GetArrayItem(j_tracks, i);
            if (!item) continue;
            TrackMetadata *t = &out_tracks[parsed_count];
            memset(t, 0, sizeof(TrackMetadata));

            cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "trackId");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "id");
            if (jid) {
                if (cJSON_IsString(jid) && jid->valuestring) {
                    safe_copy_str(t->id, sizeof(t->id), jid->valuestring);
                } else if (cJSON_IsNumber(jid)) {
                    snprintf(t->id, sizeof(t->id), "%.0f", jid->valuedouble);
                }
            }

            cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(item, "title");
            if (jtitle && cJSON_IsString(jtitle) && jtitle->valuestring) {
                safe_copy_str(t->title, sizeof(t->title), jtitle->valuestring);
            }

            parse_artists_to_string(item, default_artist_name, t->artist, sizeof(t->artist));
            safe_copy_str(t->album, sizeof(t->album), "Top Tracks");

            t->duration_sec = parse_duration_sec(item);

            cJSON *jtart = cJSON_GetObjectItemCaseSensitive(item, "artwork");
            if (jtart && cJSON_IsString(jtart) && jtart->valuestring && jtart->valuestring[0]) {
                safe_copy_str(t->cover_url, sizeof(t->cover_url), jtart->valuestring);
            }

            t->sample_rate = 44100;
            t->bit_depth = 16;
            t->is_hi_res = false;
            parsed_count++;
        }
    }

    cJSON_Delete(root);

    net_hal_log("HTTP", "[ARTIST RESULTADO] Artist ID: %s | Nombre: \"%s\" | Top tracks: %d",
                artist_id, default_artist_name, parsed_count);

    return parsed_count;
}

/* ── Persistent Search Worker Thread ── */

static void search_worker_func(void *arg)
{
    (void)arg;

    for (;;) {
        sysMutexLock(s_search_mutex, 0);
        while (s_worker_running && !s_has_pending_req) {
            sysCondWait(s_search_cond, 0);
        }
        if (!s_worker_running) {
            sysMutexUnlock(s_search_mutex);
            break;
        }

        AsyncReqType req_type = s_pending_req_type;
        uint32_t my_seq = s_pending_seq;
        char query[128];
        char release_id[64];
        char album_title[128];
        char artist_id[64];
        char artist_name[128];

        safe_copy_str(query, sizeof(query), s_pending_query);
        safe_copy_str(release_id, sizeof(release_id), s_pending_release_id);
        safe_copy_str(album_title, sizeof(album_title), s_pending_album_title);
        safe_copy_str(artist_id, sizeof(artist_id), s_pending_artist_id);
        safe_copy_str(artist_name, sizeof(artist_name), s_pending_artist_name);

        s_has_pending_req = false;
        s_cancel_requested = false;
        sysMutexUnlock(s_search_mutex);

        int code = 0;
        int err = 0;
        int count = 0;
        int alb_cnt = 0;
        int art_cnt = 0;
        int pl_cnt = 0;

        if (req_type == ASYNC_REQ_ALBUM) {
            count = fetch_album_internal(release_id, album_title,
                                         s_worker_tracks, MAX_SEARCH_RESULTS,
                                         &s_worker_recv_buf, &s_cancel_requested, true,
                                         &code, &err);
        } else if (req_type == ASYNC_REQ_ARTIST) {
            count = fetch_artist_internal(artist_id, artist_name,
                                          s_worker_tracks, MAX_SEARCH_RESULTS,
                                          &s_worker_recv_buf, &s_cancel_requested, true,
                                          &code, &err);
        } else {
            count = search_internal(query,
                                    s_worker_tracks, MAX_SEARCH_RESULTS,
                                    s_worker_albums, MAX_SEARCH_RESULTS, &alb_cnt,
                                    s_worker_artists, MAX_SEARCH_RESULTS, &art_cnt,
                                    s_worker_playlists, MAX_SEARCH_RESULTS, &pl_cnt,
                                    &s_worker_recv_buf, &s_cancel_requested, true,
                                    &code, &err);
        }

        sysMutexLock(s_search_mutex, 0);
        if (!s_worker_running) {
            sysMutexUnlock(s_search_mutex);
            break;
        }
        /* Drop stale results if superseded/cancelled by a newer request */
        if (s_has_pending_req || s_cancel_requested || my_seq != s_pending_seq) {
            sysMutexUnlock(s_search_mutex);
            continue;
        }

        s_async_http_code = code;
        s_async_errno = err;

        if (req_type == ASYNC_REQ_SEARCH) {
            s_async_album_count = alb_cnt;
            if (alb_cnt > 0) {
                memcpy(s_async_albums, s_worker_albums, (size_t)alb_cnt * sizeof(AlbumMetadata));
            }
            s_async_artist_count = art_cnt;
            if (art_cnt > 0) {
                memcpy(s_async_artists, s_worker_artists, (size_t)art_cnt * sizeof(ArtistMetadata));
            }
            s_async_playlist_count = pl_cnt;
            if (pl_cnt > 0) {
                memcpy(s_async_playlists, s_worker_playlists, (size_t)pl_cnt * sizeof(PlaylistMetadata));
            }
        }

        if (count > 0) {
            s_async_result_count = count;
            memcpy(s_async_results, s_worker_tracks, (size_t)count * sizeof(TrackMetadata));
            s_async_status = SEARCH_STATUS_OK;
        } else if (count == 0) {
            s_async_result_count = 0;
            s_async_status = SEARCH_STATUS_NO_RESULTS;
        } else {
            s_async_result_count = 0;
            s_async_status = SEARCH_STATUS_ERROR;
        }
        sysMutexUnlock(s_search_mutex);
    }

    sysThreadExit(0);
}

/* ── Public Lifecycle API ── */

int http_net_init(void)
{
    init_sync_primitives();

    if (s_search_mutex_inited) sysMutexLock(s_search_mutex, 0);
    if (s_net_inited) {
        if (s_search_mutex_inited) sysMutexUnlock(s_search_mutex);
        return 0;
    }
    if (s_search_mutex_inited) sysMutexUnlock(s_search_mutex);

    if (net_hal_init() != 0) {
        net_hal_log("HTTP ERR", "[NET] net_hal_init() failed");
        return -1;
    }

    if (s_search_mutex_inited) sysMutexLock(s_search_mutex, 0);
    s_net_inited = true;
    s_worker_running = true;
    s_cancel_requested = false;
    s_has_pending_req = false;
    s_active_sock = -1;
    s_async_status = SEARCH_STATUS_IDLE;

    if (!s_search_thread_created) {
        int ret = sysThreadCreate(&s_search_thread_id, search_worker_func, NULL,
                                  HTTP_WORKER_PRIO, HTTP_WORKER_STACK,
                                  THREAD_JOINABLE, "mono_search");
        if (ret == 0) {
            s_search_thread_created = true;
        } else {
            s_worker_running = false;
            net_hal_log("HTTP ERR", "[ERROR] sysThreadCreate(mono_search) fallo: %d", ret);
            if (s_search_mutex_inited) sysMutexUnlock(s_search_mutex);
            net_hal_shutdown();
            return -1;
        }
    }
    if (s_search_mutex_inited) sysMutexUnlock(s_search_mutex);

    net_hal_log("HTTP", "[NET] HTTP client + persistent mono_search worker initialized (prio %d)",
                HTTP_WORKER_PRIO);
    return 0;
}

void http_net_shutdown(void)
{
    if (!s_search_mutex_inited) return;

    sysMutexLock(s_search_mutex, 0);
    if (!s_net_inited && !s_search_thread_created) {
        sysMutexUnlock(s_search_mutex);
        return;
    }

    s_net_inited = false;
    s_worker_running = false;
    s_cancel_requested = true;
    s_has_pending_req = false;
    s_pending_seq++;

    if (s_active_sock >= 0) {
        shutdown(s_active_sock, SHUT_RDWR);
    }
    if (s_search_cond_inited) {
        sysCondBroadcast(s_search_cond);
    }
    sysMutexUnlock(s_search_mutex);

    if (s_search_thread_created) {
        uint64_t exit_val = 0;
        sysThreadJoin(s_search_thread_id, &exit_val);
        s_search_thread_created = false;
    }

    free(s_worker_recv_buf.data);
    s_worker_recv_buf.data = NULL;
    s_worker_recv_buf.cap = 0;

    net_hal_log("HTTP SHUTDOWN", "[NET] HTTP client + mono_search worker shut down cleanly");
    net_hal_shutdown();

    if (s_search_cond_inited) {
        sysCondDestroy(s_search_cond);
        s_search_cond_inited = false;
    }
    if (s_search_mutex_inited) {
        s_search_mutex_inited = false;
        sysMutexDestroy(s_search_mutex);
    }
}

/* ── Public Synchronous API ── */

int http_search_sync(const char *query, TrackMetadata *out_tracks, int max_tracks,
                     int *out_http_code, int *out_errno)
{
    init_sync_primitives();
    HttpRecvBuf tmp_buf = { NULL, 0, false };
    AlbumMetadata tmp_albums[MAX_SEARCH_RESULTS];
    ArtistMetadata tmp_artists[MAX_SEARCH_RESULTS];
    PlaylistMetadata tmp_playlists[MAX_SEARCH_RESULTS];
    int alb_cnt = 0, art_cnt = 0, pl_cnt = 0;
    volatile bool no_abort = false;

    int rc = search_internal(query, out_tracks, max_tracks,
                             tmp_albums, MAX_SEARCH_RESULTS, &alb_cnt,
                             tmp_artists, MAX_SEARCH_RESULTS, &art_cnt,
                             tmp_playlists, MAX_SEARCH_RESULTS, &pl_cnt,
                             &tmp_buf, &no_abort, false,
                             out_http_code, out_errno);
    release_recv_buf(&tmp_buf);

    if (rc >= 0 && s_search_mutex_inited) {
        sysMutexLock(s_search_mutex, 0);
        s_async_album_count = alb_cnt;
        if (alb_cnt > 0) memcpy(s_async_albums, tmp_albums, (size_t)alb_cnt * sizeof(AlbumMetadata));
        s_async_artist_count = art_cnt;
        if (art_cnt > 0) memcpy(s_async_artists, tmp_artists, (size_t)art_cnt * sizeof(ArtistMetadata));
        s_async_playlist_count = pl_cnt;
        if (pl_cnt > 0) memcpy(s_async_playlists, tmp_playlists, (size_t)pl_cnt * sizeof(PlaylistMetadata));
        sysMutexUnlock(s_search_mutex);
    }
    return rc;
}

int http_fetch_album_tracks_sync(const char *release_id, const char *album_title,
                                 TrackMetadata *out_tracks, int max_tracks,
                                 int *out_http_code, int *out_errno)
{
    HttpRecvBuf tmp_buf = { NULL, 0, false };
    volatile bool no_abort = false;
    int rc = fetch_album_internal(release_id, album_title, out_tracks, max_tracks,
                                  &tmp_buf, &no_abort, false, out_http_code, out_errno);
    release_recv_buf(&tmp_buf);
    return rc;
}

int http_fetch_artist_tracks_sync(const char *artist_id, const char *artist_name,
                                  TrackMetadata *out_tracks, int max_tracks,
                                  int *out_http_code, int *out_errno)
{
    HttpRecvBuf tmp_buf = { NULL, 0, false };
    volatile bool no_abort = false;
    int rc = fetch_artist_internal(artist_id, artist_name, out_tracks, max_tracks,
                                   &tmp_buf, &no_abort, false, out_http_code, out_errno);
    release_recv_buf(&tmp_buf);
    return rc;
}

/* ── Public Asynchronous Queue/Replace API ── */

static void enqueue_async_request(AsyncReqType type,
                                  const char *query,
                                  const char *release_id, const char *album_title,
                                  const char *artist_id, const char *artist_name)
{
    if (!s_net_inited) {
        if (http_net_init() != 0) {
            if (s_search_mutex_inited) {
                sysMutexLock(s_search_mutex, 0);
                s_async_status = SEARCH_STATUS_ERROR;
                sysMutexUnlock(s_search_mutex);
            }
            return;
        }
    }

    sysMutexLock(s_search_mutex, 0);
    s_pending_req_type = type;
    s_pending_seq++;
    s_has_pending_req = true;
    s_cancel_requested = true; /* Cooperatively cancel any in-flight request */

    if (type == ASYNC_REQ_SEARCH) {
        const char *q = (query && query[0]) ? query : "daft";
        safe_copy_str(s_pending_query, sizeof(s_pending_query), q);
        safe_copy_str(s_async_query, sizeof(s_async_query), q);
    } else if (type == ASYNC_REQ_ALBUM) {
        safe_copy_str(s_pending_release_id, sizeof(s_pending_release_id), release_id);
        safe_copy_str(s_pending_album_title, sizeof(s_pending_album_title), album_title);
    } else if (type == ASYNC_REQ_ARTIST) {
        safe_copy_str(s_pending_artist_id, sizeof(s_pending_artist_id), artist_id);
        safe_copy_str(s_pending_artist_name, sizeof(s_pending_artist_name), artist_name);
    }

    s_async_result_count = 0;
    s_async_http_code = 0;
    s_async_errno = 0;
    s_async_status = SEARCH_STATUS_BUSY;

    if (s_active_sock >= 0) {
        shutdown(s_active_sock, SHUT_RDWR);
    }
    if (s_search_cond_inited) {
        sysCondSignal(s_search_cond);
    }
    sysMutexUnlock(s_search_mutex);
}

void http_search_async_start(const char *query)
{
    enqueue_async_request(ASYNC_REQ_SEARCH, query, NULL, NULL, NULL, NULL);
}

void http_fetch_album_async_start(const char *release_id, const char *album_title)
{
    enqueue_async_request(ASYNC_REQ_ALBUM, NULL, release_id, album_title, NULL, NULL);
}

void http_fetch_artist_async_start(const char *artist_id, const char *artist_name)
{
    enqueue_async_request(ASYNC_REQ_ARTIST, NULL, NULL, NULL, artist_id, artist_name);
}

void http_search_async_cancel(void)
{
    if (!s_search_mutex_inited) return;
    sysMutexLock(s_search_mutex, 0);
    s_has_pending_req = false;
    s_cancel_requested = true;
    s_pending_seq++;
    if (s_active_sock >= 0) {
        shutdown(s_active_sock, SHUT_RDWR);
    }
    if (s_async_status == SEARCH_STATUS_BUSY) {
        s_async_status = SEARCH_STATUS_IDLE;
    }
    sysMutexUnlock(s_search_mutex);
}

SearchStatus http_search_async_poll(TrackMetadata *out_tracks, int *out_count, int max_tracks,
                                    int *out_http_code, int *out_errno)
{
    if (!s_search_mutex_inited) {
        if (out_http_code) *out_http_code = 0;
        if (out_errno) *out_errno = 0;
        if (out_count) *out_count = 0;
        return SEARCH_STATUS_IDLE;
    }

    sysMutexLock(s_search_mutex, 0);
    if (out_http_code) *out_http_code = s_async_http_code;
    if (out_errno) *out_errno = s_async_errno;

    SearchStatus st = s_async_status;
    if (st == SEARCH_STATUS_OK) {
        int clamped_max = (max_tracks > 0) ? max_tracks : 0;
        int to_copy = (s_async_result_count < clamped_max) ? s_async_result_count : clamped_max;
        if (to_copy < 0) to_copy = 0;
        if (out_tracks && to_copy > 0) {
            memcpy(out_tracks, s_async_results, (size_t)to_copy * sizeof(TrackMetadata));
        }
        if (out_count) *out_count = to_copy;
        s_async_status = SEARCH_STATUS_IDLE;
    }
    sysMutexUnlock(s_search_mutex);

    return st;
}

const char *http_search_get_active_query(void)
{
    return s_async_query;
}

int http_search_get_albums(AlbumMetadata *out_albums, int max_albums)
{
    if (!s_search_mutex_inited || max_albums <= 0) return 0;
    sysMutexLock(s_search_mutex, 0);
    int n = (s_async_album_count < max_albums) ? s_async_album_count : max_albums;
    if (n < 0) n = 0;
    if (out_albums && n > 0) {
        memcpy(out_albums, s_async_albums, (size_t)n * sizeof(AlbumMetadata));
    }
    sysMutexUnlock(s_search_mutex);
    return n;
}

int http_search_get_artists(ArtistMetadata *out_artists, int max_artists)
{
    if (!s_search_mutex_inited || max_artists <= 0) return 0;
    sysMutexLock(s_search_mutex, 0);
    int n = (s_async_artist_count < max_artists) ? s_async_artist_count : max_artists;
    if (n < 0) n = 0;
    if (out_artists && n > 0) {
        memcpy(out_artists, s_async_artists, (size_t)n * sizeof(ArtistMetadata));
    }
    sysMutexUnlock(s_search_mutex);
    return n;
}

int http_search_get_playlists(PlaylistMetadata *out_playlists, int max_playlists)
{
    if (!s_search_mutex_inited || max_playlists <= 0) return 0;
    sysMutexLock(s_search_mutex, 0);
    int n = (s_async_playlist_count < max_playlists) ? s_async_playlist_count : max_playlists;
    if (n < 0) n = 0;
    if (out_playlists && n > 0) {
        memcpy(out_playlists, s_async_playlists, (size_t)n * sizeof(PlaylistMetadata));
    }
    sysMutexUnlock(s_search_mutex);
    return n;
}

int http_get_timeout_ms(void)
{
    return g_http_timeout_ms;
}

void http_set_timeout_ms(int timeout_ms)
{
    if (timeout_ms < 5000) timeout_ms = 5000;
    if (timeout_ms > 120000) timeout_ms = 120000;
    g_http_timeout_ms = timeout_ms;
}

int http_get_max_retries(void)
{
    return g_http_max_retries;
}

void http_set_max_retries(int max_retries)
{
    if (max_retries < 1) max_retries = 1;
    if (max_retries > 20) max_retries = 20;
    g_http_max_retries = max_retries;
}
