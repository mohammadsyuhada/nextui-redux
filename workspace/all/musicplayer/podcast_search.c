#define _GNU_SOURCE
#include "podcast.h"
#include "wget_fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "api.h"

// JSON library
#include "include/parson/parson.h"

// URL encode a string for use in query parameters
static void url_encode(const char* src, char* dest, size_t dest_size) {
	const char* hex = "0123456789ABCDEF";
	size_t j = 0;

	for (size_t i = 0; src[i] && j < dest_size - 4; i++) {
		unsigned char c = (unsigned char)src[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
			dest[j++] = c;
		} else if (c == ' ') {
			dest[j++] = '+';
		} else {
			dest[j++] = '%';
			dest[j++] = hex[(c >> 4) & 0x0F];
			dest[j++] = hex[c & 0x0F];
		}
	}
	dest[j] = '\0';
}

// Convert Apple artwork URL to larger size (100x100 -> 400x400)
// Example: https://...mzstatic.com/.../100x100bb.png -> https://...mzstatic.com/.../400x400bb.png
static void artwork_url_upscale(const char* src, char* dest, size_t dest_size) {
	if (!src || !dest || dest_size == 0)
		return;

	// Copy the URL and look for size pattern to replace
	strncpy(dest, src, dest_size - 1);
	dest[dest_size - 1] = '\0';

	// Find and replace "100x100bb" with "400x400bb"
	char* pos = strstr(dest, "100x100bb");
	if (pos) {
		// Replace in place - same length so safe
		memcpy(pos, "400x400bb", 9);
	}
}

// Search iTunes for podcasts
// Returns number of results, or -1 on error
int podcast_search_itunes(const char* query, PodcastSearchResult* results, int max_results) {
	if (!query || !results || max_results <= 0) {
		return -1;
	}

	// URL encode the query
	char encoded_query[512];
	url_encode(query, encoded_query, sizeof(encoded_query));

	// Build search URL
	// https://itunes.apple.com/search?term=QUERY&media=podcast&limit=50
	char url[1024];
	snprintf(url, sizeof(url),
			 "https://itunes.apple.com/search?term=%s&media=podcast&limit=%d",
			 encoded_query, max_results > 50 ? 50 : max_results);


	// Fetch JSON response
	uint8_t* buffer = (uint8_t*)malloc(128 * 1024); // 128KB buffer
	if (!buffer) {
		LOG_error("[PodcastSearch] Failed to allocate buffer\n");
		return -1;
	}

	int bytes = wget_fetch(url, buffer, 128 * 1024);

	if (bytes <= 0) {
		LOG_error("[PodcastSearch] Failed to fetch search results\n");
		free(buffer);
		return -1;
	}

	// Null-terminate for JSON parsing
	buffer[bytes] = '\0';

	// Parse JSON response
	JSON_Value* root = json_parse_string((const char*)buffer);

	if (!root) {
		// Log first bytes for diagnostics (truncate to 200 chars)
		char preview[201];
		int preview_len = bytes > 200 ? 200 : bytes;
		memcpy(preview, buffer, preview_len);
		preview[preview_len] = '\0';
		// Replace non-printable chars for safe logging
		for (int i = 0; i < preview_len; i++) {
			if ((unsigned char)preview[i] < 0x20 && preview[i] != '\n' && preview[i] != '\r' && preview[i] != '\t') {
				preview[i] = '?';
			}
		}
		LOG_error("[PodcastSearch] Failed to parse JSON response (%d bytes). First bytes: %.200s\n", bytes, preview);
		free(buffer);
		return -1;
	}
	free(buffer);

	JSON_Object* obj = json_value_get_object(root);
	if (!obj) {
		json_value_free(root);
		return -1;
	}

	JSON_Array* results_arr = json_object_get_array(obj, "results");
	if (!results_arr) {
		json_value_free(root);
		return 0; // No results
	}

	int count = json_array_get_count(results_arr);
	int result_count = 0;

	for (int i = 0; i < count && result_count < max_results; i++) {
		JSON_Object* item = json_array_get_object(results_arr, i);
		if (!item)
			continue;

		// Filter out premium podcasts (trackPrice > 0)
		double track_price = json_object_get_number(item, "trackPrice");
		if (track_price > 0) {
			continue;
		}

		// Filter out podcasts without feed URL
		const char* feed_url = json_object_get_string(item, "feedUrl");
		if (!feed_url || feed_url[0] == '\0') {
			continue;
		}

		PodcastSearchResult* result = &results[result_count];
		memset(result, 0, sizeof(PodcastSearchResult));

		// Get track ID (iTunes ID)
		double track_id = json_object_get_number(item, "trackId");
		if (track_id > 0) {
			snprintf(result->itunes_id, sizeof(result->itunes_id), "%.0f", track_id);
		}

		// Get podcast name
		const char* name = json_object_get_string(item, "trackName");
		if (name) {
			strncpy(result->title, name, PODCAST_MAX_TITLE - 1);
		}

		// Get artist name
		const char* artist = json_object_get_string(item, "artistName");
		if (artist) {
			strncpy(result->author, artist, PODCAST_MAX_AUTHOR - 1);
		}

		// Get artwork URL (100x100 -> upscale to 400x400)
		const char* artwork = json_object_get_string(item, "artworkUrl100");
		if (artwork) {
			artwork_url_upscale(artwork, result->artwork_url, PODCAST_MAX_URL);
		}

		// Copy feed URL (already validated above)
		strncpy(result->feed_url, feed_url, PODCAST_MAX_URL - 1);

		// Get genre
		const char* genre = json_object_get_string(item, "primaryGenreName");
		if (genre) {
			strncpy(result->genre, genre, PODCAST_MAX_GENRE - 1);
		}

		result_count++;
	}

	json_value_free(root);
	return result_count;
}

