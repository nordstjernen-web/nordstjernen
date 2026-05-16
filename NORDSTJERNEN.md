# Nordstjernen — Development Plan

Living development plan for the clean-room browser. See `README.md`
for the product vision and `CLAUDE.md` for working agreements.

Nordstjernen is a clean-room browser engine written from scratch.
No upstream browser source — Gecko, WebKit, Blink, KHTML, none —
is read, ported, or imported. The aim is to be small enough to be
audited end-to-end by one human, fast enough to feel native, and
honest enough to default to the user's interests.

## Project goals

- **One competent human's worth of code.** Whenever a piece of work
  risks ballooning, cut scope, not corners. A working subset beats
  an unfinished superset.
- **Vertical slices that ship.** Every task in the list below ends
  with something that can be run and demoed end-to-end, not a
  library waiting for a caller.
- **No third-party engines we haven't read.** Vendor dependencies
  must be small, embeddable, and auditable. Today: lexbor (HTML +
  WHATWG URL), QuickJS (JS), Wuffs (image decode).
- **No automated test suite.** Resources don't allow it. Verify by
  running the browser; code that's hard to verify by manual exercise
  should be redesigned, not test-covered.
- **No code comments.** Each source file gets one short header
  comment naming it; no inline comments, no section banners, no
  TODOs. See `CLAUDE.md` for the full rule.
- **All JavaScript bindings live in `src/js.c`.** One file is easy
  to grep, easy to skim, and easy to keep a single mental model of
  how QuickJS values map to our DOM.

### Positioning: a good, simple, fast web browser.

Nordstjernen is built on GTK 4 and targets Linux, Windows and Mac.

This goal is explicit because every other choice flows from it:

- GTK 4 native, never an Electron / CEF / WebKitGTK shell.
- libadwaita widgets where they don't compromise minimalism.
  Adaptive layouts so the same binary runs on phone, tablet, and
  desktop.
- Flathub as the primary distribution channel; distro packages
  follow, not lead.
- xdg-desktop-portal first for file pickers, screenshots,
  notifications, secret storage.
- Track GNOME release cadence; match the GTK / libadwaita ABIs of
  the current and previous GNOME release.

The engine itself stays clean-room — this positioning is about
*how the app behaves*, not about embedding someone else's renderer.

## Improvement tasks for v0.6.0, sorted by ROI

ROI = visible user benefit divided by implementation cost. The list
is reordered whenever priorities shift; do the top items first.
Version `src/version.h` is `0.6.0-dev`; the items below are what
needs to land before we bump it to `0.6.0`.

### Done in this cycle

- **CSS custom properties (`var()`) with real cascade.**
  `src/css.c` now captures `--name: …` declarations into a per-rule
  vars hash and `var()`-bearing declarations into a per-rule pending
  list; `cascade_walk` builds a per-element vars map (inherited from
  the parent style, then overridden by matched rules in cascade
  order) and re-parses each pending declaration against that map.
  Inheritance, fallbacks, and chained refs (`--a: var(--b)`) all
  resolve through the same path.

### 1. `position: sticky`

Already partly wired (`src/layout.c:3018` recognises the keyword
in the box-position helper) but never actually pins anything to
the scroll container — sticky headers / table headers / footers
just behave like `position: static`. Wikipedia infoboxes, GitHub
file headers, every modern docs site uses this. Implement by
tracking the nearest scroll container in layout and offsetting the
sticky box during paint based on the current scroll position; cap
inside the containing block's content box.

### 2. CSS `transition` + `@keyframes` / `animation`

A clamped, low-fps implementation of property interpolation is a
huge improvement over "property snaps instantly." Parse
`transition-*` and `@keyframes` blocks (the at-rule walker in
`src/css.c::parse_rules_until` already skips them cleanly), keep a
per-document set of active animations driven off the existing
`g_timeout` that already feeds `requestAnimationFrame`, and
re-resolve the affected properties each tick. Cap concurrent
animations and refuse to animate properties that would force a
re-layout under load (transform / opacity only is fine for v0.6).

### 3. Real `IntersectionObserver` (geometry-aware)

The current implementation
(`src/js.c::nd_intersection_observer_observe`) fires once per
`observe()` call with `isIntersecting: true` regardless of where
the element actually is. Lazy-loading sites that gate
`<img>` / `<iframe>` swaps on the callback work by accident on
short pages and silently break on long ones. Hook the layout pass
so each observed target reports its actual viewport rect and we
emit entries when crossings happen, not on registration.

