#ifndef COVER_ART_H
#define COVER_ART_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "monochrome.h"
#include "gfx_engine.h"

/* Número máximo de carátulas en el caché LRU */
#define MAX_COVER_CACHE 16

/* ── Entrada del caché LRU ─────────────────────────────────────── */
typedef struct {
    char          url[512];        /* URL de la carátula (clave del caché) */
    GfxTexture    tex_og;          /* Textura RSX modo Original (OG) */
    GfxTexture    tex_1bit;        /* Textura RSX modo 1-bit B/N (Bayer dither) */
    bool          has_og;          /* true si tex_og está cargada */
    bool          has_1bit;        /* true si tex_1bit está cargada */
    uint32_t      last_access;     /* Timestamp de último acceso (para LRU) */
    bool          ready;           /* true si al menos una versión está lista */
    int           width;
    int           height;
} CoverCacheEntry;

/* ── Inicialización y apagado ──────────────────────────────────── */
void cover_art_init(void);
void cover_art_shutdown(void);

/* ── Carga y modo ──────────────────────────────────────────────── */
bool cover_art_load(const char *url, CoverArtMode mode);
void cover_art_set_mode(CoverArtMode mode);
CoverArtMode cover_art_get_mode(void);

/* ── Acceso a textura para renderizado ─────────────────────────── */
/* Devuelve true si hay una textura lista para la URL en el modo actual */
bool cover_art_get_texture(const char *url, GfxTexture *out);

/* ── Gestión del caché ─────────────────────────────────────────── */
void cover_art_clear(const char *url);
void cover_art_clear_all(void);
bool cover_art_is_cached(const char *url);

#endif /* COVER_ART_H */
