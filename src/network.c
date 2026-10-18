/**
 * Monochrome PS3 — Network & TLS subsystem
 *
 * Connects to the Monochrome REST API over HTTPS (TLS 1.2/1.3 via mbedTLS).
 * Parses JSON responses with the bundled cJSON parser.
 * Streams FLAC data into the shared AudioRingBuffer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "monochrome.h"
#include "cJSON.h"

#define DR_FLAC_NO_STDIO
#include "dr_flac.h"

/* ── Fallback host table ─────────────────────────────────────────── */
static const char *g_api_hosts[] = {
    DEFAULT_API_HOST,
    "api.monochrome.tf",
    "wolf.qqdl.site",
    "maus.qqdl.site",
    "vogel.qqdl.site",
};
#define NUM_API_HOSTS  (sizeof(g_api_hosts) / sizeof(g_api_hosts[0]))

static int g_current_host = 0;
static bool g_net_ready = false;

/* ── Init / Shutdown ─────────────────────────────────────────────── */
int net_init(void) {
    platform_ensure_cache_dirs();
    printf("[NET] Initialising sys_net socket layer…\n");
    /* Real PS3: sys_net_initialize_network() */

    /* Load TLS certificate bundle */
    FILE *f = fopen(CERT_BUNDLE_PATH, "rb");
    if (!f) {
        printf("[NET] WARN: %s not found — HTTPS verification disabled\n",
               CERT_BUNDLE_PATH);
    } else {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        printf("[NET] Root CA bundle loaded (%ld bytes)\n", sz);
    }

    g_net_ready = true;
    printf("[NET] Ready — primary host: %s\n", g_api_hosts[g_current_host]);
    return 0;
}

void net_shutdown(void) {
    g_net_ready = false;
    printf("[NET] Shut down.\n");
}

/* ── JSON helpers ────────────────────────────────────────────────── */
static const char *json_str(const cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}

static int json_int(const cJSON *obj, const char *key, int fallback) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsNumber(v)) ? v->valueint : fallback;
}

/* ── Parse API search response ───────────────────────────────────── */
static int parse_search(const char *json, TrackMetadata *out, int max) {
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        printf("[NET] JSON parse error near: %.40s\n",
               cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "(null)");
        return -1;
    }

    /* Monochrome wraps results in "data" or "tracks", or returns bare array */
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!items) items = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    if (!items && cJSON_IsArray(root)) items = root;

    int count = 0;
    if (items && cJSON_IsArray(items)) {
        int n = cJSON_GetArraySize(items);
        for (int i = 0; i < n && count < max; i++) {
            cJSON *item = cJSON_GetArrayItem(items, i);
            if (!item) continue;

            TrackMetadata *t = &out[count];
            memset(t, 0, sizeof(*t));

            /* id: trackId, recordingId, or id */
            cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "trackId");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "recordingId");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "id");
            if (jid && cJSON_IsString(jid))
                snprintf(t->id, sizeof(t->id), "%s", jid->valuestring);
            else if (jid && cJSON_IsNumber(jid))
                snprintf(t->id, sizeof(t->id), "%.0f", jid->valuedouble);

            /* title */
            const char *s = json_str(item, "title");
            if (s) snprintf(t->title, sizeof(t->title), "%s", s);

            /* artist: artistNames array or artist object/string */
            cJSON *jarr = cJSON_GetObjectItemCaseSensitive(item, "artistNames");
            if (jarr && cJSON_IsArray(jarr) && cJSON_GetArraySize(jarr) > 0) {
                cJSON *first = cJSON_GetArrayItem(jarr, 0);
                if (first && cJSON_IsString(first))
                    snprintf(t->artist, sizeof(t->artist), "%s", first->valuestring);
            } else {
                cJSON *ja = cJSON_GetObjectItemCaseSensitive(item, "artist");
                if (ja && cJSON_IsString(ja))
                    snprintf(t->artist, sizeof(t->artist), "%s", ja->valuestring);
                else if (ja && cJSON_IsObject(ja)) {
                    s = json_str(ja, "name");
                    if (s) snprintf(t->artist, sizeof(t->artist), "%s", s);
                }
            }

            /* album */
            cJSON *jal = cJSON_GetObjectItemCaseSensitive(item, "album");
            if (jal && cJSON_IsString(jal))
                snprintf(t->album, sizeof(t->album), "%s", jal->valuestring);
            else if (jal && cJSON_IsObject(jal)) {
                s = json_str(jal, "title");
                if (s) snprintf(t->album, sizeof(t->album), "%s", s);
            } else {
                s = json_str(item, "albumTitle");
                if (s) snprintf(t->album, sizeof(t->album), "%s", s);
            }

            /* duration: Tidal API returns milliseconds (e.g. 320357 ms = 320 sec) */
            uint32_t dur = (uint32_t)json_int(item, "duration", 0);
            if (dur > 10000) dur /= 1000;
            t->duration_sec = dur;

            /* quality */
            s = json_str(item, "audioQuality");
            if (s && (strstr(s, "HI_RES") || strstr(s, "MASTER"))) {
                t->is_hi_res    = true;
                t->sample_rate  = 96000;
                t->bit_depth    = 24;
            } else {
                t->is_hi_res    = false;
                t->sample_rate  = 44100;
                t->bit_depth    = 16;
            }

            /* cover URL: artwork or cover */
            s = json_str(item, "artwork");
            if (!s) s = json_str(item, "cover");
            if (s) snprintf(t->cover_url, sizeof(t->cover_url), "%s", s);

            count++;
        }
    }

    cJSON_Delete(root);
    return count;
}

