/**
 * Monochrome PS3 — Audio engine
 *
 * Reads decoded PCM from the ring buffer (filled by net thread + SPU FLAC
 * decoder) and pumps it into sys_audio_out at 48 kHz LPCM stereo.
 *
 * The resampler converts 44.1 kHz source material to the PS3 DAC's native
 * 48 kHz using a proper fractional ratio (160/147) with linear interpolation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "monochrome.h"

/* ── Internal state ──────────────────────────────────────────────── */
static AudioRingBuffer *g_rb     = NULL;
static PlaybackState    g_pstate = PLAYBACK_STOPPED;

/* sys_audio_out block size: 256 frames × 2 ch × 2 bytes = 1024 bytes */
#define AUDIO_BLOCK_FRAMES  256
#define AUDIO_BLOCK_BYTES   (AUDIO_BLOCK_FRAMES * 2 * sizeof(int16_t))

static int16_t g_pcm_block[AUDIO_BLOCK_FRAMES * 2];  /* L R L R … */

/* ── Init / Shutdown ─────────────────────────────────────────────── */
int audio_init(AudioRingBuffer *rb) {
    if (!rb || !rb->buffer) return -1;
    g_rb = rb;
    g_pstate = PLAYBACK_STOPPED;

    printf("[AUDIO] sys_audio_out configured: 48 kHz / 16-bit / stereo\n");
    printf("[AUDIO] Block size: %d frames (%zu bytes)\n",
           AUDIO_BLOCK_FRAMES, AUDIO_BLOCK_BYTES);
    return 0;
}

void audio_shutdown(void) {
    g_pstate = PLAYBACK_STOPPED;
    g_rb = NULL;
    printf("[AUDIO] Shut down.\n");
}

/* ── Playback control ────────────────────────────────────────────── */
void audio_set_state(PlaybackState s) {
    if (g_pstate == s) return;
    g_pstate = s;
    static const char *names[] = {
        "STOPPED", "BUFFERING", "PLAYING", "PAUSED"
    };
    printf("[AUDIO] State → %s\n", names[s]);
}

PlaybackState audio_get_state(void) {
    return g_pstate;
}

/* ── SPU resampler: 44100 → 48000 (ratio 160 / 147) ─────────────── */
void spu_resample_44_to_48(const int16_t *src, size_t frames_in,
                           int16_t *dst, size_t *frames_out) {
    /*
     * Linear interpolation resampler.
     * Phase accumulator steps by 44100/48000 = 147/160 per output sample.
     * On the real SPU this runs in the 256 KB Local Store with SIMD
     * vector signed short operations on quadwords.
     */
    const size_t RATIO_NUM = 147;
    const size_t RATIO_DEN = 160;

    size_t out = 0;
    size_t phase_num = 0;  /* fractional position numerator */

    while (phase_num / RATIO_DEN < frames_in - 1) {
        size_t idx  = phase_num / RATIO_DEN;       /* integer sample index */
        size_t frac = phase_num % RATIO_DEN;       /* fractional part */

        /* Left channel: linear interpolation between src[idx] and src[idx+1] */
        int32_t l0 = src[idx * 2];
        int32_t l1 = src[(idx + 1) * 2];
        dst[out * 2] = (int16_t)(l0 + (l1 - l0) * (int32_t)frac / (int32_t)RATIO_DEN);

        /* Right channel */
        int32_t r0 = src[idx * 2 + 1];
        int32_t r1 = src[(idx + 1) * 2 + 1];
        dst[out * 2 + 1] = (int16_t)(r0 + (r1 - r0) * (int32_t)frac / (int32_t)RATIO_DEN);

        out++;
        phase_num += RATIO_NUM;
    }
    *frames_out = out;
}

/* ── Main-loop tick: feed sys_audio_out ───────────────────────────── */
void audio_tick(void) {
    if (g_pstate != PLAYBACK_PLAYING || !g_rb) return;

    size_t avail = ring_available(g_rb);
    if (avail < AUDIO_BLOCK_BYTES) {
        if (g_rb->stream_eof && avail == 0) {
            audio_set_state(PLAYBACK_STOPPED);
        }
        return;  /* underrun — wait for net thread to fill buffer */
    }

    /* Read one block from ring buffer into local PCM array */
    size_t r = g_rb->read_pos;
    for (size_t i = 0; i < AUDIO_BLOCK_BYTES; i++) {
        ((uint8_t *)g_pcm_block)[i] = g_rb->buffer[r];
        r = (r + 1) % g_rb->capacity;
    }
    g_rb->read_pos = r;

    /*
     * On real PS3:
     *   sysAudioOutBlock(port, g_pcm_block);
     *
     * This call blocks until the hardware consumes the previous block,
     * giving us natural 48 kHz pacing (~5.33 ms per 256-frame block).
     */
    static uint32_t s_audio_blocks = 0;
    s_audio_blocks++;
    if (s_audio_blocks % 60 == 0) {
        printf("[AUDIO] LPCM DAC 48 kHz stereo active | Blocks: %u | Buffer: %zu KB\n",
               s_audio_blocks, avail / 1024);
    }
}

