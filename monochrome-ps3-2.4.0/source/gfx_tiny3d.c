/*
 * Monochrome PS3 - GFX Engine Backend (RSX GPU / Tiny3D-compatible API)
 * Phase 2: GPU-accelerated rendering backend for Monochrome PS3.
 *
 * Implementa la API de gfx_engine.h sobre RSX directo (librsx/gcm_sys).
 * Mantiene la misma interfaz que Tiny3D para facilitar migración futura
 * a libtiny3d real cuando esté disponible en el toolchain.
 */

#include <ppu-lv2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>

#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>
#include <rsxutil.h>

#include "gfx_engine.h"
#include "platform.h"
#include "monochrome.h"

/* ── RSX screen buffers (modo CPU framebuffer, compatible con rsxutil) ── */
static gcmContextData *s_context = NULL;
static void           *s_host_addr = NULL;
static rsxBuffer       s_buffers[2];
static int             s_cur_buffer = 0;
static u16             s_screen_w = 1920;
static u16             s_screen_h = 1080;

/* Overscan / escala virtual 1920x1080 -> salida real */
static int   s_overscan_x = 0;
static int   s_overscan_y = 0;
static float s_overscan_scale = 1.0f;
static float s_scale_x = 1.0f;
static float s_scale_y = 1.0f;
static float s_offset_x = 0.0f;
static float s_offset_y = 0.0f;

/* ── 8x8 Bitmap Font (misma que la usada antes en platform_psl1ght.c) ── */
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

/* ── Textura temporal para platform_draw_texture bridge ────────── */
static GfxTexture s_tmp_tex = {0};
static bool       s_tmp_tex_allocated = false;

/* ── Helpers de coordenadas ────────────────────────────────────── */

static float vx(float x) { return s_offset_x + x * s_scale_x; }
static float vy(float y) { return s_offset_y + y * s_scale_y; }
static float vw(float w) { return w * s_scale_x; }
static float vh(float h) { return h * s_scale_y; }

/* ── Inicialización RSX ────────────────────────────────────────── */

int gfx_init(void)
{
    /* Allocate host memory for RSX command buffer */
    s_host_addr = memalign(1024 * 1024, HOST_SIZE);
    if (!s_host_addr) {
        platform_log("[GFX] Error: memalign fallo para host_addr");
        return -1;
    }
    platform_log("[GFX] host_addr asignado @%p (%d MB)",
                 s_host_addr, HOST_SIZE / (1024 * 1024));

    /* initScreen viene de rsxutil.h */
    s_context = initScreen(s_host_addr, HOST_SIZE);
    if (!s_context) {
        platform_log("[GFX] Error: initScreen fallo");
        /* No liberar s_host_addr: memoria RSX activa hasta sysProcessExit(0) */
        return -1;
    }
    platform_log("[GFX] initScreen OK (context @%p)", s_context);

    /* Resolución nativa */
    if (!getResolution(&s_screen_w, &s_screen_h)) {
        s_screen_w = 1920;
        s_screen_h = 1080;
    }
    platform_log("[GFX] Resolución nativa: %dx%d", s_screen_w, s_screen_h);

    /* Buffers de pantalla */
    for (int i = 0; i < 2; i++) {
        if (!makeBuffer(&s_buffers[i], s_screen_w, s_screen_h, i)) {
            platform_log("[GFX] Error: makeBuffer[%d] fallo", i);
            return -1;
        }
    }

    /* Escala virtual 1920x1080 -> salida real */
    s_scale_x = (float)s_screen_w / 1920.0f;
    s_scale_y = (float)s_screen_h / 1080.0f;
    s_offset_x = (float)s_overscan_x;
    s_offset_y = (float)s_overscan_y;

    flip(s_context, 1);
    waitFlip();
    s_cur_buffer = 0;

    platform_log("[GFX] Backend RSX inicializado (%dx%d, escala x=%.4f y=%.4f)",
                 s_screen_w, s_screen_h, s_scale_x, s_scale_y);
    return 0;
}

