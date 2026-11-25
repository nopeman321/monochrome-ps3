/*
 * Monochrome PS3 - Real-Time Lossless FLAC Streaming Audio Engine (PSL1GHT)
 * Stage 6B: Fully Instrumentated Recv / Streaming Pipeline
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
#include <errno.h>
#include <fcntl.h>

#include <audio/audio.h>
#include <sys/thread.h>
#include <sys/mutex.h>
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

#define STAGE_LOG_PATH "/dev_hdd0/tmp/mono_stage6.log"
#define SAMPLE_RATE 48000
#define NUM_CHANNELS 2

#define API_HOST "tracks.monochrome.st"
#define API_PORT 80

#define STREAM_BUFFER_CAPACITY (48 * 1024 * 1024) /* 48 MB XDR buffer */
#define PREBUFFER_THRESHOLD    (2 * 1024 * 1024)  /* 2 MB (~10-12s de audio FLAC) prellenado para streaming sin cortes */

#define RING_BUFFER_FRAMES 576000                 /* 12.0s at 48kHz stereo float = 4.5 MB (increased from 8s) */
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
    volatile int sock;
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

/* ── Synchronization Mutex ── */
static sys_mutex_t s_log_mutex;
static bool s_log_mutex_inited = false;

static void stage6_log(const char *fmt, ...)
{
    if (s_log_mutex_inited) {
        sysMutexLock(s_log_mutex, 0);
    }

    FILE *f = fopen(STAGE_LOG_PATH, "a");
    if (f) {
        time_t t = time(NULL);
        fprintf(f, "[%ld] ", (long)t);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fprintf(f, "\n");
        fflush(f);
        fclose(f);
    }

    if (s_log_mutex_inited) {
        sysMutexUnlock(s_log_mutex);
    }
}

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

/* Live status message for UI */
static char s_status_message[128] = "Listo";
static volatile bool s_has_error = false;
static volatile int  s_last_error_code = 0;

const char *audio_engine_get_status_message(void)
{
    return s_status_message;
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
    s_has_error = false;
    s_last_error_code = 0;
}

/* ── Lock-Free Ring Buffer ── */
static float s_ring_buffer[RING_BUFFER_FRAMES * 2];
static volatile size_t s_ring_write_pos = 0;
static volatile size_t s_ring_read_pos = 0;

static inline size_t ring_frames_available(void)
{
    size_t w = s_ring_write_pos;
    size_t r = s_ring_read_pos;
    if (w >= r) return w - r;
    return RING_BUFFER_FRAMES - (r - w);
}

static inline size_t ring_space_available(void)
{
    size_t w = s_ring_write_pos;
    size_t r = s_ring_read_pos;
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
}

