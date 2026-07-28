#!/usr/bin/env python3
"""Build the Nordstjernen architecture poster: graphviz pieces + PIL compose."""

import html
import subprocess
import sys

DPI = 110
FONT = "DejaVu Sans"


def node(nid, name, desc, deps=None, fill="white"):
    rows = ['<tr><td align="left"><b>%s</b></td></tr>' % html.escape(name)]
    for line in desc.split("\n"):
        rows.append(
            '<tr><td align="left"><font point-size="10">%s</font></td></tr>'
            % html.escape(line))
    if deps:
        rows.append(
            '<tr><td align="left"><font point-size="9" color="#7A5C00">'
            '<i>&#8658; %s</i></font></td></tr>' % html.escape(deps))
    label = ('<<table border="0" cellborder="0" cellspacing="0" '
             'cellpadding="1">%s</table>>' % "".join(rows))
    return '%s [label=%s fillcolor="%s"];' % (nid, label, fill)


EDGE_STYLE = {
    "spawn":  'color="#C0392B", style=dashed, penwidth=1.6, fontcolor="#C0392B"',
    "ipc":    'color="#2C5FA8", penwidth=2.6, fontcolor="#2C5FA8"',
    "ipcthin": 'color="#2C5FA8", penwidth=1.3, fontcolor="#2C5FA8"',
    "px":     'color="#1E7A3C", penwidth=2.6, fontcolor="#1E7A3C"',
    "pxthin": 'color="#1E7A3C", style=dashed, penwidth=1.4, fontcolor="#1E7A3C"',
    "media":  'color="#B8860B", penwidth=1.8, fontcolor="#8B6508"',
    "flow":   'color="#444444", fontcolor="#333333"',
    "pipe":   'color="#1A1A1A", penwidth=2.2, fontcolor="#1A1A1A"',
    "thin":   'color="#888888", penwidth=0.9, fontcolor="#666666", arrowsize=0.6',
    "mode":   'color="#666666", style=dashed, fontcolor="#555555"',
}


def E(a, b, label=None, cls="flow", **kw):
    parts = [EDGE_STYLE[cls]]
    if label:
        parts.append('label=<<font point-size="10">%s</font>>'
                     % label.replace("\n", "<br/>"))
    for k, v in kw.items():
        parts.append('%s=%s' % (k, v))
    return '%s -> %s [%s];' % (a, b, ", ".join(parts))


def header(ranksep=0.55, nodesep=0.3):
    return [
        'graph [rankdir=TB, compound=true, splines=spline, '
        'ranksep=%s, nodesep=%s, fontname="%s", bgcolor=transparent, '
        'margin=0.08];' % (ranksep, nodesep, FONT),
        'node [shape=box, style="rounded,filled", fillcolor=white, '
        'color=gray35, fontname="%s", fontsize=12, margin="0.14,0.07"];' % FONT,
        'edge [fontname="%s", fontsize=10, color=gray25, penwidth=1.1, '
        'arrowsize=0.8];' % FONT,
    ]


CL_LABEL = ('<<font point-size="19"><b>%s</b></font><br/>'
            '<font point-size="12">%s</font>>')
SUB_LABEL = '<<font point-size="14"><b>%s</b></font>>'


def cluster(name, title, sub, fill, color, penwidth=2, just="l"):
    return ('subgraph cluster_%s {label=%s; style="rounded,filled"; '
            'fillcolor="%s"; color="%s"; penwidth=%s; labeljust=%s;'
            % (name, CL_LABEL % (title, sub), fill, color, penwidth, just))


def subcl(name, title, color="#7FA97F", fill="#F9FCF9"):
    return ('subgraph cluster_%s {label=%s; style="rounded,filled"; '
            'fillcolor="%s"; color="%s"; penwidth=1.4; labeljust=l;'
            % (name, SUB_LABEL % title, fill, color))


PIECES = {}

# ------------------------------------------------------------- watchdog
L = ['digraph w {'] + header()
L.append(cluster("watchdog", "PROCESS: nordstjernen (watchdog supervisor)",
                 "the process you launch; owns no UI", "#FBEAE5", "#A6543F"))
L.append(node("wd_sup", "watchdog.c",
              "ns_watchdog_run_supervisor: spawns the GTK shell,\n"
              "restarts it on crash or hang; crash-loop cap 5 restarts/60 s"))
