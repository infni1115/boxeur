#ifndef BASE64_H
#define BASE64_H

#include <stddef.h>

/* Encodes `len` bytes from `data` as base64 into `out`, which must be at
 * least base64_encoded_len(len) bytes. Writes a terminating NUL.
 * Returns the number of characters written (excluding the NUL). */
size_t base64_encode(const unsigned char *data, size_t len, char *out);

/* Size (including the terminating NUL) needed to base64-encode `len` bytes. */
#define BASE64_ENCODED_LEN(len) ((((len) + 2) / 3) * 4 + 1)

#endif
