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
- **`position: sticky`.** `paint_walk` in `src/paint.c` now shifts a
  sticky-positioned box during paint based on the current vertical
  scroll position of the nearest scroll container, capped inside the
  containing block's content box. Sticky table headers and
  Wikipedia infobox toolbars pin as expected.
- **Geometry-aware `IntersectionObserver`.** The QuickJS-side
  observer in `src/js.c` now consults the live layout tree on every
  rAF tick and dispatches `intersectionEntry` records only when a
  target's viewport rect actually crosses the configured threshold,
  instead of firing once at `observe()` time. Lazy-load gates on
  long pages now trigger at the right moment.
- **Subresource Integrity (`integrity="…"`).** External `<script>`
  loads in `src/js.c::nd_js_walk_scripts` and `<link
  rel=stylesheet>` loads in `src/main.c`'s CSS fetch path now hash
  the fetched bytes with `nd_security_sri_check` (`src/security.c`),
  compare against the strongest `sha256-` / `sha384-` / `sha512-`
  digest in the `integrity` attribute, and refuse to apply on
  mismatch with a console log line.
- **Brotli + zstd response decoding.** `src/net.c::nd_net_init`
  inspects `curl_version_info()` for `CURL_VERSION_LIBZ`,
  `CURL_VERSION_BROTLI`, and `CURL_VERSION_ZSTD`, builds the
  matching `Accept-Encoding` list, and feeds it to every request via
  `CURLOPT_ACCEPT_ENCODING`. The active list is exposed through
  `nd_net_supported_encodings()` and surfaced in the JS console
  banner so it's verifiable on a real page.
- **`display: table` / `table-row` / `table-cell` fallback layout.**
  `src/layout.c` now treats any element with one of these computed
  displays as the corresponding table box: `style_is_block` accepts
  `table` / `inline-table`, `build_block` routes to `build_table`
  via `is_table_box`, `collect_rows_recurse` recognises rows by
  display (not just `<tr>`), and the cell scanner accepts
  `display: table-cell` boxes alongside `<td>` / `<th>`. A
  `<div>`-based table now lays out like the `<table>`-tag version.

### 1. CSS `transition` + `@keyframes` / `animation`

A clamped, low-fps implementation of property interpolation is a
huge improvement over "property snaps instantly." Parse
`transition-*` and `@keyframes` blocks (the at-rule walker in
`src/css.c::parse_rules_until` already skips them cleanly), keep a
per-document set of active animations driven off the existing
`g_timeout` that already feeds `requestAnimationFrame`, and
re-resolve the affected properties each tick. Cap concurrent
animations and refuse to animate properties that would force a
re-layout under load (transform / opacity only is fine for v0.6).

### 2. CSS Grid: `minmax()`, `auto-fit` / `auto-fill`, `grid-template-areas`

`src/layout.c` already lays out fr / px / % tracks and spans;
`minmax(a, b)` and `repeat(auto-fit, minmax(…))` are the two
constructs every modern card layout uses. `grid-template-areas`
is a small parse-time mapping from area-name strings to track
indices. With these three, the default-mode (light) home pages of
most news / docs sites lay out correctly instead of single-column
fallback.

### 3. `overflow: auto` / `overflow: scroll` scroll containers

Today an `overflow: auto` box paints its overflow regardless of
the property, which makes code blocks, sidebars, and modal bodies
spill over the page. Clip at the box's padding edge during paint,
add a scroll offset field to `nd_box`, and route mouse-wheel /
touch-scroll events to the topmost scrollable ancestor of the hit
box. Unblocks code-heavy pages (Stack Overflow answers, blog
syntax-highlighter blocks).

### 4. Web fonts via `@font-face`

`src/css.c::parse_rules_until` already extracts `font-family` and
`src` from `@font-face`, but the URL is never fetched and the font
never registered with Pango. Wire it: fetch through the existing
image-cache path, drop the bytes in a temp file, register with
Pango via `pango_fc_font_map_*` (Linux/macOS) or by writing to a
private `FONTCONFIG_PATH` dir. Falls back to the family stack on
fetch failure or unsupported format. Most "wrong font" bug reports
collapse to this.

### 5. CSS `:is()`, `:where()`, `:has()`

