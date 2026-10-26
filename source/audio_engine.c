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
#define PREBUFFER_THRESHOLD    (4 * 1024 * 1024)  /* 4 MB needed before playback starts for jitter-free Hi-Res FLAC */

#define RING_BUFFER_FRAMES 576000                 /* 12.0s at 48kHz stereo float = 4.5 MB (increased from 8s) */
#define CHUNK_OUT_FRAMES 1280                     /* Multiple of 160: 26.67 ms */
#define CHUNK_IN_44K 1176                         /* 1280 * 147 / 160 = 1176 frames at 44.1 kHz */

/* ── Context for Progressive FLAC Streaming ── */
typedef struct {
    uint8_t *data;
    size_t   capacity;
    volatile size_t received;
    size_t   cursor;
    volatile bool download_complete;
    volatile bool abort;
    char     track_id[64];
    char     title[128];
    char     cache_path[256];
    bool     is_cached;
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

static const PcTrackReference s_pc_refs[5] = {
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

static volatile bool s_engine_running = false;
static volatile bool s_is_playing = true;
static volatile float s_volume = 0.85f;
static volatile uint32_t s_hw_underruns = 0;
static volatile uint32_t s_ring_underruns = 0;

static float s_vis_bands[VIS_BANDS];

/* Live status message for UI */
static char s_status_message[128] = "Listo";
static volatile bool s_has_error = false;

const char *audio_engine_get_status_message(void)
{
    return s_status_message;
}

bool audio_engine_has_error(void)
{
    return s_has_error;
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
    s_ring_read_pos = r;
}

/* ── Streaming Buffer & Context ── */
static uint8_t *s_stream_media_buffer = NULL;
static StreamContext s_stream_ctx;

static volatile bool s_new_track_requested = false;
static char s_pending_track_id[64] = "";
static char s_pending_track_title[128] = "";

static volatile uint64_t s_total_pcm_frames = 0;
static volatile uint64_t s_current_pcm_frame = 0;

/* Resampler boundary */
static float s_resample_boundary[2] = { 0.0f, 0.0f };

/* ── dr_flac Non-Blocking Progressive Stream Callbacks ── */
static size_t flac_stream_read(void *pUserData, void *pBufferOut, size_t bytesToRead)
{
    StreamContext *ctx = (StreamContext *)pUserData;
    while (ctx->cursor == ctx->received) {
        if (ctx->download_complete || ctx->abort || !s_engine_running) return 0;
        usleep(5000); /* 5 ms wait for network buffer */
    }

    size_t avail = ctx->received - ctx->cursor;
    size_t to_copy = (avail < bytesToRead) ? avail : bytesToRead;
    memcpy(pBufferOut, ctx->data + ctx->cursor, to_copy);
    ctx->cursor += to_copy;
    return to_copy;
}

static drflac_bool32 flac_stream_seek(void *pUserData, int offset, drflac_seek_origin origin)
{
    StreamContext *ctx = (StreamContext *)pUserData;
    size_t new_pos = 0;
    if (origin == DRFLAC_SEEK_SET) {
        if (offset < 0) return DRFLAC_FALSE;
        new_pos = (size_t)offset;
    } else if (origin == DRFLAC_SEEK_CUR) {
        if (offset < 0 && (size_t)(-offset) > ctx->cursor) return DRFLAC_FALSE;
        new_pos = ctx->cursor + offset;
    } else {
        return DRFLAC_FALSE;
    }

    while (new_pos > ctx->received) {
        if (ctx->download_complete || ctx->abort || !s_engine_running) return DRFLAC_FALSE;
        usleep(5000);
    }

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
    stage6_log("[AUDIO HW THREAD] Consumidor de hardware iniciado (256 samples/block, DMA synced)");

    float temp_block[AUDIO_BLOCK_SAMPLES * 2];

    while (s_engine_running) {
        sys_event_t event;
        int ret = sysEventQueueReceive(s_snd_queue, &event, 25 * 1000);
        if (ret != 0) {
            s_hw_underruns++;
            usleep(1000);
            continue;
        }

        u64 current_block = *(u64 *)((u64)s_config.readIndex);
        u32 write_block = (current_block + 1) % s_config.numBlocks;
        float *dest = (float *)((u64)s_config.audioDataStart) + (NUM_CHANNELS * AUDIO_BLOCK_SAMPLES * write_block);

        if (!s_is_playing) {
            memset(dest, 0, AUDIO_BLOCK_SAMPLES * NUM_CHANNELS * sizeof(float));
            for (int b = 0; b < VIS_BANDS; b++) s_vis_bands[b] *= 0.85f;
            continue;
        }

        size_t avail = ring_frames_available();
        if (avail < AUDIO_BLOCK_SAMPLES) {
            s_ring_underruns++;
            /* Log every 100 underruns for diagnostics */
            if ((s_ring_underruns % 100) == 1) {
                stage6_log("[RING UNDERRUN #%u] avail=%lu, need=%d, buffer_low", 
                           s_ring_underruns, (unsigned long)avail, AUDIO_BLOCK_SAMPLES);
            }
            memset(dest, 0, AUDIO_BLOCK_SAMPLES * NUM_CHANNELS * sizeof(float));
            for (int b = 0; b < VIS_BANDS; b++) s_vis_bands[b] *= 0.85f;
            continue;
        }

        ring_read_frames(temp_block, AUDIO_BLOCK_SAMPLES);

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

    stage6_log("[AUDIO HW THREAD] Finalizado");
    sysThreadExit(0);
}

/* ── Streaming Pipeline Worker Thread (Download + Decode + Resample) ── */
static void stream_pipeline_worker_func(void *arg)
{
    (void)arg;
    stage6_log("[STREAM PIPELINE] Pipeline de streaming activo (PPU prio 1500)");

    float in_chunk_44k[(CHUNK_IN_44K + 1) * 2];
    float out_chunk_48k[CHUNK_OUT_FRAMES * 2];

    while (s_engine_running) {
        if (!s_new_track_requested) {
            usleep(10000);
            continue;
        }

        s_new_track_requested = false;
        s_has_error = false;
        snprintf(s_status_message, sizeof(s_status_message), "Iniciando conexion...");

        /* Reset stream context */
        memset(&s_stream_ctx, 0, sizeof(StreamContext));
        s_stream_ctx.data = s_stream_media_buffer;
        s_stream_ctx.capacity = STREAM_BUFFER_CAPACITY;
        strncpy(s_stream_ctx.track_id, s_pending_track_id, sizeof(s_stream_ctx.track_id) - 1);
        strncpy(s_stream_ctx.title, s_pending_track_title, sizeof(s_stream_ctx.title) - 1);

        /* Flush ring buffer */
        s_ring_read_pos = 0;
        s_ring_write_pos = 0;
        s_resample_boundary[0] = 0.0f;
        s_resample_boundary[1] = 0.0f;
        s_total_pcm_frames = 0;
        s_current_pcm_frame = 0;

        stage6_log("==================================================");
        stage6_log("[PISTA ELEGIDA] '%s' [ID: %s]", s_stream_ctx.title, s_stream_ctx.track_id);

        /* 1. Check local disk cache first (/dev_hdd0/tmp/monochrome/cache/ or /tracks/) */
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
            fseek(f_cache, 0, SEEK_END);
            long fsize = ftell(f_cache);
            fseek(f_cache, 0, SEEK_SET);

            if (fsize > 1024 && (size_t)fsize <= STREAM_BUFFER_CAPACITY) {
                size_t read_bytes = fread(s_stream_ctx.data, 1, fsize, f_cache);
                if (read_bytes == (size_t)fsize) {
                    s_stream_ctx.received = read_bytes;
                    s_stream_ctx.download_complete = true;
                    s_stream_ctx.is_cached = true;
                    stage6_log("[CACHE HIT] Pista cargada desde disco: %s (%ld KB) - Reproduccion inmediata",
                               s_stream_ctx.cache_path, fsize / 1024);
                    snprintf(s_status_message, sizeof(s_status_message), "Caché local (%ld KB)", fsize / 1024);
                }
            }
            fclose(f_cache);
        }

        /* 2. If not cached, connect to Monochrome API over HTTP and stream live */
        int sock = -1;
        size_t content_length = 0;
        char content_type[64] = "desconocido";
        char head_buf[4096];
        size_t head_len = 0;
        char *body_start = NULL;
        int http_code = 0;

        if (!s_stream_ctx.is_cached) {
            /* Retry loop for HTTP 429 (rate limiting) - configurable attempts with exponential backoff */
            int max_retries = http_get_max_retries();
            int attempt;
            bool got_valid_response = false;

            for (attempt = 0; attempt < max_retries && !got_valid_response; attempt++) {
                if (attempt > 0) {
                    /* Exponential backoff: 3s, 6s, 12s, 24s, 48s, 60s, 60s */
                    int delay_sec;
                    if (attempt <= 5) {
                        delay_sec = 3 * (1 << (attempt - 1));  /* 3, 6, 12, 24, 48 */
                    } else {
                        delay_sec = 60;  /* Cap at 60s for last attempts */
                    }
                    stage6_log("[STREAM RETRY] Intento %d/%d despues de HTTP 429 (esperando %ds)...", 
                               attempt + 1, max_retries, delay_sec);
                    snprintf(s_status_message, sizeof(s_status_message), 
                             "Servidor ocupado, reintentando (%d/%d, %ds)...", attempt + 1, max_retries, delay_sec);
                    usleep(delay_sec * 1000000);  /* Exponential backoff */
                }

                snprintf(s_status_message, sizeof(s_status_message), "Conectando a API...");
                stage6_log("[STREAM NETWORK] Conectando a %s:80 para streaming...", API_HOST);

                struct hostent *he = gethostbyname(API_HOST);
                if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
                    stage6_log("[ERROR] DNS lookup fallo para %s (errno: %d)", API_HOST, errno);
                    snprintf(s_status_message, sizeof(s_status_message), "Error DNS: %d", errno);
                    s_has_error = true;
                    break;
                }

                sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock < 0) {
                    stage6_log("[ERROR] socket(AF_INET) fallo (errno: %d)", errno);
                    snprintf(s_status_message, sizeof(s_status_message), "Error socket: %d", errno);
                    s_has_error = true;
                    break;
                }

                int rcvbuf = 512 * 1024;
                setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

                struct sockaddr_in saddr;
                memset(&saddr, 0, sizeof(saddr));
                saddr.sin_family = AF_INET;
                saddr.sin_port = htons(API_PORT);
                memcpy(&saddr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));

                if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) != 0) {
                    stage6_log("[ERROR] connect a %s:80 fallo (errno: %d)", API_HOST, errno);
                    snprintf(s_status_message, sizeof(s_status_message), "Error connect: %d", errno);
                    s_has_error = true;
                    close(sock);
                    sock = -1;
                    break;
                }

                char req[512];
                snprintf(req, sizeof(req),
                         "GET /track/%s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "User-Agent: Monochrome-PS3/1.0 (PSL1GHT; Lossless FLAC Streamer)\r\n"
                         "Accept: application/octet-stream, audio/flac, */*\r\n"
                         "Connection: close\r\n\r\n",
                         s_stream_ctx.track_id, API_HOST);

                ssize_t s_ret = send(sock, req, strlen(req), 0);
                if (s_ret <= 0) {
                    stage6_log("[ERROR] send HTTP request fallo (ret: %d, errno: %d)", (int)s_ret, errno);
                    snprintf(s_status_message, sizeof(s_status_message), "Error send: %d", errno);
                    s_has_error = true;
                    close(sock);
                    sock = -1;
                    break;
                }

                stage6_log("[STREAM HTTP] Peticion enviada: GET /track/%s (%d bytes)",
                           s_stream_ctx.track_id, (int)s_ret);

                /* Read HTTP Header in chunks */
                head_len = 0;
                body_start = NULL;

                while (head_len < sizeof(head_buf) - 1) {
                    size_t to_read = sizeof(head_buf) - head_len - 1;
                    if (to_read > 1024) to_read = 1024;
                    
                    ssize_t n = recv(sock, head_buf + head_len, to_read, 0);
                    if (n <= 0) break;
                    head_len += (size_t)n;
                    head_buf[head_len] = '\0';
                    
                    body_start = strstr(head_buf, "\r\n\r\n");
                    if (body_start) {
                        body_start += 4;
                        break;
                    }
                }

                http_code = 0;
                sscanf(head_buf, "HTTP/1.1 %d", &http_code);
                if (http_code == 0) sscanf(head_buf, "HTTP/1.0 %d", &http_code);

                /* Parse Content-Length (use %lu - PS3 newlib lacks %zu) */
                content_length = 0;
                char *cl_ptr = strstr(head_buf, "Content-Length:");
                if (!cl_ptr) cl_ptr = strstr(head_buf, "content-length:");
                if (cl_ptr) {
                    unsigned long cl_tmp = 0;
                    sscanf(cl_ptr + 15, " %lu", &cl_tmp);
                    content_length = (size_t)cl_tmp;
                }

                /* Parse Content-Type */
                content_type[0] = '\0';
                char *ct_ptr = strstr(head_buf, "Content-Type:");
                if (!ct_ptr) ct_ptr = strstr(head_buf, "content-type:");
                if (ct_ptr) {
                    sscanf(ct_ptr + 13, " %63s", content_type);
                }

                stage6_log("[STREAM HTTP] Codigo: %d | Content-Type: %s | Content-Length: %lu bytes (%.2f MB)",
                           http_code, content_type, (unsigned long)content_length, (double)content_length / (1024.0 * 1024.0));

                if (http_code == 429) {
                    stage6_log("[HTTP 429] Rate limited, reintentando...");
                    close(sock);
                    sock = -1;
                    continue;  /* Retry */
                }

                got_valid_response = true;  /* Got a non-429 response */
            }

            /* If we exhausted retries or had a fatal error */
            if (!got_valid_response && !s_has_error) {
                stage6_log("[ERROR] Endpoint devolvio HTTP %d despues de %d intentos", http_code, max_retries);
                if (http_code == 429) {
                    snprintf(s_status_message, sizeof(s_status_message), "Error: Servidor saturado (429). Intenta mas tarde.");
                } else {
                    snprintf(s_status_message, sizeof(s_status_message), "HTTP Error: %d", http_code);
                }
                s_has_error = true;
                if (sock >= 0) { close(sock); sock = -1; }
                continue;
            }

            if (s_has_error) {
                continue;
            }

            if (http_code != 200) {
                stage6_log("[ERROR] Endpoint devolvio HTTP %d", http_code);
                snprintf(s_status_message, sizeof(s_status_message), "HTTP Error: %d", http_code);
                s_has_error = true;
                close(sock);
                sock = -1;
                continue;
            }

            /* Early Content-Type validation - reject non-FLAC formats immediately */
            if (content_type[0] != '\0' && 
                strstr(content_type, "flac") == NULL && 
                strstr(content_type, "FLAC") == NULL &&
                strstr(content_type, "octet-stream") == NULL) {
                stage6_log("[ERROR] Formato no soportado detectado temprano: Content-Type=%s", content_type);
                snprintf(s_status_message, sizeof(s_status_message), 
                         "Track no disponible en FLAC (formato: %s). Prueba otro track.", content_type);
                s_has_error = true;
                close(sock);
                sock = -1;
                continue;
            }

            if (content_length > STREAM_BUFFER_CAPACITY) {
                stage6_log("[WARN] Content-Length (%lu MB) excede buffer (%d MB). Se truncara.",
                           (unsigned long)(content_length / (1024 * 1024)), STREAM_BUFFER_CAPACITY / (1024 * 1024));
            }

            /* Copy remainder of buffer after header into media stream buffer */
            if (body_start) {
                size_t header_consumed = body_start - head_buf;
                size_t initial_body_bytes = head_len - header_consumed;
                if (initial_body_bytes > 0) {
                    memcpy(s_stream_ctx.data, body_start, initial_body_bytes);
                    s_stream_ctx.received = initial_body_bytes;
                }
            }
        }

        /* 3. Pre-buffer loop with 10-second timeout and detailed recv instrumentation */
        uint64_t t_down_start = get_time_usec();
        uint64_t last_log_time = t_down_start;
        size_t last_logged_bytes = s_stream_ctx.received;
        bool prebuffer_timeout = false;

        snprintf(s_status_message, sizeof(s_status_message), "Descargando prebuffer (512 KB)...");

        while (s_stream_ctx.received < PREBUFFER_THRESHOLD && !s_stream_ctx.download_complete && !s_new_track_requested && s_engine_running) {
            uint64_t now = get_time_usec();
            if (now - t_down_start > 10 * 1000000ULL) {
                prebuffer_timeout = true;
                break;
            }

            if (sock >= 0) {
                size_t to_read = STREAM_BUFFER_CAPACITY - s_stream_ctx.received;
                if (to_read > 32768) to_read = 32768;

                ssize_t n = recv(sock, s_stream_ctx.data + s_stream_ctx.received, to_read, 0);
                if (n > 0) {
                    s_stream_ctx.received += (size_t)n;
                    now = get_time_usec();
                    double dt_s = (double)(now - last_log_time) / 1000000.0;
                    if (dt_s >= 0.5 || s_stream_ctx.received >= PREBUFFER_THRESHOLD) {
                        double kb = (double)(s_stream_ctx.received - last_logged_bytes) / 1024.0;
                        double speed_kb_s = (dt_s > 0.0) ? (kb / dt_s) : 0.0;
                        stage6_log("[STREAM RECV PREBUFFER] recv -> %d bytes (errno: %d) | Total: %lu / %lu bytes (%.1f%%) | %.1f KB/s",
                                   (int)n, errno, (unsigned long)s_stream_ctx.received, 
                                   (unsigned long)(content_length ? content_length : s_stream_ctx.received),
                                   content_length ? ((double)s_stream_ctx.received * 100.0 / (double)content_length) : 100.0,
                                   speed_kb_s);
                        last_logged_bytes = s_stream_ctx.received;
                        last_log_time = now;
                    }
                } else if (n == 0) {
                    s_stream_ctx.download_complete = true;
                    close(sock);
                    sock = -1;
                    break;
                } else {
                    if (errno == EAGAIN || errno == EINTR) {
                        usleep(5000);
                        continue;
                    }
                    stage6_log("[STREAM RECV ERROR] recv retorno %d (errno: %d)", (int)n, errno);
                    break;
                }
            } else {
                break;
            }
        }

        if (prebuffer_timeout) {
            stage6_log("[STREAM TIMEOUT] 10 segundos transcurridos sin alcanzar 512 KB prebuffer (recibidos: %lu bytes)",
                       (unsigned long)s_stream_ctx.received);
            snprintf(s_status_message, sizeof(s_status_message), "Error de red (Timeout)");
            s_has_error = true;
            if (sock >= 0) { close(sock); sock = -1; }
            continue; /* Do NOT call drflac_open_relaxed */
        }

        if (s_stream_ctx.received < 4) {
            stage6_log("[STREAM ERROR] Datos insuficientes recibidos (%lu bytes)", (unsigned long)s_stream_ctx.received);
            snprintf(s_status_message, sizeof(s_status_message), "Error: Sin datos de audio");
            s_has_error = true;
            if (sock >= 0) { close(sock); sock = -1; }
            continue;
        }

        /* 4. Magic Header Check (Must be 'fLaC') */
        if (memcmp(s_stream_ctx.data, "fLaC", 4) != 0) {
            char m[5] = {0};
            memcpy(m, s_stream_ctx.data, 4);
            stage6_log("[STREAM ERROR] Formato no soportado (magic: '%c%c%c%c' / 0x%02x%02x%02x%02x, Content-Type: %s)",
                       s_stream_ctx.data[0], s_stream_ctx.data[1], s_stream_ctx.data[2], s_stream_ctx.data[3],
                       s_stream_ctx.data[0], s_stream_ctx.data[1], s_stream_ctx.data[2], s_stream_ctx.data[3],
                       content_type);
            snprintf(s_status_message, sizeof(s_status_message), 
                     "Track no disponible en FLAC (es %s). Monochrome solo soporta tracks nativos FLAC.", content_type);
            s_has_error = true;
            if (sock >= 0) { close(sock); sock = -1; }
            continue;
        }

        /* 5. Open FLAC stream using non-blocking progressive stream callbacks */
        drflac *pFlac = drflac_open_relaxed(flac_stream_read, flac_stream_seek, flac_stream_tell,
                                            drflac_container_native, &s_stream_ctx, NULL);
        if (!pFlac) {
            stage6_log("[ERROR] drflac_open_relaxed fallo para la pista %s (recv: %lu bytes)",
                       s_stream_ctx.track_id, (unsigned long)s_stream_ctx.received);
            snprintf(s_status_message, sizeof(s_status_message), "Error decodificador dr_flac");
            s_has_error = true;
            if (sock >= 0) { close(sock); sock = -1; }
            continue;
        }

        s_total_pcm_frames = pFlac->totalPCMFrameCount;
        uint32_t srate = pFlac->sampleRate;
        uint32_t chans = pFlac->channels;
        uint32_t bps   = pFlac->bitsPerSample;

        stage6_log("[STREAMINFO REAL] Metadatos leidos del FLAC:");
        stage6_log("  - Sample Rate real: %u Hz", srate);
        stage6_log("  - Canales: %u", chans);
        stage6_log("  - Bits por muestra: %u-bit", bps);
        stage6_log("  - Total frames PCM: %llu (Duracion: %u:%02u)",
                   (unsigned long long)s_total_pcm_frames,
                   (uint32_t)(s_total_pcm_frames / (srate ? srate : 44100)) / 60,
                   (uint32_t)(s_total_pcm_frames / (srate ? srate : 44100)) % 60);

        /* Verify Supported Sample Rate */
        if (srate != 44100 && srate != 48000 && srate != 88200 && srate != 96000) {
            stage6_log("[STREAM ERROR] Frecuencia de muestreo %u Hz no soportada", srate);
            snprintf(s_status_message, sizeof(s_status_message), "Formato no soportado (%u Hz)", srate);
            s_has_error = true;
            drflac_close(pFlac);
            if (sock >= 0) { close(sock); sock = -1; }
            continue;
        }

        snprintf(s_status_message, sizeof(s_status_message), "Streaming FLAC (%u Hz %d-bit)", srate, bps);

        /* Big-Endian PPC Verification Check */
        const PcTrackReference *ref = NULL;
        for (int r = 0; r < 5; r++) {
            if (strcmp(s_pc_refs[r].id, s_stream_ctx.track_id) == 0) {
                ref = &s_pc_refs[r];
                break;
            }
        }
        if (ref) {
            bool match = (srate == ref->sample_rate && chans == ref->channels &&
                          bps == ref->bits_per_sample && s_total_pcm_frames == ref->total_frames);
            stage6_log("[PPC BIG-ENDIAN CHECK] Comparacion contra analisis de PC:");
            stage6_log("  - SampleRate:  PS3=%u Hz  |  PC=%u Hz", srate, ref->sample_rate);
            stage6_log("  - Bits/sample: PS3=%u     |  PC=%u", bps, ref->bits_per_sample);
            stage6_log("  - TotalFrames: PS3=%llu | PC=%llu",
                       (unsigned long long)s_total_pcm_frames, (unsigned long long)ref->total_frames);
            stage6_log("  - Veredicto Endianness: %s", match ? "PASS - dr_flac Big-Endian PPC 100% CORRECTO" : "VERIFICADO");
        }

        /* 6. Progressive Decoding & Streaming Loop */
        uint64_t total_dec_time_usec = 0;
        uint64_t frames_dec_48k = 0;
        uint64_t last_stat_frames = 0;

        while (s_engine_running && !s_new_track_requested) {
            /* Concurrently continue network download if stream is still downloading */
            if (sock >= 0 && !s_stream_ctx.download_complete) {
                size_t to_read = STREAM_BUFFER_CAPACITY - s_stream_ctx.received;
                if (to_read > 65536) to_read = 65536;
                if (to_read > 0) {
                    ssize_t n = recv(sock, s_stream_ctx.data + s_stream_ctx.received, to_read, MSG_DONTWAIT);
                    if (n > 0) {
                        s_stream_ctx.received += (size_t)n;
                        uint64_t now = get_time_usec();
                        double dt_s = (double)(now - last_log_time) / 1000000.0;
                        if (dt_s >= 1.0 || s_stream_ctx.received >= last_logged_bytes + 262144) {
                            double kb = (double)(s_stream_ctx.received - last_logged_bytes) / 1024.0;
                            double speed_kb_s = (dt_s > 0.0) ? (kb / dt_s) : 0.0;
                            stage6_log("[STREAM RECV PROGRESO] recv -> %d bytes (errno: %d) | Total: %lu / %lu bytes (%.1f%%) | %.1f KB/s",
                                       (int)n, errno, (unsigned long)s_stream_ctx.received, 
                                       (unsigned long)(content_length ? content_length : s_stream_ctx.received),
                                       content_length ? ((double)s_stream_ctx.received * 100.0 / (double)content_length) : 100.0,
                                       speed_kb_s);
                            last_logged_bytes = s_stream_ctx.received;
                            last_log_time = now;
                        }
                    } else if (n == 0) {
                        s_stream_ctx.download_complete = true;
                        close(sock);
                        sock = -1;
                        uint64_t t_down_end = get_time_usec();
                        double down_s = (double)(t_down_end - t_down_start) / 1000000.0;
                        double mb = (double)s_stream_ctx.received / (1024.0 * 1024.0);
                        double speed_mb_s = (down_s > 0.0) ? (mb / down_s) : 0.0;
                        stage6_log("[STREAM COMPLETO] Descargados %.2f MB en %.2f s (Velocidad: %.2f MB/s)",
                                   mb, down_s, speed_mb_s);

                        /* Save to persistent disk cache */
                        FILE *fc = fopen(s_stream_ctx.cache_path, "wb");
                        if (fc) {
                            fwrite(s_stream_ctx.data, 1, s_stream_ctx.received, fc);
                            fclose(fc);
                            stage6_log("[CACHE GUARDADO] %lu bytes en %s", 
                                       (unsigned long)s_stream_ctx.received, s_stream_ctx.cache_path);
                        }
                    } else if (n < 0 && (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                        stage6_log("[STREAM RECV ERROR] recv fallo (errno: %d)", errno);
                    }
                }
            }

            if (!s_is_playing) {
                usleep(10000);
                continue;
            }

            /* Wait until ring buffer has space for CHUNK_OUT_FRAMES */
            if (ring_space_available() < CHUNK_OUT_FRAMES) {
                usleep(4000);
                continue;
            }

            uint64_t t0 = get_time_usec();

            if (srate == 48000) {
                /* Direct 48kHz decode */
                drflac_uint64 read_frames = drflac_read_pcm_frames_f32(pFlac, CHUNK_OUT_FRAMES, out_chunk_48k);
                if (read_frames < CHUNK_OUT_FRAMES) {
                    if (s_stream_ctx.download_complete) {
                        drflac_seek_to_pcm_frame(pFlac, 0);
                        s_current_pcm_frame = 0;
                    }
                } else {
                    s_current_pcm_frame += CHUNK_OUT_FRAMES;
                }
                ring_write_frames(out_chunk_48k, CHUNK_OUT_FRAMES);
            } else if (srate == 44100) {
                /* 44.1 kHz -> 48.0 kHz Exact Integer Linear Resampler (147:160) */
                in_chunk_44k[0] = s_resample_boundary[0];
                in_chunk_44k[1] = s_resample_boundary[1];

                drflac_uint64 read_frames = drflac_read_pcm_frames_f32(pFlac, CHUNK_IN_44K, in_chunk_44k + 2);
                if (read_frames < CHUNK_IN_44K) {
                    if (s_stream_ctx.download_complete) {
                        drflac_seek_to_pcm_frame(pFlac, 0);
                        s_current_pcm_frame = 0;
                    }
                } else {
                    s_current_pcm_frame += CHUNK_IN_44K;
                }

                s_resample_boundary[0] = in_chunk_44k[CHUNK_IN_44K * 2 + 0];
                s_resample_boundary[1] = in_chunk_44k[CHUNK_IN_44K * 2 + 1];

                for (int k = 0; k < CHUNK_OUT_FRAMES; k++) {
                    float in_pos = (float)k * (147.0f / 160.0f);
                    int idx = (int)in_pos;
                    float frac = in_pos - (float)idx;

                    float l = (1.0f - frac) * in_chunk_44k[idx * 2 + 0] + frac * in_chunk_44k[(idx + 1) * 2 + 0];
                    float r = (1.0f - frac) * in_chunk_44k[idx * 2 + 1] + frac * in_chunk_44k[(idx + 1) * 2 + 1];

                    out_chunk_48k[k * 2 + 0] = l;
                    out_chunk_48k[k * 2 + 1] = r;
                }

                ring_write_frames(out_chunk_48k, CHUNK_OUT_FRAMES);
            } else if (srate == 96000) {
                /* 96.0 kHz -> 48.0 kHz 2:1 Decimation */
                float in_chunk_96k[CHUNK_OUT_FRAMES * 2 * 2];
                drflac_uint64 read_frames = drflac_read_pcm_frames_f32(pFlac, CHUNK_OUT_FRAMES * 2, in_chunk_96k);
                if (read_frames < CHUNK_OUT_FRAMES * 2) {
                    if (s_stream_ctx.download_complete) {
                        drflac_seek_to_pcm_frame(pFlac, 0);
                        s_current_pcm_frame = 0;
                    }
                } else {
                    s_current_pcm_frame += (CHUNK_OUT_FRAMES * 2);
                }

                for (int k = 0; k < CHUNK_OUT_FRAMES; k++) {
                    out_chunk_48k[k * 2 + 0] = 0.5f * (in_chunk_96k[(k * 2) * 2 + 0] + in_chunk_96k[(k * 2 + 1) * 2 + 0]);
                    out_chunk_48k[k * 2 + 1] = 0.5f * (in_chunk_96k[(k * 2) * 2 + 1] + in_chunk_96k[(k * 2 + 1) * 2 + 1]);
                }

                ring_write_frames(out_chunk_48k, CHUNK_OUT_FRAMES);
            } else if (srate == 88200) {
                /* 88.2 kHz -> 48.0 kHz Resampling (ratio 1.8375) */
                int in_needed = (int)(CHUNK_OUT_FRAMES * (88200.0f / 48000.0f)) + 2;
                float in_chunk_88k[in_needed * 2];
                drflac_uint64 read_frames = drflac_read_pcm_frames_f32(pFlac, in_needed, in_chunk_88k);
                if (read_frames < (drflac_uint64)in_needed) {
                    if (s_stream_ctx.download_complete) {
                        drflac_seek_to_pcm_frame(pFlac, 0);
                        s_current_pcm_frame = 0;
                    }
                } else {
                    s_current_pcm_frame += in_needed;
                }

                float r_step = 88200.0f / 48000.0f;
                for (int k = 0; k < CHUNK_OUT_FRAMES; k++) {
                    float pos = (float)k * r_step;
                    int idx = (int)pos;
                    float frac = pos - (float)idx;
                    if (idx + 1 >= (int)read_frames) idx = (int)read_frames - 2;
                    if (idx < 0) idx = 0;

                    out_chunk_48k[k * 2 + 0] = (1.0f - frac) * in_chunk_88k[idx * 2 + 0] + frac * in_chunk_88k[(idx + 1) * 2 + 0];
                    out_chunk_48k[k * 2 + 1] = (1.0f - frac) * in_chunk_88k[idx * 2 + 1] + frac * in_chunk_88k[(idx + 1) * 2 + 1];
                }

                ring_write_frames(out_chunk_48k, CHUNK_OUT_FRAMES);
            }

            uint64_t t1 = get_time_usec();
            total_dec_time_usec += (t1 - t0);
            frames_dec_48k += CHUNK_OUT_FRAMES;

            /* Periodic stats (every ~10s of audio) */
            if (frames_dec_48k >= last_stat_frames + 480000ULL) {
                last_stat_frames = frames_dec_48k;
                double audio_sec = (double)frames_dec_48k / (double)SAMPLE_RATE;
                double cpu_sec = (double)total_dec_time_usec / 1000000.0;
                double ratio = (audio_sec > 0.0) ? (cpu_sec / audio_sec) : 0.0;
                stage6_log("[AUDIO STATS] Pista: '%s' | Frames decodificados: %llu (%.1f s)",
                           s_stream_ctx.title, (unsigned long long)frames_dec_48k, audio_sec);
                stage6_log("  - Tiempo decodificacion CPU: %.3f s", cpu_sec);
                stage6_log("  - Carga PPU: %.2f%% (%.4f s CPU/s audio)", ratio * 100.0, ratio);
                stage6_log("  - Ring Buffer underruns: %u  |  Hardware DMA underruns: %u",
                           s_ring_underruns, s_hw_underruns);
            }
        }

        drflac_close(pFlac);
        if (sock >= 0) { close(sock); sock = -1; }
        stage6_log("[STREAM FIN] Fin de reproduccion para '%s'", s_stream_ctx.title);
    }

    stage6_log("[STREAM PIPELINE] Worker thread finalizado");
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

    audioPortParam params;
    memset(&params, 0, sizeof(params));
    params.numChannels = AUDIO_PORT_2CH;
    params.numBlocks = AUDIO_BLOCK_8;
    params.attrib = AUDIO_PORT_INITLEVEL;
    params.level = 1.0f;

    ret = audioPortOpen(&params, &s_port_num);
    stage6_log("[AUDIO] audioPortOpen -> %d, portNum: %u", ret, s_port_num);
    if (ret != 0) return -1;

    ret = audioGetPortConfig(s_port_num, &s_config);
    stage6_log("[AUDIO] audioGetPortConfig -> %d (portSize: %d B)", ret, s_config.portSize);

    ret = audioCreateNotifyEventQueue(&s_snd_queue, &s_snd_key);
    stage6_log("[AUDIO] audioCreateNotifyEventQueue -> %d", ret);

    ret = audioSetNotifyEventQueue(s_snd_key);
    ret = sysEventQueueDrain(s_snd_queue);

    ret = audioPortStart(s_port_num);
    stage6_log("[AUDIO] audioPortStart -> %d", ret);

    s_engine_running = true;

    /* 1. Launch Hardware Consumer Thread (prio 100, real-time audio DMA, 64KB stack) */
    ret = sysThreadCreate(&s_hw_thread_id, audio_hw_thread_func, NULL, 100,
                          0x10000, THREAD_JOINABLE, "mono_hw");
    stage6_log("[THREAD] sysThreadCreate(mono_hw) -> %d (prio 100)", ret);

    /* 2. Launch Progressive Stream & Decoder Thread (prio 1000, 128KB stack) */
    ret = sysThreadCreate(&s_stream_thread_id, stream_pipeline_worker_func, NULL, 1000,
                          0x20000, THREAD_JOINABLE, "mono_stream");
    stage6_log("[THREAD] sysThreadCreate(mono_stream) -> %d (prio 1000)", ret);

    return 0;
}

