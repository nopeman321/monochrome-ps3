/*
 * Monochrome PS3 - Platform Implementation for PSL1GHT
 * Separated hardware abstraction layer (Rule: mantener separada la capa de plataforma)
 */

#include <ppu-lv2.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include <sysutil/video.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <io/pad.h>

#include "platform.h"
#include "rsxutil.h"

#define STAGE_LOG_PATH "/dev_hdd0/tmp/mono_stage6.log"
#define MAX_BUFFERS 2

static gcmContextData *s_context = NULL;
static void *s_host_addr = NULL;
static rsxBuffer s_buffers[MAX_BUFFERS];
static int s_cur_buffer = 0;
static u16 s_screen_w = 0;
static u16 s_screen_h = 0;

/* Screen adjustment for overscan compensation */
static int s_screen_offset_x = 0;
static int s_screen_offset_y = 0;
static float s_screen_scale = 1.0f;
static float s_font_scale = 1.0f;

static uint16_t s_old_buttons = 0;

/* ── 8x8 Bitmap Font ── */
static const uint8_t FONT8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 20 SPC */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* 21 !   */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* 22 "   */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* 23 #   */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* 24 $   */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* 25 %   */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* 26 &   */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* 27 '   */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* 28 (   */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* 29 )   */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 2A *   */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* 2B +   */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* 2C ,   */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* 2D -   */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* 2E .   */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* 2F /   */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 30 0   */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 31 1   */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 32 2   */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 33 3   */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 34 4   */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 35 5   */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 36 6   */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 37 7   */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 38 8   */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 39 9   */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* 3A :   */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* 3B ;   */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* 3C <   */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* 3D =   */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* 3E >   */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* 3F ?   */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* 40 @   */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* 41 A   */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* 42 B   */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* 43 C   */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* 44 D   */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, /* 45 E   */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, /* 46 F   */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* 47 G   */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* 48 H   */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 49 I   */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* 4A J   */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* 4B K   */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, /* 4C L   */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* 4D M   */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* 4E N   */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* 4F O   */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, /* 50 P   */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* 51 Q   */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* 52 R   */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* 53 S   */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 54 T   */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* 55 U   */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 56 V   */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* 57 W   */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* 58 X   */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* 59 Y   */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* 5A Z   */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* 5B [   */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* 5C \   */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* 5D ]   */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* 5E ^   */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* 5F _   */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* 60 `   */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* 61 a   */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* 62 b   */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* 63 c   */
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, /* 64 d   */
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, /* 65 e   */
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, /* 66 f   */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* 67 g   */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* 68 h   */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* 69 i   */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* 6A j   */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* 6B k   */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 6C l   */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* 6D m   */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* 6E n   */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* 6F o   */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* 70 p   */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* 71 q   */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* 72 r   */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* 73 s   */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* 74 t   */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* 75 u   */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 76 v   */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* 77 w   */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* 78 x   */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* 79 y   */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* 7A z   */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* 7B {   */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* 7C |   */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* 7D }   */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /* 7E ~   */
};

