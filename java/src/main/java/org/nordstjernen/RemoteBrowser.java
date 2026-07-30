/* Nordstjernen — a persistent renderer-process browser session.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

package org.nordstjernen;

import java.awt.image.BufferedImage;
import java.awt.image.DataBufferInt;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.List;

/**
 * A long-lived browser backed by a single {@code nordstjernen-renderer}
 * process. Unlike {@link RemotePage} (one page, then the renderer exits),
 * this keeps the renderer alive so an interactive shell can navigate, scroll,
 * render viewports, and follow links — the model the GTK shell uses.
 * No native engine is loaded into the JVM.
 *
 * <p>The full renderer control protocol is exposed: navigation (with
 * back/forward cache reuse), viewport and scroll, hover and cursors, key and
 * pointer input, text selection, find-in-page, nested (overflow) scrolling and
 * in-page scrollbar dragging, the focused editable's text and caret, page
 * dumps (source, DOM, layout, network, performance), JavaScript evaluation and
 * console output, favicons, media, downloads, WebGL and camera permission
 * prompts, and PDF/PNG export.
 *
 * <p>Not thread-safe; drive one instance from a single thread (or serialise).
 */
public final class RemoteBrowser implements AutoCloseable {

    /** Maximum framebuffer the renderer is asked to allocate. */
    public static final int MAX_W = 2560;
    public static final int MAX_H = 1600;

    /** Result of a render: the image plus any side-channel the page requested. */
    public static final class Frame {
        /** The rendered image, or null when {@link #unchanged} (reuse the prior one). */
        public final BufferedImage image;
        public final String nav;
        /** The renderer reported nothing changed since the last render. */
        public final boolean unchanged;
        /** The page is still animating / loading; keep rendering. */
        public final boolean animating;
        /** A caret is blinking in a focused editable; keep rendering to blink it. */
        public final boolean caretBlinking;
        /** Origin requesting WebGL (needs a trust prompt), or null. */
        public final String webgl;
        /** Origin requesting camera access (needs a trust prompt), or null. */
        public final String camera;
        /** A download the page initiated (URL), or null. */
        public final String download;
        /** A {@code window.moveTo}/{@code resizeTo}-style request, or null. */
        public final String windowAction;
        /** Laid-out page size in CSS pixels, as of this frame. */
        public final int pageWidth;
        public final int pageHeight;
        /** Scroll position the page asked for (anchors, {@code scrollTo}), or -1. */
        public final int requestedScrollY;

        Frame(BufferedImage image, String nav, boolean unchanged,
              boolean animating, boolean caretBlinking, String webgl,
              String camera, String download, String windowAction,
              int pageWidth, int pageHeight, int requestedScrollY) {
            this.image = image;
            this.nav = nav;
            this.unchanged = unchanged;
            this.animating = animating;
            this.caretBlinking = caretBlinking;
            this.webgl = webgl;
            this.camera = camera;
            this.download = download;
            this.windowAction = windowAction;
            this.pageWidth = pageWidth;
            this.pageHeight = pageHeight;
            this.requestedScrollY = requestedScrollY;
        }
    }

    /** A hover probe: whether the frame changed, the link under the point, and the CSS cursor. */
    public static final class Hover {
        public final boolean changed;
        public final String href;
        public final String cursor;

        Hover(boolean changed, String href, String cursor) {
            this.changed = changed;
            this.href = href;
            this.cursor = cursor;
        }
    }

    /** Result of a find-in-page query: match count, the current 1-based index, and where to scroll. */
    public static final class Find {
        public final int total;
        public final int current;
        public final int scrollY;

        Find(int total, int current, int scrollY) {
            this.total = total;
            this.current = current;
            this.scrollY = scrollY;
        }
    }

    /** A clickable media element resolved at a point: its URL, whether it is video, and if it streams. */
    public static final class Media {
        public final String url;
        public final boolean video;
        public final boolean stream;

        Media(String url, boolean video, boolean stream) {
            this.url = url;
            this.video = video;
            this.stream = stream;
        }
    }

    /** The focused text field's contents and selection, in UTF-8 byte offsets. */
    public static final class Editable {
        public final boolean active;
        public final String value;
        public final int caret;
        public final int anchor;

        Editable(boolean active, String value, int caret, int anchor) {
            this.active = active;
            this.value = value;
            this.caret = caret;
            this.anchor = anchor;
        }
    }

