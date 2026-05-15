/* Nordstjernen — AVIF decode via libavif. */

#include "image.h"

#include <string.h>

#ifdef ND_HAVE_AVIF
#include <avif/avif.h>
#endif

enum {
    ND_AVIF_MAX_DIM    = 16384,
    ND_AVIF_MAX_PIXELS = 64 * 1024 * 1024,
    ND_AVIF_MAX_INPUT  = 32 * 1024 * 1024,
};

static gboolean
nd_avif_magic(const guchar *data, gsize len)
{
    if (!data || len < 12) return FALSE;
    if (memcmp(data + 4, "ftyp", 4) != 0) return FALSE;
    static const char *const brands[] = { "avif", "avis", "mif1", "msf1", NULL };
    for (int i = 0; brands[i]; i++)
        if (memcmp(data + 8, brands[i], 4) == 0) return TRUE;
    return FALSE;
}

gboolean
nd_image_avif_supports_bytes(const guchar *data, gsize len)
{
#ifdef ND_HAVE_AVIF
    return nd_avif_magic(data, len);
#else
    (void)data; (void)len;
    return FALSE;
#endif
}

#ifdef ND_HAVE_AVIF
GdkTexture *
nd_image_decode_avif(const guchar *data, gsize len, int *out_w, int *out_h)
{
    if (!nd_avif_magic(data, len)) return NULL;
    if (len > ND_AVIF_MAX_INPUT) return NULL;

    avifDecoder *dec = avifDecoderCreate();
    if (!dec) return NULL;
    dec->maxThreads = 1;
    dec->imageSizeLimit = ND_AVIF_MAX_PIXELS;
    dec->imageDimensionLimit = ND_AVIF_MAX_DIM;
    dec->strictFlags = AVIF_STRICT_DISABLED;

    avifResult r = avifDecoderSetIOMemory(dec, data, len);
    if (r != AVIF_RESULT_OK) { avifDecoderDestroy(dec); return NULL; }
    r = avifDecoderParse(dec);
    if (r != AVIF_RESULT_OK) { avifDecoderDestroy(dec); return NULL; }
    r = avifDecoderNextImage(dec);
    if (r != AVIF_RESULT_OK) { avifDecoderDestroy(dec); return NULL; }

    uint32_t w = dec->image->width, h = dec->image->height;
    if (w == 0 || h == 0 ||
        w > ND_AVIF_MAX_DIM || h > ND_AVIF_MAX_DIM ||
        (uint64_t)w * (uint64_t)h > (uint64_t)ND_AVIF_MAX_PIXELS) {
        avifDecoderDestroy(dec);
        return NULL;
    }

    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, dec->image);
    rgb.format = AVIF_RGB_FORMAT_BGRA;
    rgb.depth = 8;
    if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK) {
        avifDecoderDestroy(dec);
        return NULL;
    }
    r = avifImageYUVToRGB(dec->image, &rgb);
    if (r != AVIF_RESULT_OK) {
        avifRGBImageFreePixels(&rgb);
        avifDecoderDestroy(dec);
        return NULL;
    }

    gsize stride = rgb.rowBytes;
    gsize total = stride * (gsize)h;
    guint8 *pix = g_try_malloc(total);
    if (!pix) {
        avifRGBImageFreePixels(&rgb);
        avifDecoderDestroy(dec);
        return NULL;
    }
    for (uint32_t y = 0; y < h; y++) {
        guint8 *src = rgb.pixels + (gsize)y * stride;
        guint8 *dst = pix + (gsize)y * stride;
        for (uint32_t x = 0; x < w; x++) {
            guint8 b = src[4*x + 0], g = src[4*x + 1];
            guint8 rd = src[4*x + 2], a = src[4*x + 3];
            uint32_t bm = (uint32_t)b * (uint32_t)a / 255;
            uint32_t gm = (uint32_t)g * (uint32_t)a / 255;
            uint32_t rm = (uint32_t)rd * (uint32_t)a / 255;
            dst[4*x + 0] = (guint8)bm;
            dst[4*x + 1] = (guint8)gm;
            dst[4*x + 2] = (guint8)rm;
            dst[4*x + 3] = a;
        }
    }
    avifRGBImageFreePixels(&rgb);
    avifDecoderDestroy(dec);

    GBytes *bytes = g_bytes_new_take(pix, total);
    GdkTexture *tex = gdk_memory_texture_new(
        (int)w, (int)h,
        GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
        bytes, stride);
    g_bytes_unref(bytes);
    if (tex) {
        if (out_w) *out_w = (int)w;
        if (out_h) *out_h = (int)h;
    }
    return tex;
}
#else
GdkTexture *
nd_image_decode_avif(const guchar *data, gsize len, int *out_w, int *out_h)
{
    (void)data; (void)len; (void)out_w; (void)out_h;
    return NULL;
}
#endif
