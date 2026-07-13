# Nordstjernen — Development Plan

Living plan for a web browser written from scratch in **C**,
small enough for one person to audit end-to-end. The engine and all
non-toolkit logic live as portable C in `src/`; the GUI is a thin
**GTK 4** frontend over that shared C and **libcurl**.
No upstream engine (Gecko / WebKit / Blink) is read, ported, or imported.
See `README.md` for the product vision and `CLAUDE.md` for working rules.

## Principles

- **One competent human's worth of code** — when work balloons, cut
  scope, not corners. A working subset beats an unfinished superset.
- **Vertical slices that ship** — every task ends in something runnable.
- **Few small auditable deps**, vendored in-tree: lexbor (HTML + WHATWG
  URL), QuickJS (JS interpreter), Wuffs (image decode), WAMR
  (WebAssembly interpreter).
- **No automated test suite** — verify by running the browser.
- No JIT, therefore more secure. However, this means the rest of the browser
  engine needs to be super-fast. 
- **No code comments** beyond one header line per file (see `CLAUDE.md`).

## Non-goals (won't change)

WebRTC / WebUSB / WebBluetooth / WebMIDI / WebHID;
service-worker push and background sync (registration, lifecycle, and
`FetchEvent` network interception via `respondWith` *are* supported —
`ns_sw_post_fetch_request` routes `fetch()` through the worker);
DRM / EME; **JIT** (QuickJS
interpreter only — W^X holds process-wide); plugins (NPAPI / PPAPI /
WebExtensions); sync / accounts / telemetry /
"studies".

WebGL is the standard exception to the no-GPU-APIs stance: a minimalist,
opt-in WebGL 1 / 2 implementation mapped directly onto OpenGL ES, off by
default and gated by a per-site trust prompt (see `docs/webgl.md`).

