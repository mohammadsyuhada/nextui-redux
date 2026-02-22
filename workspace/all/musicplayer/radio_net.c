#define _GNU_SOURCE
#include "radio_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "api.h"

// zlib for gzip decompression (some CDNs send gzip despite Accept-Encoding: identity)
#include <zlib.h>

// mbedTLS for HTTPS support
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

// TLS 1.3: mbedtls_ssl_read/write may return non-fatal errors that require retry
#define SSL_READ_IS_RETRYABLE(r)          \
	((r) == MBEDTLS_ERR_SSL_WANT_READ ||  \
	 (r) == MBEDTLS_ERR_SSL_WANT_WRITE || \
	 (r) == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
// SSL context for fetch operations (heap allocated to save stack space)
typedef struct {
	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
	bool initialized;
} FetchSSLContext;

// Parse URL into host, port, path, and detect HTTPS
int radio_net_parse_url(const char* url, char* host, int host_size,
						int* port, char* path, int path_size, bool* is_https) {
	if (!url || !host || !path || host_size < 1 || path_size < 1) {
		return -1;
	}

	*is_https = false;
	*port = 80;
	host[0] = '\0';
	path[0] = '\0';

	// Skip protocol
	const char* start = url;
	if (strncmp(url, "https://", 8) == 0) {
		start = url + 8;
		*is_https = true;
		*port = 443;
	} else if (strncmp(url, "http://", 7) == 0) {
		start = url + 7;
	}

	// Find path
	const char* path_start = strchr(start, '/');
	if (path_start) {
		snprintf(path, path_size, "%s", path_start);
	} else {
		strncpy(path, "/", path_size - 1);
		path[path_size - 1] = '\0';
		path_start = start + strlen(start);
	}

	// Find port
	const char* port_start = strchr(start, ':');
	if (port_start && port_start < path_start) {
		*port = atoi(port_start + 1);
		int host_len = port_start - start;
		if (host_len >= host_size)
			host_len = host_size - 1;
		memcpy(host, start, host_len);
		host[host_len] = '\0';
	} else {
		int host_len = path_start - start;
		if (host_len >= host_size)
			host_len = host_size - 1;
		memcpy(host, start, host_len);
		host[host_len] = '\0';
	}

	return 0;
}

// Maximum redirect depth to prevent infinite redirect loops
#define RADIO_NET_MAX_REDIRECTS 10

// Network timeout in seconds (configurable for slow WiFi connections)
#define RADIO_NET_TIMEOUT_SECONDS 15

// Internal fetch with redirect depth tracking
static int radio_net_fetch_internal(const char* url, uint8_t* buffer, int buffer_size,
									char* content_type, int ct_size, int redirect_depth);

// Fetch content from URL into buffer
// Returns bytes read, or -1 on error
int radio_net_fetch(const char* url, uint8_t* buffer, int buffer_size,
					char* content_type, int ct_size) {
	return radio_net_fetch_internal(url, buffer, buffer_size, content_type, ct_size, 0);
}

// Internal fetch with redirect depth tracking
static int radio_net_fetch_internal(const char* url, uint8_t* buffer, int buffer_size,
									char* content_type, int ct_size, int redirect_depth) {
	if (!url || !buffer || buffer_size <= 0) {
		LOG_error("[RadioNet] Invalid parameters\n");
		return -1;
	}

	// Check redirect depth limit
	if (redirect_depth >= RADIO_NET_MAX_REDIRECTS) {
		LOG_error("[RadioNet] Too many redirects (max %d)\n", RADIO_NET_MAX_REDIRECTS);
		return -1;
	}

	// Use heap for URL components to reduce stack usage
	char* host = (char*)malloc(256);
	char* path = (char*)malloc(512);
	if (!host || !path) {
		LOG_error("[RadioNet] Failed to allocate host/path buffers\n");
		free(host);
		free(path);
		return -1;
	}

	int port;
	bool is_https;

	if (radio_net_parse_url(url, host, 256, &port, path, 512, &is_https) != 0) {
		LOG_error("[RadioNet] Failed to parse URL: %s\n", url);
		free(host);
		free(path);
		return -1;
	}

	int sock_fd = -1;
	FetchSSLContext* ssl_ctx = NULL;
	char* header_buf = NULL;

	if (is_https) {
		// Allocate SSL context on heap to avoid stack overflow
		ssl_ctx = (FetchSSLContext*)calloc(1, sizeof(FetchSSLContext));
		if (!ssl_ctx) {
			LOG_error("[RadioNet] Failed to allocate SSL context\n");
			free(host);
			free(path);
			return -1;
		}

		const char* pers = "radio_net_fetch";
		mbedtls_net_init(&ssl_ctx->net);
		mbedtls_ssl_init(&ssl_ctx->ssl);
		mbedtls_ssl_config_init(&ssl_ctx->conf);
		mbedtls_entropy_init(&ssl_ctx->entropy);
		mbedtls_ctr_drbg_init(&ssl_ctx->ctr_drbg);

		int ssl_ret;
		ssl_ret = mbedtls_ctr_drbg_seed(&ssl_ctx->ctr_drbg, mbedtls_entropy_func, &ssl_ctx->entropy,
										(const unsigned char*)pers, strlen(pers));
		if (ssl_ret != 0) {
			LOG_error("[RadioNet] mbedtls_ctr_drbg_seed failed: %d\n", ssl_ret);
			goto cleanup;
		}

		ssl_ret = mbedtls_ssl_config_defaults(&ssl_ctx->conf, MBEDTLS_SSL_IS_CLIENT,
											  MBEDTLS_SSL_TRANSPORT_STREAM,
											  MBEDTLS_SSL_PRESET_DEFAULT);
		if (ssl_ret != 0) {
			LOG_error("[RadioNet] mbedtls_ssl_config_defaults failed: %d\n", ssl_ret);
			goto cleanup;
		}
		mbedtls_ssl_conf_authmode(&ssl_ctx->conf, MBEDTLS_SSL_VERIFY_NONE);
		mbedtls_ssl_conf_rng(&ssl_ctx->conf, mbedtls_ctr_drbg_random, &ssl_ctx->ctr_drbg);

		ssl_ret = mbedtls_ssl_setup(&ssl_ctx->ssl, &ssl_ctx->conf);
		if (ssl_ret != 0) {
			LOG_error("[RadioNet] mbedtls_ssl_setup failed: %d\n", ssl_ret);
			goto cleanup;
		}

		mbedtls_ssl_set_hostname(&ssl_ctx->ssl, host);

		char port_str[16];
		snprintf(port_str, sizeof(port_str), "%d", port);

		int connect_ret = mbedtls_net_connect(&ssl_ctx->net, host, port_str, MBEDTLS_NET_PROTO_TCP);
		if (connect_ret != 0) {
			LOG_error("[RadioNet] mbedtls_net_connect failed: %d (host=%s, port=%s)\n", connect_ret, host, port_str);
			goto cleanup;
		}

		// Set socket timeout for SSL operations to prevent indefinite blocking
		int ssl_sock_fd = ssl_ctx->net.fd;
		struct timeval tv = {RADIO_NET_TIMEOUT_SECONDS, 0};
		setsockopt(ssl_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(ssl_sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		mbedtls_ssl_set_bio(&ssl_ctx->ssl, &ssl_ctx->net, mbedtls_net_send, mbedtls_net_recv, NULL);

		// SSL handshake with timeout protection (max 10 seconds)
		int ret;
		int handshake_retries = 0;
		const int max_handshake_retries = 100; // 100 * 100ms = 10 seconds max
		while ((ret = mbedtls_ssl_handshake(&ssl_ctx->ssl)) != 0) {
			// TLS 1.3: session ticket received means handshake is complete
			if (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
				break;
			}
			if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
				LOG_error("[RadioNet] SSL handshake failed: -0x%04X host=%s\n", -ret, host);
				goto cleanup;
			}
			if (++handshake_retries > max_handshake_retries) {
				LOG_error("[RadioNet] SSL handshake timeout\n");
				goto cleanup;
			}
			usleep(100000); // 100ms between retries
		}

		ssl_ctx->initialized = true;
		sock_fd = ssl_ctx->net.fd;
	} else {
		// Use getaddrinfo instead of gethostbyname (thread-safe)
		struct addrinfo hints, *result = NULL;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;

		char port_str[16];
		snprintf(port_str, sizeof(port_str), "%d", port);

		int gai_ret = getaddrinfo(host, port_str, &hints, &result);
		if (gai_ret != 0 || !result) {
			LOG_error("[RadioNet] getaddrinfo failed for host: %s (error: %d)\n", host, gai_ret);
			if (result)
				freeaddrinfo(result);
			free(host);
			free(path);
			return -1;
		}

		sock_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
		if (sock_fd < 0) {
			LOG_error("[RadioNet] socket() failed\n");
			freeaddrinfo(result);
			free(host);
			free(path);
			return -1;
		}

		struct timeval tv = {RADIO_NET_TIMEOUT_SECONDS, 0}; // Configurable timeout for slow WiFi
		if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ||
			setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
			LOG_error("[RadioNet] setsockopt() failed\n");
			close(sock_fd);
			freeaddrinfo(result);
			free(host);
			free(path);
			return -1;
		}

		if (connect(sock_fd, result->ai_addr, result->ai_addrlen) < 0) {
			LOG_error("[RadioNet] connect() failed: %s\n", strerror(errno));
			close(sock_fd);
			freeaddrinfo(result);
			free(host);
			free(path);
			return -1;
		}
		freeaddrinfo(result);
	}

	// Send HTTP request (use HTTP/1.1 with proper headers for CDN compatibility)
	char request[1024];
	snprintf(request, sizeof(request),
			 "GET %s HTTP/1.1\r\n"
			 "Host: %s\r\n"
			 "User-Agent: Mozilla/5.0 (Linux) AppleWebKit/537.36\r\n"
			 "Accept: */*\r\n"
			 "Accept-Encoding: identity\r\n"
			 "Connection: close\r\n"
			 "\r\n",
			 path, host);

	int sent;
	if (is_https) {
		int write_retries = 0;
		do {
			sent = mbedtls_ssl_write(&ssl_ctx->ssl, (unsigned char*)request, strlen(request));
		} while (SSL_READ_IS_RETRYABLE(sent) && ++write_retries < 10);
	} else {
		sent = send(sock_fd, request, strlen(request), 0);
	}

	if (sent < 0) {
		LOG_error("[RadioNet] Failed to send HTTP request: %d\n", sent);
		goto cleanup;
	}

// Read response - allocate header buffer on heap to reduce stack pressure
// 8KB to handle servers with many headers (e.g., megaphone.fm CDNs)
#define HEADER_BUF_SIZE 8192
	header_buf = (char*)malloc(HEADER_BUF_SIZE);
	if (!header_buf) {
		LOG_error("[RadioNet] Failed to allocate header buffer\n");
		goto cleanup;
	}
	int header_pos = 0;
	bool headers_done = false;

	// Read headers with timeout protection
	int header_retries = 0;
	const int max_header_retries = 150; // 150 * 10ms = 1.5 seconds max for headers
	while (header_pos < HEADER_BUF_SIZE - 1) {
		char c;
		int r;
		if (is_https) {
			r = mbedtls_ssl_read(&ssl_ctx->ssl, (unsigned char*)&c, 1);
			if (SSL_READ_IS_RETRYABLE(r)) {
				if (++header_retries > max_header_retries) {
					LOG_error("[RadioNet] Header read timeout\n");
					break;
				}
				usleep(10000); // 10ms
				continue;
			}
			header_retries = 0;
		} else {
			r = recv(sock_fd, &c, 1, 0);
		}
		if (r != 1)
			break;

		header_buf[header_pos++] = c;
		if (header_pos >= 4 &&
			header_buf[header_pos - 4] == '\r' && header_buf[header_pos - 3] == '\n' &&
			header_buf[header_pos - 2] == '\r' && header_buf[header_pos - 1] == '\n') {
			headers_done = true;
			break;
		}
	}
	header_buf[header_pos] = '\0';

	if (!headers_done) {
		LOG_error("[RadioNet] Failed to receive complete HTTP headers (got %d bytes)\n", header_pos);
		goto cleanup;
	}

	// Find end of first line (status line) for redirect detection
	char* first_line_end = strstr(header_buf, "\r\n");

	// Check for redirect - only check the status line (first line), not entire headers
	bool is_redirect = false;
	if (first_line_end) {
		char* status_start = strstr(header_buf, "HTTP/");
		if (status_start && status_start < first_line_end) {
			is_redirect = (strstr(status_start, " 301 ") && strstr(status_start, " 301 ") < first_line_end) ||
						  (strstr(status_start, " 302 ") && strstr(status_start, " 302 ") < first_line_end) ||
						  (strstr(status_start, " 303 ") && strstr(status_start, " 303 ") < first_line_end) ||
						  (strstr(status_start, " 307 ") && strstr(status_start, " 307 ") < first_line_end) ||
						  (strstr(status_start, " 308 ") && strstr(status_start, " 308 ") < first_line_end);
		}
	}
	if (is_redirect) {
		char* loc = strcasestr(header_buf, "\nLocation:");
		if (loc) {
			loc += 10;
			while (*loc == ' ')
				loc++;
			char* end = loc;
			while (*end && *end != '\r' && *end != '\n')
				end++;

			// Copy redirect URL before cleanup
			char redirect_url[1024];
			int rlen = end - loc;
			if (rlen >= (int)sizeof(redirect_url))
				rlen = sizeof(redirect_url) - 1;
			strncpy(redirect_url, loc, rlen);
			redirect_url[rlen] = '\0';

			// Cleanup current connection and buffers before recursive call
			if (ssl_ctx) {
				mbedtls_ssl_close_notify(&ssl_ctx->ssl);
				mbedtls_net_free(&ssl_ctx->net);
				mbedtls_ssl_free(&ssl_ctx->ssl);
				mbedtls_ssl_config_free(&ssl_ctx->conf);
				mbedtls_ctr_drbg_free(&ssl_ctx->ctr_drbg);
				mbedtls_entropy_free(&ssl_ctx->entropy);
				free(ssl_ctx);
			} else {
				close(sock_fd);
			}
			free(header_buf);
			free(host);
			free(path);

			// Follow redirect with incremented depth
			return radio_net_fetch_internal(redirect_url, buffer, buffer_size, content_type, ct_size, redirect_depth + 1);
		}
		LOG_error("[RadioNet] Redirect response has no Location header\n");
		goto cleanup;
	}

	// Check HTTP status code - reject 4xx/5xx errors
	int http_status = 0;
	{
		char* status_ptr = strstr(header_buf, "HTTP/");
		if (status_ptr) {
			char* space = strchr(status_ptr, ' ');
			if (space) {
				http_status = atoi(space + 1);
			}
		}
	}
	if (http_status >= 400) {
		LOG_error("[RadioNet] HTTP %d error for: %s\n", http_status, url);
		goto cleanup;
	}

	// Extract content type if requested
	if (content_type && ct_size > 0) {
		content_type[0] = '\0';
		char* ct = strcasestr(header_buf, "\nContent-Type:");
		if (ct) {
			ct += 14;
			while (*ct == ' ')
				ct++;
			char* end = ct;
			while (*end && *end != '\r' && *end != '\n' && *end != ';')
				end++;
			int len = end - ct;
			if (len < ct_size) {
				strncpy(content_type, ct, len);
				content_type[len] = '\0';
			}
		}
	}

	// Check for chunked transfer encoding
	// Look for "chunked" in the Transfer-Encoding header value, tolerant of whitespace
	bool is_chunked = false;
	{
		char* te = strcasestr(header_buf, "\nTransfer-Encoding:");
		if (te) {
			char* line_end = strstr(te + 1, "\r\n");
			if (line_end) {
				char* ck = strcasestr(te, "chunked");
				if (ck && ck < line_end) {
					is_chunked = true;
				}
			}
		}
	}

	// Read body with timeout protection
	int total_read = 0;
	int read_retries = 0;
	const int max_read_retries = 50; // Limit retries on WANT_READ/WANT_WRITE

	if (is_chunked) {
		// Handle chunked transfer encoding - read and decode incrementally
		char chunk_size_buf[20];
		int chunk_size_pos = 0;

		while (total_read < buffer_size - 1) {
			// Read chunk size line (hex number followed by \r\n)
			chunk_size_pos = 0;
			while (chunk_size_pos < (int)sizeof(chunk_size_buf) - 1) {
				char c;
				int r;
				if (is_https) {
					r = mbedtls_ssl_read(&ssl_ctx->ssl, (unsigned char*)&c, 1);
					if (SSL_READ_IS_RETRYABLE(r)) {
						if (++read_retries > max_read_retries)
							break;
						usleep(10000);
						continue;
					}
					read_retries = 0;
				} else {
					r = recv(sock_fd, &c, 1, 0);
				}
				if (r != 1)
					goto chunked_done;
				if (c == '\r')
					continue; // Skip CR
				if (c == '\n')
					break; // End of chunk size line
				chunk_size_buf[chunk_size_pos++] = c;
			}
			chunk_size_buf[chunk_size_pos] = '\0';

			// Parse chunk size
			long chunk_size = strtol(chunk_size_buf, NULL, 16);
			if (chunk_size <= 0)
				break; // End of chunks or error

			// Read chunk data
			int chunk_read = 0;
			while (chunk_read < chunk_size && total_read < buffer_size - 1) {
				int to_read = chunk_size - chunk_read;
				if (total_read + to_read > buffer_size - 1) {
					to_read = buffer_size - 1 - total_read;
				}
				int r;
				if (is_https) {
					r = mbedtls_ssl_read(&ssl_ctx->ssl, buffer + total_read, to_read);
					if (SSL_READ_IS_RETRYABLE(r)) {
						if (++read_retries > max_read_retries)
							goto chunked_done;
						usleep(10000);
						continue;
					}
					read_retries = 0;
				} else {
					r = recv(sock_fd, buffer + total_read, to_read, 0);
				}
				if (r <= 0)
					goto chunked_done;
				total_read += r;
				chunk_read += r;
			}

			// Skip any remaining chunk data if buffer is full
			while (chunk_read < chunk_size) {
				char discard[256];
				int to_discard = chunk_size - chunk_read;
				if (to_discard > (int)sizeof(discard))
					to_discard = sizeof(discard);
				int r;
				if (is_https) {
					r = mbedtls_ssl_read(&ssl_ctx->ssl, (unsigned char*)discard, to_discard);
					if (SSL_READ_IS_RETRYABLE(r)) {
						if (++read_retries > max_read_retries)
							goto chunked_done;
						usleep(10000);
						continue;
					}
					read_retries = 0;
				} else {
					r = recv(sock_fd, discard, to_discard, 0);
				}
				if (r <= 0)
					goto chunked_done;
				chunk_read += r;
			}

			// Skip trailing \r\n after chunk data
			char crlf[2];
			int crlf_read = 0;
			while (crlf_read < 2) {
				int r;
				if (is_https) {
					r = mbedtls_ssl_read(&ssl_ctx->ssl, (unsigned char*)&crlf[crlf_read], 1);
					if (SSL_READ_IS_RETRYABLE(r)) {
						if (++read_retries > max_read_retries)
							goto chunked_done;
						usleep(10000);
						continue;
					}
					read_retries = 0;
				} else {
					r = recv(sock_fd, &crlf[crlf_read], 1, 0);
				}
				if (r != 1)
					goto chunked_done;
				crlf_read++;
			}
		}
	chunked_done:;
	} else {
		// Non-chunked: read directly
		while (total_read < buffer_size - 1) {
			int r;
			if (is_https) {
				r = mbedtls_ssl_read(&ssl_ctx->ssl, buffer + total_read, buffer_size - total_read - 1);
				if (SSL_READ_IS_RETRYABLE(r)) {
					if (++read_retries > max_read_retries) {
						LOG_error("[RadioNet] SSL read timeout (too many retries)\n");
						break;
					}
					usleep(10000); // 10ms between retries
					continue;
				}
				read_retries = 0; // Reset on successful read
			} else {
				r = recv(sock_fd, buffer + total_read, buffer_size - total_read - 1, 0);
			}
			if (r <= 0)
				break;
			total_read += r;
		}
	}

	// Decompress gzip if Content-Encoding indicates it or gzip magic bytes detected
	bool is_gzip = false;
	if (header_buf) {
		char* ce = strcasestr(header_buf, "\nContent-Encoding:");
		if (ce) {
			ce += 18;
			while (*ce == ' ')
				ce++;
			if (strncasecmp(ce, "gzip", 4) == 0) {
				is_gzip = true;
			}
		}
	}
	// Also detect gzip by magic bytes (0x1f 0x8b) as fallback
	if (!is_gzip && total_read >= 2 && buffer[0] == 0x1f && buffer[1] == 0x8b) {
		is_gzip = true;
	}

	if (is_gzip && total_read > 0) {
		uint8_t* decompressed = (uint8_t*)malloc(buffer_size);
		if (decompressed) {
			z_stream strm;
			memset(&strm, 0, sizeof(strm));
			strm.next_in = buffer;
			strm.avail_in = total_read;
			strm.next_out = decompressed;
			strm.avail_out = buffer_size - 1;

			// MAX_WBITS + 16 tells zlib to detect gzip header
			if (inflateInit2(&strm, MAX_WBITS + 16) == Z_OK) {
				int zret = inflate(&strm, Z_FINISH);
				if (zret == Z_STREAM_END || zret == Z_OK) {
					int decompressed_size = strm.total_out;
					memcpy(buffer, decompressed, decompressed_size);
					total_read = decompressed_size;
				} else {
					LOG_error("[RadioNet] gzip decompression failed: %d\n", zret);
				}
				inflateEnd(&strm);
			}
			free(decompressed);
		}
	}

	// Cleanup
	if (ssl_ctx) {
		mbedtls_ssl_close_notify(&ssl_ctx->ssl);
		mbedtls_net_free(&ssl_ctx->net);
		mbedtls_ssl_free(&ssl_ctx->ssl);
		mbedtls_ssl_config_free(&ssl_ctx->conf);
		mbedtls_ctr_drbg_free(&ssl_ctx->ctr_drbg);
		mbedtls_entropy_free(&ssl_ctx->entropy);
		free(ssl_ctx);
	} else {
		close(sock_fd);
	}

	free(header_buf);
	free(host);
	free(path);
	return total_read;

cleanup:
	if (ssl_ctx) {
		if (ssl_ctx->initialized) {
			mbedtls_ssl_close_notify(&ssl_ctx->ssl);
		}
		mbedtls_net_free(&ssl_ctx->net);
		mbedtls_ssl_free(&ssl_ctx->ssl);
		mbedtls_ssl_config_free(&ssl_ctx->conf);
		mbedtls_ctr_drbg_free(&ssl_ctx->ctr_drbg);
		mbedtls_entropy_free(&ssl_ctx->entropy);
		free(ssl_ctx);
	} else if (sock_fd >= 0) {
		close(sock_fd);
	}
	free(header_buf);
	free(host);
	free(path);
	return -1;
}
