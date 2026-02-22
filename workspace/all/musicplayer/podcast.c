#define _GNU_SOURCE
#include "podcast.h"
#include "wget_fetch.h"
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <locale.h>
#include <ctype.h>

#include "defines.h"
#include "api.h"
#include "wifi.h"
#include "ui_podcast.h"
#include "module_common.h"
#include <sys/statvfs.h>

// SDCARD_PATH is defined in platform.h via api.h

// JSON library
#include "include/parson/parson.h"

// Timezone to country code mapping for Apple Podcast charts
typedef struct {
	const char* timezone; // Timezone name or city
	const char* country;  // ISO 3166-1 alpha-2 country code
} TimezoneCountryMap;

static const TimezoneCountryMap tz_country_map[] = {
	// Asia
	{"Kuala_Lumpur", "my"},
	{"Singapore", "sg"},
	{"Jakarta", "id"},
	{"Bangkok", "th"},
	{"Ho_Chi_Minh", "vn"},
	{"Saigon", "vn"},
	{"Manila", "ph"},
	{"Tokyo", "jp"},
	{"Seoul", "kr"},
	{"Shanghai", "cn"},
	{"Hong_Kong", "hk"},
	{"Taipei", "tw"},
	{"Kolkata", "in"},
	{"Calcutta", "in"},
	{"Mumbai", "in"},
	{"Dubai", "ae"},
	{"Riyadh", "sa"},
	{"Jerusalem", "il"},
	{"Tel_Aviv", "il"},
	// Europe
	{"London", "gb"},
	{"Paris", "fr"},
	{"Berlin", "de"},
	{"Rome", "it"},
	{"Madrid", "es"},
	{"Amsterdam", "nl"},
	{"Brussels", "be"},
	{"Vienna", "at"},
	{"Zurich", "ch"},
	{"Stockholm", "se"},
	{"Oslo", "no"},
	{"Copenhagen", "dk"},
	{"Helsinki", "fi"},
	{"Warsaw", "pl"},
	{"Prague", "cz"},
	{"Budapest", "hu"},
	{"Athens", "gr"},
	{"Moscow", "ru"},
	{"Dublin", "ie"},
	{"Lisbon", "pt"},
	// Americas
	{"New_York", "us"},
	{"Los_Angeles", "us"},
	{"Chicago", "us"},
	{"Denver", "us"},
	{"Phoenix", "us"},
	{"Anchorage", "us"},
	{"Honolulu", "us"},
	{"Toronto", "ca"},
	{"Vancouver", "ca"},
	{"Montreal", "ca"},
	{"Mexico_City", "mx"},
	{"Sao_Paulo", "br"},
	{"Buenos_Aires", "ar"},
	{"Lima", "pe"},
	{"Bogota", "co"},
	{"Santiago", "cl"},
	// Oceania
	{"Sydney", "au"},
	{"Melbourne", "au"},
	{"Brisbane", "au"},
	{"Perth", "au"},
	{"Adelaide", "au"},
	{"Auckland", "nz"},
	// Africa
	{"Cairo", "eg"},
	{"Johannesburg", "za"},
	{"Lagos", "ng"},
	{"Nairobi", "ke"},
	{"Casablanca", "ma"},
	{NULL, NULL}};

// Apple Podcast supported countries (subset of most common ones)
// Full list: https://rss.marketingtools.apple.com/
static const char* apple_podcast_countries[] = {
	"us", "gb", "ca", "au", "nz", "ie",										// English-speaking
	"de", "fr", "es", "it", "nl", "be", "at", "ch", "pt",					// Western Europe
	"se", "no", "dk", "fi",													// Nordic
	"pl", "cz", "hu", "gr", "ru",											// Eastern Europe
	"jp", "kr", "cn", "hk", "tw", "sg", "my", "th", "id", "ph", "vn", "in", // Asia
	"ae", "sa", "il",														// Middle East
	"br", "mx", "ar", "cl", "co", "pe",										// Latin America
	"za", "eg", "ng", "ke", "ma",											// Africa
	NULL};

// Check if country code is supported by Apple Podcast
static bool is_apple_podcast_country(const char* country) {
	if (!country)
		return false;
	for (int i = 0; apple_podcast_countries[i] != NULL; i++) {
		if (strcasecmp(country, apple_podcast_countries[i]) == 0) {
			return true;
		}
	}
	return false;
}

// Get country code from timezone path like /usr/share/zoneinfo/Asia/Kuala_Lumpur
static const char* get_country_from_timezone(const char* tz_path) {
	if (!tz_path)
		return NULL;

	// Find the last component (city name)
	const char* city = strrchr(tz_path, '/');
	if (city)
		city++;
	else
		city = tz_path;

	// Look up in mapping table
	for (int i = 0; tz_country_map[i].timezone != NULL; i++) {
		if (strcmp(city, tz_country_map[i].timezone) == 0) {
			return tz_country_map[i].country;
		}
	}
	return NULL;
}

// Paths
static char subscriptions_file[512] = "";
static char progress_file[512] = "";
static char downloads_file[512] = "";
static char charts_cache_file[512] = "";
static char continue_listening_file[512] = "";
static char download_dir[512] = "";

// Module state
static bool podcast_initialized = false;
static PodcastState podcast_state = PODCAST_STATE_IDLE;
static char error_message[256] = "";

// Subscriptions
static PodcastFeed subscriptions[PODCAST_MAX_SUBSCRIPTIONS];
static int subscription_count = 0;
static pthread_mutex_t subscriptions_mutex = PTHREAD_MUTEX_INITIALIZER;

// Search
static pthread_t search_thread;
static volatile bool search_running = false;
static volatile bool search_should_stop = false;
static PodcastSearchResult search_results[PODCAST_MAX_SEARCH_RESULTS];
static int search_result_count = 0;
static PodcastSearchStatus search_status = {0};
static char search_query_copy[256] = "";

// Charts
static pthread_t charts_thread;
static volatile bool charts_running = false;
static volatile bool charts_should_stop = false;
static PodcastChartItem top_shows[PODCAST_CHART_FETCH_LIMIT]; // Sized for fetch limit, filtered down to MAX_CHART_ITEMS
static int top_shows_count = 0;
static PodcastChartsStatus charts_status = {0};
static char charts_country_code[8] = "us";

// Downloads
static PodcastDownloadItem download_queue[PODCAST_MAX_DOWNLOAD_QUEUE];
static int download_queue_count = 0;
static pthread_mutex_t download_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t download_thread;
static volatile bool download_running = false;
static volatile bool download_should_stop = false;
static PodcastDownloadProgress download_progress = {0};

// Current playback state
static int current_episode_duration_sec = 0;
static PodcastFeed* current_feed = NULL;
static int current_feed_index = -1;
static int current_episode_index = -1;

// Progress tracking - simple in-memory cache (will be persisted to JSON)
#define MAX_PROGRESS_ENTRIES 500
typedef struct {
	char feed_url[PODCAST_MAX_URL];
	char episode_guid[PODCAST_MAX_GUID];
	int position_sec;
} ProgressEntry;
static ProgressEntry progress_entries[MAX_PROGRESS_ENTRIES];
static int progress_entry_count = 0;

// Episode cache - only load PODCAST_EPISODE_PAGE_SIZE episodes at a time
static PodcastEpisode episode_cache[PODCAST_EPISODE_PAGE_SIZE];
static int episode_cache_feed_index = -1;
static int episode_cache_offset = 0;
static int episode_cache_count = 0;
static pthread_mutex_t episode_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

// Continue Listening
static ContinueListeningEntry continue_listening[PODCAST_MAX_CONTINUE_LISTENING];
static int continue_listening_count = 0;

// Feed refresh
static pthread_t refresh_thread;
static volatile bool refresh_running = false;
static int refresh_feed_index = -1; // -1 = all feeds, >=0 = specific feed
static volatile bool refresh_completed = false;
#define REFRESH_COOLDOWN_SEC 900 // 15 minutes

// Base data directory for podcast data
static char podcast_data_dir[512] = "";

// Forward declarations
static void* search_thread_func(void* arg);
static void* charts_thread_func(void* arg);
static void* download_thread_func(void* arg);
static void* refresh_thread_func(void* arg);
static int Podcast_startDownloads(void);
static void save_charts_cache(void);
static bool load_charts_cache(void);
static void save_continue_listening(void);
static void load_continue_listening(void);
static void validate_continue_listening(void);
static void sanitize_for_filename(char* str);


// ============================================================================
// Feed ID and Path Helpers
// ============================================================================

// Set feed_id: use itunes_id if available, otherwise generate hash from URL
static void set_feed_id(PodcastFeed* feed) {
	if (!feed || feed->feed_id[0])
		return; // Already set

	// Prefer iTunes ID (stable, readable)
	if (feed->itunes_id[0]) {
		strncpy(feed->feed_id, feed->itunes_id, sizeof(feed->feed_id) - 1);
		feed->feed_id[sizeof(feed->feed_id) - 1] = '\0';
		return;
	}

	// Fallback: generate hash from feed URL
	if (!feed->feed_url[0])
		return;

	unsigned long hash1 = 5381;
	unsigned long hash2 = 0;
	const char* p = feed->feed_url;
	while (*p) {
		hash1 = ((hash1 << 5) + hash1) + (unsigned char)*p;
		hash2 = hash2 * 31 + (unsigned char)*p;
		p++;
	}
	snprintf(feed->feed_id, sizeof(feed->feed_id), "%08lx%08lx", hash1 & 0xFFFFFFFF, hash2 & 0xFFFFFFFF);
}

// Get path to feed's data directory
void Podcast_getFeedDataPath(const char* feed_id, char* path, int path_size) {
	if (!feed_id || !path || path_size <= 0)
		return;
	snprintf(path, path_size, "%s/%s", podcast_data_dir, feed_id);
}

// Helper: find feed index from feed pointer
static int get_feed_index(PodcastFeed* feed) {
	if (!feed)
		return -1;
	for (int i = 0; i < subscription_count; i++) {
		if (&subscriptions[i] == feed) {
			return i;
		}
	}
	return -1;
}

// Get path to feed's episodes JSON file
static void get_episodes_file_path(const char* feed_id, char* path, int path_size) {
	if (!feed_id || !path || path_size <= 0)
		return;
	snprintf(path, path_size, "%s/%s/episodes.json", podcast_data_dir, feed_id);
}

