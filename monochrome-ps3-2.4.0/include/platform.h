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

/* Ciclo de frame (delegado a gfx_tiny3d) */
void platform_prepare_frame(void);
void platform_flip(void);

/* Logging y tiempo */
void platform_log(const char *fmt, ...);
uint64_t platform_get_time_usec(void);

/* Las funciones de dibujo legacy viven en platform_gfx.h (bridge a Tiny3D) */
#include "platform_gfx.h"

#endif /* PLATFORM_PSL1GHT_H */
