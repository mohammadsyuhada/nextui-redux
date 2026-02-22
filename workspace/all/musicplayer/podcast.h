#ifndef __PODCAST_H__
#define __PODCAST_H__

#include <stdint.h>
#include <stdbool.h>

// Limits
#define PODCAST_MAX_SUBSCRIPTIONS 50
#define PODCAST_MAX_SEARCH_RESULTS 50
#define PODCAST_MAX_CHART_ITEMS 25
#define PODCAST_CHART_FETCH_LIMIT 50 // Fetch more to filter out premium podcasts
#define PODCAST_MAX_DOWNLOAD_QUEUE 50
#define PODCAST_MAX_URL 512
#define PODCAST_MAX_TITLE 256
#define PODCAST_MAX_AUTHOR 128
#define PODCAST_MAX_DESCRIPTION 1024
#define PODCAST_MAX_GUID 128
#define PODCAST_MAX_GENRE 64

// Continue Listening
#define PODCAST_MAX_CONTINUE_LISTENING 10
#define PODCAST_CONTINUE_LISTENING_DISPLAY 2

typedef struct {
	char feed_url[PODCAST_MAX_URL];
	char feed_id[17];
	char episode_guid[PODCAST_MAX_GUID];
	char episode_title[PODCAST_MAX_TITLE];
	char feed_title[PODCAST_MAX_TITLE];
	char artwork_url[PODCAST_MAX_URL];
} ContinueListeningEntry;

// Episode pagination - only load this many into memory at a time
#define PODCAST_EPISODE_PAGE_SIZE 50

// Data paths (relative to SDCARD_PATH/.userdata/tg5040/)
#define PODCAST_DATA_DIR "music-player/podcast"
#define PODCAST_SUBSCRIPTIONS_FILE "subscriptions.json"

// Podcast episode
typedef struct {
	char guid[PODCAST_MAX_GUID];
	char title[PODCAST_MAX_TITLE];
	char url[PODCAST_MAX_URL]; // Audio file URL
	char description[PODCAST_MAX_DESCRIPTION];
	int duration_sec;
	uint32_t pub_date; // Unix timestamp
	int progress_sec;  // Resume position
	bool downloaded;
	char local_path[PODCAST_MAX_URL];
	bool is_new; // True if episode was added by a refresh (not initial subscribe)
} PodcastEpisode;

// Podcast feed (subscription) - episodes stored separately on disk
typedef struct {
	char feed_url[PODCAST_MAX_URL];
	char feed_id[17];	// 16-char hex hash of feed_url + null
	char itunes_id[32]; // iTunes ID for subscription tracking
	char title[PODCAST_MAX_TITLE];
	char author[PODCAST_MAX_AUTHOR];
	char description[PODCAST_MAX_DESCRIPTION];
	char artwork_url[PODCAST_MAX_URL];
	int episode_count;	   // Total episodes (stored on disk)
	uint32_t last_updated; // Unix timestamp
	int new_episode_count; // Count of episodes with is_new == true
} PodcastFeed;

// iTunes search result
typedef struct {
	char itunes_id[32];
	char title[PODCAST_MAX_TITLE];
	char author[PODCAST_MAX_AUTHOR];
	char artwork_url[PODCAST_MAX_URL];
	char feed_url[PODCAST_MAX_URL];
	char genre[PODCAST_MAX_GENRE];
} PodcastSearchResult;

// Chart item (for Top Shows)
typedef struct {
	char itunes_id[32]; // Apple ID for lookup
	char title[PODCAST_MAX_TITLE];
	char author[PODCAST_MAX_AUTHOR];
	char artwork_url[PODCAST_MAX_URL];
	char genre[PODCAST_MAX_GENRE];
	char feed_url[PODCAST_MAX_URL]; // Populated via lookup API
} PodcastChartItem;

// Download queue item
typedef enum {
	PODCAST_DOWNLOAD_PENDING = 0,
	PODCAST_DOWNLOAD_DOWNLOADING,
	PODCAST_DOWNLOAD_COMPLETE,
	PODCAST_DOWNLOAD_FAILED
} PodcastDownloadStatus;