    /**
     * One step of the engine's animation/idle loop, driven when the shell is
     * not rendering — timers, transitions and async loads still progress, and
     * the page can raise the same side-channels a render does.
     */
    public static final class Tick {
        public final boolean changed;
        public final boolean animating;
        public final int pageWidth;
        public final int pageHeight;
        public final String nav;
        public final String webgl;
        public final String camera;
        public final String download;
        public final String windowAction;

        Tick(boolean changed, boolean animating, int pageWidth, int pageHeight,
             String nav, String webgl, String camera, String download,
             String windowAction) {
            this.changed = changed;
            this.animating = animating;
            this.pageWidth = pageWidth;
            this.pageHeight = pageHeight;
            this.nav = nav;
            this.webgl = webgl;
            this.camera = camera;
            this.download = download;
            this.windowAction = windowAction;
        }
    }

    /** Transport security of the current page. */
    public enum Security {
        /** Not applicable — {@code about:}, {@code data:} and local pages. */
        NONE,
        /** HTTPS with a validated certificate chain. */
        SECURE,
        /** HTTPS whose certificate chain did not validate. */
        UNTRUSTED,
        /** Plain HTTP: the connection is readable and modifiable in transit. */
        INSECURE
    }

    private final boolean privateMode;
    private RendererProcess renderer;
    private String title = "";
    private String url = "";
    private String pendingNav = "";
    private String serverIp = "";
    private Security security = Security.NONE;
    private int pageWidth;
    private int pageHeight;

    public RemoteBrowser() {
        this(false);
    }

    /**
     * @param privateMode drive a renderer in private/incognito mode, where
     *                    cookies, cache, history and storage are ephemeral and
     *                    nothing outlives the process
     */
    public RemoteBrowser(boolean privateMode) {
        this.privateMode = privateMode;
        this.renderer = new RendererProcess(MAX_W, MAX_H, privateMode);
    }

    /** Whether this session's renderer keeps no cookies, cache or history. */
    public boolean isPrivate() {
        return privateMode;
    }

    /** Navigate to {@code url}; returns false if the page failed to open. */
    public boolean navigate(String url, int viewportWidthCss,
                            int viewportHeightCss, int settleMs) {
        return navigate(url, viewportWidthCss, viewportHeightCss, settleMs,
                        false, false);
    }

    /**
     * Navigate to {@code url}.
     *
     * @param fromHistory    a back/forward traversal — lets the renderer restore
     *                       the page from its back/forward cache instead of
     *                       refetching and re-running it
     * @param userActivated  the navigation came from a real user gesture, which
     *                       the engine uses to gate popup-style behaviour
     * @return false if the page failed to open
     */
    public boolean navigate(String url, int viewportWidthCss,
                            int viewportHeightCss, int settleMs,
                            boolean fromHistory, boolean userActivated) {
        if (url == null || url.isEmpty()) {
            return false;
        }
        String body = "{\"url\":\"" + Json.escape(url) + "\",\"width\":"
            + viewportWidthCss + ",\"height\":" + viewportHeightCss
            + ",\"settle_ms\":" + settleMs
            + ",\"history\":" + (fromHistory ? 1 : 0)
            + ",\"user_activated\":" + (userActivated ? 1 : 0) + "}";
        RendererProcess.Response resp = renderer.request("POST", "/open", body);
        String json = new String(resp.body, StandardCharsets.UTF_8);
        if (!Json.flag(json, "ok")) {
            return false;
        }
        this.title = Json.string(json, "title");
        this.url = Json.string(json, "url");
        this.pendingNav = Json.string(json, "nav");
        this.serverIp = Json.string(json, "ip");
        this.security = securityOf(Json.integer(json, "security", 0));
        this.pageWidth = Json.integer(json, "page_width", viewportWidthCss);
        this.pageHeight = Json.integer(json, "page_height", viewportHeightCss);
        return true;
    }

    private static Security securityOf(int code) {
        switch (code) {
            case 1: return Security.SECURE;
            case 2: return Security.UNTRUSTED;
            case 3: return Security.INSECURE;
            default: return Security.NONE;
        }
    }

    /**
     * A client-side redirect (meta refresh or JS {@code location}) that fired
     * while the just-opened page settled, or null. Follow it to land on the
     * real destination — e.g. a {@code duckduckgo.com/l/?uddg=…} result link.
     */
    public String pendingNav() {
        return emptyToNull(pendingNav);
    }

