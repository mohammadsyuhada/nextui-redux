#ifndef __BROWSER_H__
#define __BROWSER_H__

#include <stdbool.h>
#include "player.h" // For AudioFormat

// File entry structure
typedef struct {
	char name[256];
	char path[512];
	bool is_dir;
	bool is_play_all; // Special "Play All" entry for folders with only subfolders
	AudioFormat format;
} FileEntry;

// Browser context structure
typedef struct {
	char current_path[512];
	FileEntry* entries;
	int entry_count;
	int selected;
	int scroll_offset;
	int items_per_page;
} BrowserContext;

// Free browser entries
void Browser_freeEntries(BrowserContext* ctx);

// Load directory contents
void Browser_loadDirectory(BrowserContext* ctx, const char* path, const char* music_root);

// Get display name for file (without extension)
void Browser_getDisplayName(const char* filename, char* out, int max_len);

// Count audio files in browser
int Browser_countAudioFiles(const BrowserContext* ctx);

// Get current track number (1-based)
int Browser_getCurrentTrackNumber(const BrowserContext* ctx);

// Check if file is a supported audio format
bool Browser_isAudioFile(const char* filename);

// Check if browser has a parent entry (..) — i.e., not at root
bool Browser_hasParent(const BrowserContext* ctx);

// Recursively check if any audio files exist under a directory
bool Browser_hasAudioRecursive(const char* path);

#endif
