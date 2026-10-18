/**
 * Monochrome — Album Cover Art Caching & Scaling Engine
 *
 * Asynchronously downloads cover artwork from tracks.monochrome.st/proxy/mi/...
 * Decodes via stb_image into RGB, applies high-quality box-filter downsampling
 * to 80x80 32-bit ARGB texture, and caches for instant rendering on PC & PS3.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "monochrome.h"
#include "platform.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

static CoverArtImage        s_current_cover = {0};
static PlatformThreadHandle s_cover_thread  = NULL;
static volatile bool        s_cover_abort   = false;
static volatile bool        s_request_pending = false;

static char s_pending_id[64]   = {0};
static char s_pending_url[256] = {0};

/* Box-filter resizer & 32-bit ARGB format converter (RGB -> 0xAARRGGBB) */
static void resize_and_convert(const unsigned char *src_rgb, int src_w, int src_h,
                               uint32_t *dst_pixels, int dst_w, int dst_h) {
    if (!src_rgb || !dst_pixels || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;

    for (int dy = 0; dy < dst_h; dy++) {
        int sy0 = (dy * src_h) / dst_h;
        int sy1 = ((dy + 1) * src_h) / dst_h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > src_h) sy1 = src_h;

        for (int dx = 0; dx < dst_w; dx++) {
            int sx0 = (dx * src_w) / dst_w;
            int sx1 = ((dx + 1) * src_w) / dst_w;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > src_w) sx1 = src_w;

            uint32_t r_sum = 0, g_sum = 0, b_sum = 0;
            int count = 0;

            for (int y = sy0; y < sy1; y++) {
                const unsigned char *row = &src_rgb[y * src_w * 3];
                for (int x = sx0; x < sx1; x++) {
                    r_sum += row[x * 3 + 0];
                    g_sum += row[x * 3 + 1];
                    b_sum += row[x * 3 + 2];
                    count++;
                }
            }

            if (count > 0) {
                uint32_t r = r_sum / (uint32_t)count;
                uint32_t g = g_sum / (uint32_t)count;
                uint32_t b = b_sum / (uint32_t)count;
                dst_pixels[dy * dst_w + dx] = 0xFF000000u | (r << 16) | (g << 8) | b;
            } else {
                dst_pixels[dy * dst_w + dx] = 0xFF14161Cu;
            }
        }
    }
}

/* Generates a stylish procedural vinyl / music disc artwork fallback */
static void generate_procedural_cover(uint32_t *dst_pixels, int w, int h, const char *title) {
    uint32_t hash = 5381;
    if (title) {
        for (const char *p = title; *p; p++) hash = ((hash << 5) + hash) + (uint8_t)*p;
    }
    uint8_t accent_r = 40 + (uint8_t)(hash & 0x7F);
    uint8_t accent_g = 60 + (uint8_t)((hash >> 8) & 0x7F);
    uint8_t accent_b = 90 + (uint8_t)((hash >> 16) & 0x7F);

    int cx = w / 2;
    int cy = h / 2;
    int max_r = (w < h ? w : h) / 2 - 2;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int dx = x - cx;
            int dy = y - cy;
            int dist2 = dx * dx + dy * dy;
            int dist = 0;
            /* Simple integer square root */
            while (dist * dist <= dist2) dist++;

            if (dist > max_r) {
                /* Background margin */
                dst_pixels[y * w + x] = 0xFF12141Au;
            } else if (dist < 10) {
                /* Center hole */
                dst_pixels[y * w + x] = 0xFFFFFFFFu;
            } else if (dist < 22) {
                /* Center label */
                dst_pixels[y * w + x] = 0xFF000000u | ((uint32_t)accent_r << 16) | ((uint32_t)accent_g << 8) | accent_b;
            } else {
                /* Grooves */
                uint8_t groove = (uint8_t)(20 + (dist % 4 == 0 ? 30 : 0));
                dst_pixels[y * w + x] = 0xFF000000u | ((uint32_t)groove << 16) | ((uint32_t)groove << 8) | groove;
            }
        }
    }
}

