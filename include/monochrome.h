/**
 * Monochrome Music Player - PlayStation 3 Port
 * Sony Computer Entertainment Cell Broadband Engine Architecture
 *
 * Memory map (208 MB usable XDR + 256 MB GDDR3):
 *   Ring Buffer (FLAC stream)  : 8 MB XDR    (2+ min at 24/96)
 *   PCM output double-buffer   : 2 MB XDR
 *   Cover art texture cache    : 16 MB GDDR3
 *   RSX framebuffers (720p x2) : 7.2 MB GDDR3
 *   Application heap + stack   : ~32 MB XDR
 *   GameOS kernel reservation  : ~48 MB XDR
 */

#ifndef MONOCHROME_PS3_H
#define MONOCHROME_PS3_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Application identity ────────────────────────────────────────── */
#define APP_TITLE          "Monochrome"
#define APP_VERSION        "01.00"
#define APP_TITLEID        "MCRM00001"

/* ── Display (PS3 480p Minimum Resolution) ───────────────────────── */
#define SCREEN_WIDTH        720
#define SCREEN_HEIGHT       480
#define TARGET_FPS          60

/* ── Memory budgets (XDR DRAM) ───────────────────────────────────── */
#define STREAM_RING_BUFFER_SIZE  (8 * 1024 * 1024)   /* 8 MB FLAC stream */
#define AUDIO_PCM_BUFFER_SIZE    (2 * 1024 * 1024)   /* 2 MB decoded PCM */
#define MAX_SEARCH_RESULTS       20
#define MAX_PLAYLIST_SIZE        200

/* ── API endpoints (from upstream monochrome-music/monochrome) ───── */
#define DEFAULT_API_HOST   "tracks.monochrome.st"
#define FALLBACK_API_HOSTS { "api.monochrome.tf", "wolf.qqdl.site", "maus.qqdl.site", "vogel.qqdl.site" }
#define NUM_FALLBACK_HOSTS 4
#define HTTPS_PORT         443

/* ── Filesystem paths on dev_hdd0 ────────────────────────────────── */
#define CERT_BUNDLE_PATH   "/dev_hdd0/game/" APP_TITLEID "/USRDIR/certs/cacert.pem"
#define FONT_PATH          "/dev_hdd0/game/" APP_TITLEID "/USRDIR/assets/font.ttf"

/* ── Application states ──────────────────────────────────────────── */
typedef enum {
    STATE_SPLASH,
    STATE_HOME,
    STATE_SEARCH,
    STATE_ALBUM,
    STATE_PLAYLIST,
    STATE_PLAYING,
    STATE_SETTINGS
} AppState;

/* ── Playback states ─────────────────────────────────────────────── */
typedef enum {
    PLAYBACK_STOPPED,
    PLAYBACK_BUFFERING,
    PLAYBACK_PLAYING,
    PLAYBACK_PAUSED
} PlaybackState;

/* ── Repeat & Shuffle Modes ──────────────────────────────────────── */
typedef enum {
    REPEAT_OFF = 0,
    REPEAT_ALL = 1,
    REPEAT_ONE = 2
} RepeatMode;

int  ui_get_repeat_mode(void);
bool ui_get_shuffle_mode(void);
void ui_set_repeat_mode(int mode);
void ui_set_shuffle_mode(bool shuffle);
void ui_track_next(void);
void ui_track_prev(void);

/* ── Track metadata ──────────────────────────────────────────────── */
typedef struct TrackMetadata {
    char     id[64];
    char     title[128];
    char     artist[128];
    char     album[128];
    char     cover_url[256];
    char     stream_url[512];
    uint32_t duration_sec;
    uint32_t sample_rate;      /* 44100, 48000, 96000, 192000 */
    uint8_t  bit_depth;        /* 16 or 24 */
    bool     is_hi_res;
} TrackMetadata;

/* ── Lock-free ring buffer for PPE→SPU audio pipeline ────────────── */
typedef struct {
    uint8_t  *buffer;
    size_t    capacity;
    volatile size_t write_pos;  /* only written by producer (net thread) */
    volatile size_t read_pos;   /* only written by consumer (audio thread) */
    volatile bool   stream_eof;
} AudioRingBuffer;