// Create directory recursively
static void mkdir_recursive(const char* path) {
	char tmp[512];
	char* p = NULL;
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	if (tmp[len - 1] == '/')
		tmp[len - 1] = '\0';

	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

// ============================================================================
// Episode Storage (JSON on disk)
// ============================================================================

// Save episodes to JSON file for a feed
int Podcast_saveEpisodes(int feed_index, PodcastEpisode* episodes, int count) {
	if (feed_index < 0 || feed_index >= subscription_count || !episodes || count < 0) {
		return -1;
	}

	PodcastFeed* feed = &subscriptions[feed_index];

	set_feed_id(feed);

	// Create feed directory
	char feed_dir[512];
	Podcast_getFeedDataPath(feed->feed_id, feed_dir, sizeof(feed_dir));
	mkdir_recursive(feed_dir);

	// Build episodes JSON
	JSON_Value* root = json_value_init_array();
	JSON_Array* arr = json_value_get_array(root);

	for (int i = 0; i < count; i++) {
		PodcastEpisode* ep = &episodes[i];
		JSON_Value* ep_val = json_value_init_object();
		JSON_Object* ep_obj = json_value_get_object(ep_val);

		json_object_set_string(ep_obj, "guid", ep->guid);
		json_object_set_string(ep_obj, "title", ep->title);
		json_object_set_string(ep_obj, "url", ep->url);
		json_object_set_string(ep_obj, "description", ep->description);
		json_object_set_number(ep_obj, "duration", ep->duration_sec);
		json_object_set_number(ep_obj, "pub_date", ep->pub_date);
		json_object_set_number(ep_obj, "progress", ep->progress_sec);
		json_object_set_boolean(ep_obj, "downloaded", ep->downloaded);
		if (ep->local_path[0]) {
			json_object_set_string(ep_obj, "local_path", ep->local_path);
		}
		json_object_set_boolean(ep_obj, "is_new", ep->is_new);

		json_array_append_value(arr, ep_val);
	}

	// Save to file
	char episodes_path[512];
	get_episodes_file_path(feed->feed_id, episodes_path, sizeof(episodes_path));
	int result = json_serialize_to_file_pretty(root, episodes_path);
	json_value_free(root);

	if (result == JSONSuccess) {
		feed->episode_count = count;
		return 0;
	}

	LOG_error("[Podcast] Failed to save episodes to %s\n", episodes_path);
	return -1;
}

// Load a page of episodes from JSON file into cache
int Podcast_loadEpisodePage(int feed_index, int offset) {
	if (feed_index < 0 || feed_index >= subscription_count || offset < 0) {
		return 0;
	}

	PodcastFeed* feed = &subscriptions[feed_index];

	set_feed_id(feed);

	char episodes_path[512];
	get_episodes_file_path(feed->feed_id, episodes_path, sizeof(episodes_path));

	JSON_Value* root = json_parse_file(episodes_path);
	if (!root) {
		LOG_error("[Podcast] Failed to load episodes from %s\n", episodes_path);
		return 0;
	}

	JSON_Array* arr = json_value_get_array(root);
	if (!arr) {
		json_value_free(root);
		return 0;
	}

	int total = json_array_get_count(arr);
	feed->episode_count = total; // Update total count

	pthread_mutex_lock(&episode_cache_mutex);

	episode_cache_feed_index = feed_index;
	episode_cache_offset = offset;
	episode_cache_count = 0;

	for (int i = offset; i < total && episode_cache_count < PODCAST_EPISODE_PAGE_SIZE; i++) {
		JSON_Object* ep_obj = json_array_get_object(arr, i);
		if (!ep_obj)
			continue;

		PodcastEpisode* ep = &episode_cache[episode_cache_count];
		memset(ep, 0, sizeof(PodcastEpisode));

		const char* str;
		str = json_object_get_string(ep_obj, "guid");
		if (str)
			strncpy(ep->guid, str, PODCAST_MAX_GUID - 1);
		str = json_object_get_string(ep_obj, "title");
		if (str)
			strncpy(ep->title, str, PODCAST_MAX_TITLE - 1);
		str = json_object_get_string(ep_obj, "url");
		if (str)
			strncpy(ep->url, str, PODCAST_MAX_URL - 1);
		str = json_object_get_string(ep_obj, "description");
		if (str)
			strncpy(ep->description, str, PODCAST_MAX_DESCRIPTION - 1);
		str = json_object_get_string(ep_obj, "local_path");
		if (str)
			strncpy(ep->local_path, str, PODCAST_MAX_URL - 1);

		ep->duration_sec = (int)json_object_get_number(ep_obj, "duration");
		ep->pub_date = (uint32_t)json_object_get_number(ep_obj, "pub_date");
		ep->progress_sec = (int)json_object_get_number(ep_obj, "progress");
		ep->downloaded = json_object_get_boolean(ep_obj, "downloaded");
		ep->is_new = (json_object_get_boolean(ep_obj, "is_new") == 1);

		// Cross-reference with progress.json (more recent than episodes.json)
		int cached_progress = Podcast_getProgress(feed->feed_url, ep->guid);
		if (cached_progress != 0) {
			ep->progress_sec = cached_progress;
		}

		episode_cache_count++;
	}

	pthread_mutex_unlock(&episode_cache_mutex);
	json_value_free(root);

	return episode_cache_count;
}

// Get episode by index (loads from cache, auto-loads page if needed)
PodcastEpisode* Podcast_getEpisode(int feed_index, int episode_index) {
	if (feed_index < 0 || feed_index >= subscription_count || episode_index < 0) {
		return NULL;
	}

	PodcastFeed* feed = &subscriptions[feed_index];
	if (episode_index >= feed->episode_count) {
		return NULL;
	}

	pthread_mutex_lock(&episode_cache_mutex);

	// Check if we need to load a different page
	if (episode_cache_feed_index != feed_index ||
		episode_index < episode_cache_offset ||
		episode_index >= episode_cache_offset + episode_cache_count) {
		pthread_mutex_unlock(&episode_cache_mutex);

		// Calculate page offset (align to page boundaries for better caching)
		int page_offset = (episode_index / PODCAST_EPISODE_PAGE_SIZE) * PODCAST_EPISODE_PAGE_SIZE;
		Podcast_loadEpisodePage(feed_index, page_offset);

		pthread_mutex_lock(&episode_cache_mutex);
	}

	// Get from cache
	int cache_index = episode_index - episode_cache_offset;
	PodcastEpisode* result = NULL;
	if (cache_index >= 0 && cache_index < episode_cache_count) {
		result = &episode_cache[cache_index];
	}

	pthread_mutex_unlock(&episode_cache_mutex);
	return result;
}

void Podcast_invalidateEpisodeCache(void) {
	pthread_mutex_lock(&episode_cache_mutex);
	episode_cache_feed_index = -1;
	episode_cache_offset = 0;
	episode_cache_count = 0;
	pthread_mutex_unlock(&episode_cache_mutex);
}

int Podcast_getEpisodeCount(int feed_index) {
	if (feed_index < 0 || feed_index >= subscription_count) {
		return 0;
	}
	return subscriptions[feed_index].episode_count;
}

// External RSS parser functions (in podcast_rss.c)
extern int podcast_rss_parse(const char* xml_data, int xml_len, PodcastFeed* feed);

// External search functions (in podcast_search.c)
extern int podcast_search_itunes(const char* query, PodcastSearchResult* results, int max_results);
extern int podcast_search_lookup(const char* itunes_id, char* feed_url, int feed_url_size);
extern int podcast_search_lookup_full(const char* itunes_id, char* feed_url, int feed_url_size,
									  char* artwork_url, int artwork_url_size);
extern int podcast_charts_fetch(const char* country_code, PodcastChartItem* top, int* top_count,
								PodcastChartItem* new_items, int* new_count, int max_items);
extern int podcast_charts_filter_premium(PodcastChartItem* items, int count, int max_items);

// Check if image data is complete (JPEG ends with FF D9, PNG ends with IEND)
static bool is_image_data_complete(const uint8_t* data, int size) {
	if (size < 4)
		return false;
	// JPEG: starts with FF D8, ends with FF D9
	if (data[0] == 0xFF && data[1] == 0xD8) {
		return (data[size - 2] == 0xFF && data[size - 1] == 0xD9);
	}
	// PNG: starts with 89 50 4E 47, ends with IEND chunk (AE 42 60 82)
	if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) {
		return (size >= 8 &&
				data[size - 4] == 0xAE && data[size - 3] == 0x42 &&
				data[size - 2] == 0x60 && data[size - 1] == 0x82);
	}
	return true; // Unknown format — assume complete
}

// Validate a cached image file on disk. Returns true if valid, false if corrupt (and deletes it).
static bool validate_cached_image(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;

	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	if (fsize < 4) {
		fclose(f);
		remove(path);
		return false;
	}

	// Read just the header and tail bytes for validation
	uint8_t header[4], tail[4];
	fseek(f, 0, SEEK_SET);
	fread(header, 1, 4, f);
	fseek(f, fsize - 4, SEEK_SET);
	fread(tail, 1, 4, f);
	fclose(f);

	// Check JPEG: FF D8 ... FF D9
	if (header[0] == 0xFF && header[1] == 0xD8) {
		if (tail[2] == 0xFF && tail[3] == 0xD9)
			return true;
		remove(path);
		return false;
	}
	// Check PNG: 89 50 4E 47 ... AE 42 60 82
	if (header[0] == 0x89 && header[1] == 0x50 && header[2] == 0x4E && header[3] == 0x47) {
		if (tail[0] == 0xAE && tail[1] == 0x42 && tail[2] == 0x60 && tail[3] == 0x82)
			return true;
		remove(path);
		return false;
	}
	return true; // Unknown format
}

// Download artwork image to feed's data directory
static void download_feed_artwork(PodcastFeed* feed) {
	if (!feed || !feed->artwork_url[0] || !feed->feed_id[0])
		return;

	char feed_dir[512];
	Podcast_getFeedDataPath(feed->feed_id, feed_dir, sizeof(feed_dir));

	char art_path[768];
	snprintf(art_path, sizeof(art_path), "%s/artwork.jpg", feed_dir);

	// Skip if already cached and valid
	if (validate_cached_image(art_path))
		return;

	// Fetch and save (only if image is complete)
	uint8_t* buf = (uint8_t*)malloc(1024 * 1024);
	if (!buf)
		return;

	int size = wget_fetch(feed->artwork_url, buf, 1024 * 1024);
	if (size > 0 && is_image_data_complete(buf, size)) {
		FILE* f = fopen(art_path, "wb");
		if (f) {
			fwrite(buf, 1, size, f);
			fclose(f);
		}
	}
	free(buf);
}

// ============================================================================
// Initialization
// ============================================================================

