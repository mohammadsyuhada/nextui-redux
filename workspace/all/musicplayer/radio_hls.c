#define _GNU_SOURCE
#include "radio_hls.h"
#include "radio_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MPEG-TS constants
#define TS_PACKET_SIZE 188
#define TS_SYNC_BYTE 0x47
#define TS_PAT_PID 0x0000

// Check if URL is an HLS stream
bool radio_hls_is_url(const char* url) {
	const char* ext = strrchr(url, '.');
	if (ext && strcasecmp(ext, ".m3u8") == 0)
		return true;
	// Also check for m3u8 in query string
	if (strstr(url, ".m3u8") != NULL)
		return true;
	return false;
}

// Extract base URL from full URL (for resolving relative paths)
void radio_hls_get_base_url(const char* url, char* base, int base_size) {
	strncpy(base, url, base_size - 1);
	base[base_size - 1] = '\0';

	// Find last slash after the host
	char* last_slash = strrchr(base, '/');
	if (last_slash && last_slash > base + 8) { // After "https://"
		*(last_slash + 1) = '\0';
	}
}

// Resolve a potentially relative URL
void radio_hls_resolve_url(const char* base, const char* relative, char* result, int result_size) {
	if (strncmp(relative, "http://", 7) == 0 || strncmp(relative, "https://", 8) == 0) {
		// Absolute URL
		strncpy(result, relative, result_size - 1);
		result[result_size - 1] = '\0';
	} else if (relative[0] == '/') {
		// Root-relative URL - extract host from base
		const char* host_start = strstr(base, "://");
		if (host_start) {
			host_start += 3;
			const char* host_end = strchr(host_start, '/');
			if (host_end) {
				int host_len = host_end - base;
				strncpy(result, base, host_len);
				result[host_len] = '\0';
				strncat(result, relative, result_size - strlen(result) - 1);
			} else {
				snprintf(result, result_size, "%s%s", base, relative);
			}
		}
	} else {
		// Relative URL
		snprintf(result, result_size, "%s%s", base, relative);
	}
}

