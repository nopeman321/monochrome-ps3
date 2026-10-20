/**
 * Monochrome PS3 — Real HTTP/1.0 client over GameOS sys_net syscalls.
 * Replaces the former "fallback catalogue" stubs.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "platform.h"

extern int ps3_sys_net_socket(int domain, int type, int protocol);
extern int ps3_sys_net_connect(int s, const void *addr, uint32_t addrlen);
extern int ps3_sys_net_sendto(int s, const void *buf, uint32_t len, int flags, const void *addr, uint32_t addrlen);
extern int ps3_sys_net_recvfrom(int s, void *buf, uint32_t len, int flags, void *addr, uint32_t *addrlen);
extern int ps3_sys_net_close(int s);
extern int ps3_sys_net_setsockopt(int s, int level, int optname, const void *optval, uint32_t optlen);

#define NET_AF_INET        2
#define NET_SOCK_STREAM    1
#define NET_SOCK_DGRAM     2
#define NET_SOL_SOCKET     0xffff
#define NET_SO_RCVTIMEO    0x1006

typedef struct {
    uint8_t  sin_len;
    uint8_t  sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint64_t sin_zero;
} NetSockaddrIn;

typedef struct { int64_t tv_sec; int64_t tv_usec; } NetTimeval;

#define HTTP_HDR_MAX 8192

static int net_eagain(int r) { return r == -35 || r == 35; }

static int recv_some(int s, void *buf, size_t len)
{
    int spins = 0;
    for (;;) {
        int r = ps3_sys_net_recvfrom(s, buf, (uint32_t)len, 0, NULL, NULL);
        if (r > 0) return r;
        if (r == 0) return 0;
        if (net_eagain(r)) {
            if (++spins > 10000) return -1;
            platform_sleep_ms(2);
            continue;
        }
        return -1;
    }
}

static int send_all(int s, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        int r = ps3_sys_net_sendto(s, p + sent, (uint32_t)(len - sent), 0, NULL, 0);
        if (r > 0) { sent += (size_t)r; continue; }
        if (net_eagain(r)) { platform_sleep_ms(2); continue; }
        return -1;
    }
    return 0;
}

static void set_recv_timeout(int s, int64_t sec)
{
    NetTimeval tv; tv.tv_sec = sec * 1000000; tv.tv_usec = 0;
    ps3_sys_net_setsockopt(s, NET_SOL_SOCKET, NET_SO_RCVTIMEO, &tv, sizeof(tv));
}

/* ── DNS ─────────────────────────────────────────────────────────── */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static uint32_t parse_ipv4(const char *s)
{
    uint32_t parts[4] = {0,0,0,0};
    int idx = 0;
    const char *p = s;
    while (*p && idx < 4) {
        if (*p < '0' || *p > '9') return 0;
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); p++; }
        parts[idx++] = v;
        if (*p == '.') { p++; continue; }
        break;
    }
    if (idx != 4 || *p != '\0') return 0;
    for (int i = 0; i < 4; i++) if (parts[i] > 255) return 0;
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

