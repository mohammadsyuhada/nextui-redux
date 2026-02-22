#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "defines.h"
#include "resume.h"

#define RESUME_DIR SHARED_USERDATA_PATH "/music-player"
#define RESUME_FILE SHARED_USERDATA_PATH "/music-player/resume.cfg"

// In-memory state
static ResumeState state = {.type = RESUME_TYPE_NONE};
static char label_buf[300];

// Write state to disk
static void save_to_disk(void) {
	mkdir(RESUME_DIR, 0755);

	FILE* f = fopen(RESUME_FILE, "w");
	if (!f)
		return;

	fprintf(f, "type=%d\n", (int)state.type);
	fprintf(f, "folder_path=%s\n", state.folder_path);
	fprintf(f, "playlist_path=%s\n", state.playlist_path);
	fprintf(f, "track_path=%s\n", state.track_path);
	fprintf(f, "track_name=%s\n", state.track_name);
	fprintf(f, "track_index=%d\n", state.track_index);
	fprintf(f, "position_ms=%d\n", state.position_ms);
	fclose(f);
}

void Resume_init(void) {
	memset(&state, 0, sizeof(state));
	state.type = RESUME_TYPE_NONE;

	FILE* f = fopen(RESUME_FILE, "r");
	if (!f)
		return;

	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		// Strip newline
		char* nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';

		int ival;
		if (sscanf(line, "type=%d", &ival) == 1) {
			if (ival >= RESUME_TYPE_NONE && ival <= RESUME_TYPE_PLAYLIST)
				state.type = (ResumeType)ival;
		} else if (strncmp(line, "folder_path=", 12) == 0) {
			snprintf(state.folder_path, sizeof(state.folder_path), "%s", line + 12);
		} else if (strncmp(line, "playlist_path=", 14) == 0) {
			snprintf(state.playlist_path, sizeof(state.playlist_path), "%s", line + 14);
		} else if (strncmp(line, "track_path=", 11) == 0) {
			snprintf(state.track_path, sizeof(state.track_path), "%s", line + 11);
		} else if (strncmp(line, "track_name=", 11) == 0) {
			snprintf(state.track_name, sizeof(state.track_name), "%s", line + 11);
		} else if (sscanf(line, "track_index=%d", &ival) == 1) {
			state.track_index = ival;
		} else if (sscanf(line, "position_ms=%d", &ival) == 1) {
			state.position_ms = ival;
		}
	}
	fclose(f);

	// Validate: must have a track path
	if (state.type != RESUME_TYPE_NONE && state.track_path[0] == '\0') {
		state.type = RESUME_TYPE_NONE;
	}
}

bool Resume_isAvailable(void) {
	return state.type != RESUME_TYPE_NONE;
}

const ResumeState* Resume_getState(void) {
	if (state.type == RESUME_TYPE_NONE)
		return NULL;
	return &state;
}

const char* Resume_getLabel(void) {
	if (state.type == RESUME_TYPE_NONE)
		return NULL;

	if (state.track_name[0]) {
		snprintf(label_buf, sizeof(label_buf), "Resume: %s", state.track_name);
	} else {
		// Fallback: extract filename from track_path
		const char* slash = strrchr(state.track_path, '/');
		const char* name = slash ? slash + 1 : state.track_path;
		snprintf(label_buf, sizeof(label_buf), "Resume: %s", name);
	}
	return label_buf;
}

void Resume_saveFiles(const char* folder_path, const char* track_path,
					  const char* track_name, int track_index, int position_ms) {
	state.type = RESUME_TYPE_FILES;
	snprintf(state.folder_path, sizeof(state.folder_path), "%s", folder_path ? folder_path : "");
	state.playlist_path[0] = '\0';
	snprintf(state.track_path, sizeof(state.track_path), "%s", track_path ? track_path : "");
	snprintf(state.track_name, sizeof(state.track_name), "%s", track_name ? track_name : "");
	state.track_index = track_index;
	state.position_ms = position_ms;
	save_to_disk();
}

void Resume_savePlaylist(const char* playlist_path, const char* track_path,
						 const char* track_name, int track_index, int position_ms) {
	state.type = RESUME_TYPE_PLAYLIST;
	state.folder_path[0] = '\0';
	snprintf(state.playlist_path, sizeof(state.playlist_path), "%s", playlist_path ? playlist_path : "");
	snprintf(state.track_path, sizeof(state.track_path), "%s", track_path ? track_path : "");
	snprintf(state.track_name, sizeof(state.track_name), "%s", track_name ? track_name : "");
	state.track_index = track_index;
	state.position_ms = position_ms;
	save_to_disk();
}

void Resume_updatePosition(int position_ms) {
	if (state.type == RESUME_TYPE_NONE)
		return;
	state.position_ms = position_ms;
	save_to_disk();
}

void Resume_clear(void) {
	memset(&state, 0, sizeof(state));
	state.type = RESUME_TYPE_NONE;
	remove(RESUME_FILE);
}
