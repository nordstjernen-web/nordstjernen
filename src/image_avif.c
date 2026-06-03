/* Nordstjernen — AVIF decode via libavif.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "image.h"

#include <avif/avif.h>

enum {
    ND_AVIF_MAX_DIM    = 16384,
    ND_AVIF_MAX_PIXELS = 64 * 1024 * 1024,
    ND_AVIF_MAX_INPUT  = 32 * 1024 * 1024,
};

gboolean
nd_image_avif_supports_bytes(const guchar *data, gsize len)
{
    if (!data || len < 12) return FALSE;
    if (memcmp(data + 4, "ftyp", 4) != 0) return FALSE;
    static const char *const brands[] = { "avif", "avis", "mif1", "msf1", NULL };
    for (int i = 0; brands[i]; i++)
        if (memcmp(data + 8, brands[i], 4) == 0) return TRUE;
    return FALSE;
}

nd_texture *
nd_image_decode_avif(const guchar *data, gsize len, int *out_w, int *out_h)
{
    if (!nd_image_avif_supports_bytes(data, len)) return NULL;
    if (len > ND_AVIF_MAX_INPUT) return NULL;

    avifDecoder *dec = avifDecoderCreate();
    if (!dec) return NULL;
    dec->maxThreads = 1;
    dec->imageSizeLimit = ND_AVIF_MAX_PIXELS;
    dec->imageDimensionLimit = ND_AVIF_MAX_DIM;
    dec->strictFlags = AVIF_STRICT_DISABLED;

    avifRGBImage rgb = {0};
    nd_texture *tex = NULL;

    if (avifDecoderSetIOMemory(dec, data, len) != AVIF_RESULT_OK) goto out;
    if (avifDecoderParse(dec)                  != AVIF_RESULT_OK) goto out;
    if (avifDecoderNextImage(dec)              != AVIF_RESULT_OK) goto out;

    uint32_t w = dec->image->width, h = dec->image->height;
    if (w == 0 || h == 0 || w > ND_AVIF_MAX_DIM || h > ND_AVIF_MAX_DIM ||
        (uint64_t)w * (uint64_t)h > (uint64_t)ND_AVIF_MAX_PIXELS) goto out;

    avifRGBImageSetDefaults(&rgb, dec->image);
    rgb.format = AVIF_RGB_FORMAT_BGRA;
    rgb.depth  = 8;
    rgb.alphaPremultiplied = AVIF_TRUE;
    if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK) goto out;
    if (avifImageYUVToRGB(dec->image, &rgb) != AVIF_RESULT_OK) goto out;

    if (rgb.rowBytes < (uint32_t)w * 4) goto out;
    if (h && (gsize)rgb.rowBytes > G_MAXSIZE / (gsize)h) goto out;
    gsize buf_size = (gsize)rgb.rowBytes * (gsize)h;
    GBytes *bytes = g_bytes_new(rgb.pixels, buf_size);
    tex = nd_texture_new((int)w, (int)h,
                                 ND_TEXTURE_BGRA_PREMULTIPLIED,
                                 bytes, rgb.rowBytes);
    g_bytes_unref(bytes);
    if (tex) {
        if (out_w) *out_w = (int)w;
        if (out_h) *out_h = (int)h;
    }

out:
    if (rgb.pixels) avifRGBImageFreePixels(&rgb);
    avifDecoderDestroy(dec);
    return tex;
}