L.append('}')
L.append('}')
PIECES["watchdog"] = "\n".join(L)

# ------------------------------------------------------------- GTK shell
L = ['digraph s {'] + header(ranksep=0.5)
L.append(cluster("shell", "PROCESS: nordstjernen (GTK 4 shell)",
                 "thin, engine-free UI &#8212; never parses HTML, "
                 "runs CSS or JS", "#E8F0FA", "#3E6FA8"))
L.append(node("appmain", "gtk/appmain.c",
              "entry point: process bootstrap, mode dispatch\n"
              "(supervisor / GUI / headless / single-process)",
              "GTK 4, GLib"))
L.append(node("wd_hb", "watchdog.c (heartbeat thread)",
              "turns a wedged GTK main loop into a\n"
              "non-zero exit the supervisor can recover"))
L.append(node("threaddump", "threaddump.c",
              "SIGQUIT per-process thread dump\n"
              "(linked into shell + renderer)"))
L.append(node("procwindow", "gtk/procwindow.c",
              "tabbed browser window: tabs, address bar,\n"
              "menus, downloads, settings UI", "GTK 4"))
L.append(node("headless", "headless.c",
              "headless engine driver: --headless --dump --eval\n"
              "--wpt --act; drives the engine in-process,\n"
              "no shell and no IPC at all"))
L.append(node("procview", "gtk/procview.c",
              "per-tab thin client: blits the shared framebuffer,\n"
              "forwards input, spawns renderer + media helpers,\n"
              "handles X-Nav / X-Audio / X-Camera / X-Download", "GTK 4"))
L.append(node("rproc_inproc", "rproc_inproc.c",
              "single-process mode (--single-process):\n"
              "serves every tab's renderer session\n"
              "in-process over the same protocol"))
L.append(node("rproc_http", "rproc_http.c",
              "IPC client wrappers: one function per\n"
              "protocol message (/open, /render, /click, ...)"))
L.append(node("ipc_http", "ipc_http.c",
              "minimal HTTP/1.1 + JSON framing,\nlinked into BOTH shell "
              "and renderer", fill="#FFF8DC"))
L.append(E("appmain", "procwindow", "GUI startup"))
L.append(E("procwindow", "procview", "one NsProcView per tab"))
L.append(E("procview", "rproc_http", "per-tab IPC client"))
L.append(E("procview", "rproc_inproc", "--single-process", "mode"))
L.append(E("appmain", "headless", "--headless / --dump / --wpt", "mode"))
L.append(E("rproc_http", "ipc_http", None, "ipcthin"))
L.append('{rank=same; appmain; wd_hb; threaddump;}')
L.append('{rank=same; procwindow; headless;}')
L.append('{rank=same; rproc_inproc; rproc_http;}')
L.append('appmain -> wd_hb [style=invis]; wd_hb -> threaddump [style=invis];')
L.append('}')
L.append('}')
PIECES["shell"] = "\n".join(L)

# ------------------------------------------------------------- renderer
L = ['digraph r {'] + header(ranksep=0.5, nodesep=0.27)
L.append(cluster("renderer", "PROCESS: nordstjernen-renderer &#215; one per tab",
                 "the whole engine, sandboxed: Landlock + seccomp-bpf (Linux), "
                 "Seatbelt (macOS), mitigation policies (Windows) &#8212; "
                 "no JIT anywhere", "#EDF6ED", "#4E8A4E", 2.5, "r"))

L.append(subcl("ripc", "IPC server"))
L.append(node("renderer_http", "renderer_http.c",
              "renderer main(): request loop on fd 3,\n"
              "seals the sandbox before the first page"))
L.append(node("renderer_serve", "renderer_serve.c",
              "protocol dispatch: /open /render /click /key /hover\n"
              "/scroll /select /find /viewport /eval /media /export ...\n"
              "drains side-channel queues into X-* reply headers"))
L.append('}')

L.append(subcl("api", "Engine core (libnordstjernen \xb7 also a shared "
               "library for embedders)"))
L.append(node("libns", "libnordstjernen.c",
              "public C embedding API (ns_browser_*): open, input,\n"
              "ns_browser_tick (animations, rAF, timers),\n"
              "ns_browser_render_argb32 (paint into any buffer)"))
L.append(node("engine", "engine.c",
              "synchronous fetch→cascade→layout→capture\n"
              "pipeline (headless / export path)"))
