#ifndef PLATFORM_PSL1GHT_H
#define PLATFORM_PSL1GHT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "monochrome.h"

int  platform_init(void);
void platform_shutdown(void);
bool platform_should_exit(void);
void platform_set_exit_requested(bool req);
bool platform_is_overlay_open(void);

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
int  platform_draw_text_clipped(int x, int y, int max_w, const char *str, uint32_t color, int scale);

void platform_draw_texture(const uint8_t *texture_data, int tex_w, int tex_h,
                           int dst_x, int dst_y, int dst_w, int dst_h);

void platform_prepare_frame(void);
void platform_flip(void);
void platform_log(const char *fmt, ...);

uint64_t platform_get_time_usec(void);

#endif /* PLATFORM_PSL1GHT_H */