void platform_log(const char *fmt, ...)
{
    FILE *f = fopen(STAGE_LOG_PATH, "a");
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

uint64_t platform_get_time_usec(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

int platform_init(void)
{
    platform_log("=== MONOCHROME MUSIC STREAMER: INICIO ===");

    s_host_addr = memalign(1024 * 1024, HOST_SIZE);
    if (!s_host_addr) {
        platform_log("[ERROR] Fallo memalign host_addr");
        return -1;
    }
    platform_log("[MEM] host_addr asignado @%p (tamano: %d MB)", s_host_addr, HOST_SIZE / (1024 * 1024));

    s_context = initScreen(s_host_addr, HOST_SIZE);
    if (!s_context) {
        platform_log("[ERROR] initScreen fallo");
        free(s_host_addr);
        return -1;
    }
    platform_log("[RSX] initScreen OK (context @%p)", s_context);

    int pret = ioPadInit(7);
    platform_log("[PAD] ioPadInit(7) retorno %d", pret);

    if (!getResolution(&s_screen_w, &s_screen_h)) {
        s_screen_w = 1280;
        s_screen_h = 720;
    }
    platform_log("[VIDEO] Resolucion nativa: %dx%d", s_screen_w, s_screen_h);

    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (!makeBuffer(&s_buffers[i], s_screen_w, s_screen_h, i)) {
            platform_log("[ERROR] makeBuffer[%d] fallo", i);
            return -1;
        }
    }
    platform_log("[BUFFER] 2 buffers asignados en RSX (%dx%d, %d bytes c/u)",
                 s_screen_w, s_screen_h, s_screen_w * s_screen_h * 4);

    flip(s_context, MAX_BUFFERS - 1);
    s_cur_buffer = 0;

    /* Load saved screen scale/offset before first render */
    platform_load_screen_config();
    return 0;
}

void platform_shutdown(void)
{
    platform_log("[CLEANUP] Liberando recursos de plataforma...");

    if (s_context) {
        gcmSetWaitFlip(s_context);
    }

    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (s_buffers[i].ptr) {
            rsxFree(s_buffers[i].ptr);
            s_buffers[i].ptr = NULL;
        }
    }
    platform_log("[CLEANUP] rsxBuffer liberados");

    if (s_context) {
        rsxFinish(s_context, 1);
        platform_log("[CLEANUP] rsxFinish OK");
    }

    if (s_host_addr) {
        free(s_host_addr);
        platform_log("[CLEANUP] host_addr liberado");
    }

    ioPadEnd();
    platform_log("[CLEANUP] ioPadEnd OK");
    platform_log("salida limpia");
}

int platform_get_width(void)  { return (int)s_screen_w; }
int platform_get_height(void) { return (int)s_screen_h; }

/* Screen adjustment functions */
void platform_set_screen_adjust(int offset_x, int offset_y, float scale)
{
    s_screen_offset_x = offset_x;
    s_screen_offset_y = offset_y;
    s_screen_scale = scale;
    platform_log("[SCREEN] Ajuste: offset=(%d, %d), scale=%.2f", offset_x, offset_y, scale);
}

void platform_get_screen_adjust(int *offset_x, int *offset_y, float *scale)
{
    if (offset_x) *offset_x = s_screen_offset_x;
    if (offset_y) *offset_y = s_screen_offset_y;
    if (scale) *scale = s_screen_scale;
}

void platform_set_font_scale(float scale)
{
    s_font_scale = scale;
}

float platform_get_font_scale(void)
{
    return s_font_scale;
}

#define SCREEN_CONFIG_PATH "/dev_hdd0/tmp/monochrome/screen_config.txt"

int platform_save_screen_config(void)
{
    FILE *f = fopen(SCREEN_CONFIG_PATH, "w");
    if (!f) {
        platform_log("[SCREEN ERROR] No se pudo guardar config en %s", SCREEN_CONFIG_PATH);
        return -1;
    }
    fprintf(f, "%d\n%d\n%.4f\n%.4f\n", s_screen_offset_x, s_screen_offset_y, s_screen_scale, s_font_scale);
    fclose(f);
    platform_log("[SCREEN] Config guardada: offset=(%d, %d), scale=%.4f", 
                 s_screen_offset_x, s_screen_offset_y, s_screen_scale);
    return 0;
}

int platform_load_screen_config(void)
{
    FILE *f = fopen(SCREEN_CONFIG_PATH, "r");
    if (!f) {
        platform_log("[SCREEN] No hay config guardada, usando valores por defecto");
        return -1;
    }
    int ox, oy;
    float sc, fs = 1.0f;
    if (fscanf(f, "%d\n%d\n%f\n%f\n", &ox, &oy, &sc, &fs) == 4) {
        s_screen_offset_x = ox;
        s_screen_offset_y = oy;
        s_screen_scale = sc;
        s_font_scale = fs;
        platform_log("[SCREEN] Config cargada: offset=(%d, %d), scale=%.4f, font=%.4f", ox, oy, sc, fs);
        fclose(f);
        return 0;
    }
    /* backward compat: 3 lines */
    if (fscanf(f, "%d\n%d\n%f\n", &ox, &oy, &sc) == 3) {
        s_screen_offset_x = ox;
        s_screen_offset_y = oy;
        s_screen_scale = sc;
        platform_log("[SCREEN] Config cargada: offset=(%d, %d), scale=%.4f", ox, oy, sc);
        fclose(f);
        return 0;
    }
    fclose(f);
    platform_log("[SCREEN ERROR] Config invalida, usando valores por defecto");
    return -1;
}

