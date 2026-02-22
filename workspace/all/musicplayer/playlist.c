#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "api.h"
#include "playlist.h"
#include "player.h"

// Forward declarations for internal helpers
static int scan_directory_recursive(PlaylistContext* ctx, const char* path, int depth);
static int compare_strings(const void* a, const void* b);

// Initialize playlist context
void Playlist_init(PlaylistContext* ctx) {
	if (!ctx)
		return;

	ctx->tracks = malloc(sizeof(PlaylistTrack) * PLAYLIST_MAX_TRACKS);
	ctx->track_count = 0;
	ctx->current_index = 0;
}

// Free playlist memory
void Playlist_free(PlaylistContext* ctx) {
	if (!ctx)
		return;

	if (ctx->tracks) {
		free(ctx->tracks);
		ctx->tracks = NULL;
	}
	ctx->track_count = 0;
	ctx->current_index = 0;
}

// Clear playlist (reset count but keep memory)
void Playlist_clear(PlaylistContext* ctx) {
	if (!ctx)
		return;
	ctx->track_count = 0;
	ctx->current_index = 0;
}

// Check if file is a supported audio format
static bool is_audio_file(const char* filename) {
	AudioFormat fmt = Player_detectFormat(filename);
	return fmt != AUDIO_FORMAT_UNKNOWN;
}

// String comparison for qsort (case-insensitive)
static int compare_strings(const void* a, const void* b) {
	return strcasecmp(*(const char**)a, *(const char**)b);
}

// Add a track to the playlist
static int add_track(PlaylistContext* ctx, const char* path, const char* name) {
	if (ctx->track_count >= PLAYLIST_MAX_TRACKS) {
		return -1; // Playlist full
	}

	PlaylistTrack* track = &ctx->tracks[ctx->track_count];
	strncpy(track->path, path, sizeof(track->path) - 1);
	track->path[sizeof(track->path) - 1] = '\0';
	strncpy(track->name, name, sizeof(track->name) - 1);
	track->name[sizeof(track->name) - 1] = '\0';
	track->format = Player_detectFormat(name);

	ctx->track_count++;
	return 0;
}

// Scan a directory and add audio files, then recurse into subdirectories
// This is used for subdirectories (not the starting directory)
static int scan_directory_recursive(PlaylistContext* ctx, const char* path, int depth) {
	if (depth > PLAYLIST_MAX_DEPTH) {
		return 0; // Prevent stack overflow
	}

	DIR* dir = opendir(path);
	if (!dir) {
		return 0;
	}

	// Collect file names and directory names separately
	char** files = NULL;
	char** dirs = NULL;
	int file_count = 0;
	int dir_count = 0;
	int files_capacity = 64;
	int dirs_capacity = 32;

	files = malloc(sizeof(char*) * files_capacity);
	dirs = malloc(sizeof(char*) * dirs_capacity);

	if (!files || !dirs) {
		if (files)
			free(files);
		if (dirs)
			free(dirs);
		closedir(dir);
		return 0;
	}

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		// Skip hidden files and . / ..
		if (ent->d_name[0] == '.')
			continue;

		char full_path[512];
		snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);

		struct stat st;
		if (lstat(full_path, &st) != 0)
			continue; // Use lstat to detect symlinks

		// Skip symlinks to prevent infinite loops
		if (S_ISLNK(st.st_mode))
			continue;

		if (S_ISDIR(st.st_mode)) {
			// Add directory name
			if (dir_count >= dirs_capacity) {
				dirs_capacity *= 2;
				char** new_dirs = realloc(dirs, sizeof(char*) * dirs_capacity);
				if (!new_dirs)
					continue;
				dirs = new_dirs;
			}
			dirs[dir_count] = strdup(ent->d_name);
			if (dirs[dir_count])
				dir_count++;
		} else if (is_audio_file(ent->d_name)) {
			// Add audio file name
			if (file_count >= files_capacity) {
				files_capacity *= 2;
				char** new_files = realloc(files, sizeof(char*) * files_capacity);
				if (!new_files)
					continue;
				files = new_files;
			}
			files[file_count] = strdup(ent->d_name);
			if (files[file_count])
				file_count++;
		}
	}
	closedir(dir);

	// Sort files and directories alphabetically
	if (file_count > 1) {
		qsort(files, file_count, sizeof(char*), compare_strings);
	}
	if (dir_count > 1) {
		qsort(dirs, dir_count, sizeof(char*), compare_strings);
	}

	int added = 0;

	// Add all audio files first
	for (int i = 0; i < file_count && ctx->track_count < PLAYLIST_MAX_TRACKS; i++) {
		char full_path[512];
		snprintf(full_path, sizeof(full_path), "%s/%s", path, files[i]);
		if (add_track(ctx, full_path, files[i]) == 0) {
			added++;
		}
	}

	// Then recurse into subdirectories
	for (int i = 0; i < dir_count && ctx->track_count < PLAYLIST_MAX_TRACKS; i++) {
		char full_path[512];
		snprintf(full_path, sizeof(full_path), "%s/%s", path, dirs[i]);
		added += scan_directory_recursive(ctx, full_path, depth + 1);
	}

	// Free memory
	for (int i = 0; i < file_count; i++)
		free(files[i]);
	for (int i = 0; i < dir_count; i++)
		free(dirs[i]);
	free(files);
	free(dirs);

	return added;
}