typedef struct {
	char feed_title[PODCAST_MAX_TITLE];
	char feed_url[PODCAST_MAX_URL]; // For updating episode status
	char episode_title[PODCAST_MAX_TITLE];
	char episode_guid[PODCAST_MAX_GUID]; // For updating episode status
	char url[PODCAST_MAX_URL];
	char local_path[PODCAST_MAX_URL];
	PodcastDownloadStatus status;
	int progress_percent;
	int retry_count;
} PodcastDownloadItem;

// Podcast module states
typedef enum {
	PODCAST_STATE_IDLE = 0,
	PODCAST_STATE_LOADING,
	PODCAST_STATE_SEARCHING,
	PODCAST_STATE_LOADING_CHARTS,
	PODCAST_STATE_DOWNLOADING
} PodcastState;

// Search status
typedef struct {
	bool searching;
	bool completed;
	int result_count;
	char error_message[256];
} PodcastSearchStatus;

// Charts status
typedef struct {
	bool loading;
	bool completed;
	int top_shows_count;
	char error_message[256];
} PodcastChartsStatus;

// Download progress
typedef struct {
	PodcastState state;
	int current_index;
	int total_items;
	int completed_count;
	int failed_count;
	char current_title[PODCAST_MAX_TITLE];
	char error_message[256];
	int speed_bps;
	int eta_sec;
} PodcastDownloadProgress;

// ============================================================================
// Core API
// ============================================================================

// Initialize podcast module
int Podcast_init(void);

// Cleanup resources
void Podcast_cleanup(void);

// Get last error message
const char* Podcast_getError(void);

// Update function (call in main loop)
void Podcast_update(void);

// ============================================================================
// Subscription Management
// ============================================================================

// Get list of subscribed feeds
int Podcast_getSubscriptionCount(void);
PodcastFeed* Podcast_getSubscriptions(int* count);
PodcastFeed* Podcast_getSubscription(int index);

// Add subscription by RSS URL
int Podcast_subscribe(const char* feed_url);

// Subscribe from search/chart result (uses iTunes ID to lookup RSS)
int Podcast_subscribeFromItunes(const char* itunes_id);

// Unsubscribe by index
int Podcast_unsubscribe(int index);

// Check if already subscribed (by feed URL)
bool Podcast_isSubscribed(const char* feed_url);

// Check if already subscribed (by iTunes ID - for chart items)
bool Podcast_isSubscribedByItunesId(const char* itunes_id);

// Refresh a feed (fetch latest episodes)
int Podcast_refreshFeed(int index);

// Background feed refresh (non-blocking)
int Podcast_startRefreshAll(void);							  // Refresh all subscriptions in background
int Podcast_startRefreshFeed(int index);					  // Refresh single feed in background
bool Podcast_isRefreshing(void);							  // Check if refresh is in progress
bool Podcast_checkRefreshCompleted(void);					  // Check & clear completed flag (returns true once)
void Podcast_clearNewFlag(int feed_index, int episode_index); // Clear is_new on episode

// Save/load subscriptions
void Podcast_saveSubscriptions(void);
void Podcast_loadSubscriptions(void);

// ============================================================================
// Search API (iTunes)
// ============================================================================

// Start async search
int Podcast_startSearch(const char* query);

// Get search status
const PodcastSearchStatus* Podcast_getSearchStatus(void);

// Get search results (valid after search completes)
PodcastSearchResult* Podcast_getSearchResults(int* count);

// Cancel ongoing search
void Podcast_cancelSearch(void);

// ============================================================================
// Charts API (Apple Podcast Charts)
// ============================================================================

// Start loading charts (Top Shows)
int Podcast_loadCharts(const char* country_code);

// Clear charts cache (forces fresh fetch on next load)
void Podcast_clearChartsCache(void);

// Get charts status
const PodcastChartsStatus* Podcast_getChartsStatus(void);

// Get Top Shows
PodcastChartItem* Podcast_getTopShows(int* count);

// Get country code (uses device locale or default)
const char* Podcast_getCountryCode(void);

// ============================================================================
// Playback (Streaming)
// ============================================================================

// Play a downloaded episode
int Podcast_play(PodcastFeed* feed, int episode_index);

// Load episode and seek to saved position without starting playback
// Returns 0 on success. Caller should poll Player_resume() then call Player_play().
int Podcast_loadAndSeek(PodcastFeed* feed, int episode_index);

