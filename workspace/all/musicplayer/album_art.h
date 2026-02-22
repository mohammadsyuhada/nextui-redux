#ifndef __ALBUM_ART_H__
#define __ALBUM_ART_H__

#include <stdbool.h>

// Forward declaration for SDL_Surface
struct SDL_Surface;

// Initialize album art module
void album_art_init(void);

// Cleanup album art module
void album_art_cleanup(void);

// Fetch album art for artist/title (async, non-blocking)
// Call this when metadata changes
void album_art_fetch(const char* artist, const char* title);

// Get current album art surface (NULL if none or still fetching)
struct SDL_Surface* album_art_get(void);

// Check if a fetch is in progress
bool album_art_is_fetching(void);

// Clear current album art and reset state
void album_art_clear(void);

// Get the total size of the album art disk cache in bytes
long album_art_get_cache_size(void);

// Clear all cached album art from disk
void album_art_clear_disk_cache(void);

#endif
