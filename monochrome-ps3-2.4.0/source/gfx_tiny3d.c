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
#include <png.h>

#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>
#include <rsxutil.h>

#include "gfx_engine.h"
#include "platform.h"
#include "monochrome.h"
#include "bg_fracture_hud_png.h"
#include "bg_matte_slabs_png.h"
#include "bg_liquid_chrome_png.h"

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

/* XDR shadow framebuffer and pre-baked background state */
static uint32_t *s_shadow_fb     = NULL;
static uint32_t *s_baked_bg_xdr  = NULL;
static int       s_bg_theme      = BG_THEME_FRACTURE_HUD;
static float     s_bg_brightness = 1.0f;
static bool      s_bg_dirty      = true;

static void bake_background(void);

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

    /* Allocate XDR shadow framebuffer for zero-VRAM-read CPU compositing */
    size_t fb_bytes = (size_t)s_screen_w * (size_t)s_screen_h * 4u;
    s_shadow_fb = (uint32_t *)memalign(64, fb_bytes);
    if (!s_shadow_fb) {
        platform_log("[GFX] Error: memalign failed for s_shadow_fb (%lu B)",
                     (unsigned long)fb_bytes);
        return -1;
    }
    size_t fb_pixels = (size_t)s_screen_w * (size_t)s_screen_h;
    for (size_t i = 0; i < fb_pixels; i++) {
        s_shadow_fb[i] = 0xFF000000u;
    }

    /* Virtual 1920x1080 -> native centered output with overscan + pre-baked bg */
    gfx_set_overscan(s_overscan_x, s_overscan_y, s_overscan_scale);
    if (s_bg_dirty) {
        bake_background();
    }

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
    free(s_shadow_fb);
    s_shadow_fb = NULL;
    free(s_baked_bg_xdr);
    s_baked_bg_xdr = NULL;
    s_bg_dirty = true;
    platform_log("[GFX] Backend RSX cerrado");
}

