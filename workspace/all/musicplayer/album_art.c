#define _GNU_SOURCE
#include "album_art.h"
#include "wget_fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>
#include "api.h"
#include "include/parson/parson.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

// Album art cache directory path on SD card
#define ALBUMART_CACHE_DIR SDCARD_PATH "/.cache/albumart"
#define CACHE_PARENT_DIR SDCARD_PATH "/.cache"

// Album art module state
typedef struct {
	SDL_Surface* album_art;
	char last_art_artist[256];
	char last_art_title[256];
	bool art_fetch_in_progress;

	// Thread state
	pthread_t fetch_thread;
	bool thread_active;
	char req_artist[256];
	char req_title[256];

	// Thread result (written by thread, read by main)
	SDL_Surface* pending_art;
	bool result_ready;
} AlbumArtContext;

static AlbumArtContext art_ctx = {0};

// Simple hash function for cache filename
static unsigned int simple_hash(const char* str) {
	unsigned int hash = 5381;
	int c;
	while ((c = *str++)) {
		hash = ((hash << 5) + hash) + c;
	}
	return hash;
}

// Get album art cache directory path (on SD card)
static void get_cache_dir(char* path, int path_size) {
	snprintf(path, path_size, "%s", ALBUMART_CACHE_DIR);
}

// Ensure cache directory exists
static void ensure_cache_dir(void) {
	// Create .cache directory on SD card
	mkdir(CACHE_PARENT_DIR, 0755);
	// Create albumart cache directory
	mkdir(ALBUMART_CACHE_DIR, 0755);
}

// Get cache file path for artist+title
static void get_cache_filepath(const char* artist, const char* title, char* path, int path_size) {
	char cache_dir[512];
	get_cache_dir(cache_dir, sizeof(cache_dir));

	// Create hash from artist+title
	char combined[512];
	snprintf(combined, sizeof(combined), "%s_%s", artist ? artist : "", title ? title : "");
	unsigned int hash = simple_hash(combined);

	snprintf(path, path_size, "%s/%08x.jpg", cache_dir, hash);
}

// Load album art from cache file
static SDL_Surface* load_cached_album_art(const char* cache_path) {
	FILE* f = fopen(cache_path, "rb");
	if (!f)
		return NULL;

	// Get file size
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size <= 0 || size > 2 * 1024 * 1024) { // Max 2MB
		fclose(f);
		return NULL;
	}

	uint8_t* data = (uint8_t*)malloc(size);
	if (!data) {
		fclose(f);
		return NULL;
	}

	if (fread(data, 1, size, f) != (size_t)size) {
		free(data);
		fclose(f);
		return NULL;
	}
	fclose(f);

	SDL_RWops* rw = SDL_RWFromConstMem(data, size);
	SDL_Surface* art = NULL;
	if (rw) {
		art = IMG_Load_RW(rw, 1);
	}
	free(data);

	return art;
}

