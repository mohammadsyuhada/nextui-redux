#include <unistd.h>
#include "recents.h"
#include "api.h"

///////////////////////////////////////
// Internal state

static Array* recents = NULL;
static char* recent_alias = NULL;
static HasEmuFunc _hasEmu = NULL;
static HasM3uFunc _hasM3u = NULL;

#define MAX_RECENTS 24 // a multiple of all menu rows

///////////////////////////////////////
// Lifecycle

void Recents_init(void) {
	recents = Array_new();
}
void Recents_quit(void) {
	if (recents) {
		RecentArray_free(recents);
		recents = NULL;
	}
}

void Recents_setHasEmu(HasEmuFunc func) {
	_hasEmu = func;
}
void Recents_setHasM3u(HasM3uFunc func) {
	_hasM3u = func;
}

///////////////////////////////////////
// Recent struct methods

Recent* Recent_new(char* path, char* alias) {
	Recent* self = malloc(sizeof(Recent));
	if (!self)
		return NULL;

	char sd_path[MAX_PATH]; // only need to get emu name
	snprintf(sd_path, sizeof(sd_path), "%s%s", SDCARD_PATH, path);

	char emu_name[MAX_PATH];
	getEmuName(sd_path, emu_name);

	self->path = strdup(path);
	if (!self->path) {
		free(self);
		return NULL;
	}
	self->alias = alias ? strdup(alias) : NULL;
	self->available = _hasEmu ? _hasEmu(emu_name) : 0;
	return self;
}
void Recent_free(Recent* self) {
	free(self->path);
	if (self->alias)
		free(self->alias);
	free(self);
}

int RecentArray_indexOf(Array* self, char* str) {
	for (int i = 0; i < self->count; i++) {
		Recent* item = self->items[i];
		if (exactMatch(item->path, str))
			return i;
	}
	return -1;
}
void RecentArray_free(Array* self) {
	for (int i = 0; i < self->count; i++) {
		Recent_free(self->items[i]);
	}
	Array_free(self);
}

///////////////////////////////////////
// Core API

void Recents_save(void) {
	FILE* file = fopen(RECENT_PATH, "w");
	if (file) {
		for (int i = 0; i < recents->count; i++) {
			Recent* recent = recents->items[i];
			fputs(recent->path, file);
			if (recent->alias) {
				fputs("\t", file);
				fputs(recent->alias, file);
			}
			putc('\n', file);
		}
		fclose(file);
	}
}

void Recents_add(char* path, char* alias) {
	path += strlen(SDCARD_PATH); // makes paths platform agnostic
	int id = RecentArray_indexOf(recents, path);
	if (id == -1) { // add
		Recent* recent = Recent_new(path, alias);
		if (!recent)
			return;
		if (recents->count >= MAX_RECENTS) {
			Recent_free(Array_pop(recents));
		}
		Array_unshift(recents, recent);
	} else if (id > 0) { // bump to top
		Recent* existing = recents->items[id];
		if (alias) {
			char* new_alias = strdup(alias);
			if (new_alias) {
				free(existing->alias);
				existing->alias = new_alias;
			}
		}
		for (int i = id; i > 0; i--) {
			void* tmp = recents->items[i - 1];
			recents->items[i - 1] = recents->items[i];
			recents->items[i] = tmp;
		}
	}
	Recents_save();
}