L.append(node("render", "render.c",
              "shared style+layout core: viewport, cascade run,\n"
              "box-tree build, container-query second pass,\n"
              "publishes styles + geometry to JS"))
L.append(node("embed_shim", "embed_shim.c",
              "app-level hooks stubbed for embedding"))
L.append('}')

L.append(subcl("doc", "Document pipeline"))
L.append(node("html", "html.c",
              "charset decode: BOM, declared charset, UTF-8\n"
              "validation, uchardet detection, cp1252 fallback",
              "uchardet"))
L.append(node("html_lexbor", "html_lexbor.c",
              "drives the lexbor tokeniser + tree builder, converts\n"
              "the lexbor tree to the native DOM; quirks mode,\n"
              "declarative shadow DOM, media-metadata extraction"))
L.append(node("xml", "xml.c",
              "minimal namespaced XML / XHTML parser to the DOM"))
L.append(node("dom", "dom.c",
              "the DOM: hand-rolled ns_node tree with per-document\n"
              "id/class/tag hash indexes + attribute Bloom filter"))
L.append('}')

L.append(subcl("style", "Style \xb7 Layout \xb7 Paint (single main thread, "
               "software rendering)"))
L.append(node("css", "css.c  (~17 k loc)",
              "CSS parser, selector engine and cascade: 190+ longhands,\n"
              "layers, :has()/:is(), @property, UA stylesheet"))
L.append(node("css_media", "css_media.c",
              "Media Queries Level 4 parser + evaluator"))
L.append(node("layout", "layout.c  (~11 k loc)",
              "box-tree layout: block, inline (Pango runs), flex,\n"
              "grid, table, multicol, margin collapsing", "Pango"))
L.append(node("mathml", "mathml.c", "presentation MathML layout + paint"))
L.append(node("paint", "paint.c",
              "Cairo painter: stacking contexts, opacity/blend/mask,\n"
              "2D/3D transforms, clip-paths, culling", "Cairo, Pango"))
L.append(node("font", "font.c",
              "@font-face loader: WOFF/WOFF2 to SFNT via FreeType,\n"
              "registered with Fontconfig for Pango",
              "FreeType, Fontconfig"))
L.append(node("anim", "anim.c", "CSS transitions + @keyframes engine"))
L.append(node("image", "image.c",
              "image decode dispatch + cache; content-sniffed\n"
              "decoder order, animated formats",
              "gdk-pixbuf (fallback), in-engine SVG"))
L.append(node("image_dec", "image_wuffs.c \xb7 image_webp.c \xb7 "
              "image_avif.c \xb7 image_ico.c",
              "format decoders: memory-safe PNG/APNG/GIF/BMP/JPEG\n"
              "(Wuffs), WebP incl. animated (libwebp), AVIF (libavif), ICO/CUR",
              "libwebp, libavif"))
L.append(node("texture", "texture.c", "decoded-image texture abstraction"))
L.append(node("selection", "selection.c",
              "text selection on the rendered page"))
L.append(node("forms", "forms.c",
              "form validation, serialization, submission"))
L.append(node("spellcheck", "spellcheck.c",
              "optional spell checking of editable text",
              "Enchant (optional)"))
L.append(node("pdf", "pdf.c", "PDF rendered to an inline HTML page",
              "poppler-glib (optional)"))
L.append('}')

L.append(subcl("js", "JavaScript engine + bindings"))
L.append(node("js", "js.c  (~47 k loc, largest file)",
              "the whole Web-platform binding surface over QuickJS:\n"
              "DOM bindings (one JSClassID, per-kind prototypes), events,\n"
              "fetch/XHR + CORS, Web Workers (GThread each), storage,\n"
              "observers, 60 s wall-clock interrupt monitor"))
L.append(node("js_realm", "js_realm.c", "native ShadowRealm"))
L.append(node("js_canvas", "js_canvas.c",
              "Canvas 2D, Path2D, ImageBitmap, DOMMatrix", "Cairo, Pango"))
L.append(node("js_intl", "js_intl.c", "ECMA-402 Intl, ICU-free"))
L.append(node("js_date", "js_date.c", "Temporal API, ICU-free"))
L.append(node("js_perf", "js_perf.c", "performance.* + PerformanceObserver"))
L.append(node("bytecode_cache", "bytecode_cache.c",
              "compiled-bytecode cache keyed by SHA-256;\n"
              "16 MB memory LRU + sharded disk tier"))
