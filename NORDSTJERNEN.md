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

### Positioning: a first-class GNOME / GTK browser

Nordstjernen is built on GTK 4 and targets Linux first. Its
long-term positioning is to be **the small, native, auditable
GNOME-aligned browser** — one that follows the GNOME HIG, ships
through Flathub, integrates with GVFS / GSettings / libsecret /
portals, and one day appears in the GNOME Circle (or its successor
showcase for third-party GNOME applications).

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

## Improvement tasks, sorted by ROI

ROI = visible user benefit divided by implementation cost. The list
is reordered whenever priorities shift; do the top items first.

### 1. Fix the Hacker News upvote-arrow column

HN's CSS sizes each `<div class="votearrow">` to 10×10 px and
supplies the glyph via `background-image: url("grayarrow.svg")`.
The renderer ignores `width` / `height` / `background-image` on
non-replaced blocks, so the column reserves its full table-width
share and appears as a wide empty cell next to every story title.
HN is a primary correctness anchor; this is the single most
visible rendering regression.

### 2. Sign the Windows installer

`scripts/pack-windows-installer.sh` already produces a working NSIS
package; the only missing piece is Authenticode signing. Unsigned
binaries get a SmartScreen "unknown publisher" warning that scares
off most Windows users on first run. Buy an EV or OV code-signing
certificate, wire `signtool` into the script, and document the
secret-handling path. Single biggest distribution-side ROI.

### 3. Ship a Flathub Flatpak

A reviewed, reproducible Flatpak is the canonical install path for
the GNOME positioning above. Write the manifest, get it into
flathub/flathub, and add a "Get it on Flathub" badge to the README.
Once it's there, the GNOME Circle submission becomes feasible.

### 4. macOS notarized DMG

The Homebrew build already works (see `docs/macOS.md`). Wrap the
bundle in a notarized `.dmg` so users can drag-and-drop install
without `xattr -d com.apple.quarantine`. Requires an Apple
Developer ID and the `notarytool` workflow; same one-time cost
shape as Windows Authenticode.

### 5. Responsive UA stylesheet + `@media (max-width: …)`

The layout engine already takes a viewport width; add `@media
(max-width)` parsing (most of the machinery is in `src/css.c`
already for `prefers-color-scheme`) and ship a UA stylesheet
variant for narrow viewports — larger tap targets, single-column
flow, hidden chrome. Verify on a 360 px-wide headless screenshot.
Cheap, large visible win on a phone-sized window.

### 6. CSS transitions and `@keyframes` (paint-time)

Many sites *look* broken without `transition: opacity 0.2s` etc.
Even a clamped, low-fps implementation is a huge improvement over
"property snaps instantly." Drive frames off a single
`g_timeout_add` tied to the affected box list; cap concurrent
animations.

### 7. AppImage for old-distro Linux

For the "works on any 64-bit Linux including ones too old for
modern GTK 4" case, `linuxdeploy` + `linuxdeploy-plugin-gtk`
bundles the GTK 4 stack alongside our binary. Maybe two days of
work, makes downloads work on every distro forever.

### 8. Finish `<video>` playback

We currently demux WebM and paint the first VP9 frame. Remaining:
playback timer that advances frames on a `g_timeout` / GMainLoop
tied to per-frame timecode, play/pause overlay, status-bar
progress, audio decode (Vorbis or Opus) with PipeWire / CoreAudio
/ WASAPI output. The hard work (demuxer + VP9 decode + YUV→RGBA)
is already done.

### 9. CSS background-image + width/height on non-replaced blocks

Generalises the fix in #1. Painting backgrounds and sized boxes on
arbitrary blocks unlocks roughly half the "looks ugly" reports
from non-HN sites too. Image fetched through the same image cache
as `<img>`, painted in `paint_block` after the background colour
fill.

### 10. Per-window OS process split

Each top-level window already lives in its own GTK window and JS
runtime; the remaining piece is fork+exec on `Ctrl+N` /
`target=_blank` / middle-click so a crash in one window can't take
the whole browser down. Most of the plumbing is already in
`nd_spawn_window`. Medium effort, large robustness win.

### 11. WebSocket cookies + `withCredentials` for XHR / fetch

