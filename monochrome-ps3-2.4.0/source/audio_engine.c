/*
 * Monochrome PS3 - Real-Time Lossless FLAC Streaming Audio Engine (PSL1GHT)
 * Hardened Network & Threading Pipeline
 */

#include <ppu-lv2.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

#include <audio/audio.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/cond.h>
#include <sys/event_queue.h>

#include <sysmodule/sysmodule.h>
#include <net/net.h>
#include <net/netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#include "audio_engine.h"
#include "http_client.h"
#include "monochrome.h"
#include "net_hal.h"

#define SAMPLE_RATE 48000
#define NUM_CHANNELS 2

#define API_HOST "tracks.monochrome.st"
#define API_PORT 80

#define STREAM_BUFFER_CAPACITY (24 * 1024 * 1024) /* 24 MB XDR buffer */
#define PREBUFFER_THRESHOLD    (2 * 1024 * 1024)  /* 2 MB (~10-12s de audio FLAC) prellenado para streaming sin cortes */

#define CACHE_DIR_PATH         "/dev_hdd0/tmp/monochrome/cache"
#define CACHE_MAX_FILES        25
#define CACHE_MAX_BYTES        (512ULL * 1024ULL * 1024ULL)
#define MAX_STREAM_RESUME_RETRIES 5

#define RING_BUFFER_FRAMES 576000                 /* 12.0s at 48kHz stereo float = 4.5 MB */
#define RING_PREFILL_FRAMES 48000                 /* 1.0s de PCM 48 kHz prellenado antes de reproducir */
#define RING_REBUFFER_FRAMES 24000                /* 0.5s para reanudar tras underrun real (histeresis) */
#define CHUNK_OUT_FRAMES 1280                     /* Multiple of 160: 26.67 ms */
#define CHUNK_IN_44K 1176                         /* 1280 * 147 / 160 = 1176 frames at 44.1 kHz */

/* ── Context for Progressive FLAC Streaming ── */
typedef struct {
    uint8_t *data;
    size_t   capacity;
    volatile uint64_t received;
    volatile uint64_t cursor;
    volatile bool download_complete;
    volatile bool abort;
    volatile uint32_t seq;
    char     track_id[64];
    char     title[128];
    char     cache_path[256];
    bool     is_cached;
    int      sock;
} StreamContext;

/* Reference table from PC analysis for Big-Endian verification */
typedef struct {
    const char *id;
    const char *title;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits_per_sample;
    uint64_t total_frames;
} PcTrackReference;

static const PcTrackReference s_pc_refs[5] __attribute__((unused)) = {
    { "155142491052445696", "Instant Crush",               44100, 2, 16, 14886396ULL },
    { "192484768301232128", "Blinding Lights",             48000, 2, 24,  7600825ULL },
    { "159468426658455552", "Around the World",            44100, 2, 16,  9130166ULL },
    { "185831396026093568", "Starboy",                     48000, 2, 24, 10807423ULL },
    { "155142639597916160", "Lose Yourself to Dance",      44100, 2, 16, 11396028ULL },
};

/* ── Stream Synchronization Mutex & Condvar (NEVER acquired by audio_hw_thread_func!) ── */
static sys_mutex_t s_stream_mutex;
static bool s_stream_mutex_inited = false;
static sys_cond_t s_stream_cond;
static bool s_stream_cond_inited = false;

static uint64_t get_time_usec(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* ── Hardware Audio State ── */
static u32 s_port_num = 0;
static audioPortConfig s_config;
static sys_event_queue_t s_snd_queue;
static sys_ipc_key_t s_snd_key = 0;
static sys_ppu_thread_t s_hw_thread_id;
static sys_ppu_thread_t s_stream_thread_id;
static sys_ppu_thread_t s_download_thread_id;
static bool s_hw_thread_created = false;
static bool s_stream_thread_created = false;
static bool s_download_thread_created = false;
static volatile uint32_t s_track_sequence = 0;

static bool s_audio_inited = false;
static bool s_audio_port_opened = false;
static bool s_audio_port_started = false;

static volatile bool s_engine_running = false;
static volatile bool s_is_playing = false;
static volatile bool s_ring_primed = false;
static volatile float s_volume = 0.85f;
static volatile uint32_t s_hw_underruns = 0;
static volatile uint32_t s_ring_underruns = 0;
static u32 s_write_block = 0;

static float s_vis_bands[VIS_BANDS];

/* Live status message for UI (protected by s_stream_mutex) */
static char s_status_message[128] = "Listo";
static char s_status_snapshot[128] = "Listo";
static volatile bool s_has_error = false;
static volatile int  s_last_error_code = 0;

static void set_status_message(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';

    if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
    memcpy(s_status_message, buf, sizeof(s_status_message));
    if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
}

const char *audio_engine_get_status_message(void)
{
    if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
    memcpy(s_status_snapshot, s_status_message, sizeof(s_status_snapshot));
    s_status_snapshot[sizeof(s_status_snapshot) - 1] = '\0';
    if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
    return s_status_snapshot;
}

bool audio_engine_has_error(void)
{
    return s_has_error;
}

int audio_engine_get_last_error(void)
{
    return s_last_error_code;
}

void audio_engine_clear_error(void)
{
    if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
    s_has_error = false;
    s_last_error_code = 0;
    if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
}

/* ── Lock-Free SPSC Ring Buffer (PPC lwsync/isync barriers) ── */
static float s_ring_buffer[RING_BUFFER_FRAMES * 2];
static volatile size_t s_ring_write_pos = 0;
static volatile size_t s_ring_read_pos = 0;

static inline size_t ring_frames_available(void)
{
    size_t w = s_ring_write_pos;
    __asm__ volatile("lwsync" ::: "memory");
    size_t r = s_ring_read_pos;
    if (w >= r) return w - r;
    return RING_BUFFER_FRAMES - (r - w);
}

static inline size_t ring_space_available(void)
{
    size_t w = s_ring_write_pos;
    size_t r = s_ring_read_pos;
    __asm__ volatile("lwsync" ::: "memory");
    size_t occupied;
    if (w >= r) occupied = w - r;
    else occupied = RING_BUFFER_FRAMES - (r - w);
    if (occupied >= RING_BUFFER_FRAMES - 1) return 0;
    return (RING_BUFFER_FRAMES - 1) - occupied;
}

static void ring_write_frames(const float *src, size_t count)
{
    size_t w = s_ring_write_pos;
    for (size_t i = 0; i < count; i++) {
        s_ring_buffer[w * 2 + 0] = src[i * 2 + 0];
        s_ring_buffer[w * 2 + 1] = src[i * 2 + 1];
        w = (w + 1) % RING_BUFFER_FRAMES;
    }
    __asm__ volatile("lwsync" ::: "memory");
    s_ring_write_pos = w;
}

static void ring_read_frames(float *dst, size_t count)
{
    size_t r = s_ring_read_pos;
    __asm__ volatile("isync" ::: "memory");
    for (size_t i = 0; i < count; i++) {
        dst[i * 2 + 0] = s_ring_buffer[r * 2 + 0];
        dst[i * 2 + 1] = s_ring_buffer[r * 2 + 1];
        r = (r + 1) % RING_BUFFER_FRAMES;
    }
    __asm__ volatile("lwsync" ::: "memory");
    s_ring_read_pos = r;
}

/* ── Streaming Buffer & Context ── */
static uint8_t *s_stream_media_buffer = NULL;
static StreamContext s_stream_ctx;

static char s_pending_track_id[64] = "";
static char s_pending_track_title[128] = "";

static volatile uint64_t s_total_pcm_frames = 0;
static volatile uint64_t s_current_pcm_frame = 0;
static volatile uint64_t s_played_pcm_frames = 0;
static volatile uint32_t s_stream_sample_rate = 44100;

/* Resampler boundary */
static float s_resample_boundary[2] = { 0.0f, 0.0f };

/* ── Socket Ownership Helper (ONLY mono_net calls close(sock)) ── */
static void close_stream_sock_net_only(int sock)
{
    if (sock < 0) return;
    if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
    if (s_stream_ctx.sock == sock) {
        s_stream_ctx.sock = -1;
    }
    if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
    close(sock);
}

/* ── Ring Buffer Write Helper for Progressive FLAC Streaming ── */
static void stream_ring_write(const uint8_t *src, size_t len)
{
    if (!src || len == 0 || !s_stream_ctx.data || s_stream_ctx.capacity == 0) return;
    size_t cap = s_stream_ctx.capacity;
    size_t off = (size_t)(s_stream_ctx.received % cap);
    size_t contig = cap - off;
    if (len <= contig) {
        memcpy(s_stream_ctx.data + off, src, len);
    } else {
        memcpy(s_stream_ctx.data + off, src, contig);
        memcpy(s_stream_ctx.data, src + contig, len - contig);
    }
    __asm__ volatile("lwsync" ::: "memory");
    s_stream_ctx.received += len;
    if (s_stream_mutex_inited && s_stream_cond_inited) {
        sysMutexLock(s_stream_mutex, 0);
        sysCondBroadcast(s_stream_cond);
        sysMutexUnlock(s_stream_mutex);
    }
}

/* ── dr_flac Progressive In-Memory Ring Stream Callbacks ── */
static size_t flac_stream_read(void *pUserData, void *pBufferOut, size_t bytesToRead)
{
    StreamContext *ctx = (StreamContext *)pUserData;
    uint64_t wait_start = net_hal_time_ms();

    if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
    while (ctx->cursor >= ctx->received) {
        if (ctx->download_complete || ctx->abort || s_has_error ||
            !s_engine_running || ctx->seq != s_track_sequence) {
            if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
            return 0;
        }
        if (net_hal_time_ms() - wait_start > 25000ULL) {
            if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
            net_hal_log("AUDIO", "[STREAM TIMEOUT] 25s sin nuevos datos recibidos en flac_stream_read");
            return 0;
        }
        if (s_stream_cond_inited) {
            sysCondWait(s_stream_cond, 20000); /* 20ms max slice -> <50ms abort latency */
        } else {
            if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
            usleep(10000);
            if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
        }
    }
    if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);

    __asm__ volatile("lwsync" ::: "memory");
    uint64_t rec = ctx->received;
    if (ctx->cursor >= rec) return 0;

    uint64_t avail = rec - ctx->cursor;
    size_t to_copy = (avail < (uint64_t)bytesToRead) ? (size_t)avail : bytesToRead;
    size_t cap = ctx->capacity;
    size_t off = (size_t)(ctx->cursor % cap);
    size_t contig = cap - off;

    if (to_copy <= contig) {
        memcpy(pBufferOut, ctx->data + off, to_copy);
    } else {
        memcpy(pBufferOut, ctx->data + off, contig);
        memcpy((uint8_t *)pBufferOut + contig, ctx->data, to_copy - contig);
    }

    __asm__ volatile("lwsync" ::: "memory");
    ctx->cursor += to_copy;
    return to_copy;
}

