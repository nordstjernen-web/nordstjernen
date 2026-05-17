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

- **Wikipedia rendering pass: floated `max-width` and inline
  `text-align: justify`.** `layout_block`'s float-sizing branch
  (`src/layout.c`) now consults `max-width` and `min-width` on the
  float before placing it, so Wikipedia's
  `table.infobox { max-width: 320px; float: right; }` actually
  caps at 320 px instead of stretching to the 60%-of-container
  fallback that the natural-width branch would otherwise give it.
  Body text that flows next to the infobox correspondingly gets
  the remaining width. `src/paint.c::apply_text_align` now
  recognises `justify` and toggles
  `pango_layout_set_justify(layout, TRUE)` for the inline run, so
  the `p { text-align: justify; }` that Wikipedia inherits from
  the compatibility sheet stops being a no-op and the body
  paragraphs flush both edges.
- **Windows audio output (WinMM).** `src/audio.c`'s playback path
  is now backend-pluggable behind a small `nd_audio_sink` interface
  (open / write / drain / close). The Linux side still uses
  `pa_simple` under `ND_AUDIO_PULSE`; a new `ND_AUDIO_WINMM`
  backend uses Win32 `waveOut*` with a 4-buffer ring keyed off a
  `WOM_DONE` callback so the WebM/Opus decode loop blocks only when
  every output buffer is in flight. `meson.build` autodetects
  `winmm` on `host_machine.system() == 'windows'`, sets
  `ND_HAVE_AUDIO` plus the right backend define, and `src/meson.build`
  pulls in `libwinmm` only when needed. `<video>` and `<audio>`
  tags that previously played silent on Windows now produce sound
  using the same Opus decoder.
- **Animated GIF playback.** New `nd_image_decode_wuffs_anim` in
  `src/image_wuffs.c` walks the GIF frame-config / frame-decode
  loop, snapshots the composited pixel buffer after each
  `decode_frame`, and emits a `GArray` of `nd_image_anim_frame`
  (texture + flicks-to-ms delay). `nd_image` now carries
  `anim_frames` / `anim_start_us` / `anim_current` /
  `anim_total_ms`; `src/image.c::on_image_fetched` routes
  `image/gif` bytes through the animation decoder first and only
  falls back to the single-frame path when there's one frame.
  A new `nd_image_cache_tick` advances every animated image's
  current frame based on monotonic time and is called every tick
  from `nd_window_raf_tick`, which queues a redraw when any frame
  changed. Emoji / reaction GIFs and small avatars now animate.
  APNG is still pending — wuffs PNG doesn't expose `fcTL` /
  `fdAT` walking, so that's a separate piece.
- **`Range` / `Selection` API completion.** `nd_js` now carries a
  `selection_text` + `selection_has_range` + bounding-rect snapshot
  pushed in via the new `nd_js_set_selection`; `main.c::nd_window_sync_selection_to_js`
  collects the current text via `nd_selection_collect_text` and the
  bounding box via the new `nd_selection_bounds` helper
  (`src/selection.c`) on every selection change — drag begin /
  update / end, Ctrl+A select-all, click-to-clear. The JS
  `getSelection()` now returns an object whose `toString()` /
  `isCollapsed` / `rangeCount` / `type` reflect the real selection,
  and `getRangeAt(0)` returns a real `Range` with `toString`,
  `cloneContents`, `getBoundingClientRect`, and `getClientRects`.
  `selectionchange` fires on `document` whenever the selected text
  changes. Copy-to-clipboard logic on heavy webapps and
  quote-pickers on news sites stop seeing empty strings.
- **Bookmarks panel UI.** The `user-bookmarks` toolbar button now
  opens a wider, scrollable popover (max ~420px tall) headed
  "Bookmarks" with one row per entry. Each row is a horizontal box
  containing a flat title + URL button that loads the bookmark in
  the current window, a `window-new-symbolic` button that calls
  `nd_spawn_window` to open a fresh top-level for the URL, and a
  `user-trash-symbolic` button that removes the entry via
  `nd_bookmarks_remove` and pulls the row out of the list in place.
  Empty state ("No bookmarks yet — star a page to add one.") stays
  the same. No folder hierarchy in v0.6.
