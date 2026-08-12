#include "base64.h"
#include <stdint.h>

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encode(const unsigned char *data, size_t len, char *out) {
    size_t i, o = 0;

    for (i = 0; i + 2 < len; i += 3) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[o++] = b64_table[(n >> 18) & 0x3F];
        out[o++] = b64_table[(n >> 12) & 0x3F];
        out[o++] = b64_table[(n >> 6) & 0x3F];
        out[o++] = b64_table[n & 0x3F];
    }

    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)data[i] << 16;
        out[o++] = b64_table[(n >> 18) & 0x3F];
        out[o++] = b64_table[(n >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out[o++] = b64_table[(n >> 18) & 0x3F];
        out[o++] = b64_table[(n >> 12) & 0x3F];
        out[o++] = b64_table[(n >> 6) & 0x3F];
        out[o++] = '=';
    }

    out[o] = '\0';
    return o;
}
