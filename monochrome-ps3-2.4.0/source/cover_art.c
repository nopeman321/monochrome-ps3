/*
 * Monochrome PS3 - Asynchronous Cover Art LRU Cache (Phase 2)
 * Caché LRU multi-carátula (16 slots) con soporte dual:
 *   - 1-bit B/N (Bayer 4x4 dithering)
 *   - Original (OG, color)
 *
 * Descarga asíncrona en hilo de baja prioridad para nunca congelar
 * los 60 FPS ni el audio.
 */

#define _DEFAULT_SOURCE
#include <ppu-lv2.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <setjmp.h>

#include <sysmodule/sysmodule.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <net/net.h>
#include <net/netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <jpeglib.h>

#include "cover_art.h"
#include "platform.h"

#define COVER_LOG_PATH    "/dev_hdd0/tmp/mono_cover.log"
#define DEFAULT_API_HOST  "tracks.monochrome.st"
#define DEFAULT_API_PORT  80
#define MAX_COVER_SIZE    (512 * 1024)
#define COVER_RESIZE_W    256
#define COVER_RESIZE_H    256

/* ── Bayer 4x4 matriz ordenada (0-15) ──────────────────────────── */
static const uint8_t BAYER4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5}
};

/* ── Logging ───────────────────────────────────────────────────── */
static void cover_log(const char *fmt, ...)
{
    FILE *f = fopen(COVER_LOG_PATH, "a");
    if (f) {
        time_t t = time(NULL);
        fprintf(f, "[%ld] ", (long)t);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fprintf(f, "\n");
        fflush(f);
        fclose(f);
    }
}

/* ── Estado global ─────────────────────────────────────────────── */
static CoverCacheEntry s_cache[MAX_COVER_CACHE];
static uint32_t s_access_clock = 0;
static CoverArtMode s_cover_mode = COVER_MODE_1BIT_DITHER;

static sys_ppu_thread_t s_cover_thread_id;
static volatile bool s_cover_thread_running = false;
static char s_pending_url[512] = "";
static sys_mutex_t s_cover_mutex;
static bool s_mutex_initialized = false;
static CoverArtMode cover_art_mode_global = COVER_MODE_1BIT_DITHER;

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
    cover_log("[COVER ERROR libjpeg] %s", buffer);
    longjmp(myerr->setjmp_buffer, 1);
}

static void custom_output_message(j_common_ptr cinfo)
{
    char buffer[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, buffer);
    cover_log("[COVER WARN libjpeg] %s", buffer);
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

/* ── Bayer 4x4 1-bit dithering ─────────────────────────────────── */
/* Convierte luminancia a blanco puro (#FFFFFF) o negro puro (#000000)
   usando la matriz Bayer 4x4 ordenada escalada a 0-255. */
static void apply_bayer4x4_1bit(uint8_t *rgba, int w, int h)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *p = &rgba[(y * w + x) * 4];
            uint8_t r = p[0], g = p[1], b = p[2];
            /* Luminancia Rec.601 */
            uint8_t lum = (uint8_t)((r * 299 + g * 587 + b * 114) / 1000);
            uint8_t thr = BAYER4[y & 3][x & 3];
            thr = (uint8_t)((thr * 255) / 15);
            uint8_t val = (lum > thr) ? 255 : 0;
            p[0] = val; p[1] = val; p[2] = val;
        }
    }
}

