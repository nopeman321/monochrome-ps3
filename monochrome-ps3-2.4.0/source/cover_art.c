/*
 * Monochrome PS3 - Asynchronous Cover Art LRU Cache (Hardened)
 * Caché LRU multi-carátula (16 slots) con buffer de staging UI seguro
 * contra Use-After-Free y descarga asíncrona por condvar + net_hal.
 */

#define _DEFAULT_SOURCE
#include <ppu-lv2.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <setjmp.h>

#include <sysmodule/sysmodule.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/cond.h>
#include <net/net.h>
#include <net/netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <jpeglib.h>

#include "cover_art.h"
#include "platform.h"
#include "http_client.h"
#include "net_hal.h"

#define DEFAULT_API_HOST       "tracks.monochrome.st"
#define DEFAULT_API_PORT       80
#define MAX_COVER_SIZE         (512 * 1024)
#define COVER_RESIZE_W         256
#define COVER_RESIZE_H         256
#define MAX_PENDING_COVERS     8
#define MAX_FAILED_COVERS      32
#define COVER_HTTP_TIMEOUT_MS  5000
#define COVER_MAX_REDIRECTS    3

/* ── Estado global ─────────────────────────────────────────────── */
static CoverCacheEntry s_cache[MAX_COVER_CACHE];
static uint32_t s_access_clock = 0;
static CoverArtMode s_cover_mode = COVER_MODE_ORIGINAL;

static sys_ppu_thread_t s_cover_thread_id;
static volatile bool s_worker_keep_running = false;
static volatile bool s_cover_abort = false;
static bool s_worker_thread_created = false;
static char s_pending_queue[MAX_PENDING_COVERS][512];
static int  s_pending_head = 0;
static int  s_pending_tail = 0;
static int  s_pending_count = 0;
static char s_active_url[512] = "";
static int  s_active_sock = -1;
static char s_failed_urls[MAX_FAILED_COVERS][512];
static int  s_failed_next = 0;

static sys_mutex_t s_cover_mutex;
static bool s_mutex_initialized = false;
static sys_cond_t s_cover_cond;
static bool s_cond_initialized = false;

/* Static reusable 512 KB download buffer (worker thread only) */
static uint8_t s_cover_dl_buf[MAX_COVER_SIZE];

/* Dedicated UI double-buffered pixel staging (prevents Use-After-Free on LRU eviction) */
static uint8_t s_ui_tex_buf[2][COVER_RESIZE_W * COVER_RESIZE_H * 4] __attribute__((aligned(16)));
static int s_ui_tex_idx = 0;

static bool is_url_failed_locked(const char *url)
{
    for (int i = 0; i < MAX_FAILED_COVERS; i++) {
        if (s_failed_urls[i][0] && strcmp(s_failed_urls[i], url) == 0)
            return true;
    }
    return false;
}

static void mark_url_failed(const char *url)
{
    if (!url || !url[0]) return;
    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
    if (!is_url_failed_locked(url)) {
        strncpy(s_failed_urls[s_failed_next], url, 511);
        s_failed_urls[s_failed_next][511] = '\0';
        s_failed_next = (s_failed_next + 1) % MAX_FAILED_COVERS;
    }
    s_active_url[0] = '\0';
    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
}

static void close_cover_sock(int sock)
{
    if (sock < 0) return;
    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
    if (s_active_sock == sock) {
        s_active_sock = -1;
    }
    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
    close(sock);
}

/* ── libjpeg error manager ─────────────────────────────────────── */
struct custom_jpeg_error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void custom_error_exit(j_common_ptr cinfo)
{
    struct custom_jpeg_error_mgr *myerr = (struct custom_jpeg_error_mgr *)cinfo->err;
    char buffer[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, buffer);
    net_hal_log("COVER", "[COVER ERROR libjpeg] %s", buffer);
    longjmp(myerr->setjmp_buffer, 1);
}

