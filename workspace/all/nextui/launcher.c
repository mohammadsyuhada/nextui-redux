#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <limits.h>
#include <spawn.h>
#include <sys/wait.h>
#include "defines.h"
#include "api.h"
#include "utils.h"
#include "config.h"
#include "types.h"
#include "recents.h"
#include "content.h"
#include "launcher.h"
#include "shortcuts.h"

static CleanupPoolFunc _cleanupPool = NULL;

void Launcher_setCleanupFunc(CleanupPoolFunc func) {
	_cleanupPool = func;
}

///////////////////////////////////////

void queueNext(char* cmd) {
	LOG_info("cmd: %s\n", cmd);
	putFile("/tmp/next", cmd);
	quit = 1;
}

extern char** environ;
static int runCommand(const char* path, char* const argv[]) {
	pid_t pid;
	int status;
	if (posix_spawn(&pid, path, NULL, NULL, argv, environ) != 0) {
		return -1;
	}
	waitpid(pid, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

///////////////////////////////////////

void readyResumePath(char* rom_path, int type) {
	char* tmp;
	resume.can_resume = 0;
	resume.has_preview = 0;
	resume.has_boxart = 0;
	char path[MAX_PATH];
	strcpy(path, rom_path);

	if (!prefixMatch(ROMS_PATH, path))
		return;

	char auto_path[MAX_PATH];
	if (type == ENTRY_DIR) {
		if (!hasCue(path, auto_path)) {	   // no cue?
			tmp = strrchr(auto_path, '.'); // extension
			if (!tmp)
				return;
			strcpy(tmp + 1, "m3u"); // replace with m3u
			if (!exists(auto_path))
				return; // no m3u
		}
		strcpy(path, auto_path); // cue or m3u if one exists
	}

	if (!suffixMatch(".m3u", path)) {
		char m3u_path[MAX_PATH];
		if (hasM3u(path, m3u_path)) {
			// change path to m3u path
			strcpy(path, m3u_path);
		}
	}

	char emu_name[MAX_PATH];
	getEmuName(path, emu_name);

	char rom_file[MAX_PATH];
	tmp = strrchr(path, '/');
	if (!tmp)
		return;
	strcpy(rom_file, tmp + 1);

	snprintf(resume.slot_path, sizeof(resume.slot_path), "%s/.minui/%s/%s.txt", SHARED_USERDATA_PATH, emu_name, rom_file); // /.userdata/.minui/<EMU>/<romname>.ext.txt
	resume.can_resume = exists(resume.slot_path);

	// resume.slot_path contains a single integer representing the last used slot
	if (resume.can_resume) {
		char slot[16];
		getFile(resume.slot_path, slot, 16);
		int s = atoi(slot);
		snprintf(resume.preview_path, sizeof(resume.preview_path), "%s/.minui/%s/%s.%0d.bmp", SHARED_USERDATA_PATH, emu_name, rom_file, s); // /.userdata/.minui/<EMU>/<romname>.ext.<n>.bmp
		resume.has_preview = exists(resume.preview_path);
	}

	// Boxart fallback: if no savestate preview, check for boxart in .media folder
	if (!resume.has_preview) {
		char rom_dir[MAX_PATH];
		char rom_name[MAX_PATH];
		strcpy(rom_dir, rom_path);
		char* last_slash = strrchr(rom_dir, '/');
		if (last_slash) {
			*last_slash = '\0';				  // rom_dir now has directory
			strcpy(rom_name, last_slash + 1); // rom_name has filename with ext
			char* dot = strrchr(rom_name, '.');
			if (dot)
				*dot = '\0'; // remove extension
			snprintf(resume.boxart_path, sizeof(resume.boxart_path), "%s/.media/%s.png", rom_dir, rom_name);
			resume.has_boxart = exists(resume.boxart_path);

			// For multi-disk games in folders: if boxart not found, check parent folder
			// e.g., /Roms/PS1/GameFolder/game.m3u -> check /Roms/PS1/.media/GameFolder.png
			if (!resume.has_boxart) {
				char parent_dir[MAX_PATH];
				char folder_name[MAX_PATH];
				strcpy(parent_dir, rom_dir);
				char* parent_slash = strrchr(parent_dir, '/');
				if (parent_slash) {
					*parent_slash = '\0';				   // parent_dir now has grandparent directory
					strcpy(folder_name, parent_slash + 1); // folder_name has the game folder name
					snprintf(resume.boxart_path, sizeof(resume.boxart_path), "%s/.media/%s.png", parent_dir, folder_name);
					resume.has_boxart = exists(resume.boxart_path);
				}
			}
		}
	}
}
void readyResume(Entry* entry) {
	readyResumePath(entry->path, entry->type);
}

int autoResume(void) {
	// NOTE: bypasses recents

	if (!exists(AUTO_RESUME_PATH))
		return 0;

	char path[MAX_PATH];
	getFile(AUTO_RESUME_PATH, path, MAX_PATH);
	unlink(AUTO_RESUME_PATH);
	sync();

	// make sure rom still exists
	char sd_path[MAX_PATH];
	snprintf(sd_path, sizeof(sd_path), "%s%s", SDCARD_PATH, path);
	if (!exists(sd_path))
		return 0;

	// make sure emu still exists
	char emu_name[MAX_PATH];
	getEmuName(sd_path, emu_name);

	char emu_path[MAX_PATH];
	getEmuPath(emu_name, emu_path);

	if (!exists(emu_path))
		return 0;

	// putFile(LAST_PATH, FAUX_RECENT_PATH); // saveLast() will crash here because top is NULL

	char* gametimectl_argv[] = {"gametimectl.elf", "start", sd_path, NULL};
	runCommand("gametimectl.elf", gametimectl_argv);

	char escaped_emu[MAX_PATH];
	char escaped_sd[MAX_PATH];
	strncpy(escaped_emu, emu_path, sizeof(escaped_emu) - 1);
	escaped_emu[sizeof(escaped_emu) - 1] = '\0';
	strncpy(escaped_sd, sd_path, sizeof(escaped_sd) - 1);
	escaped_sd[sizeof(escaped_sd) - 1] = '\0';

	char cmd[MAX_PATH];
	snprintf(cmd, sizeof(cmd), "'%s' '%s'", escapeSingleQuotes(escaped_emu, sizeof(escaped_emu)), escapeSingleQuotes(escaped_sd, sizeof(escaped_sd)));
	putInt(RESUME_SLOT_PATH, AUTO_RESUME_SLOT);
	queueNext(cmd);
	return 1;
}

void openPak(char* path) {
	saveLast(path);

	char escaped_path[MAX_PATH];
	strncpy(escaped_path, path, sizeof(escaped_path) - 1);
	escaped_path[sizeof(escaped_path) - 1] = '\0';

	char cmd[MAX_PATH];
	snprintf(cmd, sizeof(cmd), "'%s/launch.sh'", escapeSingleQuotes(escaped_path, sizeof(escaped_path)));
	queueNext(cmd);
}
void openRom(char* path, char* last) {
	LOG_info("openRom(%s,%s)\n", path, last);

	char sd_path[MAX_PATH];
	strcpy(sd_path, path);

	char m3u_path[MAX_PATH];
	int has_m3u = hasM3u(sd_path, m3u_path);

	char recent_path[MAX_PATH];
	strcpy(recent_path, has_m3u ? m3u_path : sd_path);

	if (has_m3u && suffixMatch(".m3u", sd_path)) {
		getFirstDisc(m3u_path, sd_path);
	}

	char emu_name[MAX_PATH];
	getEmuName(sd_path, emu_name);

	if (resume.should_resume) {
		char slot[16];
		getFile(resume.slot_path, slot, 16);
		putFile(RESUME_SLOT_PATH, slot);
		resume.should_resume = 0;

		if (has_m3u) {
			char rom_file[MAX_PATH];
			char* m3u_slash = strrchr(m3u_path, '/');
			if (!m3u_slash)
				return;
			strcpy(rom_file, m3u_slash + 1);

			// get disc for state
			char disc_path_path[MAX_PATH];
			snprintf(disc_path_path, sizeof(disc_path_path), "%s/.minui/%s/%s.%s.txt", SHARED_USERDATA_PATH, emu_name, rom_file, slot); // /.userdata/arm-480/.minui/<EMU>/<romname>.ext.0.txt

			if (exists(disc_path_path)) {
				// switch to disc path
				char disc_path[MAX_PATH];
				getFile(disc_path_path, disc_path, MAX_PATH);
				if (disc_path[0] == '/')
					strcpy(sd_path, disc_path); // absolute
				else {							// relative
					strcpy(sd_path, m3u_path);
					char* tmp = strrchr(sd_path, '/');
					if (!tmp)
						return;
					strcpy(tmp + 1, disc_path);
				}
			}
		}
	} else
		putInt(RESUME_SLOT_PATH, 8); // resume hidden default state

	char emu_path[MAX_PATH];
	getEmuPath(emu_name, emu_path);

	// NOTE: escapeSingleQuotes() modifies the passed string
	// so we need to save the path before we call that
	Recents_add(recent_path, Recents_getAlias()); // yiiikes

	// For multi-disc games in a subfolder, save the game folder instead of
	// the disc file path so we return to the console folder on next launch.
	char parent_dir[MAX_PATH];
	if (last == NULL) {
		strncpy(parent_dir, sd_path, MAX_PATH - 1);
		parent_dir[MAX_PATH - 1] = '\0';
		char* slash = strrchr(parent_dir, '/');
		if (slash) {
			*slash = '\0';
			// Only save parent if ROM is inside a game subfolder (not directly
			// in a console directory). This avoids navigating into disc listings.
			if (!isConsoleDir(parent_dir)) {
				last = parent_dir;
			}
		}
	}

	char* save_path = (last == NULL) ? sd_path : last;

	// If launched from root and the game is a shortcut, save root path
	// so the user returns to main menu instead of the console folder.
	if (exactMatch(top->path, SDCARD_PATH) && prefixMatch(SDCARD_PATH, save_path)) {
		if (Shortcuts_exists(save_path + strlen(SDCARD_PATH))) {
			save_path = SDCARD_PATH;
		}
	}

	saveLast(save_path);

	char* gametimectl_argv[] = {"gametimectl.elf", "start", sd_path, NULL};
	runCommand("gametimectl.elf", gametimectl_argv);

	char escaped_emu[MAX_PATH];
	char escaped_sd[MAX_PATH];
	strncpy(escaped_emu, emu_path, sizeof(escaped_emu) - 1);
	escaped_emu[sizeof(escaped_emu) - 1] = '\0';
	strncpy(escaped_sd, sd_path, sizeof(escaped_sd) - 1);
	escaped_sd[sizeof(escaped_sd) - 1] = '\0';

	char cmd[MAX_PATH];
	snprintf(cmd, sizeof(cmd), "'%s' '%s'", escapeSingleQuotes(escaped_emu, sizeof(escaped_emu)), escapeSingleQuotes(escaped_sd, sizeof(escaped_sd)));
	queueNext(cmd);
}

static bool isDirectSubdirectory(const Directory* parent, const char* child_path) {
	const char* parent_path = parent->path;

	size_t parent_len = strlen(parent_path);
	size_t child_len = strlen(child_path);

	// Child must be longer than parent to be a subdirectory
	if (child_len <= parent_len || strncmp(child_path, parent_path, parent_len) != 0) {
		return false;
	}

	// Next char after parent path must be '/'
	if (child_path[parent_len] != '/')
		return false;

	// Walk through the child path after parent, skipping PLATFORM segments
	const char* cursor = child_path + parent_len + 1; // skip the slash

	int levels = 0;
	while (*cursor) {
		const char* next = strchr(cursor, '/');
		size_t segment_len = next ? (size_t)(next - cursor) : strlen(cursor);

		if (segment_len == 0)
			break;

		// Copy segment into a buffer to compare
		char segment[PATH_MAX];
		if (segment_len >= PATH_MAX)
			return false;
		strncpy(segment, cursor, segment_len);
		segment[segment_len] = '\0';

		// Count level only if it's not PLATFORM
		if (strcmp(segment, PLATFORM) != 0 && strcmp(segment, "Roms") != 0) {
			levels++;
		}

		if (!next)
			break;
		cursor = next + 1;
	}

	return (levels == 1); // exactly one meaningful level deeper
}

Array* pathToStack(const char* path) {
	Array* array = Array_new();

	if (!path || strlen(path) == 0)
		return array;

	if (!prefixMatch(SDCARD_PATH, path))
		return array;

	// Always include root directory
	Directory* root_dir = Directory_new(SDCARD_PATH, 0);
	root_dir->start = 0;
	root_dir->end = (root_dir->entries->count < MAIN_ROW_COUNT) ? root_dir->entries->count : MAIN_ROW_COUNT;
	Array_push(array, root_dir);

	if (exactMatch(path, SDCARD_PATH))
		return array;

	char temp_path[PATH_MAX];
	strcpy(temp_path, SDCARD_PATH);
	size_t current_len = strlen(SDCARD_PATH);

	const char* cursor = path + current_len;
	if (*cursor == '/')
		cursor++;

	while (*cursor) {
		const char* next = strchr(cursor, '/');
		size_t segment_len = next ? (size_t)(next - cursor) : strlen(cursor);
		if (segment_len == 0 || segment_len >= PATH_MAX)
			break;

		char segment[PATH_MAX];
		strncpy(segment, cursor, segment_len);
		segment[segment_len] = '\0';

		// Append '/' if needed
		if (temp_path[current_len - 1] != '/') {
			if (current_len + 1 >= PATH_MAX)
				break;
			temp_path[current_len++] = '/';
			temp_path[current_len] = '\0';
		}

		// Append segment
		if (current_len + segment_len >= PATH_MAX)
			break;
		strcat(temp_path, segment);
		current_len += segment_len;

		if (strcmp(segment, PLATFORM) == 0) {
			// Merge with previous directory
			if (array->count > 0) {
				// Remove the previous directory
				Directory* last = (Directory*)array->items[array->count - 1];
				Array_pop(array);
				Directory_free(last); // assuming you have a Directory_free

				// Replace with updated one using combined path
				Directory* merged = Directory_new(temp_path, 0);
				merged->start = 0;
				merged->end = (merged->entries->count < MAIN_ROW_COUNT) ? merged->entries->count : MAIN_ROW_COUNT;
				Array_push(array, merged);
			}
		} else {
			Directory* dir = Directory_new(temp_path, 0);
			dir->start = 0;
			dir->end = (dir->entries->count < MAIN_ROW_COUNT) ? dir->entries->count : MAIN_ROW_COUNT;
			Array_push(array, dir);
		}

		if (!next)
			break;
		cursor = next + 1;
	}

	return array;
}

void openDirectory(char* path, int auto_launch) {
	char auto_path[MAX_PATH];
	if (hasCue(path, auto_path) && auto_launch) {
		startgame = 1;
		openRom(auto_path, path);
		return;
	}

	char m3u_path[MAX_PATH];
	strcpy(m3u_path, auto_path);
	char* tmp = strrchr(m3u_path, '.');
	if (!tmp)
		return;
	strcpy(tmp + 1, "m3u");
	if (exists(m3u_path) && auto_launch) {
		auto_path[0] = '\0';
		if (getFirstDisc(m3u_path, auto_path)) {
			startgame = 1;
			openRom(auto_path, path);
			return;
		}
		// TODO: doesn't handle empty m3u files
	}

	// If this is the exact same directory for some reason, just return.
	if (top && strcmp(top->path, path) == 0)
		return;

	// If this path is a direct subdirectory of top, push it on top of the stack
	// If it isnt, we need to recreate the stack to keep navigation consistent
	if (!top || isDirectSubdirectory(top, path)) {
		int selected = 0;
		int start = 0;
		int end = 0;
		if (top && top->entries->count > 0) {
			if (restore.depth == stack->count && top->selected == restore.relative) {
				selected = restore.selected;
				start = restore.start;
				end = restore.end;
			}
		}

		top = Directory_new(path, selected);
		top->start = start;
		top->end = end ? end : ((top->entries->count < MAIN_ROW_COUNT) ? top->entries->count : MAIN_ROW_COUNT);

		Array_push(stack, top);
	} else {
		// keep a copy of path, which might be a reference into stack which is about to be freed
		char temp_path[MAX_PATH];
		strncpy(temp_path, path, MAX_PATH - 1);
		temp_path[MAX_PATH - 1] = '\0';

		// construct a fresh stack by walking upwards until SDCARD_ROOT
		DirectoryArray_free(stack);

		stack = pathToStack(temp_path);
		top = stack->items[stack->count - 1];
	}
}

void closeDirectory(void) {
	if (!top || stack->count <= 1)
		return; // never pop root
	restore.selected = top->selected;
	restore.start = top->start;
	restore.end = top->end;
	DirectoryArray_pop(stack);
	restore.depth = stack->count;
	top = stack->items[stack->count - 1];
	restore.relative = top->selected;
}

void toggleQuick(Entry* self) {
	if (!self)
		return;

	switch (self->quickId) {
	case QUICK_WIFI:
		WIFI_enable(!WIFI_enabled());
		break;
	case QUICK_BLUETOOTH:
		BT_enable(!BT_enabled());
		break;
	case QUICK_SLEEP:
		PWR_sleep();
		break;
	case QUICK_REBOOT:
		if (_cleanupPool)
			_cleanupPool();
		PWR_powerOff(1);
		break;
	case QUICK_POWEROFF:
		if (_cleanupPool)
			_cleanupPool();
		PWR_powerOff(0);
		break;
	default:
		break;
	}
}

void Entry_open(Entry* self) {
	Recents_setAlias(self->name); // yiiikes
	if (self->type == ENTRY_ROM) {
		startgame = 1;
		char* last = NULL;
		char last_path[MAX_PATH];
		if (prefixMatch(COLLECTIONS_PATH, top->path)) {
			char* tmp;
			char filename[MAX_PATH];
			filename[0] = '\0';

			tmp = strrchr(self->path, '/');
			if (tmp)
				strcpy(filename, tmp + 1);

			snprintf(last_path, sizeof(last_path), "%s/%s", top->path, filename);
			last = last_path;
		}
		openRom(self->path, last);
	} else if (self->type == ENTRY_PAK) {
		startgame = 1;
		openPak(self->path);
	} else if (self->type == ENTRY_DIR) {
		openDirectory(self->path, 1);
	} else if (self->type == ENTRY_DIP) {
		toggleQuick(self);
	}
}

///////////////////////////////////////

void saveLast(char* path) {
	// special case for recently played
	if (exactMatch(top->path, FAUX_RECENT_PATH)) {
		// NOTE: that we don't have to save the file because
		// your most recently played game will always be at
		// the top which is also the default selection
		path = FAUX_RECENT_PATH;
	}
	putFile(LAST_PATH, path);
}
void loadLast(void) { // call after loading root directory
	if (!exists(LAST_PATH))
		return;

	char last_path[MAX_PATH];
	getFile(LAST_PATH, last_path, MAX_PATH);

	char full_path[MAX_PATH];
	strcpy(full_path, last_path);

	char* tmp;
	char filename[MAX_PATH];
	tmp = strrchr(last_path, '/');
	if (tmp)
		strcpy(filename, tmp + 1);

	Array* last = Array_new();
	while (!exactMatch(last_path, SDCARD_PATH)) {
		Array_push(last, strdup(last_path));

		char* slash = strrchr(last_path, '/');
		last_path[(slash - last_path)] = '\0';
	}

	while (last->count > 0) {
		char* path = Array_pop(last);
		if (!exactMatch(path, ROMS_PATH)) { // romsDir is effectively root as far as restoring state after a game
			char collated_path[MAX_PATH];
			collated_path[0] = '\0';
			if (suffixMatch(")", path) && isConsoleDir(path)) {
				strcpy(collated_path, path);
				tmp = strrchr(collated_path, '(');
				if (tmp)
					tmp[1] = '\0'; // 1 because we want to keep the opening parenthesis to avoid collating "Game Boy Color" and "Game Boy Advance" into "Game Boy"
			}

			for (int i = 0; i < top->entries->count; i++) {
				Entry* entry = top->entries->items[i];

				// NOTE: strlen() is required for collated_path, '\0' wasn't reading as NULL for some reason
				if (exactMatch(entry->path, path) || (strlen(collated_path) && prefixMatch(collated_path, entry->path) && isConsoleDir(entry->path)) || (prefixMatch(COLLECTIONS_PATH, full_path) && suffixMatch(filename, entry->path))) {
					top->selected = i;
					if (i >= top->end) {
						top->start = i;
						top->end = top->start + MAIN_ROW_COUNT;
						if (top->end > top->entries->count) {
							top->end = top->entries->count;
							top->start = top->end - MAIN_ROW_COUNT;
						}
					}
					if (last->count == 0 && !exactMatch(entry->path, FAUX_RECENT_PATH) && !(!exactMatch(entry->path, COLLECTIONS_PATH) && prefixMatch(COLLECTIONS_PATH, entry->path)))
						break; // don't show contents of auto-launch dirs

					if (entry->type == ENTRY_DIR) {
						// Don't navigate into auto-launch game folders
						// (directories with a matching .cue or .m3u file)
						char auto_path[MAX_PATH];
						if (hasCue(entry->path, auto_path)) {
							break; // just select the game folder
						}
						char* ext = strrchr(auto_path, '.');
						if (ext) {
							strcpy(ext + 1, "m3u");
							if (exists(auto_path)) {
								break; // just select the game folder
							}
						}
						openDirectory(entry->path, 0);
						break;
					}
				}
			}
		}
		free(path); // we took ownership when we popped it
	}

	StringArray_free(last);

	if (top->selected >= 0 && top->selected < top->entries->count) {
		Entry* selected_entry = top->entries->items[top->selected];
		readyResume(selected_entry);
	}
}