WebGPU (`navigator.gpu`) is an **experimental** feature layered on the
external [wgpu-native](https://github.com/gfx-rs/wgpu-native) library. The
`webgpu` build feature is `auto`: it is compiled in whenever wgpu-native is
present and skipped otherwise, so a machine without the library still gets a
WebGPU-free binary. At runtime it stays off until the browser is started with
`--enable-webgpu` (equivalently `NS_WEBGPU_ALLOW=1`); see `docs/webgpu.md`.
wgpu-native is a large dependency that does not fit the minimalism the rest of
the engine is built around, so it is never made a hard or default dependency.

## Current state

A usable HTML5 + modern-CSS + ~ES2020 browser. lexbor parses to a DOM;
the engine does the CSS cascade and selectors, block/inline/flex/grid/
table/multicol/float/positioned layout, and Cairo + Pango paint
(gradients incl. `repeating-*` and gradient masks, filters, transforms,
transitions / `@keyframes`). QuickJS bindings cover DOM, Shadow DOM,
`fetch`/XHR, canvas 2D, storage (`localStorage`/`sessionStorage`,
IndexedDB over SQLite), `WebSocket`/`EventSource`, the
Resize/Intersection/Mutation observers, and `crypto.subtle`
(WebCrypto over OpenSSL, `src/webcrypto.c`); forms support submission
and constraint validation; `overflow` boxes scroll. The full
`WebAssembly` JS API runs on a vendored WAMR interpreter
(`src/wasm.c`), and an opt-in, per-site-gated WebGL 1 / 2 maps onto
OpenGL ES (`src/webgl.c`). Painting skips off-screen boxes (viewport
culling). Runs on Linux, Windows (MSYS2) and macOS, with an Android
port [published on Google Play](https://play.google.com/store/apps/details?id=org.nordstjernen.WebBrowser);
CI builds the desktop three plus musl and
the Java binding on every push (the BSDs run nightly / on dispatch).
The GTK frontend is a tabbed, **process-per-tab** browser:
each tab drives its own sandboxed renderer process over the engine and
shows full-fidelity output. The shell is a thin display/input client —
it carries no bespoke in-process renderer anymore (the optional
`--single-process` mode runs the same renderer on a thread). The
`about:start` new-tab page hosts a local AI assistant (`src/ai.c`,
llama.cpp over a pinned Meson subproject): chat, Wikipedia/DuckDuckGo
tools, and digest-pinned model downloads, all on-device with no network
at inference time (see `docs/ai.md`).

The release version lives in the meson project definition and is
surfaced through `src/version.h`.

## Architecture & frontends

The codebase is layered so the GUI stays thin and the engine stays
toolkit-agnostic:

- **`src/` — common C core.** The engine (lexbor parse, CSS cascade,
  layout, Cairo/Pango paint, QuickJS, WAMR, image decode, networking)
  plus frontend-agnostic `ns_` helpers shared by alternative GUIs
  (the `ns_net_fetch_*` networking calls, the `ns_url_*` URL helpers,
  the `src/headless.c` driver, and the `ns_browser` embedding API in
  `src/libnordstjernen.h`). New shared logic
  lands here in C, in the house style (`ns_` snake_case, one-line SPDX
  header, no comments).
- **`src/gtk/` — GTK 4 frontend.** A thin process-per-tab
  shell (`appmain` entry point, `procwindow`/`procview` over `rproc_http`)
  that spawns one sandboxed `nordstjernen-renderer` process per tab and
  blits its framebuffer. It carries the browser chrome — navigation,
  tabs, history, zoom, selection, `:hover`, find-in-page, a context menu,
  the DevTools console, save/export, the media helper plumbing, an app
  menu, settings, and bookmarks. The former in-process engine renderer
  has been removed.
- **`java/` — Java / JVM binding and Swing app.** A Java library
  (`org.nordstjernen.Nordstjernen`, JDK 21) embeds the engine on the JVM
  through a thin JNI bridge over the C embedding API
  (`src/libnordstjernen.h`), with a no-JNI `RemotePage`/`RemoteBrowser`
  client that drives a separate `nordstjernen-renderer` process over the
  renderer's HTTP/JSON protocol. `org.nordstjernen.app.Browser` is a
  standalone Swing browser app built on that client with GTK-shell-style
  chrome. See `java/README.md`.
- **`android/` — Android port (on Google Play).** A Kotlin shell
  (`MainActivity`, `PageView`, `NativeBrowser`) over the same engine via
  JNI, [published on the Play Store](https://play.google.com/store/apps/details?id=org.nordstjernen.WebBrowser);
  see `docs/Android.md`.

**Process-per-tab renderer boundary — shipped.** Each tab runs in its own
sandboxed *process* that runs the engine and produces a rendered surface;
the GUI process is reduced to hosting a widget per tab, blitting the
shared-memory framebuffer, and forwarding input over the `rproc_http` IPC
protocol (render/viewport, click/key/hover/select, find, export, media,
console/eval). This is the payoff that makes the design cohere: the GTK
shell is a thin display-plus-input layer showing the engine's output
(so it needs no separate renderer), and isolation is
real — `nordstjernen-renderer` applies the same Landlock + seccomp
confinement as the engine at startup (`ns_browser_sandbox`, called right
after `ns_browser_init` and before any page
is opened), so a renderer crash is a per-tab failure and untrusted content
always runs under a loaded syscall filter. The shell carries no bespoke
in-process renderer any more; the optional `--single-process` mode serves the
same renderer over a thread instead of a child process. The thin shell parses
no untrusted bytes but must `fork`/`execv`
renderers and create POSIX shm, so it runs under a widened Landlock with
seccomp skipped. The mechanics live in `docs/tab-isolation.md`.

**The plan from here:**

1. **Browser-process broker services** (networking, cookies, cache,
   storage) so the renderer can be credential-less rather than fetching and
   persisting on its own — the true security payoff.
2. A shell-specific seccomp profile (or a renderer zygote) and re-enabled
   shell watchdog supervision.

## Priorities

**Now**
- **18 · CSS cascade performance** — the biggest "feels slow" lever.
  Incremental/partial re-cascade of dirtied subtrees plus a conservative
  computed-style sharing cache. A full r/news-scale cascade is ~0.5 s
  today and the post-hydration re-cascade dominates SPAs. Land as small,
  measured commits — profile before/after; this area regresses easily
  (prior per-element class-hashset attempts both regressed).

**Next**
- **19 · Incremental paint** — viewport culling shipped; the real win is
  dirty-region partial repaint (re-rasterize only changed rects), which
  also unblocks animated smooth scrolling. A retained full-document
  surface cache was tried and reverted (re-rastered the whole page on
  every change); smooth wheel scrolling was reverted for the same reason
  (full repaint × ~15 ease-frames saturated CPU). Both need dirty rects.
- **14 · YouTube watch-page playback** — baseline shipped: a streaming
  `<video>` (MSE/`blob:`, no file URL) plays *inline* — the renderer
  materializes the growing stream and the `nordstjernen-video` helper
  (`src/videoproc/main.c`, built when libav is present) decodes frames
  into a shm ring the shell composites over the page (see
  `docs/media.md`). Next: widen codec/site coverage and surface clearer
  in-page feedback when the helper is unavailable.
- **8 · Sign the Windows build** — biggest distribution-side ROI. Wire
  signtool + timestamp now; flip on once a cert is procured.

**Later**
- 5 · Reader mode · 6 · APNG playback · 9 · macOS notarized DMG.

## Future focus areas

Candidate directions once the performance work above lands — none
committed, listed to keep the long view in one place:

- **Layout & paint throughput** — the dirty-region partial repaint and
  incremental re-cascade in *Now/Next* are the headline wins; both
  unblock smooth animated scrolling, which has been reverted twice for
  want of them. Treat them as the gating performance work.
- **Frame-loop idling / animation throttling** *(partially landed)* —
  quiet-page idling shipped: the renderer reports `X-Anim` from
  `ns_browser_animating`, the shell stops requesting frames once a page
  is quiet (6a519a6, 6d598fc), and a tick that ran no work reuses the
  previous frame (`X-Unchanged`, staged-surface reuse in the shell) —
  safe because it skips only when *nothing* ran, unlike the reverted
  coarse `frame_dirty` flag that skipped after no-op rAFs and caused
  judder; don't re-attempt that version. The reflow-loop dampener now
  *defers* suppressed relayouts instead of discarding them (51659ba) —
  pending JS work must always count as animating or the page freezes.
  What remains is the rAF-pinned SPA case (the full `duckduckgo.com`
  spinner still burns CPU because its rAF genuinely runs every frame):
  that needs per-region paint invalidation (the dirty-region work above)
  and, for transform/opacity spinners, compositor-thread animation — a
  larger architectural effort. The re-entrant-relayout leak was a
  distinct bug, fixed in 80ca0d5.
- **Worker maturity** — dedicated workers are partial today (§10). Round
  out `postMessage` structured clone, transferables, and module workers
  so wasm-bindgen + worker bundles run unmodified.
- **`iframe` rendering** — `sandbox` is parsed and enforced, but nested
  document rendering is still limited. Real child-document layout/paint
  would close one of the larger remaining HTML gaps.
- **Image animation** — APNG/animated-WebP playback (GIF animation
  already works through the Wuffs path).
- **Accessibility** — no AT-SPI / accessibility tree is exposed yet;
  a minimal accessible-name + role surface would be a high-value,
  self-contained slice.
- **WebGL extensions** — `getExtension()` implements
  `WEBGL_debug_renderer_info` and `EXT_texture_filter_anisotropic` and
  returns `null` for the rest; a small allow-list of further widely-used,
  safe extensions (e.g. instancing, `OES_*` float textures) would
  unblock more content without re-architecting `src/webgl.c`.
- **Packaging reach** — a signed Windows build and a notarized macOS
  DMG are the distribution-side levers.

**Done:** process-per-tab renderers behind the IPC +
shared-memory-framebuffer boundary (the GTK shell is a thin display
client now; the legacy bespoke in-process renderer removed — see
*Architecture & frontends* and `docs/tab-isolation.md`),
10 embeddable `libnordstjernen` (built and header-installed
from meson, see `docs/Embedding.md`; Java JNI binding and Swing app in
`java/`, see `java/README.md`),
15 Debian/Ubuntu `.deb` packaging (built nightly, see `docs/Nightly.md`).

**Ongoing — security hardening passes.** Recurring source audits of the
attacker-reachable surface — network parsing, cookie scoping, layout
allocation, the renderer/IPC boundary — with each fix landed as its own
commit (the running detail lives in the git history). The fundamentals stay
strong: http/https-only scheme allow-listing with no HTTPS→HTTP redirect
downgrade, public-suffix cookie scoping via libpsl, CRLF-filtered request
headers, credentials never replayed across a redirect host
(`CURLOPT_UNRESTRICTED_AUTH=0`), clamped/overflow-checked sizes on the
untrusted IPC and layout paths, depth-limited recursive CSS parsers, and
per-renderer Landlock + seccomp confinement applied before any page opens.

**Mostly done / partial:** 12 downloads under Landlock (path 2),
13 fragment navigation, 16 silent-stub honesty, 17 GTK renderer
selection.

## How we work

meson + ninja, with `ccache` for fast rebuilds. Build and smoke-launch
locally before pushing — the local machine is the build and run oracle.
Commit small; push logical units to `origin/main` as they land.
