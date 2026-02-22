#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "defines.h"
#include "playlist_m3u.h"
#include "player.h"

void M3U_init(void) {
	mkdir(SHARED_USERDATA_PATH "/music-player", 0755);
	mkdir(PLAYLISTS_DIR, 0755);
}

// Count non-comment, non-empty lines in an m3u file (= track count)
static int count_tracks_in_file(const char* path) {
	FILE* f = fopen(path, "r");
	if (!f)
		return 0;

	int count = 0;
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		// Trim trailing newline
		int len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (len == 0 || line[0] == '#')
			continue;
		count++;
	}
	fclose(f);
	return count;
}

int M3U_listPlaylists(PlaylistInfo* out, int max) {
	DIR* dir = opendir(PLAYLISTS_DIR);
	if (!dir)
		return 0;

	int count = 0;
	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL && count < max) {
		if (ent->d_name[0] == '.')
			continue;

		// Only .m3u files
		int len = strlen(ent->d_name);
		if (len < 5 || strcasecmp(ent->d_name + len - 4, ".m3u") != 0)
			continue;

		PlaylistInfo* info = &out[count];
		snprintf(info->path, sizeof(info->path), "%s/%s", PLAYLISTS_DIR, ent->d_name);

		// Name without .m3u extension
		snprintf(info->name, sizeof(info->name), "%.*s", len - 4, ent->d_name);

		info->track_count = count_tracks_in_file(info->path);
		count++;
	}
	closedir(dir);

	// Sort by name (case-insensitive)
	for (int i = 0; i < count - 1; i++) {
		for (int j = i + 1; j < count; j++) {
			if (strcasecmp(out[i].name, out[j].name) > 0) {
				PlaylistInfo tmp = out[i];
				out[i] = out[j];
				out[j] = tmp;
			}
		}
	}

	return count;
}

int M3U_create(const char* name) {
	if (!name || !name[0])
		return -1;

	M3U_init();

	char path[512];
	snprintf(path, sizeof(path), "%s/%s.m3u", PLAYLISTS_DIR, name);

	// Don't overwrite existing
	if (access(path, F_OK) == 0)
		return -1;

	FILE* f = fopen(path, "w");
	if (!f)
		return -1;

	fprintf(f, "#EXTM3U\n");
	fclose(f);
	return 0;
}

int M3U_delete(const char* m3u_path) {
	if (!m3u_path)
		return -1;
	return unlink(m3u_path);
}

int M3U_addTrack(const char* m3u_path, const char* track_path, const char* display_name) {
	if (!m3u_path || !track_path)
		return -1;

	// Don't add duplicates
	if (M3U_containsTrack(m3u_path, track_path))
		return -1;

	FILE* f = fopen(m3u_path, "a");
	if (!f)
		return -1;

	const char* name = display_name ? display_name : track_path;
	fprintf(f, "#EXTINF:0,%s\n%s\n", name, track_path);
	fclose(f);
	return 0;
}

int M3U_removeTrack(const char* m3u_path, int index) {
	if (!m3u_path || index < 0)
		return -1;

	FILE* f = fopen(m3u_path, "r");
	if (!f)
		return -1;

	// Read all lines
	char** lines = NULL;
	int line_count = 0;
	int capacity = 64;
	lines = malloc(sizeof(char*) * capacity);
	if (!lines) {
		fclose(f);
		return -1;
	}

	char buf[1024];
	while (fgets(buf, sizeof(buf), f)) {
		if (line_count >= capacity) {
			capacity *= 2;
			char** new_lines = realloc(lines, sizeof(char*) * capacity);
			if (!new_lines) {
				for (int i = 0; i < line_count; i++)
					free(lines[i]);
				free(lines);
				fclose(f);
				return -1;
			}
			lines = new_lines;
		}
		lines[line_count] = strdup(buf);
		if (lines[line_count])
			line_count++;
	}
	fclose(f);

	// Find the track line at the given index (skip # and empty lines)
	int track_idx = 0;
	int remove_line = -1;
	for (int i = 0; i < line_count; i++) {
		char* line = lines[i];
		int len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			len--;
		if (len == 0 || line[0] == '#')
			continue;

		if (track_idx == index) {
			remove_line = i;
			break;
		}
		track_idx++;
	}

	if (remove_line < 0) {
		for (int i = 0; i < line_count; i++)
			free(lines[i]);
		free(lines);
		return -1;
	}

	// Rewrite the file without the removed track (and its preceding #EXTINF line)
	f = fopen(m3u_path, "w");
	if (!f) {
		for (int i = 0; i < line_count; i++)
			free(lines[i]);
		free(lines);
		return -1;
	}

	for (int i = 0; i < line_count; i++) {
		// Skip the track path line
		if (i == remove_line)
			continue;
		// Skip the #EXTINF line right before the track
		if (i == remove_line - 1 && lines[i][0] == '#' &&
			strncmp(lines[i], "#EXTINF", 7) == 0)
			continue;
		fputs(lines[i], f);
	}
	fclose(f);

	for (int i = 0; i < line_count; i++)
		free(lines[i]);
	free(lines);
	return 0;
}

int M3U_loadTracks(const char* m3u_path, PlaylistTrack* tracks, int max, int* count) {
	if (!m3u_path || !tracks || !count)
		return -1;
	*count = 0;

	FILE* f = fopen(m3u_path, "r");
	if (!f)
		return -1;

	char line[1024];
	char last_extinf_name[256] = "";

	while (fgets(line, sizeof(line), f) && *count < max) {
		// Trim trailing newline
		int len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';

		if (len == 0)
			continue;

		// Parse #EXTINF for display name
		if (strncmp(line, "#EXTINF:", 8) == 0) {
			char* comma = strchr(line + 8, ',');
			if (comma) {
				snprintf(last_extinf_name, sizeof(last_extinf_name), "%s", comma + 1);
			}
			continue;
		}

		// Skip other comment lines
		if (line[0] == '#')
			continue;

		// This is a track path — validate it exists
		if (access(line, F_OK) != 0) {
			last_extinf_name[0] = '\0';
			continue;
		}

		PlaylistTrack* track = &tracks[*count];
		snprintf(track->path, sizeof(track->path), "%s", line);

		// Use EXTINF name if available, otherwise extract from filename
		if (last_extinf_name[0]) {
			snprintf(track->name, sizeof(track->name), "%s", last_extinf_name);
		} else {
			// Extract filename from path
			const char* slash = strrchr(line, '/');
			const char* fname = slash ? slash + 1 : line;
			snprintf(track->name, sizeof(track->name), "%s", fname);
		}

		track->format = Player_detectFormat(line);
		last_extinf_name[0] = '\0';
		(*count)++;
	}

	fclose(f);
	return 0;
}

bool M3U_containsTrack(const char* m3u_path, const char* track_path) {
	if (!m3u_path || !track_path)
		return false;

	FILE* f = fopen(m3u_path, "r");
	if (!f)
		return false;

	char line[1024];
	bool found = false;
	while (fgets(line, sizeof(line), f)) {
		int len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (len == 0 || line[0] == '#')
			continue;

		if (strcmp(line, track_path) == 0) {
			found = true;
			break;
		}
	}

	fclose(f);
	return found;
}