// Stop playback
void Podcast_stop(void);

// Get duration (uses episode metadata if available)
int Podcast_getDuration(void);

// Check if podcast audio is active
bool Podcast_isActive(void);

// Check if podcast download thread is running
bool Podcast_isDownloading(void);

// ============================================================================
// Progress Tracking
// ============================================================================

// Save episode progress
void Podcast_saveProgress(const char* feed_url, const char* episode_guid, int position_sec);

// Get episode progress
int Podcast_getProgress(const char* feed_url, const char* episode_guid);

// Mark episode as played
void Podcast_markAsPlayed(const char* feed_url, const char* episode_guid);

// Flush progress entries to progress.json immediately
void Podcast_flushProgress(void);

// ============================================================================
// Download Queue
// ============================================================================

// Add episode to download queue (auto-starts download if not running)
int Podcast_queueDownload(PodcastFeed* feed, int episode_index);

// Cancel a specific episode download (by feed URL and episode GUID)
int Podcast_cancelEpisodeDownload(const char* feed_url, const char* episode_guid);

// Get download status for a specific episode
// Returns: -1 if not in queue, otherwise PodcastDownloadStatus enum
// progress_out: set to 0-100 if downloading, 0 otherwise
int Podcast_getEpisodeDownloadStatus(const char* feed_url, const char* episode_guid, int* progress_out);

// Generate local file path for an episode (does not check if file exists)
void Podcast_getEpisodeLocalPath(PodcastFeed* feed, int episode_index, char* buf, int buf_size);

// Check if episode file exists locally
bool Podcast_episodeFileExists(PodcastFeed* feed, int episode_index);

// Get queue items
PodcastDownloadItem* Podcast_getDownloadQueue(int* count);

// Stop/cancel downloads
void Podcast_stopDownloads(void);

// Get download progress
const PodcastDownloadProgress* Podcast_getDownloadProgress(void);

// Save/load download queue
void Podcast_saveDownloadQueue(void);
void Podcast_loadDownloadQueue(void);

// Count how many episodes are downloaded for a feed
int Podcast_countDownloadedEpisodes(int feed_index);

// Get the index of an episode among downloaded episodes only
// Returns -1 if episode is not downloaded
int Podcast_getDownloadedEpisodeIndex(int feed_index, int episode_index);

// ============================================================================
// Episode Management (on-disk storage)
// ============================================================================

// Get episode by index (loads from disk cache if needed)
// Returns pointer to episode in internal cache, or NULL if invalid
PodcastEpisode* Podcast_getEpisode(int feed_index, int episode_index);

// Load a page of episodes into cache
// Returns number of episodes loaded
int Podcast_loadEpisodePage(int feed_index, int offset);

// Invalidate episode cache (call when switching feeds)
void Podcast_invalidateEpisodeCache(void);

// Save episodes to disk (called after RSS parse)
int Podcast_saveEpisodes(int feed_index, PodcastEpisode* episodes, int count);

// Get total episode count for a feed (from metadata, no disk read)
int Podcast_getEpisodeCount(int feed_index);

// Get path to feed's data directory
void Podcast_getFeedDataPath(const char* feed_id, char* path, int path_size);

// ============================================================================
// Continue Listening
// ============================================================================

int Podcast_getContinueListeningCount(void);
ContinueListeningEntry* Podcast_getContinueListening(int index);
void Podcast_updateContinueListening(const char* feed_url, const char* feed_id,
									 const char* episode_guid, const char* episode_title,
									 const char* feed_title, const char* artwork_url);
void Podcast_removeContinueListening(const char* feed_url, const char* episode_guid);
int Podcast_findFeedIndex(const char* feed_url);

// ============================================================================
// RSS Parser (podcast_rss.c)
// ============================================================================

// Parse RSS feed (simple - just feed metadata)
int podcast_rss_parse(const char* xml_data, int xml_len, PodcastFeed* feed);

// Parse RSS feed with episodes output
int podcast_rss_parse_with_episodes(const char* xml_data, int xml_len, PodcastFeed* feed,
									PodcastEpisode* episodes_out, int max_episodes,
									int* episode_count_out);

#endif // __PODCAST_H__