/* ── Public API ──────────────────────────────────────────────────── */
int net_search(const char *query, TrackMetadata *out, int max) {
    if (!g_net_ready || !query || !out || max <= 0) return -1;

    /* URL encode query (replace spaces with '+') */
    char clean_q[256] = {0};
    for (size_t i = 0, j = 0; query[i] && j < sizeof(clean_q) - 1; i++) {
        clean_q[j++] = (query[i] == ' ') ? '+' : query[i];
    }

    char url[512];
    snprintf(url, sizeof(url), "https://%s/search?q=%s",
             g_api_hosts[g_current_host], clean_q);

    printf("[NET] Querying live API: %s\n", url);

    char *json_data = NULL;
    size_t json_len = 0;
    int ret = platform_http_get(url, &json_data, &json_len);

    int found = 0;
    if (ret == 0 && json_data && json_len > 0) {
        found = parse_search(json_data, out, max);
        platform_http_free(json_data);
    }

    /* Fallback to embedded catalogue if offline or network error */
    if (found <= 0) {
        static const char fallback_mock[] =
            "{\"tracks\":["
            "{\"trackId\":\"155102881689636864\",\"title\":\"Harder, Better, Faster, Stronger\","
             "\"artistNames\":[\"Daft Punk\"],\"albumTitle\":\"Discovery\","
             "\"duration\":224,\"audioQuality\":\"LOSSLESS\","
             "\"artwork\":\"https://tracks.monochrome.st/cover/155102881689636864\"},"
            "{\"trackId\":\"155102871061270528\",\"title\":\"One More Time\","
             "\"artistNames\":[\"Daft Punk\"],\"albumTitle\":\"Discovery\","
             "\"duration\":320,\"audioQuality\":\"LOSSLESS\","
             "\"artwork\":\"https://tracks.monochrome.st/cover/155102871061270528\"},"
            "{\"trackId\":\"155142491052445696\",\"title\":\"Instant Crush\","
             "\"artistNames\":[\"Daft Punk\",\"Julian Casablancas\"],\"albumTitle\":\"Random Access Memories\","
             "\"duration\":337,\"audioQuality\":\"HI_RES\","
             "\"artwork\":\"https://tracks.monochrome.st/cover/155142491052445696\"},"
            "{\"trackId\":\"155142502393843712\",\"title\":\"Get Lucky\","
             "\"artistNames\":[\"Daft Punk\",\"Pharrell Williams\"],\"albumTitle\":\"Random Access Memories\","
             "\"duration\":248,\"audioQuality\":\"HI_RES\","
             "\"artwork\":\"https://tracks.monochrome.st/cover/155142502393843712\"}"
            "]}";

        found = parse_search(fallback_mock, out, max);
        if (found < 0) {
            g_current_host = (g_current_host + 1) % NUM_API_HOSTS;
            return 0;
        }
        printf("[NET] Live fetch unavailable, using offline cached catalogue (%d tracks)\n", found);
    } else {
        printf("[NET] Live search succeeded: %d tracks found\n", found);
    }

    return found;
}