### 4. Brotli + zstd response decoding

`libcurl` already negotiates them when the build links against
`libbrotlidec` / `libzstd` — we just don't link them. Several
high-traffic sites now send `br`-only responses and we silently
fall back to identity, doubling the bytes transferred. Add the two
optional dependencies to `meson.build`, enable
`CURLOPT_ACCEPT_ENCODING` with the full list, and surface the
active set in the JavaScript console banner so we can verify on
a real page.

### 5. CSS Grid: `minmax()`, `auto-fit` / `auto-fill`, `grid-template-areas`

`src/layout.c` already lays out fr / px / % tracks and spans;
`minmax(a, b)` and `repeat(auto-fit, minmax(…))` are the two
constructs every modern card layout uses. `grid-template-areas`
is a small parse-time mapping from area-name strings to track
indices. With these three, the default-mode (light) home pages of
most news / docs sites lay out correctly instead of single-column
fallback.

### 6. `display: table` / `table-row` / `table-cell` fallback layout

Wikipedia and HN both render through tables, but only `<table>`
elements get the table-layout path — `display: table` on a
`<div>` (used by some CMS-generated articles) falls through to
block layout. Reuse the existing table-layout entrypoint when the
computed `display` says so. Cheap, removes a recurring "looks
broken" failure mode.

### 7. `overflow: auto` / `overflow: scroll` scroll containers

Today an `overflow: auto` box paints its overflow regardless of
the property, which makes code blocks, sidebars, and modal bodies
spill over the page. Clip at the box's padding edge during paint,
add a scroll offset field to `nd_box`, and route mouse-wheel /
touch-scroll events to the topmost scrollable ancestor of the hit
box. Unblocks code-heavy pages (Stack Overflow answers, blog
syntax-highlighter blocks).

### 8. Web fonts via `@font-face`

`src/css.c::parse_rules_until` already extracts `font-family` and
`src` from `@font-face`, but the URL is never fetched and the font
never registered with Pango. Wire it: fetch through the existing
image-cache path, drop the bytes in a temp file, register with
Pango via `pango_fc_font_map_*` (Linux/macOS) or by writing to a
private `FONTCONFIG_PATH` dir. Falls back to the family stack on
fetch failure or unsupported format. Most "wrong font" bug reports
collapse to this.

### 9. CSS `:is()`, `:where()`, `:has()`

`:is()` / `:where()` are mechanical — desugar to the cross product
during selector matching, with `:where()` forcing specificity to
zero. `:has()` is harder (needs reverse matching) but the
ultra-common `:has(> svg)` / `:has(+ *)` shape can be handled with
a bounded forward scan. Ship `:is` / `:where` in v0.6 and gate
`:has` behind `ND_CSS_ENGINE=lexbor` if our own engine isn't ready.

### 10. `<details>` / `<summary>` toggle behaviour

The elements parse fine but never collapse — `open` attribute is
ignored, click does nothing. Add a layout flag that hides children
other than `<summary>` when `open` is absent, toggle the attribute
on `<summary>` click, fire a `toggle` event. ~50 lines, fixes
GitHub README expandable sections, MDN spec collapsibles, and any
disclosure widget on the web.

### 11. `Range` / `Selection` API completion

`src/selection.c` already tracks the on-screen text selection; the
JS-side Range / Selection objects expose only stubs. Wire
`getSelection().toString()`, `Range.cloneContents`,
`Range.getBoundingClientRect`, and `selectionchange` to the real
selection. Unblocks copy-to-clipboard logic on heavy webapps and
quote-pickers on news sites.

### 12. Print preview + paginated print path

The Print menu item already opens a `GtkPrintOperation`
(`src/main.c::nd_on_print_begin`) but draws a single full-page
bitmap, so multi-page articles get cropped. Replace the
single-page draw with a real paginator: re-layout into the print
context's page size, split block flow at page boundaries, hand
each page to `GtkPrintContext` separately. Same machinery
benefits headless `--dump=pdf:` for long pages.

### 13. Save Page As HTML…

Sibling to "Save Page As PDF…" (`win.save-pdf` in
`src/main.c`). Writes the served bytes verbatim when the DOM is
unmutated; otherwise serialises the live DOM via the existing
`nd_dom_serialize` path. Routes through the file portal. One day
of work, well-known affordance, asked-for repeatedly.