// Lookup podcast feed URL and artwork from iTunes ID (full version)
int podcast_search_lookup_full(const char* itunes_id, char* feed_url, int feed_url_size,
							   char* artwork_url, int artwork_url_size) {
	if (!itunes_id || !feed_url || feed_url_size <= 0) {
		return -1;
	}

	// Build lookup URL
	char url[256];
	snprintf(url, sizeof(url), "https://itunes.apple.com/lookup?id=%s", itunes_id);


	// Fetch JSON response
	uint8_t* buffer = (uint8_t*)malloc(32 * 1024); // 32KB buffer
	if (!buffer) {
		return -1;
	}

	int bytes = wget_fetch(url, buffer, 32 * 1024);
	if (bytes <= 0) {
		free(buffer);
		return -1;
	}

	buffer[bytes] = '\0';

	// Parse JSON
	JSON_Value* root = json_parse_string((const char*)buffer);
	free(buffer);

	if (!root) {
		return -1;
	}

	JSON_Object* obj = json_value_get_object(root);
	if (!obj) {
		json_value_free(root);
		return -1;
	}

	JSON_Array* results = json_object_get_array(obj, "results");
	if (!results || json_array_get_count(results) == 0) {
		json_value_free(root);
		return -1;
	}

	JSON_Object* podcast = json_array_get_object(results, 0);
	if (!podcast) {
		json_value_free(root);
		return -1;
	}

	const char* url_str = json_object_get_string(podcast, "feedUrl");
	if (!url_str) {
		json_value_free(root);
		return -1;
	}

	strncpy(feed_url, url_str, feed_url_size - 1);
	feed_url[feed_url_size - 1] = '\0';

	// Also get artwork URL if requested (try artworkUrl600 first, then artworkUrl100 upscaled)
	if (artwork_url && artwork_url_size > 0) {
		const char* art = json_object_get_string(podcast, "artworkUrl600");
		if (art) {
			strncpy(artwork_url, art, artwork_url_size - 1);
			artwork_url[artwork_url_size - 1] = '\0';
		} else {
			art = json_object_get_string(podcast, "artworkUrl100");
			if (art) {
				artwork_url_upscale(art, artwork_url, artwork_url_size);
			}
		}
		if (artwork_url[0]) {
		}
	}

	json_value_free(root);
	return 0;
}

