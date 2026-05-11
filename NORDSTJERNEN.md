# Nordstjernen — Development Plan

Live roadmap for the clean-room implementation. See `README.md` for
product vision and `CLAUDE.md` for working agreements.

Nordstjernen is a clean-room reimplementation inspired by Firefox 1.0
(via the `webmosilla` fork) — AI reads that code for orientation only;
no source is copied. The plan is to first match the feature set of
Firefox 1.0, then modernize to a usable browser with HTML5 and a
pragmatic subset of modern JavaScript and CSS.

## Current status

Phases 0–5 are done, plus most of Phase 6 chrome. The browser
fetches HTML / images over HTTP/HTTPS with persistent cookies and
bookmarks, parses HTML, cascades CSS, lays out and paints via
Cairo + Pango, and ships the navigation chrome a desktop browser
needs. See the "Phases 0–5 — Done" block below for the exact
surface.

The next slice is the **per-window OS process split** (Phase 6's
remaining deliverable). After that, **Phase 7 — JavaScript via
QuickJS** is the big chunk; Phase 8 (forms, SameSite cookies,
localStorage) and Phase 9 (security hardening) sit behind it.

## Guiding principles

- **One very competent human's worth of code.** Whenever a phase risks ballooning, cut
  scope, not corners. A working subset > an unfinished superset.
- **Vertical slices.** Each phase should end with something that can be
  run and demoed end-to-end, not just a library waiting for a caller.
- **No third-party engines we haven't read.** Vendor dependencies must
  be small, embeddable, and auditable.
- **No automated test suite.** Resources don't allow it. Verify
  changes by running the browser. Code that's hard to verify by
  manual exercise should be redesigned, not test-covered.
- **No code comments.** The code is self-explaining. Each file gets
  one short header comment naming it; no inline comments, no section
  banners, no TODOs. See `CLAUDE.md` for the full rule.

## Phases

### Phases 0–5 — Done

- Phase 0: meson + ninja scaffold, GTK 4 window, CI on Linux + macOS
  + Windows stubs, `--werror` build, binary uploaded as artifact.
- Phase 1: libcurl-backed `nd_net_fetch_async` (TLS verified, redirects
  capped at 10, http/https only) + address bar shell.
- Phase 2: DOM tree (`src/dom.[ch]`) and pragmatic HTML parser
  (`src/html.[ch]`) with rawtext handling and a debug DOM dump.
- Phase 3: CSS engine (`src/css.[ch]`) — selectors (type/id/class/`*`
  + descendant + child), property/value parsers for the
  layout-relevant subset, shorthand expansion, cascade with
  specificity + `!important`, inheritance pass, UA defaults.
- Phase 4: layout tree (`src/layout.[ch]`) distinct from the DOM —
  block / anonymous-inline / replaced-image / text boxes, content +
  padding + border + margin model, two-pass block stacking with
  Pango-measured inline heights, auto-margin centering, percent /
  em / rem / pt resolution.
- Phase 5: paint pass (`src/paint.c`) via Cairo + Pango. Block
  backgrounds, per-side borders, inline text with bold / italic /
  monospace / underline / strikethrough / link styling, list-marker
  glyphs, horizontal rules, image scaling, search highlights,
  scrollable viewport with `GtkScrolledWindow`, PDF print via
  `cairo_pdf_surface`.
- Phase 6 (mostly): Back / Forward / Home / Reload / URL bar / Go /
  Stop / Render-DOM-Raw-Layout view dropdown / star + bookmarks
  popover / About button. Ctrl+N / Ctrl+T new window. Middle-click
  and Ctrl+click and `<a target="_blank">` open a link in a new
  top-level window. Bookmarks file in `$XDG_CONFIG_HOME`. Find on
  Page (Ctrl+F) with match counter and Enter-to-next. Ctrl+= /
  Ctrl+- / Ctrl+0 zoom. Page fragments (`#anchor`) scroll to the
  element. Smart-bar (bare words route through DuckDuckGo). Error
  pages and a built-in `about:mozilla` info page.

### Phase 6 — Browser chrome (remaining)

Phase 6 is now done in its first form. Per-window OS processes
shipped (see iteration log entry for the fork+exec on Ctrl+N /
target=_blank / middle-click). Future polish:

- File watches for live bookmark refresh across windows.
- Configurable home URL (currently a compile-time constant).

### Phase 7 — JavaScript