int Podcast_init(void) {
	if (podcast_initialized)
		return 0; // Already initialized

	snprintf(podcast_data_dir, sizeof(podcast_data_dir), "%s/" PODCAST_DATA_DIR, SHARED_USERDATA_PATH);
	snprintf(subscriptions_file, sizeof(subscriptions_file), "%s/" PODCAST_SUBSCRIPTIONS_FILE, podcast_data_dir);
	snprintf(progress_file, sizeof(progress_file), "%s/progress.json", podcast_data_dir);
	snprintf(downloads_file, sizeof(downloads_file), "%s/downloads.json", podcast_data_dir);
	snprintf(charts_cache_file, sizeof(charts_cache_file), "%s/charts.json", podcast_data_dir);
	snprintf(continue_listening_file, sizeof(continue_listening_file), "%s/continue_listening.json", podcast_data_dir);
	snprintf(download_dir, sizeof(download_dir), "%s/Podcasts", SDCARD_PATH);

	// Create podcast data directory
	mkdir_recursive(podcast_data_dir);

	// Create download directory
	mkdir(download_dir, 0755);

	// Detect country code from system timezone
	// /etc/localtime symlinks to /tmp/localtime which resolves to the actual timezone
	char tz_path[256] = {0};
	ssize_t len = readlink("/tmp/localtime", tz_path, sizeof(tz_path) - 1);
	if (len > 0) {
		tz_path[len] = '\0';
		const char* country = get_country_from_timezone(tz_path);
		if (country) {
			strncpy(charts_country_code, country, sizeof(charts_country_code) - 1);
			charts_country_code[sizeof(charts_country_code) - 1] = '\0';
		}
	} else {
		// Fallback: try LANG environment variable
		const char* lang = getenv("LANG");
		if (lang && strlen(lang) >= 5 && lang[2] == '_') {
			charts_country_code[0] = tolower(lang[3]);
			charts_country_code[1] = tolower(lang[4]);
			charts_country_code[2] = '\0';
		}
	}

	// Validate country code - if not supported by Apple Podcast, fallback to US
	if (!is_apple_podcast_country(charts_country_code)) {
		strcpy(charts_country_code, "us");
	}

	// Load saved data
	Podcast_loadSubscriptions();
	Podcast_loadDownloadQueue();

	// Auto-resume pending downloads if WiFi is already connected
	if (download_queue_count > 0 && Wifi_isConnected()) {
		Podcast_startDownloads();
	}

	// Load progress entries
	JSON_Value* root = json_parse_file(progress_file);
	if (root) {
		JSON_Array* arr = json_value_get_array(root);
		if (arr) {
			int count = json_array_get_count(arr);
			for (int i = 0; i < count && progress_entry_count < MAX_PROGRESS_ENTRIES; i++) {
				JSON_Object* obj = json_array_get_object(arr, i);
				if (obj) {
					const char* feed = json_object_get_string(obj, "feed_url");
					const char* guid = json_object_get_string(obj, "guid");
					int pos = (int)json_object_get_number(obj, "position");
					if (feed && guid) {
						strncpy(progress_entries[progress_entry_count].feed_url, feed, PODCAST_MAX_URL - 1);
						strncpy(progress_entries[progress_entry_count].episode_guid, guid, PODCAST_MAX_GUID - 1);
						progress_entries[progress_entry_count].position_sec = pos;
						progress_entry_count++;
					}
				}
			}
		}
		json_value_free(root);
	}

	// Load and validate continue listening entries
	load_continue_listening();
	validate_continue_listening();

	podcast_initialized = true;
	return 0;
}

void Podcast_cleanup(void) {
	// Stop any running operations
	Podcast_cancelSearch();
	Podcast_stopDownloads();
	Podcast_stop();

	// Wait for refresh thread to finish
	for (int i = 0; i < 20 && refresh_running; i++) {
		usleep(100000);
	}

	// Save state
	Podcast_saveSubscriptions();
	Podcast_saveDownloadQueue();
	save_continue_listening();

	// Save progress
	Podcast_flushProgress();

	// Clear UI caches
	Podcast_clearThumbnailCache();

	// Allow full re-initialization on next entry (including auto-resume of downloads)
	podcast_initialized = false;
}

const char* Podcast_getError(void) {
	return error_message;
}

void Podcast_update(void) {
	// Check for completed async operations
	if (search_status.searching && !search_running) {
		search_status.searching = false;
		search_status.completed = true;
	}
	if (charts_status.loading && !charts_running) {
		charts_status.loading = false;
		charts_status.completed = true;
	}
}

// ============================================================================
// Subscription Management
// ============================================================================

int Podcast_getSubscriptionCount(void) {
	return subscription_count;
}

PodcastFeed* Podcast_getSubscriptions(int* count) {
	if (count)
		*count = subscription_count;
	return subscriptions;
}

PodcastFeed* Podcast_getSubscription(int index) {
	if (index < 0 || index >= subscription_count)
		return NULL;
	return &subscriptions[index];
}

int Podcast_subscribe(const char* feed_url) {
	if (!feed_url || subscription_count >= PODCAST_MAX_SUBSCRIPTIONS) {
		return -1;
	}

	// Check if already subscribed
	if (Podcast_isSubscribed(feed_url)) {
		return 0; // Already subscribed, not an error
	}

	// Fetch the feed
	uint8_t* buffer = (uint8_t*)malloc(5 * 1024 * 1024); // 5MB buffer for large RSS feeds
	if (!buffer) {
		snprintf(error_message, sizeof(error_message), "Out of memory");
		return -1;
	}

	int bytes = wget_fetch(feed_url, buffer, 5 * 1024 * 1024);
	if (bytes <= 0) {
		LOG_error("[Podcast] Failed to fetch feed: %s\n", feed_url);
		free(buffer);
		snprintf(error_message, sizeof(error_message), "Failed to fetch feed");
		return -1;
	}

	// Allocate temporary episodes array (parse all episodes from feed)
	// Using 2000 as a reasonable max - most podcasts have far fewer
	int max_episodes = 2000;
	PodcastEpisode* temp_episodes = (PodcastEpisode*)malloc(max_episodes * sizeof(PodcastEpisode));
	if (!temp_episodes) {
		free(buffer);
		snprintf(error_message, sizeof(error_message), "Out of memory");
		return -1;
	}

	// Parse RSS into feed and episodes array
	PodcastFeed temp_feed;
	memset(&temp_feed, 0, sizeof(PodcastFeed));
	strncpy(temp_feed.feed_url, feed_url, PODCAST_MAX_URL - 1);

	int episode_count = 0;
	int result = podcast_rss_parse_with_episodes((const char*)buffer, bytes, &temp_feed,
												 temp_episodes, max_episodes, &episode_count);
	free(buffer);

	if (result != 0) {
		LOG_error("[Podcast] Failed to parse feed: %s\n", feed_url);
		free(temp_episodes);
		snprintf(error_message, sizeof(error_message), "Invalid RSS feed");
		return -1;
	}

	set_feed_id(&temp_feed);
	temp_feed.last_updated = (uint32_t)time(NULL);
	temp_feed.episode_count = episode_count;

	// Add to subscriptions
	pthread_mutex_lock(&subscriptions_mutex);
	int feed_index = subscription_count;
	memcpy(&subscriptions[feed_index], &temp_feed, sizeof(PodcastFeed));
	subscription_count++;
	pthread_mutex_unlock(&subscriptions_mutex);

	// Save episodes to disk
	if (episode_count > 0) {
		Podcast_saveEpisodes(feed_index, temp_episodes, episode_count);
	}
	free(temp_episodes);

	Podcast_saveSubscriptions();

	// Fetch artwork image now so thumbnails are available immediately
	download_feed_artwork(&subscriptions[feed_index]);

	return 0;
}

int Podcast_subscribeFromItunes(const char* itunes_id) {
	if (!itunes_id) {
		LOG_error("[Podcast] subscribeFromItunes: null itunes_id\n");
		return -1;
	}


	// Check if already subscribed by iTunes ID
	if (Podcast_isSubscribedByItunesId(itunes_id)) {
		return 0; // Already subscribed
	}

	char feed_url[PODCAST_MAX_URL];
	char artwork_url[PODCAST_MAX_URL] = {0};

	// Get both feed URL and artwork URL from iTunes lookup
	if (podcast_search_lookup_full(itunes_id, feed_url, sizeof(feed_url),
								   artwork_url, sizeof(artwork_url)) != 0) {
		LOG_error("[Podcast] subscribeFromItunes: lookup failed for itunes_id=%s\n", itunes_id);
		snprintf(error_message, sizeof(error_message), "Failed to lookup podcast");
		return -1;
	}


	int result = Podcast_subscribe(feed_url);

	if (result == 0 && subscription_count > 0) {
		PodcastFeed* feed = &subscriptions[subscription_count - 1];
		// Store iTunes ID
		strncpy(feed->itunes_id, itunes_id, sizeof(feed->itunes_id) - 1);
		// Always prefer iTunes artwork (guaranteed 400x400) over RSS artwork
		if (artwork_url[0]) {
			strncpy(feed->artwork_url, artwork_url, sizeof(feed->artwork_url) - 1);
		}
		Podcast_saveSubscriptions(); // Save again with iTunes ID and artwork
		// Fetch artwork if RSS didn't have one but iTunes does
		download_feed_artwork(feed);
	}
	return result;
}

// Recursively remove a directory and all its contents
static void remove_directory_recursive(const char* path) {
	DIR* dir = opendir(path);
	if (!dir)
		return;

	struct dirent* entry;
	char filepath[512];
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		snprintf(filepath, sizeof(filepath), "%s/%s", path, entry->d_name);
		struct stat st;
		if (stat(filepath, &st) == 0 && S_ISDIR(st.st_mode)) {
			remove_directory_recursive(filepath);
		} else {
			unlink(filepath);
		}
	}
	closedir(dir);
	rmdir(path);
}

int Podcast_unsubscribe(int index) {
	if (index < 0 || index >= subscription_count)
		return -1;

	const char* feed_url = subscriptions[index].feed_url;

	// Remove continue listening entries for this feed
	for (int i = continue_listening_count - 1; i >= 0; i--) {
		if (strcmp(continue_listening[i].feed_url, feed_url) == 0) {
			for (int j = i; j < continue_listening_count - 1; j++) {
				memcpy(&continue_listening[j], &continue_listening[j + 1], sizeof(ContinueListeningEntry));
			}
			continue_listening_count--;
		}
	}
	save_continue_listening();

	// Cancel/remove all download queue entries for this feed
	pthread_mutex_lock(&download_mutex);
	bool had_active_download = false;
	int write_idx = 0;
	for (int i = 0; i < download_queue_count; i++) {
		if (strcmp(download_queue[i].feed_url, feed_url) == 0) {
			if (download_queue[i].status == PODCAST_DOWNLOAD_DOWNLOADING) {
				had_active_download = true;
			}
			continue; // Skip (remove) this entry
		}
		if (write_idx != i) {
			memcpy(&download_queue[write_idx], &download_queue[i], sizeof(PodcastDownloadItem));
		}
		write_idx++;
	}
	download_queue_count = write_idx;
	if (had_active_download) {
		download_should_stop = true;
	}
	pthread_mutex_unlock(&download_mutex);
	Podcast_saveDownloadQueue();

	// Delete downloaded audio files for this feed
	char safe_feed[256];
	strncpy(safe_feed, subscriptions[index].title, sizeof(safe_feed) - 1);
	safe_feed[sizeof(safe_feed) - 1] = '\0';
	sanitize_for_filename(safe_feed);

	char feed_download_dir[512];
	snprintf(feed_download_dir, sizeof(feed_download_dir), "%s/%s", download_dir, safe_feed);
	remove_directory_recursive(feed_download_dir);

	pthread_mutex_lock(&subscriptions_mutex);

	// Shift remaining subscriptions
	for (int i = index; i < subscription_count - 1; i++) {
		memcpy(&subscriptions[i], &subscriptions[i + 1], sizeof(PodcastFeed));
	}
	subscription_count--;

	pthread_mutex_unlock(&subscriptions_mutex);

	Podcast_saveSubscriptions();
	return 0;
}