static int dns_ask(uint32_t server, const char *host, uint32_t *out_ip)
{
    uint8_t query[512];
    memset(query, 0, sizeof(query));
    size_t qn = strlen(host);
    if (qn == 0 || qn > 200) return -1;

    query[0] = 0x4d; query[1] = 0x43;
    query[2] = 0x01; query[3] = 0x00;
    query[4] = 0x00; query[5] = 0x01;

    size_t off = 12, label_start = 0;
    for (size_t i = 0; i <= qn; i++) {
        if (i == qn || host[i] == '.') {
            size_t lab = i - label_start;
            if (lab == 0 || lab > 63 || off + 1 + lab + 5 >= sizeof(query)) return -1;
            query[off++] = (uint8_t)lab;
            memcpy(&query[off], host + label_start, lab);
            off += lab;
            label_start = i + 1;
        }
    }
    query[off++] = 0x00;
    query[off++] = 0x00; query[off++] = 0x01;
    query[off++] = 0x00; query[off++] = 0x01;

    int s = ps3_sys_net_socket(NET_AF_INET, NET_SOCK_DGRAM, 0);
    if (s < 0) return -1;
    set_recv_timeout(s, 3);

    NetSockaddrIn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = 16;
    addr.sin_family = NET_AF_INET;
    addr.sin_port = 53;
    addr.sin_addr = server;
    if (ps3_sys_net_connect(s, &addr, sizeof(addr)) < 0) { ps3_sys_net_close(s); return -1; }
    if (send_all(s, query, off) != 0) { ps3_sys_net_close(s); return -1; }

    uint8_t resp[512];
    int n = recv_some(s, resp, sizeof(resp));
    ps3_sys_net_close(s);
    if (n < 12) return -1;

    int an = rd16(resp + 6);
    int pos = 12;
    while (pos < n && resp[pos] != 0) {
        if ((resp[pos] & 0xc0) == 0xc0) { pos += 2; break; }
        pos += resp[pos] + 1;
    }
    if (pos < n) pos += 1;
    pos += 4;

    for (int a = 0; a < an && pos + 10 <= n; a++) {
        if ((resp[pos] & 0xc0) == 0xc0) pos += 2;
        else { while (pos < n && resp[pos] != 0) pos += resp[pos] + 1; pos += 1; }
        if (pos + 10 > n) break;
        uint16_t type = rd16(resp + pos);
        uint16_t rdlen = rd16(resp + pos + 8);
        pos += 10;
        if (type == 1 && rdlen == 4 && pos + 4 <= n) { *out_ip = rd32(resp + pos); return 0; }
        pos += rdlen;
    }
    return -1;
}

static uint32_t resolve_host(const char *host)
{
    uint32_t ip = parse_ipv4(host);
    if (ip) return ip;
    static const uint32_t servers[] = { 0x08080808u, 0x01010101u, 0x09090909u };
    for (size_t i = 0; i < sizeof(servers)/sizeof(servers[0]); i++) {
        if (dns_ask(servers[i], host, &ip) == 0 && ip) return ip;
    }
    return 0;
}

/* ── URL parse + TCP ─────────────────────────────────────────────── */
static int parse_url(const char *url, char *host, size_t hostlen,
                     uint16_t *port, char *path, size_t pathlen)
{
    if (!url || strncmp(url, "http://", 7) != 0) return -1;
    const char *p = url + 7;
    const char *h = p;
    while (*p && *p != '/' && *p != ':') p++;
    size_t hl = (size_t)(p - h);
    if (hl == 0 || hl >= hostlen) return -1;
    memcpy(host, h, hl);
    host[hl] = '\0';

    *port = 80;
    if (*p == ':') {
        p++;
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); p++; }
        if (v == 0 || v > 65535) return -1;
        *port = (uint16_t)v;
    }
    if (*p == '/') {
        if (strlen(p) >= pathlen) return -1;
        strcpy(path, p);
    } else {
        strcpy(path, "/");
    }
    return 0;
}

static int tcp_connect(uint32_t ip, uint16_t port)
{
    int s = ps3_sys_net_socket(NET_AF_INET, NET_SOCK_STREAM, 0);
    if (s < 0) return -1;
    NetSockaddrIn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = 16;
    addr.sin_family = NET_AF_INET;
    addr.sin_port = port;
    addr.sin_addr = ip;
    if (ps3_sys_net_connect(s, &addr, sizeof(addr)) < 0) { ps3_sys_net_close(s); return -1; }
    set_recv_timeout(s, 20);
    return s;
}

/* ── headers ─────────────────────────────────────────────────────── */
typedef struct {
    int  status;
    long content_length;
    bool chunked;
    char location[512];
} HttpMeta;
static uint8_t g_prebuf[8192];
static size_t  g_prelen = 0;

static const char *ci_find(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i]) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            i++;
        }
        if (i == nl) return p;
    }
    return NULL;
}