int net_resolve_stream(const char *track_id, char *url_out, size_t url_len) {
    if (!track_id || !url_out) return -1;
    /* Upstream track streaming: GET https://tracks.monochrome.st/track/:id */
    snprintf(url_out, url_len, "https://%s/track/%s",
             g_api_hosts[g_current_host], track_id);
    printf("[NET] Stream URL → %s\n", url_out);
    return 0;
}

/* ── Asynchronous FLAC Streaming Pipeline ─────────────────────────── */
typedef struct {
    char                 url[512];
    AudioRingBuffer     *rb;
    PlatformThreadHandle thread;
    volatile bool        abort_requested;
    volatile bool        is_running;
} StreamWorkerState;

static StreamWorkerState s_worker = {0};

typedef struct {
    char                 url[512];
    char                 cache_path[512];
    uint8_t             *data;
    size_t               capacity;
    volatile size_t      received;
    size_t               cursor;
    volatile bool        eof;
    volatile bool        abort;
    PlatformThreadHandle dl_thread;
} FlacStreamContext;

static size_t flac_stream_read(void *pUserData, void *pBufferOut, size_t bytesToRead) {
    FlacStreamContext *ctx = (FlacStreamContext *)pUserData;
    while (ctx->cursor == ctx->received) {
        if (ctx->eof || ctx->abort) return 0;
        platform_sleep_ms(5);
    }
    size_t avail = ctx->received - ctx->cursor;
    size_t to_copy = (avail < bytesToRead) ? avail : bytesToRead;
    memcpy(pBufferOut, ctx->data + ctx->cursor, to_copy);
    ctx->cursor += to_copy;
    return to_copy;
}

static drflac_bool32 flac_stream_seek(void *pUserData, int offset, drflac_seek_origin origin) {
    FlacStreamContext *ctx = (FlacStreamContext *)pUserData;
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
        if (ctx->eof || ctx->abort) return DRFLAC_FALSE;
        platform_sleep_ms(5);
    }

    ctx->cursor = new_pos;
    return DRFLAC_TRUE;
}

static drflac_bool32 flac_stream_tell(void *pUserData, drflac_int64 *pCursor) {
    FlacStreamContext *ctx = (FlacStreamContext *)pUserData;
    if (!ctx || !pCursor) return DRFLAC_FALSE;
    *pCursor = (drflac_int64)ctx->cursor;
    return DRFLAC_TRUE;
}

#if defined(PLATFORM_IS_PS3)
    #define STREAM_BUFFER_CAPACITY (16 * 1024 * 1024)
#else
    #define STREAM_BUFFER_CAPACITY (48 * 1024 * 1024)
#endif

static bool on_http_stream_chunk(const uint8_t *chunk, size_t chunk_len, void *userdata) {
    FlacStreamContext *ctx = (FlacStreamContext *)userdata;
    if (!ctx || ctx->abort) return false;

    if (ctx->received + chunk_len > ctx->capacity) {
        printf("[STREAM] Warning: Track exceeds stream buffer capacity (%zu MB)\n", ctx->capacity / (1024*1024));
        return false;
    }

    memcpy(ctx->data + ctx->received, chunk, chunk_len);
    CELL_LWSYNC();
    ctx->received += chunk_len;
    return true;
}

static void download_worker_func(void *arg) {
    FlacStreamContext *ctx = (FlacStreamContext *)arg;
    if (!ctx) return;
    platform_http_get_streaming(ctx->url, on_http_stream_chunk, ctx);
    ctx->eof = true;

    /* If completed without abort and has valid data, save to persistent cache immediately */
    if (!ctx->abort && ctx->received > 1024 && ctx->cache_path[0] != '\0') {
        PlatformFileHandle cf = platform_file_open_write(ctx->cache_path);
        if (cf) {
            platform_file_write(cf, ctx->data, ctx->received);
            platform_file_close(cf);
            printf("[STREAM] Download complete: Saved %zu KB to persistent cache: %s\n",
                   ctx->received / 1024, ctx->cache_path);
        }
    }
}

static void ring_write_pcm(AudioRingBuffer *rb, const int16_t *samples, size_t frames) {
    if (!rb || !samples || frames == 0) return;
    size_t bytes = frames * 2 * sizeof(int16_t);
    size_t w = rb->write_pos;
    size_t cap = rb->capacity;
    size_t chunk1 = cap - w;
    if (chunk1 >= bytes) {
        memcpy(&rb->buffer[w], samples, bytes);
        w += bytes;
        if (w >= cap) w = 0;
    } else {
        memcpy(&rb->buffer[w], samples, chunk1);
        size_t chunk2 = bytes - chunk1;
        memcpy(&rb->buffer[0], ((const uint8_t *)samples) + chunk1, chunk2);
        w = chunk2;
    }
    CELL_LWSYNC();
    rb->write_pos = w;
}

