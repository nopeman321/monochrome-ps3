/**
 * Monochrome Music Player — Lossless FLAC Stream Decoder (dr_flac integration)
 * High-performance, zero-dependency C99 decoder for PC and Cell B.E. SPU/PPU.
 */

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "dr_flac.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "monochrome.h"

struct FlacDecoder {
    drflac   *pFlac;
    uint32_t  sample_rate;
    uint8_t   channels;
    uint64_t  total_frames;
};

FlacDecoder *flac_decoder_create(const uint8_t *flac_data, size_t flac_size) {
    if (!flac_data || flac_size < 4) return NULL;
    if (memcmp(flac_data, "fLaC", 4) != 0) return NULL;

    drflac *pFlac = drflac_open_memory(flac_data, flac_size, NULL);
    if (!pFlac) return NULL;

    FlacDecoder *dec = (FlacDecoder *)malloc(sizeof(FlacDecoder));
    if (!dec) {
        drflac_close(pFlac);
        return NULL;
    }

    dec->pFlac = pFlac;
    dec->sample_rate = pFlac->sampleRate;
    dec->channels = (uint8_t)pFlac->channels;
    dec->total_frames = pFlac->totalPCMFrameCount;
    return dec;
}

size_t flac_decoder_read_pcm16(FlacDecoder *dec, int16_t *pcm_out, size_t frames_to_read) {
    if (!dec || !dec->pFlac || !pcm_out || frames_to_read == 0) return 0;
    drflac_uint64 read = drflac_read_pcm_frames_s16(dec->pFlac, frames_to_read, pcm_out);
    return (size_t)read;
}

uint32_t flac_decoder_get_sample_rate(const FlacDecoder *dec) {
    return dec ? dec->sample_rate : 0;
}

uint8_t flac_decoder_get_channels(const FlacDecoder *dec) {
    return dec ? dec->channels : 0;
}

uint64_t flac_decoder_get_total_frames(const FlacDecoder *dec) {
    return dec ? dec->total_frames : 0;
}

void flac_decoder_destroy(FlacDecoder *dec) {
    if (!dec) return;
    if (dec->pFlac) drflac_close(dec->pFlac);
    free(dec);
}

int flac_decode_memory(const uint8_t *flac_data, size_t flac_size,
                       int16_t *pcm_out, size_t max_pcm_frames,
                       size_t *frames_decoded, uint32_t *sample_rate, uint8_t *channels) {
    if (!flac_data || flac_size < 4 || !pcm_out || !frames_decoded) {
        return -1;
    }

    /* Check FLAC magic 'fLaC' */
    if (memcmp(flac_data, "fLaC", 4) != 0) {
        return -2; /* Not a valid FLAC stream */
    }

    drflac *pFlac = drflac_open_memory(flac_data, flac_size, NULL);
    if (!pFlac) {
        return -3;
    }

    if (sample_rate) *sample_rate = pFlac->sampleRate;
    if (channels)    *channels    = (uint8_t)pFlac->channels;

    drflac_uint64 read_frames = drflac_read_pcm_frames_s16(pFlac, max_pcm_frames, pcm_out);
    *frames_decoded = (size_t)read_frames;

    drflac_close(pFlac);
    return 0;
}