void platform_clear(uint32_t color)
{
    rsxBuffer *b = &s_buffers[s_cur_buffer];
    int count = b->width * b->height;
    for (int i = 0; i < count; i++) {
        b->ptr[i] = color;
    }
}

void platform_draw_rect(int x, int y, int w, int h, uint32_t color)
{
    rsxBuffer *b = &s_buffers[s_cur_buffer];
    
    /* Apply screen adjustment */
    x = s_screen_offset_x + (int)(x * s_screen_scale);
    y = s_screen_offset_y + (int)(y * s_screen_scale);
    w = (int)(w * s_screen_scale);
    h = (int)(h * s_screen_scale);
    
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > b->width)  w = b->width - x;
    if (y + h > b->height) h = b->height - y;
    if (w <= 0 || h <= 0) return;

    for (int r = y; r < y + h; r++) {
        uint32_t *row = &b->ptr[r * b->width + x];
        for (int c = 0; c < w; c++) {
            row[c] = color;
        }
    }
}

static inline void draw_pixel(rsxBuffer *b, int x, int y, uint32_t color)
{
    if ((unsigned)x < (unsigned)b->width && (unsigned)y < (unsigned)b->height) {
        b->ptr[y * b->width + x] = color;
    }
}

int platform_draw_text(int x, int y, const char *str, uint32_t color, int scale)
{
    rsxBuffer *b = &s_buffers[s_cur_buffer];
    
    /* Apply screen adjustment */
    int adj_x = s_screen_offset_x + (int)(x * s_screen_scale);
    int adj_y = s_screen_offset_y + (int)(y * s_screen_scale);
    int adj_scale = (int)(scale * s_screen_scale * s_font_scale);
    if (adj_scale < 1) adj_scale = 1;
    
    int cur_x = adj_x;
    while (*str) {
        unsigned char ch = (unsigned char)*str++;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        const uint8_t *g = FONT8[ch - 0x20];
        for (int r = 0; r < 8; r++) {
            uint8_t bits = g[r];
            for (int c = 0; c < 8; c++) {
                if (bits & (1u << c)) {
                    for (int sy = 0; sy < adj_scale; sy++) {
                        for (int sx = 0; sx < adj_scale; sx++) {
                            draw_pixel(b, cur_x + c * adj_scale + sx, adj_y + r * adj_scale + sy, color);
                        }
                    }
                }
            }
        }
        cur_x += 8 * adj_scale;
    }
    return cur_x - adj_x;
}

int platform_draw_text_r(int rx, int y, const char *str, uint32_t color, int scale)
{
    int len = (int)strlen(str) * 8 * scale;
    return platform_draw_text(rx - len, y, str, color, scale);
}