static int read_headers(int s, HttpMeta *meta)
{
    static char hdr[HTTP_HDR_MAX];
    size_t used = 0;
    meta->status = -1;
    meta->content_length = -1;
    meta->chunked = false;
    meta->location[0] = '\0';
    g_prelen = 0;

    for (;;) {
        if (used + 1 >= sizeof(hdr)) return -1;
        int r = recv_some(s, hdr + used, sizeof(hdr) - 1 - used);
        if (r <= 0) return -1;
        used += (size_t)r;
        hdr[used] = '\0';
        char *end = strstr(hdr, "\r\n\r\n");
        if (end) {
            size_t hlen = (size_t)(end - hdr) + 4;
            g_prelen = used - hlen;
            if (g_prelen > sizeof(g_prebuf)) g_prelen = sizeof(g_prebuf);
            if (g_prelen) memcpy(g_prebuf, hdr + hlen, g_prelen);
            break;
        }
    }

    if (strncmp(hdr, "HTTP/", 5) != 0) return -1;
    const char *sp = strchr(hdr, ' ');
    if (!sp) return -1;
    meta->status = atoi(sp + 1);
    if (meta->status < 100) return -1;

    const char *cl = ci_find(hdr, "content-length:");
    if (cl) { cl += 15; while (*cl == ' ') cl++; meta->content_length = atol(cl); }

    if (ci_find(hdr, "transfer-encoding:") && ci_find(hdr, "chunked"))
        meta->chunked = true;

    const char *loc = ci_find(hdr, "location:");
    if (loc) {
        loc += 9;
        while (*loc == ' ') loc++;
        size_t i = 0;
        while (loc[i] && loc[i] != '\r' && loc[i] != '\n' && i + 1 < sizeof(meta->location)) {
            meta->location[i] = loc[i]; i++;
        }
        meta->location[i] = '\0';
    }
    return 0;
}

/* ── body ────────────────────────────────────────────────────────── */
typedef struct {
    bool chunked;
    int  chunk_left;
    char line[64];
    size_t ll;
    long remaining;
    PlatformHttpChunkCallback cb;
    void *ud;
    char *acc;
    size_t len, cap;
} BodyState;

static int body_emit(BodyState *st, const uint8_t *p, size_t n)
{
    if (!n) return 0;
    if (st->cb && !st->cb(p, n, st->ud)) return -2;
    if (st->acc) {
        if (st->len + n + 1 > st->cap) {
            size_t ncap = st->cap * 2 + n + 1;
            char *g = (char *)malloc(ncap);
            if (!g) return -1;
            memcpy(g, st->acc, st->len);
            free(st->acc);
            st->acc = g;
            st->cap = ncap;
        }
        memcpy(st->acc + st->len, p, n);
        st->len += n;
    }
    return 0;
}

static int body_feed(BodyState *st, const uint8_t *buf, size_t n)
{
    size_t i = 0;
    if (!st->chunked) {
        if (st->remaining > 0 && (long)n > st->remaining) n = (size_t)st->remaining;
        int rc = body_emit(st, buf, n);
        if (rc) return rc;
        if (st->remaining > 0) st->remaining -= (long)n;
        return 0;
    }

    while (i < n) {
        if (st->chunk_left < 0) {
            while (i < n && buf[i] != '\n') {
                if (st->ll + 1 < sizeof(st->line)) st->line[st->ll++] = (char)buf[i];
                i++;
            }
            if (i < n && buf[i] == '\n') i++;
            if (st->ll == 0) continue;
            if (st->line[st->ll - 1] == '\r') st->ll--;
            st->line[st->ll] = '\0';
            st->chunk_left = (int)strtol(st->line, NULL, 16);
            st->ll = 0;
            if (st->chunk_left == 0) { st->chunked = false; return 0; }
            continue;
        }
        size_t take = n - i;
        if ((int)take > st->chunk_left) take = (size_t)st->chunk_left;
        int rc = body_emit(st, buf + i, take);
        if (rc) return rc;
        st->chunk_left -= (int)take;
        i += take;
    }
    return 0;
}