- **CSS Grid `grid-template-areas` + `grid-area: name`.** New
  `ND_CSS_V_AREAS` value kind in `src/css.h` carries a list of
  `nd_css_area_rect`s (name + 0-based row / column ranges) parsed
  from the quoted-string rows by `src/css.c::parse_areas`; the
  parser folds same-name cells into bounding rectangles and skips
  `.` null cells. `src/layout.c::layout_grid` now dispatches to a
  new `layout_grid_areas` whenever the container has a non-empty
  areas template: column sizes still come from
  `grid-template-columns` (or a default `1fr` per column from the
  template), each child's `grid-area: name` resolves to the named
  rect via `find_area_rect`, items without a matching name fall
  back to `grid-row` / `grid-column` line numbers or auto-flow
  into empty cells, then row heights are computed from item
  content (clamped up by any explicit `grid-template-rows` track)
  and items are positioned at their resolved (row, column) cell.
  `<div grid-area="header">` now lands in the named area
  regardless of source order, so the classic "header / sidebar /
  main / aside / footer" layouts render correctly.
- **CSS Grid `minmax()` + `repeat(auto-fit, …)` / `repeat(auto-fill, …)`.**
  `nd_css_track` (in `src/css.h`) now carries an optional minimum
  alongside its primary size, and `nd_css_tracks` records whether
  the track list contains an auto-repeat pattern.
  `src/css.c::parse_tracks` is paren-aware (so `minmax(150px, 1fr)`
  survives whitespace inside the parens), `parse_one_track` handles
  the new `minmax(a, b)` token, and the `repeat(...)` branch
  recognises `auto-fit` / `auto-fill` as well as integer counts.
  `src/layout.c::expand_auto_repeat` materialises the auto-repeat
  pattern at layout time by counting how many copies of the
  minimum-track-width plus gap fit inside the container, and
  `resolve_track_sizes` clamps each track's resolved size to its
  `minmax` minimum. Default-mode card layouts of the form
  `repeat(auto-fit, minmax(150px, 1fr))` now actually reflow at
  different viewport widths instead of falling back to a single
  column. `grid-template-areas` is still pending.
- **`overflow: hidden` / `clip` / `auto` / `scroll` clip at the
  padding edge.** `paint_walk` in `src/paint.c` now reads
  `ND_CSS_OVERFLOW` on every block / table-cell box and, when set to
  one of the clipping values, pushes a `cairo_clip` rect at the
  box's padding edge before descending into children and restores
  after. `layout_block` in `src/layout.c` complements this by
  honouring an explicit `height` (instead of stretching to fit
  children) whenever the same overflow keyword is in effect, so a
  `<pre style="height:60px;overflow:auto">` actually stops at 60px
  and code-block / sidebar / modal-body content no longer bleeds
  across the page. The full scrolling story — per-box scroll
  offset, scrollbars, mouse-wheel / touch routing to the topmost
  scrollable ancestor — is still to come; for now the page-level
  `GtkScrolledWindow` continues to be the only scrollable surface.

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
- **CSS `:is()` and `:where()`.** The selector parser in
  `src/css.c::parse_one_selector` recognises both functional pseudo-
  classes, splits the argument on top-level commas via
  `parse_selector_group`, and attaches each parsed selector list to
  the enclosing simple selector as a `matches_any` group.
  `match_simple` then requires at least one sub-selector per group
  to match the element via the existing `match_selector` path.
  Specificity follows the spec: `:is()` adds the max specificity of
  its arguments, `:where()` adds zero.
- **CSS `:has()` (single-compound relative selectors).** The same
  `parse_selector_group` path now feeds `:has(...)` arguments into a
  new `has_groups` field on the simple selector. `match_simple`
  consults `has_relative_matches` in `src/css.c`, which does a
  bounded forward scan from the anchor element based on the
  relative selector's leading combinator: descendant
  (`:has(.foo)`) recurses through descendants, child (`:has(> svg)`)
  walks direct children only, adjacent (`:has(+ p)`) checks the
  next element sibling, and subsequent-sibling (`:has(~ .end)`)
  walks forward siblings. Specificity adds the max of the
  arguments' specificities, like `:is()`. Multi-compound relative
  selectors are intentionally left out and silently never match —
  the proper reverse-matching path can land in a later cycle.
- **Save Page As HTML…** New `win.save-html` action and Page-menu
  entry. `on_win_save_html` opens a `GtkFileDialog`; on confirm,
  writes the served bytes verbatim when `dom_mutated` is false (set
  by `nd_window_js_mutated`, cleared on each navigation), otherwise
  serialises the live DOM via the existing `nd_node_outer_html`
  path. Routes the bytes through `g_file_set_contents`.