    /** Tell the engine the viewport changed (re-lays out, fires resize). */
    public void setViewport(int widthCss, int heightCss) {
        RendererProcess.Response resp = renderer.request("POST", "/viewport",
            "{\"width\":" + widthCss + ",\"height\":" + heightCss + "}");
        String json = new String(resp.body, StandardCharsets.UTF_8);
        if (Json.flag(json, "ok")) {
            this.pageWidth = Json.integer(json, "page_width", pageWidth);
            this.pageHeight = Json.integer(json, "page_height", pageHeight);
        }
    }

    /** Render a viewport region (document coordinates via scroll) to an image. */
    public Frame render(int scrollX, int scrollY, int width, int height,
                        double scale) {
        return render(scrollX, scrollY, width, height, scale, false);
    }

    /**
     * Render a viewport region. Pass {@code caretActive} when a text field in
     * the page holds the focus, so the renderer blinks its caret.
     */
    public Frame render(int scrollX, int scrollY, int width, int height,
                        double scale, boolean caretActive) {
        if (width <= 0 || height <= 0) {
            throw new IllegalArgumentException("width and height must be positive");
        }
        String body = "{\"width\":" + width + ",\"height\":" + height
            + ",\"scroll_x\":" + scrollX + ",\"scroll_y\":" + scrollY
            + ",\"scale\":" + formatScale(scale)
            + ",\"caret\":" + (caretActive ? 1 : 0) + "}";
        RendererProcess.Response resp = renderer.request("POST", "/render", body);
        String nav = emptyToNull(resp.header("X-Nav"));
        String webgl = emptyToNull(resp.header("X-WebGL"));
        String camera = emptyToNull(resp.header("X-Camera"));
        String download = emptyToNull(resp.header("X-Download"));
        String windowAction = emptyToNull(resp.header("X-Window-Action"));
        int anim = headerInt(resp, "X-Anim", 0);
        boolean animating = (anim & 1) != 0;
        boolean caretBlinking = (anim & 2) != 0;
        boolean unchanged = "1".equals(resp.header("X-Unchanged"));
        this.pageWidth = headerInt(resp, "X-PageW", pageWidth);
        this.pageHeight = headerInt(resp, "X-PageH", pageHeight);
        int requestedScrollY = headerInt(resp, "X-ScrollY", -1);
        BufferedImage img = null;
        if (!unchanged && resp.body.length >= 4) {
            int w = headerInt(resp, "X-W", width);
            int h = headerInt(resp, "X-H", height);
            img = imageOf(resp.body, w, h, headerInt(resp, "X-Stride", w * 4));
        }
        return new Frame(img, nav, unchanged || img == null, animating,
                         caretBlinking, webgl, camera, download, windowAction,
                         pageWidth, pageHeight, requestedScrollY);
    }

    /**
     * Wrap premultiplied BGRA rows as a {@code TYPE_INT_ARGB_PRE} image. On a
     * little-endian machine the four bytes already spell the ARGB int the
     * raster wants, so the pixels move in one bulk copy.
     */
    private static BufferedImage imageOf(byte[] bgra, int w, int h, int stride) {
        if (w <= 0 || h <= 0 || stride < w * 4) {
            return null;
        }
        BufferedImage img = new BufferedImage(w, h, BufferedImage.TYPE_INT_ARGB_PRE);
        int[] data = ((DataBufferInt) img.getRaster().getDataBuffer()).getData();
        ByteBuffer buf = ByteBuffer.wrap(bgra).order(ByteOrder.LITTLE_ENDIAN);
        int rows = Math.min(h, bgra.length / stride);
        if (stride == w * 4) {
            buf.asIntBuffer().get(data, 0, rows * w);
            return img;
        }
        for (int y = 0; y < rows; y++) {
            buf.position(y * stride).asIntBuffer().get(data, y * w, w);
        }
        return img;
    }

    /** The link URL at a document-coordinate point, or null. */
    public String linkAt(int x, int y) {
        RendererProcess.Response resp = renderer.request("POST", "/link",
            "{\"x\":" + x + ",\"y\":" + y + "}");
        return Json.stringOrNull(new String(resp.body, StandardCharsets.UTF_8), "href");
    }

    /**
     * Probe the point under the pointer: moves the engine's hover state (firing
     * {@code :hover} / {@code mouseover}), and reports the link there and the CSS
     * cursor name so the shell can mirror the GTK status bar and pointer shape.
     */
    public Hover hover(int x, int y) {
        RendererProcess.Response resp = renderer.request("POST", "/hover",
            "{\"x\":" + x + ",\"y\":" + y + "}");
        String json = new String(resp.body, StandardCharsets.UTF_8);
        return new Hover(Json.flag(json, "changed"),
                         Json.stringOrNull(json, "href"),
                         Json.stringOrNull(json, "cursor"));
    }

