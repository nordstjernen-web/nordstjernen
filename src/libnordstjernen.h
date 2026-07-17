/* Nordstjernen — public C API for embedding the browser engine.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef LIBNORDSTJERNEN_H
#define LIBNORDSTJERNEN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ns_browser ns_browser;

int ns_browser_init(void);

ns_browser *ns_browser_open(const char *url, int viewport_width, int settle_ms);
ns_browser *ns_browser_open_viewport(const char *url, int viewport_width,
                                     double viewport_height, int settle_ms);

/* Like ns_browser_open but issues an HTTP POST with the given request body and
 * Content-Type (used by the out-of-process renderer to navigate a submitted
 * <form method=post>). */
ns_browser *ns_browser_open_post(const char *url, int viewport_width,
                                 int settle_ms, const void *body,
                                 size_t body_len, const char *content_type);
ns_browser *ns_browser_open_post_viewport(const char *url, int viewport_width,
                                          double viewport_height,
                                          int settle_ms, const void *body,
                                          size_t body_len,
                                          const char *content_type);

/* After ns_browser_click / ns_browser_key, retrieve a pending POST submission
 * (body + Content-Type) stashed by a submitted form, transferring ownership to
 * the caller (free with free()). Returns NULL if the last interaction produced
 * no POST. The pending navigation URL is returned by click/key as usual. */
char *ns_browser_take_post(ns_browser *browser, size_t *out_len,
                           char **out_content_type);

char *ns_browser_render_text(ns_browser *browser);

/* Serialize the current DOM tree / layout box tree to a newly-allocated
 * string (caller frees). Used by headless dumps driven over the renderer
 * IPC so they exercise the same engine state as the GUI. */
char *ns_browser_dump_dom(ns_browser *browser);
char *ns_browser_dump_layout(ns_browser *browser);
char *ns_browser_dump_performance(ns_browser *browser);

int ns_browser_render_image(ns_browser *browser, const char *path);

/* Total laid-out page size in CSS pixels. Returns 0 on success. */
int ns_browser_page_size(ns_browser *browser, int *out_width, int *out_height);

/* Re-lay-out the page for a new CSS-pixel viewport width (e.g. a window
 * resize). Re-evaluates @media queries, viewport units, and fluid widths and
 * reflows; the viewport height follows the engine's width*0.75 convention.
 * A no-op (returns 0) if the width is unchanged. Returns 0 on success. */
int ns_browser_set_viewport_width(ns_browser *browser, int css_width);
int ns_browser_set_viewport(ns_browser *browser, int css_width,
                            double css_height);

/* Render a viewport into a caller-owned RGBA8888 (premultiplied) buffer of
 * `height` rows, each `stride` bytes wide — the pixel layout of an Android
 * ARGB_8888 Bitmap, so callers can hand it straight to AndroidBitmap_lockPixels.
 *
 * scroll_x/scroll_y are CSS-pixel offsets into the page; `scale` maps CSS
 * pixels to output device pixels (e.g. the display density), so the buffer
 * shows a `width/scale` x `height/scale` CSS region rendered crisply at native
 * resolution. Pass scale = 1.0 for 1:1. Returns 0 on success. */
int ns_browser_render_rgba(ns_browser *browser, int scroll_x, int scroll_y,
                           int width, int height, double scale,
                           unsigned char *out, int stride);

/* Like ns_browser_render_rgba, but paints directly into the caller buffer in
 * Cairo-native ARGB32 (premultiplied, host byte order) with no intermediate
 * surface or channel swizzle — so an out-of-process renderer can hand the bytes
 * to a Cairo client that wraps them as ARGB32 without a second conversion.
 * Every pixel of the width x height region is written (opaque background), so
 * the buffer does not need pre-clearing. Returns 0 on success. */
int ns_browser_render_argb32(ns_browser *browser, int scroll_x, int scroll_y,
                             int width, int height, double scale,
                             unsigned char *out, int stride);

/* Absolute URL of the link at page coordinates (CSS px), or NULL if none.
 * The result is newly allocated; the caller frees it with free(). */
char *ns_browser_link_at(ns_browser *browser, int x, int y);