/* Cell B.E. lightweight memory barrier for weakly-ordered PowerPC memory */
#if defined(__powerpc__)
    #define CELL_LWSYNC() asm volatile("lwsync" ::: "memory")
#elif defined(_MSC_VER)
    #include <windows.h>
    #define CELL_LWSYNC() MemoryBarrier()
#else
    #define CELL_LWSYNC() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

#include "platform.h"

/* Ring buffer helpers (lock-free single-producer single-consumer) */
static inline size_t ring_available(const AudioRingBuffer *rb) {
    CELL_LWSYNC();
    size_t w = rb->write_pos, r = rb->read_pos;
    return (w >= r) ? (w - r) : (rb->capacity - r + w);
}
static inline size_t ring_free(const AudioRingBuffer *rb) {
    return rb->capacity - ring_available(rb) - 1;
}

/* ── FLAC Decoder ────────────────────────────────────────────────── */
typedef struct FlacDecoder FlacDecoder;

FlacDecoder *flac_decoder_create(const uint8_t *flac_data, size_t flac_size);
size_t       flac_decoder_read_pcm16(FlacDecoder *dec, int16_t *pcm_out, size_t frames_to_read);
uint32_t     flac_decoder_get_sample_rate(const FlacDecoder *dec);
uint8_t      flac_decoder_get_channels(const FlacDecoder *dec);
uint64_t     flac_decoder_get_total_frames(const FlacDecoder *dec);
void         flac_decoder_destroy(FlacDecoder *dec);

int flac_decode_memory(const uint8_t *flac_data, size_t flac_size,
                       int16_t *pcm_out, size_t max_pcm_frames,
                       size_t *frames_decoded, uint32_t *sample_rate, uint8_t *channels);

/* ── System lifecycle ────────────────────────────────────────────── */
int  app_init(void);
void app_shutdown(void);
void app_request_exit(void);
AudioRingBuffer *app_get_ring(void);
TrackMetadata   *app_get_current_track(void);

/* ── Network subsystem (PPE Thread 1) ────────────────────────────── */
int  net_init(void);
void net_shutdown(void);
int  net_search(const char *query, TrackMetadata *out, int max);
int  net_resolve_stream(const char *track_id, char *url_out, size_t url_len);
int  net_start_stream(const char *url, AudioRingBuffer *rb);
void net_stop_stream(void);

/* ── Audio subsystem (SPU decode + sys_audio_out) ────────────────── */
int  audio_init(AudioRingBuffer *rb);
void audio_shutdown(void);
void audio_tick(void);  /* called from main loop — feeds sys_audio_out */
void audio_set_state(PlaybackState state);
PlaybackState audio_get_state(void);
float audio_get_progress(uint32_t total_duration_sec);
#define VIS_BANDS 16
void audio_get_visualization(float *out_bands, int count);
void audio_reset_progress(void);

/* ── SPU DSP kernels ─────────────────────────────────────────────── */
void spu_resample_44_to_48(const int16_t *src, size_t frames_in,
                           int16_t *dst, size_t *frames_out);

/* ── Graphics / UI (RSX + DualShock 3) ───────────────────────────── */
int  gfx_init(void);
void gfx_shutdown(void);
void gfx_render(AppState state, const TrackMetadata *track,
                PlaybackState pstate, float progress);
AppState pad_update(AppState current);
void     ui_get_osk_state(int *row, int *col, const char **query);
void     ui_osk_input_char(char ch);
void     ui_osk_set_cursor(int row, int col);
/* ── Cover Art Caching Subsystem ─────────────────────────────────── */
#define COVER_SIZE 80
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixels[COVER_SIZE * COVER_SIZE];
    char     track_id[64];
    volatile bool ready;
} CoverArtImage;

void cover_art_init(void);
void cover_art_shutdown(void);
void cover_art_request(const char *track_id, const char *cover_url);
const CoverArtImage *cover_art_get_current(void);

#endif /* MONOCHROME_PS3_H */