// Build playlist from a directory recursively
// Order: selected → files after → files before → subdirectories
// If start_track_path is NULL or empty, starts from first track
int Playlist_buildFromDirectory(PlaylistContext* ctx, const char* path, const char* start_track_path) {
	if (!ctx || !path)
		return -1;

	// Ensure playlist memory is allocated
	if (!ctx->tracks) {
		Playlist_init(ctx);
		if (!ctx->tracks)
			return -1;
	}

	Playlist_clear(ctx);

	DIR* dir = opendir(path);
	if (!dir) {
		LOG_error("Failed to open directory: %s\n", path);
		return -1;
	}

	// Collect file names and directory names
	char** files = NULL;
	char** dirs = NULL;
	int file_count = 0;
	int dir_count = 0;
	int files_capacity = 64;
	int dirs_capacity = 32;

	files = malloc(sizeof(char*) * files_capacity);
	dirs = malloc(sizeof(char*) * dirs_capacity);

	if (!files || !dirs) {
		if (files)
			free(files);
		if (dirs)
			free(dirs);
		closedir(dir);
		return -1;
	}

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		char full_path[512];
		snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);

		struct stat st;
		if (lstat(full_path, &st) != 0)
			continue;

		// Skip symlinks
		if (S_ISLNK(st.st_mode))
			continue;

		if (S_ISDIR(st.st_mode)) {
			if (dir_count >= dirs_capacity) {
				dirs_capacity *= 2;
				char** new_dirs = realloc(dirs, sizeof(char*) * dirs_capacity);
				if (!new_dirs)
					continue;
				dirs = new_dirs;
			}
			dirs[dir_count] = strdup(ent->d_name);
			if (dirs[dir_count])
				dir_count++;
		} else if (is_audio_file(ent->d_name)) {
			if (file_count >= files_capacity) {
				files_capacity *= 2;
				char** new_files = realloc(files, sizeof(char*) * files_capacity);
				if (!new_files)
					continue;
				files = new_files;
			}
			files[file_count] = strdup(ent->d_name);
			if (files[file_count])
				file_count++;
		}
	}
	closedir(dir);

	// Sort files and directories
	if (file_count > 1) {
		qsort(files, file_count, sizeof(char*), compare_strings);
	}
	if (dir_count > 1) {
		qsort(dirs, dir_count, sizeof(char*), compare_strings);
	}

	// Find the index of the selected track in the sorted files list
	int selected_idx = -1;
	if (start_track_path && start_track_path[0] != '\0') {
		for (int i = 0; i < file_count; i++) {
			char full_path[512];
			snprintf(full_path, sizeof(full_path), "%s/%s", path, files[i]);
			if (strcmp(full_path, start_track_path) == 0) {
				selected_idx = i;
				break;
			}
		}
	}

	// If start track not found or not specified, start from beginning
	if (selected_idx < 0) {
		selected_idx = 0;
	}

	// Add files in order: selected → after → before
	// First: selected track
	if (file_count > 0 && selected_idx < file_count) {
		char full_path[512];
		snprintf(full_path, sizeof(full_path), "%s/%s", path, files[selected_idx]);
		add_track(ctx, full_path, files[selected_idx]);
	}

	// Then: files after selected
	for (int i = selected_idx + 1; i < file_count && ctx->track_count < PLAYLIST_MAX_TRACKS; i++) {
		char full_path[512];
		snprintf(full_path, sizeof(full_path), "%s/%s", path, files[i]);
		add_track(ctx, full_path, files[i]);
	}

	// Then: files before selected
	for (int i = 0; i < selected_idx && ctx->track_count < PLAYLIST_MAX_TRACKS; i++) {
		char full_path[512];
		snprintf(full_path, sizeof(full_path), "%s/%s", path, files[i]);
		add_track(ctx, full_path, files[i]);
	}

	// Then: recurse into subdirectories
	for (int i = 0; i < dir_count && ctx->track_count < PLAYLIST_MAX_TRACKS; i++) {
		char full_path[512];
		snprintf(full_path, sizeof(full_path), "%s/%s", path, dirs[i]);
		scan_directory_recursive(ctx, full_path, 1);
	}

	// Free memory
	for (int i = 0; i < file_count; i++)
		free(files[i]);
	for (int i = 0; i < dir_count; i++)
		free(dirs[i]);
	free(files);
	free(dirs);

	// Current index is always 0 (the selected track)
	ctx->current_index = 0;

	return ctx->track_count;
}

