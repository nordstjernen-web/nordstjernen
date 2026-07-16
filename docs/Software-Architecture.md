# Software architecture

How Nordstjernen is built, from the process model down to each engine
subsystem, and how those choices compare with Firefox (Gecko), Chrome
(Blink), and Ladybird (LibWeb).

Snapshot: **1.0.19**, 2026-07-16. This is a living map of the codebase;
the source is the source of truth. File references are given as
`path:line` and were accurate at the snapshot revision.

---

## 1. What Nordstjernen is

Nordstjernen is a web browser **written from scratch in C**, using GTK 4
for its desktop shell and libcurl for networking. It is an **independent
engine**: there is no Gecko, WebKit, or Blink lineage, no forked layout
or CSS code, nothing imported from another browser. The HTML→DOM parser
(lexbor), the JavaScript engine (QuickJS-ng), the image decoders
(Wuffs, libwebp), and the WebAssembly runtime (WAMR) are proven
third-party libraries vendored and integrated in-tree; the DOM, the CSS
cascade and selector engine, layout, paint, the media pipeline, and all
of the Web-platform bindings are original code.

The whole engine is roughly **155,000 lines of C** (`src/`, excluding the
vendored lexbor and QuickJS trees), small enough for a single person to
read and audit end to end. That constraint drives most of the
architecture below.

### Guiding constraints

- **Minimal, compact, auditable.** One human should be able to read the
  source. This rules out a code-generated binding layer, a GPU
  compositor, and a multi-megabyte dependency tree.
- **Secure by construction.** Each tab's engine runs in its own
  sandboxed OS process (seccomp + Landlock on Linux). There is **no
  JIT** anywhere — neither for JavaScript (QuickJS is a bytecode
  interpreter) nor for WebAssembly (WAMR runs the classic interpreter),
  so the renderer never generates or maps executable code at runtime and
  presents no W^X surface.
- **Standards-first, not browser-mimicking.** Behaviour is measured
  against the WHATWG/W3C/TC39 spec text section by section (see
  [HTML-compatibility.md](HTML-compatibility.md),
  [CSS-compatibility.md](CSS-compatibility.md)), not against another
  browser's quirks.
- **No telemetry.** The browser does not phone home. Safe-browsing is a
  local on-device blocklist with no network callout.
- **Reuse proven libraries, write the browser.** Hard, well-solved
  problems (HTML tokenisation, WHATWG URL parsing, a conformant JS
  interpreter, memory-safe image decoding) are delegated to vendored
  libraries; the browser-specific parts are written in-house.

---

## 2. Process model

A running Nordstjernen desktop session is a small tree of processes,
not one monolith. From the top:

```
nordstjernen (watchdog supervisor)
└── nordstjernen (GTK shell — thin, engine-free UI)
    ├── nordstjernen-renderer   (tab 1 — sandboxed engine)
    │   ├── nordstjernen-audio   (lazy, per tab)
    │   └── nordstjernen-video   (lazy, per tab)
    ├── nordstjernen-renderer   (tab 2 — sandboxed engine)
    └── …
```

**Watchdog supervisor.** A normal launch turns the process you start
into a tiny supervisor (`ns_watchdog_run_supervisor`,
`src/watchdog.c:537`, entered from `src/gtk/appmain.c:503`) that spawns
the real GUI shell as a child and restarts it if it crashes or hangs,
bounded by a crash-loop limit of 5 restarts per 60 s
(`src/watchdog.c:394`). A dedicated heartbeat thread inside the shell
(`src/watchdog.c:56`) turns a wedged main loop into a non-zero exit the
supervisor can recover. See [watchdog.md](watchdog.md).

**GTK shell.** The shell (`src/gtk/appmain.c`, `src/gtk/procview.c`) is
deliberately thin and **engine-free**: it owns windows, tabs, the
address bar, and input, but it never parses HTML, runs CSS, or executes
JavaScript. Each tab (`NsProcView`) blits a shared-memory framebuffer
and forwards input to its renderer.

