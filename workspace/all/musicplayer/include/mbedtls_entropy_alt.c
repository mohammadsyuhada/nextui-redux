/**
 * Custom entropy source using /dev/urandom
 */
#include "mbedtls/entropy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(MBEDTLS_ENTROPY_HARDWARE_ALT)

int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen) {
	(void)data;
	*olen = 0;

	FILE* f = fopen("/dev/urandom", "rb");
	if (f == NULL) {
		return -1;
	}

	size_t read_len = fread(output, 1, len, f);
	fclose(f);

	if (read_len != len) {
		return -1;
	}

	*olen = len;
	return 0;
}

#endif /* MBEDTLS_ENTROPY_HARDWARE_ALT */
