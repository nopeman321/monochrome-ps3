/*
 * Monochrome PS3 - Platform GFX Bridge (Phase 2)
 * Implementa las funciones platform_draw_* (usadas por ui.c) delegando
 * al backend Tiny3D (gfx_engine.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "platform_gfx.h"
#include "gfx_engine.h"

/* ── Textura temporal reutilizable para platform_draw_texture ──── */

static GfxTexture s_tmp_tex = {0};
static bool s_tmp_tex_allocated = false;

void platform_clear(uint32_t color)
{
    gfx_begin_frame(color);
}

void platform_draw_rect(int x, int y, int w, int h, uint32_t color)
{
    gfx_draw_rect((float)x, (float)y, (float)w, (float)h, color);
}

void platform_draw_texture(const uint8_t *texture_data, int tex_w, int tex_h,
                           int dst_x, int dst_y, int dst_w, int dst_h)
{
    if (!texture_data || tex_w <= 0 || tex_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return;

    /* Subir (o re-subir) la textura a VRAM */
    if (!s_tmp_tex_allocated ||
        s_tmp_tex.width != tex_w || s_tmp_tex.height != tex_h) {
        if (s_tmp_tex_allocated && s_tmp_tex.data) {
            free(s_tmp_tex.data);
            s_tmp_tex.data = NULL;
        }
        if (gfx_upload_rgba_texture(texture_data, tex_w, tex_h, &s_tmp_tex) != 0) {
            return;
        }
        s_tmp_tex_allocated = true;
    } else {
        gfx_upload_rgba_texture(texture_data, tex_w, tex_h, &s_tmp_tex);
    }

    /* Dibujar como sprite (cubriendo todo el sprite) */
    gfx_draw_sprite(&s_tmp_tex,
                    (float)dst_x, (float)dst_y,
                    (float)dst_w, (float)dst_h,
                    0.0f, 0.0f, 1.0f, 1.0f,
                    0xFFFFFFFF);
}

int platform_draw_text(int x, int y, const char *str, uint32_t color, int scale)
{
    if (!str || !*str) return 0;
    float size_px = 8.0f * (float)scale;
    return gfx_draw_text((float)x, (float)y, str, size_px, color);
}

int platform_draw_text_r(int rx, int y, const char *str, uint32_t color, int scale)
{
    if (!str || !*str) return 0;
    float scale_f = (float)scale;
    int char_w = (int)(8.0f * scale_f);
    if (char_w < 1) char_w = 1;
    int text_w = (int)strlen(str) * char_w;
    int start_x = rx - text_w;
    if (start_x < 0) start_x = 0;
    return platform_draw_text(start_x, y, str, color, scale);
}

int platform_draw_text_clipped(int x, int y, int max_w, const char *str,
                               uint32_t color, int scale)
{
    if (!str || !*str) return 0;
    float size_px = 8.0f * (float)scale;
    return gfx_draw_text_clipped((float)x, (float)y, (float)max_w, str, size_px, color);
}