L.append(node("ext", "ext.c", "WebExtensions loader + content-script host"))
L.append('}')

L.append(subcl("webapi", "Web APIs (bound into JS by js.c)"))
L.append(node("webcrypto", "webcrypto.c",
              "crypto.subtle: AES, RSA, ECDSA/ECDH, Ed25519,\n"
              "PBKDF2, HKDF over EVP", "OpenSSL libcrypto"))
L.append(node("idb", "idb.c", "IndexedDB native backend", "SQLite"))
L.append(node("ws", "ws.c", "WebSocket client", "libcurl (native WS)"))
L.append(node("eventsource", "eventsource.c",
              "EventSource / Server-Sent Events", "libcurl"))
L.append(node("wasm", "wasm.c \xb7 wamr/ns_wamr.c",
              "WebAssembly JS API over the WAMR\n"
              "classic interpreter (no JIT/AOT)"))
L.append(node("webgl", "webgl.c",
              "WebGL 1/2 mapped onto OpenGL ES; renders to FBO,\n"
              "read back + composited into the Cairo scene", "libepoxy"))
L.append(node("glctx", "glctx.c",
              "toolkit-independent offscreen GLES context\n"
              "(surfaceless EGL / WGL / CGL)", "libepoxy, EGL"))
L.append(node("webgpu", "webgpu.c",
              "EXPERIMENTAL navigator.gpu; runtime-gated\n"
              "behind --enable-webgpu", "wgpu-native (optional)"))
L.append(node("camera", "camera.c",
              "getUserMedia webcam capture + per-site permission", "V4L2"))
L.append(node("mic", "mic.c",
              "microphone capture for getUserMedia\n+ Web Audio", "SDL2"))
L.append('}')

L.append(subcl("net", "Networking"))
L.append(node("net", "net.c  (~6 k loc)",
              "async fetcher: HTTP/2 by default, redirects, HSTS,\n"
              "HTTPS-first, Sec-Fetch-*/client hints, referer,\n"
              "per-site partitioned cookies; WHATWG URL via lexbor",
              "libcurl, libpsl, zlib"))
L.append(node("net_http2", "net_http2.c",
              "alternate transport backend (http_backend=nghttp2):\n"
              "in-tree h2 client, per-origin multiplexing; HTTP/3\n"
              "sub-feature via ngtcp2 + nghttp3 + gnutls",
              "libnghttp2, OpenSSL (optional)"))
L.append(node("netutil", "netutil.c",
              "curl-free helpers: Accept-Language,\nsearch URLs, proxy masking"))
L.append(node("cache", "cache.c",
              "SQLite-indexed HTTP cache, on-disk bodies,\n"
              "ETag / Last-Modified revalidation", "SQLite"))
L.append(node("csp", "csp.c",
              "Content-Security-Policy: 11 directives,\nnonces, inline hashes"))
L.append(node("safebrowsing", "safebrowsing.c",
              "LOCAL SHA-256 host blocklist + interstitial\n"
              "(no network callout, no telemetry)"))
L.append('}')

L.append(subcl("media_r", "Media (renderer side)"))
L.append(node("video", "video.c",
              "inline <video> playback + poster cache;\n"
              "frames advanced off the animation tick;\n"
              "queues commands for the audio/video helpers"))
L.append(node("video_decode", "video_decode.c",
              "video decode: MPEG-1 (pl_mpeg) in-tree;\n"
              "VP8/VP9 WebM via libav + swscale",
              "FFmpeg libav* (Linux/Win required)"))
L.append('}')

L.append(subcl("sec", "Security", color="#B03A2E", fill="#FBF1EF"))
L.append(node("security", "security.c",
              "Landlock fs ruleset + seccomp-bpf filter (~230 syscalls\n"
              "allowed; execve/ptrace/mount/setuid/bpf denied);\n"
              "also compiled into the audio + video helpers",
              "libseccomp"))
L.append('}')

L.append(subcl("svc", "Browser services"))
L.append(node("config", "config.c", "flat key/value config"))
L.append(node("history", "history.c", "browsing history", "SQLite"))
L.append(node("bookmarks", "bookmarks.c", "bookmarks storage"))
L.append(node("i18n", "i18n.c",
              "UI translation via in-tree catalogues\n"
              "(data/i18n/*.lang), no gettext"))