static int pump_body(int s, const HttpMeta *meta, PlatformHttpChunkCallback cb, void *ud,
                     char **out, size_t *out_size)
{
    BodyState st;
    memset(&st, 0, sizeof(st));
    st.chunked = meta->chunked;
    st.chunk_left = -1;
    st.remaining = meta->content_length;
    st.cb = cb;
    st.ud = ud;

    if (out) {
        st.cap = (meta->content_length > 0) ? (size_t)meta->content_length : 65536;
        st.acc = (char *)malloc(st.cap + 1);
        if (!st.acc) return -1;
        st.acc[0] = '\0';
        st.len = 0;
    }
    int rc = 0;
    if (g_prelen) {
        rc = body_feed(&st, g_prebuf, g_prelen);
        g_prelen = 0;
    }

    uint8_t buf[8192];
    while (rc == 0) {
        if (!st.chunked && st.remaining == 0) break;
        size_t want = sizeof(buf);
        if (!st.chunked && st.remaining > 0 && (long)want > st.remaining) want = (size_t)st.remaining;
        int r = recv_some(s, buf, want);
        if (r < 0) { rc = -1; break; }
        if (r == 0) break;
        rc = body_feed(&st, buf, (size_t)r);
    }

    if (st.acc) {
        st.acc[st.len] = '\0';
        *out = st.acc;
        if (out_size) *out_size = st.len;
    }
    return rc;
}

static int http_request(const char *url, PlatformHttpChunkCallback cb, void *ud,
                        char **out, size_t *out_size)
{
    int attempts = 0;
retry:
    char host[256], path[1024];
    uint16_t port = 80;
    if (parse_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        printf("[HTTP] Unsupported URL: %s\n", url ? url : "(null)");
        return -1;
    }

    uint32_t ip = resolve_host(host);
    if (!ip) { printf("[HTTP] DNS failed for %s\n", host); return -1; }

    int s = tcp_connect(ip, port);
    if (s < 0) { printf("[HTTP] connect failed %s:%u\n", host, (unsigned)port); return -1; }

    char req[1400];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: MonochromePS3/1.0\r\n"
                      "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n", path, host);
    if (rl <= 0 || (size_t)rl >= sizeof(req) || send_all(s, req, (size_t)rl) != 0) {
        ps3_sys_net_close(s); return -1;
    }

    HttpMeta meta;
    if (read_headers(s, &meta) != 0) { ps3_sys_net_close(s); return -1; }

    if (meta.status == 429 && attempts < 3) {
        ps3_sys_net_close(s);
        attempts++;
        printf("[HTTP] 429 rate-limited, retry %d\n", attempts);
        platform_sleep_ms(4000);
        goto retry;
    }

    printf("[HTTP] %d len=%ld chunked=%d %s\n", meta.status, meta.content_length,
           (int)meta.chunked, url);

    if (meta.status >= 300 && meta.status < 400 && meta.location[0]) {
        ps3_sys_net_close(s);
        if (strncmp(meta.location, "http://", 7) == 0)
            return http_request(meta.location, cb, ud, out, out_size);
        return -1;
    }
    if (meta.status >= 400) { ps3_sys_net_close(s); return -1; }

    int rc = pump_body(s, &meta, cb, ud, out, out_size);
    ps3_sys_net_close(s);
    return rc;
}

int platform_http_get(const char *url, char **resp, size_t *sz)
{
    if (!url || !resp) return -1;
    *resp = NULL;
    if (sz) *sz = 0;
    char *body = NULL;
    size_t len = 0;
    int rc = http_request(url, NULL, NULL, &body, &len);
    if (rc != 0) return -1;
    *resp = body;
    if (sz) *sz = len;
    return 0;
}

int platform_http_get_streaming(const char *url, PlatformHttpChunkCallback on_chunk, void *userdata)
{
    if (!url || !on_chunk) return -1;
    int rc = http_request(url, on_chunk, userdata, NULL, NULL);
    if (rc == -2) return 0;
    return rc == 0 ? 0 : -1;
}

void platform_http_free(char *r) { if (r) free(r); }
