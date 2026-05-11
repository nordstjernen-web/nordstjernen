# Nordstjernen — Development Plan

Live roadmap for the clean-room implementation. See `README.md` for
product vision and `CLAUDE.md` for working agreements.

Nordstjernen is a clean-room reimplementation inspired by Firefox 1.0
(via the `webmosilla` fork) — AI reads that code for orientation only;
no source is copied. The plan is to first match the feature set of
Firefox 1.0, then modernize to a usable browser with HTML5 and a
pragmatic subset of modern JavaScript and CSS.

## Current status

Phases 0–5 are done, plus most of Phase 6 chrome. The browser:

- Fetches HTML/PNG/JPEG/GIF over HTTP/HTTPS with a persistent
  cookie jar.
- Parses HTML pragmatically (rawtext, entities, void elements).
- Cascades CSS (selectors, em/%/px units, !important, inheritance,
  the full 147-name CSS3 palette plus rgb/rgba).
- Lays out block / inline / replaced-image boxes with margin
  collapsing-ish, padding, borders, auto-margin centering, and
  Pango-measured inline heights.
- Paints via Cairo + Pango with bold/italic/code/underline/
  strikethrough inline runs, links, list markers, hr rules,
  images, and search highlights.
- Carries Firefox-style chrome: Back / Forward / Home / Reload /
  URL bar / Go / Stop / Render-DOM-Raw-Layout view dropdown /
  bookmark star / bookmarks popover / About / window menu /
  status bar.
- Supports multiple top-level windows (Ctrl+N, Ctrl+click,
  middle-click), Find on Page (Ctrl+F with N-match counter and
  Enter-to-next), zoom (Ctrl+= / Ctrl+- / Ctrl+0), URL fragment
  scroll-to, smart-bar (bare words → DuckDuckGo search), error
  pages, and a built-in `about:mozilla` info page.
- Persists bookmarks and visit history in
  `$XDG_CONFIG_HOME/nordstjernen/`.

**Phase 7 — JavaScript (QuickJS)** is the next big slice. Before
that, a smaller pass on Phase 8 (forms, history dropdown) and
quality-of-life work on the layout engine is queued.

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

### Phases 0–3 — Done

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

### Phase 4 — Layout (current)

Deliverables:

- Block formatting context (vertical stacking, margin collapse — at
  least the common case)
- Inline formatting context (line boxes, text wrapping)
- Box model (content/padding/border/margin)
- Layout tree distinct from DOM (anonymous boxes for mixed content)

### Phase 5 — Rendering

Deliverables:

- Paint pass via Cairo or GSK
- Text rendering via Pango
- Backgrounds, borders, basic box shadows (no `filter:` effects)
- Scrollable viewport

Done when: Wikipedia article pages render legibly.

### Phase 6 — Browser chrome

Most navigation chrome lands ahead of schedule. The remaining
items:

Deliverables:

- [x] **One page per window. No tab strip.** Opening a link in a new
      context spawns a new top-level window in the same process via
      GtkApplication. Each window owns its own history and Render
      surface; cookies and bookmarks are shared.
- [x] Ctrl+N to open a new window, middle-click / Ctrl+click on a
      link to open it in a new window
- [x] Bookmarks (single file on disk, plain text) — star button +
      bookmarks popover in the header bar
- [x] About button (loads `about:mozilla`)
- [x] Reload button (distinct from Go)
- [ ] URL bar with history dropdown (in-progress)

### Phase 7 — JavaScript

**QuickJS** (small, MIT, embeddable). SpiderMonkey and V8 are out —
too large, drag in too much.

Deliverables:

- Engine vendored in `third_party/` with a thin C binding layer
- DOM bindings for the read-only subset first
  (`document.querySelector`, `Element.textContent`, etc.)
- Event loop integrated with `GMainContext`
- Mutations: `innerHTML`, `appendChild`, `setAttribute`

### Phase 8 — Forms, cookies, history, storage

- `<form>` submission (GET + POST `application/x-www-form-urlencoded`)
- Cookie jar with secure / httpOnly / SameSite semantics
- History stored as SQLite or a flat journal
- `localStorage` (no IndexedDB)

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

### Tier 1 — should work once layout + paint land (Phase 4–5)

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
- **No plugins.** No NPAPI, no PPAPI, no WebExtensions / browser
  extensions, no Flash, no shims, no plugin host process. The browser
  ships exactly what's in this repo, and never executes third-party
  native code on the user's behalf.
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
