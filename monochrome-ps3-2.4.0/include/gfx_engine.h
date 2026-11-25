#ifndef GFX_ENGINE_H
#define GFX_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Textura RSX/Tiny3D ────────────────────────────────────────── */
typedef struct {
    void    *data;      /* Datos de la textura en RAM (RGBA32) */
    uint32_t offset;    /* Offset de la textura en VRAM RSX */
    int      width;
    int      height;
    uint32_t pitch;     /* Pitch de la textura en bytes */
    bool     uploaded;  /* true si ya fue subida a VRAM */
} GfxTexture;

/* ── Inicialización y ciclo de frame ───────────────────────────── */
int  gfx_init(void);
void gfx_shutdown(void);
void gfx_begin_frame(uint32_t clear_rgba);
void gfx_end_frame(void);      /* tiny3d_Flip() */

/* ── Primitive 2D ──────────────────────────────────────────────── */
void gfx_draw_rect(float x, float y, float w, float h, uint32_t rgba);
void gfx_draw_rect_outline(float x, float y, float w, float h,
                           uint32_t rgba, float thickness);

/* ── Texturas y sprites ────────────────────────────────────────── */
int  gfx_load_texture_png(const char *path, GfxTexture *out);
int  gfx_upload_rgba_texture(const uint8_t *rgba, int w, int h, GfxTexture *out);
void gfx_draw_sprite(const GfxTexture *tex, float x, float y, float w, float h,
                     float u0, float v0, float u1, float v1, uint32_t tint_rgba);
void gfx_draw_9slice(const GfxTexture *tex, float x, float y, float w, float h,
                     float border_px, uint32_t tint_rgba);

/* ── Texto (FreeType atlas) ────────────────────────────────────── */
int  gfx_draw_text(float x, float y, const char *utf8, float size_px, uint32_t rgba);
int  gfx_draw_text_clipped(float x, float y, float max_w, const char *utf8,
                           float size_px, uint32_t rgba);

/* ── Visualizador 3D wireframe ─────────────────────────────────── */
void gfx_draw_wireframe_mesh(float cx, float cy, float scale,
                             const float *vis_bands, int num_bands,
                             float phase, uint32_t rgba);

/* ── Utilidades ────────────────────────────────────────────────── */
int  gfx_get_width(void);
int  gfx_get_height(void);
void gfx_set_overscan(int offset_x, int offset_y, float scale);
int  gfx_set_background_theme(int theme_id);
void gfx_set_background_dim(float brightness);
int  gfx_decode_png_memory(const uint8_t *buf, size_t len,
                           uint8_t **out_rgba, int *out_w, int *out_h);

#endif /* GFX_ENGINE_H */
