/* Nordstjernen — FIPS 202 SHA3-512 helper used by crypto.subtle.digest.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_SHA3_H
#define ND_SHA3_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ND_SHA3_512_DIGEST_LEN 64

void nd_sha3_512(const uint8_t *data, size_t len, uint8_t out[ND_SHA3_512_DIGEST_LEN]);

#ifdef __cplusplus
}
#endif

#endif
