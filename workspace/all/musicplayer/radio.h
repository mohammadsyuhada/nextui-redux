#ifndef __RADIO_H__
#define __RADIO_H__

#include <stdint.h>
#include <stdbool.h>

#define RADIO_MAX_STATIONS 32
#define RADIO_MAX_URL 512
#define RADIO_MAX_NAME 128
#define RADIO_BUFFER_SIZE (64 * 1024) // 64KB buffer

// Radio station
typedef struct {
	char name[RADIO_MAX_NAME];
	char url[RADIO_MAX_URL];
	char genre[64];
	char slogan[128];
} RadioStation;

// Curated country for station browser
typedef struct {
	char name[64];
	char code[8];
} CuratedCountry;

// Curated station entry
typedef struct {
	char name[RADIO_MAX_NAME];
	char url[RADIO_MAX_URL];
	char genre[64];
	char slogan[128];
	char country_code[8];
} CuratedStation;

// Stream metadata (from ICY)
typedef struct {
	char title[256];		// Current song title
	char artist[256];		// Artist (parsed from title if available)
	char station_name[256]; // Station name from ICY
	int bitrate;			// Stream bitrate in kbps
	char content_type[64];	// audio/mpeg, audio/ogg, etc.
} RadioMetadata;

// Radio states
typedef enum {
	RADIO_STATE_STOPPED = 0,
	RADIO_STATE_CONNECTING,
	RADIO_STATE_BUFFERING,
	RADIO_STATE_PLAYING,
	RADIO_STATE_ERROR
} RadioState;

// Initialize radio module
int Radio_init(void);

// Cleanup
void Radio_quit(void);

// Get list of preset stations
int Radio_getStations(RadioStation** stations);

// Add a custom station
int Radio_addStation(const char* name, const char* url, const char* genre, const char* slogan);

// Remove a station by index
void Radio_removeStation(int index);

// Save stations to file
void Radio_saveStations(void);

// Load stations from file
void Radio_loadStations(void);

// Connect and start streaming
int Radio_play(const char* url);

// Stop streaming
void Radio_stop(void);

// Get current state
RadioState Radio_getState(void);

// Get current/last played URL (for resume functionality)
const char* Radio_getCurrentUrl(void);

// Find the index of the currently playing station in the station list (-1 if not found)
int Radio_findCurrentStationIndex(void);

// Get current metadata
const RadioMetadata* Radio_getMetadata(void);

// Get buffer level (0.0 to 1.0)
float Radio_getBufferLevel(void);

// Get error message if state is RADIO_STATE_ERROR
const char* Radio_getError(void);

// Update radio (call in main loop)
void Radio_update(void);

// Get audio samples for playback (called by audio callback)
int Radio_getAudioSamples(int16_t* buffer, int max_samples);

// Check if radio is active
bool Radio_isActive(void);

// Curated stations API
int Radio_getCuratedCountryCount(void);
const CuratedCountry* Radio_getCuratedCountries(void);
int Radio_getCuratedStationCount(const char* country_code);
const CuratedStation* Radio_getCuratedStations(const char* country_code, int* count);
bool Radio_stationExists(const char* url);
bool Radio_removeStationByUrl(const char* url);

// Album art for current radio track (fetched from iTunes)
struct SDL_Surface* Radio_getAlbumArt(void);

// Check if user has custom stations (vs using defaults)
bool Radio_hasUserStations(void);

#endif