bool Podcast_isSubscribed(const char* feed_url) {
	if (!feed_url)
		return false;
	for (int i = 0; i < subscription_count; i++) {
		if (strcmp(subscriptions[i].feed_url, feed_url) == 0) {
			return true;
		}
	}
	return false;
}

bool Podcast_isSubscribedByItunesId(const char* itunes_id) {
	if (!itunes_id || !itunes_id[0])
		return false;
	for (int i = 0; i < subscription_count; i++) {
		if (subscriptions[i].itunes_id[0] && strcmp(subscriptions[i].itunes_id, itunes_id) == 0) {
			return true;
		}
	}
	return false;
}

int Podcast_refreshFeed(int index) {
	if (index < 0 || index >= subscription_count)
		return -1;

	PodcastFeed* feed = &subscriptions[index];

	uint8_t* buffer = (uint8_t*)malloc(5 * 1024 * 1024); // 5MB buffer for large RSS feeds
	if (!buffer)
		return -1;

	int bytes = wget_fetch(feed->feed_url, buffer, 5 * 1024 * 1024);
	if (bytes <= 0) {
		free(buffer);
		return -1;
	}

	// Allocate temporary episodes array
	int max_episodes = 2000;
	PodcastEpisode* new_episodes = (PodcastEpisode*)malloc(max_episodes * sizeof(PodcastEpisode));
	if (!new_episodes) {
		free(buffer);
		return -1;
	}

	// Parse into temporary feed
	PodcastFeed temp_feed;
	memset(&temp_feed, 0, sizeof(temp_feed));
	strncpy(temp_feed.feed_url, feed->feed_url, PODCAST_MAX_URL - 1);

	int new_episode_count = 0;
	if (podcast_rss_parse_with_episodes((const char*)buffer, bytes, &temp_feed,
										new_episodes, max_episodes, &new_episode_count) == 0) {
		// Load existing episodes to preserve progress/downloaded status
		char episodes_path[512];
		set_feed_id(feed);
		get_episodes_file_path(feed->feed_id, episodes_path, sizeof(episodes_path));

		JSON_Value* old_root = json_parse_file(episodes_path);
		if (old_root) {
			JSON_Array* old_arr = json_value_get_array(old_root);
			if (old_arr) {
				int old_count = json_array_get_count(old_arr);
				// Preserve progress for matching episodes, detect new ones
				for (int i = 0; i < new_episode_count; i++) {
					bool found_in_old = false;
					for (int j = 0; j < old_count; j++) {
						JSON_Object* old_ep = json_array_get_object(old_arr, j);
						const char* old_guid = json_object_get_string(old_ep, "guid");
						if (old_guid && strcmp(new_episodes[i].guid, old_guid) == 0) {
							new_episodes[i].progress_sec = (int)json_object_get_number(old_ep, "progress");
							new_episodes[i].downloaded = json_object_get_boolean(old_ep, "downloaded");
							const char* local = json_object_get_string(old_ep, "local_path");
							if (local)
								strncpy(new_episodes[i].local_path, local, PODCAST_MAX_URL - 1);
							new_episodes[i].is_new = (json_object_get_boolean(old_ep, "is_new") == 1);
							found_in_old = true;
							break;
						}
					}
					if (!found_in_old) {
						new_episodes[i].is_new = true; // Brand new episode
					}
				}
			}
			json_value_free(old_root);
		}

		// Update feed metadata
		pthread_mutex_lock(&subscriptions_mutex);
		strncpy(feed->title, temp_feed.title, PODCAST_MAX_TITLE - 1);
		strncpy(feed->author, temp_feed.author, PODCAST_MAX_AUTHOR - 1);
		strncpy(feed->description, temp_feed.description, PODCAST_MAX_DESCRIPTION - 1);
		// Only update artwork if we didn't have it from iTunes
		if (!feed->artwork_url[0] && temp_feed.artwork_url[0]) {
			strncpy(feed->artwork_url, temp_feed.artwork_url, PODCAST_MAX_URL - 1);
		}
		feed->episode_count = new_episode_count;
		feed->last_updated = (uint32_t)time(NULL);
		pthread_mutex_unlock(&subscriptions_mutex);

		// Save new episodes to disk
		Podcast_saveEpisodes(index, new_episodes, new_episode_count);

		// Recount new_episode_count from the episodes we just saved
		int nc = 0;
		for (int i = 0; i < new_episode_count; i++) {
			if (new_episodes[i].is_new)
				nc++;
		}
		feed->new_episode_count = nc;

		// Invalidate cache if this feed was cached
		if (episode_cache_feed_index == index) {
			Podcast_invalidateEpisodeCache();
		}
	}

	free(new_episodes);
	free(buffer);
	return 0;
}

void Podcast_saveSubscriptions(void) {
	JSON_Value* root = json_value_init_array();
	JSON_Array* arr = json_value_get_array(root);

	pthread_mutex_lock(&subscriptions_mutex);
	for (int i = 0; i < subscription_count; i++) {
		PodcastFeed* feed = &subscriptions[i];

		// Generate feed_id if not set
		set_feed_id(feed);

		JSON_Value* feed_val = json_value_init_object();
		JSON_Object* feed_obj = json_value_get_object(feed_val);

		json_object_set_string(feed_obj, "feed_url", feed->feed_url);
		json_object_set_string(feed_obj, "feed_id", feed->feed_id);
		json_object_set_string(feed_obj, "itunes_id", feed->itunes_id);
		json_object_set_string(feed_obj, "title", feed->title);
		json_object_set_string(feed_obj, "author", feed->author);
		json_object_set_string(feed_obj, "description", feed->description);
		json_object_set_string(feed_obj, "artwork_url", feed->artwork_url);
		json_object_set_number(feed_obj, "last_updated", feed->last_updated);
		json_object_set_number(feed_obj, "episode_count", feed->episode_count);
		// Note: episodes are stored separately in <feed_id>/episodes.json
		// new_episode_count is computed dynamically from episodes.json

		json_array_append_value(arr, feed_val);
	}
	pthread_mutex_unlock(&subscriptions_mutex);

	json_serialize_to_file_pretty(root, subscriptions_file);
	json_value_free(root);
}

void Podcast_loadSubscriptions(void) {
	JSON_Value* root = json_parse_file(subscriptions_file);
	if (!root)
		return;

	JSON_Array* arr = json_value_get_array(root);
	if (!arr) {
		json_value_free(root);
		return;
	}

	pthread_mutex_lock(&subscriptions_mutex);
	subscription_count = 0;

	int count = json_array_get_count(arr);
	for (int i = 0; i < count && subscription_count < PODCAST_MAX_SUBSCRIPTIONS; i++) {
		JSON_Object* feed_obj = json_array_get_object(arr, i);
		if (!feed_obj)
			continue;

		PodcastFeed* feed = &subscriptions[subscription_count];
		memset(feed, 0, sizeof(PodcastFeed));

		const char* str;
		str = json_object_get_string(feed_obj, "feed_url");
		if (str)
			strncpy(feed->feed_url, str, PODCAST_MAX_URL - 1);
		str = json_object_get_string(feed_obj, "feed_id");
		if (str)
			strncpy(feed->feed_id, str, sizeof(feed->feed_id) - 1);
		str = json_object_get_string(feed_obj, "itunes_id");
		if (str)
			strncpy(feed->itunes_id, str, sizeof(feed->itunes_id) - 1);
		str = json_object_get_string(feed_obj, "title");
		if (str)
			strncpy(feed->title, str, PODCAST_MAX_TITLE - 1);
		str = json_object_get_string(feed_obj, "author");
		if (str)
			strncpy(feed->author, str, PODCAST_MAX_AUTHOR - 1);
		str = json_object_get_string(feed_obj, "description");
		if (str)
			strncpy(feed->description, str, PODCAST_MAX_DESCRIPTION - 1);
		str = json_object_get_string(feed_obj, "artwork_url");
		if (str)
			strncpy(feed->artwork_url, str, PODCAST_MAX_URL - 1);

		feed->last_updated = (uint32_t)json_object_get_number(feed_obj, "last_updated");
		feed->episode_count = (int)json_object_get_number(feed_obj, "episode_count");

		// Generate feed_id if not loaded (for backward compatibility)
		set_feed_id(feed);

		// Note: episodes are loaded on-demand via Podcast_getEpisode()

		subscription_count++;
	}
	pthread_mutex_unlock(&subscriptions_mutex);

	// Compute new_episode_count dynamically from each feed's episodes.json
	for (int i = 0; i < subscription_count; i++) {
		PodcastFeed* feed = &subscriptions[i];
		feed->new_episode_count = 0;
		char episodes_path[512];
		get_episodes_file_path(feed->feed_id, episodes_path, sizeof(episodes_path));
		JSON_Value* ep_root = json_parse_file(episodes_path);
		if (ep_root) {
			JSON_Array* ep_arr = json_value_get_array(ep_root);
			if (ep_arr) {
				int ep_count = json_array_get_count(ep_arr);
				for (int j = 0; j < ep_count; j++) {
					JSON_Object* ep_obj = json_array_get_object(ep_arr, j);
					if (ep_obj && json_object_get_boolean(ep_obj, "is_new") == 1) {
						feed->new_episode_count++;
					}
				}
			}
			json_value_free(ep_root);
		}
	}

	json_value_free(root);
}

// ============================================================================
// Search API
// ============================================================================

