#ifndef __PLAYLIST_H__
#define __PLAYLIST_H__

#include <stdbool.h>
#include "player.h" // For AudioFormat

#define PLAYLIST_MAX_TRACKS 500 // Maximum tracks in playlist (~380KB memory)
#define PLAYLIST_MAX_DEPTH 10	// Maximum recursion depth for directory scanning

// Single track in the playlist
typedef struct {
	char path[512];
	char name[256];
	AudioFormat format;
} PlaylistTrack;

// Playlist context
typedef struct {
	PlaylistTrack* tracks; // Dynamically allocated array of tracks
	int track_count;	   // Number of tracks in playlist
	int current_index;	   // Currently playing track index (0-based)
} PlaylistContext;

// Initialize playlist context (allocates memory)
void Playlist_init(PlaylistContext* ctx);

// Free playlist memory
void Playlist_free(PlaylistContext* ctx);

// Clear playlist (reset count but keep memory)
void Playlist_clear(PlaylistContext* ctx);

// Build playlist from a directory recursively
// - path: directory to scan
// - start_track_path: path of the track to start from (will be at index 0)
// Returns: number of tracks added, or -1 on error
int Playlist_buildFromDirectory(PlaylistContext* ctx, const char* path, const char* start_track_path);

// Navigation (no wrap-around)
// Returns: new index, or -1 if at end/start
int Playlist_next(PlaylistContext* ctx);
int Playlist_prev(PlaylistContext* ctx);

// Shuffle - pick a random track
// Returns: new index, or -1 if empty
int Playlist_shuffle(PlaylistContext* ctx);

// Set current track by index
// Returns: 0 on success, -1 if invalid index
int Playlist_setCurrentIndex(PlaylistContext* ctx, int index);

// Accessors
const PlaylistTrack* Playlist_getCurrentTrack(const PlaylistContext* ctx);
const PlaylistTrack* Playlist_getTrack(const PlaylistContext* ctx, int index);
int Playlist_getCount(const PlaylistContext* ctx);
int Playlist_getCurrentIndex(const PlaylistContext* ctx);

// Check if playlist is valid/active
bool Playlist_isActive(const PlaylistContext* ctx);

#endif // __PLAYLIST_H__
