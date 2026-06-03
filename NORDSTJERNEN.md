# Nordstjernen — Development Plan

Living plan for a clean-room web browser written from scratch in **C** on
**GTK 4** + **libcurl**, small enough for one person to audit end-to-end.
No upstream engine (Gecko / WebKit / Blink) is read, ported, or imported.
See `README.md` for the product vision and `CLAUDE.md` for working rules.

## Principles

- **One competent human's worth of code** — when work balloons, cut
  scope, not corners. A working subset beats an unfinished superset.
- **Vertical slices that ship** — every task ends in something runnable.
- **Few small auditable deps**, vendored in-tree: lexbor (HTML + WHATWG
  URL), QuickJS (JS interpreter), Wuffs (image decode).
- **No automated test suite** — verify by running the browser.
- No JIT, therefore more secure. However, this means the rest of the browser
  engine needs to be super-fast. 
- **No code comments** beyond one header line per file (see `CLAUDE.md`).

## Non-goals (won't change)

WebGL / WebGPU / WebRTC / WebUSB / WebBluetooth / WebMIDI / WebHID;
service workers, push, background sync; DRM / EME; **JIT** (QuickJS
interpreter only — W^X holds process-wide); plugins (NPAPI / PPAPI /
WebExtensions); persistent on-disk history; sync / accounts / telemetry /
"studies"; localization beyond English (for now).

## Current state

A usable HTML5 + modern-CSS + ~ES2020 browser. lexbor parses to a DOM;
the engine does the CSS cascade and selectors, block/inline/flex/grid/
table/multicol/float/positioned layout, and Cairo + Pango paint
(gradients incl. `repeating-*` and gradient masks, filters, transforms,
transitions / `@keyframes`). QuickJS bindings cover DOM, `fetch`/XHR,
canvas 2D, storage, and the Resize/Intersection/Mutation observers; forms
support submission and constraint validation; `overflow` boxes scroll.
Painting skips off-screen boxes (viewport culling). Runs on Linux,
Windows (MSYS2) and macOS; CI builds all three plus musl on every push.

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
- **14 · YouTube watch-page playback** — baseline shipped: clicking a
  streaming `<video>` (MSE/`blob:`, no file URL) hands the *page* URL to
  the external player, which resolves it with yt-dlp
  (`nd_media_launch_external`, `stream=TRUE`). Next: detect more sites and
  surface a clearer in-page affordance; optionally run the `base.js`
  signature-cipher transform in QuickJS to get a direct URL without
  yt-dlp.
- **8 · Sign the Windows build** — biggest distribution-side ROI. Wire
  signtool + timestamp now; flip on once a cert is procured.
- **11 · Flathub packaging** — canonical Linux install path. Blocked
  only by a missing AppStream `metainfo.xml` and installing
  `nordstjernen.desktop` from meson.

**Later (0.9.0+)**
- 5 · Reader mode · 6 · APNG playback ·
  9 · macOS notarized DMG · 10 · embeddable `libnordstjernen` ·
  15 · Debian/Ubuntu `.deb` packaging.

**Mostly done / partial:** 12 downloads under Landlock (path 2),
13 fragment navigation, 16 silent-stub honesty, 17 GTK renderer
selection.

## How we work

meson + ninja, with `ccache` for fast rebuilds. Build and smoke-launch
locally before pushing — the local machine is the build and run oracle.
Commit small; push logical units to `origin/main` as they land.