static void custom_output_message(j_common_ptr cinfo)
{
    char buffer[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, buffer);
    net_hal_log("COVER", "[COVER WARN libjpeg] %s", buffer);
}

/* ── Utilidades de caché ───────────────────────────────────────── */

static int cache_find_slot(const char *url)
{
    for (int i = 0; i < MAX_COVER_CACHE; i++) {
        if (s_cache[i].ready && strcmp(s_cache[i].url, url) == 0)
            return i;
    }
    return -1;
}

static int cache_lru_victim(void)
{
    int victim = 0;
    uint32_t oldest = s_cache[0].last_access;
    for (int i = 1; i < MAX_COVER_CACHE; i++) {
        if (!s_cache[i].ready) return i;
        if (s_cache[i].last_access < oldest) {
            oldest = s_cache[i].last_access;
            victim = i;
        }
    }
    return victim;
}

static void cache_free_entry(int idx)
{
    if (s_cache[idx].tex_og.data) {
        free(s_cache[idx].tex_og.data);
        s_cache[idx].tex_og.data = NULL;
        s_cache[idx].tex_og.uploaded = false;
    }
    if (s_cache[idx].tex_1bit.data) {
        free(s_cache[idx].tex_1bit.data);
        s_cache[idx].tex_1bit.data = NULL;
        s_cache[idx].tex_1bit.uploaded = false;
    }
    s_cache[idx].ready = false;
    s_cache[idx].has_og = false;
    s_cache[idx].has_1bit = false;
    s_cache[idx].url[0] = '\0';
}

/* ── Decodificador JPEG ────────────────────────────────────────── */
static bool decode_jpeg_to_rgba(const uint8_t *jpeg_data, size_t jpeg_size,
                                uint8_t **out_rgba, int *out_w, int *out_h)
{
    struct jpeg_decompress_struct cinfo;
    struct custom_jpeg_error_mgr jerr;
    JSAMPARRAY buffer;
    int row_stride;

    if (!jpeg_data || jpeg_size < 16 || !out_rgba || !out_w || !out_h) return false;
    *out_rgba = NULL;
    *out_w = 0;
    *out_h = 0;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = custom_error_exit;
    jerr.pub.output_message = custom_output_message;

    if (setjmp(jerr.setjmp_buffer)) {
        net_hal_log("COVER", "[COVER ERROR] Decodificacion abortada de forma segura");
        jpeg_destroy_decompress(&cinfo);
        if (*out_rgba) { free(*out_rgba); *out_rgba = NULL; }
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)jpeg_data, (unsigned long)jpeg_size);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        net_hal_log("COVER", "[COVER ERROR] JPEG header invalido");
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    *out_w = (int)cinfo.output_width;
    *out_h = (int)cinfo.output_height;
    if (*out_w <= 0 || *out_h <= 0 || *out_w > 4096 || *out_h > 4096) {
        net_hal_log("COVER", "[COVER ERROR] Dimensiones JPEG fuera de rango (%dx%d)", *out_w, *out_h);
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    row_stride = (*out_w) * (int)cinfo.output_components;

    size_t rgba_size = (size_t)(*out_w) * (size_t)(*out_h) * 4u;
    *out_rgba = (uint8_t *)memalign(16, rgba_size);
    if (!*out_rgba) {
        net_hal_log("COVER", "[COVER ERROR] malloc fallo para RGBA (%lu B)",
                    (unsigned long)rgba_size);
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);
    uint8_t *dst = *out_rgba;

    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, buffer, 1);
        uint8_t *src = buffer[0];
        for (int x = 0; x < *out_w; x++) {
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
            src += 3; dst += 4;
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

/* ── Redimensionar RGBA a 256x256 (simple box filter) ──────────── */
static uint8_t *resize_rgba(const uint8_t *src, int sw, int sh, int dw, int dh)
{
    size_t size = (size_t)dw * (size_t)dh * 4u;
    uint8_t *dst = (uint8_t *)memalign(16, size);
    if (!dst) return NULL;

    for (int y = 0; y < dh; y++) {
        int sy = (y * sh) / dh;
        for (int x = 0; x < dw; x++) {
            int sx = (x * sw) / dw;
            const uint8_t *sp = &src[(sy * sw + sx) * 4];
            uint8_t *dp = &dst[(y * dw + x) * 4];
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
        }
    }
    return dst;
}

/* ── Subir textura (vía gfx_engine) ────────────────────────────── */
static bool upload_texture(GfxTexture *tex, const uint8_t *rgba, int w, int h)
{
    if (gfx_upload_rgba_texture(rgba, w, h, tex) != 0) return false;
    tex->uploaded = true;
    return true;
}

/* ── Helper para extraer cabecera Location en redirecciones ────── */
static bool extract_header_value(const char *headers, const char *name, char *out, size_t out_sz)
{
    size_t nlen = strlen(name);
    const char *p = headers;
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        if (!eol) break;
        if (eol == p) break;
        if ((size_t)(eol - p) > nlen + 1 && strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *val = p + nlen + 1;
            while (val < eol && (*val == ' ' || *val == '\t')) val++;
            size_t vlen = (size_t)(eol - val);
            if (vlen >= out_sz) vlen = out_sz - 1;
            memcpy(out, val, vlen);
            out[vlen] = '\0';
            return true;
        }
        p = eol + 2;
    }
    return false;
}