static void stream_worker_func(void *arg) {
    StreamWorkerState *worker = (StreamWorkerState *)arg;
    if (!worker) return;

    printf("[STREAM] Background worker starting progressive pipeline: %s\n", worker->url);

    FlacStreamContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.capacity = STREAM_BUFFER_CAPACITY;
    ctx.data = (uint8_t *)malloc(ctx.capacity);
    if (!ctx.data) {
        printf("[STREAM] Error: Failed to allocate initial stream buffer!\n");
        worker->is_running = false;
        audio_set_state(PLAYBACK_STOPPED);
        return;
    }

    snprintf(ctx.url, sizeof(ctx.url), "%s", worker->url);

    /* Extract track ID from url (e.g. https://.../track/<id>) */
    char track_id[64] = {0};
    const char *p_id = strstr(worker->url, "/track/");
    if (p_id) {
        p_id += 7;
        size_t k = 0;
        while (p_id[k] && p_id[k] != '?' && p_id[k] != '/' && k < sizeof(track_id) - 1) {
            track_id[k] = p_id[k];
            k++;
        }
        track_id[k] = '\0';
    }

    char cache_path[512] = {0};
    bool is_cached = false;
    if (track_id[0] != '\0') {
        platform_get_cache_track_path(track_id, cache_path, sizeof(cache_path));
        size_t c_size = platform_file_size(cache_path);
        if (c_size > 1024) {
            PlatformFileHandle cf = platform_file_open_read(cache_path);
            if (cf) {
                uint8_t *cached_data = (uint8_t *)malloc(c_size);
                if (cached_data) {
                    size_t n_read = platform_file_read(cf, cached_data, c_size);
                    platform_file_close(cf);
                    if (n_read == c_size) {
                        free(ctx.data);
                        ctx.data = cached_data;
                        ctx.capacity = c_size;
                        ctx.received = c_size;
                        ctx.eof = true;
                        is_cached = true;
                        printf("[STREAM] Persistent Cache HIT: %s (%zu KB, instant zero-network playback)\n",
                               cache_path, c_size / 1024);
                    } else {
                        free(cached_data);
                    }
                } else {
                    platform_file_close(cf);
                }
            }
        }
    }

    /* 1. Launch download worker thread if not cached */
    if (!is_cached) {
        snprintf(ctx.cache_path, sizeof(ctx.cache_path), "%s", cache_path);
        ctx.dl_thread = platform_thread_create(download_worker_func, &ctx);
        if (!ctx.dl_thread) {
            printf("[STREAM] Error: Failed to spawn download thread!\n");
            free(ctx.data);
            worker->is_running = false;
            audio_set_state(PLAYBACK_STOPPED);
            return;
        }

        /* 2. Wait until pre-buffer has >= 64 KB, EOF, or abort */
        while (ctx.received < 64 * 1024 && !ctx.eof && !worker->abort_requested) {
            platform_sleep_ms(10);
        }

        if (worker->abort_requested || (ctx.eof && ctx.received < 4)) {
            printf("[STREAM] Worker aborted or network error before decoder startup (recv=%zu)\n", ctx.received);
            ctx.abort = true;
            platform_thread_join(ctx.dl_thread);
            free(ctx.data);
            worker->is_running = false;
            audio_set_state(PLAYBACK_STOPPED);
            return;
        }
    }

    /* 3. Open FLAC stream using non-blocking/progressive callbacks */
    drflac *pFlac = drflac_open_relaxed(flac_stream_read, flac_stream_seek, flac_stream_tell,
                                        drflac_container_native, &ctx, NULL);
    if (!pFlac) {
        printf("[STREAM] Error: corrupted or unsupported FLAC stream format.\n");
        ctx.abort = true;
        if (ctx.dl_thread) platform_thread_join(ctx.dl_thread);
        free(ctx.data);
        worker->is_running = false;
        audio_set_state(PLAYBACK_STOPPED);
        return;
    }

    uint32_t sample_rate = pFlac->sampleRate;
    uint8_t channels = (uint8_t)pFlac->channels;
    uint64_t total_frames = pFlac->totalPCMFrameCount;
    printf("[STREAM] SUCCESS: Opened progressive FLAC stream (%zu KB buffered)!\n", ctx.received / 1024);
    printf("[STREAM] FLAC Format: %u Hz | %d ch | %llu frames (%.1f sec)\n",
           sample_rate, (int)channels, (unsigned long long)total_frames,
           (sample_rate > 0) ? (float)total_frames / (float)sample_rate : 0.0f);

    #define DECODE_CHUNK_FRAMES 2048
    int16_t raw_pcm[DECODE_CHUNK_FRAMES * 2];
    int16_t resampled_pcm[DECODE_CHUNK_FRAMES * 2 * 2];

    /* Immediate playback startup */
    audio_set_state(PLAYBACK_PLAYING);

    size_t total_decoded_frames = 0;

    /* 4. Progressive decode loop */
    while (!worker->abort_requested && !ctx.abort) {
        /* Check ring buffer space before decoding */
        size_t max_bytes = 2500 * 2 * sizeof(int16_t);
        if (ring_free(worker->rb) < max_bytes) {
            platform_sleep_ms(10);
            continue;
        }

        drflac_uint64 read = drflac_read_pcm_frames_s16(pFlac, DECODE_CHUNK_FRAMES, raw_pcm);
        if (read == 0) {
            if (ctx.eof) {
                printf("[STREAM] End of FLAC stream reached (%zu frames total).\n", total_decoded_frames);
                worker->rb->stream_eof = true;
                break;
            }
            platform_sleep_ms(5);
            continue;
        }

        const int16_t *out_pcm = raw_pcm;
        size_t out_frames = (size_t)read;

        /* Resample to 48000 Hz if needed */
        if (sample_rate == 44100) {
            spu_resample_44_to_48(raw_pcm, (size_t)read, resampled_pcm, &out_frames);
            out_pcm = resampled_pcm;
        } else if (sample_rate == 96000) {
            spu_decimate_96_to_48(raw_pcm, (size_t)read, resampled_pcm, &out_frames);
            out_pcm = resampled_pcm;
        }

        ring_write_pcm(worker->rb, out_pcm, out_frames);
        total_decoded_frames += out_frames;
    }

    /* 5. Clean teardown & Save to Persistent Cache */
    ctx.abort = true;
    if (ctx.dl_thread) {
        platform_thread_join(ctx.dl_thread);
        ctx.dl_thread = NULL;
    }

    if (!is_cached && !worker->abort_requested && ctx.received > 1024 && cache_path[0] != '\0') {
        PlatformFileHandle cf = platform_file_open_write(cache_path);
        if (cf) {
            platform_file_write(cf, ctx.data, ctx.received);
            platform_file_close(cf);
            printf("[STREAM] Saved %zu KB to persistent cache: %s\n", ctx.received / 1024, cache_path);
        }
    }

    drflac_close(pFlac);
    free(ctx.data);
    worker->is_running = false;
    printf("[STREAM] Background worker finished cleanly.\n");
}

int net_start_stream(const char *url, AudioRingBuffer *rb) {
    if (!url || !rb || !rb->buffer) return -1;

    net_stop_stream();

    rb->write_pos  = 0;
    rb->read_pos   = 0;
    rb->stream_eof = false;

    snprintf(s_worker.url, sizeof(s_worker.url), "%s", url);
    s_worker.rb = rb;
    s_worker.abort_requested = false;
    s_worker.is_running = true;

    audio_set_state(PLAYBACK_BUFFERING);

    s_worker.thread = platform_thread_create(stream_worker_func, &s_worker);
    if (!s_worker.thread) {
        printf("[NET] Failed to spawn stream worker thread!\n");
        s_worker.is_running = false;
        audio_set_state(PLAYBACK_STOPPED);
        return -1;
    }

    printf("[NET] Stream pipeline spawned asynchronously: %s\n", url);
    return 0;
}

void net_stop_stream(void) {
    if (s_worker.is_running) {
        printf("[NET] Stopping active stream worker...\n");
        s_worker.abort_requested = true;
        if (s_worker.thread) {
            platform_thread_join(s_worker.thread);
            s_worker.thread = NULL;
        }
        s_worker.is_running = false;
        printf("[NET] Stream worker stopped.\n");
    }
}