/* ── Decodificador JPEG ────────────────────────────────────────── */
static bool decode_jpeg_to_rgba(const uint8_t *jpeg_data, size_t jpeg_size,
                                uint8_t **out_rgba, int *out_w, int *out_h)
{
    struct jpeg_decompress_struct cinfo;
    struct custom_jpeg_error_mgr jerr;
    JSAMPARRAY buffer;
    int row_stride;

    if (!jpeg_data || jpeg_size < 16) return false;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = custom_error_exit;
    jerr.pub.output_message = custom_output_message;

    if (setjmp(jerr.setjmp_buffer)) {
        cover_log("[COVER ERROR] Decodificacion abortada de forma segura");
        jpeg_destroy_decompress(&cinfo);
        if (*out_rgba) { free(*out_rgba); *out_rgba = NULL; }
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)jpeg_data, (unsigned long)jpeg_size);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        cover_log("[COVER ERROR] JPEG header invalido");
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    *out_w = (int)cinfo.output_width;
    *out_h = (int)cinfo.output_height;
    row_stride = (int)cinfo.output_width * (int)cinfo.output_components;

    size_t rgba_size = (size_t)(*out_w) * (size_t)(*out_h) * 4u;
    *out_rgba = (uint8_t *)memalign(16, rgba_size);
    if (!*out_rgba) {
        cover_log("[COVER ERROR] malloc fallo para RGBA (%lu B)",
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

/* ── Subir textura a VRAM RSX (vía gfx_engine) ─────────────────── */
static bool upload_texture(GfxTexture *tex, const uint8_t *rgba, int w, int h)
{
    if (gfx_upload_rgba_texture(rgba, w, h, tex) != 0) return false;
    tex->uploaded = true;
    return true;
}

/* ── Worker thread ─────────────────────────────────────────────── */
static void cover_art_worker(void *arg)
{
    (void)arg;
    char url[512];

    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
    strncpy(url, s_pending_url, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);

    if (url[0] == '\0') {
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }

    cover_log("[COVER THREAD] Descargando: %s", url);

    /* Parse URL */
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;

    const char *slash = strchr(p, '/');
    if (!slash) {
        cover_log("[COVER ERROR] URL sin path: %s", url);
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }

    char host[96] = DEFAULT_API_HOST;
    int port = DEFAULT_API_PORT;
    size_t host_len = (size_t)(slash - p);
    if (host_len > 0 && host_len < sizeof(host)) {
        memcpy(host, p, host_len);
        host[host_len] = '\0';
        char *colon = strchr(host, ':');
        if (colon) {
            *colon = '\0';
            int prt = atoi(colon + 1);
            if (prt > 0 && prt < 65536) port = prt;
        }
    }
    const char *path = slash;

    struct hostent *he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        cover_log("[COVER ERROR] DNS fallo para %s (errno: %d)", host, errno);
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        cover_log("[COVER ERROR] socket() fallo: %d", errno);
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons((unsigned short)port);
    memcpy(&saddr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));

    if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        cover_log("[COVER ERROR] connect() fallo (errno: %d)", errno);
        close(sock);
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }

    char req[1024];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: Monochrome-PS3/2.4.0 (PSL1GHT)\r\n"
             "Accept: image/jpeg, image/*, */*\r\n"
             "Connection: close\r\n\r\n",
             path, host);

    if (send(sock, req, strlen(req), 0) <= 0) {
        cover_log("[COVER ERROR] send HTTP fallo");
        close(sock);
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }

    char head_buf[4096];
    size_t head_len = 0;
    char *body_start = NULL;

    while (head_len < sizeof(head_buf) - 1) {
        size_t to_read = sizeof(head_buf) - head_len - 1;
        if (to_read > 1024) to_read = 1024;
        ssize_t n = recv(sock, head_buf + head_len, to_read, 0);
        if (n <= 0) break;
        head_len += (size_t)n;
        head_buf[head_len] = '\0';
        body_start = strstr(head_buf, "\r\n\r\n");
        if (body_start) { body_start += 4; break; }
    }

    if (!body_start) {
        cover_log("[COVER ERROR] Headers HTTP incompletos");
        close(sock);
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }

    int http_code = 0;
    char *sp = strchr(head_buf, ' ');
    if (sp) http_code = atoi(sp + 1);
    if (http_code != 200) {
        cover_log("[COVER ERROR] HTTP %d", http_code);
        close(sock);
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }

    uint8_t *jpeg_buf = (uint8_t *)malloc(MAX_COVER_SIZE);
    if (!jpeg_buf) {
        cover_log("[COVER ERROR] malloc fallo para JPEG");
        close(sock);
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }

    size_t jpeg_size = 0;
    size_t initial_body = head_len - (size_t)(body_start - head_buf);
    if (initial_body > 0 && initial_body < MAX_COVER_SIZE) {
        memcpy(jpeg_buf, body_start, initial_body);
        jpeg_size = initial_body;
    }

    while (jpeg_size < MAX_COVER_SIZE) {
        size_t to_read = MAX_COVER_SIZE - jpeg_size;
        if (to_read > 8192) to_read = 8192;
        ssize_t n = recv(sock, jpeg_buf + jpeg_size, to_read, 0);
        if (n <= 0) break;
        jpeg_size += (size_t)n;
    }
    close(sock);

    if (jpeg_size < 100) {
        cover_log("[COVER ERROR] Archivo muy corto: %lu bytes", (unsigned long)jpeg_size);
        free(jpeg_buf);
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }

    /* Decodificar JPEG */
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (!decode_jpeg_to_rgba(jpeg_buf, jpeg_size, &rgba, &w, &h)) {
        cover_log("[COVER ERROR] decode_jpeg_to_rgba fallo");
        free(jpeg_buf);
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }
    free(jpeg_buf);

    /* Redimensionar a 256x256 */
    uint8_t *resized = resize_rgba(rgba, w, h, COVER_RESIZE_W, COVER_RESIZE_H);
    free(rgba);
    if (!resized) {
        cover_log("[COVER ERROR] resize_rgba fallo");
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }

    /* Preparar versión 1-bit (Bayer dither) */
    uint8_t *rgba_1bit = (uint8_t *)memalign(16,
        (size_t)COVER_RESIZE_W * (size_t)COVER_RESIZE_H * 4u);
    if (!rgba_1bit) {
        free(resized);
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }
    memcpy(rgba_1bit, resized, (size_t)COVER_RESIZE_W * (size_t)COVER_RESIZE_H * 4u);
    apply_bayer4x4_1bit(rgba_1bit, COVER_RESIZE_W, COVER_RESIZE_H);

    /* Guardar en caché (slot LRU o existente) */
    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);

    int slot = cache_find_slot(url);
    if (slot < 0) slot = cache_lru_victim();
    if (slot < 0) {
        if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
        free(resized); free(rgba_1bit);
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }

    cache_free_entry(slot);
    strncpy(s_cache[slot].url, url, sizeof(s_cache[slot].url) - 1);
    s_cache[slot].url[sizeof(s_cache[slot].url) - 1] = '\0';
    s_cache[slot].width = COVER_RESIZE_W;
    s_cache[slot].height = COVER_RESIZE_H;
    s_cache[slot].last_access = ++s_access_clock;

    /* Subir versión OG */
    if (upload_texture(&s_cache[slot].tex_og, resized,
                       COVER_RESIZE_W, COVER_RESIZE_H)) {
        s_cache[slot].has_og = true;
        cover_log("[COVER OK] Textura OG subida: %s", url);
    }

    /* Subir versión 1-bit */
    if (upload_texture(&s_cache[slot].tex_1bit, rgba_1bit,
                       COVER_RESIZE_W, COVER_RESIZE_H)) {
        s_cache[slot].has_1bit = true;
        cover_log("[COVER OK] Textura 1-bit subida: %s", url);
    }

    s_cache[slot].ready = s_cache[slot].has_og || s_cache[slot].has_1bit;

    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);

    free(resized);
    free(rgba_1bit);

    cover_log("[COVER READY] Caratula cacheada: %s (slot %d)", url, slot);
    s_cover_thread_running = false;
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
    memset(s_cache, 0, sizeof(s_cache));
    s_access_clock = 0;
    s_cover_mode = COVER_MODE_1BIT_DITHER;
    cover_log("[COVER] Caché LRU inicializado (%d slots, modo %s)",
              MAX_COVER_CACHE,
              s_cover_mode == COVER_MODE_1BIT_DITHER ? "1BIT" : "OG");
}

