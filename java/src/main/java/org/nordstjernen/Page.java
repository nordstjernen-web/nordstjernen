/* Nordstjernen — an open page held by the engine.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

package org.nordstjernen;

import java.awt.image.BufferedImage;
import java.awt.image.DataBufferInt;
import java.nio.file.Path;
import java.util.List;

/**
 * A laid-out page. Backed by native memory; {@link #close()} frees it. Not
 * thread-safe.
 */
public final class Page implements AutoCloseable {

    private long handle;

    Page(long handle) {
        this.handle = handle;
    }

    private long handle() {
        if (handle == 0L) {
            throw new IllegalStateException("Page is closed");
        }
        return handle;
    }

    /** Total laid-out page size in CSS pixels. */
    public Size pageSize() {
        int[] wh = Nordstjernen.nativePageSize(handle());
        if (wh == null || wh.length < 2) {
            throw new NordstjernenException("page size unavailable");
        }
        return new Size(wh[0], wh[1]);
    }

    /** The rendered text content of the page. */
    public String text() {
        return Nordstjernen.nativeRenderText(handle());
    }

    /** The page's {@code <title>} (whitespace-collapsed), or {@code null}. */
    public String title() {
        return Nordstjernen.nativeTitle(handle());
    }

    /** The page's final URL (after redirects). */
    public String url() {
        return Nordstjernen.nativeUrl(handle());
    }

    /**
     * All {@code <a href>} links on the page as absolute URLs, de-duplicated and
     * in document order. {@code javascript:} and pure-fragment ({@code #…})
     * links are excluded.
     */
    public List<String> links() {
        String joined = Nordstjernen.nativeLinks(handle());
        if (joined == null || joined.isEmpty()) {
            return List.of();
        }
        return List.of(joined.split("\n"));
    }

    /**
     * The absolute URL of the link at page coordinates {@code (x, y)} in CSS
     * pixels, or {@code null} if there is none.
     */
    public String linkAt(int x, int y) {
        return Nordstjernen.nativeLinkAt(handle(), x, y);
    }

    /**
     * Render a viewport region into raw RGBA8888 (premultiplied) bytes,
     * {@code height} rows of {@code width*4} bytes.
     *
     * @param scrollX CSS-pixel x offset into the page
     * @param scrollY CSS-pixel y offset into the page
     * @param width   output width in device pixels
     * @param height  output height in device pixels
     * @param scale   CSS-pixel to device-pixel scale (1.0 for 1:1)
     */
    public byte[] renderRgba(int scrollX, int scrollY, int width, int height, double scale) {
        if (width <= 0 || height <= 0) {
            throw new IllegalArgumentException("width and height must be positive");
        }
        byte[] px = Nordstjernen.nativeRenderRgba(handle(), scrollX, scrollY, width, height, scale);
        if (px == null) {
            throw new NordstjernenException("render failed");
        }
        return px;
    }

    /** Convenience wrapper around {@link #renderRgba} producing a premultiplied-ARGB image. */
    public BufferedImage render(int scrollX, int scrollY, int width, int height, double scale) {
        byte[] rgba = renderRgba(scrollX, scrollY, width, height, scale);
        BufferedImage img = new BufferedImage(width, height, BufferedImage.TYPE_INT_ARGB_PRE);
        int[] data = ((DataBufferInt) img.getRaster().getDataBuffer()).getData();
        for (int i = 0, p = 0; i < data.length; i++, p += 4) {
            int r = rgba[p] & 0xFF;
            int g = rgba[p + 1] & 0xFF;
            int b = rgba[p + 2] & 0xFF;
            int a = rgba[p + 3] & 0xFF;
            data[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        return img;
    }

    /** Largest full-page render dimension, in device pixels, to bound memory. */
    public static final int MAX_FULL_PAGE_PX = 20000;

    /**
     * Render the entire page (top to bottom, full width) to a single image at
     * {@code scale}. Each dimension is clamped to {@link #MAX_FULL_PAGE_PX}
     * device pixels.
     */
    public BufferedImage renderFullPage(double scale) {
        Size size = pageSize();
        int width = clampPx(Math.round(size.width() * scale));
        int height = clampPx(Math.round(size.height() * scale));
        return render(0, 0, width, height, scale);
    }

    private static int clampPx(long px) {
        if (px < 1) {
            return 1;
        }
        return (int) Math.min(px, MAX_FULL_PAGE_PX);
    }

    /**
     * Render the whole page to a file. A {@code .pdf} path produces a PDF;
     * any other path produces a PNG.
     */
    public void renderToFile(Path path) {
        int rc = Nordstjernen.nativeRenderImage(handle(), path.toString());
        if (rc != 0) {
            throw new NordstjernenException("render to " + path + " failed (rc=" + rc + ")");
        }
    }

    @Override
    public void close() {
        if (handle != 0L) {
            Nordstjernen.nativeClose(handle);
            handle = 0L;
        }
    }
}
