#define _GNU_SOURCE // For strcasestr
#include "radio.h"
#include "radio_net.h"
#include "album_art.h"
#include "radio_hls.h"
#include "radio_curated.h"
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include "defines.h"
#include "api.h"

// SDL for rendering
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

// mbedTLS for HTTPS support
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

// MP3 streaming decoder (implementation is in player.c)
#include "audio/dr_mp3.h"

// AAC decoder (FDK-AAC)
#include <src/aacdecoder_lib.h>

// Audio format types for radio streams
typedef enum {
	RADIO_FORMAT_UNKNOWN = 0,
	RADIO_FORMAT_MP3,
	RADIO_FORMAT_AAC
} RadioAudioFormat;

// Stream types
typedef enum {
	STREAM_TYPE_DIRECT = 0, // Direct MP3/AAC stream (Shoutcast/Icecast)
	STREAM_TYPE_HLS			// HLS (m3u8 playlist)
} StreamType;

// HLS types are now defined in radio_hls.h

#define SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2
#define RADIO_STATIONS_FILE SHARED_USERDATA_PATH "/music-player/radio/stations.txt"

// Ring buffer for decoded audio
#define AUDIO_RING_SIZE (SAMPLE_RATE * 2 * 10) // 10 seconds of stereo audio

// Default radio stations
static RadioStation default_stations[] = {
	{"Hitz FM", "https://n10.rcs.revma.com/488kt4sbv4uvv/10_xn1quxmoht3902/playlist.m3u8", "Pop", "More the Hitz, One the Time"},
};

// Curated stations are now in radio_curated.c module

// Radio context
typedef struct {
	// State - volatile for cross-thread access
	volatile RadioState state;
	char error_msg[256];

	// Connection
	int socket_fd;
	char current_url[RADIO_MAX_URL];
	char redirect_url[RADIO_MAX_URL]; // For handling HTTP redirects

	// SSL/TLS support
	bool use_ssl;
	mbedtls_net_context ssl_net;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config ssl_conf;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
	bool ssl_initialized;

	// ICY metadata
	int icy_metaint;	  // Bytes between metadata
	int bytes_until_meta; // Countdown to next metadata
	RadioMetadata metadata;

	// Stream buffer (raw network data)
	uint8_t* stream_buffer;
	int stream_buffer_size;
	int stream_buffer_pos;

	// Audio ring buffer (decoded PCM)
	int16_t* audio_ring;
	int audio_ring_write;
	int audio_ring_read;
	int audio_ring_count;
	pthread_mutex_t audio_mutex;

	// Audio format detection
	RadioAudioFormat audio_format;

	// MP3 decoder (low-level for streaming)
	drmp3dec mp3_decoder;
	bool mp3_initialized;
	int mp3_sample_rate;
	int mp3_channels;

	// AAC decoder (FDK-AAC)
	HANDLE_AACDECODER aac_decoder;
	bool aac_initialized;
	uint8_t aac_inbuf[768 * 2 * 2]; // Buffer for ADTS stream data (matches old AAC_MAINBUF_SIZE * 2)
	int aac_inbuf_size;
	int aac_sample_rate;
	int aac_channels;

	// HLS support
	StreamType stream_type;
	HLSContext hls;
	uint8_t* hls_segment_buffer; // Buffer for downloading segments
	int hls_segment_buffer_size;
	int hls_segment_buffer_pos;

	// Pre-allocated HLS buffers (to reduce memory fragmentation)
	uint8_t* hls_segment_buf;		  // Segment download buffer
	uint8_t* hls_aac_buf;			  // AAC decode buffer
	uint8_t* hls_prefetch_buf;		  // Prefetch buffer for next segment
	int hls_prefetch_len;			  // Length of prefetched data
	int hls_prefetch_segment;		  // Which segment index is prefetched (-1 if none)
	volatile bool hls_prefetch_ready; // Is prefetch data ready to use?
	pthread_mutex_t hls_mutex;		  // Mutex for HLS prefetch and segments access

	// TS demuxer state
	int ts_aac_pid; // PID of AAC audio stream
	bool ts_pid_detected;

	// Threading
	pthread_t stream_thread;
	bool thread_running;
	bool should_stop;

	// Stations
	RadioStation stations[RADIO_MAX_STATIONS];
	int station_count;

	// Album art is now managed by album_art module

	// Deferred audio configuration (to avoid blocking stream thread)
	bool pending_sample_rate_change;
	int pending_sample_rate;
	bool pending_audio_resume;

	// Track if user has custom stations loaded
	bool has_user_stations;
} RadioContext;

static RadioContext radio = {0};

// Use radio_net_parse_url for URL parsing

