#ifndef UI_COMPONENTS_H
#define UI_COMPONENTS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "monochrome.h"
#include "app_state.h"
#include "gfx_engine.h"
#include "ui_theme.h"
#include "cover_art.h"

/* ── Component visual states ─────────────────────────────────────── */
typedef enum {
    UI_COMPONENT_STATE_DEFAULT = 0,
    UI_COMPONENT_STATE_FOCUSED,
    UI_COMPONENT_STATE_PLAYING,
} UiComponentState;

/* ── album_card_render ───────────────────────────────────────────── */
/* Tarjeta de álbum/item en coordenadas virtuales 1920x1080.           */
void album_card_render(float x, float y, float w, float h,
                       const AppTrack *track,
                       UiComponentState state);

/* ── track_row_render ────────────────────────────────────────────── */
/* Fila de track con índice, título, artista/álbum y duración.        */
void track_row_render(float x, float y, float w, float h,
                      uint32_t index, const AppTrack *track,
                      UiComponentState state);

#endif /* UI_COMPONENTS_H */