int Recents_load(void) {
	LOG_info("hasRecents %s\n", RECENT_PATH);
	int has = 0;
	bool changed = false;
	RecentArray_free(recents);
	recents = Array_new();

	Array* parent_paths = Array_new();
	if (exists(CHANGE_DISC_PATH)) {
		changed = true;
		char sd_path[MAX_PATH];
		getFile(CHANGE_DISC_PATH, sd_path, MAX_PATH);
		if (exists(sd_path)) {
			char* disc_path = sd_path + strlen(SDCARD_PATH); // makes path platform agnostic
			Recent* recent = Recent_new(disc_path, NULL);
			if (!recent)
				goto done_disc;
			if (recent->available)
				has += 1;
			Array_push(recents, recent);

			char parent_path[MAX_PATH];
			strcpy(parent_path, disc_path);
			char* tmp = strrchr(parent_path, '/');
			if (tmp) {
				tmp[1] = '\0';
				char* dup = strdup(parent_path);
				if (dup)
					Array_push(parent_paths, dup);
			}
		}
	done_disc:
		unlink(CHANGE_DISC_PATH);
	}

	FILE* file = fopen(RECENT_PATH, "r"); // newest at top
	if (file) {
		char line[MAX_PATH];
		while (fgets(line, sizeof(line), file) != NULL) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			if (strlen(line) == 0)
				continue; // skip empty lines

			char* path = line;
			char* alias = NULL;
			char* tmp = strchr(line, '\t');
			if (tmp) {
				tmp[0] = '\0';
				alias = tmp + 1;
			}

			char sd_path[MAX_PATH];
			snprintf(sd_path, sizeof(sd_path), "%s%s", SDCARD_PATH, path);
			if (!exists(sd_path)) {
				changed = true;
				continue;
			}
			if (recents->count >= MAX_RECENTS) {
				changed = true;
				continue;
			}
			// this logic replaces an existing disc from a multi-disc game with the last used
			char m3u_path[MAX_PATH];
			if (_hasM3u && _hasM3u(sd_path, m3u_path)) { // TODO: this might tank launch speed
				char parent_path[MAX_PATH];
				strcpy(parent_path, path);
				char* tmp = strrchr(parent_path, '/');
				if (!tmp)
					goto skip_m3u;
				tmp[1] = '\0';

				bool found = false;
				for (int i = 0; i < parent_paths->count; i++) {
					char* parent = parent_paths->items[i];
					if (prefixMatch(parent, parent_path)) {
						found = true;
						break;
					}
				}
				if (found) {
					changed = true;
					continue;
				}

				char* dup = strdup(parent_path);
				if (dup)
					Array_push(parent_paths, dup);
			}

		skip_m3u:;
			Recent* recent = Recent_new(path, alias);
			if (!recent)
				continue;
			if (recent->available)
				has += 1;
			Array_push(recents, recent);
		}
		fclose(file);
	}

	if (changed)
		Recents_save();

	StringArray_free(parent_paths);
	return has > 0;
}

///////////////////////////////////////
// Access

Array* Recents_getArray(void) {
	return recents;
}
int Recents_count(void) {
	return recents ? recents->count : 0;
}

Recent* Recents_at(int index) {
	if (!recents || index < 0 || index >= recents->count)
		return NULL;
	return recents->items[index];
}

void Recents_removeAt(int index) {
	if (!recents || index < 0 || index >= recents->count)
		return;
	Recent* recent = recents->items[index];
	Array_remove(recents, recent);
	Recent_free(recent);
	Recents_save();
}

///////////////////////////////////////
// Entry conversion

Entry* Recents_entryFromRecent(Recent* recent) {
	if (!recent || !recent->available)
		return NULL;

	char sd_path[MAX_PATH];
	snprintf(sd_path, sizeof(sd_path), "%s%s", SDCARD_PATH, recent->path);
	int type = suffixMatch(".pak", sd_path) ? ENTRY_PAK : ENTRY_ROM; // ???
	Entry* entry = Entry_new(sd_path, type);
	if (recent->alias) {
		free(entry->name);
		entry->name = strdup(recent->alias);
	}
	return entry;
}

Array* Recents_getEntries(void) {
	Array* entries = Array_new();
	if (!recents)
		return entries;
	for (int i = 0; i < recents->count; i++) {
		Recent* recent = recents->items[i];
		Entry* entry = Recents_entryFromRecent(recent);
		if (entry)
			Array_push(entries, entry);
	}
	return entries;
}

///////////////////////////////////////
// Alias management

void Recents_setAlias(char* alias) {
	recent_alias = alias;
}
char* Recents_getAlias(void) {
	return recent_alias;
}
