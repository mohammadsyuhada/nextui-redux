#ifndef __RADIO_HLS_H__
#define __RADIO_HLS_H__

#include <stdint.h>
#include <stdbool.h>

#define HLS_MAX_SEGMENTS 64
#define HLS_MAX_URL_LEN 1024
#define HLS_SEGMENT_BUF_SIZE (256 * 1024)
#define HLS_AAC_BUF_SIZE (128 * 1024)

// HLS segment info
typedef struct {
	char url[HLS_MAX_URL_LEN];
	float duration;
	char title[128];
	char artist[128];
} HLSSegment;

// HLS context
typedef struct {
	char base_url[HLS_MAX_URL_LEN];
	HLSSegment segments[HLS_MAX_SEGMENTS];
	int segment_count;
	int current_segment;
	float target_duration;
	int media_sequence;
	int last_played_sequence;
	bool is_live;
	uint32_t last_playlist_fetch;
} HLSContext;

// Check if URL is an HLS stream (.m3u8)
bool radio_hls_is_url(const char* url);

// Parse M3U8 playlist from content
// Returns number of segments found
int radio_hls_parse_playlist(HLSContext* ctx, const char* content, const char* base_url);

// Fetch and parse M3U8 playlist from URL
// Returns number of segments found, or -1 on error
int radio_hls_fetch_playlist(HLSContext* ctx, const char* url);

// URL utilities
void radio_hls_get_base_url(const char* url, char* base, int base_size);
void radio_hls_resolve_url(const char* base, const char* relative, char* result, int result_size);

// Parse ID3 metadata from HLS segment
// Returns bytes to skip (ID3 tag size), or 0 if no ID3 tag
int radio_hls_parse_id3_metadata(const uint8_t* data, int len,
								 char* artist, int artist_size,
								 char* title, int title_size);

// MPEG-TS demuxer
// Extract AAC audio from MPEG-TS data
// Returns number of AAC bytes extracted
int radio_hls_demux_ts(const uint8_t* ts_data, int ts_len,
					   uint8_t* aac_out, int aac_out_size,
					   int* audio_pid, bool* pid_detected);

#endif
