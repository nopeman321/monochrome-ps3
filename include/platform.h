#ifndef PLATFORM_PSL1GHT_H
#define PLATFORM_PSL1GHT_H

#include <stdint.h>
#include <stdbool.h>
#include "monochrome.h"

int  platform_init(void);
void platform_shutdown(void);

int  platform_get_width(void);
int  platform_get_height(void);

/* Screen adjustment for overscan compensation */
void platform_set_screen_adjust(int offset_x, int offset_y, float scale);
void platform_get_screen_adjust(int *offset_x, int *offset_y, float *scale);
void platform_set_font_scale(float scale);
float platform_get_font_scale(void);
int  platform_save_screen_config(void);
int  platform_load_screen_config(void);

InputAction platform_poll_input(void);

void platform_clear(uint32_t color);
void platform_draw_rect(int x, int y, int w, int h, uint32_t color);
int  platform_draw_text(int x, int y, const char *str, uint32_t color, int scale);
int  platform_draw_text_r(int rx, int y, const char *str, uint32_t color, int scale);

// Texture drawing (for cover art)
// texture_data: RGBA8888 pixel data
// tex_w, tex_h: texture dimensions
// dst_x, dst_y, dst_w, dst_h: destination rectangle on screen
void platform_draw_texture(const uint8_t *texture_data, int tex_w, int tex_h,
                           int dst_x, int dst_y, int dst_w, int dst_h);

void platform_flip(void);
void platform_log(const char *fmt, ...);

uint64_t platform_get_time_usec(void);

#endif /* PLATFORM_PSL1GHT_H */