**QuickJS**, vendored as a git submodule at
`third_party/quickjs/` and compiled into a static library that
links into `nordstjernen`. Console.log inside `<script>` is wired
to a per-window log callback. Build prerequisite:
`git submodule update --init --recursive`.

Remaining deliverables:

- DOM bindings for the read-only subset first
  (`document.querySelector`, `Element.textContent`, etc.).
- Event loop integrated with `GMainContext`.
- Mutations: `innerHTML`, `appendChild`, `setAttribute`.

### Phase 8 — Forms, cookies, storage

- `<form>` submission (GET + POST `application/x-www-form-urlencoded`)
- Cookie jar with secure / httpOnly / SameSite semantics (already
  ships a persistent jar; SameSite handling is the work item)
- `localStorage` (no IndexedDB)

**No browser history.** A persistent visit-history file is
deliberately not stored anywhere on disk. The session-history
stack used for Back/Forward lives only in memory and is dropped
when the window closes. Rationale: annoying to manage, and a
privacy footgun — the file would otherwise need a UI to clear it,
a sensible default for incognito, and protection against cross-app
reads. Cheaper to never have it.

### Phase 9 — Security

- HSTS preload list
- Mixed-content blocking
- Certificate pinning toggle (off by default)
- SOP / CORS enforcement at fetch layer
- No third-party cookies by default

### Phase 10 — Media

- One video codec at a time (per README). Start with VP9 via libvpx
  in a `<video>` element bound to a `GtkVideo` widget.
- Audio via PipeWire (Linux) / CoreAudio (macOS) / WASAPI (Windows).

### Phase 11 — Distribution

- **Auto-updater with a monthly nag.** On launch (capped to once
  per 24h), fetch a small JSON manifest from the project's release
  hosting (URL TBD), compare the latest version with the running
  binary, and if older by more than 30 days show a non-blocking
  popup offering to download the newer build. Never auto-installs
  in the background — the user always confirms.
- **Downloadable Windows installer (.exe) and macOS .dmg.** The
  Windows build is wrapped into a signed installer with shortcut
  + uninstaller; the macOS build into a notarized DMG.
- **AI-gated download flag.** A small server-side feature flag
  controls whether the download links light up on the project
  page. The AI agent driving development flips the flag based on
  CI health, regression rate, and Acid3 score; "off" hides the
  installer from new users while keeping existing builds working.

### Phase 12 — Mobile

- **Responsive renderer.** The layout engine already takes a
  viewport width, but the UA stylesheet assumes desktop sizes.
  Add `@media (max-width: ...)` parsing and ship a UA stylesheet
  variant for narrow viewports (larger tap targets, single-column
  flow, hidden chrome). Verify on a 360 px-wide screenshot in CI.
- **Android port.** Build the same C source against the NDK with
  a thin GTK-replacement shim — either a minimal X11/Wayland-style
  abstraction for surface + events, or a hand-rolled Android-NDK
  backend. Networking stays libcurl; rendering stays Cairo +
  Pango. Image decode via libpixbuf on desktop, libjpeg-turbo /
  libpng directly on Android. No Java/Kotlin app shell beyond the
  minimal Activity that hosts the C engine.

## Test sites

Manual verification corpus. Ordered roughly by ascending pain: the
first ones must work for the browser to be useful at all; the last
ones are aspirational and will probably need future-phase support.

Update this list when a new site exposes a regression, when a
previously-broken site starts working, or when a target site changes
shape enough that the old URL no longer represents the test case.

### Tier 0 — should always work (Phase 0–3)

- `https://example.com` — IANA's canonical "is the network working?"
  page. Single short paragraph, one link, minimal styling.
- `https://motherfuckingwebsite.com` — pure HTML, no CSS to speak
  of. Renders correctly iff the UA stylesheet is sane.
- `https://lite.cnn.com` — text-only news mirror. Sanity check for
  the HTML parser on real-world markup.
- `https://text.npr.org` — same idea, slightly heavier nav.

### Tier 1 — heavier real-world pages

- `https://en.wikipedia.org/wiki/HTML5` — long article, headings,
  paragraphs, inline links, lists, tables.
- `https://news.ycombinator.com` — table-based layout, small CSS,
  user-generated text. Good stress test for inline formatting.
- `https://danluu.com` — minimal CSS blog. Long paragraphs,
  headings, the occasional inline `<code>`.
- `https://html.duckduckgo.com/html/?q=nordstjernen` — search results
  page with no JavaScript dependency.

### Tier 2 — requires forms / cookies (Phase 8)

