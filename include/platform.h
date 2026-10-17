/**
 * Monochrome Music Player — Platform Hardware Abstraction Layer (HAL)
 * Unifies host PC (Windows / Linux) and PlayStation 3 (Cell B.E. / GameOS).
 */

#ifndef PLATFORM_HAL_H
#define PLATFORM_HAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Platform Identifiers ────────────────────────────────────────── */
#if defined(__powerpc__) || defined(__CELLOS_LV2__) || defined(TARGET_PS3)
    #define PLATFORM_IS_PS3 1
    #define PLATFORM_NAME   "PlayStation 3 (Cell B.E. / GameOS)"
#else
    #define PLATFORM_IS_PC  1
    #define PLATFORM_NAME   "Host PC (Windows x86_64)"
#endif

/* ── Input Action Codes ──────────────────────────────────────────── */
typedef enum {
    INPUT_NONE = 0,
    INPUT_UP,
    INPUT_DOWN,
    INPUT_LEFT,
    INPUT_RIGHT,
    INPUT_SELECT,      /* Cross (×) on PS3 / Enter on PC */
    INPUT_CANCEL,      /* Circle (○) on PS3 / Escape on PC */
    INPUT_ACTION_SEARCH,/* Square (□) on PS3 / 'S' on PC */
    INPUT_PLAY_PAUSE,  /* Triangle (△) on PS3 / Space on PC */
    INPUT_QUIT         /* PS Button on PS3 / 'Q' on PC */
} InputAction;

/* ── Lifecycle & Timer ───────────────────────────────────────────── */
int      platform_init(void);
void     platform_shutdown(void);
void     platform_sleep_ms(uint32_t ms);
uint64_t platform_get_time_ms(void);

/* ── Network & HTTPS Abstraction ─────────────────────────────────── */
/**
 * Performs a synchronous HTTPS GET request.
 * Allocates *response_out buffer. Caller must free with platform_http_free().
 */
int  platform_http_get(const char *url, char **response_out, size_t *size_out);
void platform_http_free(char *response);

/* ── Audio Output Abstraction ────────────────────────────────────── */
int  platform_audio_init(uint32_t sample_rate, uint8_t channels);
void platform_audio_shutdown(void);
void platform_audio_write(const int16_t *samples, size_t frames);
void platform_audio_set_volume(float volume_0_to_1);

/* ── Input Abstraction ───────────────────────────────────────────── */
InputAction platform_poll_input(void);
int         platform_get_mouse_track_selection(void);

/* ── Console / Terminal Abstraction ──────────────────────────────── */
void platform_clear_screen(void);
void platform_set_cursor(int row, int col);

/* ── Display / Frame Rendering Abstraction ───────────────────────── */
struct TrackMetadata;
int  platform_gfx_init(int width, int height, const char *title);
void platform_gfx_shutdown(void);
void platform_gfx_render(int state, const struct TrackMetadata *track,
                         int pstate, float progress,
                         const struct TrackMetadata *tracks, int track_count, int cursor,
                         const char *search_query);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_HAL_H */