void platform_draw_texture(const uint8_t *texture_data, int tex_w, int tex_h,
                           int dst_x, int dst_y, int dst_w, int dst_h)
{
    if (!texture_data || tex_w <= 0 || tex_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    
    rsxBuffer *b = &s_buffers[s_cur_buffer];
    
    /* Apply screen adjustment */
    dst_x = s_screen_offset_x + (int)(dst_x * s_screen_scale);
    dst_y = s_screen_offset_y + (int)(dst_y * s_screen_scale);
    dst_w = (int)(dst_w * s_screen_scale);
    dst_h = (int)(dst_h * s_screen_scale);
    
    // Clip destination rectangle
    if (dst_x < 0) { dst_w += dst_x; dst_x = 0; }
    if (dst_y < 0) { dst_h += dst_y; dst_y = 0; }
    if (dst_x + dst_w > b->width)  dst_w = b->width - dst_x;
    if (dst_y + dst_h > b->height) dst_h = b->height - dst_y;
    if (dst_w <= 0 || dst_h <= 0) return;
    
    // Simple nearest-neighbor scaling from texture to destination
    for (int dy = 0; dy < dst_h; dy++) {
        int ty = (dy * tex_h) / dst_h;
        if (ty >= tex_h) ty = tex_h - 1;
        
        for (int dx = 0; dx < dst_w; dx++) {
            int tx = (dx * tex_w) / dst_w;
            if (tx >= tex_w) tx = tex_w - 1;
            
            // Read RGBA from texture
            int tex_offset = (ty * tex_w + tx) * 4;
            uint8_t r = texture_data[tex_offset + 0];
            uint8_t g = texture_data[tex_offset + 1];
            uint8_t b_val = texture_data[tex_offset + 2];
            uint8_t a = texture_data[tex_offset + 3];
            
            // Convert RGBA to XRGB (buffer format)
            // Assuming buffer is XRGB8888 format
            uint32_t pixel = (a << 24) | (r << 16) | (g << 8) | b_val;
            
            // Write to buffer
            int buf_x = dst_x + dx;
            int buf_y = dst_y + dy;
            if ((unsigned)buf_x < (unsigned)b->width && (unsigned)buf_y < (unsigned)b->height) {
                b->ptr[buf_y * b->width + buf_x] = pixel;
            }
        }
    }
}

void platform_flip(void)
{
    flip(s_context, s_buffers[s_cur_buffer].id);
    s_cur_buffer = (s_cur_buffer + 1) % MAX_BUFFERS;
    waitFlip();
}

InputAction platform_poll_input(void)
{
    padInfo padinfo;
    padData paddata;

    ioPadGetInfo(&padinfo);
    for (int i = 0; i < MAX_PADS; i++) {
        if (padinfo.status[i]) {
            if (ioPadGetData(i, &paddata) == 0 && paddata.len > 0) {
                uint16_t cur = 0;
                if (paddata.BTN_LEFT)     cur |= (1 << 0);
                if (paddata.BTN_RIGHT)    cur |= (1 << 1);
                if (paddata.BTN_UP)       cur |= (1 << 2);
                if (paddata.BTN_DOWN)     cur |= (1 << 3);
                if (paddata.BTN_CROSS)    cur |= (1 << 4);
                if (paddata.BTN_CIRCLE)   cur |= (1 << 5);
                if (paddata.BTN_SQUARE)   cur |= (1 << 6);
                if (paddata.BTN_TRIANGLE) cur |= (1 << 7);
                if (paddata.BTN_START)    cur |= (1 << 8);
                if (paddata.BTN_SELECT)   cur |= (1 << 9);
                if (paddata.BTN_L1)       cur |= (1 << 10);
                if (paddata.BTN_R1)       cur |= (1 << 11);
                if (paddata.BTN_L2)       cur |= (1 << 12);
                if (paddata.BTN_R2)       cur |= (1 << 13);

                uint16_t pressed = cur & ~s_old_buttons;
                s_old_buttons = cur;

                if (pressed & (1 << 2))  return ACT_UP;
                if (pressed & (1 << 3))  return ACT_DOWN;
                if (pressed & (1 << 0))  return ACT_LEFT;
                if (pressed & (1 << 1))  return ACT_RIGHT;
                if (pressed & (1 << 4))  return ACT_CROSS;
                if (pressed & (1 << 5))  return ACT_CIRCLE;
                if (pressed & (1 << 6))  return ACT_SQUARE;
                if (pressed & (1 << 7))  return ACT_TRIANGLE;
                if (pressed & (1 << 10)) return ACT_L1;
                if (pressed & (1 << 11)) return ACT_R1;
                if (pressed & (1 << 12)) return ACT_L2;
                if (pressed & (1 << 13)) return ACT_R2;
                if (pressed & (1 << 9))  return ACT_SELECT;
                if (pressed & (1 << 8))  return ACT_START;

                return ACT_NONE;
            }
        }
    }
    s_old_buttons = 0;
    return ACT_NONE;
}