- `https://duckduckgo.com` (home) — submit a query via the search
  form. Round-trips POST + session cookie.
- `https://en.wikipedia.org/wiki/Special:Random` — exercises
  redirects (we already handle these via libcurl).

### Tier 3 — requires JavaScript (Phase 7)

- `https://news.ycombinator.com` (vote / collapse) — Hacker News
  degrades gracefully without JS, but the interactive bits need it.
- A site that uses `document.querySelector` and small DOM mutations.

### Acid3 progress tracker

`http://acid3.acidtests.org/` is the long-running compatibility
target. It exercises a lot of features Nordstjernen will probably
never ship (SVG, SMIL, ECMAScript edge cases, DOM Range, …) and
many it should (selectors, generated content, table layout, font
metrics). Browse it sometimes; note the visible score in this log.
The point is to track our trajectory across phases, not to chase
100/100.

| Date       | Phases done | Visible score | Notes |
| ---------- | ----------- | ------------- | ----- |
| 2026-05-11 | 0–5b        | TBD           | Initial run pending; will record after the next manual visit. |

### Tier 4 — explicitly out of scope

- Anything that requires WebGL, WebGPU, WebRTC, service workers, or
  WebAssembly. Listed only so future-us doesn't mistake an
  intentional non-target for a regression.
- `https://youtube.com` web client — listed because YouTube *audio*
  is a Phase 10 goal, but the current web UI requires modern APIs we
  don't ship. The plan is to support `<video>`-tag pages, not the
  full YT client.

## Non-goals (explicit, won't change)

- WebGL, WebGPU, WebRTC, WebUSB, WebBluetooth, WebHID, WebMIDI.
- Service workers, push notifications, background sync.
- DRM / EME / Widevine.
- **No tab strip.** One page per top-level window; multiple windows
  are how the user manages multiple pages.
- **No plugins.** No NPAPI, no PPAPI, no WebExtensions / browser
  extensions, no Flash, no shims, no plugin host process. The browser
  ships exactly what's in this repo, and never executes third-party
  native code on the user's behalf.
- **No persistent browsing history.** The session back/forward
  stack lives only in memory; nothing about a visit is written to
  disk (see Phase 8 for the rationale).
- Sync, accounts, "studies", telemetry of any kind.
- Localization beyond English.

## Iteration log

Append-only. One line per material change.

- 2026-05-11 — Repo initialized. Phase 0 in progress.
- 2026-05-11 — Doc pass: README typo fixes, NORDSTJERNEN.md prose
  tightened, Phase 7 narrowed to QuickJS.
- 2026-05-11 — Phase 0: meson + ninja scaffold lands. `src/main.c`
  opens an empty GTK 4 `GtkApplicationWindow`; builds clean with
  `-Wall -Wextra -Wshadow -Wstrict-prototypes` and friends.
- 2026-05-11 — Phase 0 CI: linux gcc+clang matrix, smoke test, binary
  upload artifact, `--werror` build. macOS (homebrew gtk4) and
  Windows (MSYS2 MINGW64) workflow stubs added.
- 2026-05-11 — Phase 1: libcurl wrapper (`src/net.[ch]`, async via
  GTask, redirects capped at 10, http/https only). GTK shell gains
  address bar + Load/Stop + `GtkTextView` body. libcurl floor bumped
  to 7.85 for `CURLOPT_PROTOCOLS_STR`.
- 2026-05-11 — Phase 2: DOM (`src/dom.[ch]`) and pragmatic HTML
  parser (`src/html.[ch]`); rawtext handling for script/style,
  common entities, implicit `<p>` close. Main window grows a DOM
  toggle; HTML defaults to DOM dump view.
- 2026-05-11 — Phase 3: CSS engine (`src/css.[ch]`) — selectors
  (type/id/class/`*`, descendant, child), property/value parsers for
  the layout-relevant subset, shorthand expansion, cascade with
  specificity + `!important`, inheritance pass, UA defaults.
- 2026-05-11 — Removed `tests/` and `meson test` wiring on
  request — project has no automated test suite by policy.
- 2026-05-11 — Plan doc flushed: completed phase deliverables
  collapsed to one-line summaries. "No plugins" added to non-goals
  with full prose (no NPAPI/PPAPI/WebExtensions/native shims).
- 2026-05-11 — Navigation chrome: header bar gains Back / Forward /
  Home buttons (Firefox-style, on the left of the URL entry) plus a
  Go button. Session history is a stack with a cursor; new loads
  truncate the forward portion. Linux CI dropped GCC, runs clang
  only. README gains build badges for linux/macos/windows workflows.
