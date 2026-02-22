#define _GNU_SOURCE
#include "lyrics.h"
#include "radio_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include "platform.h"
#include "include/parson/parson.h"

// Lyrics cache directory path on SD card
#define LYRICS_CACHE_DIR SDCARD_PATH "/.cache/lyrics"
#define CACHE_PARENT_DIR SDCARD_PATH "/.cache"

// Lyrics state (main thread only - written by thread when done)
static LyricLine lyrics_lines[LYRICS_MAX_LINES];
static int lyrics_line_count = 0;
static int lyrics_current_index = 0;
static bool lyrics_available = false;

// Dedup tracking
static char last_artist[256] = "";
static char last_title[256] = "";

// Background thread state - uses generation counter instead of pthread_join
// to avoid blocking the main thread on network timeouts
static volatile int fetch_generation = 0;

// Simple hash function for cache filename (DJB2)
static unsigned int simple_hash(const char* str) {
	unsigned int hash = 5381;
	int c;
	while ((c = *str++)) {
		hash = ((hash << 5) + hash) + c;
	}
	return hash;
}

// Ensure cache directory exists
static void ensure_cache_dir(void) {
	mkdir(CACHE_PARENT_DIR, 0755);
	mkdir(LYRICS_CACHE_DIR, 0755);
}

// Get cache file path for artist+title
static void get_cache_filepath(const char* artist, const char* title, char* path, int path_size) {
	char combined[512];
	snprintf(combined, sizeof(combined), "%s - %s", artist ? artist : "", title ? title : "");
	unsigned int hash = simple_hash(combined);
	snprintf(path, path_size, "%s/%08x.lrc", LYRICS_CACHE_DIR, hash);
}

// URL encode a string for use in query parameters
static void url_encode(const char* src, char* dst, int dst_size) {
	const char* hex = "0123456789ABCDEF";
	int j = 0;
	for (int i = 0; src[i] && j < dst_size - 4; i++) {
		unsigned char c = (unsigned char)src[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
			dst[j++] = c;
		} else if (c == ' ') {
			dst[j++] = '+';
		} else {
			dst[j++] = '%';
			dst[j++] = hex[c >> 4];
			dst[j++] = hex[c & 0x0F];
		}
	}
	dst[j] = '\0';
}

// Parse LRC text into a line array. Returns number of lines parsed.
static int parse_lrc_text(const char* lrc_text, LyricLine* lines, int max_lines) {
	int count = 0;
	const char* p = lrc_text;

	while (*p && count < max_lines) {
		// Skip whitespace/newlines
		while (*p == '\n' || *p == '\r' || *p == ' ')
			p++;
		if (!*p)
			break;

		// Expect '[' for timestamp
		if (*p != '[') {
			while (*p && *p != '\n')
				p++;
			continue;
		}
		p++; // skip '['

		// Parse mm:ss.xx
		int mm = 0, ss = 0, cs = 0;
		char* end;
		mm = (int)strtol(p, &end, 10);
		if (*end != ':') {
			// Not a timestamp line (metadata like [ar:Artist])
			while (*p && *p != '\n')
				p++;
			continue;
		}
		p = end + 1; // skip ':'
		ss = (int)strtol(p, &end, 10);
		if (*end == '.') {
			p = end + 1;
			cs = (int)strtol(p, &end, 10);
			// Handle both .xx (centiseconds) and .xxx (milliseconds)
			if (end - p == 3) {
				cs = cs / 10;
			}
		}
		p = end;

		// Skip to ']'
		while (*p && *p != ']')
			p++;
		if (*p == ']')
			p++;

		int time_ms = mm * 60000 + ss * 1000 + cs * 10;

		// Copy lyric text until end of line
		int len = 0;
		while (*p && *p != '\n' && *p != '\r' && len < 255) {
			lines[count].text[len++] = *p++;
		}
		lines[count].text[len] = '\0';

		// Skip empty lines
		if (len == 0)
			continue;

		lines[count].time_ms = time_ms;
		count++;
	}
	return count;
}

// Load cached LRC file from disk into the provided line array.
// Returns line count, or 0 on failure.
static int load_cached_lyrics(const char* cache_path, LyricLine* lines, int max_lines) {
	FILE* f = fopen(cache_path, "r");
	if (!f)
		return 0;

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size <= 0 || size > 256 * 1024) {
		fclose(f);
		return 0;
	}

	char* data = (char*)malloc(size + 1);
	if (!data) {
		fclose(f);
		return 0;
	}

	if (fread(data, 1, size, f) != (size_t)size) {
		free(data);
		fclose(f);
		return 0;
	}
	fclose(f);
	data[size] = '\0';

	int count = parse_lrc_text(data, lines, max_lines);
	free(data);
	return count;
}

