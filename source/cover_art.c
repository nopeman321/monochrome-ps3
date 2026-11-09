/*
 * Monochrome PS3 - Asynchronous Cover Art Downloader & Decoder (EGM Hardened)
 * Non-blocking PPU thread execution + setjmp/longjmp libjpeg crash resilience
 */

#include <ppu-lv2.h>
#include <stdio.h>
#include <stdlib.h>
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

#define COVER_LOG_PATH "/dev_hdd0/tmp/mono_cover.log"
#define DEFAULT_API_HOST "tracks.monochrome.st"
#define DEFAULT_API_PORT 80
#define MAX_COVER_SIZE (512 * 1024)

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

typedef struct {
    volatile bool loaded;
    volatile int width;
    volatile int height;
    uint8_t *rgba_data;
    void *rsx_texture;
} cover_state_t;

static cover_state_t s_cover = {0};

/* Asynchronous Thread State */
static sys_ppu_thread_t s_cover_thread_id;
static volatile bool s_cover_thread_running = false;
static char s_current_url[512] = "";
static char s_pending_url[512] = "";
static sys_mutex_t s_cover_mutex;
static bool s_mutex_initialized = false;

/* Custom libjpeg error manager to prevent exit(1) crash */
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

void cover_art_init(void)
{
    if (!s_mutex_initialized) {
        sys_mutex_attr_t attr;
        sysMutexAttrInitialize(attr);
        if (sysMutexCreate(&s_cover_mutex, &attr) == 0) {
            s_mutex_initialized = true;
        }
    }
    cover_art_clear();
    cover_log("[COVER] Sistema de portadas asincrono inicializado");
}

void cover_art_shutdown(void)
{
    if (s_cover_thread_running) {
        uint64_t exit_val = 0;
        sysThreadJoin(s_cover_thread_id, &exit_val);
        s_cover_thread_running = false;
    }
    cover_art_clear();
    if (s_mutex_initialized) {
        sysMutexDestroy(s_cover_mutex);
        s_mutex_initialized = false;
    }
    cover_log("[COVER] Sistema de portadas cerrado limpiamente");
}

void cover_art_clear(void)
{
    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
    if (s_cover.rgba_data) {
        free(s_cover.rgba_data);
        s_cover.rgba_data = NULL;
    }
    s_cover.rsx_texture = NULL;
    s_cover.loaded = false;
    s_cover.width = 0;
    s_cover.height = 0;
    s_current_url[0] = '\0';
    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);
}

bool cover_art_is_ready(void)
{
    return s_cover.loaded && s_cover.rgba_data != NULL;
}

void *cover_art_get_texture(void)
{
    return (void *)s_cover.rgba_data;
}

int cover_art_get_width(void)
{
    return s_cover.width;
}

int cover_art_get_height(void)
{
    return s_cover.height;
}

/* Safe JPEG Decoder with setjmp trampoline */
static bool decode_jpeg_to_rgba(const uint8_t *jpeg_data, size_t jpeg_size, 
                                uint8_t **out_rgba, int *out_width, int *out_height)
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
        cover_log("[COVER ERROR] Decodificacion abortada de forma segura (sin crash)");
        jpeg_destroy_decompress(&cinfo);
        if (*out_rgba) {
            free(*out_rgba);
            *out_rgba = NULL;
        }
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

    *out_width = (int)cinfo.output_width;
    *out_height = (int)cinfo.output_height;
    row_stride = (int)cinfo.output_width * (int)cinfo.output_components;

    size_t rgba_size = (size_t)(*out_width) * (size_t)(*out_height) * 4u;
    *out_rgba = (uint8_t *)malloc(rgba_size);
    if (!*out_rgba) {
        cover_log("[COVER ERROR] malloc fallo para RGBA (%lu B)", (unsigned long)rgba_size);
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);

    uint8_t *dst = *out_rgba;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, buffer, 1);
        uint8_t *src = buffer[0];
        for (int x = 0; x < *out_width; x++) {
            dst[0] = src[0];  // R
            dst[1] = src[1];  // G
            dst[2] = src[2];  // B
            dst[3] = 255;     // A
            src += 3;
            dst += 4;
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    cover_log("[COVER OK] JPEG decodificado: %dx%d RGB -> RGBA", *out_width, *out_height);
    return true;
}