L.append(node("debuglog", "debuglog.c",
              "debug event log shared with JS console"))
L.append(node("datetime", "datetime.c", "civil-date math, HTML date parsing"))
L.append('}')

L.append('}')  # cluster_renderer

# vendored libraries live in the same piece so graphviz routes their edges
L.append(cluster("vendored", "Vendored / in-tree libraries",
                 "forked or vendored, built as part of the tree &#8212; "
                 "no system fallback", "#F1F1F1", "#666666"))
L.append(node("lexbor", "src/lexbor/",
              "HTML tokeniser + tree builder,\nWHATWG URL module "
              "(forked, modified freely)"))
L.append(node("quickjs", "src/quickjs/",
              "QuickJS-ng fork: bytecode interpreter,\n"
              "NO JIT; browser hooks in-tree"))
L.append(node("wamr", "src/wamr/",
              "WebAssembly Micro Runtime,\nclassic interpreter only"))
L.append(node("wuffs", "subprojects/wuffs/",
              "memory-safe transpiled-to-C\nimage decoders"))
L.append(node("plmpeg", "subprojects/plmpeg/",
              "pl_mpeg: MPEG-1 video + MP2 audio\n(MIT, single file)"))
L.append(node("minimp3", "src/audio/minimp3.h",
              "MP3 decoder (CC0, single file)"))
L.append(node("polyfills", "data/js/polyfills.js",
              "~331 KB embedded at build time; spec object\n"
              "models over thin native backends"))
L.append('}')

# renderer-internal edges
L.append(E("renderer_http", "renderer_serve", "dispatch", "thin"))
L.append(E("renderer_http", "security",
           "seals Landlock + seccomp\nbefore the first page", "flow"))
L.append(E("renderer_serve", "libns",
           "session ops: open, input,\neval, find, export"))
L.append(E("libns", "engine", "synchronous load / export path", "thin"))
L.append(E("libns", "render", "relayout on mutation"))
L.append(E("libns", "anim",
           "ns_browser_tick: advance animations,\nrAF, timers, "
           "GLib main context"))
L.append(E("engine", "render", None, "thin"))
L.append(E("render", "css", "runs cascade", "thin"))
L.append(E("render", "layout", "builds box tree", "thin"))
L.append(E("libns", "net", "1 &#183; navigate: fetch URL", "pipe"))
L.append(E("net", "html", "2 &#183; response bytes", "pipe"))
L.append(E("html", "html_lexbor", "3 &#183; decoded UTF-8", "pipe"))
L.append(E("html_lexbor", "dom", "4 &#183; lexbor tree &#8594; ns_node DOM",
           "pipe"))
L.append(E("dom", "css", "5 &#183; cascade &#8594; ns_style table", "pipe"))
L.append(E("css", "layout", "6 &#183; style &#8594; ns_box layout tree",
           "pipe"))
L.append(E("layout", "paint", "7 &#183; box tree &#8594; Cairo", "pipe"))
L.append(E("xml", "dom", "XHTML / XML", "thin"))
L.append(E("css_media", "css", "@media evaluation", "thin"))
L.append(E("layout", "font", "web-font shaping", "thin"))
L.append(E("mathml", "layout", None, "thin"))
L.append(E("anim", "css", "animated style values", "thin"))
L.append(E("image_dec", "image", "decoded pixels", "thin"))
L.append(E("image", "texture", None, "thin"))
L.append(E("texture", "paint", "textures", "thin"))
L.append(E("net", "image", "subresource bytes\n(img, fonts, media)", "thin"))
L.append(E("selection", "paint", None, "thin"))
L.append(E("forms", "dom", None, "thin", constraint="false"))
L.append(E("spellcheck", "forms", None, "thin"))
L.append(E("js", "dom", "DOM + event bindings;\nwrapper identity cached "
           "on node", "flow", dir="both", constraint="false"))
L.append('dom -> js [style=invis, weight=3];')
L.append(E("js", "net", "fetch / XHR (CORS-gated)", "flow", constraint="false"))
L.append(E("js", "render", "computed styles,\ngeometry queries", "thin", constraint="false"))
L.append(E("js", "webcrypto", "binds all Web APIs", "flow",
           lhead='"cluster_webapi"'))
L.append(E("js", "bytecode_cache", None, "thin"))
L.append(E("js", "js_canvas", None, "thin"))
L.append(E("ext", "js", "content scripts", "thin"))
L.append(E("webgl", "glctx", "offscreen GLES context", "thin"))
L.append(E("webgl", "paint", "FBO readback composited", "thin",
           constraint="false"))