/* ── dr_flac Progressive In-Memory Ring Stream Callbacks ── */
static size_t flac_stream_read(void *pUserData, void *pBufferOut, size_t bytesToRead)
{
    StreamContext *ctx = (StreamContext *)pUserData;
    int wait_ms = 0;
    while (ctx->cursor >= ctx->received) {
        if (ctx->download_complete || ctx->abort || !s_engine_running || ctx->seq != s_track_sequence) return 0;
        usleep(5000);
        wait_ms += 5;
        if (wait_ms > 25000) {
            stage6_log("[STREAM TIMEOUT] 25s sin nuevos datos recibidos en flac_stream_read");
            return 0;
        }
    }

    uint64_t avail = ctx->received - ctx->cursor;
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

    /* Si los datos solicitados ya fueron sobrescritos en el anillo (>48 MB atrás), rechazar seek */
    if (ctx->received > ctx->capacity && new_pos < (ctx->received - ctx->capacity)) {
        return DRFLAC_FALSE;
    }

    int wait_ms = 0;
    while (new_pos > ctx->received) {
        if (ctx->download_complete || ctx->abort || !s_engine_running || ctx->seq != s_track_sequence) return DRFLAC_FALSE;
        usleep(5000);
        wait_ms += 5;
        if (wait_ms > 25000) return DRFLAC_FALSE;
    }

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

/* ── Hardware Audio Consumer Thread (Priority 1500) ── */
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

        /* El driver consume un bloque por evento; llevamos nuestro propio indice
           para no leer erróneamente readIndex como puntero */
        u32 nblocks = (u32)s_config.numBlocks;
        if (nblocks == 0 || nblocks > 32) nblocks = 8;
        s_write_block = (s_write_block + 1) % nblocks;
        u32 write_block = s_write_block;
        float *dest = (float *)((u64)s_config.audioDataStart) + (NUM_CHANNELS * AUDIO_BLOCK_SAMPLES * write_block);

        /* Solo consumimos del ring buffer (y contamos underruns) cuando estamos
           reproduciendo de verdad y el ring buffer ya esta prellenado. En cualquier
           otro estado (IDLE, PAUSED, BUFFERING) escribimos silencio sin contabilizar
           underruns ni emitir logs, para evitar ráfagas falsas al arrancar. */
        if (!s_is_playing || !s_ring_primed) {
            memset(dest, 0, AUDIO_BLOCK_SAMPLES * NUM_CHANNELS * sizeof(float));
            for (int b = 0; b < VIS_BANDS; b++) s_vis_bands[b] *= 0.85f;
            continue;
        }

        size_t avail = ring_frames_available();
        if (avail < AUDIO_BLOCK_SAMPLES) {
            /* Underrun real durante reproduccion activa: activar histeresis de
               rebuffering para que el productor vuelva a juntar audio antes de
               reanudar, evitando ráfagas de miles de underruns consecutivos. */
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

/* ── HTTP Connection Helper (Supports Initial GET & HTTP Range Resumption) ── */
static int stream_connect_http(const char *track_id, uint32_t seq, uint64_t offset, uint64_t *out_content_length, char *out_content_type, size_t ct_size)
{
    if (out_content_length) *out_content_length = 0;
    if (out_content_type && ct_size > 0) out_content_type[0] = '\0';

    int max_retries = http_get_max_retries();
    if (max_retries < 1) max_retries = 3;

    for (int attempt = 0; attempt < max_retries && s_engine_running && seq == s_track_sequence; attempt++) {
        if (attempt > 0) {
            int delay_sec = 2 * (1 << (attempt - 1));
            if (delay_sec > 15) delay_sec = 15;
            stage6_log("[STREAM RETRY] Intento %d/%d (espera %ds)...", attempt + 1, max_retries, delay_sec);
            for (int w = 0; w < delay_sec * 10 && s_engine_running && seq == s_track_sequence; w++) {
                usleep(100000);
            }
            if (seq != s_track_sequence || !s_engine_running) return -1;
        }

        struct hostent *he = gethostbyname(API_HOST);
        if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
            stage6_log("[ERROR] DNS lookup fallo para %s (errno: %d)", API_HOST, errno);
            continue;
        }

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            stage6_log("[ERROR] socket(AF_INET) fallo (errno: %d)", errno);
            continue;
        }

        int rcvbuf = 128 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        struct timeval tv;
        tv.tv_sec = 6;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_in saddr;
        memset(&saddr, 0, sizeof(saddr));
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(API_PORT);
        memcpy(&saddr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));

        if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) != 0) {
            stage6_log("[ERROR] connect a %s:80 fallo (errno: %d)", API_HOST, errno);
            close(sock);
            continue;
        }

        char req[1024];
        if (offset > 0) {
            snprintf(req, sizeof(req),
                     "GET /track/%s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Range: bytes=%llu-\r\n"
                     "User-Agent: Mozilla/5.0 (PlayStation 3) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
                     "Accept: application/octet-stream, audio/flac, */*\r\n"
                     "Referer: https://monochrome.st/\r\n"
                     "Origin: https://monochrome.st\r\n"
                     "Connection: keep-alive\r\n\r\n",
                     track_id, API_HOST, (unsigned long long)offset);
        } else {
            snprintf(req, sizeof(req),
                     "GET /track/%s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: Mozilla/5.0 (PlayStation 3) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
                     "Accept: application/octet-stream, audio/flac, */*\r\n"
                     "Referer: https://monochrome.st/\r\n"
                     "Origin: https://monochrome.st\r\n"
                     "Connection: keep-alive\r\n\r\n",
                     track_id, API_HOST);
        }

        ssize_t s_ret = send(sock, req, strlen(req), 0);
        if (s_ret <= 0) {
            stage6_log("[ERROR] send HTTP fallo (ret: %d, errno: %d)", (int)s_ret, errno);
            close(sock);
            continue;
        }

        /* Read headers */
        char head_buf[4096];
        size_t head_len = 0;
        char *body_start = NULL;

        while (head_len < sizeof(head_buf) - 1) {
            ssize_t n = recv(sock, head_buf + head_len, sizeof(head_buf) - head_len - 1, 0);
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
            stage6_log("[ERROR] Cabecera HTTP incompleta");
            close(sock);
            continue;
        }

        int http_code = 0;
        sscanf(head_buf, "HTTP/1.1 %d", &http_code);
        if (http_code == 0) sscanf(head_buf, "HTTP/1.0 %d", &http_code);

        if (http_code == 429) {
            stage6_log("[HTTP 429] Rate limited, reintentando...");
            close(sock);
            continue;
        }

        if (http_code == 404 || http_code == 403 || http_code == 410) {
            stage6_log("[STREAM HTTP] Error permanente HTTP %d para pista %s (no reintentable)", http_code, track_id);
            s_last_error_code = http_code;
            close(sock);
            return -http_code;
        }

        if (http_code != 200 && http_code != 206) {
            stage6_log("[ERROR] HTTP codigo %d no esperado", http_code);
            close(sock);
            continue;
        }

        if (offset > 0 && http_code == 200) {
            stage6_log("[STREAM RESUME] Servidor ignoro Range (HTTP 200 en offset %llu), reintentando...",
                       (unsigned long long)offset);
            close(sock);
            continue;
        }

        /* Parse Content-Length */
        uint64_t clen = 0;
        char *cl_ptr = strstr(head_buf, "Content-Length:");
        if (!cl_ptr) cl_ptr = strstr(head_buf, "content-length:");
        if (cl_ptr) {
            unsigned long long cl_tmp = 0;
            sscanf(cl_ptr + 15, " %llu", &cl_tmp);
            clen = (uint64_t)cl_tmp;
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

        stage6_log("[STREAM HTTP] Conectado: Codigo %d | Content-Length: %llu bytes", http_code, (unsigned long long)clen);

        /* Copy any excess body bytes into sliding ring buffer */
        size_t header_consumed = (size_t)(body_start - head_buf);
        size_t initial_body_bytes = head_len - header_consumed;
        if (initial_body_bytes > 0) {
            while (s_engine_running && seq == s_track_sequence) {
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

/* ── Dedicated HTTP Streaming Download Worker Thread (PPU prio 1000) ── */
static void stream_download_worker_func(void *arg)
{
    (void)arg;
    stage6_log("[NET WORKER] Hilo de descarga iniciado (PPU prio 1000)");

    uint32_t current_seq = 0;

    while (s_engine_running) {
        if (current_seq == s_track_sequence) {
            usleep(10000);
            continue;
        }

        current_seq = s_track_sequence;

        /* Reset stream context */
        memset(&s_stream_ctx, 0, sizeof(StreamContext));
        s_stream_ctx.sock = -1;
        s_stream_ctx.data = s_stream_media_buffer;
        s_stream_ctx.capacity = STREAM_BUFFER_CAPACITY;
        strncpy(s_stream_ctx.track_id, s_pending_track_id, sizeof(s_stream_ctx.track_id) - 1);
        strncpy(s_stream_ctx.title, s_pending_track_title, sizeof(s_stream_ctx.title) - 1);

        stage6_log("==================================================");
        stage6_log("[PISTA ELEGIDA] '%s' [ID: %s]", s_stream_ctx.title, s_stream_ctx.track_id);

        /* 1. Check local disk cache first */
        char c_path[256];
        snprintf(c_path, sizeof(c_path), "/dev_hdd0/tmp/monochrome/cache/%s.flac", s_stream_ctx.track_id);
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
                    s_stream_ctx.received = read_bytes;
                    s_stream_ctx.download_complete = true;
                    s_stream_ctx.is_cached = true;
                    s_stream_ctx.seq = current_seq;
                    __asm__ volatile("lwsync" ::: "memory");
                    stage6_log("[CACHE HIT] Pista cargada desde disco: %s (%ld KB) - Reproduccion inmediata",
                               s_stream_ctx.cache_path, fsize / 1024);
                    snprintf(s_status_message, sizeof(s_status_message), "Caché local (%ld KB)", fsize / 1024);
                    cache_loaded = true;
                }
            }
            fclose(f_cache);
            if (cache_loaded) continue; /* Done loading cached track */
        }

        /* 2. Connect to HTTP API and download */
        uint64_t content_length = 0;
        char content_type[64] = "desconocido";
        s_stream_ctx.seq = current_seq;
        __asm__ volatile("lwsync" ::: "memory");

        int sock = stream_connect_http(s_stream_ctx.track_id, current_seq, 0, &content_length, content_type, sizeof(content_type));
        if (sock < 0) {
            if (current_seq == s_track_sequence) {
                stage6_log("[STREAM ERROR] No se pudo conectar a la API para la pista %s (ret: %d)", s_stream_ctx.track_id, sock);
                if (sock == -404) {
                    snprintf(s_status_message, sizeof(s_status_message), "Pista no disponible (404)");
                    s_last_error_code = 404;
                } else if (sock == -403) {
                    snprintf(s_status_message, sizeof(s_status_message), "Acceso restringido (403)");
                    s_last_error_code = 403;
                } else {
                    snprintf(s_status_message, sizeof(s_status_message), "Error de conexion");
                    s_last_error_code = 500;
                }
                s_has_error = true;
                s_is_playing = false;
                s_stream_ctx.abort = true;
                __asm__ volatile("lwsync" ::: "memory");
            }
            continue;
        }

        s_stream_ctx.sock = sock;
        uint64_t t_down_start = get_time_usec();
        uint64_t last_log_time = t_down_start;
        uint64_t last_logged_bytes = s_stream_ctx.received;

        snprintf(s_status_message, sizeof(s_status_message), "Descargando stream...");

        while (s_engine_running && current_seq == s_track_sequence && !s_stream_ctx.download_complete) {
            /* Esperar si la ventana deslizante de 48 MB está llena por delante del decodificador */
            while (s_engine_running && current_seq == s_track_sequence && !s_stream_ctx.download_complete) {
                uint64_t cur = s_stream_ctx.cursor;
                uint64_t rec = s_stream_ctx.received;
                uint64_t ahead = (rec >= cur) ? (rec - cur) : 0;
                if (ahead < (uint64_t)(s_stream_ctx.capacity - 131072u)) {
                    break;
                }
                usleep(20000);
            }
            if (!s_engine_running || current_seq != s_track_sequence || s_stream_ctx.download_complete) {
                break;
            }

            if (sock < 0) {
                /* Try Range resume if disconnected prematurely */
                if (content_length > 0 && s_stream_ctx.received < content_length) {
                    stage6_log("[STREAM RESUME] Reconectando via Range (recibidos: %llu / %llu bytes)...",
                               (unsigned long long)s_stream_ctx.received, (unsigned long long)content_length);
                    sock = stream_connect_http(s_stream_ctx.track_id, current_seq, s_stream_ctx.received, NULL, NULL, 0);
                    if (sock < 0) {
                        usleep(1000000); /* 1s backoff */
                        continue;
                    }
                    s_stream_ctx.sock = sock;
                } else {
                    break;
                }
            }

            uint64_t cur = s_stream_ctx.cursor;
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

            ssize_t n = recv(sock, s_stream_ctx.data + off, to_read, 0);
            if (n > 0) {
                __asm__ volatile("lwsync" ::: "memory");
                s_stream_ctx.received += (size_t)n;

                uint64_t now = get_time_usec();
                double dt_s = (double)(now - last_log_time) / 1000000.0;
                if (dt_s >= 1.0 || s_stream_ctx.received >= last_logged_bytes + 524288ULL) {
                    double kb = (double)(s_stream_ctx.received - last_logged_bytes) / 1024.0;
                    double speed_kb_s = (dt_s > 0.0) ? (kb / dt_s) : 0.0;
                    stage6_log("[STREAM RECV] Descarga: %llu / %llu bytes (%.1f%%) | %.1f KB/s",
                               (unsigned long long)s_stream_ctx.received,
                               (unsigned long long)(content_length ? content_length : s_stream_ctx.received),
                               content_length ? ((double)s_stream_ctx.received * 100.0 / (double)content_length) : 100.0,
                               speed_kb_s);
                    last_logged_bytes = s_stream_ctx.received;
                    last_log_time = now;
                }

                if (content_length > 0 && s_stream_ctx.received >= content_length) {
                    s_stream_ctx.download_complete = true;
                    close(sock);
                    sock = -1;
                    s_stream_ctx.sock = -1;
                    uint64_t t_down_end = get_time_usec();
                    double down_s = (double)(t_down_end - t_down_start) / 1000000.0;
                    double mb = (double)s_stream_ctx.received / (1024.0 * 1024.0);
                    double speed_mb_s = (down_s > 0.0) ? (mb / down_s) : 0.0;
                    stage6_log("[STREAM COMPLETO] Descargados %.2f MB en %.2f s (Velocidad: %.2f MB/s)",
                               mb, down_s, speed_mb_s);

                    /* Save to disk cache only if the full file fits unwrapped inside the 48 MB buffer */
                    if (s_stream_ctx.received <= s_stream_ctx.capacity) {
                        FILE *fc = fopen(s_stream_ctx.cache_path, "wb");
                        if (fc) {
                            fwrite(s_stream_ctx.data, 1, (size_t)s_stream_ctx.received, fc);
                            fclose(fc);
                            stage6_log("[CACHE GUARDADO] %llu bytes en %s",
                                       (unsigned long long)s_stream_ctx.received, s_stream_ctx.cache_path);
                        }
                    } else {
                        stage6_log("[STREAM HI-RES] Pista >48MB (%llu bytes) reproducida via buffer circular",
                                   (unsigned long long)s_stream_ctx.received);
                    }
                    break;
                }
            } else if (n == 0) {
                close(sock);
                sock = -1;
                s_stream_ctx.sock = -1;
                if (content_length > 0 && s_stream_ctx.received < content_length) {
                    stage6_log("[STREAM DISCONNECT] Servidor cerro conexion a los %llu / %llu bytes. Reanudando...",
                               (unsigned long long)s_stream_ctx.received, (unsigned long long)content_length);
                    continue;
                } else {
                    s_stream_ctx.download_complete = true;
                    stage6_log("[STREAM COMPLETO] Fin de stream recibido (EOF, %llu bytes)",
                               (unsigned long long)s_stream_ctx.received);
                    break;
                }
            } else {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    stage6_log("[STREAM TIMEOUT] Socket recv timeout (6s sin datos)");
                } else {
                    stage6_log("[STREAM ERROR] recv error (ret: %d, errno: %d)", (int)n, errno);
                }
                close(sock);
                sock = -1;
                s_stream_ctx.sock = -1;
                usleep(100000);
            }
        }

        if (sock >= 0) {
            close(sock);
            sock = -1;
            s_stream_ctx.sock = -1;
        }
    }

    stage6_log("[NET WORKER] Hilo de descarga finalizado");
    sysThreadExit(0);
}