static drflac_bool32 flac_stream_seek(void *pUserData, int offset, drflac_seek_origin origin)
{
    StreamContext *ctx = (StreamContext *)pUserData;
    uint64_t new_pos = 0;
    if (origin == DRFLAC_SEEK_SET) {
        if (offset < 0) return DRFLAC_FALSE;
        new_pos = (uint64_t)offset;
    } else if (origin == DRFLAC_SEEK_CUR) {
        if (offset < 0 && (uint64_t)(-offset) > ctx->cursor) return DRFLAC_FALSE;
        new_pos = (offset < 0) ? (ctx->cursor - (uint64_t)(-offset)) : (ctx->cursor + (uint64_t)offset);
    } else {
        return DRFLAC_FALSE;
    }

    /* Si los datos solicitados ya fueron sobrescritos en el anillo, rechazar seek */
    if (ctx->received > ctx->capacity && new_pos < (ctx->received - ctx->capacity)) {
        return DRFLAC_FALSE;
    }

    uint64_t wait_start = net_hal_time_ms();
    if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
    while (new_pos > ctx->received) {
        if (ctx->download_complete || ctx->abort || s_has_error ||
            !s_engine_running || ctx->seq != s_track_sequence) {
            if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
            return DRFLAC_FALSE;
        }
        if (net_hal_time_ms() - wait_start > 25000ULL) {
            if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
            return DRFLAC_FALSE;
        }
        if (s_stream_cond_inited) {
            sysCondWait(s_stream_cond, 20000);
        } else {
            if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
            usleep(10000);
            if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
        }
    }
    if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);

    __asm__ volatile("lwsync" ::: "memory");
    ctx->cursor = new_pos;
    return DRFLAC_TRUE;
}

static drflac_bool32 flac_stream_tell(void *pUserData, drflac_int64 *pCursor)
{
    StreamContext *ctx = (StreamContext *)pUserData;
    if (!ctx || !pCursor) return DRFLAC_FALSE;
    *pCursor = (drflac_int64)ctx->cursor;
    return DRFLAC_TRUE;
}

/* ── Hardware Audio Consumer Thread (Priority 100, STRICTLY LOCK-FREE!) ── */
static void audio_hw_thread_func(void *arg)
{
    (void)arg;

    float temp_block[AUDIO_BLOCK_SAMPLES * 2];

    while (s_engine_running) {
        sys_event_t event;
        int ret = sysEventQueueReceive(s_snd_queue, &event, 25 * 1000);
        if (ret != 0) {
            s_hw_underruns++;
            usleep(1000);
            continue;
        }

        u32 nblocks = (u32)s_config.numBlocks;
        if (nblocks == 0 || nblocks > 32) nblocks = 8;
        s_write_block = (s_write_block + 1) % nblocks;
        u32 write_block = s_write_block;
        float *dest = (float *)((u64)s_config.audioDataStart) + (NUM_CHANNELS * AUDIO_BLOCK_SAMPLES * write_block);

        if (!s_is_playing || !s_ring_primed) {
            memset(dest, 0, AUDIO_BLOCK_SAMPLES * NUM_CHANNELS * sizeof(float));
            for (int b = 0; b < VIS_BANDS; b++) s_vis_bands[b] *= 0.85f;
            continue;
        }

        size_t avail = ring_frames_available();
        if (avail < AUDIO_BLOCK_SAMPLES) {
            s_ring_primed = false;
            s_ring_underruns++;
            memset(dest, 0, AUDIO_BLOCK_SAMPLES * NUM_CHANNELS * sizeof(float));
            for (int b = 0; b < VIS_BANDS; b++) s_vis_bands[b] *= 0.85f;
            continue;
        }

        ring_read_frames(temp_block, AUDIO_BLOCK_SAMPLES);

        uint32_t sr = s_stream_sample_rate;
        if (sr == 0) sr = 44100;
        s_played_pcm_frames += ((uint64_t)AUDIO_BLOCK_SAMPLES * (uint64_t)sr) / 48000ULL;

        float band_energy[VIS_BANDS];
        memset(band_energy, 0, sizeof(band_energy));

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            float l = temp_block[i * 2 + 0] * s_volume;
            float r = temp_block[i * 2 + 1] * s_volume;

            if (l > 1.0f)  l = 1.0f;
            if (l < -1.0f) l = -1.0f;
            if (r > 1.0f)  r = 1.0f;
            if (r < -1.0f) r = -1.0f;

            dest[i * 2 + 0] = l;
            dest[i * 2 + 1] = r;

            int bin = (i * VIS_BANDS) / AUDIO_BLOCK_SAMPLES;
            band_energy[bin] += (fabsf(l) + fabsf(r));
        }

        for (int b = 0; b < VIS_BANDS; b++) {
            float avg = band_energy[b] / (float)(AUDIO_BLOCK_SAMPLES / VIS_BANDS);
            s_vis_bands[b] = (s_vis_bands[b] * 0.60f) + (avg * 0.40f);
            if (s_vis_bands[b] > 1.0f) s_vis_bands[b] = 1.0f;
        }
    }

    sysThreadExit(0);
}

/* ── Disk Cache Quota Eviction (FIFO/mtime LRU, max 25 files / 512 MB) ── */
static void enforce_cache_quota(void)
{
    for (int pass = 0; pass < 64; pass++) {
        DIR *dir = opendir(CACHE_DIR_PATH);
        if (!dir) return;

        int file_count = 0;
        uint64_t total_bytes = 0;
        time_t oldest_mtime = 0;
        char oldest_path[1088] = "";

        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char fpath[1088];
            snprintf(fpath, sizeof(fpath), "%s/%s", CACHE_DIR_PATH, ent->d_name);
            struct stat st;
            if (stat(fpath, &st) == 0 && S_ISREG(st.st_mode)) {
                file_count++;
                total_bytes += (uint64_t)st.st_size;
                if (oldest_path[0] == '\0' || st.st_mtime < oldest_mtime) {
                    oldest_mtime = st.st_mtime;
                    strncpy(oldest_path, fpath, sizeof(oldest_path) - 1);
                    oldest_path[sizeof(oldest_path) - 1] = '\0';
                }
            }
        }
        closedir(dir);

        if ((file_count < CACHE_MAX_FILES && total_bytes <= CACHE_MAX_BYTES) || oldest_path[0] == '\0') {
            break;
        }

        if (unlink(oldest_path) == 0) {
            net_hal_log("AUDIO", "[CACHE EVICT] Eliminado archivo antiguo de cache: %s (files=%d, bytes=%llu)",
                        oldest_path, file_count, (unsigned long long)total_bytes);
        } else {
            break;
        }
    }
}

