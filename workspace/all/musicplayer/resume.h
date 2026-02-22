#ifndef __RESUME_H__
#define __RESUME_H__

#include <stdbool.h>

// Resume source types
typedef enum {
	RESUME_TYPE_NONE,
	RESUME_TYPE_FILES,
	RESUME_TYPE_PLAYLIST
} ResumeType;

// Resume state
typedef struct {
	ResumeType type;
	char folder_path[512];	 // For FILES: the directory path
	char playlist_path[512]; // For PLAYLIST: the .m3u path
	char track_path[512];	 // Currently playing track path
	char track_name[256];	 // Display name for menu label
	int track_index;		 // Index in folder/playlist
	int position_ms;		 // Playback position in milliseconds
} ResumeState;

// Initialize (loads from disk if available)
void Resume_init(void);

// Check if resume state is available
bool Resume_isAvailable(void);

// Get current resume state (read-only)
const ResumeState* Resume_getState(void);

// Get display label for menu (e.g. "Resume: Song Name")
const char* Resume_getLabel(void);

// Save resume state for files playback
void Resume_saveFiles(const char* folder_path, const char* track_path,
					  const char* track_name, int track_index, int position_ms);

// Save resume state for playlist playback
void Resume_savePlaylist(const char* playlist_path, const char* track_path,
						 const char* track_name, int track_index, int position_ms);

// Update just the position (called periodically during playback)
void Resume_updatePosition(int position_ms);

// Clear resume state (when playlist ends naturally)
void Resume_clear(void);

#endif
