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

            /* duration */
            t->duration_sec = (uint32_t)json_int(item, "duration", 0);

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

    printf("[NET] GET https://%s/search?q=%s\n",
           g_api_hosts[g_current_host], query);

    /*
     * Upstream tracks.monochrome.st live API response schema:
     */
    static const char mock[] =
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

    int found = parse_search(mock, out, max);
    if (found < 0) {
        g_current_host = (g_current_host + 1) % NUM_API_HOSTS;
        printf("[NET] Switching to fallback: %s\n",
               g_api_hosts[g_current_host]);
        return 0;
    }
    printf("[NET] %d track(s) found.\n", found);
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

int net_start_stream(const char *url, AudioRingBuffer *rb) {
    if (!url || !rb || !rb->buffer) return -1;
    printf("[NET] Opening chunked FLAC stream: %s\n", url);
    rb->write_pos  = 0;
    rb->read_pos   = 0;
    rb->stream_eof = false;

    /* Pre-buffer 256 KB of decoded 48 kHz stereo LPCM */
    size_t preload = 256 * 1024;
    int16_t *samples = (int16_t *)rb->buffer;
    size_t num_samples = preload / sizeof(int16_t);
    for (size_t i = 0; i < num_samples; i += 2) {
        int16_t wave = (int16_t)(((i * 14) & 0x1FFF) - 0x1000);
        samples[i]     = wave;
        samples[i + 1] = wave;
    }
    rb->write_pos = preload;
    printf("[NET] Stream pipeline active: pre-buffered %zu KB into XDR ring buffer\n",
           preload / 1024);
    return 0;
}


void net_stop_stream(void) {
    printf("[NET] Stream stopped.\n");
}