/* ── HTTP Connection Helper (Supports Initial GET & HTTP Range Resumption) ── */
static int stream_connect_http(const char *track_id, uint32_t seq, uint64_t offset,
                               uint64_t *out_content_length, char *out_content_type, size_t ct_size)
{
    if (out_content_length) *out_content_length = 0;
    if (out_content_type && ct_size > 0) out_content_type[0] = '\0';

    if (!net_hal_validate_id(track_id)) {
        net_hal_log("AUDIO", "[STREAM ERROR] track_id invalido rechazado antes de GET");
        return -400;
    }

    int max_retries = http_get_max_retries();
    if (max_retries < 1) max_retries = 3;
    if (max_retries > 5) max_retries = 5;

    int timeout_ms = http_get_timeout_ms();
    if (timeout_ms < 2000) timeout_ms = 10000;

    for (int attempt = 0; attempt < max_retries && s_engine_running && seq == s_track_sequence && !s_stream_ctx.abort; attempt++) {
        if (!net_hal_is_connected()) {
            net_hal_log("AUDIO", "[STREAM NET] Sin IP activa antes de conectar (intento %d)", attempt + 1);
            return -1;
        }

        if (attempt > 0) {
            int delay_ms = 1000 * (1 << (attempt - 1));
            if (delay_ms > 8000) delay_ms = 8000;
            net_hal_log("AUDIO", "[STREAM RETRY] Intento %d/%d (espera %d ms)...", attempt + 1, max_retries, delay_ms);
            uint64_t w_start = net_hal_time_ms();
            while (s_engine_running && seq == s_track_sequence && !s_stream_ctx.abort &&
                   (net_hal_time_ms() - w_start) < (uint64_t)delay_ms) {
                if (!net_hal_is_connected()) return -1;
                usleep(20000);
            }
            if (seq != s_track_sequence || !s_engine_running || s_stream_ctx.abort) return -1;
        }

        int sock = -1;
        int net_err = 0;
        if (net_hal_connect(API_HOST, API_PORT, timeout_ms, &s_stream_ctx.abort, &sock, &net_err) != 0 || sock < 0) {
            net_hal_log("AUDIO", "[ERROR] net_hal_connect a %s:%d fallo (errno: %d)", API_HOST, API_PORT, net_err);
            continue;
        }

        int rcvbuf = 64 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
        if (!s_engine_running || seq != s_track_sequence || s_stream_ctx.abort) {
            if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
            close(sock);
            return -1;
        }
        s_stream_ctx.sock = sock;
        if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);

        char req[1024];
        if (offset > 0) {
            snprintf(req, sizeof(req),
                     "GET /track/%s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Range: bytes=%llu-\r\n"
                     "User-Agent: Monochrome-PS3/2.4.0 (PSL1GHT)\r\n"
                     "Accept: application/octet-stream, audio/flac, */*\r\n"
                     "Connection: keep-alive\r\n\r\n",
                     track_id, API_HOST, (unsigned long long)offset);
        } else {
            snprintf(req, sizeof(req),
                     "GET /track/%s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: Monochrome-PS3/2.4.0 (PSL1GHT)\r\n"
                     "Accept: application/octet-stream, audio/flac, */*\r\n"
                     "Connection: keep-alive\r\n\r\n",
                     track_id, API_HOST);
        }

        int s_ret = net_hal_send_all(sock, req, strlen(req), timeout_ms, &s_stream_ctx.abort, &net_err);
        if (s_ret != 0) {
            net_hal_log("AUDIO", "[ERROR] net_hal_send_all fallo (ret: %d, errno: %d)", s_ret, net_err);
            close_stream_sock_net_only(sock);
            continue;
        }

        /* Read headers */
        char head_buf[4096];
        size_t head_len = 0;
        char *body_start = NULL;

        while (s_engine_running && seq == s_track_sequence && !s_stream_ctx.abort &&
               head_len < sizeof(head_buf) - 1) {
            int n = net_hal_recv_some(sock, head_buf + head_len, sizeof(head_buf) - head_len - 1,
                                      timeout_ms, &s_stream_ctx.abort, &net_err);
            if (n <= 0) break;
            head_len += (size_t)n;
            head_buf[head_len] = '\0';
            body_start = strstr(head_buf, "\r\n\r\n");
            if (body_start) {
                body_start += 4;
                break;
            }
        }

        if (!body_start) {
            net_hal_log("AUDIO", "[ERROR] Cabecera HTTP incompleta o timeout (errno: %d)", net_err);
            close_stream_sock_net_only(sock);
            continue;
        }

        int http_code = 0;
        sscanf(head_buf, "HTTP/%*s %d", &http_code);

        if (http_code == 429) {
            net_hal_log("AUDIO", "[HTTP 429] Rate limited, reintentando...");
            close_stream_sock_net_only(sock);
            continue;
        }

        if (http_code == 404 || http_code == 403 || http_code == 410) {
            net_hal_log("AUDIO", "[STREAM HTTP] Error permanente HTTP %d para pista %s", http_code, track_id);
            s_last_error_code = http_code;
            close_stream_sock_net_only(sock);
            return -http_code;
        }

        if (http_code != 200 && http_code != 206) {
            net_hal_log("AUDIO", "[ERROR] HTTP codigo %d no esperado", http_code);
            close_stream_sock_net_only(sock);
            continue;
        }

        if (offset > 0 && http_code == 200) {
            net_hal_log("AUDIO", "[STREAM RESUME] Servidor ignoro Range devolviendo HTTP 200 en offset %llu",
                        (unsigned long long)offset);
            close_stream_sock_net_only(sock);
            return -200;
        }

        /* Parse Content-Length */
        uint64_t clen = 0;
        char *cl_ptr = strstr(head_buf, "Content-Length:");
        if (!cl_ptr) cl_ptr = strstr(head_buf, "content-length:");
        if (cl_ptr) {
            unsigned long long cl_tmp = 0;
            if (sscanf(cl_ptr + 15, " %llu", &cl_tmp) == 1) {
                clen = (uint64_t)cl_tmp;
            }
        }

        /* If Range (206), check Content-Range for total length */
        if (http_code == 206) {
            char *cr_ptr = strstr(head_buf, "Content-Range:");
            if (!cr_ptr) cr_ptr = strstr(head_buf, "content-range:");
            if (cr_ptr) {
                char *slash = strchr(cr_ptr, '/');
                if (slash) {
                    unsigned long long total_tmp = 0;
                    if (sscanf(slash + 1, "%llu", &total_tmp) == 1 && total_tmp > 0) {
                        clen = (uint64_t)total_tmp;
                    }
                }
            }
        }

        if (out_content_length) *out_content_length = clen;

        /* Parse Content-Type */
        if (out_content_type && ct_size > 0) {
            char *ct_ptr = strstr(head_buf, "Content-Type:");
            if (!ct_ptr) ct_ptr = strstr(head_buf, "content-type:");
            if (ct_ptr) {
                sscanf(ct_ptr + 13, " %63s", out_content_type);
            }
        }

        net_hal_log("AUDIO", "[STREAM HTTP] Conectado: Codigo %d | Content-Length: %llu bytes",
                    http_code, (unsigned long long)clen);

        /* Copy any excess body bytes into sliding ring buffer */
        size_t header_consumed = (size_t)(body_start - head_buf);
        size_t initial_body_bytes = head_len - header_consumed;
        if (initial_body_bytes > 0) {
            while (s_engine_running && seq == s_track_sequence && !s_stream_ctx.abort) {
                uint64_t ahead = (s_stream_ctx.received >= s_stream_ctx.cursor) ? (s_stream_ctx.received - s_stream_ctx.cursor) : 0;
                if (ahead + initial_body_bytes <= s_stream_ctx.capacity) {
                    stream_ring_write((const uint8_t *)body_start, initial_body_bytes);
                    break;
                }
                usleep(10000);
            }
        }

        return sock;
    }

    return -1;
}