**Per-tab renderer.** The engine — HTML parse, CSS cascade, layout,
Cairo/Pango paint, the QuickJS runtime, networking — runs inside one
sandboxed `nordstjernen-renderer` process **per tab**
(`src/renderer_http.c`, spawned at `src/gtk/procview.c:1174`). A renderer
crash is contained to its tab and transparently restarted
(`src/gtk/procview.c:1189`). This is a **process-per-tab** boundary; a
finer process-per-site boundary is noted as future work in
[tab-isolation.md](tab-isolation.md).

**Media helpers.** Audio and video decode outside the sandboxed renderer
in short-lived helper processes spawned by the shell — see
[§12](#12-media-audio-and-video).

**Single-process mode.** `--single-process` (or `NS_SINGLE_PROCESS=1`)
serves every tab from one in-process engine over the same IPC protocol
(`src/rproc_inproc.c`), trading the renderer sandbox and crash
containment for footprint and debuggability. **Headless** modes
(`--headless`/`--dump`/`--eval`/`--inspect`/`--wpt`/`--act`) are a
separate path again: they run the engine in-process with no shell and no
IPC at all (`ns_run_headless`, `src/gtk/appmain.c:399`). See
[single-process-mode.md](single-process-mode.md).

---

## 3. IPC: an HTTP/JSON control channel + a shared-memory framebuffer

The shell↔renderer boundary is one of Nordstjernen's more distinctive
choices. Rather than a bespoke binary protocol, every control message is
a **plain HTTP/1.1 POST with a small JSON body**, and only the rendered
pixels travel through shared memory (see [Rendering.md](Rendering.md)).

- **Transport.** An `AF_UNIX` `socketpair` on POSIX (the renderer reads
  and writes fd 3, `src/rproc_http.c:137`), inherited stdio pipes on
  Windows, or a `stdio` mode for JVM/Android embedders. Framing is
  minimal HTTP/1.1 in `src/ipc_http.c` (`http_write_request`,
  `http_read_head`).
- **Messages are POST paths**, all dispatched in `ns_renderer_session_handle`
  (`src/renderer_serve.c:261`): `/open`, `/render`, `/click`, `/key`,
  `/hover`, `/scroll`, `/select`, `/find`, `/viewport`, `/eval`,
  `/console`, `/media`, `/export`, `/quit`, and others. The client-side
  wrappers live in `src/rproc_http.c`.
- **Frames are shared memory.** The shell backs the framebuffer with an
  anonymous `memfd_create` (Linux) or an immediately-unlinked `shm_open`
  segment (other Unix), passes the descriptor to the renderer over
  `SCM_RIGHTS` (`src/rproc_http.c:169`), and the renderer wraps it
  directly as a Cairo ARGB32 surface. `/render` replies carry only
  geometry in `X-*` headers (`X-W`, `X-H`, `X-Stride`, `X-Anim`,
  `X-PageW`, `X-PageH`); the body is empty.
- **Renderer→shell side-channels** ride the `/render` reply as extra
  headers drained from the engine's pending queues: `X-Nav` (a
  script-initiated navigation), `X-Audio` (media-helper commands),
  `X-WebGL`/`X-Camera` (permission prompts), `X-Download` (a download
  offer).

The protocol is readable in a trace and trivial to extend, which is the
whole point: a browser's process boundary is normally the least
inspectable part of the system.

---

## 4. The rendering pipeline

Inside a renderer, a page moves through a fixed sequence. `src/engine.c`
orchestrates the synchronous form used by headless/export; `src/render.c`
is the reusable style→layout core that both `engine.c` and the live
browser share.

```
network bytes
  → charset decode        (src/html.c — uchardet, WHATWG label table)
  → HTML parse            (src/html_lexbor.c — lexbor tokeniser + tree builder)
  → DOM tree              (ns_node, src/dom.c)
  → CSS cascade + style   (src/css.c — ns_node* → ns_style*)
  → layout                (src/layout.c — the ns_box tree)
  → paint                 (src/paint.c — Cairo + Pango)
  → shared-memory framebuffer → shell blit
```

**Charset decode.** `ns_html_decode_body_full` (`src/html.c:685`) applies
BOM sniffing, then the declared charset, then UTF-8 validation, then
**uchardet** detection, then a Windows-1252 fallback. Dangerous
encodings (UTF-7, HZ, ISO-2022) are rejected.

