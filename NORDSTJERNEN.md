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

- **Right-click context menu** on the render surface — common
  browser affordance for "Open Link in New Window", "Copy Link
  Address", "Save Page As PDF", "View Source", "Reload",
  "Back", "Forward", "Inspect" (open JS console), etc. Built
  as a GtkPopoverMenu attached to the drawing area via a
  GtkGestureClick on GDK_BUTTON_SECONDARY. The menu entries
  surface the existing GActions where possible (win.print,
  win.reload, win.open-console, …) and add link-aware actions
  when the click lands on a link. **Shipped.**

### Phase 7 — JavaScript

**[quickjs-ng](https://github.com/quickjs-ng/quickjs/)**, the
maintained fork of Bellard's QuickJS. JavaScript is a moving
target — we prefer the fork that's actively keeping up with the
spec over upstream's release cadence. Pinned at
[v0.14.0](https://github.com/quickjs-ng/quickjs/releases/tag/v0.14.0),
downloaded by meson from the release zip
(`subprojects/quickjs.wrap`) into
`subprojects/quickjs-0.14.0/`, compiled into a static library
and linked into `nordstjernen`. No git submodules — `meson setup`
auto-fetches the zip into `subprojects/packagecache/`.

Console.log inside `<script>` is wired to a per-window log
callback that writes to the status bar.

Shipped so far (broad sweep — sees real-world JS run):

- Globals: `console.log/.warn/.error/.info/.debug`, `alert`,
  `navigator.userAgent`/`.appName`, `window`,
  `localStorage` / `sessionStorage` (in-memory),
  `fetch(url)` → Promise<{ok, status, url, body, text(), json()}>.
- Timers: `setTimeout` / `setInterval` / `clearTimeout` /
  `clearInterval` on GMainContext.
- Microtask drain (Promises / queueMicrotask) after every
  host-side JS_Call / JS_Eval.
- `location.href` getter+setter, `assign(url)`, `reload()`.
- `document`: `title`, `URL`, `domain`, `body`,
  `documentElement`, `getElementById`, `getElementsByTagName`,
  `getElementsByClassName`, `querySelector`, `querySelectorAll`,
  `createElement`, `createTextNode`.
- `Element` (read): `tagName`, `id`, `className`, `textContent`,
  `innerHTML`, `outerHTML`, `parentElement`, `parentNode`,
  `firstElementChild`, `nextElementSibling`,
  `previousElementSibling`, `children`, `getAttribute`,
  `hasAttribute`, descendant `getElementsByTagName` /
  `getElementsByClassName` / `querySelector(All)`.
- `Element` (write): `textContent` setter, `innerHTML` setter
  (fragment-parsed), `setAttribute`, `removeAttribute`,
  `appendChild`, `removeChild`.
- `Element.style` — full CSSStyleDeclaration with
  per-property exotic accessors (style.color, style.backgroundColor,
  …) keyed off the inline `style` attribute, plus `cssText`.
- `Element.classList` with `contains` / `add` / `remove` /
  `toggle`.
- `Element.addEventListener` / `removeEventListener`. Click
  events bubble up the DOM ancestor chain from the hit-test
  path. Event object has `type`, `target`, `currentTarget`,
  `defaultPrevented`, `bubbles`, `cancelable`,
  `preventDefault()`, `stopPropagation()`,
  `stopImmediatePropagation()`. The `submit` event fires on
  the form before navigation; both `click` on a link and
  `submit` on a form honor `preventDefault()`.
- Mutations to the DOM (attribute / text / structural) flag
  the JS context; the host drains the flag after every JS
  entry and re-cascades + redraws.
- `<script src="…">` loads synchronously via libcurl during
  document script-execution. Mixed-content rule applies (http
  scripts on https pages are blocked and logged). Maximum
  script size is 8 MB; non-200 statuses are logged but don't
  halt subsequent scripts.
- `localStorage` persists to disk per-origin under
  `$XDG_DATA_HOME/nordstjernen/localstorage/<sha256(origin)>.ini`
  (GKeyFile, mode 0600 in a mode-0700 directory). Loaded on
  document install, flushed after every JS turn that dirtied
  the table, and once more on shutdown. Disabled by setting
  `ND_NO_LOCAL_STORAGE` in the environment. About: / file://
  pages get no origin and therefore no persistence.

Remaining deliverables:

- `keydown` and `keyup` events fire on `<body>` whenever the
  rendered page has focus. Event carries `key`, `code`,
  `keyCode`, `which`, `shiftKey`, `ctrlKey`, `altKey`, `metaKey`,
  `repeat`, plus the standard preventDefault/stopPropagation
  affordances. Returning truthy / preventDefault suppresses the
  default GTK key handling.

- `input` events fire on `<input>` / `<textarea>` as the user
  types. Clicking a text-like input focuses it; printable
  characters append, Backspace removes a codepoint, Escape
  blurs, Enter submits a form (or inserts a newline inside a
  textarea).

- `focus`, `blur`, and `change` events fire on text inputs.
  Click-to-focus dispatches `focus`; subsequent click elsewhere
  (or Escape / Enter / navigation) dispatches `blur`, plus
  `change` if the value differs from the focus-time snapshot.

Phase 7 DOM-event surface is now complete in the sense
relevant to non-IME, non-caret form input. Future polish
items (composition events, selection events) are out of scope
unless we ship a real caret editor.

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

Shipped:

- **Mixed-content blocking** for subresources. On an HTTPS page,
  the browser refuses to fetch HTTP stylesheets and images and
  logs a warning. The page itself can still be HTTP (the user
  explicitly typed it); only subresources are gated.
- **Dynamic HSTS.** Strict-Transport-Security response headers
  are parsed, persisted to
  `$XDG_DATA_HOME/nordstjernen/hsts.txt` (mode 0600), and
  consulted on every subsequent navigation. http:// requests
  to any host in the table (or with `includeSubDomains` from a
  parent) are upgraded to https:// before the libcurl call is
  made. A static preload list is intentionally not bundled.

Remaining:

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
  are how the user manages multiple pages. The host OS's window
  manager / taskbar / Mission Control / Alt-Tab is the navigation
  affordance for moving between open windows — Nordstjernen does
  not add its own switcher UI.
- **No plugins.** No NPAPI, no PPAPI, no WebExtensions / browser
  extensions, no Flash, no shims, no plugin host process. The browser
  ships exactly what's in this repo, and never executes third-party
  native code on the user's behalf.
- **No persistent browsing history.** The session back/forward
  stack lives only in memory; nothing about a visit is written to
  disk (see Phase 8 for the rationale).
- Sync, accounts, "studies", telemetry of any kind.
- Localization beyond English.

## Ideas backlog

Loose notes from the user; not committed to any phase yet. Promoted
to a Phase deliverable once the scope and ordering are clear.

- **Run Claude on Windows.** The autonomous-dev loop currently runs
  on a Linux box (see CLAUDE.md). Get the same loop working on
  Windows — MSYS2 + Claude CLI + a meson build that survives without
  cygwin assumptions. Unlocks Windows-side fixing of the things the
  daily CI build flags, without round-tripping through Linux first.
- **gumbo-parser as secondary parser — shipped.** Wrap +
  adapter (`src/html_gumbo.c`) integrated as an opt-in
  alternate page-level parser; selected at runtime via
  `ND_HTML_PARSER=gumbo`. The hand-rolled
  `src/html.c` stays the primary parser. Gumbo is kept
  around for cross-checking and as a reference for how a
  real HTML5 parser handles edge cases the primary is too
  small to cover.
- **muPDF for the PDF viewer.** We already export pages to PDF
  via Cairo. The complement is rendering `application/pdf` pages
  inline rather than handing them to the OS viewer. muPDF is a
  small C library that fits the project's audit-the-deps rule.
  Decide later whether it ships statically linked or vendored as a
  meson subproject.
- **Threads.** The engine is single-threaded today; libcurl
  fetches go via GTask but everything else (HTML parse, CSS
  cascade, layout, paint, JS) runs on the GTK main loop. Identify
  the first thing that's worth moving off — likely image decode
  or large-stylesheet parsing — and introduce a single worker
  thread for it before going wider. Threads are a force multiplier
  *and* a debugging hazard; add them deliberately, not preemptively.
- **Shareware.** Distribution model: a free download with a nag
  screen that, after some grace period, asks the user to buy a
  license to suppress the nag. Mechanics borrowed from late-80s
  / early-90s shareware (think WinZip, ACDSee). No DRM and no
  online check beyond the existing monthly auto-updater ping. The
  nag is the entire enforcement surface; the binary keeps working
  either way. Fits with Phase 11's AI-gated download flag.

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
  `third_party/quickjs/` and compiled into a static
  `libquickjs.a` that links into `nordstjernen`. No external
  runtime dependency. Per-window binding instantiates a JS
  runtime, installs `console.log`, and runs every inline
  `<script>` element after parse. CI workflows updated to check
  out submodules recursively. C standard for the QuickJS unit
  relaxed to gnu11 so its inline `asm` blocks compile.
- 2026-05-11 — Switched JS engine from upstream `bellard/quickjs`
  to `quickjs-ng/quickjs` v0.14.0. Rationale: JavaScript is a
  moving target and the -ng fork keeps closer to current spec
  (and ships actual numbered releases). Build now uses
  `-DQUICKJS_NG_BUILD`, drops the obsolete `cutils.c` /
  `CONFIG_VERSION` plumbing, and pins to a tagged release rather
  than tracking `master`.
- 2026-05-11 — QuickJS purged from optional-dependency machinery:
  HAVE_QUICKJS define and nd_js_available() are gone, and the
  engine is always instantiated.
- 2026-05-11 — Plan deliverable added: a dedicated
  JavaScript-console window (Ctrl+Shift+J, scrollback,
  severity-colored, evaluation input line) so console output
  isn't squeezed through the status bar.
- 2026-05-11 — JS DOM bindings landed: Element class with
  tagName / id / className / textContent getters, getAttribute /
  hasAttribute / parentElement / firstElementChild /
  nextElementSibling / previousElementSibling / children /
  getElementsByTagName / getElementsByClassName. Document gains
  getElementById / getElementsByTagName / getElementsByClassName
  / documentElement / body. JS context is torn down on every
  navigation so element pointers stay valid.
- 2026-05-11 — JS console window (Ctrl+Shift+J) shipped:
  satellite transient window per page with a monospace
  scrollback view and a REPL entry at the bottom that
  JS_Evals against the page's runtime via the new
  nd_js_eval_source(). Console.log / warn / error / info /
  debug also flow into the buffer.
- 2026-05-11 — JS: switched QuickJS engine integration from a
  git submodule to a meson wrap that auto-downloads the v0.14.0
  zip from quickjs-ng. CI builds verified across linux / macos /
  windows.
- 2026-05-11 — JS: querySelector + querySelectorAll on
  document and Element, backed by a new public
  `nd_css_parse_selector_list` / `nd_css_selector_matches`
  pair in css.c.
- 2026-05-11 — JS event loop: setTimeout, setInterval,
  clearTimeout, clearInterval driven through GMainContext via
  g_timeout_add. nd_js_new takes a mutated callback so timer
  callbacks that touch the DOM trigger a host-side relayout.
- 2026-05-11 — JS DOM mutations: createElement,
  createTextNode, appendChild, removeChild, setAttribute,
  removeAttribute. Orphan (detached) node tracking lives on
  the JS context — nodes created or removed but never
  reattached are freed at navigation.
- 2026-05-11 — JS Element.innerHTML / outerHTML getters via a
  new `nd_node_inner_html` / `nd_node_outer_html` pair that
  serializes a subtree back to HTML with proper attribute and
  text escaping and void-element handling.
- 2026-05-11 — CI throttled to once-a-day cron + manual
  workflow_dispatch on all three platforms after the
  operativsystem42 Actions allowance hit 90%. Local builds
  on the user's machine are now the per-change correctness
  gate.
- 2026-05-11 — JS Element.textContent and Element.innerHTML
  setters. innerHTML setter parses the assigned string through
  nd_html_parse and moves the parsed top-level children into
  the element.
- 2026-05-11 — JS Element addEventListener / removeEventListener
  ('click' for now) + click dispatch wired into the existing
  drawing-area hit-test path. Listener payload carries through
  nd_listener records on the JS context; dispatch bubbles up
  the DOM ancestor chain with a minimal {type, target} event.
- 2026-05-11 — JS microtask drain via JS_ExecutePendingJob in
  the eval / console-eval / timer-fire / event-dispatch paths.
  Promises / queueMicrotask / async-await resolve before
  control returns to the GTK main loop.
- 2026-05-11 — JS DOM mutations: createElement,
  createTextNode, appendChild, removeChild, setAttribute,
  removeAttribute, textContent / innerHTML setters. Orphan
  (detached) node tracking on the JS context cleans up
  unattached nodes at navigation.
- 2026-05-11 — JS Element.style with cssText getter/setter
  (per-property accessors deferred).
- 2026-05-11 — JS location: href getter+setter,
  assign(url), reload() dispatch through a host navigate-cb
  to nd_window_load_url. reload() preserves the history entry;
  assign / href setter treat the new URL as a user navigation.
- 2026-05-11 — CSS: border / border-top / border-right /
  border-bottom / border-left shorthand parsing for the
  common `border: 1px solid black` form.
- 2026-05-11 — CSS: text-decoration property maps to inline
  underline / strikethrough on top of the existing
  <u>/<s>/<del>/<strike> tag detection.
- 2026-05-11 — CSS: inline `style="…"` attribute now applied
  during cascade (with spec-correct very-high specificity).
  Page-level `<style>` elements collected as author stylesheets
  per render. External `<link rel="stylesheet">` fetched
  async via nd_net_fetch_async with cancellation tied to the
  window; re-cascade + redraw on each arrival.
- 2026-05-11 — CSS: attribute selectors
  (`[attr]`, `=`, `^=`, `$=`, `*=`, `~=`).
- 2026-05-11 — Renderer: `<input value="…">` /
  `<textarea>` / `<input type=submit|button|reset>` /
  `<input type=checkbox|radio>` now render their visible
  payload (value or placeholder; ☐/☑ or ◯/◉) instead of
  empty bordered rectangles.
- 2026-05-11 — Net: `Accept-Language: en-US,en;q=0.9`,
  `Accept: text/html,…`, and `DNT: 1` request headers added
  to every libcurl fetch.
- 2026-05-11 — JS: `Element.style.X` per-property accessors
  via QuickJS exotic class — camelCase ↔ kebab-case.
- 2026-05-11 — JS: `Element.classList` with `contains` /
  `add` / `remove` / `toggle`.
- 2026-05-11 — JS: `localStorage` / `sessionStorage`
  (in-memory). On-disk persistence deferred for privacy.
- 2026-05-11 — JS: `window.fetch(url)` → Promise<Response>
  using QuickJS Promise capability + the existing
  nd_net_fetch_async pipeline. Response has `ok`, `status`,
  `url`, `body`, `text()`, `json()`.
- 2026-05-11 — JS Event: `preventDefault` / `stopPropagation`
  / `stopImmediatePropagation` + the standard set of event
  fields. Bubble propagation honors `stopPropagation`.
- 2026-05-11 — Console: every line is timestamped HH:MM:SS.
- 2026-05-11 — Layout: real `<table>` support with row +
  equal-width column geometry. `<thead>` / `<tbody>` /
  `<tfoot>` nesting transparent; cells contain regular
  block-flow content; rows align to the tallest cell.
- 2026-05-11 — Right-click context menu shipped: GtkPopoverMenu
  on the drawing area with link-aware "Open Link in New
  Window" / "Copy Link Address" plus page-wide Back / Forward
  / Reload / Copy Page URL / Save Page As PDF / JavaScript
  Console / Find on Page entries.
- 2026-05-11 — Window-switcher proposal explored and dropped.
  Briefly planned, then revised twice (native WM APIs;
  privacy hardening). On further consideration the feature
  was cut entirely: the host OS's taskbar / Alt-Tab /
  Mission Control already solves window switching, and a
  custom in-chrome switcher would either duplicate that or
  expose cross-process state we'd rather not have. The
  "no tab strip" non-goal still stands and now points to
  the OS for switching instead.
- 2026-05-11 — CI cost control: all three workflows
  (linux/macos/windows) dropped the `push:` and
  `pull_request:` triggers. They now run only on a daily
  cron (around 04:17/04:23/04:29 UTC) and on manual
  `workflow_dispatch`. Pushes to main no longer burn Actions
  minutes. CLAUDE.md "Autonomous mode" updated to clarify
  that CI is *not* the per-push safety net any more.
- 2026-05-11 — Compatibility sprint (overnight session):
  Phase 7/8/9 work landed across HTML/CSS/JS layers.
  Highlights: `<table>` rendering bug fixed (collect_rows was
  skipping the root table), `<select>` collapses to its
  chosen option, `<details>`/`<summary>` toggle on click,
  checkbox/radio toggle on click, real text input typing
  with `input` events. CSS gained tolerant pseudo-class
  parsing, `@media` query evaluation, `var(--x, fallback)`
  substitution, `hsl()`/`hsla()` colours, and a wider UA
  stylesheet (svg/canvas/iframe hidden; details/dialog/menu
  block; [hidden] + <template> hidden). JS gained dozens of
  Element/document/window/navigator/history/performance/
  crypto/Event/URL/URLSearchParams APIs plus `<script src>`
  sync loading, `document.title` / `document.cookie` /
  `document.readyState`, `DOMContentLoaded`/`load`,
  `keydown`/`keyup` dispatch, full `location` surface,
  `Element.matches`/`closest`/`contains`/`getBoundingClientRect`,
  `Element.style.setProperty`, observer-class stubs
  (Mutation/Intersection/Resize), exception logging with
  origin + stack, and a 5s eval budget + 128 MB memory cap
  to keep runaway scripts from freezing the chrome.
  Security: dynamic HSTS persistence shipped. Smart-bar now
  routes bare-word searches to duckduckgo.com/lite/, and that
  is also the default home page since the SPA homepage isn't
  renderable in our engine.
- 2026-05-11 — Real text input. Clicking a text-like input
  (or a <label> for one) focuses it; printable characters
  append to the value attribute and Backspace pops a UTF-8
  codepoint. Enter submits the enclosing form (with text
  input recognised as an implicit submitter); Escape blurs.
  'input' fires per-keystroke; 'focus' / 'blur' / 'change'
  bracket the focus span. Focused input paints with a thicker
  blue border so the user can tell which field is active.
- 2026-05-11 — Forms: checkbox / radio toggle on click;
  radio groups clear sibling 'checked' attrs within the same
  form. <button>label</button> renders with the same 3D button
  border as <input type=submit>. formaction / formmethod on
  the clicked submit button override the form's action / method.
- 2026-05-11 — Links: javascript: URLs evaluate via the JS
  engine. <base href> respected for relative URL resolution.
- 2026-05-11 — Paint: <img alt> renders inside the placeholder
  rectangle when the image hasn't loaded or has failed.
- 2026-05-12 — CSS layout improvements: position parsing
  (position/top/right/bottom/left/z-index/opacity) with
  position:relative applying offset shifts and
  position:absolute/fixed elements hidden from flow.
  display:flex/grid/list-item/flow-root now treated as block
  containers so their children render. calc() expression
  parsing (percent + px buckets, summed at length resolution
  with appropriate basis). Shorthands 'background' and
  'font' expand to their colour / size / line-height /
  family components. hsl()/hsla() colour functions parse.
  Attribute selector '|=' hyphen-prefix match.
- 2026-05-12 — Inline runs gain font-size, color,
  background-color, font-family, font-weight, font-style and
  text-decoration:none Pango attributes — a <span class=header>
  with 32px orange now renders correctly, fixing DDG Lite's
  big-orange heading. line-height now respects unitless
  multipliers, em, and percent.
- 2026-05-12 — Forms: clicking a <select> opens a popover
  with all options, sets 'selected' on pick, and fires
  'change'. Submit triggers honour the 'disabled' attribute.
- 2026-05-12 — JS: Element.value / hidden / disabled /
  checked as real getters/setters. Element.scrollIntoView()
  actually scrolls. <dialog>.show / showModal / close.
  XMLHttpRequest minimal (open / send / readystatechange /
  onload). FormData. AbortController / TextEncoder /
  TextDecoder stubs.
- 2026-05-12 — HTML entity table grown from 12 to ~90 named
  entities (Latin-1 accented letters, currency symbols,
  arrows, dashes, small Greek slice, math operators).
  <meta http-equiv=refresh> schedules navigation.
- 2026-05-12 — Continued: cursor CSS prop honoured on hover.
  Hex colour #rgba and #rrggbbaa accepted. <ins> underlines
  per HTML5. <input type=file/color/range/date/time/...>
  render visibly. <select> popover lets the user pick from
  the option list (and fires 'change'). Element.cloneNode(deep)
  via nd_node_clone. Element.hidden/disabled/checked/value
  as real getters and setters. Element.scrollIntoView() now
  actually scrolls. <dialog>.show/showModal/close. Minimal
  XMLHttpRequest, FormData, DOMParser. AbortController /
  TextEncoder / TextDecoder stubs. KeyboardEvent.key uses
  HTML5 names (ArrowUp / Enter / Backspace / PageUp / …).
  Relative URLs assigned to window.location.href now resolve
  against the current page. Submit triggers skip when
  disabled. <noscript> is hidden in UA; <dialog>[open]
  block; svg/canvas/iframe/video display:none.
- 2026-05-12 — Plan: ideas backlog section added (Run Claude on
  Windows, gumbo-parser evaluation, muPDF inline PDF viewer,
  threads, shareware distribution model). Not committed to a
  phase yet — promoted on a per-idea basis once scope is clear.
- 2026-05-12 — CSS: text-transform (uppercase / lowercase /
  capitalize), inherited. Applied during inline collection so
  text-node content is transformed before the Pango pass.
- 2026-05-12 — JS: Element.id and Element.className gain
  setters; Element.insertAdjacentHTML
  (beforebegin / afterbegin / beforeend / afterend).
- 2026-05-12 — JS: document.head / activeElement / forms /
  images / links collections (live walks of the current
  document tree).
- 2026-05-12 — CSS/paint: list-style-type honoured —
  decimal / upper-alpha / lower-alpha / upper-roman /
  lower-roman / square / circle / disc / none. <ol type="A"/
  type="i"/...> picks the matching marker style, and
  <ol start="N"> shifts the count. New list-style shorthand
  parser picks up the type keyword (the only sub-property
  we currently store).
- 2026-05-12 — Branch policy: per user request, fold the
  session branch back into main. Git proxy denies direct
  pushes to main (HTTP 403), so changes still commit to
  claude/coding-session-8nXPa and land on main via merged
  pull requests.
- 2026-05-12 — CSS selectors: adjacent (`+`) and general (`~`)
  sibling combinators in selector matching and querySelector.
- 2026-05-12 — JS: Element.outerHTML setter (replaces self
  with parsed fragment); Element.replaceChildren accepts
  elements or strings.
- 2026-05-12 — CSS: structural pseudo-classes resolve —
  :first-child / :last-child / :only-child /
  :first-of-type / :last-of-type / :empty / :root /
  :nth-child(an+b | odd | even) / :checked /
  :disabled / :enabled / :required / :optional. Previously
  the parser swallowed pseudos and dropped them.
- 2026-05-12 — JS: Node-level firstChild / lastChild /
  nextSibling / previousSibling / childNodes /
  childElementCount + lastElementChild (skips
  non-elements).
- 2026-05-12 — JS: Element.title / name / alt / src / href
  / type / placeholder / lang / dir as attribute aliases
  (getters + setters). Real browsers expose these so JS
  code that prefers `el.title = "x"` over
  `setAttribute("title", "x")` now lands.
- 2026-05-12 — CSS/paint: visibility: hidden /
  visibility: collapse — the element keeps its box (so
  layout is preserved) but the subtree is skipped during
  paint. Inherited per spec.
- 2026-05-12 — CSS: `inherit` keyword. A property
  explicitly set to `inherit` now resolves against the
  parent's computed value before the per-property
  inheritance pass kicks in.
- 2026-05-12 — CSS/paint: opacity is honoured. Previously
  parsed (and the value stored) but the paint pass ignored
  it. Now wraps the subtree in a cairo group and pops it
  with paint_with_alpha so opacity:0.5 actually fades.
- 2026-05-12 — JS: document.createComment(text) and
  document.createDocumentFragment().
- 2026-05-12 — JS: boolean attribute aliases as real
  getters+setters — open / selected / multiple / readOnly
  / autofocus / controls / loop / muted / autoplay /
  defer / async / noValidate. Truthy assignment sets the
  bare attribute, falsy removes it.
- 2026-05-12 — JS: parentNode now returns the actual
  parent (document, comment, …) instead of NULL when
  the parent isn't an element.
- 2026-05-12 — JS: text/comment nodes expose nodeValue /
  data getter+setter.
- 2026-05-12 — JS: anchor.click() — if the dispatched
  click isn't preventDefault'd, the browser actually
  navigates to the href.
- 2026-05-12 — JS: form.submit() / form.requestSubmit()
  / form.reset(). The host registers a form-submit
  callback with the JS engine (mirroring the existing
  scroll-to and navigate callbacks); JS-initiated submits
  go through the same path as GTK-driven submits.
- 2026-05-12 — JS: Element.htmlFor — alias for the
  `for` attribute on `<label>`.
- 2026-05-12 — JS: <select>.value / .selectedIndex /
  .options. <option>.value with text-content fallback.
  Setting select.value walks options, clears prior
  'selected', applies it to the matching option.
- 2026-05-12 — JS: form.elements (recursive collection of
  input / select / textarea / button / fieldset / output
  controls) and control.form (walk up to the enclosing
  form).
- 2026-05-12 — JS: window.matchMedia(query).matches now
  uses the real media-query evaluator from css.c
  (previously hard-coded to false).
- 2026-05-12 — JS: <img>.naturalWidth / naturalHeight /
  complete stubs (return 0 / 0 / 0 for now — real values
  would need plumbing through the image cache).
- 2026-05-12 — JS: document.documentURI / baseURI /
  characterSet / charset / compatMode / contentType
  exposed on the document object.
- 2026-05-12 — CSS units: vw / vh / vmin / vmax now
  parse (mapped onto % until proper viewport-relative
  resolution lands). ex / ch parse as ≈ 0.5em. pt / pc /
  cm / mm / in resolve to absolute px (96-DPI assumption,
  matching the rest of the engine).
- 2026-05-12 — JS: performance.mark / measure / clearMarks
  / clearMeasures / getEntries / getEntriesByName /
  getEntriesByType. The getEntries* family returns empty
  arrays so libraries that iterate the result don't
  throw.
- 2026-05-12 — CSS: currentColor keyword. Cascade now
  resolves background-color / border-*-color values of
  `currentcolor` to the element's computed color.
- 2026-05-12 — JS: Element.tabIndex (parses tabindex
  attr; defaults to 0 on natively focusable elements and
  -1 otherwise).
- 2026-05-12 — JS: submit-button.click() submits the
  enclosing form when the click event isn't
  preventDefault'd, alongside the existing
  anchor.click() → navigate path.
- 2026-05-12 — JS: form.checkValidity() /
  reportValidity() (return true), form.setCustomValidity()
  (no-op). Constraint-validation API stubs.
- 2026-05-12 — JS: more attribute aliases —
  action / method / enctype / target / rel / accept /
  acceptCharset / autocomplete / list / min / max /
  step / pattern / spellcheck. All round-trip through
  the underlying HTML attribute.
- 2026-05-12 — CSS: initial / unset / revert keywords
  treated as "clear the property"; the inheritance pass
  then fills in from the parent for inherited props.
- 2026-05-12 — JS: Node.normalize() — in-place merge of
  adjacent text node siblings.
- 2026-05-12 — Paint: text-align: justify uses
  pango_layout_set_justify; logical `end` aliases `right`
  for LTR.
- 2026-05-12 — JS: getElementsByClassName('foo bar bar')
  requires all class tokens to be present on the element
  (per spec).
- 2026-05-12 — CSS: @supports body is always parsed
  (treated as always-true). Rules inside are subject to
  the normal cascade, so features we don't honour stay
  no-ops; but rules a page hides inside @supports for
  modern browsers are no longer thrown away.
- 2026-05-12 — JS: legacy constructors `new Image(w, h)`
  / `new Audio(src)` / `new Option(text, value,
  defaultSelected, selected)` build the matching nd_node
  in the orphan pool.
- 2026-05-12 — DOM: attribute name comparison is ASCII
  case-insensitive for set / get / remove, per HTML.
- 2026-05-12 — JS: Node.isConnected (true iff reachable
  from current document) / ownerDocument / namespaceURI
  (always xhtml). Element.shadowRoot returns null,
  attachShadow throws TypeError (we don't implement
  shadow DOM).
- 2026-05-12 — JS: window.customElements with no-op
  define / get / upgrade / whenDefined.
- 2026-05-12 — Build: gumbo-parser shipped as opt-in
  secondary HTML parser. New meson_options.txt feature
  `gumbo` (default 'auto'), wrap-git against
  codeberg.org/gumbo-parser/gumbo-parser, packagefile
  meson.build that compiles upstream src/*.c into a
  static libgumbo. New `src/html_gumbo.c` adapter maps
  GumboNode → nd_node; new `nd_html_parse_for_page()`
  reads `ND_HTML_PARSER=gumbo` and routes the
  document-level parse through gumbo. Fragment parsing
  (innerHTML, DOMParser, ...) keeps the primary in-tree
  parser. Default builds stay identical when gumbo
  isn't available.
- 2026-05-12 — Yet more polish: CSS shorthand 'background' /
  'font' expansions, '|=' attribute hyphen match, cursor
  property honoured, #rgba/#rrggbbaa hex colours, letter-
  spacing / word-spacing / white-space / min-width /
  min-height parsed, calc() also accepted as a width value.
  JS: classList.replace / length, window.open / close /
  confirm / prompt / focus / blur / stop / print,
  navigator.serviceWorker stub. Layout: <ins> underlines,
  <input type=file/color/range/date/...> render visibly,
  position:absolute / fixed elements hidden from flow.
  Inline runs now propagate font-weight / font-style /
  text-decoration:none from CSS to Pango attrs.