/* ── Worker thread persistente y no bloqueante ──────────────────── */
static void download_one_cover(const char *orig_url)
{
    char cur_url[512];
    strncpy(cur_url, orig_url, sizeof(cur_url) - 1);
    cur_url[sizeof(cur_url) - 1] = '\0';

    net_hal_log("COVER", "[COVER THREAD] Descargando: %s", orig_url);

    size_t jpeg_size = 0;
    bool got_image = false;

    int timeout_ms = http_get_timeout_ms();
    if (timeout_ms < 2000) timeout_ms = COVER_HTTP_TIMEOUT_MS;
    if (timeout_ms > 15000) timeout_ms = 15000;

    for (int hop = 0; hop <= COVER_MAX_REDIRECTS && s_worker_keep_running; hop++) {
        /* Explicit scheme validation: NEVER silently downgrade https:// to HTTP port 80 */
        if (strncmp(cur_url, "https://", 8) == 0) {
            net_hal_log("COVER", "[COVER ERROR] URL HTTPS rechazado (sin TLS backend; no se degrada a puerto 80): %s", cur_url);
            mark_url_failed(orig_url);
            return;
        }
        if (strncmp(cur_url, "http://", 7) != 0) {
            net_hal_log("COVER", "[COVER ERROR] Esquema URL no soportado: %s", cur_url);
            mark_url_failed(orig_url);
            return;
        }

        const char *p = cur_url + 7;
        const char *slash = strchr(p, '/');
        if (!slash) {
            net_hal_log("COVER", "[COVER ERROR] URL sin path: %s", cur_url);
            mark_url_failed(orig_url);
            return;
        }

        char host[96] = DEFAULT_API_HOST;
        uint16_t port = DEFAULT_API_PORT;
        size_t host_len = (size_t)(slash - p);
        if (host_len == 0 || host_len >= sizeof(host)) {
            net_hal_log("COVER", "[COVER ERROR] Host invalido en URL: %s", cur_url);
            mark_url_failed(orig_url);
            return;
        }
        memcpy(host, p, host_len);
        host[host_len] = '\0';

        char *colon = strchr(host, ':');
        if (colon) {
            *colon = '\0';
            int prt = atoi(colon + 1);
            if (prt > 0 && prt < 65536) port = (uint16_t)prt;
        }
        const char *path = slash;

        /* Reject CRLF / control chars in host or path */
        if (strpbrk(host, "\r\n ") || strpbrk(path, "\r\n ")) {
            net_hal_log("COVER", "[COVER ERROR] Caracteres invalidos en URL: %s", cur_url);
            mark_url_failed(orig_url);
            return;
        }

        int sock = -1;
        int net_err = 0;
        if (net_hal_connect(host, port, timeout_ms, &s_cover_abort, &sock, &net_err) != 0 || sock < 0) {
            net_hal_log("COVER", "[COVER ERROR] net_hal_connect fallo a %s:%u (errno: %d)", host, (unsigned)port, net_err);
            mark_url_failed(orig_url);
            return;
        }

        if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
        if (!s_worker_keep_running || s_cover_abort) {
            if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
            close(sock);
            return;
        }
        s_active_sock = sock;
        if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);

        char req[1024];
        snprintf(req, sizeof(req),
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "User-Agent: Monochrome-PS3/2.4.0 (PSL1GHT)\r\n"
                 "Accept: image/jpeg, image/*, */*\r\n"
                 "Connection: close\r\n\r\n",
                 path, host);

        if (net_hal_send_all(sock, req, strlen(req), timeout_ms, &s_cover_abort, &net_err) != 0) {
            net_hal_log("COVER", "[COVER ERROR] send HTTP fallo (errno: %d)", net_err);
            close_cover_sock(sock);
            mark_url_failed(orig_url);
            return;
        }

        char head_buf[4096];
        size_t head_len = 0;
        char *body_start = NULL;

        while (s_worker_keep_running && !s_cover_abort && head_len < sizeof(head_buf) - 1) {
            size_t to_read = sizeof(head_buf) - head_len - 1;
            if (to_read > 1024) to_read = 1024;
            int n = net_hal_recv_some(sock, head_buf + head_len, to_read, timeout_ms, &s_cover_abort, &net_err);
            if (n <= 0) break;
            head_len += (size_t)n;
            head_buf[head_len] = '\0';
            body_start = strstr(head_buf, "\r\n\r\n");
            if (body_start) { body_start += 4; break; }
        }

        if (!s_worker_keep_running || s_cover_abort) {
            close_cover_sock(sock);
            return;
        }

        if (!body_start) {
            net_hal_log("COVER", "[COVER ERROR] Headers HTTP incompletos o timeout (errno: %d)", net_err);
            close_cover_sock(sock);
            mark_url_failed(orig_url);
            return;
        }

        int http_code = 0;
        sscanf(head_buf, "HTTP/%*s %d", &http_code);

        /* Handle bounded redirects */
        if (http_code == 301 || http_code == 302 || http_code == 307 || http_code == 308) {
            char loc[512] = "";
            bool has_loc = extract_header_value(head_buf, "Location", loc, sizeof(loc));
            close_cover_sock(sock);
            if (!has_loc || loc[0] == '\0' || hop == COVER_MAX_REDIRECTS) {
                net_hal_log("COVER", "[COVER ERROR] Redireccion invalida o limite excedido (HTTP %d)", http_code);
                mark_url_failed(orig_url);
                return;
            }
            if (strncmp(loc, "https://", 8) == 0) {
                net_hal_log("COVER", "[COVER ERROR] Redireccion a HTTPS rechazada (sin TLS): %s", loc);
                mark_url_failed(orig_url);
                return;
            }
            strncpy(cur_url, loc, sizeof(cur_url) - 1);
            cur_url[sizeof(cur_url) - 1] = '\0';
            continue;
        }

        if (http_code != 200) {
            net_hal_log("COVER", "[COVER ERROR] HTTP %d (%s)", http_code, cur_url);
            close_cover_sock(sock);
            mark_url_failed(orig_url);
            return;
        }

        /* Validate Content-Length against static 512 KB buffer */
        char cl_buf[32] = "";
        if (extract_header_value(head_buf, "Content-Length", cl_buf, sizeof(cl_buf))) {
            unsigned long long clen = strtoull(cl_buf, NULL, 10);
            if (clen > MAX_COVER_SIZE) {
                net_hal_log("COVER", "[COVER ERROR] Content-Length (%llu) excede %d B", clen, MAX_COVER_SIZE);
                close_cover_sock(sock);
                mark_url_failed(orig_url);
                return;
            }
        }

        jpeg_size = 0;
        size_t initial_body = head_len - (size_t)(body_start - head_buf);
        if (initial_body > MAX_COVER_SIZE) {
            net_hal_log("COVER", "[COVER ERROR] Cuerpo inicial excede %d B", MAX_COVER_SIZE);
            close_cover_sock(sock);
            mark_url_failed(orig_url);
            return;
        }
        if (initial_body > 0) {
            memcpy(s_cover_dl_buf, body_start, initial_body);
            jpeg_size = initial_body;
        }

        bool recv_error = false;
        while (s_worker_keep_running && !s_cover_abort && jpeg_size < MAX_COVER_SIZE) {
            size_t to_read = MAX_COVER_SIZE - jpeg_size;
            if (to_read > 8192) to_read = 8192;
            int n = net_hal_recv_some(sock, s_cover_dl_buf + jpeg_size, to_read, timeout_ms, &s_cover_abort, &net_err);
            if (n > 0) {
                jpeg_size += (size_t)n;
            } else if (n == 0) {
                break;
            } else {
                recv_error = true;
                break;
            }
        }
        close_cover_sock(sock);

        if (!s_worker_keep_running || s_cover_abort) {
            return;
        }
        if (recv_error) {
            net_hal_log("COVER", "[COVER ERROR] Error de recepcion JPEG (errno: %d)", net_err);
            mark_url_failed(orig_url);
            return;
        }

        got_image = true;
        break;
    }

    if (!got_image) {
        mark_url_failed(orig_url);
        return;
    }

    if (jpeg_size < 100) {
        net_hal_log("COVER", "[COVER ERROR] Archivo muy corto: %lu bytes", (unsigned long)jpeg_size);
        mark_url_failed(orig_url);
        return;
    }

    /* Decodificar JPEG desde buffer estatico de 512 KB */
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (!decode_jpeg_to_rgba(s_cover_dl_buf, jpeg_size, &rgba, &w, &h)) {
        net_hal_log("COVER", "[COVER ERROR] decode_jpeg_to_rgba fallo");
        mark_url_failed(orig_url);
        return;
    }

    /* Redimensionar a 256x256 */
    uint8_t *resized = resize_rgba(rgba, w, h, COVER_RESIZE_W, COVER_RESIZE_H);
    free(rgba);
    if (!resized) {
        net_hal_log("COVER", "[COVER ERROR] resize_rgba fallo");
        mark_url_failed(orig_url);
        return;
    }

    /* Guardar en caché (slot LRU o existente) bajo s_cover_mutex */
    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);

    int slot = cache_find_slot(orig_url);
    if (slot < 0) slot = cache_lru_victim();
    if (slot < 0) {
        s_active_url[0] = '\0';
        if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
        free(resized);
        return;
    }

    cache_free_entry(slot);
    strncpy(s_cache[slot].url, orig_url, sizeof(s_cache[slot].url) - 1);
    s_cache[slot].url[sizeof(s_cache[slot].url) - 1] = '\0';
    s_cache[slot].width = COVER_RESIZE_W;
    s_cache[slot].height = COVER_RESIZE_H;
    s_cache[slot].last_access = ++s_access_clock;

    if (upload_texture(&s_cache[slot].tex_og, resized,
                       COVER_RESIZE_W, COVER_RESIZE_H)) {
        s_cache[slot].has_og = true;
        net_hal_log("COVER", "[COVER OK] Textura OG subida: %s", orig_url);
    }
    s_cache[slot].has_1bit = false;
    s_cache[slot].ready = s_cache[slot].has_og;
    s_active_url[0] = '\0';

    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);

    free(resized);

    net_hal_log("COVER", "[COVER READY] Caratula cacheada: %s (slot %d)", orig_url, slot);
}