// Save LRC text to cache file
static void save_lyrics_to_cache(const char* cache_path, const char* lrc_text) {
	FILE* f = fopen(cache_path, "w");
	if (!f)
		return;
	fputs(lrc_text, f);
	fclose(f);
}

// Thread argument
typedef struct {
	char artist[256];
	char title[256];
	int duration_sec;
	int generation; // to detect if this fetch is still current
} FetchArgs;

// Background fetch thread function (detached — must not touch shared state if stale)
static void* fetch_thread_func(void* arg) {
	FetchArgs* args = (FetchArgs*)arg;
	int my_gen = args->generation;

	// Temporary buffer for parsing (thread-local, not shared)
	LyricLine* tmp_lines = (LyricLine*)malloc(sizeof(LyricLine) * LYRICS_MAX_LINES);
	if (!tmp_lines) {
		free(args);
		return NULL;
	}

	ensure_cache_dir();

	char cache_path[768];
	get_cache_filepath(args->artist, args->title, cache_path, sizeof(cache_path));
	// Try disk cache first
	int count = load_cached_lyrics(cache_path, tmp_lines, LYRICS_MAX_LINES);
	if (count > 0) {
		if (fetch_generation == my_gen) {
			memcpy(lyrics_lines, tmp_lines, sizeof(LyricLine) * count);
			lyrics_line_count = count;
			lyrics_current_index = 0;
			lyrics_available = true;
		}
		free(tmp_lines);
		free(args);
		return NULL;
	}

	// URL-encode artist and title separately
	char encoded_artist[512];
	char encoded_title[512];
	url_encode(args->artist, encoded_artist, sizeof(encoded_artist));
	url_encode(args->title, encoded_title, sizeof(encoded_title));

	// Build LRCLIB exact match URL: /api/get?artist_name=X&track_name=Y&duration=Z
	char url[2048];
	snprintf(url, sizeof(url),
			 "https://lrclib.net/api/get?artist_name=%s&track_name=%s&duration=%d",
			 encoded_artist, encoded_title, args->duration_sec);
	// Fetch LRCLIB API response
	uint8_t* response_buf = (uint8_t*)malloc(64 * 1024);
	if (!response_buf) {
		free(tmp_lines);
		free(args);
		return NULL;
	}

	const char* synced_lyrics = NULL;
	JSON_Value* root = NULL;

	// Try exact match first
	int bytes = radio_net_fetch(url, response_buf, 64 * 1024, NULL, 0);

	if (bytes > 0) {
		response_buf[bytes] = '\0';
		root = json_parse_string((const char*)response_buf);
		if (root) {
			JSON_Object* obj = json_value_get_object(root);
			if (obj) {
				synced_lyrics = json_object_get_string(obj, "syncedLyrics");
				if (!synced_lyrics || !synced_lyrics[0])
					synced_lyrics = NULL;
			}
			if (!synced_lyrics) {
				json_value_free(root);
				root = NULL;
			}
		}
	}

	// Check if we've been superseded before doing fallback fetch
	if (fetch_generation != my_gen) {
		free(response_buf);
		if (root)
			json_value_free(root);
		free(tmp_lines);
		free(args);
		return NULL;
	}

	// Fallback: fuzzy search if exact match failed
	if (!synced_lyrics) {
		char query[512];
		snprintf(query, sizeof(query), "%s %s", args->artist, args->title);
		char encoded_query[1024];
		url_encode(query, encoded_query, sizeof(encoded_query));
		snprintf(url, sizeof(url), "https://lrclib.net/api/search?q=%s", encoded_query);

		bytes = radio_net_fetch(url, response_buf, 64 * 1024, NULL, 0);

		if (bytes > 0) {
			response_buf[bytes] = '\0';
			root = json_parse_string((const char*)response_buf);
			if (root) {
				JSON_Array* results = json_value_get_array(root);
				if (results && json_array_get_count(results) > 0) {
					for (size_t i = 0; i < json_array_get_count(results); i++) {
						JSON_Object* item = json_array_get_object(results, i);
						if (!item)
							continue;
						synced_lyrics = json_object_get_string(item, "syncedLyrics");
						if (synced_lyrics && synced_lyrics[0])
							break;
						synced_lyrics = NULL;
					}
				}
				if (!synced_lyrics) {
					json_value_free(root);
					root = NULL;
				}
			}
		}
	}

	free(response_buf);

	if (!synced_lyrics) {
		if (root)
			json_value_free(root);
		free(tmp_lines);
		free(args);
		return NULL;
	}

	// Save raw LRC text to cache
	save_lyrics_to_cache(cache_path, synced_lyrics);

	// Parse into temp buffer
	count = parse_lrc_text(synced_lyrics, tmp_lines, LYRICS_MAX_LINES);
	json_value_free(root);

	// Only write to shared state if this fetch is still current
	if (count > 0 && fetch_generation == my_gen) {
		memcpy(lyrics_lines, tmp_lines, sizeof(LyricLine) * count);
		lyrics_line_count = count;
		lyrics_current_index = 0;
		lyrics_available = true;
	}

	free(tmp_lines);
	free(args);
	return NULL;
}

