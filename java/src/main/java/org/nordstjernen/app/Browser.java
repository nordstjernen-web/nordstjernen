/* Nordstjernen — official Java browser (Swing UI over the renderer process).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

package org.nordstjernen.app;

import org.nordstjernen.RemoteBrowser;

import javax.swing.*;
import java.awt.*;
import java.awt.datatransfer.DataFlavor;
import java.awt.datatransfer.StringSelection;
import java.awt.event.ActionEvent;
import java.awt.event.FocusAdapter;
import java.awt.event.FocusEvent;
import java.awt.image.BufferedImage;
import java.io.IOException;
import java.io.InputStream;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * A small, standalone Java web browser with a GTK-shell-style chrome — back,
 * forward, reload, home, a URL bar and a status bar — that drives a separate
 * {@code nordstjernen-renderer} process through {@link RemoteBrowser}. No
 * native engine is loaded into the JVM; rendering, layout and scripting all
 * run in the renderer.
 *
 * <p>It mirrors the GTK shell's interactive surface: hover link previews and
 * pointer shapes, zoom ({@code Ctrl}+{@code +}/{@code -}/{@code 0} and
 * {@code Ctrl}+wheel), find-in-page ({@code Ctrl+F}), drag text selection with
 * copy, a right-click context menu that the page can override, the page
 * favicon as the window icon, a transport-security indicator, WebGL and camera
 * trust prompts, file downloads, dropping files onto the page, external media
 * playback, scrolling of overflow scrollers and in-page scrollbars,
 * back/forward-cache history traversal, a JavaScript console and page dumps
 * ({@code F12}), several windows, and automatic recovery if a renderer process
 * dies.
 *
 * <p>Keyboard: {@code Alt+Left}/{@code Alt+Right} navigate history,
 * {@code Ctrl+L} or {@code Alt+D} focuses the URL bar, {@code Ctrl+R}/{@code F5}
 * reloads, {@code Alt+Home} goes home, {@code Ctrl+N}/{@code Ctrl+T} open a new
 * window, {@code Ctrl+W} closes one and {@code Ctrl+Q} quits; with the page
 * focused, the arrow keys, {@code PageUp}/{@code PageDown},
 * {@code Home}/{@code End} and {@code Space} scroll. The mouse back/forward
 * buttons navigate history.
 *
 * <p>Point at the renderer binary with {@code -Dnordstjernen.renderer=…} or the
 * {@code NORDSTJERNEN_RENDERER} environment variable.
 */
public final class Browser {

    private static final String HOME_URL = "about:start";
    private static final int SETTLE_MS = 900;
    private static final int LINE_SCROLL = 60;
    private static final double ZOOM_MIN = 0.25;
    private static final double ZOOM_MAX = 5.0;
    private static final double ZOOM_STEP = 1.1;
    private static final int MAX_JS_REDIRECTS = 20;
    private static final int MAX_RESTARTS = 3;
    private static final int CONSOLE_POLL_MS = 250;
    private static final String VERSION = resolveVersion();
    private static final String TITLE_SUFFIX = " (Java " + VERSION + ")";