/* ── Dedicated HTTP Streaming Download Worker Thread (PPU prio 750) ── */
static void stream_download_worker_func(void *arg)
{
    (void)arg;
    net_hal_log("AUDIO", "[NET WORKER] Hilo de descarga iniciado (PPU prio 750)");

    uint32_t current_seq = 0;
    uint32_t total_reconnects = 0;

    while (s_engine_running) {
        if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
        while (s_engine_running && current_seq == s_track_sequence) {
            if (s_stream_cond_inited) {
                sysCondWait(s_stream_cond, 250000);
            } else {
                sysMutexUnlock(s_stream_mutex);
                usleep(10000);
                sysMutexLock(s_stream_mutex, 0);
            }
        }
        if (!s_engine_running) {
            if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
            break;
        }

        current_seq = s_track_sequence;

        /* Reset stream context safely under s_stream_mutex */
        memset(&s_stream_ctx, 0, sizeof(StreamContext));
        s_stream_ctx.sock = -1;
        s_stream_ctx.data = s_stream_media_buffer;
        s_stream_ctx.capacity = STREAM_BUFFER_CAPACITY;
        s_stream_ctx.abort = false;
        strncpy(s_stream_ctx.track_id, s_pending_track_id, sizeof(s_stream_ctx.track_id) - 1);
        strncpy(s_stream_ctx.title, s_pending_track_title, sizeof(s_stream_ctx.title) - 1);
        if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);

        net_hal_log("AUDIO", "==================================================");
        net_hal_log("AUDIO", "[PISTA ELEGIDA] '%s' [ID: %s]", s_stream_ctx.title, s_stream_ctx.track_id);

        if (!net_hal_validate_id(s_stream_ctx.track_id)) {
            net_hal_log("AUDIO", "[STREAM ERROR] ID invalido '%s'", s_stream_ctx.track_id);
            if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
            s_last_error_code = 400;
            s_has_error = true;
            s_is_playing = false;
            s_stream_ctx.abort = true;
            snprintf(s_status_message, sizeof(s_status_message), "ID de pista invalido");
            if (s_stream_cond_inited) sysCondBroadcast(s_stream_cond);
            if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
            continue;
        }

        /* 1. Check local disk cache first */
        char c_path[256];
        snprintf(c_path, sizeof(c_path), "%s/%s.flac", CACHE_DIR_PATH, s_stream_ctx.track_id);
        strncpy(s_stream_ctx.cache_path, c_path, sizeof(s_stream_ctx.cache_path) - 1);

        FILE *f_cache = fopen(c_path, "rb");
        if (!f_cache) {
            char t_path[256];
            snprintf(t_path, sizeof(t_path), "/dev_hdd0/tmp/monochrome/tracks/%s.flac", s_stream_ctx.track_id);
            f_cache = fopen(t_path, "rb");
            if (f_cache) {
                strncpy(s_stream_ctx.cache_path, t_path, sizeof(s_stream_ctx.cache_path) - 1);
            }
        }

        if (f_cache) {
            bool cache_loaded = false;
            fseek(f_cache, 0, SEEK_END);
            long fsize = ftell(f_cache);
            fseek(f_cache, 0, SEEK_SET);

            if (fsize > 1024 && (size_t)fsize <= STREAM_BUFFER_CAPACITY) {
                size_t read_bytes = fread(s_stream_ctx.data, 1, (size_t)fsize, f_cache);
                if (read_bytes == (size_t)fsize) {
                    if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
                    s_stream_ctx.received = read_bytes;
                    s_stream_ctx.download_complete = true;
                    s_stream_ctx.is_cached = true;
                    __asm__ volatile("lwsync" ::: "memory");
                    s_stream_ctx.seq = current_seq;
                    snprintf(s_status_message, sizeof(s_status_message), "Cache local (%ld KB)", fsize / 1024);
                    if (s_stream_cond_inited) sysCondBroadcast(s_stream_cond);
                    if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);

                    net_hal_log("AUDIO", "[CACHE HIT] Pista cargada desde disco: %s (%ld KB) - Reproduccion inmediata",
                                s_stream_ctx.cache_path, fsize / 1024);
                    cache_loaded = true;
                }
            }
            fclose(f_cache);
            if (cache_loaded) continue;
        }

        /* 2. Connect to HTTP API and download */
        uint64_t content_length = 0;
        char content_type[64] = "desconocido";

        if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
        s_stream_ctx.seq = current_seq;
        __asm__ volatile("lwsync" ::: "memory");
        if (s_stream_cond_inited) sysCondBroadcast(s_stream_cond);
        if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);

        int sock = stream_connect_http(s_stream_ctx.track_id, current_seq, 0, &content_length, content_type, sizeof(content_type));
        if (sock < 0) {
            if (current_seq == s_track_sequence && s_engine_running) {
                net_hal_log("AUDIO", "[STREAM ERROR] No se pudo conectar a la API para la pista %s (ret: %d)", s_stream_ctx.track_id, sock);
                if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
                if (sock == -404) {
                    snprintf(s_status_message, sizeof(s_status_message), "Pista no disponible (404)");
                    s_last_error_code = 404;
                } else if (sock == -403) {
                    snprintf(s_status_message, sizeof(s_status_message), "Acceso restringido (403)");
                    s_last_error_code = 403;
                } else if (sock == -410) {
                    snprintf(s_status_message, sizeof(s_status_message), "Pista retirada (410)");
                    s_last_error_code = 410;
                } else if (!net_hal_is_connected()) {
                    snprintf(s_status_message, sizeof(s_status_message), "Sin conexion de red");
                    s_last_error_code = 0;
                } else {
                    snprintf(s_status_message, sizeof(s_status_message), "Error de conexion");
                    s_last_error_code = 500;
                }
                s_has_error = true;
                s_is_playing = false;
                s_stream_ctx.abort = true;
                __asm__ volatile("lwsync" ::: "memory");
                if (s_stream_cond_inited) sysCondBroadcast(s_stream_cond);
                if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
            }
            continue;
        }

        uint64_t t_down_start = get_time_usec();
        uint64_t last_log_time = t_down_start;
        uint64_t last_logged_bytes = s_stream_ctx.received;
        uint32_t last_kbps = 0;
        int resume_attempts = 0;

        set_status_message("Descargando stream...");

        while (s_engine_running && current_seq == s_track_sequence && !s_stream_ctx.abort && !s_stream_ctx.download_complete) {
            /* Esperar si la ventana deslizante de 24 MB está llena por delante del decodificador */
            while (s_engine_running && current_seq == s_track_sequence && !s_stream_ctx.abort && !s_stream_ctx.download_complete) {
                uint64_t cur = s_stream_ctx.cursor;
                __asm__ volatile("lwsync" ::: "memory");
                uint64_t rec = s_stream_ctx.received;
                uint64_t ahead = (rec >= cur) ? (rec - cur) : 0;
                if (ahead < (uint64_t)(s_stream_ctx.capacity - 131072u)) {
                    break;
                }
                usleep(20000);
            }
            if (!s_engine_running || current_seq != s_track_sequence || s_stream_ctx.abort || s_stream_ctx.download_complete) {
                break;
            }

            if (sock < 0) {
                /* Pause cleanly if NetCtl link is down; wait for IP to return */
                if (!net_hal_is_connected()) {
                    set_status_message("Sin red - esperando reconexion...");
                    net_hal_log("AUDIO", "[STREAM NET] Enlace perdido; pausando reconexion hasta recuperar IP");
                    while (s_engine_running && current_seq == s_track_sequence && !s_stream_ctx.abort && !net_hal_is_connected()) {
                        usleep(250000);
                    }
                    if (!s_engine_running || current_seq != s_track_sequence || s_stream_ctx.abort) {
                        break;
                    }
                    net_hal_log("AUDIO", "[STREAM NET] Enlace IP recuperado, reanudando via Range...");
                    set_status_message("Reconectando stream...");
                    resume_attempts = 0;
                }

                if (content_length > 0 && s_stream_ctx.received < content_length) {
                    resume_attempts++;
                    if (resume_attempts > MAX_STREAM_RESUME_RETRIES) {
                        net_hal_log("AUDIO", "[STREAM ERROR] Agotados %d reintentos de reconexion Range para %s",
                                    MAX_STREAM_RESUME_RETRIES, s_stream_ctx.track_id);
                        if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
                        snprintf(s_status_message, sizeof(s_status_message), "Conexion perdida tras reintentos");
                        s_last_error_code = 503;
                        s_has_error = true;
                        s_is_playing = false;
                        s_stream_ctx.abort = true;
                        if (s_stream_cond_inited) sysCondBroadcast(s_stream_cond);
                        if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
                        break;
                    }

                    net_hal_log("AUDIO", "[STREAM RESUME] Reconectando via Range (%d/%d, recibidos: %llu / %llu bytes)...",
                                resume_attempts, MAX_STREAM_RESUME_RETRIES,
                                (unsigned long long)s_stream_ctx.received, (unsigned long long)content_length);
                    sock = stream_connect_http(s_stream_ctx.track_id, current_seq, s_stream_ctx.received, NULL, NULL, 0);
                    if (sock == -200) {
                        /* Server ignored Range and responded HTTP 200 */
                        if (s_stream_ctx.cursor == 0 && s_stream_ctx.received < PREBUFFER_THRESHOLD) {
                            net_hal_log("AUDIO", "[STREAM RESUME] Reiniciando stream desde offset 0 (playback aun no iniciado)");
                            if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
                            s_stream_ctx.received = 0;
                            if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
                            sock = stream_connect_http(s_stream_ctx.track_id, current_seq, 0, &content_length, NULL, 0);
                        }
                        if (sock < 0) {
                            if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
                            snprintf(s_status_message, sizeof(s_status_message), "Servidor no soporta reanudacion");
                            s_last_error_code = 200;
                            s_has_error = true;
                            s_is_playing = false;
                            s_stream_ctx.abort = true;
                            if (s_stream_cond_inited) sysCondBroadcast(s_stream_cond);
                            if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
                            break;
                        }
                    } else if (sock == -404 || sock == -403 || sock == -410 || sock == -400) {
                        if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
                        snprintf(s_status_message, sizeof(s_status_message), "Error HTTP %d al reanudar", -sock);
                        s_last_error_code = -sock;
                        s_has_error = true;
                        s_is_playing = false;
                        s_stream_ctx.abort = true;
                        if (s_stream_cond_inited) sysCondBroadcast(s_stream_cond);
                        if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
                        break;
                    } else if (sock < 0) {
                        uint64_t bw_start = net_hal_time_ms();
                        while (s_engine_running && current_seq == s_track_sequence && !s_stream_ctx.abort &&
                               (net_hal_time_ms() - bw_start) < 1000ULL) {
                            usleep(20000);
                        }
                        continue;
                    }

                    resume_attempts = 0;
                    total_reconnects++;
                    net_hal_record_stream_telemetry(last_kbps, total_reconnects, s_ring_underruns);
                    set_status_message("Descargando stream...");
                } else {
                    break;
                }
            }

            uint64_t cur = s_stream_ctx.cursor;
            __asm__ volatile("lwsync" ::: "memory");
            uint64_t rec = s_stream_ctx.received;
            uint64_t ahead = (rec >= cur) ? (rec - cur) : 0;
            size_t cap = s_stream_ctx.capacity;
            size_t space_free = (cap > 65536u && ahead < (uint64_t)(cap - 65536u)) ? (size_t)((uint64_t)(cap - 65536u) - ahead) : 0;
            if (space_free == 0) {
                usleep(10000);
                continue;
            }

            size_t off = (size_t)(rec % cap);
            size_t contig_space = cap - off;
            size_t to_read = (space_free < contig_space) ? space_free : contig_space;
            if (to_read > 65536u) to_read = 65536u;

            int timeout_ms = http_get_timeout_ms();
            if (timeout_ms < 2000) timeout_ms = 10000;
            int net_err = 0;
            int n = net_hal_recv_some(sock, s_stream_ctx.data + off, to_read, timeout_ms, &s_stream_ctx.abort, &net_err);
            if (n > 0) {
                resume_attempts = 0;
                __asm__ volatile("lwsync" ::: "memory");
                s_stream_ctx.received += (size_t)n;
                if (s_stream_mutex_inited && s_stream_cond_inited) {
                    sysMutexLock(s_stream_mutex, 0);
                    sysCondBroadcast(s_stream_cond);
                    sysMutexUnlock(s_stream_mutex);
                }

                uint64_t now = get_time_usec();
                double dt_s = (double)(now - last_log_time) / 1000000.0;
                if (dt_s >= 1.0 || s_stream_ctx.received >= last_logged_bytes + 524288ULL) {
                    double kb = (double)(s_stream_ctx.received - last_logged_bytes) / 1024.0;
                    double speed_kb_s = (dt_s > 0.0) ? (kb / dt_s) : 0.0;
                    last_kbps = (uint32_t)(speed_kb_s * 8.0);
                    net_hal_record_stream_telemetry(last_kbps, total_reconnects, s_ring_underruns);
                    net_hal_log("AUDIO", "[STREAM RECV] Descarga: %llu / %llu bytes (%.1f%%) | %.1f KB/s",
                                (unsigned long long)s_stream_ctx.received,
                                (unsigned long long)(content_length ? content_length : s_stream_ctx.received),
                                content_length ? ((double)s_stream_ctx.received * 100.0 / (double)content_length) : 100.0,
                                speed_kb_s);
                    last_logged_bytes = s_stream_ctx.received;
                    last_log_time = now;
                }

                if (content_length > 0 && s_stream_ctx.received >= content_length) {
                    close_stream_sock_net_only(sock);
                    sock = -1;

                    if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
                    s_stream_ctx.download_complete = true;
                    if (s_stream_cond_inited) sysCondBroadcast(s_stream_cond);
                    if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);

                    uint64_t t_down_end = get_time_usec();
                    double down_s = (double)(t_down_end - t_down_start) / 1000000.0;
                    double mb = (double)s_stream_ctx.received / (1024.0 * 1024.0);
                    double speed_mb_s = (down_s > 0.0) ? (mb / down_s) : 0.0;
                    net_hal_log("AUDIO", "[STREAM COMPLETO] Descargados %.2f MB en %.2f s (Velocidad: %.2f MB/s)",
                                mb, down_s, speed_mb_s);

                    /* Save to disk cache only if the full file fits unwrapped inside the 24 MB buffer */
                    if (s_stream_ctx.received <= s_stream_ctx.capacity) {
                        enforce_cache_quota();
                        FILE *fc = fopen(s_stream_ctx.cache_path, "wb");
                        if (fc) {
                            size_t written = fwrite(s_stream_ctx.data, 1, (size_t)s_stream_ctx.received, fc);
                            int f_err = errno;
                            fclose(fc);
                            if (written == (size_t)s_stream_ctx.received) {
                                net_hal_log("AUDIO", "[CACHE GUARDADO] %llu bytes en %s",
                                            (unsigned long long)s_stream_ctx.received, s_stream_ctx.cache_path);
                            } else {
                                unlink(s_stream_ctx.cache_path);
                                net_hal_log("AUDIO", "[CACHE ERROR] Escritura incompleta (%lu/%llu, errno=%d), borrado parcial",
                                            (unsigned long)written, (unsigned long long)s_stream_ctx.received, f_err);
                            }
                        }
                    } else {
                        net_hal_log("AUDIO", "[STREAM HI-RES] Pista >24MB (%llu bytes) reproducida via buffer circular",
                                    (unsigned long long)s_stream_ctx.received);
                    }
                    break;
                }
            } else if (n == 0) {
                close_stream_sock_net_only(sock);
                sock = -1;
                if (content_length > 0 && s_stream_ctx.received < content_length) {
                    net_hal_log("AUDIO", "[STREAM DISCONNECT] Servidor cerro conexion a los %llu / %llu bytes. Reanudando...",
                                (unsigned long long)s_stream_ctx.received, (unsigned long long)content_length);
                    continue;
                } else {
                    if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
                    s_stream_ctx.download_complete = true;
                    if (s_stream_cond_inited) sysCondBroadcast(s_stream_cond);
                    if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
                    net_hal_log("AUDIO", "[STREAM COMPLETO] Fin de stream recibido (EOF, %llu bytes)",
                                (unsigned long long)s_stream_ctx.received);
                    break;
                }
            } else {
                net_hal_log("AUDIO", "[STREAM ERROR] net_hal_recv_some fallo (ret: %d, errno: %d)", n, net_err);
                close_stream_sock_net_only(sock);
                sock = -1;
                if (!s_engine_running || current_seq != s_track_sequence || s_stream_ctx.abort) {
                    break;
                }
                usleep(100000);
            }
        }

        if (sock >= 0) {
            close_stream_sock_net_only(sock);
            sock = -1;
        }
    }

    net_hal_log("AUDIO", "[NET WORKER] Hilo de descarga finalizado");
    sysThreadExit(0);
}