void cover_art_shutdown(void)
{
    s_cover_thread_running = false;
    cover_art_clear_all();
    cover_log("[COVER] Caché LRU detenido");
}

CoverArtMode cover_art_get_mode(void) { return s_cover_mode; }

void cover_art_set_mode(CoverArtMode mode)
{
    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
    s_cover_mode = mode;
    /* Invalidar texturas: se regenerarán al acceder según el nuevo modo */
    for (int i = 0; i < MAX_COVER_CACHE; i++) {
        if (s_cache[i].ready) {
            s_cache[i].last_access = ++s_access_clock;
        }
    }
    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
    cover_log("[COVER] Modo cambiado a %s",
              mode == COVER_MODE_1BIT_DITHER ? "1BIT DITHER" : "OG");
}

bool cover_art_load(const char *url, CoverArtMode mode)
{
    if (!url || url[0] == '\0') {
        cover_art_clear_all();
        return false;
    }

    /* Si ya está cached, solo actualizar modo */
    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
    int slot = cache_find_slot(url);
    if (slot >= 0 && s_cache[slot].ready) {
        s_cache[slot].last_access = ++s_access_clock;
        s_cover_mode = mode;
        if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
        return true;
    }
    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);

    /* Si hay un hilo corriendo, esperarlo */
    if (s_cover_thread_running) {
        uint64_t exit_val = 0;
        sysThreadJoin(s_cover_thread_id, &exit_val);
        s_cover_thread_running = false;
    }

    cover_art_mode_global = mode; /* se usará en el worker */

    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
    strncpy(s_pending_url, url, sizeof(s_pending_url) - 1);
    s_pending_url[sizeof(s_pending_url) - 1] = '\0';
    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);

    s_cover_thread_running = true;
    int ret = sysThreadCreate(&s_cover_thread_id, cover_art_worker, NULL, 2000,
                              0x20000, THREAD_JOINABLE, "mono_cover");
    if (ret != 0) {
        cover_log("[COVER ERROR] sysThreadCreate fallo: %d", ret);
        s_cover_thread_running = false;
        return false;
    }
    return true;
}

bool cover_art_get_texture(const char *url, GfxTexture *out)
{
    if (!url || !out) return false;

    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
    int slot = cache_find_slot(url);
    if (slot >= 0 && s_cache[slot].ready) {
        s_cache[slot].last_access = ++s_access_clock;
        if (s_cover_mode == COVER_MODE_1BIT_DITHER && s_cache[slot].has_1bit) {
            *out = s_cache[slot].tex_1bit;
            if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
            return true;
        }
        if (s_cover_mode == COVER_MODE_ORIGINAL && s_cache[slot].has_og) {
            *out = s_cache[slot].tex_og;
            if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
            return true;
        }
        /* Fallback: usar la que esté disponible */
        if (s_cache[slot].has_og) {
            *out = s_cache[slot].tex_og;
        } else if (s_cache[slot].has_1bit) {
            *out = s_cache[slot].tex_1bit;
        } else {
            if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
            return false;
        }
        if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
        return true;
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