    private final RemoteBrowser engine = new RemoteBrowser();
    private final ExecutorService io = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "ns-engine");
        t.setDaemon(true);
        return t;
    });

    private final JFrame frame = new JFrame("Nordstjernen" + TITLE_SUFFIX);
    private final JButton back = navButton("back", "◀", "Back (Alt+Left)");
    private final JButton forward = navButton("forward", "▶", "Forward (Alt+Right)");
    private final JButton reload = navButton("reload", "↻", "Reload (Ctrl+R)");
    private final JButton home = navButton("home", "⌂", "Home (Alt+Home)");
    private final JTextField address = new JTextField();
    private final JLabel securityBadge = new JLabel();
    private final RenderCanvas canvas = new RenderCanvas();
    private final JScrollBar vScroll = new JScrollBar(JScrollBar.VERTICAL);
    private final JScrollBar hScroll = new JScrollBar(JScrollBar.HORIZONTAL);
    private final JLabel status = new JLabel(" ");
    private final Image logoImage = loadLogo();

    private final JPanel findBar = new JPanel(new BorderLayout(4, 0));
    private final JTextField findField = new JTextField();
    private final JLabel findCount = new JLabel(" ");

    private final List<String> history = new ArrayList<>();
    private int historyIndex = -1;

    private int scrollX = 0;
    private int scrollY = 0;
    private double scale = 1.0;
    private boolean loading = false;
    private int jsRedirects = 0;
    private int restarts = 0;
    private boolean syncingScrollbar = false;
    private javax.swing.Timer refreshTimer;
    private boolean renderBusy = false;
    private int stableFrames = 0;

    private boolean hoverBusy = false;
    private boolean hoverPending = false;
    private int hoverPendingX, hoverPendingY;
    private String hoverLink = null;

    private boolean dragAnchored = false;
    private int pressDocX, pressDocY;
    private boolean hasSelection = false;
    private boolean suppressTextInsert = false;
    private boolean draggingScrollbar = false;
    private boolean caretActive = false;

    private final Set<String> webglAsked = new HashSet<>();
    private final Set<String> cameraAsked = new HashSet<>();

    private JDialog console;
    private JTextArea consoleOut;
    private JTextField consoleIn;
    private javax.swing.Timer consolePoll;

    /** Live windows; the process exits when the last one closes. */
    private static final Set<Browser> WINDOWS = new java.util.LinkedHashSet<>();

    private Browser(String startUrl) {
        buildUi();
        navigate(startUrl, true);
    }

    public static void main(String[] args) {
        try {
            UIManager.setLookAndFeel(UIManager.getSystemLookAndFeelClassName());
        } catch (Exception ignored) { }
        String start = args.length > 0 ? args[0] : "about:start";
        SwingUtilities.invokeLater(() -> openWindow(start));
    }

    /** Open another browser window on {@code url}, with its own renderer process. */
    private static Browser openWindow(String url) {
        Browser b = new Browser(url == null || url.isEmpty() ? HOME_URL : url);
        WINDOWS.add(b);
        return b;
    }

    private void closeWindow() {
        WINDOWS.remove(this);
        frame.dispose();
        io.submit(engine::close);
        if (WINDOWS.isEmpty()) {
            io.submit(() -> System.exit(0));
        }
    }

    /** Quit every window; each one's shutdown hook stops its renderer process. */
    private static void quitAll() {
        System.exit(0);
    }

    private void buildUi() {
        frame.setDefaultCloseOperation(JFrame.DO_NOTHING_ON_CLOSE);
        frame.addWindowListener(new java.awt.event.WindowAdapter() {
            @Override public void windowClosing(java.awt.event.WindowEvent e) {
                closeWindow();
            }
        });
        frame.setSize(1100, 820);
        if (logoImage != null) {
            frame.setIconImage(logoImage);
        }

        JToolBar bar = new JToolBar();
        bar.setFloatable(false);
        for (JButton b : new JButton[]{back, forward, reload, home}) {
            b.setFocusable(false);
            bar.add(b);
        }
        securityBadge.setBorder(BorderFactory.createEmptyBorder(0, 6, 0, 2));
        bar.add(securityBadge);
        bar.add(address);
        JButton go = navButton("go", "Go", "Go");
        bar.add(go);

        bar.addSeparator();
        JButton menu = new JButton("⋮");
        menu.setFocusable(false);
        menu.setToolTipText("Menu");
        menu.setFont(menu.getFont().deriveFont(Font.BOLD, 22f));
        menu.setMargin(new Insets(2, 10, 2, 10));
        menu.addActionListener(e -> showMenu(menu));
        bar.add(menu);

        if (logoImage != null) {
            JLabel brand = new JLabel(new ImageIcon(logoImage));
            brand.setBorder(BorderFactory.createEmptyBorder(0, 6, 0, 6));
            brand.setToolTipText("Nordstjernen — nordstjernen.org");
            brand.setCursor(Cursor.getPredefinedCursor(Cursor.HAND_CURSOR));
            brand.addMouseListener(new java.awt.event.MouseAdapter() {
                @Override public void mouseClicked(java.awt.event.MouseEvent e) {
                    navigate("https://nordstjernen.org", true);
                }
            });
            bar.add(brand);
        }

        back.addActionListener(e -> goBack());
        forward.addActionListener(e -> goForward());
        reload.addActionListener(e -> reloadPage());
        home.addActionListener(e -> navigate(HOME_URL, true));
        go.addActionListener(e -> navigate(normalize(address.getText()), true));
        address.addActionListener(e -> navigate(normalize(address.getText()), true));
        address.addFocusListener(new FocusAdapter() {
            @Override public void focusGained(FocusEvent e) { address.selectAll(); }
        });

        JPanel content = new JPanel(new BorderLayout());
        content.add(canvas, BorderLayout.CENTER);
        content.add(vScroll, BorderLayout.EAST);
        content.add(hScroll, BorderLayout.SOUTH);

        buildFindBar();
        JPanel south = new JPanel(new BorderLayout());
        south.add(findBar, BorderLayout.NORTH);
        south.add(status, BorderLayout.SOUTH);

        frame.add(bar, BorderLayout.NORTH);
        frame.add(content, BorderLayout.CENTER);
        frame.add(south, BorderLayout.SOUTH);

        vScroll.setUnitIncrement(LINE_SCROLL);
        vScroll.addAdjustmentListener(e -> {
            if (!syncingScrollbar) { scrollY = e.getValue(); scheduleRefresh(); }
        });
        hScroll.setUnitIncrement(LINE_SCROLL);
        hScroll.setVisible(false);
        hScroll.addAdjustmentListener(e -> {
            if (!syncingScrollbar) { scrollX = e.getValue(); scheduleRefresh(); }
        });

        canvas.setFocusable(true);
        canvas.setFocusTraversalKeysEnabled(false);
        canvas.addMouseWheelListener(e -> {
            if (e.isControlDown()) {
                if (e.getWheelRotation() < 0) zoomIn(); else zoomOut();
            } else {
                wheelScroll(docX(e.getX()), docY(e.getY()),
                            e.getWheelRotation() * LINE_SCROLL);
            }
        });
        canvas.addMouseListener(new java.awt.event.MouseAdapter() {
            @Override public void mousePressed(java.awt.event.MouseEvent e) {
                canvas.requestFocusInWindow();
                if (e.getButton() == 4) { goBack(); return; }
                if (e.getButton() == 5) { goForward(); return; }
                if (e.isPopupTrigger() || e.getButton() == java.awt.event.MouseEvent.BUTTON3) {
                    showContextMenu(e.getX(), e.getY()); return;
                }
                if (e.getButton() == java.awt.event.MouseEvent.BUTTON1) {
                    onCanvasPress(e.getX(), e.getY(), e);
                }
            }
            @Override public void mouseReleased(java.awt.event.MouseEvent e) {
                if (e.isPopupTrigger()) { showContextMenu(e.getX(), e.getY()); return; }
                if (e.getButton() != java.awt.event.MouseEvent.BUTTON1) {
                    return;
                }
                if (draggingScrollbar) {
                    draggingScrollbar = false;
                    io.submit(engine::scrollbarRelease);
                    return;
                }
                onCanvasRelease(e.getX(), e.getY());
            }
        });
        canvas.addMouseMotionListener(new java.awt.event.MouseMotionAdapter() {
            @Override public void mouseMoved(java.awt.event.MouseEvent e) {
                requestHover(docX(e.getX()), docY(e.getY()));
            }
            @Override public void mouseDragged(java.awt.event.MouseEvent e) {
                if (draggingScrollbar) {
                    final int dx = docX(e.getX()), dy = docY(e.getY());
                    io.submit(() -> {
                        engine.scrollbarDrag(dx, dy);
                        SwingUtilities.invokeLater(Browser.this::scheduleRefresh);
                    });
                    return;
                }
                onCanvasDrag(e.getX(), e.getY());
            }
        });
        canvas.addKeyListener(new java.awt.event.KeyAdapter() {
            @Override public void keyPressed(java.awt.event.KeyEvent e) { onCanvasKeyPressed(e); }
            @Override public void keyReleased(java.awt.event.KeyEvent e) { onCanvasKeyReleased(e); }
            @Override public void keyTyped(java.awt.event.KeyEvent e) { onCanvasKeyTyped(e); }
        });
        frame.addComponentListener(new java.awt.event.ComponentAdapter() {
            @Override public void componentResized(java.awt.event.ComponentEvent e) {
                if (!loading && currentUrl() != null) {
                    int w = Math.max(1, canvas.getWidth());
                    int h = Math.max(1, canvas.getHeight());
                    io.submit(() -> {
                        engine.setViewport(w, h);
                        SwingUtilities.invokeLater(() -> {
                            updateScrollModel();
                            scheduleRefresh();
                        });
                    });
                }
            }
        });

        installShortcuts();
        installFileDrop();

        frame.setVisible(true);
        Runtime.getRuntime().addShutdownHook(new Thread(engine::close));
    }

    /**
     * Dropping files on the page hands them to whatever is under the pointer —
     * a {@code <input type=file>} takes them, and anything else sees a
     * {@code drop} event with a populated {@code DataTransfer}.
     */
    private void installFileDrop() {
        canvas.setTransferHandler(new TransferHandler() {
            @Override public boolean canImport(TransferSupport support) {
                return support.isDrop()
                    && support.isDataFlavorSupported(DataFlavor.javaFileListFlavor);
            }

            @Override public boolean importData(TransferSupport support) {
                if (!canImport(support)) {
                    return false;
                }
                List<String> paths = new ArrayList<>();
                try {
                    Object data = support.getTransferable()
                        .getTransferData(DataFlavor.javaFileListFlavor);
                    for (Object o : (List<?>) data) {
                        paths.add(((java.io.File) o).getAbsolutePath());
                    }
                } catch (Exception ex) {
                    return false;
                }
                if (paths.isEmpty()) {
                    return false;
                }
                Point p = support.getDropLocation().getDropPoint();
                final int dx = docX(p.x), dy = docY(p.y);
                io.submit(() -> {
                    engine.dropFiles(dx, dy, paths);
                    SwingUtilities.invokeLater(Browser.this::scheduleRefresh);
                });
                return true;
            }
        });
    }

    private void installShortcuts() {
        JComponent root = frame.getRootPane();
        bindWindow(root, "alt LEFT", "back", this::goBack);
        bindWindow(root, "alt RIGHT", "forward", this::goForward);
        bindWindow(root, "F5", "reload", this::reloadPage);
        bindWindow(root, "control R", "reload2", this::reloadPage);
        bindWindow(root, "alt HOME", "home", () -> navigate(HOME_URL, true));
        bindWindow(root, "control L", "focusUrl", this::focusAddress);
        bindWindow(root, "alt D", "focusUrl2", this::focusAddress);
        bindWindow(root, "control N", "newWindow", () -> openWindow(HOME_URL));
        bindWindow(root, "control T", "newWindow2", () -> openWindow(HOME_URL));
        bindWindow(root, "control W", "close", this::closeWindow);
        bindWindow(root, "control Q", "quit", Browser::quitAll);
        bindWindow(root, "control F", "find", this::openFind);
        bindWindow(root, "control G", "findNext", () -> runFind(1));
        bindWindow(root, "control shift G", "findPrev", () -> runFind(2));
        bindWindow(root, "control EQUALS", "zoomIn", this::zoomIn);
        bindWindow(root, "control PLUS", "zoomIn2", this::zoomIn);
        bindWindow(root, "control ADD", "zoomIn3", this::zoomIn);
        bindWindow(root, "control MINUS", "zoomOut", this::zoomOut);
        bindWindow(root, "control SUBTRACT", "zoomOut2", this::zoomOut);
        bindWindow(root, "control 0", "zoomReset", this::zoomReset);
        bindWindow(root, "control P", "savePdf", () -> savePage(true));
        bindWindow(root, "F12", "console", this::toggleConsole);
        bindWindow(root, "control shift J", "console2", this::toggleConsole);
    }

    private void bindWindow(JComponent c, String ks, String name, Runnable action) {
        c.getInputMap(JComponent.WHEN_IN_FOCUSED_WINDOW)
            .put(KeyStroke.getKeyStroke(ks), name);
        c.getActionMap().put(name, asAction(action));
    }

    private static AbstractAction asAction(Runnable action) {
        return new AbstractAction() {
            @Override public void actionPerformed(ActionEvent e) { action.run(); }
        };
    }

    private void focusAddress() {
        address.requestFocusInWindow();
        address.selectAll();
    }

    private void reloadPage() {
        if (currentUrl() != null) {
            navigate(currentUrl(), false);
        }
    }

    // --- Coordinate helpers (document/CSS pixels at the current zoom) ---------

    private int docX(int canvasX) { return scrollX + (int) Math.round(canvasX / scale); }
    private int docY(int canvasY) { return scrollY + (int) Math.round(canvasY / scale); }
    private double visW() { return Math.max(1, canvas.getWidth()) / scale; }
    private double visH() { return Math.max(1, canvas.getHeight()) / scale; }

    private int pageStep() {
        return Math.max(LINE_SCROLL, (int) visH() - LINE_SCROLL);
    }

    private int maxScrollY() {
        return Math.max(0, engine.pageHeight() - (int) visH());
    }

    private int maxScrollX() {
        return Math.max(0, engine.pageWidth() - (int) visW());
    }

    private void setScrollY(int y) {
        int clamped = Math.max(0, Math.min(maxScrollY(), y));
        if (clamped == scrollY) { syncScrollbars(); return; }
        scrollY = clamped;
        syncScrollbars();
        scheduleRefresh();
    }

    private void setScrollX(int x) {
        int clamped = Math.max(0, Math.min(maxScrollX(), x));
        if (clamped == scrollX) { syncScrollbars(); return; }
        scrollX = clamped;
        syncScrollbars();
        scheduleRefresh();
    }

    private void syncScrollbars() {
        syncingScrollbar = true;
        if (vScroll.getValue() != scrollY) vScroll.setValue(scrollY);
        if (hScroll.getValue() != scrollX) hScroll.setValue(scrollX);
        syncingScrollbar = false;
    }

    private void updateScrollModel() {
        int vH = (int) visH();
        int maxV = Math.max(vH, engine.pageHeight());
        scrollY = Math.min(scrollY, Math.max(0, maxV - vH));
        int vW = (int) visW();
        int maxH = Math.max(vW, engine.pageWidth());
        scrollX = Math.min(scrollX, Math.max(0, maxH - vW));
        syncingScrollbar = true;
        vScroll.setValues(scrollY, vH, 0, maxV);
        vScroll.setBlockIncrement(pageStep());
        vScroll.setEnabled(maxV > vH);
        hScroll.setValues(scrollX, vW, 0, maxH);
        hScroll.setBlockIncrement(Math.max(LINE_SCROLL, vW - LINE_SCROLL));
        boolean needH = maxH > vW;
        hScroll.setEnabled(needH);
        hScroll.setVisible(needH);
        syncingScrollbar = false;
    }

    // --- Zoom ----------------------------------------------------------------

    private void zoomIn()  { setZoom(scale * ZOOM_STEP); }
    private void zoomOut() { setZoom(scale / ZOOM_STEP); }
    private void zoomReset() { setZoom(1.0); }

    private void setZoom(double s) {
        int permille = (int) Math.round(s * 1000.0);
        permille = Math.max((int) (ZOOM_MIN * 1000), Math.min((int) (ZOOM_MAX * 1000), permille));
        double clamped = permille / 1000.0;
        if (clamped == scale) return;
        scale = clamped;
        setStatus("Zoom " + (permille / 10) + "%");
        if (currentUrl() != null) {
            updateScrollModel();
            scheduleRefresh();
        }
    }

    // --- Navigation ----------------------------------------------------------

    private String currentUrl() {
        return historyIndex >= 0 ? history.get(historyIndex) : null;
    }

    private void navigate(String url, boolean record) {
        navigate(url, record, false, false);
    }

    private void navigate(String url, boolean record, boolean isRedirect) {
        navigate(url, record, isRedirect, false);
    }

    private void navigate(String url, boolean record, boolean isRedirect,
                          boolean fromHistory) {
        if (url == null || url.isEmpty() || loading) {
            return;
        }
        if (!isRedirect) {
            jsRedirects = 0;
            restarts = 0;
        }
        loading = true;
        caretActive = false;
        closeFind();
        canvas.setCursor(Cursor.getPredefinedCursor(Cursor.WAIT_CURSOR));
        setStatus("Loading " + url + " …");
        int vw = Math.max(320, canvas.getWidth() > 0 ? canvas.getWidth() : 1000);
        int vh = Math.max(240, canvas.getHeight() > 0 ? canvas.getHeight() : 700);
        io.submit(() -> {
            boolean ok = engineNavigate(url, vw, vh, fromHistory);
            String finalUrl = ok ? engine.url() : url;
            String title = ok ? engine.title() : "";
            String redirect = ok ? engine.pendingNav() : null;
            scrollX = 0;
            scrollY = 0;
            SwingUtilities.invokeLater(() -> {
                loading = false;
                canvas.setCursor(Cursor.getDefaultCursor());
                if (!ok) {
                    setStatus("Failed to load " + url);
                    return;
                }
                if (record) {
                    while (history.size() > historyIndex + 1) {
                        history.remove(history.size() - 1);
                    }
                    history.add(finalUrl);
                    historyIndex = history.size() - 1;
                } else if (isRedirect && historyIndex >= 0) {
                    history.set(historyIndex, finalUrl);
                }
                address.setText(finalUrl);
                frame.setTitle((title.isEmpty() ? "Untitled" : title)
                    + " — Nordstjernen" + TITLE_SUFFIX);
                updateNavButtons();
                updateSecurityBadge();
                updateScrollModel();
                setStatus(title);
                canvas.requestFocusInWindow();
                scheduleRefresh();
                requestFavicon();
                if (redirect != null && jsRedirects < MAX_JS_REDIRECTS) {
                    jsRedirects++;
                    navigate(redirect, false, true);
                }
            });
        });
    }

    /** Navigate, transparently respawning the renderer once if the IPC died. */
    private boolean engineNavigate(String url, int vw, int vh, boolean fromHistory) {
        try {
            return engine.navigate(url, vw, vh, SETTLE_MS, fromHistory, true);
        } catch (RuntimeException ex) {
            if (restarts >= MAX_RESTARTS) return false;
            restarts++;
            try {
                engine.restart();
                // The fresh process has no back/forward cache to restore from.
                return engine.navigate(url, vw, vh, SETTLE_MS, false, true);
            } catch (RuntimeException ex2) {
                return false;
            }
        }
    }

    /**
     * Show how the page was fetched: a lock for a validated HTTPS chain, a
     * warning for plain HTTP, nothing for {@code about:} and local pages.
     */
    private void updateSecurityBadge() {
        RemoteBrowser.Security sec = engine.security();
        String ip = engine.serverIp();
        String where = ip == null ? "" : " (" + ip + ")";
        switch (sec) {
            case SECURE:
                securityBadge.setText("🔒");
                securityBadge.setForeground(new Color(0x1B7F3B));
                securityBadge.setToolTipText("Encrypted connection, validated certificate" + where);
                break;
            case INSECURE:
                securityBadge.setText("⚠");
                securityBadge.setForeground(new Color(0xB03A2E));
                securityBadge.setToolTipText("Not encrypted — this page travelled in the clear" + where);
                break;
            default:
                securityBadge.setText("");
                securityBadge.setToolTipText(null);
                break;
        }
    }

    /**
     * Keep re-rendering the current viewport until the page settles. Async
     * image loads, late layout, and animations all land after the first
     * render, so (like the GTK shell) we render repeatedly until the renderer
     * reports the frame unchanged and not animating.
     */
    private void scheduleRefresh() {
        stableFrames = 0;
        if (refreshTimer == null) {
            refreshTimer = new javax.swing.Timer(120, e -> tickRefresh());
        }
        if (!refreshTimer.isRunning()) {
            refreshTimer.start();
        }
    }

    private void tickRefresh() {
        if (renderBusy) {
            return;
        }
        renderBusy = true;
        final int vw = Math.max(1, canvas.getWidth());
        final int vh = Math.max(1, canvas.getHeight());
        final int sx = scrollX;
        final int sy = scrollY;
        final double sc = scale;
        final boolean caret = caretActive;
        io.submit(() -> {
            RemoteBrowser.Frame frm;
            try {
                frm = engine.render(sx, sy, vw, vh, sc, caret);
            } catch (RuntimeException ex) {
                SwingUtilities.invokeLater(this::onRenderCrash);
                return;
            }
            final RemoteBrowser.Frame f = frm;
            SwingUtilities.invokeLater(() -> {
                renderBusy = false;
                if (f.image != null) {
                    canvas.setImage(f.image);
                }
                updateScrollModel();
                // Anchors, scrollTo() and focus scrolling land here: the page
                // asked to be somewhere other than where the shell put it.
                if (f.requestedScrollY >= 0 && f.requestedScrollY != scrollY) {
                    setScrollY(f.requestedScrollY);
                }
                if (f.unchanged && !f.animating && !f.caretBlinking) {
                    if (++stableFrames >= 3 && refreshTimer != null) {
                        refreshTimer.stop();
                    }
                } else {
                    stableFrames = 0;
                }
                if (f.webgl != null) {
                    promptWebgl(f.webgl);
                }
                if (f.camera != null) {
                    promptCamera(f.camera);
                }
                if (f.download != null) {
                    startDownload(f.download);
                }
                if (f.nav != null && jsRedirects < MAX_JS_REDIRECTS) {
                    jsRedirects++;
                    navigate(f.nav, true);
                }
            });
        });
    }

    private void onRenderCrash() {
        renderBusy = false;
        if (refreshTimer != null) refreshTimer.stop();
        if (restarts >= MAX_RESTARTS || currentUrl() == null) {
            setStatus("Renderer keeps failing — reload to retry");
            return;
        }
        restarts++;
        setStatus("Renderer restarted");
        io.submit(() -> {
            try { engine.restart(); } catch (RuntimeException ignored) { }
        });
        navigate(currentUrl(), false);
    }

    // --- Mouse: click, drag-select, release ----------------------------------

    private void onCanvasPress(int cx, int cy, java.awt.event.MouseEvent e) {
        final int dx = docX(cx), dy = docY(cy);
        pressDocX = dx;
        pressDocY = dy;
        dragAnchored = false;
        hasSelection = false;
        draggingScrollbar = false;
        final int mods = (e.isShiftDown() ? 1 : 0) | (e.isControlDown() ? 2 : 0)
                       | (e.isAltDown() ? 4 : 0) | (e.isMetaDown() ? 8 : 0);
        io.submit(() -> {
            // A scrollbar the page painted itself takes the press before the
            // document does, so dragging it scrolls instead of selecting text.
            if (engine.scrollbarPress(dx, dy)) {
                SwingUtilities.invokeLater(() -> {
                    draggingScrollbar = true;
                    scheduleRefresh();
                });
                return;
            }
            String pressed = engine.press(dx, dy, mods);
            if (pressed != null) {
                SwingUtilities.invokeLater(() -> navigate(pressed, true));
            }
        });
    }

    /**
     * Send a wheel notch where the pointer is: an overflow scroller under it
     * (a scrollable div, a textarea, an iframe) gets first refusal, and only
     * an unconsumed delta scrolls the page.
     */
    private void wheelScroll(int docX, int docY, int deltaCss) {
        if (currentUrl() == null) {
            return;
        }
        io.submit(() -> {
            boolean consumed = engine.scrollAt(docX, docY, 0, deltaCss);
            SwingUtilities.invokeLater(() -> {
                if (consumed) {
                    scheduleRefresh();
                } else {
                    setScrollY(scrollY + deltaCss);
                }
            });
        });
    }

    private void onCanvasDrag(int cx, int cy) {
        final int dx = docX(cx), dy = docY(cy);
        final boolean anchor = !dragAnchored;
        dragAnchored = true;
        hasSelection = true;
        io.submit(() -> {
            if (anchor) {
                engine.select(0, pressDocX, pressDocY);
            }
            engine.select(1, dx, dy);
            SwingUtilities.invokeLater(this::scheduleRefresh);
        });
    }

    private void onCanvasRelease(int cx, int cy) {
        final int dx = docX(cx), dy = docY(cy);
        io.submit(() -> {
            String href = engine.release();
            RemoteBrowser.Media media = href == null ? engine.mediaAt(dx, dy) : null;
            boolean editable = href == null && engine.focusedEditable();
            SwingUtilities.invokeLater(() -> {
                caretActive = editable;
                if (href != null) {
                    navigate(href, true);
                } else if (media != null) {
                    launchMedia(media);
                } else {
                    scheduleRefresh();
                }
            });
        });
    }

    private void copySelection() {
        io.submit(() -> {
            String text = engine.select(4, 0, 0);
            if (text != null && !text.isEmpty()) {
                SwingUtilities.invokeLater(() -> {
                    setClipboard(text);
                    setStatus("Copied selection");
                });
            }
        });
    }

    private void selectAll() {
        hasSelection = true;
        io.submit(() -> {
            engine.select(3, 0, 0);
            SwingUtilities.invokeLater(this::scheduleRefresh);
        });
    }

    // --- Hover ---------------------------------------------------------------

    private void requestHover(int dx, int dy) {
        if (loading || currentUrl() == null) return;
        if (hoverBusy) {
            hoverPendingX = dx;
            hoverPendingY = dy;
            hoverPending = true;
            return;
        }
        hoverBusy = true;
        io.submit(() -> {
            RemoteBrowser.Hover h;
            try {
                h = engine.hover(dx, dy);
            } catch (RuntimeException ex) {
                SwingUtilities.invokeLater(() -> hoverBusy = false);
                return;
            }
            SwingUtilities.invokeLater(() -> {
                hoverBusy = false;
                applyHover(h);
                if (h.changed) scheduleRefresh();
                if (hoverPending) {
                    hoverPending = false;
                    requestHover(hoverPendingX, hoverPendingY);
                }
            });
        });
    }

    private void applyHover(RemoteBrowser.Hover h) {
        if (h.href != null && !h.href.isEmpty()) {
            hoverLink = h.href;
            setStatus(h.href);
            canvas.setCursor(Cursor.getPredefinedCursor(Cursor.HAND_CURSOR));
        } else {
            if (hoverLink != null) {
                hoverLink = null;
                setStatus("");
            }
            canvas.setCursor(cursorFor(h.cursor));
        }
    }

    private static Cursor cursorFor(String css) {
        if (css == null) return Cursor.getDefaultCursor();
        switch (css) {
            case "pointer": return Cursor.getPredefinedCursor(Cursor.HAND_CURSOR);
            case "text":    return Cursor.getPredefinedCursor(Cursor.TEXT_CURSOR);
            case "wait":
            case "progress": return Cursor.getPredefinedCursor(Cursor.WAIT_CURSOR);
            case "crosshair": return Cursor.getPredefinedCursor(Cursor.CROSSHAIR_CURSOR);
            case "move":    return Cursor.getPredefinedCursor(Cursor.MOVE_CURSOR);
            case "ew-resize": return Cursor.getPredefinedCursor(Cursor.E_RESIZE_CURSOR);
            case "ns-resize": return Cursor.getPredefinedCursor(Cursor.N_RESIZE_CURSOR);
            case "not-allowed": return Cursor.getPredefinedCursor(Cursor.DEFAULT_CURSOR);
            default:        return Cursor.getDefaultCursor();
        }
    }

    // --- Keyboard ------------------------------------------------------------

    /**
     * Insert typed text only for keys that {@code keyPressed} did not already
     * deliver as a printable. The engine inserts a single printable character
     * on {@code keydown} (its {@code browser_edit_key} path), so for ordinary
     * keys {@code keyPressed} has done the insertion and we must not insert
     * again here — that is what produced doubled characters. We still run for
     * the IME / dead-key / compose path, where the composed character surfaces
     * only in {@code keyTyped} and no printable {@code keydown} was sent.
     */
    private void onCanvasKeyTyped(java.awt.event.KeyEvent e) {
        if (suppressTextInsert) {
            suppressTextInsert = false;
            return;
        }
        char c = e.getKeyChar();
        if (c == java.awt.event.KeyEvent.CHAR_UNDEFINED || Character.isISOControl(c)) {
            return;
        }
        if (e.isControlDown() || e.isAltDown() || e.isMetaDown()) {
            return;
        }
        final String s = String.valueOf(c);
        io.submit(() -> {
            engine.key(3, s, "", 0, e.isShiftDown() ? 1 : 0);
            RemoteBrowser.Key res = engine.key(2, s, "", 0, 0);
            SwingUtilities.invokeLater(() -> {
                if (res.nav != null) {
                    navigate(res.nav, true);
                } else {
                    scheduleRefresh();
                }
            });
        });
    }

    private void onCanvasKeyPressed(java.awt.event.KeyEvent e) {
        suppressTextInsert = false;
        if (e.isControlDown() && !e.isAltDown()) {
            switch (e.getKeyCode()) {
                case java.awt.event.KeyEvent.VK_C: copySelection(); e.consume(); return;
                case java.awt.event.KeyEvent.VK_A: selectAll(); e.consume(); return;
                default: break;
            }
        }
        if (e.getKeyCode() == java.awt.event.KeyEvent.VK_ESCAPE && findBar.isVisible()) {
            closeFind(); e.consume(); return;
        }
        String name = jsKeyName(e.getKeyCode());
        if (name == null) {
            String pk = printableForPress(e);
            if (pk != null && !e.isControlDown() && !e.isAltDown() && !e.isMetaDown()) {
                final String fpk = pk;
                final String fcode = printableCode(e);
                final int fkc = e.getKeyCode();
                final int fmods = swingMods(e);
                suppressTextInsert = true;
                io.submit(() -> {
                    RemoteBrowser.Key res = engine.key(0, fpk, fcode, fkc, fmods);
                    engine.key(3, fpk, fcode, fkc, fmods);
                    SwingUtilities.invokeLater(() -> {
                        if (res.nav != null) {
                            navigate(res.nav, true);
                        } else {
                            scheduleRefresh();
                        }
                    });
                });
            } else if (pk != null) {
                final String fpk = pk;
                final String fcode = printableCode(e);
                final int fkc = e.getKeyCode();
                final int fmods = swingMods(e);
                io.submit(() -> engine.key(0, fpk, fcode, fkc, fmods));
            }
            return;
        }
        e.consume();
        final String fname = name;
        final int fcode = jsKeycode(name);
        final int fmods = swingMods(e);
        io.submit(() -> {
            RemoteBrowser.Key res = engine.key(0, fname, fname, fcode, fmods);
            // Tab, Enter and Escape are how the focus enters and leaves a text
            // field without a click, so re-probe where the caret went.
            final boolean editable = focusMoved(fname) && engine.focusedEditable();
            SwingUtilities.invokeLater(() -> {
                if (focusMoved(fname)) {
                    caretActive = editable;
                }
                if (res.nav != null) {
                    navigate(res.nav, true);
                    return;
                }
                if (!res.prevented) {
                    switch (fname) {
                        case "ArrowDown": setScrollY(scrollY + LINE_SCROLL); return;
                        case "ArrowUp":   setScrollY(scrollY - LINE_SCROLL); return;
                        case "ArrowRight": setScrollX(scrollX + LINE_SCROLL); return;
                        case "ArrowLeft": setScrollX(scrollX - LINE_SCROLL); return;
                        case "PageDown":  setScrollY(scrollY + pageStep()); return;
                        case "PageUp":    setScrollY(scrollY - pageStep()); return;
                        case "Home":      setScrollY(0); return;
                        case "End":       setScrollY(maxScrollY()); return;
                        default: break;
                    }
                }
                scheduleRefresh();
            });
        });
    }

    private void onCanvasKeyReleased(java.awt.event.KeyEvent e) {
        String name = jsKeyName(e.getKeyCode());
        if (name == null) {
            return;
        }
        final String fname = name;
        final int fcode = jsKeycode(name);
        final int fmods = swingMods(e);
        io.submit(() -> engine.key(1, fname, fname, fcode, fmods));
    }

    private static boolean focusMoved(String keyName) {
        return "Tab".equals(keyName) || "Enter".equals(keyName)
            || "Escape".equals(keyName);
    }

    private static int swingMods(java.awt.event.KeyEvent e) {
        return (e.isShiftDown() ? 1 : 0) | (e.isControlDown() ? 2 : 0)
             | (e.isAltDown() ? 4 : 0) | (e.isMetaDown() ? 8 : 0);
    }

    private static String jsKeyName(int vk) {
        switch (vk) {
            case java.awt.event.KeyEvent.VK_BACK_SPACE: return "Backspace";
            case java.awt.event.KeyEvent.VK_DELETE:     return "Delete";
            case java.awt.event.KeyEvent.VK_ENTER:      return "Enter";
            case java.awt.event.KeyEvent.VK_TAB:        return "Tab";
            case java.awt.event.KeyEvent.VK_ESCAPE:     return "Escape";
            case java.awt.event.KeyEvent.VK_LEFT:       return "ArrowLeft";
            case java.awt.event.KeyEvent.VK_RIGHT:      return "ArrowRight";
            case java.awt.event.KeyEvent.VK_UP:         return "ArrowUp";
            case java.awt.event.KeyEvent.VK_DOWN:       return "ArrowDown";
            case java.awt.event.KeyEvent.VK_HOME:       return "Home";
            case java.awt.event.KeyEvent.VK_END:        return "End";
            case java.awt.event.KeyEvent.VK_PAGE_UP:    return "PageUp";
            case java.awt.event.KeyEvent.VK_PAGE_DOWN:  return "PageDown";
            default: return null;
        }
    }

    /**
     * The character a printable key produces at {@code keyPressed} time, used
     * to drive the engine's keydown insertion. Prefer the event's own
     * {@code keyChar} (which respects Shift and the keyboard layout — e.g. the
     * shifted number row), falling back to a keycode-derived letter/digit.
     */
    private static String printableForPress(java.awt.event.KeyEvent e) {
        char ch = e.getKeyChar();
        if (ch != java.awt.event.KeyEvent.CHAR_UNDEFINED && !Character.isISOControl(ch)) {
            return String.valueOf(ch);
        }
        return printableKey(e);
    }

    private static String printableKey(java.awt.event.KeyEvent e) {
        int vk = e.getKeyCode();
        if (vk >= java.awt.event.KeyEvent.VK_A && vk <= java.awt.event.KeyEvent.VK_Z) {
            char c = (char) ('a' + (vk - java.awt.event.KeyEvent.VK_A));
            return e.isShiftDown() ? String.valueOf(Character.toUpperCase(c)) : String.valueOf(c);
        }
        if (vk >= java.awt.event.KeyEvent.VK_0 && vk <= java.awt.event.KeyEvent.VK_9) {
            return String.valueOf((char) ('0' + (vk - java.awt.event.KeyEvent.VK_0)));
        }
        char ch = e.getKeyChar();
        if (ch != java.awt.event.KeyEvent.CHAR_UNDEFINED && !Character.isISOControl(ch)) {
            return String.valueOf(ch);
        }
        return null;
    }

    private static String printableCode(java.awt.event.KeyEvent e) {
        int vk = e.getKeyCode();
        if (vk >= java.awt.event.KeyEvent.VK_A && vk <= java.awt.event.KeyEvent.VK_Z) {
            return "Key" + (char) ('A' + (vk - java.awt.event.KeyEvent.VK_A));
        }
        if (vk >= java.awt.event.KeyEvent.VK_0 && vk <= java.awt.event.KeyEvent.VK_9) {
            return "Digit" + (char) ('0' + (vk - java.awt.event.KeyEvent.VK_0));
        }
        return "";
    }

    private static int jsKeycode(String name) {
        switch (name) {
            case "Backspace":  return 8;
            case "Tab":        return 9;
            case "Enter":      return 13;
            case "Escape":     return 27;
            case "PageUp":     return 33;
            case "PageDown":   return 34;
            case "End":        return 35;
            case "Home":       return 36;
            case "ArrowLeft":  return 37;
            case "ArrowUp":    return 38;
            case "ArrowRight": return 39;
            case "ArrowDown":  return 40;
            case "Delete":     return 46;
            default:           return 0;
        }
    }

    private void goBack() {
        if (historyIndex > 0) {
            historyIndex--;
            navigate(history.get(historyIndex), false, false, true);
        }
    }

    private void goForward() {
        if (historyIndex < history.size() - 1) {
            historyIndex++;
            navigate(history.get(historyIndex), false, false, true);
        }
    }

    private void updateNavButtons() {
        back.setEnabled(historyIndex > 0);
        forward.setEnabled(historyIndex < history.size() - 1);
    }

    private void setStatus(String s) {
        status.setText(s == null || s.isEmpty() ? " " : s);
    }

    // --- Find in page --------------------------------------------------------

    private void buildFindBar() {
        findBar.setVisible(false);
        findBar.setBorder(BorderFactory.createEmptyBorder(2, 6, 2, 6));
        findField.addActionListener(e -> runFind(1));
        findField.getDocument().addDocumentListener(new javax.swing.event.DocumentListener() {
            @Override public void insertUpdate(javax.swing.event.DocumentEvent e) { runFind(0); }
            @Override public void removeUpdate(javax.swing.event.DocumentEvent e) { runFind(0); }
            @Override public void changedUpdate(javax.swing.event.DocumentEvent e) { runFind(0); }
        });
        findField.getInputMap().put(KeyStroke.getKeyStroke("ESCAPE"), "closeFind");
        findField.getInputMap().put(KeyStroke.getKeyStroke("shift ENTER"), "findPrev");
        findField.getActionMap().put("closeFind", asAction(this::closeFind));
        findField.getActionMap().put("findPrev", asAction(() -> runFind(2)));

        JButton prev = new JButton("▲");
        JButton next = new JButton("▼");
        JButton close = new JButton("✕");
        for (JButton b : new JButton[]{prev, next, close}) b.setFocusable(false);
        prev.setToolTipText("Previous match (Shift+Enter)");
        next.setToolTipText("Next match (Enter)");
        close.setToolTipText("Close (Esc)");
        prev.addActionListener(e -> runFind(2));
        next.addActionListener(e -> runFind(1));
        close.addActionListener(e -> closeFind());

        JPanel right = new JPanel(new FlowLayout(FlowLayout.RIGHT, 4, 0));
        right.add(findCount);
        right.add(prev);
        right.add(next);
        right.add(close);

        findBar.add(new JLabel("Find: "), BorderLayout.WEST);
        findBar.add(findField, BorderLayout.CENTER);
        findBar.add(right, BorderLayout.EAST);
    }

    private void openFind() {
        if (currentUrl() == null) return;
        findBar.setVisible(true);
        findBar.getParent().revalidate();
        findField.requestFocusInWindow();
        findField.selectAll();
        if (!findField.getText().isEmpty()) runFind(0);
    }

    private void closeFind() {
        if (!findBar.isVisible()) return;
        findBar.setVisible(false);
        findCount.setText(" ");
        findBar.getParent().revalidate();
        final int fromY = scrollY;
        io.submit(() -> {
            engine.find("", false, 0, fromY);
            SwingUtilities.invokeLater(this::scheduleRefresh);
        });
        canvas.requestFocusInWindow();
    }

    private void runFind(int direction) {
        if (!findBar.isVisible()) return;
        final String q = findField.getText();
        final int fromY = scrollY;
        io.submit(() -> {
            RemoteBrowser.Find f = engine.find(q, false, direction, fromY);
            SwingUtilities.invokeLater(() -> {
                if (f.total > 0) {
                    findCount.setText(f.current + "/" + f.total);
                    setScrollY(Math.max(0, f.scrollY - 40));
                } else {
                    findCount.setText(q.isEmpty() ? " " : "No results");
                }
                scheduleRefresh();
            });
        });
    }

    // --- Context menu --------------------------------------------------------

    private void showContextMenu(int cx, int cy) {
        if (currentUrl() == null) return;
        canvas.requestFocusInWindow();
        final int dx = docX(cx), dy = docY(cy);
        io.submit(() -> {
            // Give the page its contextmenu event first; if it calls
            // preventDefault() it is drawing its own menu and ours must
            // stay out of the way.
            boolean prevented = engine.contextMenu(dx, dy);
            final String link = prevented ? null : engine.linkAt(dx, dy);
            SwingUtilities.invokeLater(() -> {
                if (prevented) {
                    scheduleRefresh();
                    return;
                }
                buildContextMenu(cx, cy, link);
            });
        });
    }

    private void buildContextMenu(int cx, int cy, String link) {
        JPopupMenu menu = new JPopupMenu();
        if (link != null && !link.isEmpty()) {
            JMenuItem open = new JMenuItem("Open Link");
            open.addActionListener(e -> navigate(link, true));
            menu.add(open);
            JMenuItem copyLink = new JMenuItem("Copy Link Address");
            copyLink.addActionListener(e -> { setClipboard(link); setStatus("Copied link address"); });
            menu.add(copyLink);
            menu.addSeparator();
        }
        if (hasSelection) {
            JMenuItem copy = new JMenuItem("Copy");
            copy.addActionListener(e -> copySelection());
            menu.add(copy);
            menu.addSeparator();
        }
        JMenuItem b = new JMenuItem("Back");
        b.setEnabled(historyIndex > 0);
        b.addActionListener(e -> goBack());
        JMenuItem f = new JMenuItem("Forward");
        f.setEnabled(historyIndex < history.size() - 1);
        f.addActionListener(e -> goForward());
        JMenuItem r = new JMenuItem("Reload");
        r.addActionListener(e -> reloadPage());
        menu.add(b);
        menu.add(f);
        menu.add(r);
        menu.addSeparator();
        JMenuItem selAll = new JMenuItem("Select All");
        selAll.addActionListener(e -> selectAll());
        menu.add(selAll);
        JMenuItem copyUrl = new JMenuItem("Copy Page Address");
        copyUrl.addActionListener(e -> {
            if (currentUrl() != null) { setClipboard(currentUrl()); setStatus("Copied page address"); }
        });
        menu.add(copyUrl);
        JMenuItem savePdf = new JMenuItem("Save Page as PDF…");
        savePdf.addActionListener(e -> savePage(true));
        menu.add(savePdf);
        JMenuItem savePng = new JMenuItem("Save Page as Image…");
        savePng.addActionListener(e -> savePage(false));
        menu.add(savePng);
        menu.show(canvas, cx, cy);
    }

    // --- Kebab (overflow) menu ----------------------------------------------

    private void showMenu(Component anchor) {
        boolean hasPage = currentUrl() != null;
        JPopupMenu menu = new JPopupMenu();

        JMenuItem newWindow = new JMenuItem("New Window");
        newWindow.setAccelerator(KeyStroke.getKeyStroke("control N"));
        newWindow.addActionListener(e -> openWindow(HOME_URL));
        menu.add(newWindow);

        JMenuItem reloadItem = new JMenuItem("Reload");
        reloadItem.setAccelerator(KeyStroke.getKeyStroke("control R"));
        reloadItem.setEnabled(hasPage);
        reloadItem.addActionListener(e -> reloadPage());
        menu.add(reloadItem);

        JMenuItem findItem = new JMenuItem("Find in Page…");
        findItem.setAccelerator(KeyStroke.getKeyStroke("control F"));
        findItem.setEnabled(hasPage);
        findItem.addActionListener(e -> openFind());
        menu.add(findItem);

        JMenu zoom = new JMenu("Zoom (" + Math.round(scale * 100) + "%)");
        JMenuItem zin = new JMenuItem("Zoom In");
        zin.setAccelerator(KeyStroke.getKeyStroke("control PLUS"));
        zin.addActionListener(e -> zoomIn());
        JMenuItem zout = new JMenuItem("Zoom Out");
        zout.setAccelerator(KeyStroke.getKeyStroke("control MINUS"));
        zout.addActionListener(e -> zoomOut());
        JMenuItem zreset = new JMenuItem("Reset Zoom");
        zreset.setAccelerator(KeyStroke.getKeyStroke("control 0"));
        zreset.addActionListener(e -> zoomReset());
        zoom.add(zin);
        zoom.add(zout);
        zoom.add(zreset);
        zoom.setEnabled(hasPage);
        menu.add(zoom);

        menu.addSeparator();

        JMenuItem savePdf = new JMenuItem("Save Page as PDF…");
        savePdf.setAccelerator(KeyStroke.getKeyStroke("control P"));
        savePdf.setEnabled(hasPage);
        savePdf.addActionListener(e -> savePage(true));
        menu.add(savePdf);

        JMenuItem savePng = new JMenuItem("Save Page as Image…");
        savePng.setEnabled(hasPage);
        savePng.addActionListener(e -> savePage(false));
        menu.add(savePng);

        JMenuItem copyUrl = new JMenuItem("Copy Page Address");
        copyUrl.setEnabled(hasPage);
        copyUrl.addActionListener(e -> {
            setClipboard(currentUrl());
            setStatus("Copied page address");
        });
        menu.add(copyUrl);

        menu.addSeparator();

        JMenuItem historyItem = new JMenuItem("History");
        historyItem.addActionListener(e -> navigate("about:history", true));
        menu.add(historyItem);

        JMenuItem settings = new JMenuItem("Settings");
        settings.addActionListener(e -> navigate("about:settings", true));
        menu.add(settings);

        menu.addSeparator();

        JMenuItem console = new JMenuItem("JavaScript Console");
        console.setAccelerator(KeyStroke.getKeyStroke("F12"));
        console.addActionListener(e -> toggleConsole());
        menu.add(console);

        JMenu inspect = new JMenu("Inspect Page");
        inspect.setEnabled(hasPage);
        for (String[] kind : new String[][]{
                {"dom", "View Page Source"},
                {"layout", "Layout Tree"},
                {"text", "Extracted Text"},
                {"network", "Network Log"},
                {"performance", "Performance"}}) {
            JMenuItem item = new JMenuItem(kind[1]);
            item.addActionListener(e -> showDump(kind[0], kind[1]));
            inspect.add(item);
        }
        menu.add(inspect);

        menu.addSeparator();

        JMenuItem site = new JMenuItem("Visit nordstjernen.org");
        site.addActionListener(e -> navigate("https://nordstjernen.org", true));
        menu.add(site);

        JMenuItem licenses = new JMenuItem("Licenses");
        licenses.addActionListener(e -> navigate("about:license", true));
        menu.add(licenses);

        JMenuItem about = new JMenuItem("About Nordstjernen");
        about.addActionListener(e -> showAbout());
        menu.add(about);

        menu.show(anchor, anchor.getWidth() - menu.getPreferredSize().width,
                  anchor.getHeight());
    }

    private void showAbout() {
        navigate("about:nordstjernen", true);
    }

    // --- Page inspection -----------------------------------------------------

    private void showDump(String kind, String label) {
        if (currentUrl() == null) return;
        setStatus("Collecting " + label.toLowerCase(java.util.Locale.ROOT) + " …");
        io.submit(() -> {
            String text;
            try {
                text = engine.dump(kind);
            } catch (RuntimeException ex) {
                text = null;
            }
            final String body = (text == null || text.isEmpty())
                ? "(nothing to show)" : text;
            SwingUtilities.invokeLater(() -> {
                setStatus("");
                JTextArea area = new JTextArea(body);
                area.setEditable(false);
                area.setFont(new Font(Font.MONOSPACED, Font.PLAIN, 12));
                area.setCaretPosition(0);
                JDialog dialog = new JDialog(frame, label + " — " + currentUrl(), false);
                dialog.add(new JScrollPane(area));
                dialog.setSize(900, 640);
                dialog.setLocationRelativeTo(frame);
                dialog.setVisible(true);
            });
        });
    }

    // --- Save / export -------------------------------------------------------

    private void savePage(boolean pdf) {
        if (currentUrl() == null) return;
        JFileChooser chooser = new JFileChooser();
        String t = (frame.getTitle() != null && !frame.getTitle().isEmpty())
            ? frame.getTitle().replaceAll("[\\\\/:*?\"<>|]", "_") : "page";
        chooser.setSelectedFile(new java.io.File(t + (pdf ? ".pdf" : ".png")));
        chooser.setDialogTitle(pdf ? "Save page as PDF" : "Save page as PNG");
        if (chooser.showSaveDialog(frame) != JFileChooser.APPROVE_OPTION) return;
        final String dest = chooser.getSelectedFile().getAbsolutePath();
        setStatus("Saving…");
        io.submit(() -> {
            boolean ok = engine.export(dest);
            SwingUtilities.invokeLater(() ->
                setStatus(ok ? "Saved " + dest : "Could not save page"));
        });
    }

    // --- Downloads -----------------------------------------------------------

    private void startDownload(String url) {
        URI uri;
        try {
            uri = URI.create(url);
        } catch (IllegalArgumentException ex) {
            return;
        }
        String suggested = uri.getPath();
        int slash = suggested == null ? -1 : suggested.lastIndexOf('/');
        suggested = (slash >= 0 && slash + 1 < suggested.length())
            ? suggested.substring(slash + 1) : "download";
        JFileChooser chooser = new JFileChooser();
        chooser.setSelectedFile(new java.io.File(suggested));
        chooser.setDialogTitle("Save download");
        if (chooser.showSaveDialog(frame) != JFileChooser.APPROVE_OPTION) return;
        final Path dest = chooser.getSelectedFile().toPath();
        setStatus("Downloading " + url + " …");
        io.submit(() -> {
            boolean ok = downloadTo(uri, dest);
            SwingUtilities.invokeLater(() ->
                setStatus(ok ? "Downloaded " + dest : "Download failed: " + url));
        });
    }

    private static boolean downloadTo(URI uri, Path dest) {
        try {
            HttpClient client = HttpClient.newBuilder()
                .followRedirects(HttpClient.Redirect.NORMAL).build();
            HttpRequest req = HttpRequest.newBuilder(uri)
                .header("User-Agent", "Nordstjernen").GET().build();
            HttpResponse<InputStream> resp =
                client.send(req, HttpResponse.BodyHandlers.ofInputStream());
            if (resp.statusCode() / 100 != 2) return false;
            try (InputStream in = resp.body()) {
                Files.copy(in, dest, StandardCopyOption.REPLACE_EXISTING);
            }
            return true;
        } catch (IOException | InterruptedException ex) {
            return false;
        }
    }

    // --- WebGL trust prompt --------------------------------------------------

    private void promptWebgl(String origin) {
        if (!webglAsked.add(origin)) return;
        int choice = JOptionPane.showOptionDialog(frame,
            "This page wants to use WebGL (hardware-accelerated 3D graphics) on\n"
            + origin + ".\n\nWebGL hands the page near-direct access to your GPU "
            + "driver — only allow it on sites you trust. Allowing keeps WebGL "
            + "enabled for this site for the rest of the session and reloads the "
            + "page.",
            "Enable WebGL for " + origin + "?",
            JOptionPane.YES_NO_OPTION, JOptionPane.WARNING_MESSAGE, null,
            new String[]{"Allow and trust this site", "Block"}, "Block");
        final boolean allow = choice == 0;
        io.submit(() -> engine.resolveWebgl(origin, allow));
        if (allow && currentUrl() != null) {
            navigate(currentUrl(), false);
        }
    }

    // --- Camera trust prompt -------------------------------------------------

    private void promptCamera(String origin) {
        if (!cameraAsked.add(origin)) return;
        int choice = JOptionPane.showOptionDialog(frame,
            "This page wants to use your camera on\n" + origin
            + ".\n\nAllowing lets the site capture video from this device for "
            + "the rest of the session.",
            "Share your camera with " + origin + "?",
            JOptionPane.YES_NO_OPTION, JOptionPane.WARNING_MESSAGE, null,
            new String[]{"Allow this site", "Block"}, "Block");
        final boolean allow = choice == 0;
        io.submit(() -> {
            engine.resolveCamera(origin, allow);
            SwingUtilities.invokeLater(this::scheduleRefresh);
        });
    }

    // --- Media ---------------------------------------------------------------

    private void launchMedia(RemoteBrowser.Media media) {
        String[][] players = {
            {"mpv", media.url},
            {"vlc", media.url},
        };
        for (String[] cmd : players) {
            try {
                new ProcessBuilder(cmd).inheritIO().start();
                setStatus("Opening " + (media.video ? "video" : "audio")
                    + " in external player…");
                return;
            } catch (IOException ignored) {
                // try the next player
            }
        }
        try {
            if (Desktop.isDesktopSupported()
                && Desktop.getDesktop().isSupported(Desktop.Action.BROWSE)) {
                Desktop.getDesktop().browse(URI.create(media.url));
                setStatus("Opened media in the system handler");
                return;
            }
        } catch (Exception ignored) {
            // fall through
        }
        setStatus("No external media player found (install mpv or vlc)");
    }

    // --- JavaScript console --------------------------------------------------

    private void toggleConsole() {
        if (console == null) {
            buildConsole();
        }
        if (console.isVisible()) {
            console.setVisible(false);
            if (consolePoll != null) consolePoll.stop();
        } else {
            console.setVisible(true);
            consoleIn.requestFocusInWindow();
            if (consolePoll == null) {
                consolePoll = new javax.swing.Timer(CONSOLE_POLL_MS, e -> pollConsole());
            }
            consolePoll.start();
        }
    }

    private void buildConsole() {
        console = new JDialog(frame, "JavaScript Console", false);
        console.setSize(640, 320);
        console.setLocationRelativeTo(frame);
        consoleOut = new JTextArea();
        consoleOut.setEditable(false);
        consoleOut.setFont(new Font(Font.MONOSPACED, Font.PLAIN, 12));
        consoleOut.setLineWrap(true);
        consoleOut.setWrapStyleWord(true);
        consoleIn = new JTextField();
        consoleIn.setToolTipText("Evaluate JavaScript and press Enter");
        consoleIn.addActionListener(e -> {
            String src = consoleIn.getText();
            if (src.isEmpty() || currentUrl() == null) return;
            appendConsole("> " + src + "\n");
            consoleIn.setText("");
            io.submit(() -> {
                String res = engine.eval(src);
                SwingUtilities.invokeLater(() -> {
                    appendConsole((res == null ? "undefined" : res) + "\n");
                    scheduleRefresh();
                });
            });
        });
        JButton clear = new JButton("Clear");
        clear.setFocusable(false);
        clear.addActionListener(e -> consoleOut.setText(""));
        JPanel header = new JPanel(new FlowLayout(FlowLayout.RIGHT, 4, 2));
        header.add(clear);
        console.add(header, BorderLayout.NORTH);
        console.add(new JScrollPane(consoleOut), BorderLayout.CENTER);
        console.add(consoleIn, BorderLayout.SOUTH);
    }

    private void pollConsole() {
        if (console == null || !console.isVisible() || currentUrl() == null) return;
        io.submit(() -> {
            String log = engine.consoleDrain();
            if (log != null) {
                SwingUtilities.invokeLater(() -> appendConsole(log));
            }
        });
    }

    private void appendConsole(String text) {
        if (consoleOut == null || text == null || text.isEmpty()) return;
        consoleOut.append(text);
        consoleOut.setCaretPosition(consoleOut.getDocument().getLength());
    }

    // --- Favicon -------------------------------------------------------------

    private void requestFavicon() {
        io.submit(() -> {
            BufferedImage icon;
            try {
                icon = engine.favicon();
            } catch (RuntimeException ex) {
                return;
            }
            if (icon != null) {
                SwingUtilities.invokeLater(() -> frame.setIconImage(icon));
            } else if (logoImage != null) {
                SwingUtilities.invokeLater(() -> frame.setIconImage(logoImage));
            }
        });
    }

    // --- Misc helpers --------------------------------------------------------

    private static void setClipboard(String text) {
        Toolkit.getDefaultToolkit().getSystemClipboard()
            .setContents(new StringSelection(text), null);
    }

    private static String resolveVersion() {
        String v = Browser.class.getPackage().getImplementationVersion();
        if (v != null && !v.isBlank()) {
            return v;
        }
        try (java.io.InputStream in = Browser.class.getResourceAsStream("version.properties")) {
            if (in != null) {
                java.util.Properties props = new java.util.Properties();
                props.load(in);
                String pv = props.getProperty("version");
                if (pv != null && !pv.isBlank()) {
                    return pv;
                }
            }
        } catch (java.io.IOException ignored) {
        }
        return "dev";
    }

    private static Image loadLogo() {
        ImageIcon ic = icon("logo");
        return ic != null ? ic.getImage() : null;
    }

    private static ImageIcon icon(String name) {
        java.net.URL u = Browser.class.getResource("icons/" + name + ".png");
        return u != null ? new ImageIcon(u) : null;
    }

    private static JButton navButton(String iconName, String fallbackText,
                                     String tooltip) {
        ImageIcon ic = icon(iconName);
        JButton b = ic != null ? new JButton(ic) : new JButton(fallbackText);
        b.setToolTipText(tooltip);
        b.setFocusable(false);
        return b;
    }

    private static String normalize(String input) {
        String s = input.trim();
        if (s.isEmpty()) return s;
        if (s.contains("://") || s.startsWith("about:") || s.startsWith("data:")) {
            return s;
        }
        if (s.contains(".") && !s.contains(" ")) {
            return "https://" + s;
        }
        return "https://duckduckgo.com/?q="
            + java.net.URLEncoder.encode(s, java.nio.charset.StandardCharsets.UTF_8);
    }

    private static final class RenderCanvas extends JComponent {
        private BufferedImage image;

        void setImage(BufferedImage img) {
            this.image = img;
            repaint();
        }

        @Override
        protected void paintComponent(Graphics g) {
            g.setColor(Color.WHITE);
            g.fillRect(0, 0, getWidth(), getHeight());
            if (image != null) {
                g.drawImage(image, 0, 0, null);
            }
        }
    }
}