static void cover_art_worker(void *arg)
{
    (void)arg;
    net_hal_log("COVER", "[COVER WORKER] Hilo iniciado (PPU prio 1200)");

    while (s_worker_keep_running) {
        char url[512];
        url[0] = '\0';

        if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
        while (s_worker_keep_running &&
               (s_pending_count == 0 || !net_hal_is_connected() || net_hal_is_menu_paused())) {
            if (s_cond_initialized) {
                u64 timeout_us = (s_pending_count == 0) ? 0ULL : 500000ULL;
                sysCondWait(s_cover_cond, timeout_us);
            } else {
                if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
                usleep(50000);
                if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
            }
        }

        if (!s_worker_keep_running) {
            if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
            break;
        }

        if (s_pending_count > 0) {
            strncpy(url, s_pending_queue[s_pending_head], sizeof(url) - 1);
            url[sizeof(url) - 1] = '\0';
            s_pending_head = (s_pending_head + 1) % MAX_PENDING_COVERS;
            s_pending_count--;

            strncpy(s_active_url, url, sizeof(s_active_url) - 1);
            s_active_url[sizeof(s_active_url) - 1] = '\0';
        }
        if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);

        if (url[0] != '\0') {
            download_one_cover(url);
        }
    }

    net_hal_log("COVER", "[COVER WORKER] Hilo finalizado");
    sysThreadExit(0);
}

