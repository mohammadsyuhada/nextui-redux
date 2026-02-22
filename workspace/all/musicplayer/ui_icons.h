#ifndef __UI_ICONS_H__
#define __UI_ICONS_H__

#include <SDL2/SDL.h>
#include <stdbool.h>
#include "player.h" // For AudioFormat

// Initialize icons (load from files and create inverted versions)
void Icons_init(void);

// Cleanup icons
void Icons_quit(void);

// Get icon for a file type (returns inverted or original based on selected state)
// selected=false -> white icon (inverted, for black background)
// selected=true -> black icon (original, for white/light selected background)
SDL_Surface* Icons_getFolder(bool selected);
SDL_Surface* Icons_getAudio(bool selected);
SDL_Surface* Icons_getPlayAll(bool selected);
SDL_Surface* Icons_getForFormat(AudioFormat format, bool selected);

// Podcast badge icons
SDL_Surface* Icons_getComplete(bool selected);
SDL_Surface* Icons_getDownload(bool selected);

// Empty state icon
SDL_Surface* Icons_getEmpty(bool selected);

// Check if icons are loaded
bool Icons_isLoaded(void);

#endif // __UI_ICONS_H__