// Initialize SSL/TLS
static int ssl_init(const char* host) {
	int ret;
	const char* pers = "radio_client";

	mbedtls_net_init(&radio.ssl_net);
	mbedtls_ssl_init(&radio.ssl);
	mbedtls_ssl_config_init(&radio.ssl_conf);
	mbedtls_entropy_init(&radio.entropy);
	mbedtls_ctr_drbg_init(&radio.ctr_drbg);

	// Seed random number generator
	ret = mbedtls_ctr_drbg_seed(&radio.ctr_drbg, mbedtls_entropy_func,
								&radio.entropy, (const unsigned char*)pers, strlen(pers));
	if (ret != 0) {
		LOG_error("mbedtls_ctr_drbg_seed failed: %d\n", ret);
		goto ssl_init_error;
	}

	// Set up SSL config
	ret = mbedtls_ssl_config_defaults(&radio.ssl_conf,
									  MBEDTLS_SSL_IS_CLIENT,
									  MBEDTLS_SSL_TRANSPORT_STREAM,
									  MBEDTLS_SSL_PRESET_DEFAULT);
	if (ret != 0) {
		LOG_error("mbedtls_ssl_config_defaults failed: %d\n", ret);
		goto ssl_init_error;
	}

	// Skip certificate verification (radio streams use various CAs)
	mbedtls_ssl_conf_authmode(&radio.ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
	mbedtls_ssl_conf_rng(&radio.ssl_conf, mbedtls_ctr_drbg_random, &radio.ctr_drbg);

	// Set up SSL context
	ret = mbedtls_ssl_setup(&radio.ssl, &radio.ssl_conf);
	if (ret != 0) {
		LOG_error("mbedtls_ssl_setup failed: %d\n", ret);
		goto ssl_init_error;
	}

	// Set hostname for SNI
	ret = mbedtls_ssl_set_hostname(&radio.ssl, host);
	if (ret != 0) {
		LOG_error("mbedtls_ssl_set_hostname failed: %d\n", ret);
		goto ssl_init_error;
	}

	radio.ssl_initialized = true;
	return 0;

ssl_init_error:
	// Clean up all initialized contexts on error
	mbedtls_net_free(&radio.ssl_net);
	mbedtls_ssl_free(&radio.ssl);
	mbedtls_ssl_config_free(&radio.ssl_conf);
	mbedtls_ctr_drbg_free(&radio.ctr_drbg);
	mbedtls_entropy_free(&radio.entropy);
	return -1;
}

// Cleanup SSL
static void ssl_cleanup(void) {
	if (radio.ssl_initialized) {
		mbedtls_ssl_close_notify(&radio.ssl);
		mbedtls_net_free(&radio.ssl_net);
		mbedtls_ssl_free(&radio.ssl);
		mbedtls_ssl_config_free(&radio.ssl_conf);
		mbedtls_ctr_drbg_free(&radio.ctr_drbg);
		mbedtls_entropy_free(&radio.entropy);
		radio.ssl_initialized = false;
	}
}

// Send wrapper (works with both HTTP and HTTPS)
static int radio_send(const void* buf, size_t len) {
	if (radio.use_ssl) {
		return mbedtls_ssl_write(&radio.ssl, buf, len);
	} else {
		return send(radio.socket_fd, buf, len, 0);
	}
}

// Receive wrapper (works with both HTTP and HTTPS)
static int radio_recv(void* buf, size_t len) {
	if (radio.use_ssl) {
		return mbedtls_ssl_read(&radio.ssl, buf, len);
	} else {
		return recv(radio.socket_fd, buf, len, 0);
	}
}

// Connect to stream server (supports HTTP and HTTPS)
static int connect_stream(const char* url) {
	char host[256], path[512];
	int port;
	bool is_https;
	int ret;

	if (radio_net_parse_url(url, host, 256, &port, path, 512, &is_https) != 0) {
		snprintf(radio.error_msg, sizeof(radio.error_msg), "Invalid URL");
		return -1;
	}

	radio.use_ssl = is_https;

	if (is_https) {
		// HTTPS connection using mbedTLS
		char port_str[16];
		snprintf(port_str, sizeof(port_str), "%d", port);

		// Initialize SSL
		if (ssl_init(host) != 0) {
			snprintf(radio.error_msg, sizeof(radio.error_msg), "SSL init failed");
			return -1;
		}

		// Connect using mbedTLS
		ret = mbedtls_net_connect(&radio.ssl_net, host, port_str, MBEDTLS_NET_PROTO_TCP);
		if (ret != 0) {
			LOG_error("mbedtls_net_connect failed: %d\n", ret);
			ssl_cleanup();
			snprintf(radio.error_msg, sizeof(radio.error_msg), "Connection failed");
			return -1;
		}

		// Set socket for SSL
		mbedtls_ssl_set_bio(&radio.ssl, &radio.ssl_net,
							mbedtls_net_send, mbedtls_net_recv, NULL);

		// SSL handshake with timeout protection
		int handshake_retries = 0;
		const int max_handshake_retries = 100; // 10 seconds with 100ms sleep
		while ((ret = mbedtls_ssl_handshake(&radio.ssl)) != 0) {
			// TLS 1.3: session ticket received means handshake is complete
			if (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
				break;
			}
			if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
				LOG_error("mbedtls_ssl_handshake failed: -0x%04X\n", -ret);
				ssl_cleanup();
				snprintf(radio.error_msg, sizeof(radio.error_msg), "SSL handshake failed");
				return -1;
			}
			if (++handshake_retries > max_handshake_retries) {
				LOG_error("SSL handshake timeout after %d retries\n", handshake_retries);
				ssl_cleanup();
				snprintf(radio.error_msg, sizeof(radio.error_msg), "SSL handshake timeout");
				return -1;
			}
			usleep(100000); // 100ms sleep between retries
		}


		// Store socket fd for select() compatibility
		radio.socket_fd = radio.ssl_net.fd;
	} else {
		// Plain HTTP connection - use getaddrinfo (thread-safe)
		struct addrinfo hints, *result = NULL;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;

		char port_str[16];
		snprintf(port_str, sizeof(port_str), "%d", port);

		int gai_ret = getaddrinfo(host, port_str, &hints, &result);
		if (gai_ret != 0 || !result) {
			if (result)
				freeaddrinfo(result);
			snprintf(radio.error_msg, sizeof(radio.error_msg), "DNS lookup failed");
			return -1;
		}

		radio.socket_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
		if (radio.socket_fd < 0) {
			freeaddrinfo(result);
			snprintf(radio.error_msg, sizeof(radio.error_msg), "Socket creation failed");
			return -1;
		}

		struct timeval tv;
		tv.tv_sec = 10;
		tv.tv_usec = 0;
		setsockopt(radio.socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(radio.socket_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		if (connect(radio.socket_fd, result->ai_addr, result->ai_addrlen) < 0) {
			close(radio.socket_fd);
			radio.socket_fd = -1;
			freeaddrinfo(result);
			snprintf(radio.error_msg, sizeof(radio.error_msg), "Connection failed");
			return -1;
		}
		freeaddrinfo(result);
	}

	// Send HTTP request with ICY headers
	char request[1024];
	snprintf(request, sizeof(request),
			 "GET %s HTTP/1.0\r\n"
			 "Host: %s\r\n"
			 "User-Agent: MusicPlayer/1.0\r\n"
			 "Accept: */*\r\n"
			 "Icy-MetaData: 1\r\n"
			 "Connection: close\r\n"
			 "\r\n",
			 path, host);

	if (radio_send(request, strlen(request)) < 0) {
		if (radio.use_ssl) {
			ssl_cleanup();
		} else {
			close(radio.socket_fd);
		}
		radio.socket_fd = -1;
		snprintf(radio.error_msg, sizeof(radio.error_msg), "Send failed");
		return -1;
	}

	return 0;
}

// Parse HTTP response headers
// Returns: 0 = success, 1 = redirect (check redirect_url), -1 = error
static int parse_headers(void) {
	char header_buf[4096];
	int header_pos = 0;
	char c;

	radio.redirect_url[0] = '\0';

	// Read headers until \r\n\r\n
	while (header_pos < sizeof(header_buf) - 1) {
		if (radio_recv(&c, 1) != 1) {
			snprintf(radio.error_msg, sizeof(radio.error_msg), "Header read failed");
			return -1;
		}
		header_buf[header_pos++] = c;

		// Check for end of headers
		if (header_pos >= 4 &&
			header_buf[header_pos - 4] == '\r' && header_buf[header_pos - 3] == '\n' &&
			header_buf[header_pos - 2] == '\r' && header_buf[header_pos - 1] == '\n') {
			break;
		}
	}
	header_buf[header_pos] = '\0';


	// Check for HTTP or ICY response
	if (strncmp(header_buf, "HTTP/1.", 7) != 0 && strncmp(header_buf, "ICY", 3) != 0) {
		snprintf(radio.error_msg, sizeof(radio.error_msg), "Invalid response");
		return -1;
	}

	// Check HTTP status code for redirects
	int http_status = 0;
	if (strncmp(header_buf, "HTTP/1.", 7) == 0) {
		// Parse status code (e.g., "HTTP/1.1 302 Found")
		char* status_start = header_buf + 9; // Skip "HTTP/1.X "
		http_status = atoi(status_start);

		// Check for redirect (301, 302, 303, 307, 308)
		if (http_status >= 300 && http_status < 400) {
			// Find Location header
			char* loc = strcasestr(header_buf, "\nLocation:");
			if (!loc)
				loc = strcasestr(header_buf, "\rlocation:");
			if (loc) {
				loc += 10; // Skip "Location:"
				while (*loc == ' ' || *loc == '\t')
					loc++; // Skip whitespace

				// Copy until end of line
				char* end = loc;
				while (*end && *end != '\r' && *end != '\n')
					end++;

				int len = end - loc;
				if (len > 0 && len < RADIO_MAX_URL) {
					strncpy(radio.redirect_url, loc, len);
					radio.redirect_url[len] = '\0';
					return 1; // Indicate redirect
				}
			}
			snprintf(radio.error_msg, sizeof(radio.error_msg), "Redirect without Location");
			return -1;
		}

		// Check for error status
		if (http_status >= 400) {
			snprintf(radio.error_msg, sizeof(radio.error_msg), "HTTP error %d", http_status);
			return -1;
		}
	}

	// Parse ICY headers
	radio.icy_metaint = 0;
	radio.metadata.bitrate = 0;
	radio.metadata.station_name[0] = '\0';
	radio.metadata.content_type[0] = '\0';

	char* line = strtok(header_buf, "\r\n");
	while (line) {
		if (strncasecmp(line, "icy-metaint:", 12) == 0) {
			radio.icy_metaint = atoi(line + 12);
		} else if (strncasecmp(line, "icy-br:", 7) == 0) {
			radio.metadata.bitrate = atoi(line + 7);
		} else if (strncasecmp(line, "icy-name:", 9) == 0) {
			strncpy(radio.metadata.station_name, line + 9, sizeof(radio.metadata.station_name) - 1);
			// Trim leading space
			char* name = radio.metadata.station_name;
			while (*name == ' ')
				name++;
			memmove(radio.metadata.station_name, name, strlen(name) + 1);
		} else if (strncasecmp(line, "content-type:", 13) == 0) {
			strncpy(radio.metadata.content_type, line + 13, sizeof(radio.metadata.content_type) - 1);
		}
		line = strtok(NULL, "\r\n");
	}

	radio.bytes_until_meta = radio.icy_metaint;

	// Detect audio format from content type
	radio.audio_format = RADIO_FORMAT_MP3; // Default to MP3
	const char* ct = radio.metadata.content_type;
	if (ct[0]) {
		// Skip leading whitespace
		while (*ct == ' ')
			ct++;

		if (strcasestr(ct, "aac") != NULL ||
			strcasestr(ct, "mp4") != NULL ||
			strcasestr(ct, "m4a") != NULL) {
			radio.audio_format = RADIO_FORMAT_AAC;
		} else if (strcasestr(ct, "mpeg") != NULL ||
				   strcasestr(ct, "mp3") != NULL) {
			radio.audio_format = RADIO_FORMAT_MP3;
		}
	}

	return 0;
}

// Parse ICY metadata block
static void parse_icy_metadata(const uint8_t* data, int len) {
	// Format: StreamTitle='Artist - Title';StreamUrl='...';
	char meta[4096];
	if (len >= sizeof(meta))
		len = sizeof(meta) - 1;
	memcpy(meta, data, len);
	meta[len] = '\0';

	// Save old values to detect changes
	char old_artist[256], old_title[256];
	strncpy(old_artist, radio.metadata.artist, sizeof(old_artist) - 1);
	strncpy(old_title, radio.metadata.title, sizeof(old_title) - 1);

	// Find StreamTitle
	char* title_start = strstr(meta, "StreamTitle='");
	if (title_start) {
		title_start += 13;
		char* title_end = strchr(title_start, '\'');
		if (title_end) {
			*title_end = '\0';
			strncpy(radio.metadata.title, title_start, sizeof(radio.metadata.title) - 1);

			// Try to parse "Artist - Title" format
			char* separator = strstr(radio.metadata.title, " - ");
			if (separator) {
				*separator = '\0';
				strncpy(radio.metadata.artist, radio.metadata.title, sizeof(radio.metadata.artist) - 1);
				memmove(radio.metadata.title, separator + 3, strlen(separator + 3) + 1);
			} else {
				radio.metadata.artist[0] = '\0';
			}

			// Fetch album art if metadata changed
			if (strcmp(old_artist, radio.metadata.artist) != 0 ||
				strcmp(old_title, radio.metadata.title) != 0) {
				album_art_fetch(radio.metadata.artist, radio.metadata.title);
			}
		}
	}
}

// ============== HLS SUPPORT ==============
// HLS functions are now in radio_hls.c module
// Use radio_hls_is_url(), radio_hls_get_base_url(), radio_hls_resolve_url()
// Use radio_hls_parse_playlist(), radio_hls_demux_ts(), radio_hls_parse_id3_metadata()

// TS sync byte for container detection
#define TS_SYNC_BYTE 0x47


// Prefetch thread state
static pthread_t hls_prefetch_thread;
static volatile bool hls_prefetch_thread_active = false;

// HLS segment prefetch worker thread
static void* hls_prefetch_thread_func(void* arg) {
	int seg_idx = (int)(intptr_t)arg;

	// Validate segment index under mutex
	pthread_mutex_lock(&radio.hls_mutex);
	if (seg_idx < 0 || seg_idx >= radio.hls.segment_count || radio.should_stop) {
		pthread_mutex_unlock(&radio.hls_mutex);
		return NULL;
	}

	// Copy URL to local buffer to avoid use-after-free
	char local_url[HLS_MAX_URL_LEN];
	strncpy(local_url, radio.hls.segments[seg_idx].url, HLS_MAX_URL_LEN - 1);
	local_url[HLS_MAX_URL_LEN - 1] = '\0';
	pthread_mutex_unlock(&radio.hls_mutex);

	if (local_url[0] == '\0') {
		return NULL;
	}

	// Fetch segment into prefetch buffer (outside mutex - network I/O)
	int len = radio_net_fetch(local_url, radio.hls_prefetch_buf,
							  HLS_SEGMENT_BUF_SIZE, NULL, 0);

	// Only mark as ready if fetch succeeded and we're not stopping
	pthread_mutex_lock(&radio.hls_mutex);
	if (len > 0 && !radio.should_stop) {
		radio.hls_prefetch_len = len;
		radio.hls_prefetch_segment = seg_idx;
		radio.hls_prefetch_ready = true;
	}
	pthread_mutex_unlock(&radio.hls_mutex);

	return NULL;
}

// Start prefetching a segment in the background
static void start_segment_prefetch(int segment_idx) {
	// Don't prefetch if stopping or buffer not allocated
	if (radio.should_stop || !radio.hls_prefetch_buf) {
		return;
	}

	// Wait for previous prefetch thread to complete
	if (hls_prefetch_thread_active) {
		pthread_join(hls_prefetch_thread, NULL);
		hls_prefetch_thread_active = false;
	}

	// Validate segment index
	if (segment_idx < 0 || segment_idx >= radio.hls.segment_count) {
		return;
	}

	// Start prefetch thread
	if (pthread_create(&hls_prefetch_thread, NULL, hls_prefetch_thread_func,
					   (void*)(intptr_t)segment_idx) == 0) {
		hls_prefetch_thread_active = true;
	}
}

// HLS streaming thread
static void* hls_stream_thread_func(void* arg) {
	(void)arg;

	// Use pre-allocated buffers from RadioContext to reduce memory fragmentation
	uint8_t* segment_buf = radio.hls_segment_buf;
	uint8_t* aac_buf = radio.hls_aac_buf;

	if (!segment_buf || !aac_buf) {
		radio.state = RADIO_STATE_ERROR;
		snprintf(radio.error_msg, sizeof(radio.error_msg), "HLS buffers not allocated");
		return NULL;
	}

	// Initialize FDK-AAC decoder (TT_MP4_ADTS for ADTS-framed AAC in HLS segments)
	radio.aac_decoder = aacDecoder_Open(TT_MP4_ADTS, 1);
	if (!radio.aac_decoder) {
		radio.state = RADIO_STATE_ERROR;
		snprintf(radio.error_msg, sizeof(radio.error_msg), "AAC decoder init failed");
		return NULL;
	}
	radio.aac_initialized = true;
	radio.aac_inbuf_size = 0;
	radio.aac_sample_rate = 0; // Will be set on first frame

	radio.state = RADIO_STATE_BUFFERING;

	int loop_iteration = 0;
	while (!radio.should_stop) {
		loop_iteration++;

		// Check if we need to refresh the playlist (for live streams)
		if (radio.hls.is_live && radio.hls.current_segment >= radio.hls.segment_count) {
			// Fetch updated playlist
			uint8_t* playlist_buf = malloc(64 * 1024);
			if (playlist_buf) {
				int len = radio_net_fetch(radio.current_url, playlist_buf, 64 * 1024, NULL, 0);
				if (len > 0) {
					playlist_buf[len] = '\0';
					char base_url[HLS_MAX_URL_LEN];
					radio_hls_get_base_url(radio.current_url, base_url, HLS_MAX_URL_LEN);

					radio_hls_parse_playlist(&radio.hls, (char*)playlist_buf, base_url);

					// Skip segments we've already played based on last_played_sequence
					// media_sequence is the sequence number of the first segment in the new playlist
					// We want to start at last_played_sequence + 1
					if (radio.hls.last_played_sequence >= 0) {
						int next_seq = radio.hls.last_played_sequence + 1;
						int start_idx = next_seq - radio.hls.media_sequence;
						if (start_idx < 0)
							start_idx = 0;
						if (start_idx > radio.hls.segment_count)
							start_idx = radio.hls.segment_count;
						radio.hls.current_segment = start_idx;
					} else {
						radio.hls.current_segment = 0;
					}
				}
				free(playlist_buf);
			}

			// If still no segments, wait a bit
			if (radio.hls.current_segment >= radio.hls.segment_count) {
				usleep(100000); // 100ms
				continue;
			}
		}

		// Get next segment
		if (radio.hls.current_segment >= radio.hls.segment_count) {
			if (!radio.hls.is_live) {
				// End of stream
				break;
			}
			usleep(100000);
			continue;
		}

		// Wait if buffer is nearly full to prevent overflow
		// Use high threshold (90%) and short wait to minimize network fetch delays
		while (radio.audio_ring_count > AUDIO_RING_SIZE * 9 / 10 && !radio.should_stop) {
			usleep(50000); // 50ms - short wait, check frequently
		}
		if (radio.should_stop)
			break;

		// Validate segment index
		if (radio.hls.current_segment < 0 || radio.hls.current_segment >= HLS_MAX_SEGMENTS) {
			LOG_error("[HLS] Invalid segment index: %d\n", radio.hls.current_segment);
			break;
		}

		const char* seg_url = radio.hls.segments[radio.hls.current_segment].url;
		const char* seg_title = radio.hls.segments[radio.hls.current_segment].title;
		const char* seg_artist = radio.hls.segments[radio.hls.current_segment].artist;

		// Save old metadata BEFORE any updates to detect changes for album art fetch
		char old_artist[256], old_title[256];
		strncpy(old_artist, radio.metadata.artist, sizeof(old_artist) - 1);
		old_artist[sizeof(old_artist) - 1] = '\0';
		strncpy(old_title, radio.metadata.title, sizeof(old_title) - 1);
		old_title[sizeof(old_title) - 1] = '\0';

		// Update metadata from EXTINF if available (non-empty)
		if (seg_title && seg_title[0] != '\0') {
			strncpy(radio.metadata.title, seg_title, sizeof(radio.metadata.title) - 1);
		}
		if (seg_artist && seg_artist[0] != '\0' && strcmp(seg_artist, " ") != 0) {
			strncpy(radio.metadata.artist, seg_artist, sizeof(radio.metadata.artist) - 1);
		}

		// Validate URL
		if (!seg_url || seg_url[0] == '\0') {
			LOG_error("[HLS] Empty segment URL at index %d\n", radio.hls.current_segment);
			radio.hls.current_segment++;
			continue;
		}

		// Check if segment was already prefetched (protected by mutex)
		int seg_len;
		bool use_prefetch = false;
		pthread_mutex_lock(&radio.hls_mutex);
		if (radio.hls_prefetch_ready &&
			radio.hls_prefetch_segment == radio.hls.current_segment) {
			// Use prefetched data - instant, no network wait
			seg_len = radio.hls_prefetch_len;
			memcpy(segment_buf, radio.hls_prefetch_buf, seg_len);
			radio.hls_prefetch_ready = false;
			use_prefetch = true;
		}
		pthread_mutex_unlock(&radio.hls_mutex);

		if (!use_prefetch) {
			// Fallback: fetch synchronously (first segment or prefetch not ready)
			// Retry up to 3 times on failure with short delays
			int retry_count = 0;
			const int max_retries = 3;
			while (retry_count < max_retries) {
				seg_len = radio_net_fetch(seg_url, segment_buf, HLS_SEGMENT_BUF_SIZE, NULL, 0);
				if (seg_len > 0)
					break;
				retry_count++;
				if (retry_count < max_retries && !radio.should_stop) {
					usleep(100000 * retry_count); // 100ms, 200ms, 300ms delays
				}
			}
			if (seg_len <= 0) {
				LOG_error("[HLS] Failed to fetch segment after %d retries: %s\n", max_retries, seg_url);
				radio.hls.current_segment++;
				continue;
			}
		}

		// Start prefetching next segment in background while we process current one
		int next_seg = radio.hls.current_segment + 1;
		pthread_mutex_lock(&radio.hls_mutex);
		bool should_prefetch = (next_seg < radio.hls.segment_count && !radio.hls_prefetch_ready);
		pthread_mutex_unlock(&radio.hls_mutex);
		if (should_prefetch) {
			start_segment_prefetch(next_seg);
		}

		// Calculate and update bitrate from segment size and duration
		float seg_duration = radio.hls.segments[radio.hls.current_segment].duration;
		if (seg_duration > 0) {
			int bitrate = (int)((seg_len * 8.0f) / (seg_duration * 1000.0f));
			if (bitrate > 0 && bitrate < 1000) { // Sanity check (0-1000 kbps)
				radio.metadata.bitrate = bitrate;
			}
		}

		// Check for ID3 metadata at start of segment (common in HLS radio streams)
		char id3_artist[256] = "", id3_title[256] = "";
		int id3_skip = radio_hls_parse_id3_metadata(segment_buf, seg_len,
													id3_artist, sizeof(id3_artist),
													id3_title, sizeof(id3_title));
		if (id3_skip > 0) {
			// Update metadata if ID3 tags found
			if (id3_artist[0])
				strncpy(radio.metadata.artist, id3_artist, sizeof(radio.metadata.artist) - 1);
			if (id3_title[0])
				strncpy(radio.metadata.title, id3_title, sizeof(radio.metadata.title) - 1);
			// Adjust buffer to skip ID3 tag
			seg_len -= id3_skip;
			memmove(segment_buf, segment_buf + id3_skip, seg_len);
		}

		// Fetch album art if metadata changed (from either EXTINF or ID3)
		if (strcmp(old_artist, radio.metadata.artist) != 0 ||
			strcmp(old_title, radio.metadata.title) != 0) {
			album_art_fetch(radio.metadata.artist, radio.metadata.title);
		}

		// Check if segment is MPEG-TS (starts with 0x47) or raw AAC (starts with 0xFF for ADTS)
		int aac_len = 0;
		if (seg_len > 0 && segment_buf[0] == TS_SYNC_BYTE) {
			// MPEG-TS container - demux to get AAC
			aac_len = radio_hls_demux_ts(segment_buf, seg_len, aac_buf, HLS_AAC_BUF_SIZE,
										 &radio.ts_aac_pid, &radio.ts_pid_detected);
		} else {
			// Raw AAC/ADTS - use directly
			aac_len = seg_len < HLS_AAC_BUF_SIZE ? seg_len : HLS_AAC_BUF_SIZE;
			memcpy(aac_buf, segment_buf, aac_len);
		}

		// Decode AAC - process entire segment using FDK-AAC
		if (aac_len > 0) {
			// Clear transport buffer between segments to prevent overlapped audio
			aacDecoder_SetParam(radio.aac_decoder, AAC_TPDEC_CLEAR_BUFFER, 1);

			int frames_decoded = 0;
			int aac_pos = 0; // Current position in aac_buf

			// Process all AAC data from segment
			while (aac_pos < aac_len && !radio.should_stop) {
				// Feed data to FDK-AAC (it handles ADTS sync internally)
				UCHAR* inBuffer[] = {aac_buf + aac_pos};
				UINT inBufferLength[] = {(UINT)(aac_len - aac_pos)};
				UINT bytesValid[] = {(UINT)(aac_len - aac_pos)};

				aacDecoder_Fill(radio.aac_decoder, inBuffer, inBufferLength, bytesValid);

				// Decode frames until no more data
				INT_PCM decode_buf[2048 * 2]; // HE-AAC can output 2048 frames stereo
				AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(radio.aac_decoder, decode_buf, sizeof(decode_buf) / sizeof(INT_PCM), 0);

				// Update position based on consumed bytes
				int consumed = (aac_len - aac_pos) - bytesValid[0];
				aac_pos += consumed;

				if (IS_OUTPUT_VALID(err)) {
					CStreamInfo* info = aacDecoder_GetStreamInfo(radio.aac_decoder);

					// Update sample rate/channels on first successful decode
					if (info && radio.aac_sample_rate == 0 && info->sampleRate > 0) {
						radio.aac_sample_rate = info->sampleRate;
						radio.aac_channels = info->numChannels;
						// Reconfigure audio device to match stream's sample rate
						Player_setSampleRate(info->sampleRate);
						Player_resumeAudio(); // Resume after reconfiguration
					}

					if (info && info->frameSize > 0) {
						frames_decoded++;

						pthread_mutex_lock(&radio.audio_mutex);

						int samples = info->frameSize * info->numChannels;
						for (int s = 0; s < samples; s++) {
							if (radio.audio_ring_count < AUDIO_RING_SIZE) {
								radio.audio_ring[radio.audio_ring_write] = decode_buf[s];
								radio.audio_ring_write = (radio.audio_ring_write + 1) % AUDIO_RING_SIZE;
								radio.audio_ring_count++;
							}
						}

						pthread_mutex_unlock(&radio.audio_mutex);
					}
				} else if (err == AAC_DEC_NOT_ENOUGH_BITS) {
					break;
				} else if (err == AAC_DEC_TRANSPORT_SYNC_ERROR) {
					// Sync lost, try to continue with remaining data
					if (consumed == 0)
						aac_pos++; // Skip a byte to avoid infinite loop
				} else {
					// Other error, skip a byte and try again
					if (consumed == 0)
						aac_pos++;
				}
			}
		}

		// Update state based on buffer level - require 10 seconds of audio before playing
		// This provides maximum headroom for network latency
		if (radio.state == RADIO_STATE_BUFFERING &&
			radio.audio_ring_count > SAMPLE_RATE * 2 * 10) { // 10 seconds of stereo audio
			radio.state = RADIO_STATE_PLAYING;
		}

		// Track the sequence number of the segment we just played (before incrementing)
		radio.hls.last_played_sequence = radio.hls.media_sequence + radio.hls.current_segment;

		radio.hls.current_segment++;
	}


	// Note: segment_buf and aac_buf are pre-allocated in RadioContext, not freed here

	return NULL;
}

// Find MP3 sync word in buffer
// Returns offset to sync word, or -1 if not found
static int find_mp3_sync(const uint8_t* buf, int size) {
	for (int i = 0; i < size - 1; i++) {
		// MP3 sync: 0xFF followed by 0xE0-0xFF (11 bits set)
		if (buf[i] == 0xFF && (buf[i + 1] & 0xE0) == 0xE0) {
			return i;
		}
	}
	return -1;
}

// Streaming thread
static void* stream_thread_func(void* arg) {
	(void)arg; // Unused
	uint8_t recv_buf[8192];

	while (!radio.should_stop && radio.socket_fd >= 0) {
		bool has_data = false;

		// Verify SSL context is valid before use
		if (radio.use_ssl && !radio.ssl_initialized) {
			radio.state = RADIO_STATE_ERROR;
			snprintf(radio.error_msg, sizeof(radio.error_msg), "SSL context invalid");
			break;
		}

		// For SSL, check if there's pending data in the SSL buffer first
		if (radio.use_ssl && radio.ssl_initialized && mbedtls_ssl_get_bytes_avail(&radio.ssl) > 0) {
			has_data = true;
		}

		// If no pending SSL data, use select to wait for socket data
		if (!has_data) {
			fd_set read_fds;
			FD_ZERO(&read_fds);
			FD_SET(radio.socket_fd, &read_fds);

			struct timeval tv = {0, 100000}; // 100ms timeout
			int ret = select(radio.socket_fd + 1, &read_fds, NULL, NULL, &tv);

			if (ret < 0) {
				radio.state = RADIO_STATE_ERROR;
				snprintf(radio.error_msg, sizeof(radio.error_msg), "Select error");
				break;
			}

			if (ret == 0)
				continue; // Timeout, check should_stop
			has_data = true;
		}

		// Receive data
		int bytes_read = radio_recv(recv_buf, sizeof(recv_buf));
		if (bytes_read <= 0) {
			// For SSL, check if it's a non-fatal error
			if (radio.use_ssl && (bytes_read == MBEDTLS_ERR_SSL_WANT_READ ||
								  bytes_read == MBEDTLS_ERR_SSL_WANT_WRITE ||
								  bytes_read == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)) {
				continue; // Retry
			}
			// Transient network errors - could potentially implement reconnection here
			// For now, set error state and let the user retry
			radio.state = RADIO_STATE_ERROR;
			if (bytes_read == 0) {
				snprintf(radio.error_msg, sizeof(radio.error_msg), "Stream ended - server closed connection");
			} else {
				snprintf(radio.error_msg, sizeof(radio.error_msg), "Network error - connection lost");
			}
			break;
		}

		// Process received data
		int i = 0;
		while (i < bytes_read && !radio.should_stop) {
			if (radio.icy_metaint > 0 && radio.bytes_until_meta == 0) {
				// Read metadata length byte (max 255 * 16 = 4080 bytes)
				int meta_len = recv_buf[i++] * 16;
				// Validate metadata size (max 4080 bytes per ICY spec)
				if (meta_len > 4080) {
					LOG_error("ICY metadata too large: %d bytes\n", meta_len);
					meta_len = 0; // Skip invalid metadata
				}
				if (meta_len > 0 && i + meta_len <= bytes_read) {
					parse_icy_metadata(&recv_buf[i], meta_len);
					i += meta_len;
				}
				radio.bytes_until_meta = radio.icy_metaint;
			} else {
				// Calculate how many bytes to copy
				int bytes_to_copy = bytes_read - i;
				if (radio.icy_metaint > 0 && bytes_to_copy > radio.bytes_until_meta) {
					bytes_to_copy = radio.bytes_until_meta;
				}

				// Add to stream buffer if space available
				if (radio.stream_buffer_pos + bytes_to_copy <= radio.stream_buffer_size) {
					memcpy(radio.stream_buffer + radio.stream_buffer_pos, &recv_buf[i], bytes_to_copy);
					radio.stream_buffer_pos += bytes_to_copy;
				}

				i += bytes_to_copy;
				if (radio.icy_metaint > 0) {
					radio.bytes_until_meta -= bytes_to_copy;
				}
			}
		}

		// Initialize decoder once we have enough data
		if (radio.stream_buffer_pos >= 16384) {
			if (radio.audio_format == RADIO_FORMAT_AAC && !radio.aac_initialized) {
				// Initialize FDK-AAC decoder (TT_MP4_ADTS for ADTS-framed AAC streams)
				radio.aac_decoder = aacDecoder_Open(TT_MP4_ADTS, 1);
				if (radio.aac_decoder) {
					radio.aac_initialized = true;
					radio.aac_inbuf_size = 0;
					radio.aac_sample_rate = 0; // Will be set on first frame
					radio.state = RADIO_STATE_BUFFERING;
				} else {
					LOG_error("AAC decoder init failed\n");
				}
			} else if (radio.audio_format == RADIO_FORMAT_MP3 && !radio.mp3_initialized) {
				// Initialize low-level MP3 decoder for streaming

				// Find MP3 sync word first
				int sync_offset = find_mp3_sync(radio.stream_buffer, radio.stream_buffer_pos);
				if (sync_offset >= 0) {
					// Skip to sync word
					if (sync_offset > 0) {
						memmove(radio.stream_buffer, radio.stream_buffer + sync_offset,
								radio.stream_buffer_pos - sync_offset);
						radio.stream_buffer_pos -= sync_offset;
					}

					// Initialize low-level decoder
					drmp3dec_init(&radio.mp3_decoder);
					radio.mp3_initialized = true;
					radio.mp3_sample_rate = 0; // Will be set on first frame
					radio.mp3_channels = 0;
					radio.state = RADIO_STATE_BUFFERING;
				} else {
					LOG_error("No MP3 sync found in buffer\n");
				}
			}
		}

		// Decode audio based on format
		if (radio.audio_format == RADIO_FORMAT_AAC && radio.aac_initialized && radio.stream_buffer_pos >= 4096) {
			// AAC decoding
			// Copy data to AAC input buffer
			int copy_size = radio.stream_buffer_pos;
			if (radio.aac_inbuf_size + copy_size > sizeof(radio.aac_inbuf)) {
				copy_size = sizeof(radio.aac_inbuf) - radio.aac_inbuf_size;
			}
			if (copy_size > 0) {
				memcpy(radio.aac_inbuf + radio.aac_inbuf_size, radio.stream_buffer, copy_size);
				radio.aac_inbuf_size += copy_size;
				// Shift stream buffer
				memmove(radio.stream_buffer, radio.stream_buffer + copy_size, radio.stream_buffer_pos - copy_size);
				radio.stream_buffer_pos -= copy_size;
			}

			// Decode AAC frames using FDK-AAC (handles ADTS sync internally)
			while (radio.aac_inbuf_size >= 768) {
				// Feed data to FDK-AAC
				UCHAR* inBuffer[] = {radio.aac_inbuf};
				UINT inBufferLength[] = {(UINT)radio.aac_inbuf_size};
				UINT bytesValid[] = {(UINT)radio.aac_inbuf_size};

				aacDecoder_Fill(radio.aac_decoder, inBuffer, inBufferLength, bytesValid);

				// Decode one frame
				INT_PCM decode_buf[2048 * 2]; // HE-AAC can output 2048 frames stereo
				AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(radio.aac_decoder, decode_buf, sizeof(decode_buf) / sizeof(INT_PCM), 0);

				// Consume the data that FDK-AAC processed
				int consumed = radio.aac_inbuf_size - bytesValid[0];
				if (consumed > 0) {
					memmove(radio.aac_inbuf, radio.aac_inbuf + consumed, bytesValid[0]);
					radio.aac_inbuf_size = bytesValid[0];
				}

				if (IS_OUTPUT_VALID(err)) {
					CStreamInfo* info = aacDecoder_GetStreamInfo(radio.aac_decoder);

					// Update sample rate/channels on first successful decode
					if (info && radio.aac_sample_rate == 0 && info->sampleRate > 0) {
						radio.aac_sample_rate = info->sampleRate;
						radio.aac_channels = info->numChannels;
						// Reconfigure audio device to match stream's sample rate
						Player_setSampleRate(info->sampleRate);
						Player_resumeAudio(); // Resume after reconfiguration
					}

					if (info && info->frameSize > 0) {
						pthread_mutex_lock(&radio.audio_mutex);

						// Add to ring buffer
						int samples = info->frameSize * info->numChannels;
						for (int s = 0; s < samples; s++) {
							if (radio.audio_ring_count < AUDIO_RING_SIZE) {
								radio.audio_ring[radio.audio_ring_write] = decode_buf[s];
								radio.audio_ring_write = (radio.audio_ring_write + 1) % AUDIO_RING_SIZE;
								radio.audio_ring_count++;
							}
						}

						pthread_mutex_unlock(&radio.audio_mutex);
					}
				} else if (err == AAC_DEC_NOT_ENOUGH_BITS) {
					// Need more data
					break;
				} else if (err == AAC_DEC_TRANSPORT_SYNC_ERROR) {
					// Sync lost, skip a byte if nothing was consumed
					if (consumed == 0 && radio.aac_inbuf_size > 0) {
						memmove(radio.aac_inbuf, radio.aac_inbuf + 1, radio.aac_inbuf_size - 1);
						radio.aac_inbuf_size--;
					}
				} else {
					// Other error, skip a byte and try again
					if (consumed == 0 && radio.aac_inbuf_size > 0) {
						memmove(radio.aac_inbuf, radio.aac_inbuf + 1, radio.aac_inbuf_size - 1);
						radio.aac_inbuf_size--;
					}
				}
			}

			// Update state based on buffer level
			if (radio.state == RADIO_STATE_BUFFERING &&
				radio.audio_ring_count > AUDIO_RING_SIZE * 2 / 3) {
				radio.state = RADIO_STATE_PLAYING;
			}
		} else if (radio.audio_format == RADIO_FORMAT_MP3 && radio.mp3_initialized && radio.stream_buffer_pos >= 1024) {
			// MP3 decoding using low-level frame decoder
			// DRMP3_MAX_SAMPLES_PER_FRAME = 1152*2 = 2304
			int16_t decode_buf[2304 * 2]; // Stereo samples
			drmp3dec_frame_info frame_info;

			// Decode frames while we have data
			while (radio.stream_buffer_pos >= 512) {
				// Find sync word
				int sync_offset = find_mp3_sync(radio.stream_buffer, radio.stream_buffer_pos);
				if (sync_offset < 0) {
					// No sync found, keep last few bytes in case sync spans buffer boundary
					if (radio.stream_buffer_pos > 4) {
						memmove(radio.stream_buffer, radio.stream_buffer + radio.stream_buffer_pos - 4, 4);
						radio.stream_buffer_pos = 4;
					}
					break;
				}

				// Skip to sync
				if (sync_offset > 0) {
					memmove(radio.stream_buffer, radio.stream_buffer + sync_offset,
							radio.stream_buffer_pos - sync_offset);
					radio.stream_buffer_pos -= sync_offset;
				}

				// Decode frame
				int samples = drmp3dec_decode_frame(&radio.mp3_decoder,
													radio.stream_buffer,
													radio.stream_buffer_pos,
													decode_buf,
													&frame_info);

				if (samples > 0 && frame_info.frame_bytes > 0) {
					// Update sample rate/channels on first successful decode
					if (radio.mp3_sample_rate == 0) {
						radio.mp3_sample_rate = frame_info.sample_rate;
						radio.mp3_channels = frame_info.channels;
						// Reconfigure audio device to match stream's sample rate
						Player_setSampleRate(frame_info.sample_rate);
						Player_resumeAudio(); // Resume after reconfiguration
					}

					// Consume the frame
					memmove(radio.stream_buffer, radio.stream_buffer + frame_info.frame_bytes,
							radio.stream_buffer_pos - frame_info.frame_bytes);
					radio.stream_buffer_pos -= frame_info.frame_bytes;

					// Add decoded samples to ring buffer
					pthread_mutex_lock(&radio.audio_mutex);

					int total_samples = samples * frame_info.channels;
					for (int s = 0; s < total_samples; s++) {
						if (radio.audio_ring_count < AUDIO_RING_SIZE) {
							radio.audio_ring[radio.audio_ring_write] = decode_buf[s];
							radio.audio_ring_write = (radio.audio_ring_write + 1) % AUDIO_RING_SIZE;
							radio.audio_ring_count++;
						}
					}

					pthread_mutex_unlock(&radio.audio_mutex);
				} else if (frame_info.frame_bytes > 0) {
					// Invalid frame, skip it
					memmove(radio.stream_buffer, radio.stream_buffer + frame_info.frame_bytes,
							radio.stream_buffer_pos - frame_info.frame_bytes);
					radio.stream_buffer_pos -= frame_info.frame_bytes;
				} else {
					// Need more data
					break;
				}
			}

			// Update state based on buffer level
			if (radio.state == RADIO_STATE_BUFFERING &&
				radio.audio_ring_count > AUDIO_RING_SIZE * 2 / 3) {
				radio.state = RADIO_STATE_PLAYING;
			}
		}

		// If buffering and have enough data
		if (radio.state == RADIO_STATE_CONNECTING && radio.stream_buffer_pos > 0) {
			radio.state = RADIO_STATE_BUFFERING;
		}
	}

	return NULL;
}

static bool radio_initialized = false;

int Radio_init(void) {
	if (radio_initialized)
		return 0; // Already initialized

	memset(&radio, 0, sizeof(RadioContext));

	radio.socket_fd = -1;
	radio.state = RADIO_STATE_STOPPED;

	pthread_mutex_init(&radio.audio_mutex, NULL);
	pthread_mutex_init(&radio.hls_mutex, NULL);

	// Allocate buffers
	radio.stream_buffer_size = RADIO_BUFFER_SIZE;
	radio.stream_buffer = malloc(radio.stream_buffer_size);
	radio.audio_ring = malloc(AUDIO_RING_SIZE * sizeof(int16_t));

	// Pre-allocate HLS buffers to reduce memory fragmentation
	radio.hls_segment_buf = malloc(HLS_SEGMENT_BUF_SIZE);
	radio.hls_aac_buf = malloc(HLS_AAC_BUF_SIZE);
	radio.hls_prefetch_buf = malloc(HLS_SEGMENT_BUF_SIZE);
	radio.hls_prefetch_segment = -1;
	radio.hls_prefetch_ready = false;

	if (!radio.stream_buffer || !radio.audio_ring ||
		!radio.hls_segment_buf || !radio.hls_aac_buf || !radio.hls_prefetch_buf) {
		LOG_error("Radio_init: Failed to allocate buffers\n");
		Radio_quit();
		return -1;
	}

	// Load default stations
	radio.station_count = sizeof(default_stations) / sizeof(default_stations[0]);
	memcpy(radio.stations, default_stations, sizeof(default_stations));

	// Try to load custom stations
	Radio_loadStations();

	// Load curated stations from JSON files
	radio_curated_init();

	// Initialize album art module
	album_art_init();

	radio_initialized = true;
	return 0;
}

void Radio_quit(void) {
	Radio_stop();

	// Cleanup curated stations module
	radio_curated_cleanup();

	// Cleanup album art module
	album_art_cleanup();

	pthread_mutex_destroy(&radio.audio_mutex);
	pthread_mutex_destroy(&radio.hls_mutex);

	if (radio.stream_buffer) {
		free(radio.stream_buffer);
		radio.stream_buffer = NULL;
	}
	if (radio.audio_ring) {
		free(radio.audio_ring);
		radio.audio_ring = NULL;
	}
	if (radio.hls_segment_buf) {
		free(radio.hls_segment_buf);
		radio.hls_segment_buf = NULL;
	}
	if (radio.hls_aac_buf) {
		free(radio.hls_aac_buf);
		radio.hls_aac_buf = NULL;
	}
	if (radio.hls_prefetch_buf) {
		free(radio.hls_prefetch_buf);
		radio.hls_prefetch_buf = NULL;
	}

	radio_initialized = false;
}

int Radio_getStations(RadioStation** stations) {
	*stations = radio.stations;
	return radio.station_count;
}

int Radio_addStation(const char* name, const char* url, const char* genre, const char* slogan) {
	if (radio.station_count >= RADIO_MAX_STATIONS)
		return -1;

	RadioStation* s = &radio.stations[radio.station_count];
	strncpy(s->name, name, RADIO_MAX_NAME - 1);
	strncpy(s->url, url, RADIO_MAX_URL - 1);
	strncpy(s->genre, genre ? genre : "", 63);
	strncpy(s->slogan, slogan ? slogan : "", 127);
	radio.station_count++;

	return radio.station_count - 1;
}

void Radio_removeStation(int index) {
	if (index < 0 || index >= radio.station_count)
		return;

	memmove(&radio.stations[index], &radio.stations[index + 1],
			(radio.station_count - index - 1) * sizeof(RadioStation));
	radio.station_count--;
}

void Radio_saveStations(void) {
	mkdir(SHARED_USERDATA_PATH "/music-player", 0755);
	mkdir(SHARED_USERDATA_PATH "/music-player/radio", 0755);
	FILE* f = fopen(RADIO_STATIONS_FILE, "w");
	if (!f)
		return;

	for (int i = 0; i < radio.station_count; i++) {
		fprintf(f, "%s|%s|%s|%s\n",
				radio.stations[i].name,
				radio.stations[i].url,
				radio.stations[i].genre,
				radio.stations[i].slogan);
	}

	fclose(f);

	if (radio.station_count > 0) {
		radio.has_user_stations = true;
	}
}

void Radio_loadStations(void) {
	FILE* f = fopen(RADIO_STATIONS_FILE, "r");
	if (!f)
		return;

	radio.station_count = 0;

	char line[1024];
	while (fgets(line, sizeof(line), f) && radio.station_count < RADIO_MAX_STATIONS) {
		// Remove newline
		char* nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';

		// Parse: name|url|genre|slogan (slogan is optional)
		char* name = strtok(line, "|");
		char* url = strtok(NULL, "|");
		char* genre = strtok(NULL, "|");
		char* slogan = strtok(NULL, "|");

		if (name && url) {
			Radio_addStation(name, url, genre, slogan);
		}
	}

	fclose(f);

	// Mark that user has custom stations if any were loaded
	if (radio.station_count > 0) {
		radio.has_user_stations = true;
	}
}

int Radio_play(const char* url) {
	Radio_stop();

	// Reset audio device to 48000 Hz for radio playback
	Player_resetSampleRate();

	strncpy(radio.current_url, url, RADIO_MAX_URL - 1);
	radio.state = RADIO_STATE_CONNECTING;
	radio.error_msg[0] = '\0';

	// Reset buffers
	radio.stream_buffer_pos = 0;
	radio.audio_ring_write = 0;
	radio.audio_ring_read = 0;
	radio.audio_ring_count = 0;

	memset(&radio.metadata, 0, sizeof(RadioMetadata));

	// Reset HLS state
	radio.ts_pid_detected = false;
	radio.ts_aac_pid = -1;
	memset(&radio.hls, 0, sizeof(HLSContext));

	// Check if this is an HLS stream
	if (radio_hls_is_url(url)) {
		radio.stream_type = STREAM_TYPE_HLS;

		// Fetch and parse the M3U8 playlist
		uint8_t* playlist_buf = malloc(64 * 1024);
		if (!playlist_buf) {
			radio.state = RADIO_STATE_ERROR;
			snprintf(radio.error_msg, sizeof(radio.error_msg), "Memory allocation failed");
			return -1;
		}

		int len = radio_net_fetch(url, playlist_buf, 64 * 1024, NULL, 0);
		if (len <= 0) {
			free(playlist_buf);
			radio.state = RADIO_STATE_ERROR;
			snprintf(radio.error_msg, sizeof(radio.error_msg), "Failed to fetch playlist");
			return -1;
		}

		// Check if playlist was truncated (buffer full)
		if (len >= 64 * 1024 - 1) {
			LOG_error("Warning: M3U8 playlist may be truncated (>64KB)\n");
		}

		playlist_buf[len] = '\0';

		// Get base URL for resolving relative paths
		char base_url[HLS_MAX_URL_LEN];
		radio_hls_get_base_url(url, base_url, HLS_MAX_URL_LEN);

		// Initialize segment tracking for new stream
		radio.hls.current_segment = 0;
		radio.hls.last_played_sequence = -1;

		int seg_count = radio_hls_parse_playlist(&radio.hls, (char*)playlist_buf, base_url);
		free(playlist_buf);

		if (seg_count > 0) {
			// Playlist parsed successfully
		}

		if (seg_count <= 0) {
			radio.state = RADIO_STATE_ERROR;
			snprintf(radio.error_msg, sizeof(radio.error_msg), "No segments in playlist");
			return -1;
		}

		// Start HLS streaming thread with larger stack (mbedtls + getaddrinfo need more stack space)
		radio.should_stop = false;
		radio.thread_running = true;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		size_t stacksize = 1024 * 1024; // 1MB stack - getaddrinfo uses a lot of stack
		int stack_result = pthread_attr_setstacksize(&attr, stacksize);
		if (pthread_create(&radio.stream_thread, &attr, hls_stream_thread_func, NULL) != 0) {
			pthread_attr_destroy(&attr);
			radio.state = RADIO_STATE_ERROR;
			snprintf(radio.error_msg, sizeof(radio.error_msg), "Thread creation failed");
			return -1;
		}
		pthread_attr_destroy(&attr);

		// Unpause audio device for radio playback
		Player_resumeAudio();

		return 0;
	}

	// Direct stream (Shoutcast/Icecast)
	radio.stream_type = STREAM_TYPE_DIRECT;

	// Connect with redirect handling (max 5 redirects)
	char current_url[RADIO_MAX_URL];
	strncpy(current_url, url, RADIO_MAX_URL - 1);
	current_url[RADIO_MAX_URL - 1] = '\0';

	int max_redirects = 5;
	int header_result;

	for (int redirect_count = 0; redirect_count <= max_redirects; redirect_count++) {
		// Connect to stream
		if (connect_stream(current_url) != 0) {
			radio.state = RADIO_STATE_ERROR;
			return -1;
		}

		// Parse headers
		header_result = parse_headers();

		if (header_result == 0) {
			// Success - headers parsed, ready to stream
			break;
		} else if (header_result == 1) {
			// Redirect - cleanup current connection and try new URL
			if (radio.use_ssl) {
				ssl_cleanup();
				radio.use_ssl = false;
			} else {
				close(radio.socket_fd);
			}
			radio.socket_fd = -1;

			if (radio.redirect_url[0] == '\0') {
				snprintf(radio.error_msg, sizeof(radio.error_msg), "Empty redirect URL");
				radio.state = RADIO_STATE_ERROR;
				return -1;
			}

			strncpy(current_url, radio.redirect_url, RADIO_MAX_URL - 1);
			current_url[RADIO_MAX_URL - 1] = '\0';

			if (redirect_count == max_redirects) {
				snprintf(radio.error_msg, sizeof(radio.error_msg), "Too many redirects");
				radio.state = RADIO_STATE_ERROR;
				return -1;
			}
		} else {
			// Error - cleanup connection
			if (radio.use_ssl) {
				ssl_cleanup();
				radio.use_ssl = false;
			} else {
				close(radio.socket_fd);
			}
			radio.socket_fd = -1;
			radio.state = RADIO_STATE_ERROR;
			return -1;
		}
	}

	// Start streaming thread
	radio.should_stop = false;
	radio.thread_running = true;
	if (pthread_create(&radio.stream_thread, NULL, stream_thread_func, NULL) != 0) {
		if (radio.use_ssl) {
			ssl_cleanup();
			radio.use_ssl = false;
		} else {
			close(radio.socket_fd);
		}
		radio.socket_fd = -1;
		radio.state = RADIO_STATE_ERROR;
		snprintf(radio.error_msg, sizeof(radio.error_msg), "Thread creation failed");
		return -1;
	}

	// Unpause audio device for radio playback
	Player_resumeAudio();

	return 0;
}

void Radio_stop(void) {
	radio.should_stop = true;

	// Close socket immediately to unblock any pending recv() calls
	// This makes the thread exit faster instead of waiting for timeout
	if (radio.socket_fd >= 0) {
		shutdown(radio.socket_fd, SHUT_RDWR); // Unblock recv()
	}

	if (radio.thread_running) {
		pthread_join(radio.stream_thread, NULL);
		radio.thread_running = false;
	}

	// Wait for prefetch thread to finish
	if (hls_prefetch_thread_active) {
		pthread_join(hls_prefetch_thread, NULL);
		hls_prefetch_thread_active = false;
	}
	radio.hls_prefetch_ready = false;
	radio.hls_prefetch_segment = -1;

	// Cleanup SSL if active
	if (radio.use_ssl) {
		ssl_cleanup();
		radio.use_ssl = false;
	} else if (radio.socket_fd >= 0) {
		close(radio.socket_fd);
	}
	radio.socket_fd = -1;

	if (radio.mp3_initialized) {
		// Low-level drmp3dec doesn't need uninit
		radio.mp3_initialized = false;
		radio.mp3_sample_rate = 0;
		radio.mp3_channels = 0;
	}

	if (radio.aac_initialized) {
		aacDecoder_Close(radio.aac_decoder);
		radio.aac_decoder = NULL;
		radio.aac_initialized = false;
		radio.aac_inbuf_size = 0;
		radio.aac_sample_rate = 0;
		radio.aac_channels = 0;
	}

	// Reset HLS state
	radio.stream_type = STREAM_TYPE_DIRECT;
	radio.ts_pid_detected = false;
	radio.ts_aac_pid = -1;

	// Clear album art
	album_art_clear();

	radio.state = RADIO_STATE_STOPPED;

	// Pause audio device when radio stops
	Player_pauseAudio();
}

RadioState Radio_getState(void) {
	return radio.state;
}

const char* Radio_getCurrentUrl(void) {
	return radio.current_url;
}

int Radio_findCurrentStationIndex(void) {
	const char* current_url = Radio_getCurrentUrl();
	if (!current_url || current_url[0] == '\0')
		return -1;

	RadioStation* stations;
	int station_count = Radio_getStations(&stations);
	for (int i = 0; i < station_count; i++) {
		if (strcmp(stations[i].url, current_url) == 0) {
			return i;
		}
	}
	return -1;
}

const RadioMetadata* Radio_getMetadata(void) {
	return &radio.metadata;
}

float Radio_getBufferLevel(void) {
	return (float)radio.audio_ring_count / AUDIO_RING_SIZE;
}

const char* Radio_getError(void) {
	return radio.error_msg;
}

void Radio_update(void) {
	// Check for buffer underrun - transition to buffering when below 2 seconds
	// This gives time to rebuffer before audio actually runs out
	if (radio.state == RADIO_STATE_PLAYING && radio.audio_ring_count < SAMPLE_RATE * 2 * 2) {
		radio.state = RADIO_STATE_BUFFERING;
	}
}

int Radio_getAudioSamples(int16_t* buffer, int max_samples) {
	pthread_mutex_lock(&radio.audio_mutex);

	// Check for underrun and transition to buffering if needed
	// This provides faster response than waiting for Radio_update()
	// But continue to provide remaining audio to avoid abrupt silence
	if (radio.state == RADIO_STATE_PLAYING && radio.audio_ring_count < SAMPLE_RATE * 2 * 2) {
		radio.state = RADIO_STATE_BUFFERING;
		// Don't return early - continue to drain remaining buffer smoothly
	}

	int samples_to_read = max_samples;
	if (samples_to_read > radio.audio_ring_count) {
		samples_to_read = radio.audio_ring_count;
	}

	for (int i = 0; i < samples_to_read; i++) {
		buffer[i] = radio.audio_ring[radio.audio_ring_read];
		radio.audio_ring_read = (radio.audio_ring_read + 1) % AUDIO_RING_SIZE;
	}
	radio.audio_ring_count -= samples_to_read;

	// Fill rest with silence
	for (int i = samples_to_read; i < max_samples; i++) {
		buffer[i] = 0;
	}

	pthread_mutex_unlock(&radio.audio_mutex);

	return samples_to_read;
}

bool Radio_isActive(void) {
	return radio.state != RADIO_STATE_STOPPED && radio.state != RADIO_STATE_ERROR;
}

// Curated stations API - delegates to radio_curated module
int Radio_getCuratedCountryCount(void) {
	return radio_curated_get_country_count();
}

const CuratedCountry* Radio_getCuratedCountries(void) {
	return radio_curated_get_countries();
}

int Radio_getCuratedStationCount(const char* country_code) {
	return radio_curated_get_station_count(country_code);
}

const CuratedStation* Radio_getCuratedStations(const char* country_code, int* count) {
	return radio_curated_get_stations(country_code, count);
}

bool Radio_stationExists(const char* url) {
	for (int i = 0; i < radio.station_count; i++) {
		if (strcmp(radio.stations[i].url, url) == 0) {
			return true;
		}
	}
	return false;
}

bool Radio_removeStationByUrl(const char* url) {
	for (int i = 0; i < radio.station_count; i++) {
		if (strcmp(radio.stations[i].url, url) == 0) {
			Radio_removeStation(i);
			return true;
		}
	}
	return false;
}

SDL_Surface* Radio_getAlbumArt(void) {
	return album_art_get();
}

bool Radio_hasUserStations(void) {
	return radio.has_user_stations;
}
