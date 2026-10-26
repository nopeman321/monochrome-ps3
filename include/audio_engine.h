#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

int  audio_engine_init(void);
void audio_engine_shutdown(void);

void audio_engine_play_track(const char *track_id, const char *title);
void audio_engine_play(int track_index);
void audio_engine_pause(void);
void audio_engine_resume(void);
bool audio_engine_is_playing(void);

void  audio_engine_set_volume(float vol);
float audio_engine_get_volume(void);

float audio_engine_get_progress(void);
void  audio_engine_seek(float progress_0_to_1);

void audio_engine_get_vis(float *bands, int num_bands);
uint32_t audio_engine_get_underruns(void);

const char *audio_engine_get_status_message(void);
bool audio_engine_has_error(void);

#endif /* AUDIO_ENGINE_H */