L.append(E("net", "cache", "store / revalidate", "thin"))
L.append(E("net", "csp", "policy gate", "thin"))
L.append(E("net", "safebrowsing", "host blocklist gate", "thin"))
L.append(E("net", "net_http2",
           "http_backend=nghttp2:\nalternate transport hop", "mode"))
L.append(E("ws", "net", None, "thin", constraint="false"))
L.append(E("eventsource", "net", None, "thin", constraint="false"))
L.append(E("net", "video", "media bytes\n(range requests)", "thin"))
L.append(E("libns", "config", "settings", "thin"))
L.append(E("libns", "history", "visit log", "thin"))
L.append(E("video", "video_decode", "decode frame each tick", "thin"))
L.append(E("video", "paint", "video frames as textures", "thin",
           constraint="false"))
L.append(E("video", "renderer_serve",
           "queues audio/video commands\n(drained as X-Audio)", "media",
           constraint="false", style="dashed"))
# vendored
L.append(E("html_lexbor", "lexbor", "tokeniser + tree builder", "thin"))
L.append(E("net", "lexbor", "WHATWG URL (ns_url_*)", "thin"))
L.append(E("js", "quickjs", "bytecode interpreter", "thin"))
L.append(E("polyfills", "js", "evaluated at startup", "thin",
           constraint="false"))
L.append(E("wasm", "wamr", "classic interpreter", "thin"))
L.append(E("image_dec", "wuffs", None, "thin"))
L.append(E("video_decode", "plmpeg", "MPEG-1", "thin"))
# grid-wrap invisible edges
for a, b in [
    ("webcrypto", "eventsource"), ("idb", "wasm"), ("ws", "webgl"),
    ("eventsource", "glctx"), ("wasm", "webgpu"), ("webgl", "camera"),
    ("glctx", "mic"),
    ("mic", "lexbor"), ("mic", "quickjs"), ("camera", "wamr"),
    ("camera", "wuffs"), ("webgpu", "plmpeg"),
    ("config", "debuglog"), ("history", "datetime"),
    ("js", "js_realm"), ("js", "js_intl"), ("js", "js_date"),
    ("js", "js_perf"),
    ("image_dec", "pdf"), ("texture", "spellcheck"),
    ("net", "netutil"), ("cache", "safebrowsing"), ("net_http2", "csp"),
    ("libns", "embed_shim"),
    ("lexbor", "polyfills"), ("quickjs", "minimp3"),
]:
    L.append('%s -> %s [style=invis];' % (a, b))
L.append('}')
PIECES["renderer"] = "\n".join(L)

# ------------------------------------------------------------- audio helper
L = ['digraph a {'] + header()
L.append(cluster("audio", "PROCESS: nordstjernen-audio (per tab, lazy)",
                 "unsandboxed device access, then self-sandboxes (Linux)",
                 "#FCF3DC", "#A8842C"))
L.append(node("audio_main", "audio/main.c (+ minimp3_impl.c, security.c)",
              "decodes to PCM in-process: pl_mpeg (MPEG-1/MP2),\n"
              "minimp3 (MP3), libav (Opus/Vorbis); mixes, resamples,\n"
              "outputs via SDL2 (WASAPI/CoreAudio/ALSA)",
              "SDL2, libcurl, FFmpeg libav*"))
L.append('}')
L.append('}')
PIECES["audio"] = "\n".join(L)

# ------------------------------------------------------------- video helper
L = ['digraph v {'] + header()
L.append(cluster("video", "PROCESS: nordstjernen-video (per tab, lazy)",
                 "built when libav is present; sandboxed like the renderer",
                 "#F4EAF8", "#7E4F9E"))
L.append(node("video_main", "videoproc/main.c (+ security.c)",
              "MSE / WebM frame decoding: libav demux + decode,\n"
              "swscale to BGRA, publishes into the shm frame ring",
              "FFmpeg libav*, libswscale"))
L.append('}')
L.append('vshm [shape=cylinder, fillcolor="#EFE3F5", color="#7E4F9E", '
         'penwidth=2, label=<<b>video frame shm ring</b><br/>'
         '<font point-size="10">BGRA frames, mapped read-only '
         'by the shell</font>>];')