/* ── Progressive FLAC Decoder & Audio Pipeline Worker Thread (PPU prio 700) ── */
static void stream_decode_worker_func(void *arg)
{
    (void)arg;
    net_hal_log("AUDIO", "[DECODE WORKER] Hilo de decodificacion FLAC iniciado (PPU prio 700)");

    float in_chunk_44k[(CHUNK_IN_44K + 1) * 2];
    float out_chunk_48k[CHUNK_OUT_FRAMES * 2];
    uint32_t current_seq = 0;

    while (s_engine_running) {
        if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
        while (s_engine_running && current_seq == s_track_sequence) {
            if (s_stream_cond_inited) {
                sysCondWait(s_stream_cond, 250000);
            } else {
                sysMutexUnlock(s_stream_mutex);
                usleep(10000);
                sysMutexLock(s_stream_mutex, 0);
            }
        }
        if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);

        if (!s_engine_running) break;

        current_seq = s_track_sequence;

        /* Flush ring buffer (s_ring_primed is false so mono_hw is idle) */
        s_ring_read_pos = 0;
        s_ring_write_pos = 0;
        __asm__ volatile("lwsync" ::: "memory");
        s_resample_boundary[0] = 0.0f;
        s_resample_boundary[1] = 0.0f;
        s_total_pcm_frames = 0;
        s_current_pcm_frame = 0;
        s_played_pcm_frames = 0;

        /* Sincronizar con el worker de red para asegurar que inicializo el nuevo track */
        if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
        while (s_engine_running && current_seq == s_track_sequence && s_stream_ctx.seq != current_seq) {
            if (s_stream_cond_inited) {
                sysCondWait(s_stream_cond, 20000);
            } else {
                sysMutexUnlock(s_stream_mutex);
                usleep(2000);
                sysMutexLock(s_stream_mutex, 0);
            }
        }
        if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);

        if (current_seq != s_track_sequence || !s_engine_running) continue;

        /* Wait for prebuffer: 2 MB or download_complete */
        uint64_t wait_start = get_time_usec();
        if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
        while (s_engine_running && current_seq == s_track_sequence) {
            if (s_stream_ctx.download_complete || s_stream_ctx.received >= PREBUFFER_THRESHOLD) {
                break;
            }
            if (s_stream_ctx.abort || s_has_error) {
                break;
            }
            if (get_time_usec() - wait_start > 25 * 1000000ULL) {
                if (s_stream_ctx.received >= 256 * 1024 && memcmp(s_stream_ctx.data, "fLaC", 4) == 0) {
                    net_hal_log("AUDIO", "[STREAM PREBUFFER] 25s alcanzados, arrancando con %lu KB recibidos",
                                (unsigned long)(s_stream_ctx.received / 1024));
                    break;
                }
                net_hal_log("AUDIO", "[STREAM TIMEOUT] 25s esperando prebuffer");
                break;
            }
            if (s_stream_cond_inited) {
                sysCondWait(s_stream_cond, 20000);
            } else {
                sysMutexUnlock(s_stream_mutex);
                usleep(5000);
                sysMutexLock(s_stream_mutex, 0);
            }
        }
        if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);

        if (current_seq != s_track_sequence || !s_engine_running) continue;

        if (s_stream_ctx.abort || s_has_error) {
            net_hal_log("AUDIO", "[DECODE WORKER] Sesion abortada antes de iniciar decodificacion");
            s_is_playing = false;
            continue;
        }

        if (s_stream_ctx.received < 4 || memcmp(s_stream_ctx.data, "fLaC", 4) != 0) {
            net_hal_log("AUDIO", "[DECODE ERROR] Datos recibidos invalidos (magic no es fLaC)");
            if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
            snprintf(s_status_message, sizeof(s_status_message), "Formato FLAC invalido");
            s_last_error_code = 415;
            s_has_error = true;
            s_is_playing = false;
            s_stream_ctx.abort = true;
            if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
            continue;
        }

        s_stream_ctx.cursor = 0;
        drflac *pFlac = drflac_open_relaxed(flac_stream_read, flac_stream_seek, flac_stream_tell,
                                            drflac_container_native, &s_stream_ctx, NULL);
        if (!pFlac) {
            net_hal_log("AUDIO", "[DECODE ERROR] drflac_open_relaxed fallo para %s", s_stream_ctx.track_id);
            continue;
        }

        s_total_pcm_frames = pFlac->totalPCMFrameCount;
        uint32_t srate = pFlac->sampleRate;
        s_stream_sample_rate = srate ? srate : 44100;
        s_played_pcm_frames = 0;
        uint32_t chans = pFlac->channels;
        uint32_t bps   = pFlac->bitsPerSample;

        net_hal_log("AUDIO", "[STREAMINFO REAL] Metadatos FLAC decodificados:");
        net_hal_log("AUDIO", "  - Sample Rate: %u Hz | Canales: %u | Bits: %u-bit", srate, chans, bps);
        net_hal_log("AUDIO", "  - Total frames PCM: %llu (Duracion: %u:%02u)",
                    (unsigned long long)s_total_pcm_frames,
                    (uint32_t)(s_total_pcm_frames / (srate ? srate : 44100)) / 60,
                    (uint32_t)(s_total_pcm_frames / (srate ? srate : 44100)) % 60);

        if ((chans != 1 && chans != 2) ||
            (srate != 44100 && srate != 48000 && srate != 96000 && srate != 192000)) {
            net_hal_log("AUDIO", "[DECODE ERROR] Formato no soportado: %u Hz, %u ch", srate, chans);
            if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
            snprintf(s_status_message, sizeof(s_status_message), "Formato %uHz/%uch no soportado", srate, chans);
            s_last_error_code = 415;
            s_has_error = true;
            s_is_playing = false;
            s_stream_ctx.abort = true;
            if (s_stream_cond_inited) sysCondBroadcast(s_stream_cond);
            if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
            drflac_close(pFlac);
            continue;
        }

        uint64_t total_dec_time_usec = 0;
        uint64_t frames_dec_48k = 0;
        uint64_t last_stat_frames = 0;

        while (s_engine_running && current_seq == s_track_sequence && !s_stream_ctx.abort) {
            if (!s_is_playing) {
                usleep(10000);
                continue;
            }

            /* Wait until ring buffer has space */
            if (ring_space_available() < CHUNK_OUT_FRAMES) {
                usleep(4000);
                continue;
            }

            uint64_t t0 = get_time_usec();
            size_t decoded_out_frames = 0;

            if (srate == 44100) {
                in_chunk_44k[0] = s_resample_boundary[0];
                in_chunk_44k[1] = s_resample_boundary[1];

                drflac_uint64 read_frames = drflac_read_pcm_frames_f32(pFlac, CHUNK_IN_44K, in_chunk_44k + 2);
                if (read_frames == 0) {
                    if (s_stream_ctx.download_complete || s_stream_ctx.abort || s_has_error) {
                        net_hal_log("AUDIO", "[DECODE COMPLETO] Decodificacion FLAC completada (%llu frames). Drenando buffer...",
                                    (unsigned long long)s_current_pcm_frame);
                        break;
                    }
                    usleep(5000);
                    continue;
                }

                if (chans == 1) {
                    for (int idx = (int)read_frames - 1; idx >= 0; idx--) {
                        float mono = (in_chunk_44k + 2)[idx];
                        (in_chunk_44k + 2)[idx * 2 + 0] = mono;
                        (in_chunk_44k + 2)[idx * 2 + 1] = mono;
                    }
                }

                s_current_pcm_frame += read_frames;
                s_resample_boundary[0] = in_chunk_44k[read_frames * 2 + 0];
                s_resample_boundary[1] = in_chunk_44k[read_frames * 2 + 1];

                size_t out_frames = CHUNK_OUT_FRAMES;
                if (read_frames < CHUNK_IN_44K) {
                    out_frames = (size_t)((read_frames * 160 + 146) / 147);
                    if (out_frames > CHUNK_OUT_FRAMES) out_frames = CHUNK_OUT_FRAMES;
                }

                for (size_t k = 0; k < out_frames; k++) {
                    float in_pos = (float)k * (147.0f / 160.0f);
                    int idx = (int)in_pos;
                    float frac = in_pos - (float)idx;
                    if (idx + 1 >= (int)read_frames + 1) idx = (int)read_frames - 1;
                    if (idx < 0) idx = 0;

                    out_chunk_48k[k * 2 + 0] = (1.0f - frac) * in_chunk_44k[idx * 2 + 0] + frac * in_chunk_44k[(idx + 1) * 2 + 0];
                    out_chunk_48k[k * 2 + 1] = (1.0f - frac) * in_chunk_44k[idx * 2 + 1] + frac * in_chunk_44k[(idx + 1) * 2 + 1];
                }

                ring_write_frames(out_chunk_48k, out_frames);
                decoded_out_frames = out_frames;
            } else if (srate == 48000) {
                drflac_uint64 read_frames = drflac_read_pcm_frames_f32(pFlac, CHUNK_OUT_FRAMES, out_chunk_48k);
                if (read_frames == 0) {
                    if (s_stream_ctx.download_complete || s_stream_ctx.abort || s_has_error) {
                        net_hal_log("AUDIO", "[DECODE COMPLETO] Decodificacion FLAC completada (%llu frames). Drenando buffer...",
                                    (unsigned long long)s_current_pcm_frame);
                        break;
                    }
                    usleep(5000);
                    continue;
                }
                if (chans == 1) {
                    for (int idx = (int)read_frames - 1; idx >= 0; idx--) {
                        float mono = out_chunk_48k[idx];
                        out_chunk_48k[idx * 2 + 0] = mono;
                        out_chunk_48k[idx * 2 + 1] = mono;
                    }
                }
                s_current_pcm_frame += read_frames;
                ring_write_frames(out_chunk_48k, (size_t)read_frames);
                decoded_out_frames = (size_t)read_frames;
            } else if (srate == 96000) {
                float in_chunk_96k[CHUNK_OUT_FRAMES * 2 * 2];
                drflac_uint64 read_frames = drflac_read_pcm_frames_f32(pFlac, CHUNK_OUT_FRAMES * 2, in_chunk_96k);
                if (read_frames == 0) {
                    if (s_stream_ctx.download_complete || s_stream_ctx.abort || s_has_error) {
                        net_hal_log("AUDIO", "[DECODE COMPLETO] Decodificacion FLAC completada (%llu frames). Drenando buffer...",
                                    (unsigned long long)s_current_pcm_frame);
                        break;
                    }
                    usleep(5000);
                    continue;
                }
                if (chans == 1) {
                    for (int idx = (int)read_frames - 1; idx >= 0; idx--) {
                        float mono = in_chunk_96k[idx];
                        in_chunk_96k[idx * 2 + 0] = mono;
                        in_chunk_96k[idx * 2 + 1] = mono;
                    }
                }
                s_current_pcm_frame += read_frames;
                size_t out_frames = (size_t)(read_frames / 2);
                for (size_t k = 0; k < out_frames; k++) {
                    out_chunk_48k[k * 2 + 0] = 0.5f * (in_chunk_96k[(k * 2) * 2 + 0] + in_chunk_96k[(k * 2 + 1) * 2 + 0]);
                    out_chunk_48k[k * 2 + 1] = 0.5f * (in_chunk_96k[(k * 2) * 2 + 1] + in_chunk_96k[(k * 2 + 1) * 2 + 1]);
                }
                if (out_frames > 0) {
                    ring_write_frames(out_chunk_48k, out_frames);
                }
                decoded_out_frames = out_frames;
            } else if (srate == 192000) {
                /* 4:1 box-filter downsample in 4 sub-blocks of 320 out frames to keep stack < 12 KB */
                float in_sub_192k[320 * 4 * 2];
                size_t total_out = 0;
                for (int sub = 0; sub < 4; sub++) {
                    drflac_uint64 read_frames = drflac_read_pcm_frames_f32(pFlac, 320 * 4, in_sub_192k);
                    if (read_frames == 0) break;
                    if (chans == 1) {
                        for (int idx = (int)read_frames - 1; idx >= 0; idx--) {
                            float mono = in_sub_192k[idx];
                            in_sub_192k[idx * 2 + 0] = mono;
                            in_sub_192k[idx * 2 + 1] = mono;
                        }
                    }
                    s_current_pcm_frame += read_frames;
                    size_t sub_out = (size_t)(read_frames / 4);
                    for (size_t k = 0; k < sub_out; k++) {
                        size_t base = k * 4;
                        out_chunk_48k[(total_out + k) * 2 + 0] = 0.25f * (
                            in_sub_192k[(base + 0) * 2 + 0] + in_sub_192k[(base + 1) * 2 + 0] +
                            in_sub_192k[(base + 2) * 2 + 0] + in_sub_192k[(base + 3) * 2 + 0]);
                        out_chunk_48k[(total_out + k) * 2 + 1] = 0.25f * (
                            in_sub_192k[(base + 0) * 2 + 1] + in_sub_192k[(base + 1) * 2 + 1] +
                            in_sub_192k[(base + 2) * 2 + 1] + in_sub_192k[(base + 3) * 2 + 1]);
                    }
                    total_out += sub_out;
                    if (read_frames < 320 * 4) break;
                }
                if (total_out == 0) {
                    if (s_stream_ctx.download_complete || s_stream_ctx.abort || s_has_error) {
                        net_hal_log("AUDIO", "[DECODE COMPLETO] Decodificacion FLAC completada (%llu frames). Drenando buffer...",
                                    (unsigned long long)s_current_pcm_frame);
                        break;
                    }
                    usleep(5000);
                    continue;
                }
                ring_write_frames(out_chunk_48k, total_out);
                decoded_out_frames = total_out;
            } else {
                net_hal_log("AUDIO", "[DECODE ERROR] Sample rate no soportado en loop: %u Hz", srate);
                break;
            }

            /* Ring priming */
            if (!s_ring_primed) {
                if (s_stream_ctx.download_complete && s_current_pcm_frame >= s_total_pcm_frames) {
                    s_ring_primed = true;
                } else if (ring_frames_available() >= RING_PREFILL_FRAMES) {
                    s_ring_primed = true;
                }
            }

            uint64_t t1 = get_time_usec();
            total_dec_time_usec += (t1 - t0);
            frames_dec_48k += decoded_out_frames;

            if (frames_dec_48k >= last_stat_frames + 480000ULL) {
                last_stat_frames = frames_dec_48k;
                double audio_sec = (double)frames_dec_48k / (double)SAMPLE_RATE;
                double cpu_sec = (double)total_dec_time_usec / 1000000.0;
                double ratio = (audio_sec > 0.0) ? (cpu_sec / audio_sec) : 0.0;
                net_hal_log("AUDIO", "[AUDIO STATS] Pista: '%s' | Frames decodificados: %llu (%.1f s)",
                            s_stream_ctx.title, (unsigned long long)frames_dec_48k, audio_sec);
                net_hal_log("AUDIO", "  - Tiempo decodificacion CPU: %.3f s | Carga PPU: %.2f%%", cpu_sec, ratio * 100.0);
                net_hal_log("AUDIO", "  - Ring Buffer underruns: %u  |  Hardware DMA underruns: %u",
                            s_ring_underruns, s_hw_underruns);
            }
        }

        /* Esperar a que el consumidor de audio DMA vacie el ring buffer para reproducir hasta el final */
        while (s_engine_running && current_seq == s_track_sequence && !s_stream_ctx.abort && ring_frames_available() > 0) {
            usleep(10000);
        }

        if (s_engine_running && current_seq == s_track_sequence && !s_stream_ctx.abort) {
            s_is_playing = false;
            s_ring_primed = false;
            net_hal_log("AUDIO", "[TRACK FINISHED] Cancion '%s' reproducida completamente (%llu frames).",
                        s_stream_ctx.title, (unsigned long long)s_current_pcm_frame);
        }

        drflac_close(pFlac);
        net_hal_log("AUDIO", "[STREAM FIN] Fin de sesion de reproduccion para '%s'", s_stream_ctx.title);
    }

    net_hal_log("AUDIO", "[DECODE WORKER] Hilo de decodificacion finalizado");
    sysThreadExit(0);
}

