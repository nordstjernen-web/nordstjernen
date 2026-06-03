/* Nordstjernen — public C API for embedding the browser engine.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef LIBNORDSTJERNEN_H
#define LIBNORDSTJERNEN_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nd_browser nd_browser;

int nd_browser_init(void);

nd_browser *nd_browser_open(const char *url, int viewport_width, int settle_ms);

char *nd_browser_render_text(nd_browser *browser);

int nd_browser_render_image(nd_browser *browser, const char *path);

/* Total laid-out page size in CSS pixels. Returns 0 on success. */
int nd_browser_page_size(nd_browser *browser, int *out_width, int *out_height);

/* Render a viewport into a caller-owned RGBA8888 (premultiplied) buffer of
 * `height` rows, each `stride` bytes wide — the pixel layout of an Android
 * ARGB_8888 Bitmap, so callers can hand it straight to AndroidBitmap_lockPixels.
 *
 * scroll_x/scroll_y are CSS-pixel offsets into the page; `scale` maps CSS
 * pixels to output device pixels (e.g. the display density), so the buffer
 * shows a `width/scale` x `height/scale` CSS region rendered crisply at native
 * resolution. Pass scale = 1.0 for 1:1. Returns 0 on success. */
int nd_browser_render_rgba(nd_browser *browser, int scroll_x, int scroll_y,
                           int width, int height, double scale,
                           unsigned char *out, int stride);

/* Absolute URL of the link at page coordinates (CSS px), or NULL if none.
 * The result is newly allocated; the caller frees it with free(). */
char *nd_browser_link_at(nd_browser *browser, int x, int y);

/* The page's <title>, whitespace-collapsed, or NULL if none. Newly
 * allocated; the caller frees it with free(). */
char *nd_browser_title(nd_browser *browser);

/* The page's final URL (after redirects). Newly allocated; free() it. */
char *nd_browser_url(nd_browser *browser);

/* All <a href> links on the page, resolved to absolute URLs, de-duplicated and
 * in document order, separated by '\n'. javascript: and pure-fragment (#…)
 * links are skipped. NULL if there are none. Newly allocated; free() it. */
char *nd_browser_links(nd_browser *browser);

void nd_browser_close(nd_browser *browser);

void nd_browser_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