### 14. Reading mode / reader view

A toggle that strips nav / aside / footer / form / script /
hidden elements and re-renders body content in a single column
with our existing typography defaults. Heuristic borrowed from
arc90 / Mozilla Readability is fine — measure text density per
block, keep the densest contiguous subtree. Big visible win on
ad-heavy news sites that we already render fine but uglyly.

### 15. Bookmarks panel UI

Bookmarks already persist to `bookmarks.txt`
(`src/bookmarks.c`) and right-click adds entries; what's missing
is a way to *see* them. Add a sidebar / popover triggered from the
toolbar showing title + URL, with Open / Open-in-new-window /
Delete actions. No folder hierarchy in v0.6 — flat list, sort by
add time. Folder support deferred until someone asks.

### 16. Animated GIF + APNG playback

`src/image.c` decodes the first frame only. Wuffs exposes a
frame-by-frame GIF API; APNG support means walking the
`fcTL`/`fdAT` chunks ourselves. Drive playback off a per-image
`g_timeout` tied to the per-frame delay, paint into the same
surface slot. Many emoji / reaction images and small avatars
depend on this.

### 17. macOS + Windows audio output

`src/audio.c` is PulseAudio-only today. Add a CoreAudio backend
(`AudioQueue` is the smallest dependency-free path) and a WASAPI
backend (`IAudioClient` shared-mode). Same `nd_audio_*` interface,
no QuickJS bindings to touch. Without this, `<video>` plays
silent on the two platforms where we want to be respectable.

### 18. Find-in-page polish

`Ctrl+F` already opens the bar (`src/main.c::on_win_find`) and
advances to the next match on `Enter`. Missing: shift-Enter for
previous match, a live "N of M" counter, ESC to clear with
keyboard focus returned to the page, case-sensitive toggle,
highlight all matches in a dimmed colour. Half a day, fixes a
daily-driver papercut.

### 19. Subresource Integrity (`integrity="…"`)

`<script>` and `<link rel=stylesheet>` ignore the `integrity`
attribute today. Hash the fetched bytes with the existing
glib `GChecksum`, compare against the parsed `sha256-…` /
`sha384-…` / `sha512-…` value, refuse to apply on mismatch with a
console log line. Small, self-contained, real defence-in-depth
on the JS / CSS pipeline.

### 20. Sign the Windows installer

`scripts/pack-windows-installer.sh` produces a working NSIS
package; the missing piece is Authenticode signing. Unsigned
binaries get a SmartScreen "unknown publisher" warning that scares
off most Windows users on first run. Buy an EV/OV cert, wire
`signtool` into the script, document the secret-handling path.
Single biggest distribution-side ROI; carries over from v0.5.

### 21. macOS notarized DMG

The Homebrew build works (see `docs/macOS.md`). Wrap the bundle in
a notarized `.dmg` so users can drag-and-drop install without
`xattr -d com.apple.quarantine`. Requires an Apple Developer ID
and the `notarytool` workflow. Carries over from v0.5.

### 22. Flathub Flatpak

A reviewed, reproducible Flatpak is the canonical install path for
the GNOME-aligned positioning above. Write the manifest, get it
into `flathub/flathub`, add a "Get it on Flathub" badge to the
README. Once it's there, the GNOME Circle submission becomes
feasible. Carries over from v0.5.

## Non-goals (won't change)

- **No WebGL, WebGPU, WebRTC, WebUSB, WebBluetooth, WebHID,
  WebMIDI.** The exploit-prolific surface area of modern browsers
  is foreclosed by design.
- **No service workers, push notifications, background sync.**
- **No DRM / EME / Widevine.**
- **No JIT.** QuickJS is a bytecode interpreter; W^X holds
  process-wide.
- **No tab strip.** One page per top-level window; the OS window
  manager / taskbar / Mission Control / Alt-Tab is the affordance
  for moving between open windows.
- **No plugins.** No NPAPI, no PPAPI, no WebExtensions, no Flash,
  no shims. The browser ships exactly what's in this repo and never
  executes third-party native code on the user's behalf.
- **No persistent browsing history.** The session back/forward
  stack lives only in memory; nothing about a visit is written to
  disk. Rationale: annoying to manage, privacy footgun.
- **No sync, no accounts, no "studies", no telemetry of any kind.**
- **No localization beyond English** for now.