/* ── Public API Implementation ── */

int audio_engine_init(void)
{
    net_hal_log("AUDIO", "=== MONOCHROME LOSSLESS FLAC STREAMING ENGINE: INICIO ===");

    sys_mutex_attr_t mattr;
    sysMutexAttrInitialize(mattr);
    mattr.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
    mattr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    if (sysMutexCreate(&s_stream_mutex, &mattr) == 0) {
        s_stream_mutex_inited = true;
    }

    if (s_stream_mutex_inited) {
        sys_cond_attr_t cattr;
        sysCondAttrInitialize(cattr);
        if (sysCondCreate(&s_stream_cond, s_stream_mutex, &cattr) == 0) {
            s_stream_cond_inited = true;
        }
    }

    memset(&s_stream_ctx, 0, sizeof(s_stream_ctx));
    s_stream_ctx.sock = -1;

    /* Allocate 24 MB XDR media streaming buffer */
    s_stream_media_buffer = (uint8_t *)memalign(1024 * 1024, STREAM_BUFFER_CAPACITY);
    if (!s_stream_media_buffer) {
        net_hal_log("AUDIO", "[ERROR] memalign fallo para s_stream_media_buffer (24 MB)");
        return -1;
    }
    net_hal_log("AUDIO", "[MEM] s_stream_media_buffer asignado @%p (tamano: 24 MB)", s_stream_media_buffer);

    int ret = audioInit();
    net_hal_log("AUDIO", "[AUDIO] audioInit() -> %d", ret);
    if (ret != 0) return -1;
    s_audio_inited = true;

    audioPortParam params;
    memset(&params, 0, sizeof(params));
    params.numChannels = AUDIO_PORT_2CH;
    params.numBlocks = AUDIO_BLOCK_8;
    params.attrib = AUDIO_PORT_INITLEVEL;
    params.level = 1.0f;

    ret = audioPortOpen(&params, &s_port_num);
    net_hal_log("AUDIO", "[AUDIO] audioPortOpen -> %d, portNum: %u", ret, s_port_num);
    if (ret != 0) return -1;
    s_audio_port_opened = true;

    ret = audioGetPortConfig(s_port_num, &s_config);
    net_hal_log("AUDIO", "[AUDIO] audioGetPortConfig -> %d (portSize: %d B)", ret, s_config.portSize);

    ret = audioCreateNotifyEventQueue(&s_snd_queue, &s_snd_key);
    net_hal_log("AUDIO", "[AUDIO] audioCreateNotifyEventQueue -> %d", ret);

    ret = audioSetNotifyEventQueue(s_snd_key);
    ret = sysEventQueueDrain(s_snd_queue);

    ret = audioPortStart(s_port_num);
    net_hal_log("AUDIO", "[AUDIO] audioPortStart -> %d", ret);
    if (ret == 0) s_audio_port_started = true;

    s_engine_running = true;

    /* 1. Launch Hardware Consumer Thread (prio 100, real-time audio DMA, 64KB stack, STRICTLY LOCK-FREE) */
    ret = sysThreadCreate(&s_hw_thread_id, audio_hw_thread_func, NULL, 100,
                          0x10000, THREAD_JOINABLE, "mono_hw");
    if (ret == 0) s_hw_thread_created = true;
    net_hal_log("AUDIO", "[THREAD] sysThreadCreate(mono_hw) -> %d (prio 100)", ret);

    /* 2. Launch Progressive Stream & Decoder Thread (prio 700, 128KB stack) */
    ret = sysThreadCreate(&s_stream_thread_id, stream_decode_worker_func, NULL, 700,
                          0x20000, THREAD_JOINABLE, "mono_dec");
    if (ret == 0) s_stream_thread_created = true;
    net_hal_log("AUDIO", "[THREAD] sysThreadCreate(mono_dec) -> %d (prio 700)", ret);

    /* 3. Launch HTTP Streaming Downloader Thread (prio 750, 128KB stack) */
    ret = sysThreadCreate(&s_download_thread_id, stream_download_worker_func, NULL, 750,
                          0x20000, THREAD_JOINABLE, "mono_net");
    if (ret == 0) s_download_thread_created = true;
    net_hal_log("AUDIO", "[THREAD] sysThreadCreate(mono_net) -> %d (prio 750)", ret);

    return 0;
}

