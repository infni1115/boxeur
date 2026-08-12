/* Public domain SHA1 implementation (based on Steve Reid's classic sha1.c).
 * Used only to compute the Sec-WebSocket-Accept handshake value, so we do
 * not need to link against OpenSSL or any other crypto library. */
#ifndef SHA1_H
#define SHA1_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    unsigned char buffer[64];
} SHA1_CTX;

void sha1_init(SHA1_CTX *context);
void sha1_update(SHA1_CTX *context, const unsigned char *data, size_t len);
void sha1_final(unsigned char digest[20], SHA1_CTX *context);

/* Convenience: hash `len` bytes of `data` into `digest` (20 bytes). */
void sha1_buffer(const unsigned char *data, size_t len, unsigned char digest[20]);

#endif