    /**
     * Scroll whatever the page has under {@code (x, y)} by {@code (dx, dy)} CSS
     * pixels. Returns true when an overflow scroller (a scrollable
     * {@code <div>}, a {@code <textarea>}, an iframe) took the delta, in which
     * case the shell must leave its own page scroll alone.
     */
    public boolean scrollAt(int x, int y, int dx, int dy) {
        RendererProcess.Response resp = renderer.request("POST", "/scroll",
            "{\"x\":" + x + ",\"y\":" + y + ",\"dx\":" + dx + ",\"dy\":" + dy + "}");
        return Json.flag(new String(resp.body, StandardCharsets.UTF_8), "consumed");
    }

    /**
     * Start dragging a scrollbar the page painted itself (an overflow
     * scroller's). Returns true when the point actually hit one, in which case
     * the shell should route the drag to {@link #scrollbarDrag} instead of
     * selecting text.
     */
    public boolean scrollbarPress(int x, int y) {
        RendererProcess.Response resp = renderer.request("POST", "/scrollbar-press",
            "{\"x\":" + x + ",\"y\":" + y + "}");
        return Json.flag(new String(resp.body, StandardCharsets.UTF_8), "hit");
    }

    /** Continue an in-page scrollbar drag started by {@link #scrollbarPress}. */
    public boolean scrollbarDrag(int x, int y) {
        RendererProcess.Response resp = renderer.request("POST", "/scrollbar-drag",
            "{\"x\":" + x + ",\"y\":" + y + "}");
        return Json.flag(new String(resp.body, StandardCharsets.UTF_8), "hit");
    }

    /** Finish an in-page scrollbar drag. */
    public void scrollbarRelease() {
        renderer.request("POST", "/scrollbar-release", "{}");
    }

    /**
     * Deliver a {@code contextmenu} event at a document-coordinate point.
     * Returns true when the page called {@code preventDefault()} and put up its
     * own menu, in which case the shell must not show the native one.
     */
    public boolean contextMenu(int x, int y) {
        RendererProcess.Response resp = renderer.request("POST", "/contextmenu",
            "{\"x\":" + x + ",\"y\":" + y + "}");
        return Json.flag(new String(resp.body, StandardCharsets.UTF_8), "prevented");
    }

    /**
     * Drop files on the page at a document-coordinate point, as a file manager
     * drag would. Paths are absolute. Returns true when the page took them.
     */
    public boolean dropFiles(int x, int y, List<String> paths) {
        if (paths == null || paths.isEmpty()) {
            return false;
        }
        String body = "{\"x\":" + x + ",\"y\":" + y + ",\"paths\":\""
            + Json.escape(String.join("\n", paths)) + "\"}";
        RendererProcess.Response resp = renderer.request("POST", "/dropfiles", body);
        return Json.flag(new String(resp.body, StandardCharsets.UTF_8), "changed");
    }

    /**
     * Drive one step of the engine's idle loop without rendering — timers,
     * transitions, and pending network work all progress.
     */
    public Tick tick() {
        RendererProcess.Response resp = renderer.request("POST", "/tick", "{}");
        String json = new String(resp.body, StandardCharsets.UTF_8);
        this.pageWidth = Json.integer(json, "page_width", pageWidth);
        this.pageHeight = Json.integer(json, "page_height", pageHeight);
        return new Tick(Json.flag(json, "changed"), Json.flag(json, "animating"),
                        pageWidth, pageHeight,
                        Json.stringOrNull(json, "nav"),
                        Json.stringOrNull(json, "webgl"),
                        Json.stringOrNull(json, "camera"),
                        Json.stringOrNull(json, "download"),
                        Json.stringOrNull(json, "window_action"));
    }

    /**
     * Drive a text selection. {@code kind} is 0 to anchor a new selection at the
     * point, 1 to extend it, 2 to clear it, 3 to select the whole document, and
     * 4 to return the currently selected text (the copy path). Returns the
     * selected text for {@code kind == 4}, otherwise null.
     */
    public String select(int kind, int x, int y) {
        RendererProcess.Response resp = renderer.request("POST", "/select",
            "{\"kind\":" + kind + ",\"x\":" + x + ",\"y\":" + y + "}");
        return Json.stringOrNull(new String(resp.body, StandardCharsets.UTF_8), "href");
    }

