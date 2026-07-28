/* Nordstjernen — memory-safe PNG/GIF/BMP/JPEG decode via Wuffs.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "image.h"

#include <stdlib.h>
#include <string.h>


#include "wuffs-v0.4.c"

enum {
    NS_WUFFS_MAX_DIM    = 16384,
    NS_WUFFS_MAX_PIXELS = 64 * 1024 * 1024,
};

typedef enum {
    NS_WUFFS_NONE = 0,
    NS_WUFFS_PNG,
    NS_WUFFS_GIF,
    NS_WUFFS_BMP,
    NS_WUFFS_JPEG,
    NS_WUFFS_WEBP,
} ns_wuffs_format;

static ns_wuffs_format
ns_wuffs_detect(const guchar *data, gsize len)
{
    if (!data || len < 4) return NS_WUFFS_NONE;
    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47)
        return NS_WUFFS_PNG;
    if (len >= 6 &&
        data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8' &&
        (data[4] == '7' || data[4] == '9') && data[5] == 'a')
        return NS_WUFFS_GIF;
    if (data[0] == 'B' && data[1] == 'M')
        return NS_WUFFS_BMP;
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return NS_WUFFS_JPEG;
    if (len >= 12 &&
        data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P')
        return NS_WUFFS_WEBP;
    return NS_WUFFS_NONE;
}

gboolean
ns_image_png_is_animated(const guchar *data, gsize len)
{
    static const guchar sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (!data || len < 8 + 8 || memcmp(data, sig, sizeof sig) != 0) return FALSE;

    gsize pos = 8;
    while (pos + 8 <= len) {
        guint32 size = ((guint32)data[pos] << 24) | ((guint32)data[pos + 1] << 16) |
                       ((guint32)data[pos + 2] << 8) | (guint32)data[pos + 3];
        const guchar *tag = data + pos + 4;
        if (memcmp(tag, "IDAT", 4) == 0) return FALSE;
        if (memcmp(tag, "acTL", 4) == 0) return TRUE;
        if (memcmp(tag, "IEND", 4) == 0) return FALSE;
        if (size > len - pos - 8) return FALSE;
        pos += 12 + (gsize)size;
    }
    return FALSE;
}

static wuffs_base__image_decoder *
ns_wuffs_pick_decoder(const guchar *data, gsize len)
{
    switch (ns_wuffs_detect(data, len)) {
    case NS_WUFFS_PNG:  return wuffs_png__decoder__alloc_as__wuffs_base__image_decoder();
    case NS_WUFFS_GIF:  return wuffs_gif__decoder__alloc_as__wuffs_base__image_decoder();
    case NS_WUFFS_BMP:  return wuffs_bmp__decoder__alloc_as__wuffs_base__image_decoder();
    case NS_WUFFS_JPEG: return wuffs_jpeg__decoder__alloc_as__wuffs_base__image_decoder();
    case NS_WUFFS_WEBP: return wuffs_webp__decoder__alloc_as__wuffs_base__image_decoder();
    default:            return NULL;
    }
}

gboolean
ns_image_wuffs_supports_bytes(const guchar *data, gsize len)
{
    return ns_wuffs_detect(data, len) != NS_WUFFS_NONE;
}

guint8 *
ns_image_wuffs_decode_to_bgra(const guchar *data, gsize len,
                              int *out_w, int *out_h,
                              gsize *out_stride, gsize *out_buf_len)
{
    ns_wuffs_format kind = ns_wuffs_detect(data, len);
    wuffs_base__image_decoder *dec = ns_wuffs_pick_decoder(data, len);
    if (!dec) return NULL;

    wuffs_base__io_buffer src = wuffs_base__make_io_buffer(
        wuffs_base__make_slice_u8((uint8_t *)data, len),
        wuffs_base__make_io_buffer_meta(len, 0, 0, true));

    wuffs_base__image_config ic = {0};
    wuffs_base__status st =
        wuffs_base__image_decoder__decode_image_config(dec, &ic, &src);
    if (!wuffs_base__status__is_ok(&st) ||
        !wuffs_base__image_config__is_valid(&ic)) {
        free(dec);
        return NULL;
    }

    uint32_t w = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t h = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (w == 0 || h == 0 ||
        w > NS_WUFFS_MAX_DIM || h > NS_WUFFS_MAX_DIM ||
        (uint64_t)w * (uint64_t)h > (uint64_t)NS_WUFFS_MAX_PIXELS) {
        free(dec);
        return NULL;
    }

    wuffs_base__pixel_config__set(&ic.pixcfg,
        WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL,
        WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, w, h);

    uint64_t pix_len64 = wuffs_base__pixel_config__pixbuf_len(&ic.pixcfg);
    if (pix_len64 == 0 || pix_len64 > (uint64_t)NS_WUFFS_MAX_PIXELS * 4u) {
        free(dec);
        return NULL;
    }

    uint8_t *pix = g_try_malloc0((gsize)pix_len64);
    if (!pix) { free(dec); return NULL; }

    wuffs_base__pixel_buffer pb = {0};
    st = wuffs_base__pixel_buffer__set_from_slice(
        &pb, &ic.pixcfg,
        wuffs_base__make_slice_u8(pix, (size_t)pix_len64));
    if (!wuffs_base__status__is_ok(&st)) {
        g_free(pix);
        free(dec);
        return NULL;
    }

    uint64_t workbuf_len =
        wuffs_base__image_decoder__workbuf_len(dec).max_incl;
    uint8_t *workbuf = NULL;
    if (workbuf_len) {
        if (workbuf_len > 64u * 1024u * 1024u) {
            g_free(pix); free(dec); return NULL;
        }
        workbuf = g_try_malloc((gsize)workbuf_len);
        if (!workbuf) { g_free(pix); free(dec); return NULL; }
    }

    st = wuffs_base__image_decoder__decode_frame(
        dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC,
        wuffs_base__make_slice_u8(workbuf, (size_t)workbuf_len),
        NULL);

    g_free(workbuf);
    free(dec);

    gboolean frame_ok = wuffs_base__status__is_ok(&st) ||
                        st.repr == wuffs_base__error__too_much_data;
    if (!frame_ok && kind != NS_WUFFS_GIF) {
        g_free(pix);
        return NULL;
    }

    wuffs_base__table_u8 tab = wuffs_base__pixel_buffer__plane(&pb, 0);
    if (tab.ptr == NULL || tab.stride == 0 ||
        (uint64_t)tab.stride < (uint64_t)w * 4 ||
        (uint64_t)tab.stride * (uint64_t)h > pix_len64) {
        g_free(pix);
        return NULL;
    }

    if (out_w) *out_w = (int)w;
    if (out_h) *out_h = (int)h;
    if (out_stride) *out_stride = (gsize)tab.stride;
    if (out_buf_len) *out_buf_len = (gsize)pix_len64;
    return pix;
}

ns_texture *
ns_image_decode_wuffs(const guchar *data, gsize len, int *out_w, int *out_h)
{
    int w = 0, h = 0;
    gsize stride = 0, buf_len = 0;
    guint8 *pix = ns_image_wuffs_decode_to_bgra(data, len, &w, &h,
                                                &stride, &buf_len);
    if (!pix) return NULL;

    GBytes *bytes = g_bytes_new_take(pix, buf_len);
    ns_texture *tex = ns_texture_new(
        w, h, NS_TEXTURE_BGRA_PREMULTIPLIED, bytes, stride);
    g_bytes_unref(bytes);

    if (tex) {
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
    }
    return tex;
}

GArray *
ns_image_decode_wuffs_anim_to_pixels(const guchar *data, gsize len,
                                     int *out_w, int *out_h)
{
    if (!data || len < 6) return NULL;
    ns_wuffs_format kind = ns_wuffs_detect(data, len);
    if (kind != NS_WUFFS_GIF &&
        !(kind == NS_WUFFS_PNG && ns_image_png_is_animated(data, len)))
        return NULL;

    wuffs_base__image_decoder *dec = ns_wuffs_pick_decoder(data, len);
    if (!dec) return NULL;

    wuffs_base__io_buffer src = wuffs_base__make_io_buffer(
        wuffs_base__make_slice_u8((uint8_t *)data, len),
        wuffs_base__make_io_buffer_meta(len, 0, 0, true));

    wuffs_base__image_config ic = {0};
    wuffs_base__status st =
        wuffs_base__image_decoder__decode_image_config(dec, &ic, &src);
    if (!wuffs_base__status__is_ok(&st) ||
        !wuffs_base__image_config__is_valid(&ic)) {
        free(dec);
        return NULL;
    }

    uint32_t w = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t h = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (w == 0 || h == 0 ||
        w > NS_WUFFS_MAX_DIM || h > NS_WUFFS_MAX_DIM ||
        (uint64_t)w * (uint64_t)h > (uint64_t)NS_WUFFS_MAX_PIXELS) {
        free(dec);
        return NULL;
    }

    wuffs_base__pixel_config__set(&ic.pixcfg,
        WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL,
        WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, w, h);

    uint64_t pix_len64 = wuffs_base__pixel_config__pixbuf_len(&ic.pixcfg);
    if (pix_len64 == 0 || pix_len64 > (uint64_t)NS_WUFFS_MAX_PIXELS * 4u) {
        free(dec);
        return NULL;
    }

    uint8_t *pix = g_try_malloc0((gsize)pix_len64);
    if (!pix) { free(dec); return NULL; }

    wuffs_base__pixel_buffer pb = {0};
    st = wuffs_base__pixel_buffer__set_from_slice(
        &pb, &ic.pixcfg,
        wuffs_base__make_slice_u8(pix, (size_t)pix_len64));
    if (!wuffs_base__status__is_ok(&st)) {
        g_free(pix); free(dec); return NULL;
    }

    uint64_t workbuf_len =
        wuffs_base__image_decoder__workbuf_len(dec).max_incl;
    uint8_t *workbuf = NULL;
    if (workbuf_len) {
        if (workbuf_len > 64u * 1024u * 1024u) {
            g_free(pix); free(dec); return NULL;
        }
        workbuf = g_try_malloc((gsize)workbuf_len);
        if (!workbuf) { g_free(pix); free(dec); return NULL; }
    }

    GArray *frames = g_array_new(FALSE, FALSE, sizeof(ns_image_pixel_frame));
    g_array_set_clear_func(frames, ns_image_pixel_frame_clear);
    enum { NS_ANIM_MAX_FRAMES = 1024 };
    const gsize NS_ANIM_MAX_TOTAL_BYTES = (gsize)512 * 1024 * 1024;
    gsize anim_bytes_total = 0;

    uint8_t prev_disposal = WUFFS_BASE__ANIMATION_DISPOSAL__NONE;
    wuffs_base__rect_ie_u32 prev_dirty = {0, 0, 0, 0};
    uint8_t *backup = NULL;
    gsize    pix_bytes_total = (gsize)pix_len64;

    while (frames->len < NS_ANIM_MAX_FRAMES) {
        wuffs_base__frame_config fc = {0};
        st = wuffs_base__image_decoder__decode_frame_config(dec, &fc, &src);
        if (!wuffs_base__status__is_ok(&st)) break;

        wuffs_base__table_u8 tab = wuffs_base__pixel_buffer__plane(&pb, 0);
        if (!tab.ptr || tab.stride == 0) break;

        if (prev_disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_BACKGROUND) {
            for (uint32_t y = prev_dirty.min_incl_y; y < prev_dirty.max_excl_y; y++) {
                if ((gsize)y * (gsize)tab.stride >= (gsize)tab.stride * (gsize)h)
                    break;
                uint8_t *row = pix + (gsize)y * (gsize)tab.stride;
                gsize x0 = (gsize)prev_dirty.min_incl_x * 4;
                gsize x1 = (gsize)prev_dirty.max_excl_x * 4;
                if (x1 > tab.stride) x1 = tab.stride;
                if (x0 < x1) memset(row + x0, 0, x1 - x0);
            }
        } else if (prev_disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS
                   && backup) {
            memcpy(pix, backup, pix_bytes_total);
        }

        uint8_t cur_disposal = wuffs_base__frame_config__disposal(&fc);
        if (cur_disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS) {
            if (!backup) backup = g_try_malloc(pix_bytes_total);
            if (backup) memcpy(backup, pix, pix_bytes_total);
        }

        st = wuffs_base__image_decoder__decode_frame(
            dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC_OVER,
            wuffs_base__make_slice_u8(workbuf, (size_t)workbuf_len),
            NULL);
        if (!wuffs_base__status__is_ok(&st)) break;

        prev_disposal = cur_disposal;
        prev_dirty    = wuffs_base__frame_config__bounds(&fc);

        gsize frame_bytes = (gsize)tab.stride * (gsize)h;
        if (frame_bytes > pix_bytes_total) break;
        anim_bytes_total += frame_bytes;
        if (anim_bytes_total > NS_ANIM_MAX_TOTAL_BYTES) break;
        uint8_t *copy = g_try_malloc(frame_bytes);
        if (!copy) break;
        memcpy(copy, pix, frame_bytes);

        uint64_t flicks = wuffs_base__frame_config__duration(&fc);
        int delay_ms = (int)(flicks / 705600);
        if (delay_ms <= 0) delay_ms = 100;

        ns_image_pixel_frame f = {
            .pixels = copy,
            .pixels_len = frame_bytes,
            .stride = (gsize)tab.stride,
            .format = NS_TEXTURE_BGRA_PREMULTIPLIED,
            .width = (int)w,
            .height = (int)h,
            .delay_ms = delay_ms,
        };
        g_array_append_val(frames, f);
    }

    g_free(backup);
    g_free(workbuf);
    g_free(pix);
    free(dec);

    if (frames->len == 0) { g_array_free(frames, TRUE); return NULL; }
    if (out_w) *out_w = (int)w;
    if (out_h) *out_h = (int)h;
    return frames;
}