void gfx_shutdown(void)
{
    platform_log("[GFX] Cerrando backend RSX...");
    if (s_tmp_tex.data) {
        free(s_tmp_tex.data);
        s_tmp_tex.data = NULL;
        s_tmp_tex_allocated = false;
    }
    platform_log("[GFX] Backend RSX cerrado");
}

void gfx_begin_frame(uint32_t clear_rgba)
{
    rsxBuffer *b = &s_buffers[s_cur_buffer];
    int count = b->width * b->height;
    uint32_t color = clear_rgba;
    uint64_t color64 = ((uint64_t)color << 32) | (uint64_t)color;
    uint64_t *ptr64 = (uint64_t *)b->ptr;
    int count64 = count / 2;
    for (int i = 0; i < count64; i += 4) {
        ptr64[i]     = color64;
        ptr64[i + 1] = color64;
        ptr64[i + 2] = color64;
        ptr64[i + 3] = color64;
    }
    if (count & 1) {
        b->ptr[count - 1] = color;
    }
}

void gfx_end_frame(void)
{
    flip(s_context, s_buffers[s_cur_buffer].id);
    s_cur_buffer = (s_cur_buffer + 1) % 2;
    waitFlip();
}

/* ── Primitive 2D ──────────────────────────────────────────────── */

void gfx_draw_rect(float x, float y, float w, float h, uint32_t rgba)
{
    int ix = (int)vx(x);
    int iy = (int)vy(y);
    int iw = (int)vw(w);
    int ih = (int)vh(h);

    rsxBuffer *b = &s_buffers[s_cur_buffer];
    if (ix < 0) { iw += ix; ix = 0; }
    if (iy < 0) { ih += iy; iy = 0; }
    if (ix + iw > b->width)  iw = b->width - ix;
    if (iy + ih > b->height) ih = b->height - iy;
    if (iw <= 0 || ih <= 0) return;

    uint64_t color64 = ((uint64_t)rgba << 32) | (uint64_t)rgba;

    for (int r = iy; r < iy + ih; r++) {
        uint32_t *row = &b->ptr[r * b->width + ix];
        int rem = iw;
        if (((uintptr_t)row & 7) && rem > 0) {
            *row++ = rgba;
            rem--;
        }
        int w64 = rem / 2;
        uint64_t *row64 = (uint64_t *)row;
        for (int i = 0; i < w64; i++) {
            row64[i] = color64;
        }
        if (rem & 1) {
            row[w64 * 2] = rgba;
        }
    }
}

void gfx_draw_rect_outline(float x, float y, float w, float h,
                           uint32_t rgba, float thickness)
{
    int ix = (int)vx(x);
    int iy = (int)vy(y);
    int iw = (int)vw(w);
    int ih = (int)vh(h);
    int t = (int)(thickness * s_scale_x);
    if (t < 1) t = 1;

    rsxBuffer *b = &s_buffers[s_cur_buffer];

    /* Top */
    for (int r = iy; r < iy + t; r++) {
        if (r < 0 || r >= b->height) continue;
        if (ix < 0 || ix >= b->width) continue;
        uint32_t *row = &b->ptr[(size_t)r * b->width + ix];
        for (int c = 0; c < iw && ix + c < b->width; c++) row[c] = rgba;
    }
    /* Bottom */
    for (int r = iy + ih - t; r < iy + ih; r++) {
        if (r < 0 || r >= b->height) continue;
        if (ix < 0 || ix >= b->width) continue;
        uint32_t *row = &b->ptr[(size_t)r * b->width + ix];
        for (int c = 0; c < iw && ix + c < b->width; c++) row[c] = rgba;
    }
    /* Left */
    for (int r = iy + t; r < iy + ih - t; r++) {
        if (r < 0 || r >= b->height) continue;
        if (ix < 0 || ix >= b->width) continue;
        uint32_t *row = &b->ptr[(size_t)r * b->width + ix];
        for (int c = 0; c < t && ix + c < b->width; c++) row[c] = rgba;
    }
    /* Right */
    for (int r = iy + t; r < iy + ih - t; r++) {
        if (r < 0 || r >= b->height) continue;
        if (ix + iw - t < 0 || ix + iw - t >= b->width) continue;
        uint32_t *row = &b->ptr[(size_t)r * b->width + ix + iw - t];
        for (int c = 0; c < t && ix + iw - t + c < b->width; c++) row[c] = rgba;
    }
}