    /**
     * Find {@code query} in the page. {@code direction} is 0 to (re)search from
     * {@code fromY}, 1 for the next match, 2 for the previous. Returns the match
     * count, the current match index, and the document Y to scroll the match into
     * view.
     */
    public Find find(String query, boolean caseSensitive, int direction, int fromY) {
        String body = "{\"query\":\"" + Json.escape(query == null ? "" : query)
            + "\",\"case_sensitive\":" + (caseSensitive ? 1 : 0)
            + ",\"direction\":" + direction + ",\"from_y\":" + fromY + "}";
        RendererProcess.Response resp = renderer.request("POST", "/find", body);
        String json = new String(resp.body, StandardCharsets.UTF_8);
        return new Find(Json.integer(json, "total", 0), Json.integer(json, "current", 0),
                        Json.integer(json, "scroll_y", 0));
    }

    /** Evaluate JavaScript in the page and return its result rendered as text. */
    public String eval(String src) {
        String body = "{\"src\":\"" + Json.escape(src == null ? "" : src) + "\"}";
        RendererProcess.Response resp = renderer.request("POST", "/eval", body);
        return Json.stringOrNull(new String(resp.body, StandardCharsets.UTF_8), "text");
    }

    /** Drain any buffered {@code console.*} output the page produced, or null. */
    public String consoleDrain() {
        RendererProcess.Response resp = renderer.request("POST", "/console", "{}");
        return Json.stringOrNull(new String(resp.body, StandardCharsets.UTF_8), "text");
    }

    /**
     * A developer dump of the current page. {@code kind} is one of
     * {@code "dom"}, {@code "layout"}, {@code "text"}, {@code "performance"} or
     * {@code "network"}. Returns null when the page has nothing to report.
     */
    public String dump(String kind) {
        String body = "{\"kind\":\"" + Json.escape(kind == null ? "" : kind) + "\"}";
        RendererProcess.Response resp = renderer.request("POST", "/dump", body);
        return Json.stringOrNull(new String(resp.body, StandardCharsets.UTF_8), "text");
    }

    /** Whether the page's focus is in a text field, so typing should go to it. */
    public boolean focusedEditable() {
        RendererProcess.Response resp =
            renderer.request("POST", "/focused-editable", "{}");
        return Json.flag(new String(resp.body, StandardCharsets.UTF_8), "active");
    }

    /** The focused text field's value and selection, for an IME or a caret UI. */
    public Editable focusedEditableState() {
        RendererProcess.Response resp =
            renderer.request("POST", "/focused-editable-state", "{}");
        String json = new String(resp.body, StandardCharsets.UTF_8);
        return new Editable(Json.flag(json, "active"), Json.string(json, "value"),
                            Json.integer(json, "caret", 0),
                            Json.integer(json, "anchor", 0));
    }

    /** Move the focused text field's caret/selection, in UTF-8 byte offsets. */
    public boolean setFocusedEditableSelection(int caret, int anchor) {
        RendererProcess.Response resp =
            renderer.request("POST", "/focused-editable-selection",
                "{\"caret\":" + caret + ",\"anchor\":" + anchor + "}");
        return Json.flag(new String(resp.body, StandardCharsets.UTF_8), "ok");
    }

    /** The media element (audio/video URL) at a document-coordinate point, or null. */
    public Media mediaAt(int x, int y) {
        RendererProcess.Response resp = renderer.request("POST", "/media",
            "{\"x\":" + x + ",\"y\":" + y + "}");
        String json = new String(resp.body, StandardCharsets.UTF_8);
        String url = Json.string(json, "url");
        if (url.isEmpty()) {
            return null;
        }
        return new Media(url, Json.flag(json, "is_video"), Json.flag(json, "stream"));
    }

    /** Render the whole page to {@code path} (a {@code .pdf} → PDF, else PNG); true on success. */
    public boolean export(String path) {
        String body = "{\"path\":\"" + Json.escape(path) + "\"}";
        RendererProcess.Response resp = renderer.request("POST", "/export", body);
        return Json.integer(new String(resp.body, StandardCharsets.UTF_8), "ok", -1) == 0;
    }

    /** Allow or block WebGL for {@code origin} for the rest of the session. */
    public void resolveWebgl(String origin, boolean allow) {
        renderer.request("POST", "/webgl",
            "{\"origin\":\"" + Json.escape(origin) + "\",\"allow\":" + (allow ? 1 : 0) + "}");
    }