int Podcast_startSearch(const char* query) {
	if (!query || search_running) {
		return -1;
	}

	// Reset status
	memset(&search_status, 0, sizeof(search_status));
	search_status.searching = true;
	search_result_count = 0;

	strncpy(search_query_copy, query, sizeof(search_query_copy) - 1);
	search_query_copy[sizeof(search_query_copy) - 1] = '\0';

	search_should_stop = false;
	search_running = true;

	if (pthread_create(&search_thread, NULL, search_thread_func, NULL) != 0) {
		search_running = false;
		search_status.searching = false;
		snprintf(search_status.error_message, sizeof(search_status.error_message), "Failed to start search");
		return -1;
	}

	pthread_detach(search_thread);
	podcast_state = PODCAST_STATE_SEARCHING;
	return 0;
}

static void* search_thread_func(void* arg) {
	(void)arg;

	int count = podcast_search_itunes(search_query_copy, search_results, PODCAST_MAX_SEARCH_RESULTS);

	if (search_should_stop) {
		search_running = false;
		return NULL;
	}

	if (count < 0) {
		search_status.result_count = -1;
		snprintf(search_status.error_message, sizeof(search_status.error_message), "Search failed");
	} else {
		search_result_count = count;
		search_status.result_count = count;
	}

	search_running = false;
	podcast_state = PODCAST_STATE_IDLE;
	return NULL;
}

const PodcastSearchStatus* Podcast_getSearchStatus(void) {
	return &search_status;
}

PodcastSearchResult* Podcast_getSearchResults(int* count) {
	if (count)
		*count = search_result_count;
	return search_results;
}

void Podcast_cancelSearch(void) {
	if (search_running) {
		search_should_stop = true;
		// Wait briefly for thread to notice
		for (int i = 0; i < 10 && search_running; i++) {
			usleep(50000); // 50ms
		}
	}
	search_status.searching = false;
}

// ============================================================================
// Charts API
// ============================================================================

void Podcast_clearChartsCache(void) {
	// Remove the cache file to force refresh
	if (charts_cache_file[0]) {
		unlink(charts_cache_file);
	}
	// Clear in-memory data
	top_shows_count = 0;
	memset(&charts_status, 0, sizeof(charts_status));
}

int Podcast_loadCharts(const char* country_code) {
	if (charts_running) {
		return -1;
	}

	if (country_code) {
		strncpy(charts_country_code, country_code, sizeof(charts_country_code) - 1);
		charts_country_code[sizeof(charts_country_code) - 1] = '\0';
	}

	memset(&charts_status, 0, sizeof(charts_status));

	// Try to load from cache first (daily cache)
	if (load_charts_cache()) {
		// Cache hit - no need to fetch from network
		charts_status.top_shows_count = top_shows_count;
		charts_status.loading = false;
		charts_status.completed = true;
		return 0;
	}

	// Cache miss or expired - fetch from network
	charts_status.loading = true;
	charts_should_stop = false;
	charts_running = true;

	if (pthread_create(&charts_thread, NULL, charts_thread_func, NULL) != 0) {
		charts_running = false;
		charts_status.loading = false;
		snprintf(charts_status.error_message, sizeof(charts_status.error_message), "Failed to load charts");
		return -1;
	}

	pthread_detach(charts_thread);
	podcast_state = PODCAST_STATE_LOADING_CHARTS;
	return 0;
}

// Save charts to cache file
static void save_charts_cache(void) {
	JSON_Value* root = json_value_init_object();
	JSON_Object* obj = json_value_get_object(root);

	// Save timestamp and country
	json_object_set_number(obj, "timestamp", (double)time(NULL));
	json_object_set_string(obj, "country", charts_country_code);

	// Save top shows
	JSON_Value* top_arr_val = json_value_init_array();
	JSON_Array* top_arr = json_value_get_array(top_arr_val);
	for (int i = 0; i < top_shows_count; i++) {
		JSON_Value* item_val = json_value_init_object();
		JSON_Object* item = json_value_get_object(item_val);
		json_object_set_string(item, "itunes_id", top_shows[i].itunes_id);
		json_object_set_string(item, "title", top_shows[i].title);
		json_object_set_string(item, "author", top_shows[i].author);
		json_object_set_string(item, "artwork_url", top_shows[i].artwork_url);
		json_object_set_string(item, "genre", top_shows[i].genre);
		json_object_set_string(item, "feed_url", top_shows[i].feed_url);
		json_array_append_value(top_arr, item_val);
	}
	json_object_set_value(obj, "top_shows", top_arr_val);

	json_serialize_to_file_pretty(root, charts_cache_file);
	json_value_free(root);
}

// Load charts from cache if valid (within 24 hours and same country)
// Returns true if cache was loaded successfully
static bool load_charts_cache(void) {
	JSON_Value* root = json_parse_file(charts_cache_file);
	if (!root) {
		return false;
	}

	JSON_Object* obj = json_value_get_object(root);
	if (!obj) {
		json_value_free(root);
		return false;
	}

	// Check timestamp - cache valid for 24 hours
	double timestamp = json_object_get_number(obj, "timestamp");
	time_t now = time(NULL);
	time_t cache_age = now - (time_t)timestamp;
	if (cache_age > 24 * 60 * 60) { // 24 hours in seconds
		json_value_free(root);
		return false;
	}

	// Check country matches
	const char* cached_country = json_object_get_string(obj, "country");
	if (!cached_country || strcmp(cached_country, charts_country_code) != 0) {
		json_value_free(root);
		return false;
	}

	// Load top shows
	JSON_Array* top_arr = json_object_get_array(obj, "top_shows");
	if (top_arr) {
		top_shows_count = 0;
		int count = json_array_get_count(top_arr);
		for (int i = 0; i < count && top_shows_count < PODCAST_MAX_CHART_ITEMS; i++) {
			JSON_Object* item = json_array_get_object(top_arr, i);
			if (!item)
				continue;

			PodcastChartItem* show = &top_shows[top_shows_count];
			memset(show, 0, sizeof(PodcastChartItem));

			const char* s;
			if ((s = json_object_get_string(item, "itunes_id")))
				strncpy(show->itunes_id, s, sizeof(show->itunes_id) - 1);
			if ((s = json_object_get_string(item, "title")))
				strncpy(show->title, s, PODCAST_MAX_TITLE - 1);
			if ((s = json_object_get_string(item, "author")))
				strncpy(show->author, s, PODCAST_MAX_AUTHOR - 1);
			if ((s = json_object_get_string(item, "artwork_url")))
				strncpy(show->artwork_url, s, PODCAST_MAX_URL - 1);
			if ((s = json_object_get_string(item, "genre")))
				strncpy(show->genre, s, PODCAST_MAX_GENRE - 1);
			if ((s = json_object_get_string(item, "feed_url")))
				strncpy(show->feed_url, s, PODCAST_MAX_URL - 1);

			top_shows_count++;
		}
	}

	json_value_free(root);
	return (top_shows_count > 0);
}

static void* charts_thread_func(void* arg) {
	(void)arg;

	int top_count = 0;
	// Fetch more items than needed to have buffer after filtering premium podcasts
	int result = podcast_charts_fetch(charts_country_code, top_shows, &top_count,
									  NULL, NULL, PODCAST_CHART_FETCH_LIMIT);

	if (charts_should_stop) {
		charts_running = false;
		return NULL;
	}

	if (result < 0) {
		snprintf(charts_status.error_message, sizeof(charts_status.error_message), "Failed to fetch charts");
	} else {
		// Filter out premium podcasts and those without feed URLs
		top_count = podcast_charts_filter_premium(top_shows, top_count, PODCAST_MAX_CHART_ITEMS);

		top_shows_count = top_count;
		charts_status.top_shows_count = top_count;

		// Save to cache after successful fetch (only filtered results are saved)
		save_charts_cache();
	}

	charts_running = false;
	charts_status.loading = false;
	charts_status.completed = true;
	podcast_state = PODCAST_STATE_IDLE;
	return NULL;
}

const PodcastChartsStatus* Podcast_getChartsStatus(void) {
	return &charts_status;
}

PodcastChartItem* Podcast_getTopShows(int* count) {
	if (count)
		*count = top_shows_count;
	return top_shows;
}

const char* Podcast_getCountryCode(void) {
	return charts_country_code;
}

// ============================================================================
// Playback (local files only - streaming removed)
// ============================================================================

int Podcast_play(PodcastFeed* feed, int episode_index) {
	if (!feed || episode_index < 0 || episode_index >= feed->episode_count) {
		return -1;
	}

	int feed_idx = get_feed_index(feed);
	if (feed_idx < 0) {
		return -1;
	}

	PodcastEpisode* ep = Podcast_getEpisode(feed_idx, episode_index);
	if (!ep) {
		return -1;
	}

	// Check if local file exists
	char local_path[PODCAST_MAX_URL];
	Podcast_getEpisodeLocalPath(feed, episode_index, local_path, sizeof(local_path));

	if (access(local_path, F_OK) != 0) {
		snprintf(error_message, sizeof(error_message), "Episode not downloaded");
		return -1; // File doesn't exist - caller should start download
	}

	// Store current feed and episode for later reference
	current_feed = feed;
	current_feed_index = feed_idx;
	current_episode_index = episode_index;

	if (Player_load(local_path) == 0) {
		current_episode_duration_sec = ep->duration_sec;
		Player_play();
		return 0;
	}

	snprintf(error_message, sizeof(error_message), "Failed to load local file");
	return -1;
}

int Podcast_loadAndSeek(PodcastFeed* feed, int episode_index) {
	if (!feed || episode_index < 0 || episode_index >= feed->episode_count) {
		return -1;
	}

	int feed_idx = get_feed_index(feed);
	if (feed_idx < 0)
		return -1;

	PodcastEpisode* ep = Podcast_getEpisode(feed_idx, episode_index);
	if (!ep)
		return -1;

	char local_path[PODCAST_MAX_URL];
	Podcast_getEpisodeLocalPath(feed, episode_index, local_path, sizeof(local_path));

	if (access(local_path, F_OK) != 0) {
		snprintf(error_message, sizeof(error_message), "Episode not downloaded");
		return -1;
	}

	current_feed = feed;
	current_feed_index = feed_idx;
	current_episode_index = episode_index;

	if (Player_load(local_path) == 0) {
		current_episode_duration_sec = ep->duration_sec;
		if (ep->progress_sec > 0) {
			Player_seek(ep->progress_sec * 1000);
		}
		// Don't call Player_play() — caller waits for seek to finish
		return ep->progress_sec > 0 ? 1 : 0; // 1 = seeking, 0 = ready to play
	}

	snprintf(error_message, sizeof(error_message), "Failed to load local file");
	return -1;
}

void Podcast_stop(void) {
	if (current_feed && current_feed_index >= 0 && current_episode_index >= 0) {
		// Save progress
		PodcastEpisode* ep = Podcast_getEpisode(current_feed_index, current_episode_index);
		if (ep) {
			int position = Player_getPosition();
			if (position > 0) {
				ep->progress_sec = position / 1000; // Convert ms to sec
				Podcast_saveProgress(current_feed->feed_url, ep->guid, ep->progress_sec);
			}
		}
	}

	Player_stop();

	current_episode_duration_sec = 0;
	podcast_state = PODCAST_STATE_IDLE;
	current_feed = NULL;
	current_feed_index = -1;
	current_episode_index = -1;
}