/* ── API pública ───────────────────────────────────────────────── */

void cover_art_init(void)
{
    if (!s_mutex_initialized) {
        sys_mutex_attr_t attr;
        sysMutexAttrInitialize(attr);
        if (sysMutexCreate(&s_cover_mutex, &attr) == 0)
            s_mutex_initialized = true;
    }
    if (s_mutex_initialized && !s_cond_initialized) {
        sys_cond_attr_t cattr;
        sysCondAttrInitialize(cattr);
        if (sysCondCreate(&s_cover_cond, s_cover_mutex, &cattr) == 0)
            s_cond_initialized = true;
    }

    memset(s_cache, 0, sizeof(s_cache));
    memset(s_pending_queue, 0, sizeof(s_pending_queue));
    memset(s_failed_urls, 0, sizeof(s_failed_urls));
    s_pending_head = 0;
    s_pending_tail = 0;
    s_pending_count = 0;
    s_active_url[0] = '\0';
    s_active_sock = -1;
    s_failed_next = 0;
    s_access_clock = 0;
    s_ui_tex_idx = 0;
    s_cover_mode = COVER_MODE_ORIGINAL;

    s_cover_abort = false;
    s_worker_keep_running = true;
    int ret = sysThreadCreate(&s_cover_thread_id, cover_art_worker, NULL, 1200,
                              0x20000, THREAD_JOINABLE, "mono_cover");
    if (ret == 0) {
        s_worker_thread_created = true;
    } else {
        s_worker_keep_running = false;
        s_worker_thread_created = false;
        net_hal_log("COVER", "[COVER ERROR] sysThreadCreate(mono_cover) fallo: %d", ret);
    }

    net_hal_log("COVER", "[COVER] Caché LRU inicializado (%d slots, prio 1200, condvar async)",
                MAX_COVER_CACHE);
}

