/* Public domain SHA1 implementation (based on Steve Reid's classic sha1.c). */
#include "sha1.h"
#include <string.h>

#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

static uint32_t blk0(unsigned char *block, int i) {
    uint32_t w;
    memcpy(&w, block + i * 4, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    w = (rol(w, 24) & 0xFF00FF00) | (rol(w, 8) & 0x00FF00FF);
#endif
    return w;
}

static uint32_t blk(uint32_t block[16], int i) {
    uint32_t v = block[(i + 13) & 15] ^ block[(i + 8) & 15] ^ block[(i + 2) & 15] ^ block[i & 15];
    return rol(v, 1);
}

static void sha1_transform(uint32_t state[5], unsigned char buffer[64]) {
    uint32_t a, b, c, d, e;
    uint32_t block[16];

    for (int i = 0; i < 16; i++) block[i] = blk0(buffer, i);

    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k, w;
        if (i < 16) {
            w = block[i];
        } else {
            w = blk(block, i);
            block[i & 15] = w;
        }
        if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }

        uint32_t temp = rol(a, 5) + f + e + k + w;
        e = d; d = c; c = rol(b, 30); b = a; a = temp;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

void sha1_init(SHA1_CTX *context) {
    context->state[0] = 0x67452301;
    context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE;
    context->state[3] = 0x10325476;
    context->state[4] = 0xC3D2E1F0;
    context->count[0] = context->count[1] = 0;
}

void sha1_update(SHA1_CTX *context, const unsigned char *data, size_t len) {
    size_t i, j;

    j = (context->count[0] >> 3) & 63;
    if ((context->count[0] += (uint32_t)(len << 3)) < (uint32_t)(len << 3))
        context->count[1]++;
    context->count[1] += (uint32_t)(len >> 29);

    if ((j + len) > 63) {
        i = 64 - j;
        memcpy(&context->buffer[j], data, i);
        sha1_transform(context->state, context->buffer);
        for (; i + 63 < len; i += 64)
            sha1_transform(context->state, (unsigned char *)&data[i]);
        j = 0;
    } else {
        i = 0;
    }
    memcpy(&context->buffer[j], &data[i], len - i);
}

void sha1_final(unsigned char digest[20], SHA1_CTX *context) {
    unsigned char finalcount[8];
    unsigned char c;

    for (int i = 0; i < 8; i++)
        finalcount[i] = (unsigned char)((context->count[(i >= 4 ? 0 : 1)] >>
                                          ((3 - (i & 3)) * 8)) & 255);

    c = 0200;
    sha1_update(context, &c, 1);
    while ((context->count[0] & 504) != 448) {
        c = 0000;
        sha1_update(context, &c, 1);
    }
    sha1_update(context, finalcount, 8);

    for (int i = 0; i < 20; i++)
        digest[i] = (unsigned char)((context->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
}

void sha1_buffer(const unsigned char *data, size_t len, unsigned char digest[20]) {
    SHA1_CTX ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(digest, &ctx);
}