// Navigation - next track (no wrap-around)
int Playlist_next(PlaylistContext* ctx) {
	if (!ctx || ctx->track_count == 0)
		return -1;
	if (ctx->current_index >= ctx->track_count - 1)
		return -1; // End of playlist

	ctx->current_index++;
	return ctx->current_index;
}

// Navigation - previous track (no wrap-around)
int Playlist_prev(PlaylistContext* ctx) {
	if (!ctx || ctx->track_count == 0)
		return -1;
	if (ctx->current_index <= 0)
		return -1; // Start of playlist

	ctx->current_index--;
	return ctx->current_index;
}

// Shuffle - pick a random track (different from current if possible)
int Playlist_shuffle(PlaylistContext* ctx) {
	if (!ctx || ctx->track_count == 0)
		return -1;
	if (ctx->track_count == 1)
		return 0; // Only one track

	// Pick a random track different from current
	int new_idx;
	do {
		new_idx = rand() % ctx->track_count;
	} while (new_idx == ctx->current_index && ctx->track_count > 1);

	ctx->current_index = new_idx;
	return ctx->current_index;
}

// Set current track by index
int Playlist_setCurrentIndex(PlaylistContext* ctx, int index) {
	if (!ctx || index < 0 || index >= ctx->track_count)
		return -1;
	ctx->current_index = index;
	return 0;
}

// Get current track
const PlaylistTrack* Playlist_getCurrentTrack(const PlaylistContext* ctx) {
	if (!ctx || !ctx->tracks || ctx->track_count == 0)
		return NULL;
	if (ctx->current_index < 0 || ctx->current_index >= ctx->track_count)
		return NULL;
	return &ctx->tracks[ctx->current_index];
}

// Get track by index
const PlaylistTrack* Playlist_getTrack(const PlaylistContext* ctx, int index) {
	if (!ctx || !ctx->tracks || index < 0 || index >= ctx->track_count)
		return NULL;
	return &ctx->tracks[index];
}

// Get track count
int Playlist_getCount(const PlaylistContext* ctx) {
	if (!ctx)
		return 0;
	return ctx->track_count;
}

// Get current index
int Playlist_getCurrentIndex(const PlaylistContext* ctx) {
	if (!ctx)
		return 0;
	return ctx->current_index;
}

// Check if playlist is active (has tracks)
bool Playlist_isActive(const PlaylistContext* ctx) {
	return ctx && ctx->tracks && ctx->track_count > 0;
}
