#define _GNU_SOURCE
#include "http_download.h"
#include "radio_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

// TLS 1.3: mbedtls_ssl_read/write may return non-fatal errors that require retry
#define SSL_READ_IS_RETRYABLE(r)          \
	((r) == MBEDTLS_ERR_SSL_WANT_READ ||  \
	 (r) == MBEDTLS_ERR_SSL_WANT_WRITE || \
	 (r) == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)

#include "api.h"

// SSL context for HTTPS downloads
typedef struct {
	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
	bool initialized;
} HttpSSLContext;

// Internal implementation with redirect depth tracking
static int http_download_file_internal(const char* url, const char* filepath,
									   volatile int* progress_percent,
									   volatile bool* should_stop, int redirect_depth) {
	if (!url || !filepath) {
		LOG_error("[HTTP] download: invalid parameters\n");
		return -1;
	}

	if (redirect_depth >= HTTP_DOWNLOAD_MAX_REDIRECTS) {
		LOG_error("[HTTP] download: too many redirects\n");
		return -1;
	}

	// Parse URL
	char* host = (char*)malloc(256);
	char* path = (char*)malloc(2048);
	if (!host || !path) {
		free(host);
		free(path);
		return -1;
	}

	int port;
	bool is_https;

	if (radio_net_parse_url(url, host, 256, &port, path, 2048, &is_https) != 0) {
		LOG_error("[HTTP] download: failed to parse URL: %s\n", url);
		free(host);
		free(path);
		return -1;
	}

	int sock_fd = -1;
	HttpSSLContext* ssl_ctx = NULL;
	char* header_buf = NULL;
	FILE* outfile = NULL;
	int result = -1;

	if (is_https) {
		ssl_ctx = (HttpSSLContext*)calloc(1, sizeof(HttpSSLContext));
		if (!ssl_ctx) {
			LOG_error("[HTTP] download: failed to allocate SSL context\n");
			free(host);
			free(path);
			return -1;
		}

		const char* pers = "http_download";
		mbedtls_net_init(&ssl_ctx->net);
		mbedtls_ssl_init(&ssl_ctx->ssl);
		mbedtls_ssl_config_init(&ssl_ctx->conf);
		mbedtls_entropy_init(&ssl_ctx->entropy);
		mbedtls_ctr_drbg_init(&ssl_ctx->ctr_drbg);

		if (mbedtls_ctr_drbg_seed(&ssl_ctx->ctr_drbg, mbedtls_entropy_func, &ssl_ctx->entropy,
								  (const unsigned char*)pers, strlen(pers)) != 0) {
			goto cleanup;
		}

		if (mbedtls_ssl_config_defaults(&ssl_ctx->conf, MBEDTLS_SSL_IS_CLIENT,
										MBEDTLS_SSL_TRANSPORT_STREAM,
										MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
			goto cleanup;
		}

		mbedtls_ssl_conf_authmode(&ssl_ctx->conf, MBEDTLS_SSL_VERIFY_NONE);
		mbedtls_ssl_conf_rng(&ssl_ctx->conf, mbedtls_ctr_drbg_random, &ssl_ctx->ctr_drbg);

		if (mbedtls_ssl_setup(&ssl_ctx->ssl, &ssl_ctx->conf) != 0) {
			goto cleanup;
		}

		mbedtls_ssl_set_hostname(&ssl_ctx->ssl, host);

		char port_str[16];
		snprintf(port_str, sizeof(port_str), "%d", port);

		int connect_ret = mbedtls_net_connect(&ssl_ctx->net, host, port_str, MBEDTLS_NET_PROTO_TCP);
		if (connect_ret != 0) {
			LOG_error("[HTTP] download: mbedtls_net_connect failed: %d (host=%s, port=%s)\n",
					  connect_ret, host, port_str);
			goto cleanup;
		}

		// Set socket timeout
		int ssl_sock_fd = ssl_ctx->net.fd;
		struct timeval tv = {HTTP_DOWNLOAD_TIMEOUT_SECONDS, 0};
		setsockopt(ssl_sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(ssl_sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		mbedtls_ssl_set_bio(&ssl_ctx->ssl, &ssl_ctx->net, mbedtls_net_send, mbedtls_net_recv, NULL);

		// SSL handshake
		int ret;
		int handshake_retries = 0;
		const int max_handshake_retries = 100;
		while ((ret = mbedtls_ssl_handshake(&ssl_ctx->ssl)) != 0) {
			if (should_stop && *should_stop)
				goto cleanup;
			// TLS 1.3: session ticket received means handshake is complete
			if (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
				break;
			}
			if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
				LOG_error("[HTTP] download: SSL handshake failed: -0x%04X\n", -ret);
				goto cleanup;
			}
			if (++handshake_retries > max_handshake_retries) {
				LOG_error("[HTTP] download: SSL handshake timeout\n");
				goto cleanup;
			}
			usleep(100000);
		}

		ssl_ctx->initialized = true;
		sock_fd = ssl_ctx->net.fd;
	} else {
		// Plain HTTP
		struct addrinfo hints, *ai_result = NULL;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;

		char port_str[16];
		snprintf(port_str, sizeof(port_str), "%d", port);

		if (getaddrinfo(host, port_str, &hints, &ai_result) != 0 || !ai_result) {
			LOG_error("[HTTP] download: getaddrinfo failed for %s\n", host);
			if (ai_result)
				freeaddrinfo(ai_result);
			free(host);
			free(path);
			return -1;
		}

		sock_fd = socket(ai_result->ai_family, ai_result->ai_socktype, ai_result->ai_protocol);
		if (sock_fd < 0) {
			freeaddrinfo(ai_result);
			free(host);
			free(path);
			return -1;
		}

		struct timeval tv = {HTTP_DOWNLOAD_TIMEOUT_SECONDS, 0};
		setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		if (connect(sock_fd, ai_result->ai_addr, ai_result->ai_addrlen) < 0) {
			LOG_error("[HTTP] download: connect failed: %s\n", strerror(errno));
			close(sock_fd);
			freeaddrinfo(ai_result);
			free(host);
			free(path);
			return -1;
		}
		freeaddrinfo(ai_result);
	}

	// Send HTTP request
	char request[2560];
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
		sent = mbedtls_ssl_write(&ssl_ctx->ssl, (unsigned char*)request, strlen(request));
	} else {
		sent = send(sock_fd, request, strlen(request), 0);
	}

	if (sent < 0) {
		LOG_error("[HTTP] download: failed to send request\n");
		goto cleanup;
	}

// Read headers
#define HEADER_BUF_SIZE 4096
	header_buf = (char*)malloc(HEADER_BUF_SIZE);
	if (!header_buf)
		goto cleanup;

	int header_pos = 0;
	bool headers_done = false;

	while (header_pos < HEADER_BUF_SIZE - 1) {
		if (should_stop && *should_stop)
			goto cleanup;

		char c;
		int r;
		if (is_https) {
			r = mbedtls_ssl_read(&ssl_ctx->ssl, (unsigned char*)&c, 1);
			if (SSL_READ_IS_RETRYABLE(r)) {
				usleep(10000);
				continue;
			}
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
		LOG_error("[HTTP] download: failed to read headers\n");
		goto cleanup;
	}

	// Check for redirect
	char* first_line_end = strstr(header_buf, "\r\n");
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

			char redirect_url[1024];
			int rlen = end - loc;
			if (rlen >= (int)sizeof(redirect_url))
				rlen = sizeof(redirect_url) - 1;
			strncpy(redirect_url, loc, rlen);
			redirect_url[rlen] = '\0';

			// Cleanup current connection
			if (ssl_ctx) {
				mbedtls_ssl_close_notify(&ssl_ctx->ssl);
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

			// Follow redirect
			return http_download_file_internal(redirect_url, filepath, progress_percent, should_stop, redirect_depth + 1);
		}
		goto cleanup;
	}

	// Parse Content-Length
	long content_length = -1;
	char* cl = strcasestr(header_buf, "\nContent-Length:");
	if (cl) {
		cl += 16;
		while (*cl == ' ')
			cl++;
		content_length = atol(cl);
	}

	// Check for chunked transfer encoding
	bool is_chunked = (strcasestr(header_buf, "Transfer-Encoding: chunked") != NULL);

	// Open output file
	outfile = fopen(filepath, "wb");
	if (!outfile) {
		LOG_error("[HTTP] download: failed to open file: %s\n", filepath);
		goto cleanup;
	}

	// Download body in chunks
	uint8_t* chunk_buf = (uint8_t*)malloc(HTTP_DOWNLOAD_CHUNK_SIZE);
	if (!chunk_buf) {
		fclose(outfile);
		outfile = NULL;
		goto cleanup;
	}

	long total_read = 0;
	int read_retries = 0;
	const int max_read_retries = 50;

	if (is_chunked) {
		// Handle chunked transfer encoding
		char chunk_size_buf[20];
		int chunk_size_pos = 0;

		while (1) {
			if (should_stop && *should_stop)
				break;

			// Read chunk size
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
					continue;
				if (c == '\n')
					break;
				chunk_size_buf[chunk_size_pos++] = c;
			}
			chunk_size_buf[chunk_size_pos] = '\0';

			long chunk_size = strtol(chunk_size_buf, NULL, 16);
			if (chunk_size <= 0)
				break;

			// Read chunk data
			long chunk_read = 0;
			while (chunk_read < chunk_size) {
				if (should_stop && *should_stop)
					goto chunked_done;

				int to_read = (chunk_size - chunk_read) < HTTP_DOWNLOAD_CHUNK_SIZE ? (int)(chunk_size - chunk_read) : HTTP_DOWNLOAD_CHUNK_SIZE;
				int r;
				if (is_https) {
					r = mbedtls_ssl_read(&ssl_ctx->ssl, chunk_buf, to_read);
					if (SSL_READ_IS_RETRYABLE(r)) {
						if (++read_retries > max_read_retries)
							goto chunked_done;
						usleep(10000);
						continue;
					}
					read_retries = 0;
				} else {
					r = recv(sock_fd, chunk_buf, to_read, 0);
				}
				if (r <= 0)
					goto chunked_done;

				fwrite(chunk_buf, 1, r, outfile);
				chunk_read += r;
				total_read += r;
			}

			// Skip trailing CRLF
			char crlf[2];
			int crlf_read = 0;
			while (crlf_read < 2) {
				int r;
				if (is_https) {
					r = mbedtls_ssl_read(&ssl_ctx->ssl, (unsigned char*)&crlf[crlf_read], 1);
					if (SSL_READ_IS_RETRYABLE(r)) {
						usleep(10000);
						continue;
					}
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
		// Non-chunked: read directly with progress
		while (1) {
			if (should_stop && *should_stop)
				break;

			int r;
			if (is_https) {
				r = mbedtls_ssl_read(&ssl_ctx->ssl, chunk_buf, HTTP_DOWNLOAD_CHUNK_SIZE);
				if (SSL_READ_IS_RETRYABLE(r)) {
					if (++read_retries > max_read_retries)
						break;
					usleep(10000);
					continue;
				}
				read_retries = 0;
			} else {
				r = recv(sock_fd, chunk_buf, HTTP_DOWNLOAD_CHUNK_SIZE, 0);
			}
			if (r <= 0)
				break;

			fwrite(chunk_buf, 1, r, outfile);
			total_read += r;

			// Update progress
			if (progress_percent && content_length > 0) {
				int pct = (int)((total_read * 100) / content_length);
				if (pct > 100)
					pct = 100;
				*progress_percent = pct;
			}
		}
	}

	free(chunk_buf);
	fclose(outfile);
	outfile = NULL;

	if (total_read > 0) {
		result = (int)total_read;
		if (progress_percent)
			*progress_percent = 100;
	}

cleanup:
	if (outfile)
		fclose(outfile);
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
	return result;
}

int http_download_file(const char* url, const char* filepath,
					   volatile int* progress_pct, volatile bool* should_stop) {
	return http_download_file_internal(url, filepath, progress_pct, should_stop, 0);
}