/* Report the pointer button released after a click: clears the CSS :active
 * state set by ns_browser_click and restyles if the page has :active rules.
 * Returns 1 if the page's appearance changed and the caller should
 * re-render, 0 if nothing changed, -1 if no page is open. */
int ns_browser_release(ns_browser *browser);
char *ns_browser_release_click(ns_browser *browser, int *out_changed);

/* The computed CSS cursor keyword at page coordinates (CSS px) — one of the
 * standard cursor keywords ("pointer", "text", "move", ...) — or NULL when
 * the cursor is auto/default or no page is open. The keyword set matches the
 * CSS Basic UI names that GDK resolves natively. Newly allocated; the
 * caller frees it with free(). */
char *ns_browser_cursor_at(ns_browser *browser, int x, int y);

/* Dispatch a primary click at page coordinates (CSS px): hit-test the DOM,
 * fire the element's full button sequence
 * (pointerdown→mousedown→pointerup→mouseup→click), and — unless a click
 * listener called preventDefault — follow an <a href> at the point. Pending
 * JS work (timers,
 * promises, requestAnimationFrame) is pumped and the page is relaid out if the
 * click mutated the DOM, so the next render reflects the result.
 *
 * Returns the absolute URL the page wants to navigate to (script-initiated via
 * location/assign, or a followed link), newly allocated and freed by the caller
 * with free(); NULL if the click triggered no navigation. mods is a bitmask:
 * bit0 shift, bit1 ctrl, bit2 alt, bit3 meta. */
char *ns_browser_click(ns_browser *browser, int x, int y, int mods);
char *ns_browser_press(ns_browser *browser, int x, int y, int mods);

/* Dispatch a keyboard event into the page's JS (kind 0 = keydown, 1 = keyup),
 * targeting the focused element or, if none, <body>. key/code are the DOM
 * KeyboardEvent.key / .code strings, keycode the legacy keyCode; mods is bit0
 * shift, bit1 ctrl, bit2 alt, bit3 meta. Pending JS work is pumped and the page
 * relaid out if the DOM changed. Returns the absolute URL a handler navigated
 * to (newly allocated, free with free()), or NULL. */
char *ns_browser_key(ns_browser *browser, int kind, const char *key,
                     const char *code, int keycode, int mods);
char *ns_browser_key_full(ns_browser *browser, int kind, const char *key,
                          const char *code, int keycode, int mods,
                          int *out_prevented);
int   ns_browser_focused_editable(ns_browser *browser);
char *ns_browser_focused_editable_value(ns_browser *browser,
                                        size_t *out_caret,
                                        size_t *out_anchor);
int   ns_browser_set_focused_editable_selection(ns_browser *browser,
                                                size_t caret,
                                                size_t anchor);

/* Report the pointer hovering at page coordinates (CSS px): hit-test the DOM,
 * update the element's CSS :hover state (and its ancestors'), and fire the
 * page's pointer/mouse move/over/out JS listeners. Pending JS work is pumped
 * and the page relaid out if a :hover rule restyled the page or a listener
 * mutated the DOM. Returns 1 if the page's appearance changed and the caller
 * should re-render, 0 if nothing visible changed, -1 if no page is open. */
int ns_browser_hover(ns_browser *browser, int x, int y);

/* Apply a mouse-wheel scroll of (dx, dy) CSS px at page coordinates (CSS px) to
 * the nearest CSS overflow scroll container under that point. Returns 1 if a
 * nested scroller consumed the scroll (the caller should re-render), 0 if none
 * applied so the caller should scroll the root viewport instead. */
int ns_browser_scroll_at(ns_browser *browser, int x, int y, int dx, int dy);

/* Mouse-driven scrollbar interaction for nested overflow containers, in page
 * coordinates (CSS px). _press begins a drag if (x,y) lands on a scroll
 * container's scrollbar (paging the track when clicked off the thumb) and
 * returns 1 if it grabbed one; _drag moves the active container to follow the
 * pointer and returns 1 if it scrolled; _release ends the drag. */
int  ns_browser_scrollbar_press(ns_browser *browser, int x, int y);
int  ns_browser_scrollbar_drag(ns_browser *browser, int x, int y);
void ns_browser_scrollbar_release(ns_browser *browser);