void Lyrics_init(void) {
	lyrics_line_count = 0;
	lyrics_current_index = 0;
	lyrics_available = false;
	last_artist[0] = '\0';
	last_title[0] = '\0';
	fetch_generation = 0;
}

void Lyrics_cleanup(void) {
	fetch_generation++; // invalidate any running thread
	lyrics_line_count = 0;
	lyrics_current_index = 0;
	lyrics_available = false;
	last_artist[0] = '\0';
	last_title[0] = '\0';
}

void Lyrics_clear(void) {
	fetch_generation++; // invalidate any running thread
	lyrics_line_count = 0;
	lyrics_current_index = 0;
	lyrics_available = false;
	last_artist[0] = '\0';
	last_title[0] = '\0';
}

void Lyrics_fetch(const char* artist, const char* title, int duration_sec) {
	if (!artist || !title || (artist[0] == '\0' && title[0] == '\0')) {
		return;
	}

	// Dedup check
	if (strcmp(last_artist, artist) == 0 &&
		strcmp(last_title, title) == 0) {
		return;
	}

	// Invalidate any previous fetch — old thread will discard its results
	fetch_generation++;

	// Reset state
	strncpy(last_artist, artist, sizeof(last_artist) - 1);
	last_artist[sizeof(last_artist) - 1] = '\0';
	strncpy(last_title, title, sizeof(last_title) - 1);
	last_title[sizeof(last_title) - 1] = '\0';
	lyrics_line_count = 0;
	lyrics_current_index = 0;
	lyrics_available = false;

	// Prepare thread args
	FetchArgs* args = (FetchArgs*)malloc(sizeof(FetchArgs));
	if (!args)
		return;
	strncpy(args->artist, artist, sizeof(args->artist) - 1);
	args->artist[sizeof(args->artist) - 1] = '\0';
	strncpy(args->title, title, sizeof(args->title) - 1);
	args->title[sizeof(args->title) - 1] = '\0';
	args->duration_sec = duration_sec;
	args->generation = fetch_generation;

	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread, &attr, fetch_thread_func, args) != 0) {
		free(args);
	}
	pthread_attr_destroy(&attr);
}

const char* Lyrics_getCurrentLine(int position_ms) {
	if (!lyrics_available || lyrics_line_count == 0) {
		return NULL;
	}

	// Optimize: check if current index is still valid
	int idx = lyrics_current_index;
	int count = lyrics_line_count;

	if (idx >= 0 && idx < count &&
		lyrics_lines[idx].time_ms <= position_ms &&
		(idx + 1 >= count || lyrics_lines[idx + 1].time_ms > position_ms)) {
		return lyrics_lines[idx].text;
	}

	// Binary search for the correct line
	int lo = 0, hi = count - 1;
	int result = -1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (lyrics_lines[mid].time_ms <= position_ms) {
			result = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}

	if (result < 0) {
		lyrics_current_index = -1;
		return NULL;
	}

	lyrics_current_index = result;
	return lyrics_lines[result].text;
}

const char* Lyrics_getNextLine(void) {
	if (!lyrics_available || lyrics_line_count == 0)
		return NULL;
	int next = lyrics_current_index + 1;
	if (next >= lyrics_line_count)
		return NULL;
	return lyrics_lines[next].text;
}

bool Lyrics_isAvailable(void) {
	return lyrics_available;
}

long Lyrics_getCacheSize(void) {
	DIR* dir = opendir(LYRICS_CACHE_DIR);
	if (!dir)
		return 0;

	long total_size = 0;
	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		char filepath[768];
		snprintf(filepath, sizeof(filepath), "%s/%s", LYRICS_CACHE_DIR, ent->d_name);

		struct stat st;
		if (stat(filepath, &st) == 0)
			total_size += st.st_size;
	}
	closedir(dir);
	return total_size;
}

void Lyrics_clearCache(void) {
	DIR* dir = opendir(LYRICS_CACHE_DIR);
	if (!dir)
		return;

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		char filepath[768];
		snprintf(filepath, sizeof(filepath), "%s/%s", LYRICS_CACHE_DIR, ent->d_name);
		unlink(filepath);
	}
	closedir(dir);
}