int Podcast_getDuration(void) {
	// Return stored duration from episode metadata if available
	if (current_episode_duration_sec > 0) {
		return current_episode_duration_sec * 1000; // Convert to ms
	}
	return Player_getDuration();
}

bool Podcast_isActive(void) {
	// Podcast is active when playing a local file
	return current_feed != NULL && Player_getState() != PLAYER_STATE_STOPPED;
}

bool Podcast_isDownloading(void) {
	return download_running;
}

// ============================================================================
// Progress Tracking
// ============================================================================

void Podcast_saveProgress(const char* feed_url, const char* episode_guid, int position_sec) {
	if (!feed_url || !episode_guid)
		return;

	// Find existing entry or add new one
	for (int i = 0; i < progress_entry_count; i++) {
		if (strcmp(progress_entries[i].feed_url, feed_url) == 0 &&
			strcmp(progress_entries[i].episode_guid, episode_guid) == 0) {
			progress_entries[i].position_sec = position_sec;
			return;
		}
	}

	// Add new entry
	if (progress_entry_count < MAX_PROGRESS_ENTRIES) {
		strncpy(progress_entries[progress_entry_count].feed_url, feed_url, PODCAST_MAX_URL - 1);
		strncpy(progress_entries[progress_entry_count].episode_guid, episode_guid, PODCAST_MAX_GUID - 1);
		progress_entries[progress_entry_count].position_sec = position_sec;
		progress_entry_count++;
	}
}

int Podcast_getProgress(const char* feed_url, const char* episode_guid) {
	if (!feed_url || !episode_guid)
		return 0;

	for (int i = 0; i < progress_entry_count; i++) {
		if (strcmp(progress_entries[i].feed_url, feed_url) == 0 &&
			strcmp(progress_entries[i].episode_guid, episode_guid) == 0) {
			return progress_entries[i].position_sec;
		}
	}
	return 0;
}

void Podcast_markAsPlayed(const char* feed_url, const char* episode_guid) {
	// Mark as played by setting progress to -1 (special value)
	Podcast_saveProgress(feed_url, episode_guid, -1);
}

void Podcast_flushProgress(void) {
	JSON_Value* root = json_value_init_array();
	JSON_Array* arr = json_value_get_array(root);
	for (int i = 0; i < progress_entry_count; i++) {
		JSON_Value* item = json_value_init_object();
		JSON_Object* obj = json_value_get_object(item);
		json_object_set_string(obj, "feed_url", progress_entries[i].feed_url);
		json_object_set_string(obj, "guid", progress_entries[i].episode_guid);
		json_object_set_number(obj, "position", progress_entries[i].position_sec);
		json_array_append_value(arr, item);
	}
	json_serialize_to_file_pretty(root, progress_file);
	json_value_free(root);
}

// Helper to sanitize string for filesystem (removes problematic chars)
static void sanitize_for_filename(char* str) {
	for (char* p = str; *p; p++) {
		if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' ||
			*p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|' ||
			*p == '\'' || *p == '`' || *p == '$' || *p == '!' ||
			*p == '&' || *p == ';' || *p == '(' || *p == ')' ||
			*p == '{' || *p == '}' || *p == '[' || *p == ']' ||
			*p == '#' || *p == '~') {
			*p = '_';
		}
	}
}

// Generate local file path for an episode
void Podcast_getEpisodeLocalPath(PodcastFeed* feed, int episode_index, char* buf, int buf_size) {
	if (!feed || episode_index < 0 || episode_index >= feed->episode_count || !buf) {
		if (buf && buf_size > 0)
			buf[0] = '\0';
		return;
	}

	int feed_idx = get_feed_index(feed);
	PodcastEpisode* ep = (feed_idx >= 0) ? Podcast_getEpisode(feed_idx, episode_index) : NULL;
	if (!ep) {
		if (buf_size > 0)
			buf[0] = '\0';
		return;
	}

	char safe_title[256];
	strncpy(safe_title, ep->title, sizeof(safe_title) - 1);
	safe_title[sizeof(safe_title) - 1] = '\0';
	sanitize_for_filename(safe_title);

	char safe_feed[256];
	strncpy(safe_feed, feed->title, sizeof(safe_feed) - 1);
	safe_feed[sizeof(safe_feed) - 1] = '\0';
	sanitize_for_filename(safe_feed);

	snprintf(buf, buf_size, "%s/%s/%s.mp3", download_dir, safe_feed, safe_title);
}

// Check if episode file exists locally
bool Podcast_episodeFileExists(PodcastFeed* feed, int episode_index) {
	char local_path[PODCAST_MAX_URL];
	Podcast_getEpisodeLocalPath(feed, episode_index, local_path, sizeof(local_path));
	if (local_path[0] == '\0')
		return false;
	return access(local_path, F_OK) == 0;
}

// Get download status for a specific episode
int Podcast_getEpisodeDownloadStatus(const char* feed_url, const char* episode_guid, int* progress_out) {
	if (!feed_url || !episode_guid)
		return -1;

	pthread_mutex_lock(&download_mutex);
	for (int i = 0; i < download_queue_count; i++) {
		if (strcmp(download_queue[i].feed_url, feed_url) == 0 &&
			strcmp(download_queue[i].episode_guid, episode_guid) == 0) {
			int status = download_queue[i].status;
			int progress = download_queue[i].progress_percent;
			if (progress_out) {
				*progress_out = progress;
			}
			pthread_mutex_unlock(&download_mutex);
			return status;
		}
	}
	pthread_mutex_unlock(&download_mutex);

	if (progress_out)
		*progress_out = 0;
	return -1; // Not in queue
}

// Cancel a specific episode download
int Podcast_cancelEpisodeDownload(const char* feed_url, const char* episode_guid) {
	if (!feed_url || !episode_guid)
		return -1;

	pthread_mutex_lock(&download_mutex);
	for (int i = 0; i < download_queue_count; i++) {
		if (strcmp(download_queue[i].feed_url, feed_url) == 0 &&
			strcmp(download_queue[i].episode_guid, episode_guid) == 0) {
			// If currently downloading, signal stop
			if (download_queue[i].status == PODCAST_DOWNLOAD_DOWNLOADING) {
				download_should_stop = true;
			}
			// Remove from queue by shifting
			for (int j = i; j < download_queue_count - 1; j++) {
				memcpy(&download_queue[j], &download_queue[j + 1], sizeof(PodcastDownloadItem));
			}
			download_queue_count--;
			pthread_mutex_unlock(&download_mutex);
			Podcast_saveDownloadQueue();
			return 0;
		}
	}
	pthread_mutex_unlock(&download_mutex);
	return -1; // Not found
}

int Podcast_queueDownload(PodcastFeed* feed, int episode_index) {
	if (!feed || episode_index < 0 || episode_index >= feed->episode_count) {
		return -1;
	}
	if (download_queue_count >= PODCAST_MAX_DOWNLOAD_QUEUE) {
		return -1;
	}

	int feed_idx = get_feed_index(feed);
	PodcastEpisode* ep = (feed_idx >= 0) ? Podcast_getEpisode(feed_idx, episode_index) : NULL;
	if (!ep) {
		return -1;
	}

	// Check if already in download queue (only block if PENDING or DOWNLOADING)
	pthread_mutex_lock(&download_mutex);
	for (int i = 0; i < download_queue_count; i++) {
		if (strcmp(download_queue[i].episode_guid, ep->guid) == 0) {
			if (download_queue[i].status == PODCAST_DOWNLOAD_PENDING ||
				download_queue[i].status == PODCAST_DOWNLOAD_DOWNLOADING) {
				pthread_mutex_unlock(&download_mutex);
				return 0; // Already queued and active
			}
			// Remove completed/failed item to allow re-download
			for (int j = i; j < download_queue_count - 1; j++) {
				memcpy(&download_queue[j], &download_queue[j + 1], sizeof(PodcastDownloadItem));
			}
			download_queue_count--;
			break;
		}
	}

	PodcastDownloadItem* item = &download_queue[download_queue_count];
	memset(item, 0, sizeof(PodcastDownloadItem));

	strncpy(item->feed_title, feed->title, PODCAST_MAX_TITLE - 1);
	strncpy(item->feed_url, feed->feed_url, PODCAST_MAX_URL - 1);
	strncpy(item->episode_title, ep->title, PODCAST_MAX_TITLE - 1);
	strncpy(item->episode_guid, ep->guid, PODCAST_MAX_GUID - 1);
	strncpy(item->url, ep->url, PODCAST_MAX_URL - 1);

	// Generate local path
	Podcast_getEpisodeLocalPath(feed, episode_index, item->local_path, sizeof(item->local_path));

	item->status = PODCAST_DOWNLOAD_PENDING;
	item->progress_percent = 0;
	download_queue_count++;

	pthread_mutex_unlock(&download_mutex);

	Podcast_saveDownloadQueue();

	// Auto-start downloads if not already running
	if (!download_running) {
		Podcast_startDownloads();
	}

	return 0;
}

PodcastDownloadItem* Podcast_getDownloadQueue(int* count) {
	if (count)
		*count = download_queue_count;
	return download_queue;
}

static int Podcast_startDownloads(void) {
	if (download_running || download_queue_count == 0) {
		return -1;
	}

	memset(&download_progress, 0, sizeof(download_progress));
	download_progress.total_items = download_queue_count;

	download_should_stop = false;
	download_running = true;

	if (pthread_create(&download_thread, NULL, download_thread_func, NULL) != 0) {
		LOG_error("[Podcast] Failed to create download thread\n");
		download_running = false;
		return -1;
	}

	pthread_detach(download_thread);
	podcast_state = PODCAST_STATE_DOWNLOADING;
	return 0;
}


#define PODCAST_MAX_RETRIES 3