// Parse M3U8 playlist content
int radio_hls_parse_playlist(HLSContext* ctx, const char* content, const char* base_url) {
	ctx->segment_count = 0;
	ctx->is_live = true; // Assume live until we see ENDLIST
	ctx->target_duration = 10.0f;
	ctx->media_sequence = 0;

	strncpy(ctx->base_url, base_url, HLS_MAX_URL_LEN - 1);

	const char* line = content;
	float segment_duration = 0;
	char segment_title[128] = "";
	char segment_artist[128] = "";
	char variant_url[HLS_MAX_URL_LEN] = "";
	bool is_master_playlist = false;

	while (*line && ctx->segment_count < HLS_MAX_SEGMENTS) {
		// Skip whitespace
		while (*line == ' ' || *line == '\t')
			line++;

		// Find end of line
		const char* eol = line;
		while (*eol && *eol != '\n' && *eol != '\r')
			eol++;
		int line_len = eol - line;

		if (line_len > 0) {
			char line_buf[HLS_MAX_URL_LEN];
			if (line_len >= HLS_MAX_URL_LEN)
				line_len = HLS_MAX_URL_LEN - 1;
			strncpy(line_buf, line, line_len);
			line_buf[line_len] = '\0';

			if (strncmp(line_buf, "#EXTM3U", 7) == 0) {
				// Valid M3U8 header
			} else if (strncmp(line_buf, "#EXT-X-STREAM-INF:", 18) == 0) {
				// Master playlist - we need to fetch the variant
				is_master_playlist = true;
			} else if (strncmp(line_buf, "#EXT-X-TARGETDURATION:", 22) == 0) {
				ctx->target_duration = atof(line_buf + 22);
			} else if (strncmp(line_buf, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0) {
				ctx->media_sequence = atoi(line_buf + 22);
			} else if (strncmp(line_buf, "#EXTINF:", 8) == 0) {
				// Segment duration and optional metadata
				segment_duration = atof(line_buf + 8);
				segment_title[0] = '\0';
				segment_artist[0] = '\0';

				// Parse title="..."
				char* title_start = strstr(line_buf, "title=\"");
				if (title_start) {
					title_start += 7;
					char* title_end = strchr(title_start, '"');
					if (title_end) {
						int len = title_end - title_start;
						if (len > 127)
							len = 127;
						strncpy(segment_title, title_start, len);
						segment_title[len] = '\0';
					}
				}

				// Parse artist="..."
				char* artist_start = strstr(line_buf, "artist=\"");
				if (artist_start) {
					artist_start += 8;
					char* artist_end = strchr(artist_start, '"');
					if (artist_end) {
						int len = artist_end - artist_start;
						if (len > 127)
							len = 127;
						strncpy(segment_artist, artist_start, len);
						segment_artist[len] = '\0';
					}
				}
			} else if (strncmp(line_buf, "#EXT-X-ENDLIST", 14) == 0) {
				ctx->is_live = false;
			} else if (line_buf[0] != '#' && line_buf[0] != '\0') {
				// This is a URL
				if (is_master_playlist && variant_url[0] == '\0') {
					// Save first variant URL
					radio_hls_resolve_url(ctx->base_url, line_buf, variant_url, HLS_MAX_URL_LEN);
				} else if (!is_master_playlist) {
					// Media segment
					radio_hls_resolve_url(ctx->base_url, line_buf,
										  ctx->segments[ctx->segment_count].url, HLS_MAX_URL_LEN);
					ctx->segments[ctx->segment_count].duration = segment_duration;
					strncpy(ctx->segments[ctx->segment_count].title, segment_title, 127);
					strncpy(ctx->segments[ctx->segment_count].artist, segment_artist, 127);
					ctx->segment_count++;
					segment_duration = 0;
					segment_title[0] = '\0';
					segment_artist[0] = '\0';
				}
			}
		}

		// Move to next line
		line = eol;
		while (*line == '\n' || *line == '\r')
			line++;
	}

	// If master playlist, fetch the variant playlist
	if (is_master_playlist && variant_url[0]) {
		uint8_t* playlist_buf = malloc(64 * 1024);
		if (playlist_buf) {
			int len = radio_net_fetch(variant_url, playlist_buf, 64 * 1024, NULL, 0);
			if (len > 0) {
				playlist_buf[len] = '\0';
				// Update base URL for variant
				radio_hls_get_base_url(variant_url, ctx->base_url, HLS_MAX_URL_LEN);
				radio_hls_parse_playlist(ctx, (char*)playlist_buf, ctx->base_url);
			}
			free(playlist_buf);
		}
	}

	return ctx->segment_count;
}

// Fetch and parse M3U8 playlist from URL
int radio_hls_fetch_playlist(HLSContext* ctx, const char* url) {
	uint8_t* playlist_buf = malloc(64 * 1024);
	if (!playlist_buf) {
		return -1;
	}

	int len = radio_net_fetch(url, playlist_buf, 64 * 1024, NULL, 0);
	if (len <= 0) {
		free(playlist_buf);
		return -1;
	}

	playlist_buf[len] = '\0';

	char base_url[HLS_MAX_URL_LEN];
	radio_hls_get_base_url(url, base_url, HLS_MAX_URL_LEN);

	int seg_count = radio_hls_parse_playlist(ctx, (char*)playlist_buf, base_url);
	free(playlist_buf);

	return seg_count;
}

// Parse ID3 tags from HLS segment
int radio_hls_parse_id3_metadata(const uint8_t* data, int len,
								 char* artist, int artist_size,
								 char* title, int title_size) {
	// ID3 tag header: "ID3" + version (2 bytes) + flags (1 byte) + size (4 bytes syncsafe)
	if (len < 10)
		return 0;
	if (data[0] != 'I' || data[1] != 'D' || data[2] != '3')
		return 0;

	uint8_t version_major = data[3];

	// Parse syncsafe size (4 bytes, 7 bits each)
	uint32_t tag_size = ((data[6] & 0x7F) << 21) |
						((data[7] & 0x7F) << 14) |
						((data[8] & 0x7F) << 7) |
						(data[9] & 0x7F);

	// Check for integer overflow (tag_size + 10 could wrap)
	if (tag_size > (uint32_t)(len - 10))
		return 0; // Tag size exceeds available data

	int total_size = 10 + (int)tag_size;
	if (total_size > len || total_size < 10)
		return 0; // Incomplete tag or overflow

	// Initialize output
	if (artist && artist_size > 0)
		artist[0] = '\0';
	if (title && title_size > 0)
		title[0] = '\0';

	// Parse ID3 frames
	int pos = 10;
	while (pos + 10 < total_size) {
		// Frame header: ID (4 bytes) + size (4 bytes) + flags (2 bytes)
		char frame_id[5] = {data[pos], data[pos + 1], data[pos + 2], data[pos + 3], 0};

		// Frame size: syncsafe in ID3v2.4, regular in ID3v2.3
		uint32_t frame_size;
		if (version_major >= 4) {
			frame_size = ((data[pos + 4] & 0x7F) << 21) | ((data[pos + 5] & 0x7F) << 14) |
						 ((data[pos + 6] & 0x7F) << 7) | (data[pos + 7] & 0x7F);
		} else {
			frame_size = (data[pos + 4] << 24) | (data[pos + 5] << 16) |
						 (data[pos + 6] << 8) | data[pos + 7];
		}

		if (frame_size == 0 || pos + 10 + frame_size > (uint32_t)total_size)
			break;

		const uint8_t* frame_data = &data[pos + 10];

		// TIT2 = Title
		if (strcmp(frame_id, "TIT2") == 0 && frame_size > 1 && title && title_size > 0) {
			int encoding = frame_data[0];
			const char* text = (const char*)&frame_data[1];
			int text_len = frame_size - 1;
			if (text_len > title_size - 1)
				text_len = title_size - 1;

			if (encoding == 0 || encoding == 3) { // ISO-8859-1 or UTF-8
				memcpy(title, text, text_len);
				title[text_len] = '\0';
			}
		}
		// TPE1 = Artist
		else if (strcmp(frame_id, "TPE1") == 0 && frame_size > 1 && artist && artist_size > 0) {
			int encoding = frame_data[0];
			const char* text = (const char*)&frame_data[1];
			int text_len = frame_size - 1;
			if (text_len > artist_size - 1)
				text_len = artist_size - 1;

			if (encoding == 0 || encoding == 3) {
				memcpy(artist, text, text_len);
				artist[text_len] = '\0';
			}
		}
		// TXXX = User-defined text (may contain StreamTitle)
		else if (strcmp(frame_id, "TXXX") == 0 && frame_size > 1 && title && title_size > 0) {
			int encoding = frame_data[0];
			if (encoding == 0 || encoding == 3) {
				const char* desc = (const char*)&frame_data[1];
				if (strstr(desc, "StreamTitle") || strstr(desc, "TITLE")) {
					const char* value = desc;
					while (*value && (value - (const char*)frame_data) < (int)frame_size)
						value++;
					value++;

					if ((value - (const char*)frame_data) < (int)frame_size) {
						int val_len = frame_size - (value - (const char*)frame_data);
						if (val_len > title_size - 1)
							val_len = title_size - 1;

						memcpy(title, value, val_len);
						title[val_len] = '\0';

						// Try to parse "Artist - Title" format
						char* separator = strstr(title, " - ");
						if (separator && artist && artist_size > 0) {
							*separator = '\0';
							strncpy(artist, title, artist_size - 1);
							artist[artist_size - 1] = '\0';
							memmove(title, separator + 3, strlen(separator + 3) + 1);
						}
					}
				}
			}
		}
		// PRIV = Private frame (some streams put metadata here)
		else if (strcmp(frame_id, "PRIV") == 0 && frame_size > 0 && title && title_size > 0) {
			char priv_data[512];
			int copy_len = frame_size < sizeof(priv_data) - 1 ? frame_size : sizeof(priv_data) - 1;
			memcpy(priv_data, frame_data, copy_len);
			priv_data[copy_len] = '\0';

			char* title_start = strstr(priv_data, "StreamTitle='");
			if (title_start) {
				title_start += 13;
				char* title_end = strchr(title_start, '\'');
				if (title_end) {
					*title_end = '\0';
					strncpy(title, title_start, title_size - 1);
					title[title_size - 1] = '\0';

					char* separator = strstr(title, " - ");
					if (separator && artist && artist_size > 0) {
						*separator = '\0';
						strncpy(artist, title, artist_size - 1);
						artist[artist_size - 1] = '\0';
						memmove(title, separator + 3, strlen(separator + 3) + 1);
					}
				}
			}
		}

		pos += 10 + frame_size;
	}

	return total_size;
}

// Extract AAC audio from MPEG-TS data
int radio_hls_demux_ts(const uint8_t* ts_data, int ts_len,
					   uint8_t* aac_out, int aac_out_size,
					   int* audio_pid, bool* pid_detected) {
	int aac_pos = 0;
	int pmt_pid = -1;
	int local_audio_pid = -1;

	// Use cached audio PID if available
	if (pid_detected && *pid_detected && audio_pid) {
		local_audio_pid = *audio_pid;
	}

	int pos = 0;
	while (pos + TS_PACKET_SIZE <= ts_len && aac_pos < aac_out_size - 1024) {
		// Find sync byte
		while (pos < ts_len && ts_data[pos] != TS_SYNC_BYTE) {
			pos++;
		}

		if (pos + TS_PACKET_SIZE > ts_len)
			break;

		const uint8_t* pkt = &ts_data[pos];

		// Parse TS header
		int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
		int payload_start = (pkt[1] & 0x40) != 0;
		int adaptation_field = (pkt[3] >> 4) & 0x03;

		int header_len = 4;
		if (adaptation_field == 2 || adaptation_field == 3) {
			// Bounds check for adaptation field length byte
			if (4 >= TS_PACKET_SIZE)
				break;
			int adapt_len = pkt[4];
			// Validate adaptation field length doesn't exceed packet
			if (adapt_len > TS_PACKET_SIZE - 5)
				break;
			header_len += 1 + adapt_len; // Skip adaptation field
		}

		if (adaptation_field == 1 || adaptation_field == 3) {
			// Has payload - validate header_len first
			if (header_len >= TS_PACKET_SIZE) {
				pos += TS_PACKET_SIZE;
				continue; // Invalid packet, skip
			}
			const uint8_t* payload = pkt + header_len;
			int payload_len = TS_PACKET_SIZE - header_len;

			if (pid == TS_PAT_PID && payload_start && !(pid_detected && *pid_detected)) {
				// Parse PAT to find PMT PID
				int section_start = payload[0] + 1;
				if (section_start + 8 < payload_len) {
					const uint8_t* pat = payload + section_start;
					if (pat[0] == 0x00) { // table_id for PAT
						int section_len = ((pat[1] & 0x0F) << 8) | pat[2];
						if (section_len >= 9) {
							pmt_pid = ((pat[10] & 0x1F) << 8) | pat[11];
						}
					}
				}
			} else if (pmt_pid > 0 && pid == pmt_pid && payload_start && !(pid_detected && *pid_detected)) {
				// Parse PMT to find audio stream PID
				int section_start = payload[0] + 1;
				if (section_start + 12 < payload_len) {
					const uint8_t* pmt = payload + section_start;
					if (pmt[0] == 0x02) { // table_id for PMT
						int section_len = ((pmt[1] & 0x0F) << 8) | pmt[2];
						int prog_info_len = ((pmt[10] & 0x0F) << 8) | pmt[11];

						int es_pos = 12 + prog_info_len;
						while (es_pos + 5 <= section_len + 3 - 4) {
							int stream_type = pmt[es_pos];
							int es_pid = ((pmt[es_pos + 1] & 0x1F) << 8) | pmt[es_pos + 2];
							int es_info_len = ((pmt[es_pos + 3] & 0x0F) << 8) | pmt[es_pos + 4];

							// AAC stream types: 0x0F (ADTS), 0x11 (LATM)
							if (stream_type == 0x0F || stream_type == 0x11) {
								local_audio_pid = es_pid;
								if (audio_pid)
									*audio_pid = local_audio_pid;
								if (pid_detected)
									*pid_detected = true;
								break;
							}
							// Also check for MP3 (0x03, 0x04)
							if (stream_type == 0x03 || stream_type == 0x04) {
								local_audio_pid = es_pid;
								if (audio_pid)
									*audio_pid = local_audio_pid;
								if (pid_detected)
									*pid_detected = true;
								break;
							}

							es_pos += 5 + es_info_len;
						}
					}
				}
			} else if (local_audio_pid > 0 && pid == local_audio_pid) {
				// Extract audio data from PES packet
				const uint8_t* pes = payload;
				int pes_len = payload_len;

				if (payload_start) {
					// Check PES start code
					if (pes[0] == 0x00 && pes[1] == 0x00 && pes[2] == 0x01) {
						// Parse PES header
						int pes_header_len = 9 + pes[8];
						if (pes_header_len < pes_len) {
							int audio_len = pes_len - pes_header_len;
							if (aac_pos + audio_len < aac_out_size) {
								memcpy(aac_out + aac_pos, pes + pes_header_len, audio_len);
								aac_pos += audio_len;
							}
						}
					}
				} else {
					// Continuation of PES packet - raw audio data
					if (aac_pos + pes_len < aac_out_size) {
						memcpy(aac_out + aac_pos, pes, pes_len);
						aac_pos += pes_len;
					}
				}
			}
		}

		pos += TS_PACKET_SIZE;
	}

	return aac_pos;
}