    /** Allow or block camera access for {@code origin} for the rest of the session. */
    public void resolveCamera(String origin, boolean allow) {
        renderer.request("POST", "/camera",
            "{\"origin\":\"" + Json.escape(origin) + "\",\"allow\":" + (allow ? 1 : 0) + "}");
    }

    /** The current page's favicon as an image, or null if it has none. */
    public BufferedImage favicon() {
        RendererProcess.Response resp = renderer.request("POST", "/favicon", "{}");
        int w = headerInt(resp, "X-W", 0);
        int h = headerInt(resp, "X-H", 0);
        int stride = headerInt(resp, "X-Stride", w * 4);
        if (w <= 0 || h <= 0 || resp.body.length < stride * h) {
            return null;
        }
        return imageOf(resp.body, w, h, stride);
    }

    /** Press at a document-coordinate point (then call {@link #release}). */
    public String press(int x, int y, int mods) {
        RendererProcess.Response resp = renderer.request("POST", "/click",
            "{\"x\":" + x + ",\"y\":" + y + ",\"mods\":" + mods + "}");
        return Json.stringOrNull(new String(resp.body, StandardCharsets.UTF_8), "href");
    }

    /** Release a pending press; returns a navigation URL if the click followed a link. */
    public String release() {
        RendererProcess.Response resp = renderer.request("POST", "/release", "{}");
        return Json.stringOrNull(new String(resp.body, StandardCharsets.UTF_8), "href");
    }

    /** Result of a key event: any navigation it triggered, and whether the page consumed it. */
    public static final class Key {
        /** Navigation URL the key triggered (e.g. Enter submitting a form), or null. */
        public final String nav;
        /** The page called preventDefault() — the shell should not apply its default. */
        public final boolean prevented;

        Key(String nav, boolean prevented) {
            this.nav = nav;
            this.prevented = prevented;
        }
    }

    /**
     * Forward a key event to the focused element. {@code kind} is 0 for keydown,
     * 1 for keyup, 2 to insert {@code key} as text, 3 for keypress. {@code key}
     * and {@code code} are the {@code KeyboardEvent.key}/{@code .code} values.
     */
    public Key key(int kind, String key, String code, int keycode, int mods) {
        String body = "{\"kind\":" + kind
            + ",\"key\":\"" + Json.escape(key == null ? "" : key)
            + "\",\"code\":\"" + Json.escape(code == null ? "" : code)
            + "\",\"keycode\":" + keycode + ",\"mods\":" + mods + "}";
        RendererProcess.Response resp = renderer.request("POST", "/key", body);
        String json = new String(resp.body, StandardCharsets.UTF_8);
        return new Key(Json.stringOrNull(json, "href"), Json.flag(json, "prevented"));
    }

    public String title() { return title; }
    public String url() { return url; }
    public int pageWidth() { return pageWidth; }
    public int pageHeight() { return pageHeight; }

    /** Transport security of the current page, for a URL-bar indicator. */
    public Security security() { return security; }

    /** The server address the current page was fetched from, or null. */
    public String serverIp() { return emptyToNull(serverIp); }

    /**
     * Replace a dead renderer with a fresh process. Call after a request throws
     * (the child crashed or wedged); the caller must then re-navigate, since the
     * new process starts with no page loaded.
     */
    public void restart() {
        try { renderer.close(); } catch (RuntimeException ignored) { }
        this.renderer = new RendererProcess(MAX_W, MAX_H, privateMode);
        this.title = "";
        this.url = "";
        this.pendingNav = "";
        this.serverIp = "";
        this.security = Security.NONE;
        this.pageWidth = 0;
        this.pageHeight = 0;
    }

    @Override
    public void close() {
        renderer.close();
    }

    private static String emptyToNull(String s) {
        return (s == null || s.isEmpty()) ? null : s;
    }

    private static int headerInt(RendererProcess.Response resp, String name, int fb) {
        String v = resp.header(name);
        if (v == null) return fb;
        try { return Integer.parseInt(v.trim()); } catch (NumberFormatException e) { return fb; }
    }

    private static String formatScale(double scale) {
        if (!(scale > 0)) scale = 1.0;
        int milli = (int) (scale * 1000.0 + 0.5);
        return (milli / 1000) + "." + String.format("%03d", milli % 1000);
    }
}