void gfx_begin_frame(uint32_t clear_rgba)
{
    if (!s_shadow_fb) return;

    int count = (int)s_screen_w * (int)s_screen_h;
    int count64 = count / 2;

    if (s_bg_theme == BG_THEME_PURE_BLACK || s_baked_bg_xdr == NULL ||
        s_bg_brightness <= 0.001f) {
        uint32_t color = 0xFF000000u | (clear_rgba & 0x00FFFFFFu);
        uint64_t color64 = ((uint64_t)color << 32) | (uint64_t)color;
        uint64_t *dst64 = (uint64_t *)s_shadow_fb;
        int i = 0;
        for (; i + 3 < count64; i += 4) {
            dst64[i]     = color64;
            dst64[i + 1] = color64;
            dst64[i + 2] = color64;
            dst64[i + 3] = color64;
        }
        for (; i < count64; i++) {
            dst64[i] = color64;
        }
        if (count & 1) {
            s_shadow_fb[count - 1] = color;
        }
    } else if (s_bg_brightness >= 0.999f) {
        const uint64_t *src64 = (const uint64_t *)s_baked_bg_xdr;
        uint64_t *dst64 = (uint64_t *)s_shadow_fb;
        int i = 0;
        for (; i + 3 < count64; i += 4) {
            dst64[i]     = src64[i];
            dst64[i + 1] = src64[i + 1];
            dst64[i + 2] = src64[i + 2];
            dst64[i + 3] = src64[i + 3];
        }
        for (; i < count64; i++) {
            dst64[i] = src64[i];
        }
        if (count & 1) {
            s_shadow_fb[count - 1] = s_baked_bg_xdr[count - 1];
        }
    } else {
        int s_int = (int)(s_bg_brightness * 256.0f + 0.5f);
        if (s_int < 0) s_int = 0;
        if (s_int > 256) s_int = 256;
        uint32_t scale = (uint32_t)s_int;
        for (int i = 0; i < count; i++) {
            uint32_t px = s_baked_bg_xdr[i];
            uint32_t r = (((px >> 16) & 0xFFu) * scale) >> 8;
            uint32_t g = (((px >> 8)  & 0xFFu) * scale) >> 8;
            uint32_t b = ((px         & 0xFFu) * scale) >> 8;
            s_shadow_fb[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

void gfx_end_frame(void)
{
    rsxBuffer *b = &s_buffers[s_cur_buffer];
    if (s_shadow_fb && b->ptr) {
        int count = b->width * b->height;
        int max_count = (int)s_screen_w * (int)s_screen_h;
        if (count > max_count) count = max_count;
        const uint64_t *src64 = (const uint64_t *)s_shadow_fb;
        uint64_t *dst64 = (uint64_t *)b->ptr;
        int count64 = count / 2;
        int i = 0;
        for (; i + 3 < count64; i += 4) {
            dst64[i]     = src64[i];
            dst64[i + 1] = src64[i + 1];
            dst64[i + 2] = src64[i + 2];
            dst64[i + 3] = src64[i + 3];
        }
        for (; i < count64; i++) {
            dst64[i] = src64[i];
        }
        if (count & 1) {
            b->ptr[count - 1] = s_shadow_fb[count - 1];
        }
    }
    flip(s_context, b->id);
    s_cur_buffer = (s_cur_buffer + 1) % 2;
    waitFlip();
}

/* ── Primitive 2D ──────────────────────────────────────────────── */

static inline void blend_pixel_xrgb(uint32_t *dst, uint32_t rgba)
{
    uint32_t a = (rgba >> 24) & 0xFFu;
    if (a == 0u) return;
    if (a == 255u) {
        *dst = 0xFF000000u | (rgba & 0x00FFFFFFu);
        return;
    }
    uint32_t inv = 255u - a;
    uint32_t d = *dst;
    uint32_t sr = (rgba >> 16) & 0xFFu;
    uint32_t sg = (rgba >> 8)  & 0xFFu;
    uint32_t sb =  rgba        & 0xFFu;
    uint32_t dr = (d >> 16) & 0xFFu;
    uint32_t dg = (d >> 8)  & 0xFFu;
    uint32_t db =  d        & 0xFFu;
    uint32_t r = (sr * a + dr * inv + 127u) / 255u;
    uint32_t g = (sg * a + dg * inv + 127u) / 255u;
    uint32_t b = (sb * a + db * inv + 127u) / 255u;
    *dst = 0xFF000000u | (r << 16) | (g << 8) | b;
}

void gfx_draw_rect(float x, float y, float w, float h, uint32_t rgba)
{
    if (!s_shadow_fb) return;

    uint32_t a = (rgba >> 24) & 0xFFu;
    if (a == 0u) return;
    if (w <= 0.0f || h <= 0.0f) return;

    int ix = (int)vx(x);
    int iy = (int)vy(y);
    int iw = (int)vw(w);
    int ih = (int)vh(h);
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
    int bw = (int)s_screen_w;
    int bh = (int)s_screen_h;

    if (ix < 0) { iw += ix; ix = 0; }
    if (iy < 0) { ih += iy; iy = 0; }
    if (ix + iw > bw) iw = bw - ix;
    if (iy + ih > bh) ih = bh - iy;
    if (iw <= 0 || ih <= 0) return;

    if (a == 255u) {
        uint32_t opaque = 0xFF000000u | (rgba & 0x00FFFFFFu);
        uint64_t color64 = ((uint64_t)opaque << 32) | (uint64_t)opaque;

        for (int r = iy; r < iy + ih; r++) {
            uint32_t *row = &s_shadow_fb[(size_t)r * (size_t)bw + (size_t)ix];
            int rem = iw;
            if (((uintptr_t)row & 7u) && rem > 0) {
                *row++ = opaque;
                rem--;
            }
            int w64 = rem / 2;
            uint64_t *row64 = (uint64_t *)row;
            for (int i = 0; i < w64; i++) {
                row64[i] = color64;
            }
            if (rem & 1) {
                row[w64 * 2] = opaque;
            }
        }
    } else {
        for (int r = iy; r < iy + ih; r++) {
            uint32_t *row = &s_shadow_fb[(size_t)r * (size_t)bw + (size_t)ix];
            for (int c = 0; c < iw; c++) {
                blend_pixel_xrgb(&row[c], rgba);
            }
        }
    }
}

void gfx_draw_rect_outline(float x, float y, float w, float h,
                           uint32_t rgba, float thickness)
{
    if (!s_shadow_fb) return;

    uint32_t a = (rgba >> 24) & 0xFFu;
    if (a == 0u) return;

    int ix = (int)vx(x);
    int iy = (int)vy(y);
    int iw = (int)vw(w);
    int ih = (int)vh(h);
    int t = (int)(thickness * s_scale_x);
    if (t < 1) t = 1;
    if (iw <= 0 || ih <= 0) return;

    int bw = (int)s_screen_w;
    int bh = (int)s_screen_h;

    int x_start = (ix > 0) ? ix : 0;
    int x_end   = (ix + iw < bw) ? (ix + iw) : bw;
    if (x_start >= x_end) return;

    int top_end   = (iy + t < iy + ih) ? (iy + t) : (iy + ih);
    int bot_start = (iy + ih - t > top_end) ? (iy + ih - t) : top_end;

    /* Top */
    for (int r = (iy > 0 ? iy : 0); r < top_end && r < bh; r++) {
        uint32_t *row = &s_shadow_fb[(size_t)r * (size_t)bw];
        for (int c = x_start; c < x_end; c++) {
            blend_pixel_xrgb(&row[c], rgba);
        }
    }
    /* Bottom */
    for (int r = (bot_start > 0 ? bot_start : 0); r < iy + ih && r < bh; r++) {
        uint32_t *row = &s_shadow_fb[(size_t)r * (size_t)bw];
        for (int c = x_start; c < x_end; c++) {
            blend_pixel_xrgb(&row[c], rgba);
        }
    }

    int mid_y0 = (top_end > 0) ? top_end : 0;
    int mid_y1 = (bot_start < bh) ? bot_start : bh;
    if (mid_y0 >= mid_y1) return;

    int left_end    = (ix + t < ix + iw) ? (ix + t) : (ix + iw);
    int right_start = (ix + iw - t > left_end) ? (ix + iw - t) : left_end;
    int lx0 = (ix > 0) ? ix : 0;
    int lx1 = (left_end < bw) ? left_end : bw;
    int rx0 = (right_start > 0) ? right_start : 0;
    int rx1 = (ix + iw < bw) ? (ix + iw) : bw;

    /* Left and Right */
    for (int r = mid_y0; r < mid_y1; r++) {
        uint32_t *row = &s_shadow_fb[(size_t)r * (size_t)bw];
        for (int c = lx0; c < lx1; c++) {
            blend_pixel_xrgb(&row[c], rgba);
        }
        for (int c = rx0; c < rx1; c++) {
            blend_pixel_xrgb(&row[c], rgba);
        }
    }
}

/* ── Texturas ──────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *data;
    size_t         pos;
    size_t         len;
} PngMemCursor;

static void png_mem_read_cb(png_structp png_ptr, png_bytep out, png_size_t count)
{
    PngMemCursor *cursor = (PngMemCursor *)png_get_io_ptr(png_ptr);
    if (!cursor || cursor->pos > cursor->len ||
        (size_t)count > cursor->len - cursor->pos) {
        png_error(png_ptr, "PNG unexpected EOF in memory buffer");
        return;
    }
    memcpy(out, cursor->data + cursor->pos, (size_t)count);
    cursor->pos += (size_t)count;
}

int gfx_decode_png_memory(const uint8_t *buf, size_t len,
                          uint8_t **out_rgba, int *out_w, int *out_h)
{
    if (out_rgba) *out_rgba = NULL;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!buf || len < 8u || !out_rgba || !out_w || !out_h) return -1;

    png_byte sig[8];
    memcpy(sig, buf, 8u);
    if (png_sig_cmp(sig, 0, 8) != 0) return -1;

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) return -1;

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        return -1;
    }

    uint8_t * volatile rgba_buf = NULL;
    png_bytep * volatile row_ptrs = NULL;
    PngMemCursor cursor = { buf, 8u, len };

    if (setjmp(png_jmpbuf(png_ptr))) {
        free(row_ptrs);
        free(rgba_buf);
        *out_rgba = NULL;
        *out_w = 0;
        *out_h = 0;
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return -1;
    }

    png_set_read_fn(png_ptr, &cursor, png_mem_read_cb);
    png_set_sig_bytes(png_ptr, 8);
    png_read_info(png_ptr, info_ptr);

    png_uint_32 w = 0;
    png_uint_32 h = 0;
    int bit_depth = 0;
    int color_type = 0;
    png_get_IHDR(png_ptr, info_ptr, &w, &h, &bit_depth, &color_type,
                 NULL, NULL, NULL);

    if (w == 0 || h == 0 || w > 4096 || h > 4096) {
        png_error(png_ptr, "PNG dimensions out of range");
    }

    if (bit_depth == 16) {
        png_set_strip_16(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png_ptr);
    }
    if (!(color_type & PNG_COLOR_MASK_ALPHA) &&
        !png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
        png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
    }

    png_set_interlace_handling(png_ptr);
    png_read_update_info(png_ptr, info_ptr);
    png_size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);
    if (rowbytes != (png_size_t)w * 4u) {
        png_error(png_ptr, "PNG unexpected RGBA rowbytes");
    }

    size_t rgba_size = (size_t)w * (size_t)h * 4u;
    uint8_t *rgba = (uint8_t *)memalign(16, rgba_size);
    if (!rgba) {
        png_error(png_ptr, "PNG out of memory");
    }
    rgba_buf = rgba;

    png_bytep *rows = (png_bytep *)malloc((size_t)h * sizeof(png_bytep));
    if (!rows) {
        png_error(png_ptr, "PNG out of memory for row pointers");
    }
    row_ptrs = rows;

    for (png_uint_32 y = 0; y < h; y++) {
        rows[y] = rgba + (size_t)y * (size_t)rowbytes;
    }

    png_read_image(png_ptr, rows);
    png_read_end(png_ptr, NULL);

    free(rows);
    row_ptrs = NULL;
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

    *out_rgba = rgba;
    *out_w = (int)w;
    *out_h = (int)h;
    return 0;
}

int gfx_load_texture_png(const char *path, GfxTexture *out)
{
    if (!path || !out) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) {
        platform_log("[GFX] Failed to open PNG: %s", path);
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long fsize = ftell(f);
    if (fsize < 8 || fsize > 32 * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        platform_log("[GFX] Invalid PNG size (%ld): %s", fsize, path);
        return -1;
    }

    uint8_t *file_buf = (uint8_t *)malloc((size_t)fsize);
    if (!file_buf) {
        fclose(f);
        platform_log("[GFX] malloc failed for PNG file (%ld B)", fsize);
        return -1;
    }

    size_t nread = fread(file_buf, 1u, (size_t)fsize, f);
    fclose(f);
    if (nread != (size_t)fsize) {
        free(file_buf);
        platform_log("[GFX] Short read for PNG: %s", path);
        return -1;
    }

    uint8_t *rgba = NULL;
    int w = 0;
    int h = 0;
    int dec_rc = gfx_decode_png_memory(file_buf, (size_t)fsize, &rgba, &w, &h);
    free(file_buf);

    if (dec_rc != 0 || !rgba) {
        platform_log("[GFX] Failed to decode PNG: %s", path);
        return -1;
    }

    int up_rc = gfx_upload_rgba_texture(rgba, w, h, out);
    free(rgba);

    if (up_rc == 0) {
        platform_log("[GFX] Texture loaded: %s (%dx%d)", path, w, h);
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

static void blit_rgba(uint32_t *dst_buf, int dst_w, int dst_h,
                      int dx, int dy, int dw, int dh,
                      const uint8_t *src, int sw, int sh, int src_stride_px,
                      uint32_t tint_rgba)
{
    if (!dst_buf || !src || dst_w <= 0 || dst_h <= 0 ||
        dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || src_stride_px <= 0) {
        return;
    }

    uint32_t t_a = (tint_rgba >> 24) & 0xFFu;
    if (t_a == 0u) return;

    uint32_t t_r = (tint_rgba >> 16) & 0xFFu;
    uint32_t t_g = (tint_rgba >> 8)  & 0xFFu;
    uint32_t t_b =  tint_rgba        & 0xFFu;

    int x0 = (dx > 0) ? dx : 0;
    int y0 = (dy > 0) ? dy : 0;
    int x1 = (dx + dw < dst_w) ? (dx + dw) : dst_w;
    int y1 = (dy + dh < dst_h) ? (dy + dh) : dst_h;
    if (x0 >= x1 || y0 >= y1) return;

    for (int y = y0; y < y1; y++) {
        int sy = ((y - dy) * sh) / dh;
        if (sy >= sh) sy = sh - 1;
        const uint8_t *src_scanline = &src[(size_t)sy * (size_t)src_stride_px * 4u];
        uint32_t *dst_row = &dst_buf[(size_t)y * (size_t)dst_w];

        for (int x = x0; x < x1; x++) {
            int sx = ((x - dx) * sw) / dw;
            if (sx >= sw) sx = sw - 1;
            const uint8_t *p = &src_scanline[(size_t)sx * 4u];

            uint32_t eff_a = ((uint32_t)p[3] * t_a + 127u) / 255u;
            if (eff_a == 0u) continue;

            uint32_t r = ((uint32_t)p[0] * t_r + 127u) / 255u;
            uint32_t g = ((uint32_t)p[1] * t_g + 127u) / 255u;
            uint32_t b = ((uint32_t)p[2] * t_b + 127u) / 255u;
            uint32_t pixel = (eff_a << 24) | (r << 16) | (g << 8) | b;

            if (eff_a == 255u) {
                dst_row[x] = pixel;
            } else {
                blend_pixel_xrgb(&dst_row[x], pixel);
            }
        }
    }
}

void gfx_draw_sprite(const GfxTexture *tex, float x, float y, float w, float h,
                     float u0, float v0, float u1, float v1, uint32_t tint_rgba)
{
    if (!s_shadow_fb || !tex || !tex->data || !tex->uploaded) return;
    if (tex->width <= 0 || tex->height <= 0) return;

    int dx = (int)vx(x);
    int dy = (int)vy(y);
    int dw = (int)vw(w);
    int dh = (int)vh(h);

    int src_x0 = (int)(u0 * (float)tex->width);
    int src_y0 = (int)(v0 * (float)tex->height);
    int src_x1 = (int)(u1 * (float)tex->width);
    int src_y1 = (int)(v1 * (float)tex->height);
    if (src_x0 < 0) src_x0 = 0;
    if (src_y0 < 0) src_y0 = 0;
    if (src_x1 > tex->width) src_x1 = tex->width;
    if (src_y1 > tex->height) src_y1 = tex->height;
    int sw = src_x1 - src_x0;
    int sh = src_y1 - src_y0;
    if (sw <= 0) { src_x0 = 0; sw = tex->width; }
    if (sh <= 0) { src_y0 = 0; sh = tex->height; }

    const uint8_t *src = (const uint8_t *)tex->data +
                         (size_t)src_y0 * tex->pitch + (size_t)src_x0 * 4u;

    blit_rgba(s_shadow_fb, (int)s_screen_w, (int)s_screen_h,
              dx, dy, dw, dh, src, sw, sh, tex->width, tint_rgba);
}

void gfx_draw_9slice(const GfxTexture *tex, float x, float y, float w, float h,
                     float border_px, uint32_t tint_rgba)
{
    if (!s_shadow_fb || !tex || !tex->data || !tex->uploaded) return;

    int dx = (int)vx(x);
    int dy = (int)vy(y);
    int dw = (int)vw(w);
    int dh = (int)vh(h);
    int tw = tex->width;
    int th = tex->height;
    if (dw <= 0 || dh <= 0 || tw <= 0 || th <= 0) return;

    int b = (int)(border_px * s_scale_x);
    if (b < 1) b = 1;

    /* 9 regiones: corners, bordes, centro */
    int cx0 = dx, cx1 = dx + b, cx2 = dx + dw - b, cx3 = dx + dw;
    int cy0 = dy, cy1 = dy + b, cy2 = dy + dh - b, cy3 = dy + dh;

    const uint8_t *src = (const uint8_t *)tex->data;
    int sb = (int)((float)b / (float)dw * (float)tw); /* border en src */
    if (sb < 1) sb = 1;
    if (sb * 2 >= tw) sb = tw / 2;
    if (sb * 2 >= th) sb = th / 2;
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

    for (int i = 0; i < 9; i++) {
        int rw = regions[i].dx1 - regions[i].dx0;
        int rh = regions[i].dy1 - regions[i].dy0;
        if (rw <= 0 || rh <= 0) continue;

        const uint8_t *rsrc = src + (size_t)regions[i].sy0 * (size_t)tw * 4u +
                              (size_t)regions[i].sx0 * 4u;
        int rsw = regions[i].sx1 - regions[i].sx0;
        int rsh = regions[i].sy1 - regions[i].sy0;
        if (rsw <= 0 || rsh <= 0) continue;

        blit_rgba(s_shadow_fb, (int)s_screen_w, (int)s_screen_h,
                  regions[i].dx0, regions[i].dy0, rw, rh,
                  rsrc, rsw, rsh, tw, tint_rgba);
    }
}

/* ── Texto (placeholder: bloques sólidos por carácter) ─────────── */

static int s_font_loaded = 0;

static unsigned char normalize_utf8_char(const char **p_utf8)
{
    const unsigned char *s = (const unsigned char *)*p_utf8;
    if (*s < 0x80) {
        (*p_utf8)++;
        return *s;
    }

    /* UTF-8 multi-byte sequence */
    if (s[0] == 0xC3 && s[1]) {
        (*p_utf8) += 2;
        unsigned char c2 = s[1];
        /* á, à, ä, â, ã, å */
        if (c2 >= 0xA0 && c2 <= 0xA5) return 'a';
        /* Á, À, Ä, Â, Ã, Å */
        if (c2 >= 0x80 && c2 <= 0x85) return 'A';
        /* é, è, ë, ê */
        if (c2 >= 0xA8 && c2 <= 0xAB) return 'e';
        /* É, È, Ë, Ê */
        if (c2 >= 0x88 && c2 <= 0x8B) return 'E';
        /* í, ì, ï, î */
        if (c2 >= 0xAC && c2 <= 0xAF) return 'i';
        /* Í, Ì, Ï, Î */
        if (c2 >= 0x8C && c2 <= 0x8F) return 'I';
        /* ó, ò, ö, ô, õ */
        if (c2 >= 0xB2 && c2 <= 0xB6) return 'o';
        /* Ó, Ò, Ö, Ô, Õ */
        if (c2 >= 0x92 && c2 <= 0x96) return 'O';
        /* ú, ù, ü, û */
        if (c2 >= 0xB9 && c2 <= 0xBC) return 'u';
        /* Ú, Ù, Ü, Û */
        if (c2 >= 0x99 && c2 <= 0x9C) return 'U';
        /* ñ */
        if (c2 == 0xB1) return 'n';
        /* Ñ */
        if (c2 == 0x91) return 'N';
        /* ç, Ç */
        if (c2 == 0xA7) return 'c';
        if (c2 == 0x87) return 'C';
        return '?';
    } else if (s[0] == 0xC2 && s[1]) {
        (*p_utf8) += 2;
        unsigned char c2 = s[1];
        if (c2 == 0xBF) return '?'; /* ¿ */
        if (c2 == 0xA1) return '!'; /* ¡ */
        if (c2 == 0xB7) return '-'; /* middle dot · */
        return ' ';
    } else if (s[0] == 0xE2 && s[1] && s[2]) {
        (*p_utf8) += 3;
        /* E2 80 93 / 94 (en dash, em dash) */
        if (s[1] == 0x80 && (s[2] == 0x93 || s[2] == 0x94)) return '-';
        /* E2 80 98 / 99 (single quotes) */
        if (s[1] == 0x80 && (s[2] == 0x98 || s[2] == 0x99)) return '\'';
        /* E2 80 9C / 9D (double quotes) */
        if (s[1] == 0x80 && (s[2] == 0x9C || s[2] == 0x9D)) return '"';
        /* E2 80 A2 (bullet •) */
        if (s[1] == 0x80 && s[2] == 0xA2) return '*';
        return ' ';
    }

    /* Fallback: skip leading byte */
    (*p_utf8)++;
    return '?';
}

static void draw_glyph_box(uint32_t *dst_buf, int dst_w, int dst_h,
                           int x0, int y0, int x1, int y1,
                           unsigned char ch, uint32_t rgba)
{
    if (!dst_buf || dst_w <= 0 || dst_h <= 0) return;
    if (((rgba >> 24) & 0xFFu) == 0u) return;

    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8_t *g = FONT8[ch - 0x20];

    int gw = x1 - x0;
    int gh = y1 - y0;
    if (gw <= 0 || gh <= 0) return;

    for (int r = 0; r < 8; r++) {
        uint8_t bits = g[r];
        if (!bits) continue;
        int py_start = y0 + (r * gh) / 8;
        int py_end   = y0 + ((r + 1) * gh) / 8;
        if (py_end <= 0 || py_start >= dst_h) continue;
        if (py_start < 0) py_start = 0;
        if (py_end > dst_h) py_end = dst_h;

        for (int c = 0; c < 8; c++) {
            if (!(bits & (1u << c))) continue;
            int px_start = x0 + (c * gw) / 8;
            int px_end   = x0 + ((c + 1) * gw) / 8;
            if (px_end <= 0 || px_start >= dst_w) continue;
            if (px_start < 0) px_start = 0;
            if (px_end > dst_w) px_end = dst_w;

            for (int py = py_start; py < py_end; py++) {
                uint32_t *row = &dst_buf[(size_t)py * (size_t)dst_w + (size_t)px_start];
                int span = px_end - px_start;
                for (int s = 0; s < span; s++) {
                    blend_pixel_xrgb(&row[s], rgba);
                }
            }
        }
    }
}

int gfx_draw_text(float x, float y, const char *utf8, float size_px, uint32_t rgba)
{
    if (!utf8 || !*utf8) return 0;
    if (size_px < 12.0f) size_px = 12.0f;
    if (size_px > 96.0f) size_px = 96.0f;
    if (!s_font_loaded) {
        platform_log("[GFX] Fuente bitmap 8x8 optimizada cargada (render continuo)");
        s_font_loaded = 1;
    }

    float cur_vx = x;
    const char *p = utf8;
    while (*p) {
        unsigned char ch = normalize_utf8_char(&p);
        int gx0 = (int)vx(cur_vx);
        int gy0 = (int)vy(y);
        int gx1 = (int)vx(cur_vx + size_px);
        int gy1 = (int)vy(y + size_px);
        draw_glyph_box(s_shadow_fb, (int)s_screen_w, (int)s_screen_h,
                       gx0, gy0, gx1, gy1, ch, rgba);
        cur_vx += size_px;
    }
    return (int)(cur_vx - x);
}

int gfx_draw_text_clipped(float x, float y, float max_w, const char *utf8,
                          float size_px, uint32_t rgba)
{
    if (!utf8 || !*utf8) return 0;
    if (size_px < 12.0f) size_px = 12.0f;
    int max_chars = (int)(max_w / size_px);
    if (max_chars <= 0) return 0;

    /* Measure visual characters considering UTF-8 normalization */
    int char_count = 0;
    const char *p = utf8;
    while (*p) {
        normalize_utf8_char(&p);
        char_count++;
    }

    if (char_count <= max_chars) {
        return gfx_draw_text(x, y, utf8, size_px, rgba);
    }

    char buf[512];
    int out_idx = 0;
    int target_chars = (max_chars > 3) ? (max_chars - 3) : max_chars;
    if (target_chars < 1) target_chars = 1;

    p = utf8;
    for (int i = 0; i < target_chars && *p; i++) {
        unsigned char ch = normalize_utf8_char(&p);
        if (out_idx < (int)sizeof(buf) - 5) {
            buf[out_idx++] = (char)ch;
        }
    }
    if (max_chars > 3 && out_idx <= (int)sizeof(buf) - 4) {
        buf[out_idx++] = '.';
        buf[out_idx++] = '.';
        buf[out_idx++] = '.';
    }
    buf[out_idx] = '\0';
    return gfx_draw_text(x, y, buf, size_px, rgba);
}

/* ── Visualizador 3D wireframe (líneas por software) ───────────── */

static void draw_line_sw(uint32_t *dst_buf, int dst_w, int dst_h,
                         int x0, int y0, int x1, int y1, uint32_t color)
{
    if (!dst_buf || dst_w <= 0 || dst_h <= 0) return;
    if (((color >> 24) & 0xFFu) == 0u) return;

    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    while (true) {
        if (x0 >= 0 && x0 < dst_w && y0 >= 0 && y0 < dst_h) {
            blend_pixel_xrgb(&dst_buf[(size_t)y0 * (size_t)dst_w + (size_t)x0], color);
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
    if (!s_shadow_fb || !vis_bands || num_bands <= 0) return;

    int fx = (int)vx(cx);
    int fy = (int)vy(cy);
    int grid = 20;
    float cell = 16.0f * scale;

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

            draw_line_sw(s_shadow_fb, (int)s_screen_w, (int)s_screen_h,
                         px0, py0, px1, py1, rgba);
            draw_line_sw(s_shadow_fb, (int)s_screen_w, (int)s_screen_h,
                         px0, py0, px2, py2, rgba);
        }
    }
}

/* ── Background pre-baking & theme control ─────────────────────── */

static void bake_background(void)
{
    if (s_shadow_fb == NULL) {
        s_bg_dirty = true;
        return;
    }

    if (s_bg_theme == BG_THEME_PURE_BLACK) {
        free(s_baked_bg_xdr);
        s_baked_bg_xdr = NULL;
        s_bg_dirty = false;
        return;
    }

    const uint8_t *png_data = bg_fracture_hud_png;
    size_t png_size = (size_t)bg_fracture_hud_png_size;
    switch (s_bg_theme) {
        case BG_THEME_MATTE_SLABS:
            png_data = bg_matte_slabs_png;
            png_size = (size_t)bg_matte_slabs_png_size;
            break;
        case BG_THEME_LIQUID_CHROME:
            png_data = bg_liquid_chrome_png;
            png_size = (size_t)bg_liquid_chrome_png_size;
            break;
        case BG_THEME_FRACTURE_HUD:
        default:
            png_data = bg_fracture_hud_png;
            png_size = (size_t)bg_fracture_hud_png_size;
            break;
    }

    uint8_t *decoded_rgba = NULL;
    int sw = 0;
    int sh = 0;
    if (gfx_decode_png_memory(png_data, png_size, &decoded_rgba, &sw, &sh) != 0 ||
        !decoded_rgba || sw <= 0 || sh <= 0) {
        free(decoded_rgba);
        free(s_baked_bg_xdr);
        s_baked_bg_xdr = NULL;
        platform_log("[GFX] Warning: failed to decode background theme %d", s_bg_theme);
        return;
    }

    if (s_baked_bg_xdr == NULL) {
        s_baked_bg_xdr = (uint32_t *)memalign(64, (size_t)s_screen_w * (size_t)s_screen_h * 4u);
        if (s_baked_bg_xdr == NULL) {
            free(decoded_rgba);
            platform_log("[GFX] Error: memalign failed for s_baked_bg_xdr");
            return;
        }
    }

    if (s_scale_x <= 0.0f || s_scale_y <= 0.0f) {
        free(decoded_rgba);
        return;
    }

    int dw = (int)s_screen_w;
    int dh = (int)s_screen_h;
    float inv_sx = 1.0f / s_scale_x;
    float inv_sy = 1.0f / s_scale_y;
    float fsw = (float)sw;
    float fsh = (float)sh;

    for (int py = 0; py < dh; py++) {
        uint32_t *dst_row = &s_baked_bg_xdr[(size_t)py * (size_t)dw];
        float vy_inv = ((float)py - s_offset_y) * inv_sy;
        if (vy_inv < 0.0f || vy_inv >= fsh) {
            for (int px = 0; px < dw; px++) {
                dst_row[px] = 0xFF000000u;
            }
            continue;
        }

        int y0 = (int)vy_inv;
        if (y0 >= sh) y0 = sh - 1;
        int y1 = (y0 + 1 < sh) ? (y0 + 1) : (sh - 1);
        float fy = vy_inv - (float)y0;
        const uint8_t *src_row0 = &decoded_rgba[(size_t)y0 * (size_t)sw * 4u];
        const uint8_t *src_row1 = &decoded_rgba[(size_t)y1 * (size_t)sw * 4u];

        for (int px = 0; px < dw; px++) {
            float vx_inv = ((float)px - s_offset_x) * inv_sx;
            if (vx_inv < 0.0f || vx_inv >= fsw) {
                dst_row[px] = 0xFF000000u;
                continue;
            }

            int x0 = (int)vx_inv;
            if (x0 >= sw) x0 = sw - 1;
            int x1 = (x0 + 1 < sw) ? (x0 + 1) : (sw - 1);
            float fx = vx_inv - (float)x0;

            uint8_t c00 = src_row0[(size_t)x0 * 4u];
            uint8_t c10 = src_row0[(size_t)x1 * 4u];
            uint8_t c01 = src_row1[(size_t)x0 * 4u];
            uint8_t c11 = src_row1[(size_t)x1 * 4u];

            float top = (float)c00 + fx * ((float)c10 - (float)c00);
            float bot = (float)c01 + fx * ((float)c11 - (float)c01);
            int val = (int)(top + fy * (bot - top) + 0.5f);
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            uint32_t gray = (uint32_t)val;
            dst_row[px] = 0xFF000000u | (gray << 16) | (gray << 8) | gray;
        }
    }

    free(decoded_rgba);
    s_bg_dirty = false;
}

int gfx_set_background_theme(int theme_id)
{
    if (theme_id < 0 || theme_id >= BG_THEME_COUNT) {
        theme_id = BG_THEME_FRACTURE_HUD;
    }
    if (theme_id != s_bg_theme || s_bg_dirty) {
        s_bg_theme = theme_id;
        s_bg_dirty = true;
        bake_background();
    }
    return s_bg_theme;
}

void gfx_set_background_dim(float brightness)
{
    if (!(brightness > 0.0f)) {
        brightness = 0.0f;
    } else if (brightness > 1.0f) {
        brightness = 1.0f;
    }
    s_bg_brightness = brightness;
}

/* ── Utilidades ────────────────────────────────────────────────── */

int gfx_get_width(void)  { return s_screen_w; }
int gfx_get_height(void) { return s_screen_h; }

void gfx_set_overscan(int offset_x, int offset_y, float scale)
{
    if (scale < 0.70f) scale = 0.70f;
    if (scale > 1.00f) scale = 1.00f;
    bool changed = (offset_x != s_overscan_x ||
                    offset_y != s_overscan_y ||
                    fabsf(scale - s_overscan_scale) > 0.0001f);
    s_overscan_x = offset_x;
    s_overscan_y = offset_y;
    s_overscan_scale = scale;
    s_scale_x = scale * ((float)s_screen_w / 1920.0f);
    s_scale_y = scale * ((float)s_screen_h / 1080.0f);
    s_offset_x = (float)offset_x + ((float)s_screen_w * (1.0f - scale) * 0.5f);
    s_offset_y = (float)offset_y + ((float)s_screen_h * (1.0f - scale) * 0.5f);
    if ((changed || s_bg_dirty) && s_shadow_fb != NULL) {
        bake_background();
    }
}