The cookie jar exists and partitions correctly; XHR and fetch just
don't read or write it. Plug `nd_cookie_jar_*` into
`nd_net_websocket_*` and into `XMLHttpRequest.send` when
`withCredentials` is set. Unblocks a long list of jQuery-using
sites.

### 12. `responseType: 'arraybuffer'` / `'blob'` and binary fetch

Today only `'text'` and `'json'` work; binary responses get
shoehorned through a JS string and corrupt. Wire `ArrayBuffer` and
a minimal `Blob` through QuickJS's typed-array surface. Small,
self-contained, frequently requested.

### 13. Real `MutationObserver`

Currently a no-op shell. Some jQuery plugins rely on it; without
it they look like the page is silently broken. Hook
`nd_dom_mutate_*` to fire microtasks with batched mutation
records. Bounded work, well-understood spec.

### 14. CSP enforcement on inline `eval` and `$.getScript`

`script-src` is honoured at fetch time; `'unsafe-eval'` matching
on `eval` / `new Function` / `Function.prototype.constructor` is
not. Read the parsed `nd_csp` from the JS runtime and reject the
call when the policy doesn't allow it.

### 15. Right-click "Save Page As HTML"

We already do "Save Page As PDF" via `cairo_pdf_surface`. Add a
sibling that writes the original response body (or, if it's been
mutated by JS, the serialized DOM) to a user-chosen path through
the file portal. One-day task, well-known affordance.

### 16. Certificate-pinning toggle (off by default)

Optional `--pin` / config-file knob that records the SPKI of the
server certificate on first connect to a host and refuses to
proceed if it changes. Off by default; opt-in for users who care.
Small surface, real security value.

### 17. GSettings schema for user preferences

Replace the hand-rolled `~/.config/nordstjernen/nordstjernen.conf`
parser with a GSettings schema once we're inside Flatpak. Removes
~150 lines from `src/config.c` and gets us `gsettings get/set`,
`dconf-editor`, and integration with GNOME Settings for free.
Depends on #3.

### 18. libsecret-backed password store (autofill)

There is no password store today. When we add `<input
type="password">` autofill, store credentials via libsecret through
the Secret Service portal — never in our own files. Bounded scope:
read-only autofill first; save-prompt later.

### 19. Android port

Build the same C source against the NDK with a thin
GTK-replacement shim — minimal Wayland-style abstraction for
surface + events, or a hand-rolled NDK backend. Networking stays
libcurl; rendering stays Cairo + Pango. Largest single line item
on this list; lowest near-term ROI; listed so the option stays
visible.

## Test sites — manual verification corpus

Ordered by ascending pain. Tier 0 + Tier 1 lines also live in
`reading-list.txt`; `./scripts/dev.sh smoke` iterates that file
through `nordstjernen --headless --dump=text` and diffs against
committed baselines.

### Tier 0 — must always work

- `https://example.com` — IANA's canonical "is the network
  working?" page.
- `https://lite.cnn.com` — text-only news mirror; sanity-checks
  the HTML parser on real-world markup.
- `https://text.npr.org` — same idea, slightly heavier nav.

### Tier 1 — heavier real-world pages

- `https://en.wikipedia.org/wiki/Trondheim` — primary Wikipedia
  benchmark. Infobox, climate table, demographics, ref-numbered
  superscripts, long flowing body. Any regression here blocks a
  release.
- `https://news.ycombinator.com` — primary correctness anchor.
  Dense table-based layout, user-generated text. Visible regression
  blocks a release. Known gap: see improvement task #1.
- `https://danluu.com` — minimal-CSS blog. Long paragraphs,
  occasional inline `<code>`.
- `https://html.duckduckgo.com/html/?q=nordstjernen` — search
  results page with no JavaScript dependency.

### Tier 2 — forms / cookies

- `https://duckduckgo.com` (home) — submit a query via the search
  form. Round-trips POST + session cookie.
- `https://en.wikipedia.org/wiki/Special:Random` — exercises
  libcurl redirects.

### Tier 3 — JavaScript

- `https://news.ycombinator.com` interactive bits (vote /
  collapse).
- A site that uses `document.querySelector` + small DOM mutations.

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
