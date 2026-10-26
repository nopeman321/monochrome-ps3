#ifndef COVER_ART_H
#define COVER_ART_H

#include <stdint.h>
#include <stdbool.h>

// Initialize cover art system
void cover_art_init(void);

// Shutdown cover art system
void cover_art_shutdown(void);

// Download and decode cover art from URL
// Returns true on success, false on error
bool cover_art_load(const char *url);

// Check if cover art is loaded and ready
bool cover_art_is_ready(void);

// Get cover art texture pointer for rendering (NULL if not ready)
void *cover_art_get_texture(void);

// Get cover art dimensions
int cover_art_get_width(void);
int cover_art_get_height(void);

// Clear current cover art
void cover_art_clear(void);

#endif // COVER_ART_H
