/* Nordstjernen — WebP decode via libwebp.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "image.h"

#include <string.h>
#include <webp/decode.h>
#include <webp/demux.h>

enum {
    NS_WEBP_MAX_DIM    = 16384,
    NS_WEBP_MAX_PIXELS = 64 * 1024 * 1024,
    NS_WEBP_MAX_INPUT  = 64 * 1024 * 1024,
    NS_WEBP_MAX_FRAMES = 4096,
    NS_WEBP_MAX_TOTAL_BYTES = 512 * 1024 * 1024,
};

gboolean
ns_image_webp_supports_bytes(const guchar *data, gsize len)
{
    return data && len >= 12 &&
           memcmp(data, "RIFF", 4) == 0 &&
           memcmp(data + 8, "WEBP", 4) == 0;
}

static guint8 *
ns_image_webp_decode_still_premultiplied(const guchar *data, gsize len,
                                         int *out_w, int *out_h,
                                         gsize *out_stride)
{
    if (!ns_image_webp_supports_bytes(data, len)) return NULL;
    if (len > NS_WEBP_MAX_INPUT) return NULL;
    int w = 0, h = 0;
    if (!WebPGetInfo(data, len, &w, &h)) return NULL;
    if (w <= 0 || h <= 0 || w > NS_WEBP_MAX_DIM || h > NS_WEBP_MAX_DIM ||
        (guint64)w * (guint64)h > (guint64)NS_WEBP_MAX_PIXELS) return NULL;
    gsize stride = (gsize)w * 4;
    gsize size = stride * (gsize)h;
    guint8 *pix = g_try_malloc(size);
    if (!pix) return NULL;
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)) {
        g_free(pix);
        return NULL;
    }
    config.output.colorspace = MODE_bgrA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = pix;
    config.output.u.RGBA.stride = (int)stride;
    config.output.u.RGBA.size = size;
    if (WebPDecode(data, len, &config) != VP8_STATUS_OK) {
        g_free(pix);
        return NULL;
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_stride) *out_stride = stride;
    return pix;
}

static guint8 *
ns_image_webp_decode_animation_first_frame(const guchar *data, gsize len,
                                           int *out_w, int *out_h,
                                           gsize *out_stride)
{
    if (!ns_image_webp_supports_bytes(data, len) || len > NS_WEBP_MAX_INPUT)
        return NULL;
    WebPAnimDecoderOptions opts;
    if (!WebPAnimDecoderOptionsInit(&opts)) return NULL;
    opts.color_mode = MODE_bgrA;
    WebPData wd = { data, len };
    WebPAnimDecoder *dec = WebPAnimDecoderNew(&wd, &opts);
    if (!dec) return NULL;
    WebPAnimInfo info;
    guint8 *buf = NULL;
    int ts = 0;
    if (!WebPAnimDecoderGetInfo(dec, &info) ||
        info.canvas_width == 0 || info.canvas_height == 0 ||
        info.canvas_width > NS_WEBP_MAX_DIM ||
        info.canvas_height > NS_WEBP_MAX_DIM ||
        (guint64)info.canvas_width * (guint64)info.canvas_height >
            (guint64)NS_WEBP_MAX_PIXELS ||
        !WebPAnimDecoderHasMoreFrames(dec) ||
        !WebPAnimDecoderGetNext(dec, &buf, &ts)) {
        WebPAnimDecoderDelete(dec);
        return NULL;
    }
    gsize stride = (gsize)info.canvas_width * 4;
    gsize size = stride * (gsize)info.canvas_height;
    guint8 *pix = g_try_malloc(size);
    if (pix) memcpy(pix, buf, size);
    WebPAnimDecoderDelete(dec);
    if (!pix) return NULL;
    if (out_w) *out_w = (int)info.canvas_width;
    if (out_h) *out_h = (int)info.canvas_height;
    if (out_stride) *out_stride = stride;
    return pix;
}

static guint8 *
ns_image_webp_decode_premultiplied(const guchar *data, gsize len,
                                   int *out_w, int *out_h, gsize *out_stride)
{
    guint8 *pix = ns_image_webp_decode_still_premultiplied(
        data, len, out_w, out_h, out_stride);
    if (pix) return pix;
    return ns_image_webp_decode_animation_first_frame(data, len,
                                                     out_w, out_h,
                                                     out_stride);
}

ns_texture *
ns_image_decode_webp(const guchar *data, gsize len, int *out_w, int *out_h)
{
    int w = 0, h = 0;
    gsize stride = 0;
    guint8 *pix = ns_image_webp_decode_premultiplied(data, len, &w, &h,
                                                     &stride);
    if (!pix) return NULL;
    GBytes *bytes = g_bytes_new_take(pix, stride * (gsize)h);
    ns_texture *tex = ns_texture_new(w, h, NS_TEXTURE_BGRA_PREMULTIPLIED,
                                     bytes, stride);
    g_bytes_unref(bytes);
    if (tex) {
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
    }
    return tex;
}

GArray *
ns_image_decode_webp_anim_to_pixels(const guchar *data, gsize len,
                                    int *out_w, int *out_h)
{
    if (!ns_image_webp_supports_bytes(data, len) || len > NS_WEBP_MAX_INPUT)
        return NULL;
    WebPAnimDecoderOptions opts;
    if (!WebPAnimDecoderOptionsInit(&opts)) return NULL;
    opts.color_mode = MODE_bgrA;
    WebPData wd = { data, len };
    WebPAnimDecoder *dec = WebPAnimDecoderNew(&wd, &opts);
    if (!dec) return NULL;
    WebPAnimInfo info;
    if (!WebPAnimDecoderGetInfo(dec, &info) || info.frame_count < 2 ||
        info.frame_count > NS_WEBP_MAX_FRAMES ||
        info.canvas_width == 0 || info.canvas_height == 0 ||
        info.canvas_width > NS_WEBP_MAX_DIM ||
        info.canvas_height > NS_WEBP_MAX_DIM ||
        (guint64)info.canvas_width * (guint64)info.canvas_height >
            (guint64)NS_WEBP_MAX_PIXELS) {
        WebPAnimDecoderDelete(dec);
        return NULL;
    }
    gsize stride = (gsize)info.canvas_width * 4;
    gsize size = stride * (gsize)info.canvas_height;
    GArray *frames = g_array_new(FALSE, FALSE, sizeof(ns_image_pixel_frame));
    g_array_set_clear_func(frames, ns_image_pixel_frame_clear);
    int prev_ts = 0;
    gboolean ok = TRUE;
    gsize anim_bytes_total = 0;
    while (WebPAnimDecoderHasMoreFrames(dec)) {
        guint8 *buf = NULL;
        int ts = 0;
        if (!WebPAnimDecoderGetNext(dec, &buf, &ts)) {
            ok = FALSE;
            break;
        }
        if (anim_bytes_total + size > (gsize)NS_WEBP_MAX_TOTAL_BYTES)
            break;
        anim_bytes_total += size;
        ns_image_pixel_frame f = {0};
        f.pixels = g_try_malloc(size);
        if (!f.pixels) {
            ok = FALSE;
            break;
        }
        memcpy(f.pixels, buf, size);
        f.pixels_len = size;
        f.stride = stride;
        f.format = NS_TEXTURE_BGRA_PREMULTIPLIED;
        f.width = (int)info.canvas_width;
        f.height = (int)info.canvas_height;
        f.delay_ms = ts - prev_ts;
        prev_ts = ts;
        g_array_append_val(frames, f);
    }
    WebPAnimDecoderDelete(dec);
    if (!ok || frames->len < 2) {
        g_array_free(frames, TRUE);
        return NULL;
    }
    if (out_w) *out_w = (int)info.canvas_width;
    if (out_h) *out_h = (int)info.canvas_height;
    return frames;
}

guint8 *
ns_image_webp_decode_to_bgra(const guchar *data, gsize len,
                             int *out_w, int *out_h,
                             gsize *out_stride, gsize *out_buf_len)
{
    int w = 0, h = 0;
    gsize stride = 0;
    guint8 *pix = ns_image_webp_decode_premultiplied(data, len, &w, &h,
                                                     &stride);
    if (!pix) return NULL;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_stride) *out_stride = stride;
    if (out_buf_len) *out_buf_len = stride * (gsize)h;
    return pix;
}