// Simple wrapper - lookup feed URL only
int podcast_search_lookup(const char* itunes_id, char* feed_url, int feed_url_size) {
	return podcast_search_lookup_full(itunes_id, feed_url, feed_url_size, NULL, 0);
}

// Fetch Apple Podcast Charts (Top Shows)
int podcast_charts_fetch(const char* country_code, PodcastChartItem* top, int* top_count,
						 PodcastChartItem* new_items, int* new_count, int max_items) {
	(void)new_items; // Unused - kept for API compatibility
	(void)new_count; // Unused - kept for API compatibility

	if (!country_code || !top || !top_count || max_items <= 0) {
		return -1;
	}

	*top_count = 0;

	// Fetch Top Podcasts
	// URL: https://rss.marketingtools.apple.com/api/v2/{country}/podcasts/top/{limit}/podcasts.json
	// Allow up to 100 items to have buffer after filtering premium podcasts
	char url[512];
	int fetch_limit = max_items > 100 ? 100 : max_items;
	snprintf(url, sizeof(url),
			 "https://rss.marketingtools.apple.com/api/v2/%s/podcasts/top/%d/podcasts.json",
			 country_code, fetch_limit);


	uint8_t* buffer = (uint8_t*)malloc(256 * 1024); // 256KB buffer
	if (!buffer) {
		LOG_error("[PodcastCharts] Failed to allocate buffer for top shows\n");
		return -1;
	}

	int bytes = wget_fetch(url, buffer, 256 * 1024);
	if (bytes <= 0) {
		LOG_error("[PodcastCharts] Network fetch failed for top shows (bytes=%d)\n", bytes);
	} else {
		buffer[bytes] = '\0';

		JSON_Value* root = json_parse_string((const char*)buffer);
		if (root) {
			JSON_Object* obj = json_value_get_object(root);
			JSON_Object* feed = json_object_get_object(obj, "feed");
			if (feed) {
				JSON_Array* results = json_object_get_array(feed, "results");
				if (results) {
					int count = json_array_get_count(results);
					for (int i = 0; i < count && *top_count < max_items; i++) {
						JSON_Object* item = json_array_get_object(results, i);
						if (!item)
							continue;

						PodcastChartItem* chart = &top[*top_count];
						memset(chart, 0, sizeof(PodcastChartItem));

						const char* id = json_object_get_string(item, "id");
						if (id)
							strncpy(chart->itunes_id, id, sizeof(chart->itunes_id) - 1);

						const char* name = json_object_get_string(item, "name");
						if (name)
							strncpy(chart->title, name, PODCAST_MAX_TITLE - 1);

						const char* artist = json_object_get_string(item, "artistName");
						if (artist)
							strncpy(chart->author, artist, PODCAST_MAX_AUTHOR - 1);

						const char* artwork = json_object_get_string(item, "artworkUrl100");
						if (artwork)
							artwork_url_upscale(artwork, chart->artwork_url, PODCAST_MAX_URL);

						// Get genre from genres array
						JSON_Array* genres = json_object_get_array(item, "genres");
						if (genres && json_array_get_count(genres) > 0) {
							JSON_Object* genre = json_array_get_object(genres, 0);
							if (genre) {
								const char* genre_name = json_object_get_string(genre, "name");
								if (genre_name)
									strncpy(chart->genre, genre_name, PODCAST_MAX_GENRE - 1);
							}
						}

						// Note: feed_url needs to be fetched via lookup API using itunes_id
						(*top_count)++;
					}
				}
			}
			json_value_free(root);
		}
	}

	free(buffer);

	return (*top_count > 0) ? 0 : -1;
}