static void* download_thread_func(void* arg) {
	(void)arg;

	// Prevent device auto-sleep during downloads
	ModuleCommon_setAutosleepDisabled(true);

	for (int i = 0; i < download_queue_count && !download_should_stop; i++) {
		PodcastDownloadItem* item = &download_queue[i];

		if (item->status != PODCAST_DOWNLOAD_PENDING) {
			continue;
		}

		download_progress.current_index = i;
		strncpy(download_progress.current_title, item->episode_title, PODCAST_MAX_TITLE - 1);
		item->status = PODCAST_DOWNLOAD_DOWNLOADING;
		item->progress_percent = 0;
		item->retry_count = 0;
		download_progress.speed_bps = 0;
		download_progress.eta_sec = 0;

		// Create directory for podcast (sanitize feed title for directory name)
		char safe_feed[256];
		strncpy(safe_feed, item->feed_title, sizeof(safe_feed) - 1);
		safe_feed[sizeof(safe_feed) - 1] = '\0';
		sanitize_for_filename(safe_feed);

		char dir_path[512];
		snprintf(dir_path, sizeof(dir_path), "%s/%s", download_dir, safe_feed);
		mkdir(dir_path, 0755);

		// Check disk space before downloading
		struct statvfs fs_stat;
		if (statvfs(download_dir, &fs_stat) == 0) {
			unsigned long free_mb = (fs_stat.f_bavail * fs_stat.f_frsize) / (1024 * 1024);
			if (free_mb < 50) {
				item->status = PODCAST_DOWNLOAD_FAILED;
				snprintf(download_progress.error_message, sizeof(download_progress.error_message),
						 "Low disk space (%lu MB free)", free_mb);
				download_progress.failed_count++;
				LOG_error("[Podcast] Low disk space (%lu MB), skipping: %s\n", free_mb, item->episode_title);
				continue;
			}
		}

		// Retry loop with WiFi check and exponential backoff
		int retries = 0;
		int bytes = -1;
		while (retries < PODCAST_MAX_RETRIES && !download_should_stop) {
			// Check WiFi before each attempt
			if (!Wifi_ensureConnected(NULL, 0)) {
				LOG_error("[Podcast] No network connection (attempt %d/%d): %s\n",
						  retries + 1, PODCAST_MAX_RETRIES, item->episode_title);
				retries++;
				item->retry_count = retries;
				if (retries < PODCAST_MAX_RETRIES)
					usleep(2000000); // 2s backoff
				continue;
			}

			bytes = wget_download_file(item->url, item->local_path,
									   &item->progress_percent,
									   &download_should_stop,
									   &download_progress.speed_bps,
									   &download_progress.eta_sec);

			if (bytes > 0 || download_should_stop)
				break;

			retries++;
			item->retry_count = retries;
			LOG_error("[Podcast] Download attempt %d/%d failed: %s\n",
					  retries, PODCAST_MAX_RETRIES, item->episode_title);
			if (retries < PODCAST_MAX_RETRIES) {
				usleep(2000000 * retries); // Exponential backoff: 2s, 4s
			}
		}

		// Reset speed/ETA between downloads
		download_progress.speed_bps = 0;
		download_progress.eta_sec = 0;

		if (download_should_stop) {
			// Remove partial file if cancelled
			unlink(item->local_path);
			break;
		}

		if (bytes > 0) {
			item->status = PODCAST_DOWNLOAD_COMPLETE;
			item->progress_percent = 100;
			download_progress.completed_count++;
		} else {
			item->status = PODCAST_DOWNLOAD_FAILED;
			download_progress.failed_count++;
			// Remove partial file after all retries exhausted
			unlink(item->local_path);
			snprintf(download_progress.error_message, sizeof(download_progress.error_message),
					 "Download failed after %d attempts", PODCAST_MAX_RETRIES);
			LOG_error("[Podcast] Failed to download after %d retries: %s\n",
					  PODCAST_MAX_RETRIES, item->url);
		}
	}

	// Remove completed and failed items from queue, keep pending and interrupted
	pthread_mutex_lock(&download_mutex);
	int write_idx = 0;
	for (int i = 0; i < download_queue_count; i++) {
		if (download_queue[i].status == PODCAST_DOWNLOAD_PENDING ||
			download_queue[i].status == PODCAST_DOWNLOAD_DOWNLOADING) {
			// Reset interrupted downloads to pending
			if (download_queue[i].status == PODCAST_DOWNLOAD_DOWNLOADING) {
				download_queue[i].status = PODCAST_DOWNLOAD_PENDING;
				download_queue[i].progress_percent = 0;
			}
			if (write_idx != i) {
				memcpy(&download_queue[write_idx], &download_queue[i], sizeof(PodcastDownloadItem));
			}
			write_idx++;
		}
		// COMPLETE and FAILED items are removed (not copied)
	}
	download_queue_count = write_idx;
	pthread_mutex_unlock(&download_mutex);

	// Re-enable auto-sleep
	download_progress.speed_bps = 0;
	download_progress.eta_sec = 0;
	ModuleCommon_setAutosleepDisabled(false);

	download_running = false;
	podcast_state = PODCAST_STATE_IDLE;
	Podcast_saveDownloadQueue();
	return NULL;
}

void Podcast_stopDownloads(void) {
	if (download_running) {
		download_should_stop = true;
		for (int i = 0; i < 20 && download_running; i++) {
			usleep(100000); // 100ms, wait up to 2 seconds
		}
	}

	// Reset any DOWNLOADING items to PENDING so they resume on next app start
	pthread_mutex_lock(&download_mutex);
	for (int i = 0; i < download_queue_count; i++) {
		if (download_queue[i].status == PODCAST_DOWNLOAD_DOWNLOADING) {
			download_queue[i].status = PODCAST_DOWNLOAD_PENDING;
			download_queue[i].progress_percent = 0;
		}
	}
	pthread_mutex_unlock(&download_mutex);
}

const PodcastDownloadProgress* Podcast_getDownloadProgress(void) {
	return &download_progress;
}

void Podcast_saveDownloadQueue(void) {
	JSON_Value* root = json_value_init_array();
	JSON_Array* arr = json_value_get_array(root);

	pthread_mutex_lock(&download_mutex);
	for (int i = 0; i < download_queue_count; i++) {
		PodcastDownloadItem* item = &download_queue[i];

		// Don't persist completed or failed items
		if (item->status == PODCAST_DOWNLOAD_COMPLETE ||
			item->status == PODCAST_DOWNLOAD_FAILED) {
			continue;
		}

		JSON_Value* val = json_value_init_object();
		JSON_Object* obj = json_value_get_object(val);

		json_object_set_string(obj, "feed_title", item->feed_title);
		json_object_set_string(obj, "feed_url", item->feed_url);
		json_object_set_string(obj, "episode_title", item->episode_title);
		json_object_set_string(obj, "episode_guid", item->episode_guid);
		json_object_set_string(obj, "url", item->url);
		json_object_set_string(obj, "local_path", item->local_path);
		json_object_set_number(obj, "status", item->status);
		json_object_set_number(obj, "progress", item->progress_percent);

		json_array_append_value(arr, val);
	}
	pthread_mutex_unlock(&download_mutex);

	json_serialize_to_file_pretty(root, downloads_file);
	json_value_free(root);
}

void Podcast_loadDownloadQueue(void) {
	JSON_Value* root = json_parse_file(downloads_file);
	if (!root)
		return;

	JSON_Array* arr = json_value_get_array(root);
	if (!arr) {
		json_value_free(root);
		return;
	}

	pthread_mutex_lock(&download_mutex);
	download_queue_count = 0;

	int count = json_array_get_count(arr);
	for (int i = 0; i < count && download_queue_count < PODCAST_MAX_DOWNLOAD_QUEUE; i++) {
		JSON_Object* obj = json_array_get_object(arr, i);
		if (!obj)
			continue;

		PodcastDownloadItem* item = &download_queue[download_queue_count];
		memset(item, 0, sizeof(PodcastDownloadItem));

		const char* str;
		str = json_object_get_string(obj, "feed_title");
		if (str)
			strncpy(item->feed_title, str, PODCAST_MAX_TITLE - 1);
		str = json_object_get_string(obj, "feed_url");
		if (str)
			strncpy(item->feed_url, str, PODCAST_MAX_URL - 1);
		str = json_object_get_string(obj, "episode_title");
		if (str)
			strncpy(item->episode_title, str, PODCAST_MAX_TITLE - 1);
		str = json_object_get_string(obj, "episode_guid");
		if (str)
			strncpy(item->episode_guid, str, PODCAST_MAX_GUID - 1);
		str = json_object_get_string(obj, "url");
		if (str)
			strncpy(item->url, str, PODCAST_MAX_URL - 1);
		str = json_object_get_string(obj, "local_path");
		if (str)
			strncpy(item->local_path, str, PODCAST_MAX_URL - 1);

		item->status = (PodcastDownloadStatus)(int)json_object_get_number(obj, "status");
		item->progress_percent = (int)json_object_get_number(obj, "progress");

		// Reset downloading status to pending
		if (item->status == PODCAST_DOWNLOAD_DOWNLOADING) {
			item->status = PODCAST_DOWNLOAD_PENDING;
			item->progress_percent = 0;
		}

		// Skip completed/failed items (don't load them into queue)
		if (item->status == PODCAST_DOWNLOAD_COMPLETE ||
			item->status == PODCAST_DOWNLOAD_FAILED) {
			continue; // Don't increment download_queue_count
		}

		download_queue_count++;
	}
	pthread_mutex_unlock(&download_mutex);

	json_value_free(root);
}

int Podcast_countDownloadedEpisodes(int feed_index) {
	if (feed_index < 0 || feed_index >= subscription_count) {
		return 0;
	}

	PodcastFeed* feed = &subscriptions[feed_index];
	int downloaded_count = 0;

	for (int i = 0; i < feed->episode_count; i++) {
		if (Podcast_episodeFileExists(feed, i)) {
			downloaded_count++;
		}
	}

	return downloaded_count;
}

int Podcast_getDownloadedEpisodeIndex(int feed_index, int episode_index) {
	if (feed_index < 0 || feed_index >= subscription_count) {
		return -1;
	}

	PodcastFeed* feed = &subscriptions[feed_index];
	if (episode_index < 0 || episode_index >= feed->episode_count) {
		return -1;
	}

	// Check if the current episode is downloaded
	if (!Podcast_episodeFileExists(feed, episode_index)) {
		return -1;
	}

	// Count how many downloaded episodes come before this one
	int index_among_downloaded = 0;
	for (int i = 0; i < episode_index; i++) {
		if (Podcast_episodeFileExists(feed, i)) {
			index_among_downloaded++;
		}
	}

	return index_among_downloaded;
}

// ============================================================================
// Background Feed Refresh
// ============================================================================

static void* refresh_thread_func(void* arg) {
	(void)arg;

	if (refresh_feed_index >= 0) {
		// Refresh single feed
		Podcast_refreshFeed(refresh_feed_index);
	} else {
		// Refresh all feeds - snapshot count to avoid race with unsubscribe
		pthread_mutex_lock(&subscriptions_mutex);
		int count = subscription_count;
		pthread_mutex_unlock(&subscriptions_mutex);
		for (int i = 0; i < count && i < subscription_count; i++) {
			if (!refresh_running)
				break;
			Podcast_refreshFeed(i);
		}
	}

	refresh_completed = true;
	refresh_running = false;
	return NULL;
}

