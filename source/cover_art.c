/*
 * Monochrome PS3 - Cover Art Downloader & Decoder
 * Downloads JPEG artwork from tracks.monochrome.st and renders to RSX
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

#include <sysmodule/sysmodule.h>
#include <net/net.h>
#include <net/netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <jpeglib.h>

#include "cover_art.h"
#include "platform.h"

#define COVER_LOG_PATH "/dev_hdd0/tmp/mono_cover.log"
#define API_HOST "tracks.monochrome.st"
#define API_PORT 80
#define MAX_COVER_SIZE (512 * 1024)  // 512KB max for cover art

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
    bool loaded;
    int width;
    int height;
    uint8_t *rgba_data;  // RGBA8888 pixel data
    void *rsx_texture;   // RSX texture pointer
} cover_state_t;

static cover_state_t s_cover = {0};

void cover_art_init(void)
{
    memset(&s_cover, 0, sizeof(s_cover));
    cover_log("[COVER] Sistema de portadas inicializado");
}

void cover_art_shutdown(void)
{
    cover_art_clear();
    cover_log("[COVER] Sistema de portadas cerrado");
}

void cover_art_clear(void)
{
    if (s_cover.rgba_data) {
        free(s_cover.rgba_data);
        s_cover.rgba_data = NULL;
    }
    // TODO: Free RSX texture if allocated
    s_cover.rsx_texture = NULL;
    s_cover.loaded = false;
    s_cover.width = 0;
    s_cover.height = 0;
}

bool cover_art_is_ready(void)
{
    return s_cover.loaded && s_cover.rgba_data != NULL;
}

void *cover_art_get_texture(void)
{
    return s_cover.rgba_data;  // Return RGBA pixel data, not RSX texture pointer
}

int cover_art_get_width(void)
{
    return s_cover.width;
}

int cover_art_get_height(void)
{
    return s_cover.height;
}

/* Decode JPEG from memory buffer to RGBA */
static bool decode_jpeg_to_rgba(const uint8_t *jpeg_data, size_t jpeg_size, 
                                 uint8_t **out_rgba, int *out_width, int *out_height)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPARRAY buffer;
    int row_stride;
    
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)jpeg_data, jpeg_size);
    
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        cover_log("[COVER ERROR] JPEG header invalido");
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    
    *out_width = cinfo.output_width;
    *out_height = cinfo.output_height;
    row_stride = cinfo.output_width * cinfo.output_components;
    
    // Allocate RGBA buffer (4 bytes per pixel)
    size_t rgba_size = (*out_width) * (*out_height) * 4;
    *out_rgba = (uint8_t *)malloc(rgba_size);
    if (!*out_rgba) {
        cover_log("[COVER ERROR] malloc fallo para RGBA buffer (%lu bytes)", (unsigned long)rgba_size);
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    
    buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);
    
    uint8_t *dst = *out_rgba;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, buffer, 1);
        
        // Convert RGB to RGBA
        uint8_t *src = buffer[0];
        for (int x = 0; x < cinfo.output_width; x++) {
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
    
    cover_log("[COVER] JPEG decodificado: %dx%d RGB -> RGBA", *out_width, *out_height);
    return true;
}

