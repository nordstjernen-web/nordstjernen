/* Nordstjernen — a page rendered by a separate renderer process.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

package org.nordstjernen;

import java.awt.image.BufferedImage;
import java.awt.image.DataBufferInt;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.List;

/**
 * A web page driven through a separate {@code nordstjernen-renderer} process,
 * mirroring the in-process {@link Page} API without loading the native engine
 * into the JVM. The engine runs in the child process; this class is a thin
 * client of its HTTP/JSON control protocol.
 *
 * <pre>{@code
 * try (RemotePage page = RemotePage.open("https://example.com", 1000, 700, 800)) {
 *     System.out.println(page.title());
 *     ImageIO.write(page.renderFullPage(1.0), "png", new File("out.png"));
 * }
 * }</pre>
 *
 * <p>Not thread-safe.
 */
public final class RemotePage implements AutoCloseable {

    private static final int MAX_W = 2560;
    private static final int MAX_H = 4096;

    private final RendererProcess renderer;
    private final String title;
    private final String finalUrl;
    private final int pageWidth;
    private final int pageHeight;

    private RemotePage(RendererProcess renderer, String title, String finalUrl,
                       int pageWidth, int pageHeight) {
        this.renderer = renderer;
        this.title = title;
        this.finalUrl = finalUrl;
        this.pageWidth = pageWidth;
        this.pageHeight = pageHeight;
    }

    /**
     * Open {@code url} in a fresh renderer process at the given CSS viewport,
     * letting scripts settle for {@code settleMs} before reading the result.
     */
    public static RemotePage open(String url, int viewportWidthCss,
                                  int viewportHeightCss, int settleMs) {
        if (url == null || url.isEmpty()) {
            throw new IllegalArgumentException("url must not be empty");
        }
        RendererProcess r = new RendererProcess(MAX_W, MAX_H);
        boolean ok = false;
        try {
            String body = "{\"url\":\"" + Json.escape(url) + "\",\"width\":"
                + viewportWidthCss + ",\"height\":" + viewportHeightCss
                + ",\"settle_ms\":" + settleMs + "}";
            RendererProcess.Response resp = r.request("POST", "/open", body);
            String json = new String(resp.body, StandardCharsets.UTF_8);
            if (!Json.flag(json, "ok")) {
                throw new NordstjernenException("failed to open " + url);
            }
            RemotePage page = new RemotePage(r,
                Json.string(json, "title"), Json.string(json, "url"),
                Json.integer(json, "page_width", viewportWidthCss),
                Json.integer(json, "page_height", viewportHeightCss));
            ok = true;
            return page;
        } finally {
            if (!ok) {
                r.close();
            }
        }
    }

    /** The page {@code <title>}. */
    public String title() {
        return title;
    }

    /** The final URL after redirects. */
    public String url() {
        return finalUrl;
    }

    /** The laid-out page size in CSS pixels. */
    public Size pageSize() {
        return new Size(pageWidth, pageHeight);
    }

    /**
     * Render a viewport region to premultiplied RGBA8888 bytes (row-major,
     * 4 bytes per pixel, R,G,B,A).
     */
    public byte[] renderRgba(int scrollX, int scrollY, int width, int height,
                             double scale) {
        if (width <= 0 || height <= 0) {
            throw new IllegalArgumentException("width and height must be positive");
        }
        String body = "{\"width\":" + width + ",\"height\":" + height
            + ",\"scroll_x\":" + scrollX + ",\"scroll_y\":" + scrollY
            + ",\"scale\":" + formatScale(scale) + "}";
        RendererProcess.Response resp = renderer.request("POST", "/render", body);
        int w = headerInt(resp, "X-W", width);
        int h = headerInt(resp, "X-H", height);
        byte[] bgra = resp.body;
        // The renderer returns cairo ARGB32 (little-endian B,G,R,A). Convert
        // to the R,G,B,A byte order the in-process Page also exposes.
        int px = Math.min(w * h, bgra.length / 4);
        byte[] out = new byte[w * h * 4];
        for (int i = 0, p = 0; i < px; i++, p += 4) {
            out[p]     = bgra[p + 2];
            out[p + 1] = bgra[p + 1];
            out[p + 2] = bgra[p];
            out[p + 3] = bgra[p + 3];
        }
        return out;
    }