L.append(E("video_main", "vshm", "BGRA frames", "media"))
L.append('}')
PIECES["video"] = "\n".join(L)

# ------------------------------------------------------------- shm fb
L = ['digraph f {'] + header()
L.append('shmfb [shape=cylinder, fillcolor="#DFF2E1", color="#1E7A3C", '
         'penwidth=2.5, label=<<b>shared-memory framebuffer</b><br/>'
         '<font point-size="10">memfd_create / shm_open segment, '
         'Cairo ARGB32;<br/>fd passed to the renderer via '
         'SCM_RIGHTS</font>>];')
L.append('}')
PIECES["shmfb"] = "\n".join(L)

# ------------------------------------------------------------- legend
L = ['digraph l {'] + header()
L.append('legend [shape=none, margin=0, label=<'
         '<table border="1" cellborder="0" cellspacing="0" cellpadding="4" '
         'bgcolor="white" color="#999999">'
         '<tr><td align="left" colspan="2"><font point-size="15"><b>Legend'
         '</b></font></td></tr>'
         '<tr><td align="left"><font color="#C0392B"><b>&#8594; dashed red'
         '</b></font></td><td align="left">process spawn / supervision</td></tr>'
         '<tr><td align="left"><font color="#2C5FA8"><b>&#8594; blue</b>'
         '</font></td><td align="left">IPC control channel (HTTP/1.1 + JSON)'
         '</td></tr>'
         '<tr><td align="left"><font color="#1E7A3C"><b>&#8594; green</b>'
         '</font></td><td align="left">pixel path (shared memory)</td></tr>'
         '<tr><td align="left"><font color="#B8860B"><b>&#8594; gold</b>'
         '</font></td><td align="left">media command / frame flow</td></tr>'
         '<tr><td align="left"><font color="#333333"><b>&#8594; gray</b>'
         '</font></td><td align="left">in-process call / data flow '
         '(1..7 = page load pipeline)</td></tr>'
         '<tr><td align="left"><font color="#7A5C00"><i>&#8658; dep</i>'
         '</font></td><td align="left">external system-library dependency '
         'of that module</td></tr>'
         '</table>>];')
L.append('}')
PIECES["legend"] = "\n".join(L)

# ------------------------------------------------------------- syslibs
L = ['digraph y {'] + header()
L.append('syslibs [shape=none, margin=0, label=<'
         '<table border="1" cellborder="0" cellspacing="0" cellpadding="4" '
         'bgcolor="white" color="#999999">'
         '<tr><td align="left" colspan="2"><font point-size="15"><b>'
         'External system libraries (never vendored)</b></font></td></tr>'
         '<tr><td align="left"><b>UI / graphics</b></td><td align="left">'
         'GTK 4, GLib/GIO, Cairo, Pango, gdk-pixbuf, libepoxy, '
         'FreeType, Fontconfig</td></tr>'
         '<tr><td align="left"><b>network / crypto</b></td><td align="left">'
         'libcurl, OpenSSL, libpsl, zlib, brotli &#183; optional: libnghttp2, '
         'ngtcp2 + nghttp3 + gnutls (HTTP/3)</td></tr>'
         '<tr><td align="left"><b>data / text</b></td><td align="left">'
         'SQLite, uchardet, Enchant (optional)</td></tr>'
         '<tr><td align="left"><b>media</b></td><td align="left">'
         'libwebp, libavif, SDL2, FFmpeg libav* '
         '(required Linux/Windows, auto macOS), poppler (optional)</td></tr>'
         '<tr><td align="left"><b>sandbox / GPU</b></td><td align="left">'
         'libseccomp + Landlock (Linux), wgpu-native (optional, headers '
         'vendored)</td></tr>'
         '</table>>];')
L.append('}')
PIECES["syslibs"] = "\n".join(L)


def render_pieces():
    for name, src in PIECES.items():
        open("piece_%s.dot" % name, "w").write(src)
        subprocess.run(["dot", "-Tpng", "-Gdpi=%d" % DPI,
                        "piece_%s.dot" % name,
                        "-o", "piece_%s.png" % name], check=True)
        subprocess.run(["dot", "-Tplain", "piece_%s.dot" % name,
                        "-o", "piece_%s.plain" % name], check=True)
        print("piece", name, "done")


if __name__ == "__main__":
    render_pieces()
    if "--compose" in sys.argv:
        import compose
        compose.main()