bool cover_art_load(const char *url)
{
    if (!url || url[0] == '\0') {
        cover_log("[COVER ERROR] URL vacia");
        return false;
    }
    
    cover_art_clear();
    cover_log("[COVER] Descargando: %s", url);
    
    // Parse URL to extract path
    const char *path_start = strstr(url, "://");
    if (!path_start) {
        cover_log("[COVER ERROR] URL invalida (no tiene ://)");
        return false;
    }
    path_start += 3;
    const char *path = strchr(path_start, '/');
    if (!path) {
        cover_log("[COVER ERROR] URL invalida (no tiene path)");
        return false;
    }
    
    // DNS resolve using gethostbyname (PSL1GHT compatible)
    struct hostent *he = gethostbyname(API_HOST);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        cover_log("[COVER ERROR] DNS resolve fallo para %s (errno: %d)", API_HOST, errno);
        return false;
    }
    
    struct in_addr **addr_list = (struct in_addr **)he->h_addr_list;
    char ip_str[64];
    strncpy(ip_str, inet_ntoa(*addr_list[0]), sizeof(ip_str) - 1);
    ip_str[sizeof(ip_str) - 1] = '\0';
    cover_log("[COVER] DNS: %s -> %s", API_HOST, ip_str);
    
    // TCP connect
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        cover_log("[COVER ERROR] socket() fallo: %d", errno);
        return false;
    }
    
    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(API_PORT);
    memcpy(&saddr.sin_addr, addr_list[0], sizeof(struct in_addr));
    
    if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        cover_log("[COVER ERROR] connect() fallo: %d", errno);
        close(sock);
        return false;
    }
    cover_log("[COVER] TCP conectado a %s:%d", ip_str, API_PORT);
    
    // HTTP GET request
    char req[1024];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: Monochrome-PS3/1.0\r\n"
             "Accept: image/jpeg, image/*\r\n"
             "Connection: close\r\n\r\n",
             path, API_HOST);
    
    if (send(sock, req, strlen(req), 0) <= 0) {
        cover_log("[COVER ERROR] send HTTP request fallo");
        close(sock);
        return false;
    }
    
    // Read HTTP response headers
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
        cover_log("[COVER ERROR] No se encontro fin de headers HTTP");
        close(sock);
        return false;
    }
    
    // Check HTTP status
    int http_code = 0;
    sscanf(head_buf, "HTTP/%*s %d", &http_code);
    if (http_code != 200) {
        cover_log("[COVER ERROR] HTTP %d (esperaba 200)", http_code);
        close(sock);
        return false;
    }
    
    // Allocate buffer for JPEG data
    uint8_t *jpeg_buf = (uint8_t *)malloc(MAX_COVER_SIZE);
    if (!jpeg_buf) {
        cover_log("[COVER ERROR] malloc fallo para JPEG buffer");
        close(sock);
        return false;
    }
    
    size_t jpeg_size = 0;
    
    // Copy any body data already in header buffer
    size_t initial_body = head_len - (body_start - head_buf);
    if (initial_body > 0 && initial_body < MAX_COVER_SIZE) {
        memcpy(jpeg_buf, body_start, initial_body);
        jpeg_size = initial_body;
    }
    
    // Receive rest of JPEG data
    while (jpeg_size < MAX_COVER_SIZE) {
        size_t to_read = MAX_COVER_SIZE - jpeg_size;
        if (to_read > 8192) to_read = 8192;
        
        ssize_t n = recv(sock, jpeg_buf + jpeg_size, to_read, 0);
        if (n <= 0) break;
        jpeg_size += (size_t)n;
    }
    
    close(sock);
    
    if (jpeg_size < 100) {
        cover_log("[COVER ERROR] JPEG muy pequeno: %lu bytes", (unsigned long)jpeg_size);
        free(jpeg_buf);
        return false;
    }
    
    cover_log("[COVER] JPEG descargado: %lu bytes", (unsigned long)jpeg_size);
    
    // Decode JPEG to RGBA
    uint8_t *rgba = NULL;
    int width = 0, height = 0;
    
    if (!decode_jpeg_to_rgba(jpeg_buf, jpeg_size, &rgba, &width, &height)) {
        cover_log("[COVER ERROR] decode_jpeg_to_rgba fallo");
        free(jpeg_buf);
        return false;
    }
    
    free(jpeg_buf);
    
    // Store in state
    s_cover.rgba_data = rgba;
    s_cover.width = width;
    s_cover.height = height;
    s_cover.loaded = true;
    
    cover_log("[COVER OK] Portada cargada: %dx%d pixels", width, height);
    return true;
}