static void cover_worker_func(void *arg) {
    (void)arg;

    while (!s_cover_abort) {
        if (!s_request_pending) {
            platform_sleep_ms(40);
            continue;
        }

        char id[64];
        char url[256];
        strncpy(id, s_pending_id, sizeof(id) - 1);
        id[sizeof(id) - 1] = '\0';
        strncpy(url, s_pending_url, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
        s_request_pending = false;

        if (id[0] == '\0') continue;

        bool loaded = false;
        char cache_path[512];
        platform_get_cache_cover_path(id, cache_path, sizeof(cache_path));

        /* 1. Check persistent disk cache (instant 0 ms load) */
        if (platform_file_exists(cache_path)) {
            PlatformFileHandle f = platform_file_open_read(cache_path);
            if (f) {
                size_t expected = COVER_SIZE * COVER_SIZE * sizeof(uint32_t);
                size_t n = platform_file_read(f, s_current_cover.pixels, expected);
                platform_file_close(f);
                if (n == expected) {
                    s_current_cover.width = COVER_SIZE;
                    s_current_cover.height = COVER_SIZE;
                    strncpy(s_current_cover.track_id, id, sizeof(s_current_cover.track_id) - 1);
                    s_current_cover.ready = true;
                    loaded = true;
                    printf("[COVER] Persistent Cache HIT: %s (instant 0 ms load)\n", cache_path);
                }
            }
        }

        /* 2. Download from network if not cached */
        if (!loaded && url[0] != '\0') {
            printf("[COVER] Downloading album artwork: %s\n", url);
            char  *raw_buf = NULL;
            size_t raw_len = 0;

            int res = platform_http_get(url, &raw_buf, &raw_len);
            if (res == 0 && raw_buf && raw_len > 4 && !s_cover_abort) {
                /* Verify JPEG magic (ffd8) or PNG magic (8950) */
                const uint8_t *u = (const uint8_t *)raw_buf;
                if ((u[0] == 0xFF && u[1] == 0xD8) || (u[0] == 0x89 && u[1] == 0x50)) {
                    int w = 0, h = 0, channels = 0;
                    unsigned char *rgb = stbi_load_from_memory(u, (int)raw_len, &w, &h, &channels, 3);
                    if (rgb && w > 0 && h > 0) {
                        resize_and_convert(rgb, w, h, s_current_cover.pixels, COVER_SIZE, COVER_SIZE);
                        s_current_cover.width = COVER_SIZE;
                        s_current_cover.height = COVER_SIZE;
                        strncpy(s_current_cover.track_id, id, sizeof(s_current_cover.track_id) - 1);
                        s_current_cover.ready = true;
                        loaded = true;
                        printf("[COVER] Successfully decoded cover artwork (%dx%d -> %dx%d)\n",
                               w, h, COVER_SIZE, COVER_SIZE);
                        stbi_image_free(rgb);

                        /* Save to persistent disk cache */
                        PlatformFileHandle f = platform_file_open_write(cache_path);
                        if (f) {
                            platform_file_write(f, s_current_cover.pixels, COVER_SIZE * COVER_SIZE * sizeof(uint32_t));
                            platform_file_close(f);
                            printf("[COVER] Saved 80x80 decoded texture to cache: %s\n", cache_path);
                        }
                    }
                }
            }
            if (raw_buf) platform_http_free(raw_buf);
        }

        /* Fallback to procedural high-res vinyl artwork if download failed / offline */
        if (!loaded && !s_cover_abort) {
            generate_procedural_cover(s_current_cover.pixels, COVER_SIZE, COVER_SIZE, id);
            s_current_cover.width = COVER_SIZE;
            s_current_cover.height = COVER_SIZE;
            strncpy(s_current_cover.track_id, id, sizeof(s_current_cover.track_id) - 1);
            s_current_cover.ready = true;
            printf("[COVER] Generated procedural artwork for: %s\n", id);
        }
    }
}

void cover_art_init(void) {
    platform_ensure_cache_dirs();
    memset(&s_current_cover, 0, sizeof(s_current_cover));
    s_current_cover.width = COVER_SIZE;
    s_current_cover.height = COVER_SIZE;
    generate_procedural_cover(s_current_cover.pixels, COVER_SIZE, COVER_SIZE, "Monochrome");
    s_current_cover.ready = true;

    s_cover_abort = false;
    s_request_pending = false;
    s_cover_thread = platform_thread_create(cover_worker_func, NULL);
    printf("[COVER] Subsystem initialized (80x80 texture cache)\n");
}

void cover_art_shutdown(void) {
    s_cover_abort = true;
    s_request_pending = false;
    if (s_cover_thread) {
        platform_thread_join(s_cover_thread);
        s_cover_thread = NULL;
    }
    s_current_cover.ready = false;
    printf("[COVER] Subsystem shut down.\n");
}

void cover_art_request(const char *track_id, const char *cover_url) {
    if (!track_id || track_id[0] == '\0') return;
    if (strcmp(s_current_cover.track_id, track_id) == 0 && s_current_cover.ready) {
        return;
    }

    strncpy(s_pending_id, track_id, sizeof(s_pending_id) - 1);
    s_pending_id[sizeof(s_pending_id) - 1] = '\0';
    if (cover_url) {
        strncpy(s_pending_url, cover_url, sizeof(s_pending_url) - 1);
        s_pending_url[sizeof(s_pending_url) - 1] = '\0';
    } else {
        s_pending_url[0] = '\0';
    }
    s_request_pending = true;
}

const CoverArtImage *cover_art_get_current(void) {
    return &s_current_cover;
}