- 2026-05-11 — Design decision: no tabs. One page per window. New
  windows replace what other browsers would solve with tabs.
  Plan's Phase 6 deliverables updated accordingly.
- 2026-05-11 — `about:` URL scheme handled internally by `net.c`.
  An About button in the header bar opens `about:mozilla`, which
  shows a built-in info page describing the project, its design
  goals, and licensing.
- 2026-05-11 — Image rendering (Phase 5b ahead of plan): new
  `src/image.[ch]` provides an async URL → GdkTexture cache backed by
  libcurl + GdkPixbufLoader; layout grows an `ND_BOX_IMAGE` kind
  for `<img>`; paint draws the texture (or a placeholder rect
  while loading). Supports the PNG / JPEG / GIF formats GdkPixbuf
  decodes by default.
- 2026-05-11 — Bookmarks (Phase 6): `src/bookmarks.[ch]` stores a
  tab-separated `bookmarks.txt` under XDG_CONFIG_HOME. Header bar
  gains a star toggle for the current page and a bookmarks
  popover listing saved entries. Shared across windows via a
  module-global `g_bookmarks` loaded at startup.
- 2026-05-11 — Acid3 target added to the test-sites list with a
  progress tracker table. The intent is to eyeball it occasionally
  and note where we are; we are not chasing 100/100.
- 2026-05-11 — Inline formatting: nd_box gains an `attrs` array
  carrying per-character-range PangoAttribute kinds for
  bold/italic/monospace/underline/strikethrough, populated by a
  DOM walker that tracks nested depth per family. Visible effect:
  `<b><em><code>` etc. now render with their proper typography.
- 2026-05-11 — `<a target="_blank">` now opens the link in a new
  top-level window without needing Ctrl/middle-click.
- 2026-05-11 — Removed the persistent visit-history feature
  (history.[ch], the on-disk `history.txt`, all wiring). Privacy
  footgun and UX nag. Session back/forward stack stays in memory
  only. Recorded in Phase 8 as an explicit non-goal.
- 2026-05-11 — Plan grows three future phases:
    * Phase 6 deliverable for per-window processes (today they
      share one GtkApplication; the fork-exec split is queued).
    * Phase 11 — distribution: monthly-nag auto-updater, signed
      Windows .exe / macOS .dmg, AI-managed feature flag gating
      the download link.
    * Phase 12 — mobile: responsive renderer with @media support
      and a narrow-viewport UA, plus an Android port reusing the
      C engine with a minimal native-activity host.
- 2026-05-11 — Plan flush: Phases 4 and 5 moved into the "Done"
  block now that layout and paint ship; Phase 6's deliverable
  list collapsed to just the remaining per-window-process item.
  "No tab strip" and "No persistent browsing history" promoted
  into the explicit non-goals section. Tier-1 site list no
  longer pinned to Phase 4–5.
- 2026-05-11 — Per-window OS processes shipped. `nd_spawn_window`
  fork+execs a fresh `nordstjernen` binary with the target URL
  on Ctrl+N / Ctrl+T / Ctrl+click / middle-click /
  `<a target="_blank">`; the first window per process still
  starts in-process via on_activate. Self-exe path resolved via
  /proc/self/exe on Linux, argv[0] elsewhere. Bookmarks and
  cookies stay shared via the on-disk files.
- 2026-05-11 — Bookmarks file watch: a GFileMonitor on
  bookmarks.txt reloads g_bookmarks and refreshes star buttons in
  every open window when the file changes, so the per-process
  windows stay in sync on bookmark add/remove.
- 2026-05-11 — Configurable home URL: read once at startup from
  `$XDG_CONFIG_HOME/nordstjernen/home.txt`; Home button and
  on_activate fallback consult it instead of the compile-time
  ND_HOME_URL constant.
- 2026-05-11 — Phase 7 initial pass: `src/js.[ch]` wraps QuickJS.
  QuickJS is now vendored as a git submodule at
  `third_party/quickjs/` (bellard's upstream, 2025-09-13 release)
  and compiled into a static `libquickjs.a` that links into
  `nordstjernen`. No external runtime dependency. Per-window
  binding instantiates a JS runtime, installs a `console.log`,
  and runs every inline `<script>` element after parse. CI
  workflows updated to check out submodules recursively. C
  standard for the QuickJS unit relaxed to gnu11 so its inline
  `asm` blocks compile.