/* ── Texturas ──────────────────────────────────────────────────── */

int gfx_load_texture_png(const char *path, GfxTexture *out)
{
    if (!path || !out) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) {
        platform_log("[GFX] No se pudo abrir PNG: %s", path);
        return -1;
    }

    uint8_t sig[8];
    if (fread(sig, 1, 8, f) != 8 || memcmp(sig, "\x89PNG\r\n\x1a\n", 8) != 0) {
        fclose(f);
        platform_log("[GFX] PNG invalido: %s", path);
        return -1;
    }

    int w = 0, h = 0;
    while (!feof(f)) {
        uint8_t len_buf[4];
        if (fread(len_buf, 1, 4, f) != 4) break;
        uint32_t chunk_len = (len_buf[0] << 24) | (len_buf[1] << 16) |
                             (len_buf[2] << 8) | len_buf[3];

        uint8_t type[4];
        if (fread(type, 1, 4, f) != 4) break;

        if (memcmp(type, "IHDR", 4) == 0) {
            uint8_t ihdr[13];
            if (fread(ihdr, 1, 13, f) != 13) break;
            w = (ihdr[0] << 24) | (ihdr[1] << 16) | (ihdr[2] << 8) | ihdr[3];
            h = (ihdr[4] << 24) | (ihdr[5] << 16) | (ihdr[6] << 8) | ihdr[7];
            break;
        }
        if (fseek(f, (long)chunk_len + 4, SEEK_CUR) != 0) break;
    }
    fclose(f);

    if (w <= 0 || h <= 0) {
        platform_log("[GFX] No se pudieron leer dimensiones de: %s", path);
        return -1;
    }

    /* Leer datos RGBA (decodificación mínima: placeholders son 1-bit blanco) */
    size_t rgba_size = (size_t)w * (size_t)h * 4u;
    uint8_t *rgba = (uint8_t *)memalign(16, rgba_size);
    if (!rgba) {
        platform_log("[GFX] malloc fallo para RGBA (%lu B)", (unsigned long)rgba_size);
        return -1;
    }
    memset(rgba, 255, rgba_size); /* Blanco puro con alpha */

    /* Subir a VRAM usando la misma ruta que upload_rgba_texture */
    bool ok = gfx_upload_rgba_texture(rgba, w, h, out);
    free(rgba);

    if (ok) {
        platform_log("[GFX] Textura cargada: %s (%dx%d)", path, w, h);
        return 0;
    }
    return -1;
}

int gfx_upload_rgba_texture(const uint8_t *rgba, int w, int h, GfxTexture *out)
{
    if (!rgba || !out || w <= 0 || h <= 0) return -1;

    size_t rgba_size = (size_t)w * (size_t)h * 4u;
    uint8_t *data = (uint8_t *)memalign(16, rgba_size);
    if (!data) return -1;
    memcpy(data, rgba, rgba_size);

    if (out->data) free(out->data);

    out->data = data;
    out->offset = 0; /* CPU-side texture (software blit) */
    out->width = w;
    out->height = h;
    out->pitch = (uint32_t)w * 4u;
    out->uploaded = true;

    return 0;
}

/* ── Sprites y 9-slice (software blit al framebuffer) ─────────── */

