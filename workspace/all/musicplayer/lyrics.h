#ifndef __LYRICS_H__
#define __LYRICS_H__

#include <stdbool.h>

// A single timestamped lyric line
typedef struct {
	int time_ms;	// Timestamp in milliseconds
	char text[256]; // Lyric text
} LyricLine;

// Maximum number of lyric lines
#define LYRICS_MAX_LINES 512

// Initialize lyrics module
void Lyrics_init(void);

// Cleanup lyrics module
void Lyrics_cleanup(void);

// Fetch lyrics for artist/title (non-blocking, runs in background thread)
void Lyrics_fetch(const char* artist, const char* title, int duration_sec);

// Clear current lyrics and reset state
void Lyrics_clear(void);

// Get the current lyric line for the given playback position
// Returns pointer to lyric text, or NULL if no lyrics available/still fetching
const char* Lyrics_getCurrentLine(int position_ms);

// Get the next lyric line after the current one (call after Lyrics_getCurrentLine)
// Returns pointer to next lyric text, or NULL if no next line
const char* Lyrics_getNextLine(void);

// Check if lyrics are available for the current track
bool Lyrics_isAvailable(void);

// Get total size of lyrics cache on disk (in bytes)
long Lyrics_getCacheSize(void);

// Clear all cached lyrics files from disk
void Lyrics_clearCache(void);

#endif