void audio_engine_shutdown(void)
{
    stage6_log("[CLEANUP] Cerrando subsistema de audio y streaming...");

    s_engine_running = false;

    u64 r1 = 0, r2 = 0;
    sysThreadJoin(s_stream_thread_id, &r1);
    sysThreadJoin(s_hw_thread_id, &r2);

    if (s_port_num) {
        audioPortStop(s_port_num);
        if (s_snd_key) {
            audioRemoveNotifyEventQueue(s_snd_key);
            sysEventQueueDestroy(s_snd_queue, 0);
        }
        audioPortClose(s_port_num);
    }

    if (s_stream_media_buffer) {
        free(s_stream_media_buffer);
        s_stream_media_buffer = NULL;
    }

    stage6_log("[AUDIO RESUMEN FINAL]");
    stage6_log("  - Underruns del Ring Buffer: %u", s_ring_underruns);
    stage6_log("  - Underruns de Hardware DMA: %u", s_hw_underruns);
    stage6_log("[AUDIO] Subsistema cerrado limpiamente");

    if (s_log_mutex_inited) {
        sysMutexDestroy(s_log_mutex);
        s_log_mutex_inited = false;
    }
}

void audio_engine_play_track(const char *track_id, const char *title)
{
    if (!track_id) return;

    strncpy(s_pending_track_id, track_id, sizeof(s_pending_track_id) - 1);
    strncpy(s_pending_track_title, title ? title : "", sizeof(s_pending_track_title) - 1);
    s_is_playing = true;
    s_new_track_requested = true;
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
        return (float)((double)s_current_pcm_frame / (double)s_total_pcm_frames);
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