// Filter chart items by doing batch iTunes lookup
// Removes premium podcasts (trackPrice > 0) and those without feedUrl
// Returns the new count after filtering
int podcast_charts_filter_premium(PodcastChartItem* items, int count, int max_items) {
	if (!items || count <= 0) {
		return 0;
	}

	// Build comma-separated list of iTunes IDs for batch lookup
	// iTunes API supports up to 200 IDs per request
	char ids_param[2048] = "";
	int ids_len = 0;
	for (int i = 0; i < count && ids_len < (int)sizeof(ids_param) - 40; i++) {
		if (items[i].itunes_id[0]) {
			if (ids_len > 0) {
				ids_param[ids_len++] = ',';
			}
			int id_len = strlen(items[i].itunes_id);
			memcpy(ids_param + ids_len, items[i].itunes_id, id_len);
			ids_len += id_len;
		}
	}
	ids_param[ids_len] = '\0';

	if (ids_len == 0) {
		return 0;
	}

	// Build batch lookup URL
	char url[2560];
	snprintf(url, sizeof(url), "https://itunes.apple.com/lookup?id=%s", ids_param);


	uint8_t* buffer = (uint8_t*)malloc(256 * 1024); // 256KB buffer
	if (!buffer) {
		LOG_error("[PodcastCharts] Failed to allocate buffer for batch lookup\n");
		return count; // Return original count on error
	}

	int bytes = wget_fetch(url, buffer, 256 * 1024);
	if (bytes <= 0) {
		LOG_error("[PodcastCharts] Batch lookup failed\n");
		free(buffer);
		return count;
	}

	buffer[bytes] = '\0';

	// Parse response and build a map of valid (non-premium) podcast IDs with their feed URLs
	JSON_Value* root = json_parse_string((const char*)buffer);
	free(buffer);

	if (!root) {
		LOG_error("[PodcastCharts] Failed to parse batch lookup response\n");
		return count;
	}

	JSON_Object* obj = json_value_get_object(root);
	JSON_Array* results = json_object_get_array(obj, "results");
	if (!results) {
		json_value_free(root);
		return count;
	}

	// Create a simple lookup structure for valid podcasts
	typedef struct {
		char itunes_id[32];
		char feed_url[PODCAST_MAX_URL];
		bool is_valid; // true if free and has feed URL
	} LookupResult;

	int result_count = json_array_get_count(results);
	LookupResult* lookup_results = (LookupResult*)malloc(result_count * sizeof(LookupResult));
	if (!lookup_results) {
		json_value_free(root);
		return count;
	}

	int valid_count = 0;
	for (int i = 0; i < result_count; i++) {
		JSON_Object* item = json_array_get_object(results, i);
		if (!item)
			continue;

		// Get track ID
		double track_id = json_object_get_number(item, "trackId");
		if (track_id <= 0)
			continue;

		// Check track price - skip if premium (price > 0)
		double track_price = json_object_get_number(item, "trackPrice");
		if (track_price > 0) {
			continue;
		}

		// Get feed URL - skip if missing
		const char* feed_url = json_object_get_string(item, "feedUrl");
		if (!feed_url || feed_url[0] == '\0') {
			continue;
		}

		// This podcast is valid
		LookupResult* lr = &lookup_results[valid_count];
		snprintf(lr->itunes_id, sizeof(lr->itunes_id), "%.0f", track_id);
		strncpy(lr->feed_url, feed_url, PODCAST_MAX_URL - 1);
		lr->feed_url[PODCAST_MAX_URL - 1] = '\0';
		lr->is_valid = true;
		valid_count++;
	}

	json_value_free(root);

	// Now filter the original items array - keep only valid ones
	int filtered_count = 0;
	for (int i = 0; i < count && filtered_count < max_items; i++) {
		// Find this item in lookup results
		bool found_valid = false;
		for (int j = 0; j < valid_count; j++) {
			if (strcmp(items[i].itunes_id, lookup_results[j].itunes_id) == 0) {
				// Copy feed_url from lookup result
				strncpy(items[i].feed_url, lookup_results[j].feed_url, PODCAST_MAX_URL - 1);
				items[i].feed_url[PODCAST_MAX_URL - 1] = '\0';
				found_valid = true;
				break;
			}
		}

		if (found_valid) {
			// Move to filtered position if needed
			if (filtered_count != i) {
				memcpy(&items[filtered_count], &items[i], sizeof(PodcastChartItem));
			}
			filtered_count++;
		}
	}

	free(lookup_results);
	return filtered_count;
}