void cover_art_shutdown(void)
{
    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
    s_worker_keep_running = false;
    s_cover_abort = true;
    if (s_active_sock >= 0) {
        shutdown(s_active_sock, SHUT_RDWR);
    }
    if (s_cond_initialized) {
        sysCondBroadcast(s_cover_cond);
    }
    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);

    if (s_worker_thread_created) {
        uint64_t exit_val = 0;
        sysThreadJoin(s_cover_thread_id, &exit_val);
        s_worker_thread_created = false;
    }
    cover_art_clear_all();

    if (s_cond_initialized) {
        sysCondDestroy(s_cover_cond);
        s_cond_initialized = false;
    }
    if (s_mutex_initialized) {
        sysMutexDestroy(s_cover_mutex);
        s_mutex_initialized = false;
    }
    net_hal_log("COVER", "[COVER] Caché LRU detenido");
}

CoverArtMode cover_art_get_mode(void) { return COVER_MODE_ORIGINAL; }

void cover_art_set_mode(CoverArtMode mode)
{
    (void)mode;
    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
    s_cover_mode = COVER_MODE_ORIGINAL;
    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
}

bool cover_art_load(const char *url, CoverArtMode mode)
{
    if (!url || url[0] == '\0') {
        return false;
    }

    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);

    /* 1. Si ya está en caché, actualizar LRU y devolver inmediatamente */
    int slot = cache_find_slot(url);
    if (slot >= 0 && s_cache[slot].ready) {
        s_cache[slot].last_access = ++s_access_clock;
        s_cover_mode = mode;
        if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
        return true;
    }

    /* 2. Si falló previamente, no reintentar cada frame */
    if (is_url_failed_locked(url)) {
        if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
        return false;
    }

    /* 3. Si ya se está descargando actualmente, no duplicar */
    if (s_active_url[0] && strcmp(s_active_url, url) == 0) {
        if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
        return false;
    }

    /* 4. Si ya está en cola pendiente, no duplicar */
    for (int i = 0; i < s_pending_count; i++) {
        int idx = (s_pending_head + i) % MAX_PENDING_COVERS;
        if (strcmp(s_pending_queue[idx], url) == 0) {
            if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
            return false;
        }
    }

    /* 5. Encolar de forma totalmente no bloqueante */
    if (s_pending_count >= MAX_PENDING_COVERS) {
        net_hal_log("COVER", "[COVER WARN] Cola llena (%d), descartando mas antigua: %s",
                    MAX_PENDING_COVERS, s_pending_queue[s_pending_head]);
        s_pending_head = (s_pending_head + 1) % MAX_PENDING_COVERS;
        s_pending_count--;
    }
    strncpy(s_pending_queue[s_pending_tail], url, 511);
    s_pending_queue[s_pending_tail][511] = '\0';
    s_pending_tail = (s_pending_tail + 1) % MAX_PENDING_COVERS;
    s_pending_count++;

    if (s_cond_initialized) sysCondSignal(s_cover_cond);
    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
    return true;
}