int Podcast_startRefreshAll(void) {
	if (refresh_running)
		return -1;
	if (subscription_count == 0)
		return -1;

	// Cooldown check: skip if all feeds were updated recently
	time_t now = time(NULL);
	bool any_stale = false;
	for (int i = 0; i < subscription_count; i++) {
		if ((now - (time_t)subscriptions[i].last_updated) > REFRESH_COOLDOWN_SEC) {
			any_stale = true;
			break;
		}
	}
	if (!any_stale)
		return 0; // All feeds are fresh

	refresh_feed_index = -1;
	refresh_completed = false;
	refresh_running = true;

	if (pthread_create(&refresh_thread, NULL, refresh_thread_func, NULL) != 0) {
		refresh_running = false;
		return -1;
	}
	pthread_detach(refresh_thread);
	return 0;
}

int Podcast_startRefreshFeed(int index) {
	if (refresh_running)
		return -1;
	if (index < 0 || index >= subscription_count)
		return -1;

	refresh_feed_index = index;
	refresh_completed = false;
	refresh_running = true;

	if (pthread_create(&refresh_thread, NULL, refresh_thread_func, NULL) != 0) {
		refresh_running = false;
		return -1;
	}
	pthread_detach(refresh_thread);
	return 0;
}

bool Podcast_isRefreshing(void) {
	return refresh_running;
}

bool Podcast_checkRefreshCompleted(void) {
	if (refresh_completed) {
		refresh_completed = false;
		return true;
	}
	return false;
}

void Podcast_clearNewFlag(int feed_index, int episode_index) {
	if (feed_index < 0 || feed_index >= subscription_count)
		return;

	PodcastEpisode* ep = Podcast_getEpisode(feed_index, episode_index);
	if (!ep || !ep->is_new)
		return;

	// Copy GUID before releasing cache reference
	char guid_copy[PODCAST_MAX_GUID];
	strncpy(guid_copy, ep->guid, PODCAST_MAX_GUID - 1);
	guid_copy[PODCAST_MAX_GUID - 1] = '\0';

	// Update in-memory cache under lock
	pthread_mutex_lock(&episode_cache_mutex);
	ep->is_new = false;
	pthread_mutex_unlock(&episode_cache_mutex);

	PodcastFeed* feed = &subscriptions[feed_index];
	if (feed->new_episode_count > 0) {
		feed->new_episode_count--;
	}

	// Update the on-disk episodes.json
	set_feed_id(feed);
	char episodes_path[512];
	get_episodes_file_path(feed->feed_id, episodes_path, sizeof(episodes_path));

	JSON_Value* root = json_parse_file(episodes_path);
	if (root) {
		JSON_Array* arr = json_value_get_array(root);
		if (arr) {
			int total = json_array_get_count(arr);
			for (int i = 0; i < total; i++) {
				JSON_Object* obj = json_array_get_object(arr, i);
				const char* guid = json_object_get_string(obj, "guid");
				if (guid && strcmp(guid, guid_copy) == 0) {
					json_object_set_boolean(obj, "is_new", false);
					break;
				}
			}
			json_serialize_to_file_pretty(root, episodes_path);
		}
		json_value_free(root);
	}
}

// ============================================================================
// Continue Listening
// ============================================================================

int Podcast_findFeedIndex(const char* feed_url) {
	if (!feed_url)
		return -1;
	for (int i = 0; i < subscription_count; i++) {
		if (strcmp(subscriptions[i].feed_url, feed_url) == 0) {
			return i;
		}
	}
	return -1;
}

int Podcast_getContinueListeningCount(void) {
	return continue_listening_count;
}

ContinueListeningEntry* Podcast_getContinueListening(int index) {
	if (index < 0 || index >= continue_listening_count)
		return NULL;
	return &continue_listening[index];
}

void Podcast_updateContinueListening(const char* feed_url, const char* feed_id,
									 const char* episode_guid, const char* episode_title,
									 const char* feed_title, const char* artwork_url) {
	if (!feed_url || !episode_guid)
		return;

	// Check if this entry already exists
	for (int i = 0; i < continue_listening_count; i++) {
		if (strcmp(continue_listening[i].feed_url, feed_url) == 0 &&
			strcmp(continue_listening[i].episode_guid, episode_guid) == 0) {
			// Already exists — move to index 0 if not already there
			if (i > 0) {
				ContinueListeningEntry tmp;
				memcpy(&tmp, &continue_listening[i], sizeof(ContinueListeningEntry));
				for (int j = i; j > 0; j--) {
					memcpy(&continue_listening[j], &continue_listening[j - 1], sizeof(ContinueListeningEntry));
				}
				memcpy(&continue_listening[0], &tmp, sizeof(ContinueListeningEntry));
			}
			save_continue_listening();
			return;
		}
	}

	// New entry — shift existing entries down, insert at 0
	if (continue_listening_count < PODCAST_MAX_CONTINUE_LISTENING) {
		continue_listening_count++;
	}
	for (int j = continue_listening_count - 1; j > 0; j--) {
		memcpy(&continue_listening[j], &continue_listening[j - 1], sizeof(ContinueListeningEntry));
	}

	// Fill in index 0
	ContinueListeningEntry* entry = &continue_listening[0];
	memset(entry, 0, sizeof(ContinueListeningEntry));
	if (feed_url)
		strncpy(entry->feed_url, feed_url, PODCAST_MAX_URL - 1);
	if (feed_id)
		strncpy(entry->feed_id, feed_id, sizeof(entry->feed_id) - 1);
	if (episode_guid)
		strncpy(entry->episode_guid, episode_guid, PODCAST_MAX_GUID - 1);
	if (episode_title)
		strncpy(entry->episode_title, episode_title, PODCAST_MAX_TITLE - 1);
	if (feed_title)
		strncpy(entry->feed_title, feed_title, PODCAST_MAX_TITLE - 1);
	if (artwork_url)
		strncpy(entry->artwork_url, artwork_url, PODCAST_MAX_URL - 1);

	save_continue_listening();
}

void Podcast_removeContinueListening(const char* feed_url, const char* episode_guid) {
	if (!feed_url || !episode_guid)
		return;

	for (int i = 0; i < continue_listening_count; i++) {
		if (strcmp(continue_listening[i].feed_url, feed_url) == 0 &&
			strcmp(continue_listening[i].episode_guid, episode_guid) == 0) {
			for (int j = i; j < continue_listening_count - 1; j++) {
				memcpy(&continue_listening[j], &continue_listening[j + 1], sizeof(ContinueListeningEntry));
			}
			continue_listening_count--;
			save_continue_listening();
			return;
		}
	}
}

static void save_continue_listening(void) {
	JSON_Value* root = json_value_init_array();
	JSON_Array* arr = json_value_get_array(root);

	for (int i = 0; i < continue_listening_count; i++) {
		ContinueListeningEntry* e = &continue_listening[i];
		JSON_Value* val = json_value_init_object();
		JSON_Object* obj = json_value_get_object(val);

		json_object_set_string(obj, "feed_url", e->feed_url);
		json_object_set_string(obj, "feed_id", e->feed_id);
		json_object_set_string(obj, "episode_guid", e->episode_guid);
		json_object_set_string(obj, "episode_title", e->episode_title);
		json_object_set_string(obj, "feed_title", e->feed_title);
		json_object_set_string(obj, "artwork_url", e->artwork_url);

		json_array_append_value(arr, val);
	}

	json_serialize_to_file_pretty(root, continue_listening_file);
	json_value_free(root);
}

static void load_continue_listening(void) {
	continue_listening_count = 0;

	JSON_Value* root = json_parse_file(continue_listening_file);
	if (!root)
		return;

	JSON_Array* arr = json_value_get_array(root);
	if (!arr) {
		json_value_free(root);
		return;
	}

	int count = json_array_get_count(arr);
	for (int i = 0; i < count && continue_listening_count < PODCAST_MAX_CONTINUE_LISTENING; i++) {
		JSON_Object* obj = json_array_get_object(arr, i);
		if (!obj)
			continue;

		ContinueListeningEntry* e = &continue_listening[continue_listening_count];
		memset(e, 0, sizeof(ContinueListeningEntry));

		const char* s;
		if ((s = json_object_get_string(obj, "feed_url")))
			strncpy(e->feed_url, s, PODCAST_MAX_URL - 1);
		if ((s = json_object_get_string(obj, "feed_id")))
			strncpy(e->feed_id, s, sizeof(e->feed_id) - 1);
		if ((s = json_object_get_string(obj, "episode_guid")))
			strncpy(e->episode_guid, s, PODCAST_MAX_GUID - 1);
		if ((s = json_object_get_string(obj, "episode_title")))
			strncpy(e->episode_title, s, PODCAST_MAX_TITLE - 1);
		if ((s = json_object_get_string(obj, "feed_title")))
			strncpy(e->feed_title, s, PODCAST_MAX_TITLE - 1);
		if ((s = json_object_get_string(obj, "artwork_url")))
			strncpy(e->artwork_url, s, PODCAST_MAX_URL - 1);

		continue_listening_count++;
	}

	json_value_free(root);
}

static void validate_continue_listening(void) {
	for (int i = continue_listening_count - 1; i >= 0; i--) {
		ContinueListeningEntry* e = &continue_listening[i];

		// Check feed still exists (not unsubscribed)
		int feed_idx = Podcast_findFeedIndex(e->feed_url);
		if (feed_idx < 0) {
			// Feed no longer subscribed — remove
			for (int j = i; j < continue_listening_count - 1; j++) {
				memcpy(&continue_listening[j], &continue_listening[j + 1], sizeof(ContinueListeningEntry));
			}
			continue_listening_count--;
			continue;
		}

		// Check progress — remove if not in progress or completed
		int progress = Podcast_getProgress(e->feed_url, e->episode_guid);
		if (progress <= 0 || progress == -1) {
			for (int j = i; j < continue_listening_count - 1; j++) {
				memcpy(&continue_listening[j], &continue_listening[j + 1], sizeof(ContinueListeningEntry));
			}
			continue_listening_count--;
			continue;
		}

		// Check audio file exists
		PodcastFeed* feed = &subscriptions[feed_idx];
		bool file_found = false;
		for (int ei = 0; ei < feed->episode_count; ei++) {
			PodcastEpisode* ep = Podcast_getEpisode(feed_idx, ei);
			if (ep && strcmp(ep->guid, e->episode_guid) == 0) {
				if (Podcast_episodeFileExists(feed, ei)) {
					file_found = true;
				}
				break;
			}
		}
		if (!file_found) {
			for (int j = i; j < continue_listening_count - 1; j++) {
				memcpy(&continue_listening[j], &continue_listening[j + 1], sizeof(ContinueListeningEntry));
			}
			continue_listening_count--;
			continue;
		}
	}

	save_continue_listening();
}
