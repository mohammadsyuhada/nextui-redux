#ifndef __ADD_TO_PLAYLIST_H__
#define __ADD_TO_PLAYLIST_H__

#include <stdbool.h>
#include <SDL2/SDL.h>

// Open the add-to-playlist dialog for a track
void AddToPlaylist_open(const char* track_path, const char* display_name);

// Check if the dialog is currently active
bool AddToPlaylist_isActive(void);

// Handle input for the dialog.
// Returns: 0 = still active, 1 = done (added or cancelled)
int AddToPlaylist_handleInput(void);

// Render the dialog overlay
void AddToPlaylist_render(SDL_Surface* screen);

// Toast accessors (for callers to show toast after dialog closes)
const char* AddToPlaylist_getToastMessage(void);
uint32_t AddToPlaylist_getToastTime(void);
void AddToPlaylist_clearToast(void);

#endif