void audio_engine_shutdown(void)
{
    net_hal_log("AUDIO", "[CLEANUP] Cerrando subsistema de audio y streaming...");

    s_engine_running = false;
    s_is_playing = false;
    s_ring_primed = false;

    if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
    s_stream_ctx.abort = true;
    if (s_stream_ctx.sock >= 0) {
        shutdown(s_stream_ctx.sock, SHUT_RDWR);
    }
    if (s_stream_cond_inited) sysCondBroadcast(s_stream_cond);
    if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);

    /* Detener puerto DMA para silenciar salida antes de unir hilos */
    if (s_audio_port_started) {
        audioPortStop(s_port_num);
        s_audio_port_started = false;
        net_hal_log("AUDIO", "[AUDIO] audioPortStop(%u) ejecutado", s_port_num);
    }

    /* Join de los 3 hilos ANTES de destruir colas de eventos, liberar buffers o llamar audioQuit() */
    uint64_t exit_val = 0;
    if (s_download_thread_created) {
        sysThreadJoin(s_download_thread_id, &exit_val);
        s_download_thread_created = false;
        net_hal_log("AUDIO", "[THREAD] mono_net joined");
    }
    if (s_stream_thread_created) {
        sysThreadJoin(s_stream_thread_id, &exit_val);
        s_stream_thread_created = false;
        net_hal_log("AUDIO", "[THREAD] mono_dec joined");
    }
    if (s_hw_thread_created) {
        sysThreadJoin(s_hw_thread_id, &exit_val);
        s_hw_thread_created = false;
        net_hal_log("AUDIO", "[THREAD] mono_hw joined");
    }

    if (s_snd_key) {
        audioRemoveNotifyEventQueue(s_snd_key);
        sysEventQueueDestroy(s_snd_queue, 0);
        s_snd_key = 0;
    }

    if (s_audio_port_opened) {
        audioPortClose(s_port_num);
        s_audio_port_opened = false;
        net_hal_log("AUDIO", "[AUDIO] audioPortClose(%u) ejecutado", s_port_num);
    }

    if (s_audio_inited) {
        audioQuit();
        s_audio_inited = false;
        net_hal_log("AUDIO", "[AUDIO] audioQuit() ejecutado");
    }

    if (s_stream_media_buffer) {
        free(s_stream_media_buffer);
        s_stream_media_buffer = NULL;
        s_stream_ctx.data = NULL;
        net_hal_log("AUDIO", "[MEM] s_stream_media_buffer (24 MB) liberado");
    }

    if (s_stream_cond_inited) {
        sysCondDestroy(s_stream_cond);
        s_stream_cond_inited = false;
    }
    if (s_stream_mutex_inited) {
        sysMutexDestroy(s_stream_mutex);
        s_stream_mutex_inited = false;
    }

    net_hal_log("AUDIO", "[AUDIO RESUMEN FINAL]");
    net_hal_log("AUDIO", "  - Underruns del Ring Buffer: %u", s_ring_underruns);
    net_hal_log("AUDIO", "  - Underruns de Hardware DMA: %u", s_hw_underruns);
    net_hal_log("AUDIO", "[AUDIO] Subsistema cerrado limpiamente");
}