- **Find-in-page polish.** The find bar now has Shift-Enter for the
  previous match (via a key controller on the entry), an "N of M"
  counter driven by the new `nd_box_match_ordinal` helper, Esc to
  dismiss with focus returned to the page (via GtkSearchEntry's
  `stop-search` signal), an `Aa` toggle for case-sensitive matching
  threaded through `count_matches_in_text` and `find_ci_substring`,
  and a dimmed highlight colour for non-active matches in
  `paint_inline` (controlled by `nd_paint_set_search`). The active-
  match box pointer is cleared on every layout rebuild.

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

### 2. Per-box scrolling for `overflow: auto` / `scroll`

The clipping piece of overflow is now in place (see "Done in this
cycle"), so code blocks, sidebars, and modal bodies stay inside
their padding edges. What remains is the actual scrolling: add a
scroll offset field to `nd_box`, paint children at the offset, and
route mouse-wheel / touch-scroll events to the topmost scrollable
ancestor of the hit box. Today the page-level `GtkScrolledWindow`
is still the only thing the wheel talks to.

### 3. Web fonts via `@font-face`

`src/css.c::parse_rules_until` already extracts `font-family` and
`src` from `@font-face`, but the URL is never fetched and the font
never registered with Pango. Wire it: fetch through the existing
image-cache path, drop the bytes in a temp file, register with
Pango via `pango_fc_font_map_*` (Linux/macOS) or by writing to a
private `FONTCONFIG_PATH` dir. Falls back to the family stack on
fetch failure or unsupported format. Most "wrong font" bug reports
collapse to this.

### 4. Print preview + paginated print path

The Print menu item already opens a `GtkPrintOperation`
(`src/main.c::nd_on_print_begin`) but draws a single full-page
bitmap, so multi-page articles get cropped. Replace the
single-page draw with a real paginator: re-layout into the print
context's page size, split block flow at page boundaries, hand
each page to `GtkPrintContext` separately. Same machinery
benefits headless `--dump=pdf:` for long pages.

### 5. Reading mode / reader view

A toggle that strips nav / aside / footer / form / script /
hidden elements and re-renders body content in a single column
with our existing typography defaults. Heuristic borrowed from
arc90 / Mozilla Readability is fine — measure text density per
block, keep the densest contiguous subtree. Big visible win on
ad-heavy news sites that we already render fine but uglyly.

### 6. APNG playback

Animated GIF is done (see "Done in this cycle"); APNG support
still needs walking the `fcTL` / `fdAT` chunks ourselves on top of
the existing PNG decoder. Drive playback off the same
`nd_image_cache_tick` path that animated GIFs already use.

### 7. macOS audio output

Linux PulseAudio and Windows WinMM backends now plug into the
shared `nd_audio_sink` interface. Still pending: a macOS
`AudioQueue` backend (smallest dependency-free path on Darwin).
Same interface, no QuickJS bindings to touch. Without this,
`<video>` plays silent on macOS.

### 8. Sign the Windows installer

`scripts/pack-windows-installer.sh` produces a working NSIS
package; the missing piece is Authenticode signing. Unsigned
binaries get a SmartScreen "unknown publisher" warning that scares
off most Windows users on first run. Buy an EV/OV cert, wire
`signtool` into the script, document the secret-handling path.
Single biggest distribution-side ROI; carries over from v0.5.

### 9. macOS notarized DMG

The Homebrew build works (see `docs/macOS.md`). Wrap the bundle in
a notarized `.dmg` so users can drag-and-drop install without
`xattr -d com.apple.quarantine`. Requires an Apple Developer ID
and the `notarytool` workflow. Carries over from v0.5.

### 10. Embeddable engine library (`libnordstjernen`)

Today the engine and the GTK chrome are entangled in `src/main.c`
(~5.7 kLOC) and `src/window.c`. Carve a clean split so `src/meson.build`
builds two artefacts from the same tree: the existing `nordstjernen`
desktop app and a new `libnordstjernen.{a,so}` exposing a small
stable C header (`include/nordstjernen.h`, ~20 entry points: create
context, load URL, run JS, query DOM, render to PNG / PDF, headless
navigate, attach to a host-supplied GTK / Cairo surface). The
chrome stays in `main.c`; everything below it — net / cache / css /
layout / paint / js / security — is the library. No GTK in the
public header; the host either supplies a draw surface or asks the
library to rasterise. Unlocks the actual buyer pool for a 40 kLOC
auditable engine: kiosks, e-readers, set-top boxes, automotive
HMIs, regulated / air-gapped enterprise viewers, security-research
sandboxes — none of whom want a desktop browser, all of whom are
underserved since WebKitGTK got heavy. Changes the product
category from "niche desktop browser" to "hardened embeddable web
view"; the desktop app then becomes the reference embedder.

### 11. Flathub Flatpak

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