/* Deliver a native OS file drop at page coordinates (CSS px). paths is an array
 * of n_paths absolute filesystem paths. Builds a DataTransfer carrying the files
 * and dispatches the HTML dragenter -> dragover -> drop (or dragleave) sequence
 * at the hit-tested target. Returns 1 if the page changed and should re-render. */
int ns_browser_drop_files(ns_browser *browser, int x, int y,
                          const char *const *paths, int n_paths);

/* Audio playback bridge to the nordstjernen-audio helper. The renderer queues
 * commands (open/play/pause/seek/volume/stop, one per line) when page script
 * drives <audio>/<video>; the shell drains them with take_pending_audio and
 * relays them to the helper. */
char *ns_browser_take_pending_audio(ns_browser *browser);

/* DevTools console: drain accumulated console.log/warn/error/info/debug output
 * produced since the last drain, one message per line, or NULL if there is
 * none. The internal buffer is bounded; the result is newly allocated (free
 * with free()). */
char *ns_browser_console_drain(ns_browser *browser);

/* DevTools console: evaluate a JavaScript source string in the page's global
 * scope and return the stringified result (or error text), newly allocated
 * (free with free()), or NULL. Pending JS work is pumped and the page relaid
 * out if the evaluation mutated the DOM; any console output it produced is
 * captured for the next ns_browser_console_drain. */
char *ns_browser_eval(ns_browser *browser, const char *src);

/* Resolve the audio/video element at page coordinates (CSS px) to an absolute
 * media URL for handoff to an external player, or NULL if there is no media at
 * the point. *out_is_video is set to 1 for <video>, 0 for <audio>; *out_stream
 * is set to 1 when the direct media source could not be used (blob:/data:/no
 * src) and the page URL is returned instead for a resolver like yt-dlp. The
 * result is newly allocated; free it with free(). */
char *ns_browser_media_at(ns_browser *browser, int x, int y, int *out_is_video,
                          int *out_stream);

/* Dispatch a contextmenu DOM event at (x,y) in document coordinates. Returns
 * 1 if a page handler called preventDefault() (the shell should then suppress
 * its native context menu and re-render), 0 otherwise. */
int ns_browser_contextmenu(ns_browser *browser, int x, int y);

/* Find-in-page. query is the search text (NULL/empty clears the search and its
 * highlight). case_sensitive is 0/1. direction selects the match: 0 = search
 * from from_y (a document-space CSS-pixel scroll offset) downward, 1 = next
 * match, 2 = previous match; next/prev advance from the current active match
 * and wrap around. All matches of the query are highlighted by the next
 * ns_browser_render_*, with the active match emphasized. Writes the total match
 * count to *out_total, the 1-based ordinal of the active match to *out_current
 * (0 if none), and the document-space y of the active match to *out_y (so the
 * caller can scroll it into view). Returns 0 on success, -1 if no page. */
int ns_browser_find(ns_browser *browser, const char *query, int case_sensitive,
                    int direction, int from_y, int *out_total,
                    int *out_current, int *out_y);

/* Drive page text selection at page coordinates (CSS px). kind: 0 anchor (drag
 * start), 1 extend (drag move), 2 clear, 3 select-all, 4 get selected text.
 * For kind 4 returns the selected text (newly allocated, free with free()),
 * otherwise NULL. The selection highlight is painted by ns_browser_render_*. */
char *ns_browser_select(ns_browser *browser, int kind, int x, int y);

/* Advance the page's live work for up to budget_ms: fire due JS timers
 * (setTimeout/setInterval), deliver pending fetch/XHR and promise jobs, run
 * requestAnimationFrame callbacks and CSS animations, and relayout if the DOM
 * changed. Lets out-of-process renderers keep JavaScript alive past the
 * initial open() settle so deferred and animated content reaches the next
 * render. Returns 1 if the page may paint differently than the previous
 * render — the layout changed, an animated image or CSS animation advanced,
 * an animation-frame callback ran, or any main-loop source (timer, fetch
 * completion, …) was dispatched — 0 if the rendered pixels are guaranteed
 * unchanged for the same viewport/scroll/scale, -1 on error. */
int ns_browser_tick(ns_browser *browser, int budget_ms);

