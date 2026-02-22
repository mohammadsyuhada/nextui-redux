#ifndef __YTDLP_UPDATER_H__
#define __YTDLP_UPDATER_H__

#include <stdbool.h>
#include <SDL2/SDL.h>

// Update status info
typedef struct {
	bool update_available;
	char current_version[32];
	char latest_version[32];
	bool updating;
	int progress_percent;
	long download_bytes;	// Bytes downloaded so far
	long download_total;	// Total bytes to download (0 if unknown)
	char status_detail[64]; // Detailed status (e.g., "2.5 MB / 5.0 MB")
	char error_message[256];
} YtdlpUpdateStatus;

// Initialize the updater (read version from shared file, fallback to yt-dlp --version)
void YtdlpUpdater_init(void);

// Cleanup resources
void YtdlpUpdater_cleanup(void);

// Get current yt-dlp version string
const char* YtdlpUpdater_getVersion(void);

// Start update check and download in background thread
int YtdlpUpdater_startUpdate(void);

// Cancel ongoing update
void YtdlpUpdater_cancelUpdate(void);

// Get current update status
const YtdlpUpdateStatus* YtdlpUpdater_getUpdateStatus(void);

// Check if update thread is running
bool YtdlpUpdater_isUpdating(void);

// Render the yt-dlp update screen
void render_ytdlp_updating(SDL_Surface* screen, int show_setting);

#endif
