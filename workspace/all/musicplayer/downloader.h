#ifndef __DOWNLOADER_H__
#define __DOWNLOADER_H__

#include <stdint.h>
#include <stdbool.h>

#define DOWNLOADER_MAX_RESULTS 30
#define DOWNLOADER_MAX_QUEUE 100
#define DOWNLOADER_MAX_TITLE 256
#define DOWNLOADER_MAX_ARTIST 128
#define DOWNLOADER_VIDEO_ID_LEN 16

// Search result
typedef struct {
	char video_id[DOWNLOADER_VIDEO_ID_LEN];
	char title[DOWNLOADER_MAX_TITLE];
	char artist[DOWNLOADER_MAX_ARTIST];
	int duration_sec;
} DownloaderResult;

// Queue item status
typedef enum {
	DOWNLOADER_STATUS_PENDING = 0,
	DOWNLOADER_STATUS_DOWNLOADING,
	DOWNLOADER_STATUS_COMPLETE,
	DOWNLOADER_STATUS_FAILED
} DownloaderItemStatus;

// Download queue item
typedef struct {
	char video_id[DOWNLOADER_VIDEO_ID_LEN];
	char title[DOWNLOADER_MAX_TITLE];
	DownloaderItemStatus status;
	int progress_percent; // 0-100 during download
	int speed_bps;		  // Download speed in bytes/sec
	int eta_sec;		  // Estimated time remaining in seconds
} DownloaderQueueItem;

// Module states
typedef enum {
	DOWNLOADER_STATE_IDLE = 0,
	DOWNLOADER_STATE_SEARCHING,
	DOWNLOADER_STATE_DOWNLOADING,
	DOWNLOADER_STATE_UPDATING,
	DOWNLOADER_STATE_ERROR
} DownloaderState;

// Download status info
typedef struct {
	DownloaderState state;
	int current_index;	 // Currently downloading item index
	int total_items;	 // Total items in queue
	int completed_count; // Number completed
	int failed_count;	 // Number failed
	char current_title[DOWNLOADER_MAX_TITLE];
	char error_message[256];
	int speed_bps; // Current download speed
	int eta_sec;   // Current ETA
} DownloaderDownloadStatus;

// Search status info
typedef struct {
	bool searching;			 // True while search is in progress
	bool completed;			 // True when search finished (success or error)
	int result_count;		 // Number of results found (-1 on error)
	char error_message[256]; // Error message if failed
} DownloaderSearchStatus;

// Initialize downloader module
// Returns 0 on success, -1 if yt-dlp not found
int Downloader_init(void);

// Cleanup resources
void Downloader_cleanup(void);

// Check if yt-dlp binary exists
bool Downloader_isAvailable(void);

// Check network connectivity (quick ping test)
// Returns true if network is available, false otherwise
bool Downloader_checkNetwork(void);

// Get yt-dlp version
const char* Downloader_getVersion(void);

// Async search functions
// Start a background search
int Downloader_startSearch(const char* query);

// Get search status (call in main loop to check progress)
const DownloaderSearchStatus* Downloader_getSearchStatus(void);

// Get search results after search completes
// Returns pointer to internal results array, count is set via status->result_count
DownloaderResult* Downloader_getSearchResults(void);

// Cancel ongoing search
void Downloader_cancelSearch(void);

// Queue management
int Downloader_queueAdd(const char* video_id, const char* title);
int Downloader_queueRemove(int index);
int Downloader_queueRemoveById(const char* video_id);
int Downloader_queueClear(void);
int Downloader_queueCount(void);
DownloaderQueueItem* Downloader_queueGet(int* count);

// Check if video is already in queue or downloaded
bool Downloader_isInQueue(const char* video_id);
bool Downloader_isDownloaded(const char* video_id);

// Start downloading queue items (runs in background)
int Downloader_downloadStart(void);

// Stop/cancel current download
void Downloader_downloadStop(void);

// Get download status
const DownloaderDownloadStatus* Downloader_getDownloadStatus(void);

// Check if download thread is running
bool Downloader_isDownloading(void);

// Get current state
DownloaderState Downloader_getState(void);

// Get last error message
const char* Downloader_getError(void);

// Update function (call in main loop)
void Downloader_update(void);

// Save/load queue (persistence)
void Downloader_saveQueue(void);
void Downloader_loadQueue(void);

// Get download directory path
const char* Downloader_getDownloadPath(void);

// Open keyboard for search input
// Returns allocated string that caller must free, or NULL if cancelled
char* Downloader_openKeyboard(const char* prompt);

#endif
