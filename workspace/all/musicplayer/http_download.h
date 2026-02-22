#ifndef HTTP_DOWNLOAD_H
#define HTTP_DOWNLOAD_H

#include <stdbool.h>

// Download timeout in seconds
#define HTTP_DOWNLOAD_TIMEOUT_SECONDS 30

// Maximum redirect depth
#define HTTP_DOWNLOAD_MAX_REDIRECTS 10

// Chunk size for downloads (32KB)
#define HTTP_DOWNLOAD_CHUNK_SIZE 32768

/**
 * Download a file from HTTP/HTTPS URL to local filesystem.
 * Supports:
 * - HTTP and HTTPS (with SSL via mbedtls)
 * - Automatic redirect following (301, 302, 303, 307, 308)
 * - Chunked transfer encoding
 * - Progress reporting
 * - Cancellation via flag
 *
 * @param url            The URL to download from
 * @param filepath       Local path to save the file
 * @param progress_pct   Optional pointer to receive progress (0-100), can be NULL
 * @param should_stop    Optional pointer to cancellation flag, can be NULL
 * @return               Number of bytes downloaded on success, -1 on failure
 */
int http_download_file(const char* url, const char* filepath,
					   volatile int* progress_pct, volatile bool* should_stop);

#endif // HTTP_DOWNLOAD_H
