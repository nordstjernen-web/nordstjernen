/* Nordstjernen — FIPS 202 SHA3-512 (Keccak-f[1600], rate 576, capacity 1024).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "sha3.h"

#include <string.h>

static const uint64_t k_rc[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL,
    0x8000000080008000ULL, 0x000000000000808BULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008AULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000AULL,
    0x000000008000808BULL, 0x800000000000008BULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800AULL, 0x800000008000000AULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};

static const unsigned k_rot[24] = {
    1,  3,  6, 10, 15, 21, 28, 36, 45, 55,  2, 14,
   27, 41, 56,  8, 25, 43, 62, 18, 39, 61, 20, 44,
};

static const unsigned k_pi[24] = {
   10,  7, 11, 17, 18,  3,  5, 16,  8, 21, 24,  4,
   15, 23, 19, 13, 12,  2, 20, 14, 22,  9,  6,  1,
};

static inline uint64_t rol(uint64_t x, unsigned r) {
    return (x << r) | (x >> (64 - r));
}

static void
keccak_f1600(uint64_t s[25])
{
    uint64_t b[5], t;
    for (int r = 0; r < 24; r++) {
        for (int i = 0; i < 5; i++)
            b[i] = s[i] ^ s[i + 5] ^ s[i + 10] ^ s[i + 15] ^ s[i + 20];
        for (int i = 0; i < 5; i++) {
            t = b[(i + 4) % 5] ^ rol(b[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5) s[j + i] ^= t;
        }
        t = s[1];
        for (int i = 0; i < 24; i++) {
            int j = k_pi[i];
            b[0] = s[j];
            s[j] = rol(t, k_rot[i]);
            t = b[0];
        }
        for (int j = 0; j < 25; j += 5) {
            for (int i = 0; i < 5; i++) b[i] = s[j + i];
            for (int i = 0; i < 5; i++)
                s[j + i] = b[i] ^ ((~b[(i + 1) % 5]) & b[(i + 2) % 5]);
        }
        s[0] ^= k_rc[r];
    }
}

static inline void
absorb_lane_byte(uint64_t *s, size_t i, uint8_t v)
{
    s[i >> 3] ^= (uint64_t)v << (8 * (i & 7));
}

void
nd_sha3_512(const uint8_t *data, size_t len, uint8_t out[ND_SHA3_512_DIGEST_LEN])
{
    uint64_t s[25] = {0};
    const size_t rate = 72;
    size_t i;

    while (len >= rate) {
        for (i = 0; i < rate; i++)
            absorb_lane_byte(s, i, data[i]);
        keccak_f1600(s);
        data += rate;
        len -= rate;
    }

    uint8_t block[72] = {0};
    if (len) memcpy(block, data, len);
    block[len] = 0x06;
    block[rate - 1] |= 0x80;
    for (i = 0; i < rate; i++)
        absorb_lane_byte(s, i, block[i]);
    keccak_f1600(s);

    for (i = 0; i < ND_SHA3_512_DIGEST_LEN; i++)
        out[i] = (uint8_t)(s[i >> 3] >> (8 * (i & 7)));
}
