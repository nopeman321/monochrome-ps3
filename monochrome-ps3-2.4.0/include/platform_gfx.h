#ifndef PLATFORM_GFX_H
#define PLATFORM_GFX_H

#include <stdint.h>
#include <stddef.h>

/* ── Puente: funciones de dibujo legacy (usadas por ui.c) delegadas a gfx_tiny3d ──

   Estas funciones se implementan en source/platform_gfx_bridge.c y
   simplemente hacen forwarding al backend Tiny3D (gfx_engine.h). */

/* Limpia la pantalla (se llama al inicio de cada frame) */
void platform_clear(uint32_t color);

/* Dibuja un rectángulo relleno */
void platform_draw_rect(int x, int y, int w, int h, uint32_t color);

/* Dibuja una textura RGBA en RAM (subida a VRAM y renderizada como sprite) */
void platform_draw_texture(const uint8_t *texture_data, int tex_w, int tex_h,
                           int dst_x, int dst_y, int dst_w, int dst_h);

/* Dibuja texto (bitmap / fallback) */
int  platform_draw_text(int x, int y, const char *str, uint32_t color, int scale);
int  platform_draw_text_r(int rx, int y, const char *str, uint32_t color, int scale);
int  platform_draw_text_clipped(int x, int y, int max_w, const char *str,
                                uint32_t color, int scale);

#endif /* PLATFORM_GFX_H */