bool cover_art_get_texture(const char *url, GfxTexture *out)
{
    if (!url || !out) return false;

    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
    int slot = cache_find_slot(url);
    if (slot >= 0 && s_cache[slot].ready && s_cache[slot].has_og && s_cache[slot].tex_og.data) {
        size_t bytes = (size_t)s_cache[slot].tex_og.width * (size_t)s_cache[slot].tex_og.height * 4u;
        if (bytes > 0 && bytes <= sizeof(s_ui_tex_buf[0])) {
            s_cache[slot].last_access = ++s_access_clock;
            uint8_t *ui_buf = s_ui_tex_buf[s_ui_tex_idx];
            s_ui_tex_idx = (s_ui_tex_idx + 1) & 1;
            memcpy(ui_buf, s_cache[slot].tex_og.data, bytes);
            *out = s_cache[slot].tex_og;
            out->data = ui_buf;
            if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
            return true;
        }
    }
    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
    return false;
}

void cover_art_clear(const char *url)
{
    if (!url || url[0] == '\0') { cover_art_clear_all(); return; }

    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
    int slot = cache_find_slot(url);
    if (slot >= 0) cache_free_entry(slot);
    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
}

void cover_art_clear_all(void)
{
    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
    for (int i = 0; i < MAX_COVER_CACHE; i++) {
        if (s_cache[i].ready) cache_free_entry(i);
    }
    s_access_clock = 0;
    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
}

bool cover_art_is_cached(const char *url)
{
    if (!url) return false;
    bool result = false;
    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
    int slot = cache_find_slot(url);
    if (slot >= 0 && s_cache[slot].ready) result = true;
    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
    return result;
}