`:is()` / `:where()` are mechanical — desugar to the cross product
during selector matching, with `:where()` forcing specificity to
zero. `:has()` is harder (needs reverse matching) but the
ultra-common `:has(> svg)` / `:has(+ *)` shape can be handled with
a bounded forward scan. Ship `:is` / `:where` in v0.6 and gate
`:has` behind `ND_CSS_ENGINE=lexbor` if our own engine isn't ready.

### 6. `Range` / `Selection` API completion

`src/selection.c` already tracks the on-screen text selection; the
JS-side Range / Selection objects expose only stubs. Wire
`getSelection().toString()`, `Range.cloneContents`,
`Range.getBoundingClientRect`, and `selectionchange` to the real
selection. Unblocks copy-to-clipboard logic on heavy webapps and
quote-pickers on news sites.

### 7. Print preview + paginated print path

The Print menu item already opens a `GtkPrintOperation`
(`src/main.c::nd_on_print_begin`) but draws a single full-page
bitmap, so multi-page articles get cropped. Replace the
single-page draw with a real paginator: re-layout into the print
context's page size, split block flow at page boundaries, hand
each page to `GtkPrintContext` separately. Same machinery
benefits headless `--dump=pdf:` for long pages.

### 8. Save Page As HTML…

Sibling to "Save Page As PDF…" (`win.save-pdf` in
`src/main.c`). Writes the served bytes verbatim when the DOM is
unmutated; otherwise serialises the live DOM via the existing
`nd_dom_serialize` path. Routes through the file portal. One day
of work, well-known affordance, asked-for repeatedly.

### 9. Reading mode / reader view

A toggle that strips nav / aside / footer / form / script /
hidden elements and re-renders body content in a single column
with our existing typography defaults. Heuristic borrowed from
arc90 / Mozilla Readability is fine — measure text density per
block, keep the densest contiguous subtree. Big visible win on
ad-heavy news sites that we already render fine but uglyly.

### 10. Bookmarks panel UI

Bookmarks already persist to `bookmarks.txt`
(`src/bookmarks.c`) and right-click adds entries; what's missing
is a way to *see* them. Add a sidebar / popover triggered from the
toolbar showing title + URL, with Open / Open-in-new-window /
Delete actions. No folder hierarchy in v0.6 — flat list, sort by
add time. Folder support deferred until someone asks.

### 11. Animated GIF + APNG playback

`src/image.c` decodes the first frame only. Wuffs exposes a
frame-by-frame GIF API; APNG support means walking the
`fcTL`/`fdAT` chunks ourselves. Drive playback off a per-image
`g_timeout` tied to the per-frame delay, paint into the same
surface slot. Many emoji / reaction images and small avatars
depend on this.

### 12. macOS + Windows audio output

`src/audio.c` is PulseAudio-only today. Add a CoreAudio backend
(`AudioQueue` is the smallest dependency-free path) and a WASAPI
backend (`IAudioClient` shared-mode). Same `nd_audio_*` interface,
no QuickJS bindings to touch. Without this, `<video>` plays
silent on the two platforms where we want to be respectable.

### 13. Find-in-page polish

`Ctrl+F` already opens the bar (`src/main.c::on_win_find`) and
advances to the next match on `Enter`. Missing: shift-Enter for
previous match, a live "N of M" counter, ESC to clear with
keyboard focus returned to the page, case-sensitive toggle,
highlight all matches in a dimmed colour. Half a day, fixes a
daily-driver papercut.

### 14. Sign the Windows installer

`scripts/pack-windows-installer.sh` produces a working NSIS
package; the missing piece is Authenticode signing. Unsigned
binaries get a SmartScreen "unknown publisher" warning that scares
off most Windows users on first run. Buy an EV/OV cert, wire
`signtool` into the script, document the secret-handling path.
Single biggest distribution-side ROI; carries over from v0.5.

### 15. macOS notarized DMG

The Homebrew build works (see `docs/macOS.md`). Wrap the bundle in
a notarized `.dmg` so users can drag-and-drop install without
`xattr -d com.apple.quarantine`. Requires an Apple Developer ID
and the `notarytool` workflow. Carries over from v0.5.

### 16. Flathub Flatpak

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
