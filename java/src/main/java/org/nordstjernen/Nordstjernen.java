/* Nordstjernen — Java embedding API entry point.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

package org.nordstjernen;

/**
 * Entry point for embedding the Nordstjernen browser engine from Java.
 *
 * <p>This is a thin JNI veneer over the C embedding API ({@code libnordstjernen.h}).
 * The engine relies on a single GLib main context and is <strong>not</strong>
 * safe for concurrent use across threads; drive it from one thread and treat
 * {@link Page} instances as non-thread-safe. Always {@code close()} a page
 * (it owns native memory); {@link #shutdown()} releases process-wide engine
 * state.
 *
 * <pre>{@code
 * try (Page page = Nordstjernen.open("https://example.com", 360, 600)) {
 *     System.out.println(page.title());
 *     page.renderToFile(Path.of("example.png"));
 * }
 * }</pre>
 */
public final class Nordstjernen {

    private static boolean initialized = false;

    private Nordstjernen() {
    }

    /** Initialize the engine (idempotent). Loads the native libraries on first call. */
    public static synchronized void init() {
        if (initialized) {
            return;
        }
        NativeLoader.load();
        int rc = nativeInit();
        if (rc != 0) {
            throw new NordstjernenException("nd_browser_init failed (rc=" + rc + ")");
        }
        initialized = true;
    }

    /** Release process-wide engine state. After this, {@link #init()} re-initializes. */
    public static synchronized void shutdown() {
        if (!initialized) {
            return;
        }
        nativeShutdown();
        initialized = false;
    }

    /**
     * Fetch, parse, lay out and script {@code url}, returning a {@link Page}.
     *
     * @param url              absolute URL, {@code about:} page, {@code data:} URL or local path
     * @param viewportWidthCss layout viewport width in CSS pixels (e.g. 360 for a phone)
     * @param settleMs         milliseconds to let scripts/animations settle before layout
     * @return an open page; the caller must {@link Page#close()} it
     * @throws NordstjernenException if the page cannot be opened
     */
    public static Page open(String url, int viewportWidthCss, int settleMs) {
        if (url == null || url.isEmpty()) {
            throw new IllegalArgumentException("url must not be empty");
        }
        init();
        long handle = nativeOpen(url, viewportWidthCss, settleMs);
        if (handle == 0L) {
            throw new NordstjernenException("failed to open: " + url);
        }
        return new Page(handle);
    }

    /** Default settle time (ms) for the one-shot convenience methods. */
    public static final int DEFAULT_SETTLE_MS = 600;

    /** Open {@code url}, render the whole page to an image, and close. */
    public static java.awt.image.BufferedImage screenshot(String url, int viewportWidthCss, double scale) {
        try (Page page = open(url, viewportWidthCss, DEFAULT_SETTLE_MS)) {
            return page.renderFullPage(scale);
        }
    }

    /** Open {@code url}, render the whole page to a PNG/PDF file, and close. */
    public static void saveScreenshot(String url, int viewportWidthCss, java.nio.file.Path out) {
        try (Page page = open(url, viewportWidthCss, DEFAULT_SETTLE_MS)) {
            page.renderToFile(out);
        }
    }

    /** Open {@code url}, extract its text, and close. */
    public static String textOf(String url, int viewportWidthCss) {
        try (Page page = open(url, viewportWidthCss, DEFAULT_SETTLE_MS)) {
            return page.text();
        }
    }

    /** Open {@code url}, collect its links, and close. */
    public static java.util.List<String> linksOf(String url, int viewportWidthCss) {
        try (Page page = open(url, viewportWidthCss, DEFAULT_SETTLE_MS)) {
            return page.links();
        }
    }

    static native int nativeInit();

    static native void nativeShutdown();

    static native long nativeOpen(String url, int viewportWidth, int settleMs);

    static native int[] nativePageSize(long handle);

    static native String nativeRenderText(long handle);

    static native String nativeTitle(long handle);

    static native String nativeLinkAt(long handle, int x, int y);

    static native String nativeUrl(long handle);

    static native String nativeLinks(long handle);

    static native byte[] nativeRenderRgba(long handle, int scrollX, int scrollY,
                                          int width, int height, double scale);

    static native int nativeRenderImage(long handle, String path);

    static native void nativeClose(long handle);
}
