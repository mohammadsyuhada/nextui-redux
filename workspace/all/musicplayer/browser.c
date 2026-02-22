#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "api.h"
#include "browser.h"

// Check if file is a supported audio format
bool Browser_isAudioFile(const char* filename) {
	AudioFormat fmt = Player_detectFormat(filename);
	return fmt != AUDIO_FORMAT_UNKNOWN;
}

// Free browser entries
void Browser_freeEntries(BrowserContext* ctx) {
	if (ctx->entries) {
		free(ctx->entries);
		ctx->entries = NULL;
	}
	ctx->entry_count = 0;
}

// Compare function for sorting entries (directories first, then alphabetical)
static int compare_entries(const void* a, const void* b) {
	const FileEntry* ea = (const FileEntry*)a;
	const FileEntry* eb = (const FileEntry*)b;

	// Directories come first
	if (ea->is_dir && !eb->is_dir)
		return -1;
	if (!ea->is_dir && eb->is_dir)
		return 1;

	// Alphabetical
	return strcasecmp(ea->name, eb->name);
}

// Load directory contents
void Browser_loadDirectory(BrowserContext* ctx, const char* path, const char* music_root) {
	Browser_freeEntries(ctx);

	strncpy(ctx->current_path, path, sizeof(ctx->current_path) - 1);
	ctx->selected = 0;
	ctx->scroll_offset = 0;

	// Create music folder if it doesn't exist
	if (strcmp(path, music_root) == 0) {
		mkdir(path, 0755);
	}

	DIR* dir = opendir(path);
	if (!dir) {
		LOG_error("Failed to open directory: %s\n", path);
		return;
	}

	// First pass: count entries (separate count for dirs and audio files)
	int dir_count = 0;
	int audio_count = 0;
	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue; // Skip hidden files

		char full_path[1024]; // Increased to handle longer paths
		int path_len = snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);
		if (path_len < 0 || path_len >= (int)sizeof(full_path)) {
			continue; // Path too long, skip this entry
		}

		struct stat st;
		if (stat(full_path, &st) != 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			dir_count++;
		} else if (Browser_isAudioFile(ent->d_name)) {
			audio_count++;
		}
	}

	int count = dir_count + audio_count;

	// Add parent directory entry if not at root
	bool has_parent = (strcmp(path, music_root) != 0);
	if (has_parent)
		count++;

	// Add "Play All" entry if there are subdirectories
	bool add_play_all = (dir_count > 0);
	if (add_play_all)
		count++;

	// Allocate
	ctx->entries = malloc(sizeof(FileEntry) * count);
	if (!ctx->entries) {
		closedir(dir);
		return;
	}

	int idx = 0;

	// Add parent directory
	if (has_parent) {
		strncpy(ctx->entries[idx].name, "..", sizeof(ctx->entries[idx].name) - 1);
		ctx->entries[idx].name[sizeof(ctx->entries[idx].name) - 1] = '\0';
		char* last_slash = strrchr(ctx->current_path, '/');
		if (last_slash) {
			strncpy(ctx->entries[idx].path, ctx->current_path, last_slash - ctx->current_path);
			ctx->entries[idx].path[last_slash - ctx->current_path] = '\0';
		} else {
			strncpy(ctx->entries[idx].path, music_root, sizeof(ctx->entries[idx].path) - 1);
		}
		ctx->entries[idx].is_dir = true;
		ctx->entries[idx].is_play_all = false;
		ctx->entries[idx].format = AUDIO_FORMAT_UNKNOWN;
		idx++;
	}

	// Second pass: fill entries
	rewinddir(dir);
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		char full_path[1024]; // Increased to handle longer paths
		int path_len = snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);
		if (path_len < 0 || path_len >= (int)sizeof(full_path)) {
			continue; // Path too long, skip this entry
		}

		struct stat st;
		if (stat(full_path, &st) != 0)
			continue;

		bool is_dir = S_ISDIR(st.st_mode);
		AudioFormat fmt = AUDIO_FORMAT_UNKNOWN;

		if (!is_dir) {
			fmt = Player_detectFormat(ent->d_name);
			if (fmt == AUDIO_FORMAT_UNKNOWN)
				continue;
		}

		strncpy(ctx->entries[idx].name, ent->d_name, sizeof(ctx->entries[idx].name) - 1);
		strncpy(ctx->entries[idx].path, full_path, sizeof(ctx->entries[idx].path) - 1);
		ctx->entries[idx].is_dir = is_dir;
		ctx->entries[idx].is_play_all = false;
		ctx->entries[idx].format = fmt;
		idx++;
	}

	closedir(dir);

	// Sort entries (but keep ".." at top if present)
	int sort_start = has_parent ? 1 : 0;
	if (idx > sort_start + 1) {
		qsort(&ctx->entries[sort_start], idx - sort_start,
			  sizeof(FileEntry), compare_entries);
	}

	// Add "Play All" entry at the end if applicable
	if (add_play_all) {
		strncpy(ctx->entries[idx].name, "Play All", sizeof(ctx->entries[idx].name) - 1);
		ctx->entries[idx].name[sizeof(ctx->entries[idx].name) - 1] = '\0';
		strncpy(ctx->entries[idx].path, path, sizeof(ctx->entries[idx].path) - 1);
		ctx->entries[idx].path[sizeof(ctx->entries[idx].path) - 1] = '\0';
		ctx->entries[idx].is_dir = false;
		ctx->entries[idx].is_play_all = true;
		ctx->entries[idx].format = AUDIO_FORMAT_UNKNOWN;
		idx++;
	}

	ctx->entry_count = idx;
}

// Get display name for file (without extension)
void Browser_getDisplayName(const char* filename, char* out, int max_len) {
	strncpy(out, filename, max_len - 1);
	out[max_len - 1] = '\0';

	// Remove extension for audio files
	char* dot = strrchr(out, '.');
	if (dot && dot != out) {
		*dot = '\0';
	}
}

// Count audio files in browser for "X OF Y" display
int Browser_countAudioFiles(const BrowserContext* ctx) {
	int count = 0;
	for (int i = 0; i < ctx->entry_count; i++) {
		if (!ctx->entries[i].is_dir && !ctx->entries[i].is_play_all)
			count++;
	}
	return count;
}

// Get current track number (1-based)
int Browser_getCurrentTrackNumber(const BrowserContext* ctx) {
	int num = 0;
	for (int i = 0; i <= ctx->selected && i < ctx->entry_count; i++) {
		if (!ctx->entries[i].is_dir)
			num++;
	}
	return num;
}

// Check if browser has a parent entry (..) — i.e., not at root
bool Browser_hasParent(const BrowserContext* ctx) {
	return ctx->entry_count > 0 && strcmp(ctx->entries[0].name, "..") == 0;
}

// Recursively check if any audio files exist under a directory (max 3 levels deep)
static bool has_audio_recursive(const char* path, int depth) {
	if (depth > 3)
		return false;

	DIR* dir = opendir(path);
	if (!dir)
		return false;

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		char full_path[1024];
		if (snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name) >= (int)sizeof(full_path))
			continue;

		struct stat st;
		if (stat(full_path, &st) != 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			if (has_audio_recursive(full_path, depth + 1)) {
				closedir(dir);
				return true;
			}
		} else if (Browser_isAudioFile(ent->d_name)) {
			closedir(dir);
			return true;
		}
	}

	closedir(dir);
	return false;
}

bool Browser_hasAudioRecursive(const char* path) {
	return has_audio_recursive(path, 0);
}