// Save album art to cache file
static void save_album_art_to_cache(const char* cache_path, const uint8_t* data, int size) {
	FILE* f = fopen(cache_path, "wb");
	if (!f)
		return;

	fwrite(data, 1, size, f);
	fclose(f);
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

// Background thread: fetch album art from iTunes API
static void* fetch_thread_func(void* arg) {
	(void)arg;
	const char* artist = art_ctx.req_artist;
	const char* title = art_ctx.req_title;

	ensure_cache_dir();

	// Check disk cache first
	char cache_path[768];
	get_cache_filepath(artist, title, cache_path, sizeof(cache_path));

	SDL_Surface* cached_art = load_cached_album_art(cache_path);
	if (cached_art) {
		art_ctx.pending_art = cached_art;
		art_ctx.result_ready = true;
		return NULL;
	}

	// Build search query using iTunes API
	char encoded_artist[512];
	char encoded_title[512];
	url_encode(artist, encoded_artist, sizeof(encoded_artist));
	url_encode(title, encoded_title, sizeof(encoded_title));

	char search_url[1024];
	if (artist[0] && title[0]) {
		snprintf(search_url, sizeof(search_url),
				 "https://itunes.apple.com/search?term=%s+%s&media=music&limit=1",
				 encoded_artist, encoded_title);
	} else if (artist[0]) {
		snprintf(search_url, sizeof(search_url),
				 "https://itunes.apple.com/search?term=%s&media=music&limit=1",
				 encoded_artist);
	} else {
		snprintf(search_url, sizeof(search_url),
				 "https://itunes.apple.com/search?term=%s&media=music&limit=1",
				 encoded_title);
	}

	// Fetch iTunes API response
	uint8_t* response_buf = (uint8_t*)malloc(32 * 1024);
	if (!response_buf) {
		art_ctx.result_ready = true;
		return NULL;
	}

	int bytes = wget_fetch(search_url, response_buf, 32 * 1024);
	if (bytes <= 0) {
		LOG_error("Failed to fetch iTunes search results\n");
		free(response_buf);
		art_ctx.result_ready = true;
		return NULL;
	}

	response_buf[bytes] = '\0';

	// Parse JSON response
	JSON_Value* root = json_parse_string((const char*)response_buf);
	free(response_buf);

	if (!root) {
		LOG_error("Failed to parse iTunes JSON response\n");
		art_ctx.result_ready = true;
		return NULL;
	}

	JSON_Object* obj = json_value_get_object(root);
	if (!obj) {
		json_value_free(root);
		art_ctx.result_ready = true;
		return NULL;
	}

	JSON_Array* results = json_object_get_array(obj, "results");
	if (!results || json_array_get_count(results) == 0) {
		json_value_free(root);
		art_ctx.result_ready = true;
		return NULL;
	}

	JSON_Object* track = json_array_get_object(results, 0);
	if (!track) {
		json_value_free(root);
		art_ctx.result_ready = true;
		return NULL;
	}

	const char* artwork_url = json_object_get_string(track, "artworkUrl100");
	if (!artwork_url) {
		json_value_free(root);
		art_ctx.result_ready = true;
		return NULL;
	}

	// Modify URL to get larger image and convert HTTPS to HTTP for better compatibility
	char large_artwork_url[512];
	if (strncmp(artwork_url, "https://", 8) == 0) {
		const char* after_https = artwork_url + 8;
		char* ssl_pos = strstr(after_https, "-ssl.");
		if (ssl_pos) {
			int prefix_len = ssl_pos - after_https;
			snprintf(large_artwork_url, sizeof(large_artwork_url), "http://%.*s%s",
					 prefix_len, after_https, ssl_pos + 4);
		} else {
			snprintf(large_artwork_url, sizeof(large_artwork_url), "http://%s", after_https);
		}
	} else {
		strncpy(large_artwork_url, artwork_url, sizeof(large_artwork_url) - 1);
	}
	large_artwork_url[sizeof(large_artwork_url) - 1] = '\0';

	// Replace 100x100 with 300x300 for larger image
	char* size_str = strstr(large_artwork_url, "100x100");
	if (size_str) {
		memcpy(size_str, "300x300", 7);
	}

	json_value_free(root);

	// Download the image
	uint8_t* image_buf = (uint8_t*)malloc(1024 * 1024);
	if (!image_buf) {
		art_ctx.result_ready = true;
		return NULL;
	}

	int image_bytes = wget_fetch(large_artwork_url, image_buf, 1024 * 1024);
	if (image_bytes <= 0) {
		LOG_error("Failed to download album art image (bytes=%d)\n", image_bytes);
		free(image_buf);
		art_ctx.result_ready = true;
		return NULL;
	}

	// Load image into SDL_Surface
	SDL_RWops* rw = SDL_RWFromConstMem(image_buf, image_bytes);
	if (rw) {
		SDL_Surface* art = IMG_Load_RW(rw, 1);
		if (art) {
			// Save to disk cache for future use
			save_album_art_to_cache(cache_path, image_buf, image_bytes);
			art_ctx.pending_art = art;
		} else {
			LOG_error("Failed to load album art image: %s\n", IMG_GetError());
		}
	}

	free(image_buf);
	art_ctx.result_ready = true;
	return NULL;
}

void album_art_init(void) {
	memset(&art_ctx, 0, sizeof(AlbumArtContext));
}

void album_art_cleanup(void) {
	// Wait for any active thread to finish
	if (art_ctx.thread_active) {
		pthread_join(art_ctx.fetch_thread, NULL);
		art_ctx.thread_active = false;
	}
	if (art_ctx.pending_art) {
		SDL_FreeSurface(art_ctx.pending_art);
		art_ctx.pending_art = NULL;
	}
	if (art_ctx.album_art) {
		SDL_FreeSurface(art_ctx.album_art);
		art_ctx.album_art = NULL;
	}
	art_ctx.last_art_artist[0] = '\0';
	art_ctx.last_art_title[0] = '\0';
	art_ctx.art_fetch_in_progress = false;
}

void album_art_clear(void) {
	// Wait for any active thread to finish before clearing
	if (art_ctx.thread_active) {
		pthread_join(art_ctx.fetch_thread, NULL);
		art_ctx.thread_active = false;
	}
	if (art_ctx.pending_art) {
		SDL_FreeSurface(art_ctx.pending_art);
		art_ctx.pending_art = NULL;
	}
	if (art_ctx.album_art) {
		SDL_FreeSurface(art_ctx.album_art);
		art_ctx.album_art = NULL;
	}
	art_ctx.last_art_artist[0] = '\0';
	art_ctx.last_art_title[0] = '\0';
	art_ctx.art_fetch_in_progress = false;
	art_ctx.result_ready = false;
}

SDL_Surface* album_art_get(void) {
	// Check if background thread has delivered a result
	if (art_ctx.result_ready && art_ctx.thread_active) {
		pthread_join(art_ctx.fetch_thread, NULL);
		art_ctx.thread_active = false;
		art_ctx.art_fetch_in_progress = false;
		art_ctx.result_ready = false;

		if (art_ctx.pending_art) {
			if (art_ctx.album_art) {
				SDL_FreeSurface(art_ctx.album_art);
			}
			art_ctx.album_art = art_ctx.pending_art;
			art_ctx.pending_art = NULL;
		}
	}
	return art_ctx.album_art;
}

bool album_art_is_fetching(void) {
	return art_ctx.art_fetch_in_progress;
}

// Fetch album art from iTunes Search API (truly async, non-blocking)
void album_art_fetch(const char* artist, const char* title) {
	if (!artist || !title || (artist[0] == '\0' && title[0] == '\0')) {
		return;
	}

	// Check if we already fetched art for this track
	if (strcmp(art_ctx.last_art_artist, artist) == 0 &&
		strcmp(art_ctx.last_art_title, title) == 0) {
		return; // Already fetched
	}

	// Wait for any previous thread to finish
	if (art_ctx.thread_active) {
		pthread_join(art_ctx.fetch_thread, NULL);
		art_ctx.thread_active = false;
		// Discard any pending result from previous fetch
		if (art_ctx.pending_art) {
			SDL_FreeSurface(art_ctx.pending_art);
			art_ctx.pending_art = NULL;
		}
	}

	// Save current track info
	art_ctx.art_fetch_in_progress = true;
	art_ctx.result_ready = false;
	art_ctx.pending_art = NULL;
	strncpy(art_ctx.last_art_artist, artist, sizeof(art_ctx.last_art_artist) - 1);
	strncpy(art_ctx.last_art_title, title, sizeof(art_ctx.last_art_title) - 1);
	strncpy(art_ctx.req_artist, artist, sizeof(art_ctx.req_artist) - 1);
	strncpy(art_ctx.req_title, title, sizeof(art_ctx.req_title) - 1);

	// Launch background thread
	if (pthread_create(&art_ctx.fetch_thread, NULL, fetch_thread_func, NULL) == 0) {
		art_ctx.thread_active = true;
	} else {
		// Thread creation failed, fall through
		art_ctx.art_fetch_in_progress = false;
	}
}

// Get the total size of the album art disk cache in bytes
long album_art_get_cache_size(void) {
	char cache_dir[512];
	get_cache_dir(cache_dir, sizeof(cache_dir));

	DIR* dir = opendir(cache_dir);
	if (!dir)
		return 0;

	long total_size = 0;
	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		char filepath[768];
		snprintf(filepath, sizeof(filepath), "%s/%s", cache_dir, ent->d_name);

		struct stat st;
		if (stat(filepath, &st) == 0) {
			total_size += st.st_size;
		}
	}
	closedir(dir);

	return total_size;
}

// Clear all cached album art from disk
void album_art_clear_disk_cache(void) {
	char cache_dir[512];
	get_cache_dir(cache_dir, sizeof(cache_dir));

	DIR* dir = opendir(cache_dir);
	if (!dir)
		return;

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		char filepath[768];
		snprintf(filepath, sizeof(filepath), "%s/%s", cache_dir, ent->d_name);
		unlink(filepath);
	}
	closedir(dir);

	// Also clear the in-memory album art since cached files are gone
	album_art_clear();
}