void audio_engine_play_track(const char *track_id, const char *title)
{
    if (!track_id || !net_hal_validate_id(track_id)) {
        net_hal_log("AUDIO", "[AUDIO ERROR] track_id invalido rechazado");
        if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);
        s_has_error = true;
        s_last_error_code = 400;
        snprintf(s_status_message, sizeof(s_status_message), "ID de pista invalido");
        if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
        return;
    }

    if (s_stream_mutex_inited) sysMutexLock(s_stream_mutex, 0);

    /* Abortar conexion previa de forma segura: shutdown() aqui, close() solo en mono_net */
    s_stream_ctx.abort = true;
    if (s_stream_ctx.sock >= 0) {
        shutdown(s_stream_ctx.sock, SHUT_RDWR);
    }

    s_has_error = false;
    s_last_error_code = 0;
    snprintf(s_status_message, sizeof(s_status_message), "Conectando...");

    memset(s_pending_track_id, 0, sizeof(s_pending_track_id));
    strncpy(s_pending_track_id, track_id, sizeof(s_pending_track_id) - 1);
    memset(s_pending_track_title, 0, sizeof(s_pending_track_title));
    strncpy(s_pending_track_title, title ? title : "", sizeof(s_pending_track_title) - 1);

    /* Nuevo track: desactivar priming e incrementar sequence; mono_net reinicia StreamContext */
    s_ring_primed = false;
    s_ring_underruns = 0;
    s_played_pcm_frames = 0;
    __asm__ volatile("lwsync" ::: "memory");

    s_is_playing = true;
    s_track_sequence++;
    if (s_stream_cond_inited) sysCondBroadcast(s_stream_cond);

    if (s_stream_mutex_inited) sysMutexUnlock(s_stream_mutex);
}

void audio_engine_play(int track_index)
{
    (void)track_index;
    s_is_playing = true;
}

void audio_engine_pause(void)
{
    s_is_playing = false;
    net_hal_log("AUDIO", "[AUDIO] Reproduccion pausada");
}

void audio_engine_resume(void)
{
    s_is_playing = true;
    net_hal_log("AUDIO", "[AUDIO] Reproduccion reanudada");
}

bool audio_engine_is_playing(void)
{
    return s_is_playing;
}

void audio_engine_set_volume(float vol)
{
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    s_volume = vol;
    net_hal_log("AUDIO", "[AUDIO] Volumen: %d%%", (int)(s_volume * 100.0f));
}

float audio_engine_get_volume(void)
{
    return s_volume;
}

float audio_engine_get_progress(void)
{
    if (s_total_pcm_frames > 0) {
        float p = (float)((double)s_played_pcm_frames / (double)s_total_pcm_frames);
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        return p;
    }
    return 0.0f;
}

void audio_engine_seek(float progress_0_to_1)
{
    (void)progress_0_to_1;
}

void audio_engine_get_vis(float *bands, int num_bands)
{
    int count = (num_bands < VIS_BANDS) ? num_bands : VIS_BANDS;
    for (int i = 0; i < count; i++) {
        bands[i] = s_vis_bands[i];
    }
}

uint32_t audio_engine_get_underruns(void)
{
    return s_ring_underruns;
}