static void blit_rgba(rsxBuffer *dst, int dx, int dy, int dw, int dh,
                      const uint8_t *src, int sw, int sh,
                      uint32_t tint_rgba)
{
    if (dx < 0) { dw += dx; dx = 0; }
    if (dy < 0) { dh += dy; dy = 0; }
    if (dx + dw > dst->width)  dw = dst->width - dx;
    if (dy + dh > dst->height) dh = dst->height - dy;
    if (dw <= 0 || dh <= 0) return;

    uint8_t t_r = (tint_rgba >> 16) & 0xFF;
    uint8_t t_g = (tint_rgba >> 8)  & 0xFF;
    uint8_t t_b = (tint_rgba)       & 0xFF;

    int bw = dst->width;
    for (int y = 0; y < dh; y++) {
        int sy = (y * sh) / dh;
        if (sy >= sh) sy = sh - 1;
        const uint8_t *src_scanline = &src[(size_t)sy * sw * 4];
        uint32_t *dst_row = &dst->ptr[(size_t)(dy + y) * bw + dx];

        for (int x = 0; x < dw; x++) {
            int sx = (x * sw) / dw;
            if (sx >= sw) sx = sw - 1;
            const uint8_t *p = &src_scanline[(size_t)sx * 4];
            uint8_t s_a = p[3];
            if (s_a == 0) continue;
            /* Aplicar tint y alpha blend simple */
            uint8_t r = (uint8_t)((p[0] * t_r / 255) * s_a / 255);
            uint8_t g = (uint8_t)((p[1] * t_g / 255) * s_a / 255);
            uint8_t b = (uint8_t)((p[2] * t_b / 255) * s_a / 255);
            dst_row[x] = ((uint32_t)s_a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

void gfx_draw_sprite(const GfxTexture *tex, float x, float y, float w, float h,
                     float u0, float v0, float u1, float v1, uint32_t tint_rgba)
{
    if (!tex || !tex->data || !tex->uploaded) return;

    int dx = (int)vx(x);
    int dy = (int)vy(y);
    int dw = (int)vw(w);
    int dh = (int)vh(h);

    int src_x0 = (int)(u0 * (float)tex->width);
    int src_y0 = (int)(v0 * (float)tex->height);
    int src_x1 = (int)(u1 * (float)tex->width);
    int src_y1 = (int)(v1 * (float)tex->height);
    int sw = src_x1 - src_x0;
    int sh = src_y1 - src_y0;
    if (sw <= 0) sw = tex->width;
    if (sh <= 0) sh = tex->height;

    const uint8_t *src = (const uint8_t *)tex->data + (size_t)src_y0 * tex->pitch + (size_t)src_x0 * 4u;

    rsxBuffer *b = &s_buffers[s_cur_buffer];
    blit_rgba(b, dx, dy, dw, dh, src, sw, sh, tint_rgba);
}

void gfx_draw_9slice(const GfxTexture *tex, float x, float y, float w, float h,
                     float border_px, uint32_t tint_rgba)
{
    if (!tex || !tex->data || !tex->uploaded) return;

    int dx = (int)vx(x);
    int dy = (int)vy(y);
    int dw = (int)vw(w);
    int dh = (int)vh(h);
    int b = (int)(border_px * s_scale_x);
    if (b < 1) b = 1;

    /* 9 regiones: corners, bordes, centro */
    int cx0 = dx, cx1 = dx + b, cx2 = dx + dw - b, cx3 = dx + dw;
    int cy0 = dy, cy1 = dy + b, cy2 = dy + dh - b, cy3 = dy + dh;

    const uint8_t *src = (const uint8_t *)tex->data;
    int tw = tex->width;
    int th = tex->height;
    int sb = (int)((float)b / (float)dw * (float)tw); /* border en src */
    if (sb < 1) sb = 1;

    /* UV regions en la textura fuente */
    struct { int sx0, sy0, sx1, sy1; int dx0, dy0, dx1, dy1; } regions[9] = {
        /* TL corner */
        { 0,          0,          sb,         sb,         cx0, cy0, cx1, cy1 },
        /* Top edge */
        { sb,         0,          tw - sb,    sb,         cx1, cy0, cx2, cy1 },
        /* TR corner */
        { tw - sb,    0,          tw,         sb,         cx2, cy0, cx3, cy1 },
        /* Left edge */
        { 0,          sb,         sb,         th - sb,    cx0, cy1, cx1, cy2 },
        /* Center */
        { sb,         sb,         tw - sb,    th - sb,    cx1, cy1, cx2, cy2 },
        /* Right edge */
        { tw - sb,    sb,         tw,         th - sb,    cx2, cy1, cx3, cy2 },
        /* BL corner */
        { 0,          th - sb,    sb,         th,         cx0, cy2, cx1, cy3 },
        /* Bottom edge */
        { sb,         th - sb,    tw - sb,    th,         cx1, cy2, cx2, cy3 },
        /* BR corner */
        { tw - sb,    th - sb,    tw,         th,         cx2, cy2, cx3, cy3 },
    };

    rsxBuffer *b_ = &s_buffers[s_cur_buffer];
    for (int i = 0; i < 9; i++) {
        int rw = regions[i].dx1 - regions[i].dx0;
        int rh = regions[i].dy1 - regions[i].dy0;
        if (rw <= 0 || rh <= 0) continue;

        const uint8_t *rsrc = src + (size_t)regions[i].sy0 * tw * 4u +
                              (size_t)regions[i].sx0 * 4u;
        int rsw = regions[i].sx1 - regions[i].sx0;
        int rsh = regions[i].sy1 - regions[i].sy0;

        blit_rgba(b_, regions[i].dx0, regions[i].dy0, rw, rh,
                  rsrc, rsw, rsh, tint_rgba);
    }
}

/* ── Texto (placeholder: bloques sólidos por carácter) ─────────── */

static int s_font_loaded = 0;

static void draw_glyph(rsxBuffer *b, int fx, int fy, float scale,
                       unsigned char ch, uint32_t rgba)
{
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8_t *g = FONT8[ch - 0x20];

    int gw = (int)(8.0f * scale);
    int gh = (int)(8.0f * scale);

    for (int r = 0; r < 8; r++) {
        uint8_t bits = g[r];
        if (!bits) continue;
        int py = fy + (int)(r * scale);
        if (py < 0 || py >= b->height) continue;
        uint32_t *row = &b->ptr[(size_t)py * b->width + fx];
        for (int c = 0; c < 8; c++) {
            if (!(bits & (1u << c))) continue;
            int px = fx + (int)(c * scale);
            if (px < 0 || px >= b->width) continue;
            for (int sx = 0; sx < (int)scale; sx++) {
                int px2 = px + sx;
                if (px2 < 0 || px2 >= b->width) continue;
                for (int sy = 0; sy < (int)scale; sy++) {
                    int py2 = py + sy;
                    if (py2 < 0 || py2 >= b->height) continue;
                    b->ptr[(size_t)py2 * b->width + px2] = rgba;
                }
            }
        }
    }
}

int gfx_draw_text(float x, float y, const char *utf8, float size_px, uint32_t rgba)
{
    if (!utf8 || !*utf8) return 0;
    if (size_px < 8.0f) size_px = 8.0f;
    if (size_px > 96.0f) size_px = 96.0f;
    if (!s_font_loaded) {
        platform_log("[GFX] Fuente bitmap 8x8 cargada (render bitmap)");
        s_font_loaded = 1;
    }

    int fx = (int)vx(x);
    int fy = (int)vy(y);
    float scale = size_px / 8.0f;
    if (scale < 1.0f) scale = 1.0f;

    rsxBuffer *b = &s_buffers[s_cur_buffer];
    int cur_x = fx;

    while (*utf8) {
        unsigned char ch = (unsigned char)*utf8++;
        draw_glyph(b, cur_x, fy, scale, ch, rgba);
        cur_x += (int)(8.0f * scale);
    }
    return cur_x - fx;
}

int gfx_draw_text_clipped(float x, float y, float max_w, const char *utf8,
                          float size_px, uint32_t rgba)
{
    if (!utf8 || !*utf8) return 0;
    float scale = size_px / 8.0f;
    if (scale < 1.0f) scale = 1.0f;

    int char_w = (int)(8.0f * scale);
    if (char_w < 1) char_w = 1;
    int max_chars = (int)(max_w / (float)char_w);
    if (max_chars <= 0) return 0;

    int len = (int)strlen(utf8);
    if (len <= max_chars) {
        return gfx_draw_text(x, y, utf8, size_px, rgba);
    }

    char buf[256];
    int copy_len = (max_chars > 3) ? (max_chars - 3) : max_chars;
    if (copy_len < 0) copy_len = 0;
    if (copy_len >= (int)sizeof(buf) - 4) copy_len = (int)sizeof(buf) - 4;
    memcpy(buf, utf8, copy_len);
    if (max_chars > 3) {
        buf[copy_len] = '.'; buf[copy_len + 1] = '.'; buf[copy_len + 2] = '.';
        buf[copy_len + 3] = '\0';
    } else {
        buf[copy_len] = '\0';
    }
    return gfx_draw_text(x, y, buf, size_px, rgba);
}

/* ── Visualizador 3D wireframe (líneas por software) ───────────── */

static void draw_line_sw(rsxBuffer *b, int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    while (true) {
        if (x0 >= 0 && x0 < b->width && y0 >= 0 && y0 < b->height) {
            b->ptr[(size_t)y0 * b->width + x0] = color;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void gfx_draw_wireframe_mesh(float cx, float cy, float scale,
                             const float *vis_bands, int num_bands,
                             float phase, uint32_t rgba)
{
    if (!vis_bands || num_bands <= 0) return;

    int fx = (int)vx(cx);
    int fy = (int)vy(cy);
    int grid = 20;
    float cell = 16.0f * scale;

    rsxBuffer *b = &s_buffers[s_cur_buffer];

    for (int i = 0; i < grid; i++) {
        for (int j = 0; j < grid; j++) {
            float x0 = (float)i - (float)grid / 2.0f;
            float y0 = (float)j - (float)grid / 2.0f;
            float x1 = x0 + 1.0f;
            float y1 = y0 + 1.0f;

            int bi = (i + (int)(phase * 10.0f)) % num_bands;
            float z0 = vis_bands[bi] * 40.0f * scale;
            int bj = (j + (int)(phase * 7.0f)) % num_bands;
            float z1 = vis_bands[bj] * 40.0f * scale;

            /* Desplazar Y según Z para efecto 3D */
            int px0 = fx + (int)(x0 * cell);
            int py0 = fy + (int)(y0 * cell) - (int)z0;
            int px1 = fx + (int)(x1 * cell);
            int py1 = fy + (int)(y0 * cell) - (int)z1;
            int px2 = fx + (int)(x0 * cell);
            int py2 = fy + (int)(y1 * cell) - (int)z0;

            draw_line_sw(b, px0, py0, px1, py1, rgba);
            draw_line_sw(b, px0, py0, px2, py2, rgba);
        }
    }
}

/* ── Utilidades ────────────────────────────────────────────────── */

int gfx_get_width(void)  { return s_screen_w; }
int gfx_get_height(void) { return s_screen_h; }

void gfx_set_overscan(int offset_x, int offset_y, float scale)
{
    s_overscan_x = offset_x;
    s_overscan_y = offset_y;
    s_overscan_scale = scale;
    s_offset_x = (float)offset_x;
    s_offset_y = (float)offset_y;
    s_scale_x = scale * ((float)s_screen_w / 1920.0f);
    s_scale_y = scale * ((float)s_screen_h / 1080.0f);
}