/* Whether the page wants to be rendered again continuously: it has active CSS
 * transitions/animations or pending requestAnimationFrame callbacks. Lets an
 * out-of-process renderer client drive a frame loop only while there is motion,
 * then stop. Does not count one-shot setTimeout/setInterval work, which reaches
 * the next on-demand render without a continuous loop. Returns 1 or 0. */
int ns_browser_animating(ns_browser *browser);

/* The page's <title>, whitespace-collapsed, or NULL if none. Newly
 * allocated; the caller frees it with free(). */
char *ns_browser_title(ns_browser *browser);

/* The page's final URL (after redirects). Newly allocated; free() it. */
char *ns_browser_url(ns_browser *browser);

/* Sets the referrer (originating page URL) applied to the next document open.
 * Consumed once by the following ns_browser_open*; pass the current page URL
 * when the navigation originates from a link/form/script so the request
 * carries a Referer and the correct Sec-Fetch-Site. */
void ns_browser_set_next_referrer(const char *url);

/* The page's connection security (ns_security in net.h): secure/invalid/plain/
 * none. When out_ip is non-NULL it receives the server IP (owned by the
 * browser, valid until it is closed), or NULL if unknown. */
int ns_browser_security(ns_browser *browser, const char **out_ip);

/* A URL the page's own scripts asked to navigate to (location.assign/replace/
 * href, meta refresh) since the last call, or NULL. Newly allocated; free()
 * it. Returning it clears the pending state. */
char *ns_browser_take_pending_nav(ns_browser *browser);

/* WebGL permission: a page called canvas.getContext('webgl') for an origin
 * with no recorded decision. take_pending_webgl returns that origin (newly
 * allocated; free() it) or NULL — the host should prompt the user and then
 * call resolve_webgl with the decision, then reload the page so getContext
 * sees the recorded answer. Decisions are remembered per origin. */
char *ns_browser_take_pending_webgl(ns_browser *browser);
void  ns_browser_resolve_webgl(ns_browser *browser, const char *origin,
                               int allow);
char *ns_browser_take_pending_camera(ns_browser *browser);
void  ns_browser_resolve_camera(ns_browser *browser, const char *origin,
                                int allow);

/* Download: the page activated a link with a download attribute. Returns a
 * newly-allocated "url\tfilename" string (free() it) or NULL. The host
 * should fetch the URL and save it under the user's Downloads directory. */
char *ns_browser_take_pending_download(ns_browser *browser);

/* All <a href> links on the page, resolved to absolute URLs, de-duplicated and
 * in document order, separated by '\n'. javascript: and pure-fragment (#…)
 * links are skipped. NULL if there are none. Newly allocated; free() it. */
char *ns_browser_links(ns_browser *browser);

/* Absolute URL of the page's favicon: the first <link rel="…icon…"> href
 * resolved against the base URL, else the origin's /favicon.ico. NULL if the
 * page has no origin. Newly allocated; free() it. */
char *ns_browser_favicon_url(ns_browser *browser);

/* Back/forward cache support. A page is eligible to be frozen and reused on
 * history navigation when it was a successful http(s) GET without a no-store
 * directive. park fires pagehide(persisted) before freezing; restore fires
 * pageshow(persisted) and re-fits the viewport. */
int  ns_browser_bfcache_eligible(ns_browser *browser);
void ns_browser_bfcache_park(ns_browser *browser);
void ns_browser_bfcache_restore(ns_browser *browser, int viewport_width,
                                double viewport_height);

void ns_browser_close(ns_browser *browser);

int ns_browser_busy(const ns_browser *browser);

/* Record a visited page (http/https only) in the browsing history shown on
 * about:history. Shells call this once per completed top-level navigation. */
void ns_history_record(const char *url, const char *title);

void ns_browser_shutdown(void);

/* Confine the current process: Linux Landlock + seccomp, Windows process
 * mitigations (a no-op on macOS). Intended for an out-of-process renderer that
 * runs the engine on untrusted content: call once, after ns_browser_init() and
 * before opening any page, passing argv[0] as self_exe (may be NULL). Honors
 * the NS_NO_SANDBOX / NS_NO_SECCOMP environment overrides. */
void ns_browser_sandbox(const char *self_exe);

#ifdef __cplusplus
}
#endif

#endif