/* Worker Thread Function */
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

    cover_log("[COVER THREAD] Descargando portada: %s", url);

    /* Dynamic URL Parsing */
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

    struct in_addr **addr_list = (struct in_addr **)he->h_addr_list;
    char ip_str[64] = "0.0.0.0";
    if (addr_list[0]) {
        strncpy(ip_str, inet_ntoa(*addr_list[0]), sizeof(ip_str) - 1);
    }
    cover_log("[COVER] Conectando a %s (%s:%d)...", host, ip_str, port);

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
    memcpy(&saddr.sin_addr, addr_list[0], sizeof(struct in_addr));

    if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        cover_log("[COVER ERROR] connect() fallo a %s:%d (errno: %d)", host, port, errno);
        close(sock);
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }

    char req[1024];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: Monochrome-PS3/2.3.0 (PSL1GHT; CellOS)\r\n"
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
        if (body_start) {
            body_start += 4;
            break;
        }
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
        cover_log("[COVER ERROR] Servidor devolvio HTTP %d", http_code);
        close(sock);
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }

    uint8_t *jpeg_buf = (uint8_t *)malloc(MAX_COVER_SIZE);
    if (!jpeg_buf) {
        cover_log("[COVER ERROR] malloc fallo para buffer JPEG");
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

    uint8_t *rgba = NULL;
    int width = 0, height = 0;

    if (!decode_jpeg_to_rgba(jpeg_buf, jpeg_size, &rgba, &width, &height)) {
        cover_log("[COVER ERROR] decode_jpeg_to_rgba fallo");
        free(jpeg_buf);
        s_cover_thread_running = false;
        sysThreadExit(0);
        return;
    }

    free(jpeg_buf);

    /* Atomic state transition */
    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
    if (s_cover.rgba_data) free(s_cover.rgba_data);
    s_cover.rgba_data = rgba;
    s_cover.width = width;
    s_cover.height = height;
    s_cover.loaded = true;
    strncpy(s_current_url, url, sizeof(s_current_url) - 1);
    s_current_url[sizeof(s_current_url) - 1] = '\0';
    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);

    cover_log("[COVER READY] Caratula lista de forma asincrona: %dx%d pixels", width, height);
    s_cover_thread_running = false;
    sysThreadExit(0);
}

bool cover_art_load(const char *url)
{
    if (!url || url[0] == '\0') {
        cover_art_clear();
        return false;
    }

    if (s_cover.loaded && strcmp(s_current_url, url) == 0) {
        return true;  /* Already loaded */
    }

    /* If thread is currently busy, join it before launching new request */
    if (s_cover_thread_running) {
        uint64_t exit_val = 0;
        sysThreadJoin(s_cover_thread_id, &exit_val);
        s_cover_thread_running = false;
    }

    cover_art_clear();

    if (s_mutex_initialized) sysMutexLock(s_cover_mutex, 0);
    strncpy(s_pending_url, url, sizeof(s_pending_url) - 1);
    s_pending_url[sizeof(s_pending_url) - 1] = '\0';
    if (s_mutex_initialized) sysMutexUnlock(s_cover_mutex);

    s_cover_thread_running = true;
    int ret = sysThreadCreate(&s_cover_thread_id, cover_art_worker, NULL, 1500,
                              0x20000, THREAD_JOINABLE, "mono_cover");
    if (ret != 0) {
        cover_log("[COVER ERROR] sysThreadCreate fallo: %d", ret);
        s_cover_thread_running = false;
        return false;
    }

    return true;
}
