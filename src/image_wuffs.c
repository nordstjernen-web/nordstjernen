/* Nordstjernen — memory-safe PNG/GIF/BMP/JPEG decode via Wuffs.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "image.h"

#include <stdlib.h>
#include <string.h>


#include "wuffs-v0.4.c"

enum {
    ND_WUFFS_MAX_DIM    = 16384,
    ND_WUFFS_MAX_PIXELS = 64 * 1024 * 1024,
};

typedef enum {
    ND_WUFFS_NONE = 0,
    ND_WUFFS_PNG,
    ND_WUFFS_GIF,
    ND_WUFFS_BMP,
    ND_WUFFS_JPEG,
    ND_WUFFS_WEBP,
} nd_wuffs_format;

static nd_wuffs_format
nd_wuffs_detect(const guchar *data, gsize len)
{
    if (!data || len < 4) return ND_WUFFS_NONE;
    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47)
        return ND_WUFFS_PNG;
    if (len >= 6 &&
        data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8' &&
        (data[4] == '7' || data[4] == '9') && data[5] == 'a')
        return ND_WUFFS_GIF;
    if (data[0] == 'B' && data[1] == 'M')
        return ND_WUFFS_BMP;
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return ND_WUFFS_JPEG;
    if (len >= 12 &&
        data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P')
        return ND_WUFFS_WEBP;
    return ND_WUFFS_NONE;
}

static wuffs_base__image_decoder *
nd_wuffs_pick_decoder(const guchar *data, gsize len)
{
    switch (nd_wuffs_detect(data, len)) {
    case ND_WUFFS_PNG:  return wuffs_png__decoder__alloc_as__wuffs_base__image_decoder();
    case ND_WUFFS_GIF:  return wuffs_gif__decoder__alloc_as__wuffs_base__image_decoder();
    case ND_WUFFS_BMP:  return wuffs_bmp__decoder__alloc_as__wuffs_base__image_decoder();
    case ND_WUFFS_JPEG: return wuffs_jpeg__decoder__alloc_as__wuffs_base__image_decoder();
    case ND_WUFFS_WEBP: return wuffs_webp__decoder__alloc_as__wuffs_base__image_decoder();
    default:            return NULL;
    }
}

gboolean
nd_image_wuffs_supports_bytes(const guchar *data, gsize len)
{
    return nd_wuffs_detect(data, len) != ND_WUFFS_NONE;
}

guint8 *
nd_image_wuffs_decode_to_bgra(const guchar *data, gsize len,
                              int *out_w, int *out_h,
                              gsize *out_stride, gsize *out_buf_len)
{
    wuffs_base__image_decoder *dec = nd_wuffs_pick_decoder(data, len);
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
        w > ND_WUFFS_MAX_DIM || h > ND_WUFFS_MAX_DIM ||
        (uint64_t)w * (uint64_t)h > (uint64_t)ND_WUFFS_MAX_PIXELS) {
        free(dec);
        return NULL;
    }

    wuffs_base__pixel_config__set(&ic.pixcfg,
        WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL,
        WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, w, h);

    uint64_t pix_len64 = wuffs_base__pixel_config__pixbuf_len(&ic.pixcfg);
    if (pix_len64 == 0 || pix_len64 > (uint64_t)ND_WUFFS_MAX_PIXELS * 4u) {
        free(dec);
        return NULL;
    }

    uint8_t *pix = g_try_malloc((gsize)pix_len64);
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

    if (!wuffs_base__status__is_ok(&st)) {
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

nd_texture *
nd_image_decode_wuffs(const guchar *data, gsize len, int *out_w, int *out_h)
{
    int w = 0, h = 0;
    gsize stride = 0, buf_len = 0;
    guint8 *pix = nd_image_wuffs_decode_to_bgra(data, len, &w, &h,
                                                &stride, &buf_len);
    if (!pix) return NULL;

    GBytes *bytes = g_bytes_new_take(pix, buf_len);
    nd_texture *tex = nd_texture_new(
        w, h, ND_TEXTURE_BGRA_PREMULTIPLIED, bytes, stride);
    g_bytes_unref(bytes);

    if (tex) {
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
    }
    return tex;
}

GArray *
nd_image_decode_wuffs_anim(const guchar *data, gsize len, int *out_w, int *out_h)
{
    if (!data || len < 6) return NULL;
    if (!(data[0] == 'G' && data[1] == 'I' && data[2] == 'F' &&
          data[3] == '8' && (data[4] == '7' || data[4] == '9') &&
          data[5] == 'a')) return NULL;

    wuffs_base__image_decoder *dec =
        wuffs_gif__decoder__alloc_as__wuffs_base__image_decoder();
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
        w > ND_WUFFS_MAX_DIM || h > ND_WUFFS_MAX_DIM ||
        (uint64_t)w * (uint64_t)h > (uint64_t)ND_WUFFS_MAX_PIXELS) {
        free(dec);
        return NULL;
    }

    wuffs_base__pixel_config__set(&ic.pixcfg,
        WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL,
        WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, w, h);

    uint64_t pix_len64 = wuffs_base__pixel_config__pixbuf_len(&ic.pixcfg);
    if (pix_len64 == 0 || pix_len64 > (uint64_t)ND_WUFFS_MAX_PIXELS * 4u) {
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

    GArray *frames = g_array_new(FALSE, FALSE, sizeof(nd_image_anim_frame));
    enum { ND_GIF_MAX_FRAMES = 1024 };
    const gsize ND_GIF_MAX_TOTAL_BYTES = (gsize)512 * 1024 * 1024;
    gsize anim_bytes_total = 0;

    uint8_t prev_disposal = WUFFS_BASE__ANIMATION_DISPOSAL__NONE;
    wuffs_base__rect_ie_u32 prev_dirty = {0, 0, 0, 0};
    uint8_t *backup = NULL;
    gsize    pix_bytes_total = (gsize)pix_len64;

    while (frames->len < ND_GIF_MAX_FRAMES) {
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
        if (anim_bytes_total > ND_GIF_MAX_TOTAL_BYTES) break;
        uint8_t *copy = g_try_malloc(frame_bytes);
        if (!copy) break;
        memcpy(copy, pix, frame_bytes);

        GBytes *bytes = g_bytes_new_take(copy, frame_bytes);
        nd_texture *tex = nd_texture_new(
            (int)w, (int)h,
            ND_TEXTURE_BGRA_PREMULTIPLIED,
            bytes, (gsize)tab.stride);
        g_bytes_unref(bytes);
        if (!tex) break;

        uint64_t flicks = wuffs_base__frame_config__duration(&fc);
        int delay_ms = (int)(flicks / 705600);
        if (delay_ms <= 0) delay_ms = 100;

        nd_image_anim_frame f = { tex, delay_ms };
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