    /** Render a viewport region to a premultiplied-ARGB image. */
    public BufferedImage render(int scrollX, int scrollY, int width, int height,
                                double scale) {
        if (width <= 0 || height <= 0) {
            throw new IllegalArgumentException("width and height must be positive");
        }
        String body = "{\"width\":" + width + ",\"height\":" + height
            + ",\"scroll_x\":" + scrollX + ",\"scroll_y\":" + scrollY
            + ",\"scale\":" + formatScale(scale) + "}";
        RendererProcess.Response resp = renderer.request("POST", "/render", body);
        int w = headerInt(resp, "X-W", width);
        int h = headerInt(resp, "X-H", height);
        byte[] bgra = resp.body;
        BufferedImage img = new BufferedImage(w, h, BufferedImage.TYPE_INT_ARGB_PRE);
        int[] data = ((DataBufferInt) img.getRaster().getDataBuffer()).getData();
        int n = Math.min(data.length, bgra.length / 4);
        for (int i = 0, p = 0; i < n; i++, p += 4) {
            int b = bgra[p] & 0xFF;
            int g = bgra[p + 1] & 0xFF;
            int r = bgra[p + 2] & 0xFF;
            int a = bgra[p + 3] & 0xFF;
            data[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        return img;
    }

    /** Render the whole page (top to bottom, full width) at {@code scale}. */
    public BufferedImage renderFullPage(double scale) {
        int width = clampPx(Math.round(pageWidth * scale), MAX_W);
        int height = clampPx(Math.round(pageHeight * scale), MAX_H);
        return render(0, 0, width, height, scale);
    }

    /** The rendered (laid-out) text content of the page. */
    public String text() {
        return dump("text");
    }

    /**
     * All {@code <a href>} links on the page as absolute URLs, de-duplicated
     * and in document order. {@code javascript:} and pure-fragment links are
     * excluded.
     */
    public List<String> links() {
        String joined = dump("links");
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
        RendererProcess.Response resp = renderer.request("POST", "/link",
            "{\"x\":" + x + ",\"y\":" + y + "}");
        return Json.stringOrNull(new String(resp.body, StandardCharsets.UTF_8), "href");
    }

    /**
     * Render the whole page to a file. A {@code .pdf} path produces a PDF; any
     * other path produces a PNG.
     */
    public void renderToFile(Path path) {
        String body = "{\"path\":\"" + Json.escape(path.toString()) + "\"}";
        RendererProcess.Response resp = renderer.request("POST", "/export", body);
        if (Json.integer(new String(resp.body, StandardCharsets.UTF_8), "ok", -1) != 0) {
            throw new NordstjernenException("render to " + path + " failed");
        }
    }

    /**
     * A developer dump of the page: {@code "dom"}, {@code "layout"},
     * {@code "text"}, {@code "links"}, {@code "performance"} or
     * {@code "network"}. Returns null when there is nothing to report.
     */
    public String dump(String kind) {
        String body = "{\"kind\":\"" + Json.escape(kind == null ? "" : kind) + "\"}";
        RendererProcess.Response resp = renderer.request("POST", "/dump", body);
        return Json.stringOrNull(new String(resp.body, StandardCharsets.UTF_8), "text");
    }

    /** Evaluate JavaScript in the page and return its result rendered as text. */
    public String eval(String src) {
        String body = "{\"src\":\"" + Json.escape(src == null ? "" : src) + "\"}";
        RendererProcess.Response resp = renderer.request("POST", "/eval", body);
        return Json.stringOrNull(new String(resp.body, StandardCharsets.UTF_8), "text");
    }

    @Override
    public void close() {
        renderer.close();
    }

    private static int clampPx(long px, int max) {
        if (px < 1) {
            return 1;
        }
        return (int) Math.min(px, max);
    }

    private static int headerInt(RendererProcess.Response resp, String name,
                                 int fallback) {
        String v = resp.header(name);
        if (v == null) {
            return fallback;
        }
        try {
            return Integer.parseInt(v.trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private static String formatScale(double scale) {
        if (!(scale > 0)) {
            scale = 1.0;
        }
        int milli = (int) (scale * 1000.0 + 0.5);
        return (milli / 1000) + "." + String.format("%03d", milli % 1000);
    }
}