**HTML parse.** `ns_html_parse` (`src/html_lexbor.c:767`) drives the
in-tree **lexbor** tokeniser and tree constructor (scripting flag on),
then converts the lexbor tree into the native DOM
(`lxb_to_nd_root`, `:204`), preserving quirks mode, declarative shadow
DOM, and standards media-metadata extraction. The lexbor document is
kept alive as backing store on the DOM root.

**Style + layout core.** `ns_render_relayout_profile` (`src/render.c:167`)
sets the viewport, runs `ns_css_compute` to produce the `ns_node* →
ns_style*` table, builds the `ns_box` layout tree with `ns_layout_build`,
and — only if the page uses container queries — measures containers and
runs a second cascade+layout pass. It then publishes the style table and
layout root to the JS engine so scripts see live computed styles and
geometry.

**Paint.** `ns_paint` (`src/paint.c:6100`) walks the box tree into a
Cairo surface. See [§7](#7-paint-text-and-fonts).

---

## 5. The DOM

The DOM is a hand-rolled tree of `ns_node` (`src/dom.h:63`), not a
wrapper over lexbor's tree. Each node carries its `kind`
(document/doctype/element/text/comment), tag name, text, a
singly-linked attribute list, and parent/child/sibling pointers. Per
document, `ns_node` maintains **id, class, and tag indexes** (hash
tables built in bulk in document order, `src/dom.c:1280`) plus an
attribute Bloom filter for fast selector rejection, so `getElementById`
and selector matching don't rescan the tree.

The bridge from lexbor to `ns_node` is `lxb_node_convert`
(`src/html_lexbor.c:60`), which copies attributes (including namespaced
ones), tags SVG/MathML nodes with their namespace, and follows
`<template>` content. Node→JS wrapper identity is cached on the node
itself (see [§8](#8-the-javascript-engine)).

---

## 6. The CSS engine

`src/css.c` (~17,000 lines) is a full cascade and selector engine with
its own data model in `src/css.h`: 190+ longhand properties, a tagged
value union (keyword/length/color/calc/shadow/gradient/grid-tracks/
transform/…), specificity-carrying selectors, and a computed `ns_style`
record that holds a `values[]` array plus sub-styles for each supported
pseudo-element and a custom-property (`--var`) map.

- **Cascade.** `ns_css_compute` (`src/css.c:17297`) parses and caches the
  UA sheet once, builds per-sheet rule indexes, computes cascade-layer
  ranks, registers `@property` defaults, and walks the DOM. Per element,
  matches are gathered from the UA sheet, presentational hints, and
  author sheets, then sorted by origin → layer → specificity → source
  order, with `!important`, `revert`, `inherit`/`initial`, and inherited
  fill-down resolved in `cascade_for` (`src/css.c:15442`). Font-relative
  and viewport/container units are resolved during the cascade.
- **Selectors.** `ns_css_selector_matches` (`src/css.c:14051`) matches
  the rightmost compound then walks combinators right-to-left (child,
  `+`, `~`, descendant). The pseudo-class/element coverage is broad,
  including `:has()`, `:is()`/`:where()`, structural, state, and the
  HTML-connected pseudo-classes.
- **UA stylesheet.** The `kUa` literal (`src/css.c:15108`) supplies the
  default rendering (display types, heading scale, list markers, form
  controls, `[hidden]`, `dialog`, popover). It is the browser's
  implementation of the HTML Rendering section — the sections/grouping
  elements have no element-specific code path, only UA CSS.
- **Media & container queries.** `ns_css_media_query_matches`
  (`src/css.c:10347`) evaluates width/height/aspect-ratio/resolution/
  orientation/`prefers-color-scheme`/`prefers-reduced-motion`/pointer/
  hover and range syntax.

---

## 7. Layout, paint, text, and fonts

**Layout** (`src/layout.c`, ~11,000 lines) builds the `ns_box` tree from
the style table. `layout_box` (`src/layout.c:7377`) dispatches by box
kind into the implemented formatting contexts:

| Context | Entry | Notes |
|---------|-------|-------|
| Block | `layout_block` | margin collapsing, multicol |
| Inline | `inline_layout` | Pango-shaped runs, atomic inlines |
| Flex | `layout_flex_row`/`_wrap`/`_column` | `order`, wrapping |
| Grid | `layout_grid` | track sizing, spans, named areas |
| Table | `layout_table` | colspan/rowspan, border models |
| MathML | `ns_math_measure` | presentation MathML |

**Paint.** Page painting is **entirely Cairo software rendering** —
there is no GPU compositor for page content. `ns_browser_render_argb32`
(`src/libnordstjernen.c:1451`) wraps the shared-memory buffer directly as
a Cairo ARGB32 surface and calls `ns_paint` (`src/paint.c:6100`), whose
recursive `paint_walk` (`:5468`) culls hidden/offscreen boxes, applies
opacity/blend/mask groups, sticky offsets, 2D and 3D transforms, and
clip-paths, and paints positioned/z-indexed children in stacking order.

**Text** is shaped with **Pango** (`pango/pangocairo.h`). `@font-face`
fonts are fetched, converted from WOFF/WOFF2 to SFNT via FreeType, and
registered with Fontconfig so the Pango-FC fontmap can use them
(`src/font.c`).

**Animation.** `src/anim.c` tracks CSS transitions and `@keyframes`.
The per-frame pump `ns_browser_tick` (`src/libnordstjernen.c:1231`)
advances the animator, dispatches transition/animation events, runs
`requestAnimationFrame` callbacks, pumps the GLib main context, and
relayouts on mutation. `ns_browser_animating` gates whether the shell
runs a continuous frame loop or renders on demand.

**Images** are decoded by `ns_image_decode_bytes` (`src/image.c:492`),
which tries decoders in a fixed content-sniffed order and returns a
texture: **ICO/CUR → Wuffs** (memory-safe: PNG/APNG, GIF, BMP, JPEG,
WebP) **→ libwebp** (animated/extended WebP) **→ libavif** (if built)
**→ gdk-pixbuf** (catch-all raster) **→ librsvg** (SVG).

---

## 8. The JavaScript engine

The engine is an in-tree fork of **quickjs-ng** (`src/quickjs/`, version
0.15.1), a compact **bytecode interpreter with no JIT**. Each browsing
context gets one `JSRuntime` + `JSContext`, created in `ns_js_new`
(`src/js.c:39454`) with a 2 GB memory cap and a 5 MB stack.

**DOM bindings.** All DOM node types share **one** `JSClassID`
(`ns_element_class_id`); the C `ns_node *` is the JS object's opaque
pointer, and node→wrapper identity is cached on the node so the same
node always yields the same JS object. Types are distinguished by
**per-kind prototypes**, and `instanceof` is driven by a custom
`[Symbol.hasInstance]` over a tag→constructor table covering ~70 HTML
element interfaces. Genuinely dynamic surfaces (Window named properties,
`CSSStyleDeclaration`, `DOMTokenList`, live collections, `dataset`) use
QuickJS **exotic objects**. `src/js.c` is the single largest file in the
tree (~47,000 lines) because it hosts essentially the whole Web-platform
binding surface.

**Event loop.** The engine has no internal loop; JS work is **pumped
from the host render loop**. Microtasks (promises, `queueMicrotask`)
drain via `JS_ExecutePendingJob`; timers are GLib sources; `requestAnimationFrame`
callbacks are drained each frame with a clamped high-resolution
timestamp. A per-invocation soft budget and a hard **60-second wall-clock
monitor** are enforced through `JS_SetInterruptHandler`
(`ns_js_interrupt_cb`, `src/js.c:413`); time spent blocked in a nested
main loop (a synchronous fetch) is credited back so I/O doesn't burn the
budget.

**Web-platform APIs** implemented over this engine include `fetch`/XHR
(driving the C networking layer), **WebCrypto** (`crypto.subtle` over
OpenSSL libcrypto, `src/webcrypto.c`), **IndexedDB** (a low-level
SQLite-backed native backend with the W3C object model built in
`polyfills.js`), **WebSocket** (over libcurl's native WS API, `src/ws.c`),
**EventSource**, **Web Workers** (each a separate `GThread` with its own
runtime, cross-thread messaging by structured clone), `localStorage`/
`sessionStorage`, Canvas 2D (Cairo, `src/js_canvas.c`), an ICU-free
**Intl** (`src/js_intl.c`) and **Temporal** (`src/js_date.c`), and the
observer APIs.

**WebAssembly** runs on **WAMR** (`src/wamr/`, the classic interpreter
with reference-types and bulk-memory, no JIT/AOT); `src/wasm.c` binds the
`WebAssembly` JS API over it.

**Polyfills.** A ~331 KB `data/js/polyfills.js` bundle is embedded at
build time and evaluated after the native bindings install. It builds
higher-level spec object models over thin native backends (the whole
IndexedDB interface over `__nd_idb`), and adds faithfulness shims (making
patched methods report `[native code]`). It is syntax-verified against
the in-tree `qjs` at build time.

**Bytecode cache.** `src/bytecode_cache.c` caches compiled QuickJS
bytecode, keyed by SHA-256 of the source, in a two-tier memory+disk
cache (16 MB memory LRU; on-disk sharded under the cache dir), so large
scripts skip recompilation across loads.

---

## 9. Networking

Networking is built on **libcurl** (`src/net.c`, ~6,000 lines):

- **HTTP/2 by default** (`CURL_HTTP_VERSION_2TLS`), with **HTTP/3**
  opt-in.
- **WHATWG URL** parsing, resolution, origin/host/site extraction all
  route through lexbor's URL module (`ns_url_*`), so there is no separate
  URL library.
- **Same-origin / same-site** are computed with `ns_url_same_origin` and
  `ns_url_is_same_site` (the latter using the **Public Suffix List** via
  libpsl for the registrable domain).
- **Fetch metadata & client hints.** Navigations send the full
  `Sec-Fetch-*` set, `Sec-CH-UA*` under a Chrome identity,
  `Upgrade-Insecure-Requests`, and a compat-mode-selectable User-Agent
  (Chrome/Firefox/Ladybird presets).
- **HSTS** and an optional **HTTPS-first** upgrade path.
- **HTTP cache**: a **SQLite-indexed** cache with on-disk bodies
  (`src/cache.c`), honouring ETag/Last-Modified.
- **Cookies** partitioned per site; JS-set cookies persisted in a
  sibling file curl never overwrites.

The renderer performs its own fetching, cookie, and cache I/O today;
[tab-isolation.md](tab-isolation.md) records browser-process brokering of
the network as intended future hardening.

---

## 10. Security and sandboxing

Confinement is layered, and the **out-of-process renderer is the real
boundary** (`src/security.c`).

- **Linux — Landlock + seccomp.** The renderer seals a **Landlock**
  filesystem ruleset (`ns_security_sandbox_init`, `src/security.c:362`):
  read+exec on system library dirs, read-only on config/certs/proc/sys,
  and read-write only on its own runtime, cache, and downloads
  directories. It then loads a **seccomp-bpf** filter
  (`ns_security_seccomp_init`, `:814`) whose default action is
  fail-with-`EPERM`, allowing ~230 named syscalls and **denying `execve`,
  `ptrace`, `mount`, `setuid`, `bpf`, and other escape primitives by
  omission**. Both are sealed before the renderer opens any page
  (`src/renderer_http.c:258`). The shell gets Landlock only (it must
  `fork`/`execv` renderers, which the no-`execve` filter would block).
- **macOS** applies a Seatbelt filesystem-write-confinement profile;
  **Windows** applies `SetProcessMitigationPolicy` (DEP, ASLR,
  strict-handle, image-load, child-process restrictions). Other
  platforms are unsandboxed no-ops.
- **Same-Origin Policy / CORS.** Enforced through `ns_url_same_origin`;
  `fetch`/XHR responses are gated by a CORS check
  (`cors_allows`, `src/js.c:7084`).
- **CSP** (`src/csp.c`) parses and enforces 11 directives including
  `frame-ancestors`, with source-list matching, nonces, and
  sha-256/384/512 inline hashes.
- **secretbox** (`src/secretbox.c`) seals stored secrets with
  AES-256-GCM under a PBKDF2-HMAC-SHA256 key (600,000 iterations).
- **No JIT** means no runtime code generation for JS or wasm, removing
  the W^X attack surface entirely; runaway scripts are bounded by the
  60-second interrupt monitor rather than a kill.
- **Safe-browsing** (`src/safebrowsing.c`) is a **local** SHA-256 host
  blocklist with an interstitial — no network callout, no telemetry.

See [tab-isolation.md](tab-isolation.md), [watchdog.md](watchdog.md).

---

## 11. Threading model

A renderer is **single-main-thread**: DOM, CSS, layout, Cairo/Pango
paint, and page JavaScript all live on one thread running a GLib main
loop. Background threads exist only for inherently blocking or
self-contained work — network I/O, HTML/CSS parsing, image decoding,
WebSocket/EventSource pumps, and Web Workers (each its own runtime) —
and every off-thread result is handed back through a GLib main-context
invocation before it touches shared state. The invariant: **nothing
outside the main thread ever touches an `ns_node`, a layout box, or a
live page `JSContext`.** See [threading.md](threading.md).

---

## 12. Media: audio and video

Media decodes **outside** the sandboxed renderer, in helper processes
the shell spawns per tab and drives over the `X-Audio` side-channel
(see [media.md](media.md)):

- **`nordstjernen-audio`** (`src/audio/main.c`) decodes to PCM in-tree —
  pl_mpeg (MPEG-1/MP2), minimp3 (MP3), and libav (Opus/Vorbis) — and
  plays through SDL2. It speaks a one-command-per-line text protocol over
  stdin/stdout.
- **`nordstjernen-video`** (`src/videoproc/main.c`) decodes MSE/WebM
  frames and publishes BGRA frames into its **own shared-memory ring**,
  which the shell maps read-only and composites over the page surface
  each frame tick.

The always-on, in-tree codecs are **pl_mpeg** (MPEG-1) and **minimp3**
(MP3). **WebM (VP9/VP8 + Opus/Vorbis)** is the one FFmpeg-backed
extension over the system `libav*` libraries — required on Linux/Windows,
auto-detected on macOS. Nordstjernen deliberately ships **no general
media stack**.

---

## 13. Graphics APIs

- **WebGL 1/2** (`src/webgl.c`) maps the API more or less directly onto
  OpenGL ES through a toolkit-independent offscreen GL context
  (`src/glctx.c`) and libepoxy — a surfaceless EGL context on Linux, WGL
  on Windows. There is no ANGLE layer and no command-stream validator.
  It is **opt-in, off by default**, gated by a per-site trust prompt; the
  result renders into an FBO, is read back, and composited into the Cairo
  scene like any other image.
- **WebGPU** (`src/webgpu.c`) is **experimental**, layered over the
  external wgpu-native library. The build feature is `auto` (compiled
  only when wgpu-native is present) and it stays off at runtime unless
  started with `--enable-webgpu`. A stock build carries no WebGPU symbol.
- **Canvas 2D** (`src/js_canvas.c`) is Cairo+Pango backed.

See [webgl.md](webgl.md), [webgpu.md](webgpu.md).

---

## 14. Storage, i18n, and the local start page

- **Storage backends.** SQLite backs the HTTP cache (`src/cache.c`) and
  IndexedDB (`src/idb.c`); `localStorage`/`sessionStorage` are per-origin
  partitioned maps flushed to disk; config is a plain key-file under the
  user config dir.
- **i18n.** UI strings are English-source and translated at startup
  through an in-tree catalogue lookup (`src/i18n.c`, `data/i18n/*.lang`).
  There is no gettext dependency.
- **Local AI start page.** `about:start` is a chat window backed by a
  CPU-only local model through a vendored llama.cpp (`src/ai.c`).
  Inference is fully local; no network is touched. This is a browser
  feature, not a Web API — Nordstjernen exposes **no** AI-style web
  APIs to pages.

---

## 15. Build, embedding, and platforms

- **Build system:** meson + ninja. Optional features (`gtk`, `wasm`,
  `ai`, `audio`, `webgpu`) are meson feature flags; ccache is picked up
  automatically. The vendored lexbor and QuickJS trees are loaded via
  `subdir()` and expose declared dependencies directly, not as
  subprojects.
- **Embedding:** the engine is exposed as a shared library
  `libnordstjernen` with a plain-C API (`src/libnordstjernen.h`,
  `ns_browser_*`) that carries no GLib or GTK types — the same
  synchronous pipeline the headless driver uses. See
  [Embedding.md](Embedding.md).
- **Platforms:** Linux, macOS, Windows (MSYS2/MinGW), plus Android and a
  JVM binding (`java/`, JNI over the same C API) and iOS. Desktop uses
  GTK 4; the engine core is GTK-optional.
- **License:** Nordstjernen Source License v1.0 (`LicenseRef-NSL-1.0`).

---

## 16. Comparison with Firefox, Chrome, and Ladybird

Nordstjernen sits in a specific corner of the design space: an
**independent** engine like Ladybird and (historically) Gecko, but
written in **C** with **no JIT** and an unusually **small, reuse-heavy**
codebase. The table summarises; the notes explain.

| Dimension | Nordstjernen | Firefox (Gecko) | Chrome (Blink) | Ladybird (LibWeb) |
|-----------|--------------|-----------------|----------------|-------------------|
| Language | C | C++ and Rust | C++ (some Rust) | C++ (moving to Swift) |
| Engine lineage | Independent, from scratch | Independent (Gecko) | Blink, forked from WebKit ← KHTML | Independent, from scratch |
| CSS engine | Own (`src/css.c`) | Stylo (Rust, from Servo) | Blink style + LayoutNG | Own (LibWeb CSS) |
| Layout | Own (`src/layout.c`) | Reflow frame tree | LayoutNG | Own (LibWeb) |
| JS engine | QuickJS-ng (fork) | SpiderMonkey (own) | V8 (own) | LibJS (own) |
| JIT | **None** (interpreter) | Multi-tier JIT | Multi-tier JIT | Interpreter (bytecode) |
| WebAssembly | WAMR interpreter | SpiderMonkey (JIT) | V8 (JIT) | LibWasm |
| HTML parser | lexbor (vendored) | Own | Own | Own (LibWeb) |
| Page compositing | Cairo software; GPU only for WebGL/GPU/canvas | WebRender (GPU) | Viz/cc (GPU) | CPU paint (LibGfx/Skia) |
| Networking | libcurl | Necko (own) | Chromium net service (own) | RequestServer (libcurl) |
| Process model | Process-per-tab | Process-per-site (Fission) | Process-per-site (site isolation) | Process-per-tab + service processes |
| IPC | HTTP/JSON + shm framebuffer | IPDL | Mojo | Own IPC compiler over sockets |
| Sandbox | Landlock + seccomp (Linux) | seccomp + namespaces; Fission | seccomp + namespaces; site isolation | Process isolation (evolving) |
| Dependency stance | Reuse proven libs, write the browser | Mostly in-house | Mostly in-house | In-house core, some third-party |
| Approx. size | ~155 K lines C | tens of millions | tens of millions | ~1M+ lines |
| Telemetry | None | Optional | Yes | None |

### Firefox (Gecko)

Gecko is a mature, independent engine in **C++ and increasingly Rust**:
the CSS engine (**Stylo**) and the compositor (**WebRender**) are Rust
components adopted from Servo, and **SpiderMonkey** is a
multi-tier JIT. Firefox's process model has moved to **Fission** —
site-isolated content processes plus dedicated GPU, socket, and media
(RDD) processes — coordinated over the **IPDL** message system. It is a
GPU-composited, JIT-driven browser optimised for full web performance at
the cost of an enormous, multi-language codebase.

Nordstjernen shares the *independent-engine* stance but inverts almost
every implementation choice: one language (C), no JIT, no Rust
concurrency machinery, software page compositing, and a codebase five
orders of magnitude smaller. Nordstjernen's per-tab renderer with a
seccomp+Landlock sandbox is conceptually similar to a Firefox content
process, but Nordstjernen isolates per **tab**, whereas Fission isolates
per **site**.

### Chrome (Blink)

Blink descends from **WebKit** (itself from KHTML), so unlike
Nordstjernen it is *not* a from-scratch engine, but it is the reference
point for the modern browser architecture: **V8** with a full JIT tier
stack (Ignition → Sparkplug → Maglev → TurboFan), **LayoutNG**, a
Skia-based GPU compositor (**Viz**/cc), a separate **network service**, a
**GPU process**, and strict **site isolation** with one renderer per
site-instance, all wired together with **Mojo** IPC. It is the most
capable and the largest of the four.

Every one of those choices is a scale/performance trade Nordstjernen
declines on purpose. There is no JIT (security and size), no GPU
compositor (a page paints in Cairo; the GPU is used only by WebGL/WebGPU/
canvas producers that hand back CPU-side textures), no code-generated
binding layer (bindings are hand-written C in `js.c`), and no Mojo-style
typed-interface IPC (the boundary is human-readable HTTP/JSON). Where
Chrome sends `Sec-CH-UA`/`Sec-Fetch-*`/`Upgrade-Insecure-Requests` to
present a consistent client, Nordstjernen sends the same header set under
a selectable Chrome/Firefox/Ladybird identity so pages that gate on a
mainstream browser environment still run.

### Ladybird (LibWeb / LibJS)

Ladybird is Nordstjernen's closest architectural relative: a **truly
independent, from-scratch engine** (LibWeb + LibJS), originally from
SerenityOS, now cross-platform, and — like Nordstjernen — with **no JIT**
(LibJS is a bytecode interpreter) and a multi-process model that runs the
engine in a dedicated **WebContent** process per tab alongside separate
**RequestServer**, **ImageDecoder**, and **WebWorker** processes.
Ladybird, too, has adopted third-party libraries after going
cross-platform (it uses **libcurl** in RequestServer, plus Skia and
HarfBuzz).

The differences are language and reuse philosophy. Ladybird is **C++**
(with new work moving to Swift) and writes its **JS engine, CSS engine,
and layout all from scratch**, including LibJS. Nordstjernen is **C** and
takes a more pragmatic middle line: it writes its own DOM, CSS, layout,
and paint, but **vendors** a proven JS interpreter (QuickJS-ng), HTML
parser (lexbor), image decoders (Wuffs/libwebp), and wasm runtime (WAMR)
rather than reimplementing them. The result is a much smaller codebase
(~155 K lines of C versus Ladybird's 1M+), a deliberate non-goal of a
from-scratch JS engine, and the distinctive HTTP/JSON+shared-memory IPC
boundary. Both projects share the same north star — a readable,
independent, standards-first browser that is not a rebrand of an existing
engine. There is a source-level JS feature comparison between the two
engines in
[quickjs-libjs-compare.md](quickjs-libjs-compare.md).

### Where Nordstjernen is deliberately different

- **C, not C++/Rust/Swift.** Unusual among modern browsers; keeps the
  toolchain and the mental model small.
- **No JIT anywhere.** A security-first choice (no runtime codegen, no
  W^X surface) accepted at the cost of peak JS/wasm throughput. Ladybird
  shares the no-JIT-JS property; Chrome and Firefox do not.
- **Software page compositing.** The whole page paints in Cairo; the GPU
  is confined to WebGL/WebGPU/canvas. No retained-mode GPU compositor.
- **Reuse over reimplementation.** Vendored lexbor/QuickJS/Wuffs/WAMR
  instead of in-house parsers and a from-scratch JS engine — the opposite
  of Ladybird's write-everything approach, and unlike Chrome/Firefox's
  fully in-house engines.
- **Inspectable IPC.** A readable HTTP/JSON control channel plus a
  shared-memory framebuffer, versus the typed binary IPC (Mojo/IPDL/
  Ladybird's IPC compiler) the others use.
- **Auditability as a hard constraint.** ~155 K lines of C is the design
  budget, not an accident. It is what makes one-person review of the
  whole engine feasible, and it is the reason the browser reuses proven
  libraries and declines a GPU compositor and a JIT.

---

## See also

- [HTML-compatibility.md](HTML-compatibility.md) — §1–§16 spec walk-through
- [CSS-compatibility.md](CSS-compatibility.md) — CSS coverage
- [Rendering.md](Rendering.md) — render path and the IPC framebuffer
- [tab-isolation.md](tab-isolation.md), [threading.md](threading.md),
  [single-process-mode.md](single-process-mode.md),
  [watchdog.md](watchdog.md) — process, threading, isolation
- [media.md](media.md), [webgl.md](webgl.md), [webgpu.md](webgpu.md),
  [webassembly.md](webassembly.md) — media and accelerated content
- [Embedding.md](Embedding.md) — the C embedding API
- [quickjs-libjs-compare.md](quickjs-libjs-compare.md) — QuickJS vs LibJS
