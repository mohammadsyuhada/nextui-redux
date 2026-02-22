#ifndef __UI_ALBUM_ART_H__
#define __UI_ALBUM_ART_H__

#include <SDL2/SDL.h>

// Render album art as triangular background with fade effect
// The background is cached internally for performance
void render_album_art_background(SDL_Surface* screen, SDL_Surface* album_art);

// Cleanup cached background surface (call on exit or when switching tracks)
void cleanup_album_art_background(void);

#endif