/* ── Progressive FLAC Decoder & Audio Pipeline Worker Thread (PPU prio 500) ── */
static void stream_decode_worker_func(void *arg)
{
    (void)arg;
    stage6_log("[DECODE WORKER] Hilo de decodificacion FLAC iniciado (PPU prio 500)");

    float in_chunk_44k[(CHUNK_IN_44K + 1) * 2];
    float out_chunk_48k[CHUNK_OUT_FRAMES * 2];
    uint32_t current_seq = 0;

    while (s_engine_running) {
        if (current_seq == s_track_sequence) {
            usleep(10000);
            continue;
        }

        current_seq = s_track_sequence;

        /* Flush ring buffer */
        s_ring_read_pos = 0;
        s_ring_write_pos = 0;
        s_resample_boundary[0] = 0.0f;
        s_resample_boundary[1] = 0.0f;
        s_total_pcm_frames = 0;
        s_current_pcm_frame = 0;
        s_played_pcm_frames = 0;

        /* Sincronizar con el worker de red para asegurar que inicializo el nuevo track */
        while (s_engine_running && current_seq == s_track_sequence && s_stream_ctx.seq != current_seq) {
            usleep(2000);
        }
        if (current_seq != s_track_sequence || !s_engine_running) continue;

        /* Wait for prebuffer: 2 MB or download_complete */
        uint64_t wait_start = get_time_usec();
        while (s_engine_running && current_seq == s_track_sequence) {
            if (s_stream_ctx.download_complete || s_stream_ctx.received >= PREBUFFER_THRESHOLD) {
                break;
            }
            if (s_stream_ctx.abort || s_has_error) {
                break;
            }
            if (get_time_usec() - wait_start > 25 * 1000000ULL) {
                /* Si tras 25s ya tenemos al menos 256 KB con cabecera valida, arrancar */
                if (s_stream_ctx.received >= 256 * 1024 && memcmp(s_stream_ctx.data, "fLaC", 4) == 0) {
                    stage6_log("[STREAM PREBUFFER] 25s alcanzados, arrancando con %lu KB recibidos",
                               (unsigned long)(s_stream_ctx.received / 1024));
                    break;
                }
                stage6_log("[STREAM TIMEOUT] 25s esperando prebuffer");
                break;
            }
            usleep(5000);
        }

        if (current_seq != s_track_sequence || !s_engine_running) continue;

        if (s_stream_ctx.abort || s_has_error) {
            stage6_log("[DECODE WORKER] Sesion abortada antes de iniciar decodificacion");
            s_is_playing = false;
            continue;
        }

        if (s_stream_ctx.received < 4 || memcmp(s_stream_ctx.data, "fLaC", 4) != 0) {
            stage6_log("[DECODE ERROR] Datos recibidos invalidos (magic no es fLaC)");
            continue;
        }

        s_stream_ctx.cursor = 0;
        drflac *pFlac = drflac_open_relaxed(flac_stream_read, flac_stream_seek, flac_stream_tell,
                                            drflac_container_native, &s_stream_ctx, NULL);
        if (!pFlac) {
            stage6_log("[DECODE ERROR] drflac_open_relaxed fallo para %s", s_stream_ctx.track_id);
            continue;
        }

        s_total_pcm_frames = pFlac->totalPCMFrameCount;
        uint32_t srate = pFlac->sampleRate;
        s_stream_sample_rate = srate ? srate : 44100;
        s_played_pcm_frames = 0;
        uint32_t chans = pFlac->channels;
        uint32_t bps   = pFlac->bitsPerSample;

        stage6_log("[STREAMINFO REAL] Metadatos FLAC decodificados:");
        stage6_log("  - Sample Rate: %u Hz | Canales: %u | Bits: %u-bit", srate, chans, bps);
        stage6_log("  - Total frames PCM: %llu (Duracion: %u:%02u)",
                   (unsigned long long)s_total_pcm_frames,
                   (uint32_t)(s_total_pcm_frames / (srate ? srate : 44100)) / 60,
                   (uint32_t)(s_total_pcm_frames / (srate ? srate : 44100)) % 60);

        uint64_t total_dec_time_usec = 0;
        uint64_t frames_dec_48k = 0;
        uint64_t last_stat_frames = 0;

        while (s_engine_running && current_seq == s_track_sequence) {
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

            if (srate == 44100) {
                in_chunk_44k[0] = s_resample_boundary[0];
                in_chunk_44k[1] = s_resample_boundary[1];

                drflac_uint64 read_frames = drflac_read_pcm_frames_f32(pFlac, CHUNK_IN_44K, in_chunk_44k + 2);
                if (read_frames == 0) {
                    if (s_stream_ctx.download_complete) {
                        stage6_log("[DECODE COMPLETO] Decodificacion FLAC completada (%llu frames). Drenando buffer...",
                                   (unsigned long long)s_current_pcm_frame);
                        break;
                    }
                    usleep(5000);
                    continue;
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
            } else if (srate == 48000) {
                drflac_uint64 read_frames = drflac_read_pcm_frames_f32(pFlac, CHUNK_OUT_FRAMES, out_chunk_48k);
                if (read_frames == 0) {
                    if (s_stream_ctx.download_complete) {
                        stage6_log("[DECODE COMPLETO] Decodificacion FLAC completada (%llu frames). Drenando buffer...",
                                   (unsigned long long)s_current_pcm_frame);
                        break;
                    }
                    usleep(5000);
                    continue;
                }
                s_current_pcm_frame += read_frames;
                ring_write_frames(out_chunk_48k, (size_t)read_frames);
            } else if (srate == 96000) {
                float in_chunk_96k[CHUNK_OUT_FRAMES * 2 * 2];
                drflac_uint64 read_frames = drflac_read_pcm_frames_f32(pFlac, CHUNK_OUT_FRAMES * 2, in_chunk_96k);
                if (read_frames == 0) {
                    if (s_stream_ctx.download_complete) {
                        stage6_log("[DECODE COMPLETO] Decodificacion FLAC completada (%llu frames). Drenando buffer...",
                                   (unsigned long long)s_current_pcm_frame);
                        break;
                    }
                    usleep(5000);
                    continue;
                }
                s_current_pcm_frame += read_frames;
                for (int k = 0; k < CHUNK_OUT_FRAMES; k++) {
                    out_chunk_48k[k * 2 + 0] = 0.5f * (in_chunk_96k[(k * 2) * 2 + 0] + in_chunk_96k[(k * 2 + 1) * 2 + 0]);
                    out_chunk_48k[k * 2 + 1] = 0.5f * (in_chunk_96k[(k * 2) * 2 + 1] + in_chunk_96k[(k * 2 + 1) * 2 + 1]);
                }
                ring_write_frames(out_chunk_48k, CHUNK_OUT_FRAMES);
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
            frames_dec_48k += CHUNK_OUT_FRAMES;

            if (frames_dec_48k >= last_stat_frames + 480000ULL) {
                last_stat_frames = frames_dec_48k;
                double audio_sec = (double)frames_dec_48k / (double)SAMPLE_RATE;
                double cpu_sec = (double)total_dec_time_usec / 1000000.0;
                double ratio = (audio_sec > 0.0) ? (cpu_sec / audio_sec) : 0.0;
                stage6_log("[AUDIO STATS] Pista: '%s' | Frames decodificados: %llu (%.1f s)",
                           s_stream_ctx.title, (unsigned long long)frames_dec_48k, audio_sec);
                stage6_log("  - Tiempo decodificacion CPU: %.3f s | Carga PPU: %.2f%%", cpu_sec, ratio * 100.0);
                stage6_log("  - Ring Buffer underruns: %u  |  Hardware DMA underruns: %u",
                           s_ring_underruns, s_hw_underruns);
            }
        }

        /* Esperar a que el consumidor de audio DMA vacie el ring buffer para reproducir hasta el final */
        while (s_engine_running && current_seq == s_track_sequence && ring_frames_available() > 0) {
            usleep(10000);
        }

        if (s_engine_running && current_seq == s_track_sequence) {
            s_is_playing = false;
            s_ring_primed = false;
            stage6_log("[TRACK FINISHED] Cancion '%s' reproducida completamente (%llu frames).",
                       s_stream_ctx.title, (unsigned long long)s_current_pcm_frame);
        }

        drflac_close(pFlac);
        stage6_log("[STREAM FIN] Fin de sesion de reproduccion para '%s'", s_stream_ctx.title);
    }

    stage6_log("[DECODE WORKER] Hilo de decodificacion finalizado");
    sysThreadExit(0);
}

/* ── Public API Implementation ── */

int audio_engine_init(void)
{
    stage6_log("=== MONOCHROME LOSSLESS FLAC STREAMING ENGINE: INICIO ===");

    sys_mutex_attr_t mattr;
    mattr.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
    mattr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    sysMutexCreate(&s_log_mutex, &mattr);
    s_log_mutex_inited = true;

    /* Allocate 48 MB XDR media streaming buffer */
    s_stream_media_buffer = (uint8_t *)memalign(1024 * 1024, STREAM_BUFFER_CAPACITY);
    if (!s_stream_media_buffer) {
        stage6_log("[ERROR] memalign fallo para s_stream_media_buffer (48 MB)");
        return -1;
    }
    stage6_log("[MEM] s_stream_media_buffer asignado @%p (tamano: 48 MB)", s_stream_media_buffer);

    int ret = audioInit();
    stage6_log("[AUDIO] audioInit() -> %d", ret);
    if (ret != 0) return -1;
    s_audio_inited = true;

    audioPortParam params;
    memset(&params, 0, sizeof(params));
    params.numChannels = AUDIO_PORT_2CH;
    params.numBlocks = AUDIO_BLOCK_8;
    params.attrib = AUDIO_PORT_INITLEVEL;
    params.level = 1.0f;

    ret = audioPortOpen(&params, &s_port_num);
    stage6_log("[AUDIO] audioPortOpen -> %d, portNum: %u", ret, s_port_num);
    if (ret != 0) return -1;
    s_audio_port_opened = true;

    ret = audioGetPortConfig(s_port_num, &s_config);
    stage6_log("[AUDIO] audioGetPortConfig -> %d (portSize: %d B)", ret, s_config.portSize);

    ret = audioCreateNotifyEventQueue(&s_snd_queue, &s_snd_key);
    stage6_log("[AUDIO] audioCreateNotifyEventQueue -> %d", ret);

    ret = audioSetNotifyEventQueue(s_snd_key);
    ret = sysEventQueueDrain(s_snd_queue);

    ret = audioPortStart(s_port_num);
    stage6_log("[AUDIO] audioPortStart -> %d", ret);
    if (ret == 0) s_audio_port_started = true;

    s_engine_running = true;

    /* 1. Launch Hardware Consumer Thread (prio 100, real-time audio DMA, 64KB stack) */
    ret = sysThreadCreate(&s_hw_thread_id, audio_hw_thread_func, NULL, 100,
                          0x10000, THREAD_JOINABLE, "mono_hw");
    stage6_log("[THREAD] sysThreadCreate(mono_hw) -> %d (prio 100)", ret);

    /* 2. Launch Progressive Stream & Decoder Thread (prio 600, 128KB stack) */
    ret = sysThreadCreate(&s_stream_thread_id, stream_decode_worker_func, NULL, 600,
                          0x20000, THREAD_JOINABLE, "mono_dec");
    stage6_log("[THREAD] sysThreadCreate(mono_dec) -> %d (prio 600)", ret);

    /* 3. Launch HTTP Streaming Downloader Thread (prio 400, 128KB stack) */
    ret = sysThreadCreate(&s_download_thread_id, stream_download_worker_func, NULL, 400,
                          0x20000, THREAD_JOINABLE, "mono_net");
    stage6_log("[THREAD] sysThreadCreate(mono_net) -> %d (prio 400)", ret);

    return 0;
}

void audio_engine_shutdown(void)
{
    stage6_log("[CLEANUP] Cerrando subsistema de audio y streaming...");

    s_engine_running = false;
    s_is_playing = false;
    s_stream_ctx.abort = true;
    if (s_stream_ctx.sock >= 0) {
        close(s_stream_ctx.sock);
        s_stream_ctx.sock = -1;
    }

    /* Detener de inmediato el puerto de audio hardware para silenciar DMA y frenar la cola de eventos */
    if (s_audio_port_started) {
        audioPortStop(s_port_num);
        s_audio_port_started = false;
        stage6_log("[AUDIO] audioPortStop(%u) ejecutado", s_port_num);
    }

    if (s_snd_key) {
        audioRemoveNotifyEventQueue(s_snd_key);
        sysEventQueueDestroy(s_snd_queue, 0);
        s_snd_key = 0;
    }

    if (s_audio_port_opened) {
        audioPortClose(s_port_num);
        s_audio_port_opened = false;
        stage6_log("[AUDIO] audioPortClose(%u) ejecutado", s_port_num);
    }

    if (s_audio_inited) {
        audioQuit();
        s_audio_inited = false;
        stage6_log("[AUDIO] audioQuit() ejecutado");
    }

    stage6_log("[AUDIO RESUMEN FINAL]");
    stage6_log("  - Underruns del Ring Buffer: %u", s_ring_underruns);
    stage6_log("  - Underruns de Hardware DMA: %u", s_hw_underruns);
    stage6_log("[AUDIO] Subsistema cerrado limpiamente");
}

void audio_engine_play_track(const char *track_id, const char *title)
{
    if (!track_id) return;

    /* Abortar cualquier conexion activa o decodificacion de la pista previa */
    s_stream_ctx.abort = true;
    if (s_stream_ctx.sock >= 0) {
        close(s_stream_ctx.sock);
        s_stream_ctx.sock = -1;
    }

    s_has_error = false;
    s_last_error_code = 0;
    snprintf(s_status_message, sizeof(s_status_message), "Conectando...");

    strncpy(s_pending_track_id, track_id, sizeof(s_pending_track_id) - 1);
    strncpy(s_pending_track_title, title ? title : "", sizeof(s_pending_track_title) - 1);

    /* Nuevo track: desactivar priming e invalidar contexto para que el decoder
       espere a que el worker de red inicialice el nuevo sequence */
    s_ring_primed = false;
    s_ring_underruns = 0;
    s_ring_read_pos = 0;
    s_ring_write_pos = 0;

    s_stream_ctx.seq = 0;
    s_stream_ctx.received = 0;
    s_stream_ctx.download_complete = false;
    s_played_pcm_frames = 0;
    __asm__ volatile("lwsync" ::: "memory");

    s_is_playing = true;
    s_track_sequence++;
}

void audio_engine_play(int track_index)
{
    (void)track_index;
    s_is_playing = true;
}

void audio_engine_pause(void)
{
    s_is_playing = false;
    stage6_log("[AUDIO] Reproduccion pausada");
}

void audio_engine_resume(void)
{
    s_is_playing = true;
    stage6_log("[AUDIO] Reproduccion reanudada");
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
    stage6_log("[AUDIO] Volumen: %d%%", (int)(s_volume * 100.0f));
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
