# Nordstjernen — Development Plan

Live roadmap for the clean-room implementation. See `README.md` for
product vision and `CLAUDE.md` for working agreements.

Nordstjernen is a clean-room browser engine written from scratch.
No upstream browser source — Gecko, WebKit, Blink, KHTML, none —
is read, ported, or imported. The plan is to first reach a
usable feature set for reading the text-heavy web, then expand
with HTML5 and a pragmatic subset of modern JavaScript and CSS.

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
- **All JavaScript bindings live in `src/js.c`.** It's the engine
  binding layer — keep it as one file. Long is fine; sprawling across
  `js_storage.c`, `js_dom.c`, `js_xhr.c`, … is not. The tradeoff is
  that one file is easy to grep, easy to skim, and easy to keep a
  single mental model of how QuickJS values map to our DOM.

## Project goal: a first-class GNOME / GTK browser

Nordstjernen is built on GTK 4 and targets Linux first. Its
long-term positioning is to be **the small, native, auditable
GNOME-aligned browser** — the desktop browser that follows GNOME
HIG, ships through Flathub, integrates with GVFS / GSettings /
libsecret / portals, and one day appears in the GNOME Circle (or
its successor showcase for third-party GNOME applications).

This goal is explicit because every other choice flows from it:

- **GTK 4 native, never an Electron / CEF / WebKit shell.** Our
  engine and our chrome share the same toolkit.
- **libadwaita** for chrome widgets where it doesn't compromise
  the minimalism. Adaptive layouts so the same binary runs on
  phone, tablet, and desktop form factors.
- **Flathub as the primary distribution channel.** A reviewed,
  reproducible Flatpak manifest is the canonical install for
  end users. Distro packages follow, not lead.
- **xdg-desktop-portal first** for file pickers, screenshots,
  notifications, secret storage. We don't roll our own
  password store; we use libsecret via the GNOME Keyring portal.
- **GSettings schemas** for user preferences. No bespoke
  `~/.config/nordstjernen/config.toml` once the GSettings path
  is wired.
- **GVFS** for `sftp://`, `smb://`, `gphoto2://`, etc. — leverage
  what GNOME already exposes instead of reimplementing.
- **Track GNOME release cadence.** Match GTK / libadwaita
  ABIs of the current and previous GNOME release. Don't pin to
  ancient GTK 4.6 forever.
- **Engage upstream.** File issues against GTK / libadwaita /
  glib when our usage exposes bugs. Contribute fixes back. Ask
  on `#gnome-hackers` / GNOME Discourse before reinventing.
- **GNOME Circle eligibility as the visible milestone.** It's
  the most credible "this is a GNOME-aligned app" badge a
  third-party project can earn, and the review criteria
  (HIG-respecting UI, English-only OK, single-purpose,
  reproducible build, active maintenance) align almost exactly
  with our existing principles.

The browser engine itself stays clean-room — this goal is about
*positioning*, not about embedding WebKitGTK. We render with our
own code. We just commit to being a polite, predictable, native
GNOME citizen everywhere outside the engine.

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
  pages and a built-in `about:nordstjernen` info page.

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

- **Text selection + copy/paste on the render surface.** The
  rendered page is currently a custom-drawn `GtkDrawingArea`,
  not a `GtkTextView`, so the OS-native selection affordances
  don't apply. Need a selection model that:
    * Tracks a `(anchor_box, anchor_offset)` →
      `(focus_box, focus_offset)` pair against the layout tree's
      text boxes, where offsets are codepoint indices into the
      `nd_box.text` UTF-8 buffer.
    * Updates on `GtkGestureDrag` (begin / update / end) and on
      Shift-click. Single click clears selection; double-click
      selects a word; triple-click selects a line / block.
    * Repaints the selected codepoint ranges with a system-style
      highlight background (`GtkStyleContext` "selected" colours
      so it matches dark / light themes).
    * Reuses the Pango layout already built per-box: take
      `pango_layout_index_to_pos()` for begin/end, paint a
      `cairo_rectangle()` behind the run before the text.
    * Linearises the selection back to plain text in document
      order for the clipboard. Ctrl+C copies the selection (or
      the focused link's href when nothing is selected); Ctrl+A
      selects the entire page text. Selection survives Find on
      Page highlighting (different overlay).
    * On the URL bar / search bar / future `<input>` widgets
      the OS text-entry affordances already handle selection +
      Ctrl+X/C/V; this work is only for the rendered document.
  Paste is forms-only — there's nowhere to paste *into* on the
  render surface until `<input>` / `<textarea>` are editable;
  the bookkeeping for that lives behind Phase 7's input-event
  remaining work. Out of scope for this phase: rich-text /
  HTML clipboard payloads (selection copies plain text only),
  IME composition events.

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
- **Refuse to run as root** on Linux / macOS (`geteuid() == 0`)
  and as **Administrator** on Windows (token is a member of
  BUILTIN\Administrators via `CheckTokenMembership`). The startup
  check prints a message and exits 77. Override is
  `ND_ALLOW_ROOT=1` for the few legitimate uses (containers,
  sandboxes that drop caps elsewhere).
- **Linux Landlock filesystem sandbox.** On startup, after
  resolving the binary path, `src/security.c` installs a Landlock
  ruleset that grants the engine write access only to the user's
  XDG config / data / cache / runtime dirs plus `/tmp`. Reads on
  the rest of the filesystem are allowed (fonts, themes, library
  data). Execute is gated to the dir containing the binary itself
  so `nd_spawn_window`'s self-respawn still works. Disable with
  `ND_NO_SANDBOX=1`. Silently no-op on kernels without Landlock
  support, or non-Linux.

- **Content-Security-Policy.** The document's
  `Content-Security-Policy` response header is parsed into an
  `nd_csp` struct on each navigation, freed on the next. The
  CSP1+CSP2 subset is supported: `default-src`, `script-src`,
  `style-src`, `img-src`, `media-src`, `connect-src`,
  `font-src`, `frame-src`/`child-src`. Sources understood:
  `'self'`, `'none'`, `*`, scheme matches (`https:`, `data:`,
  …), host-only matches, and `*.example.com` wildcards.
  Stylesheets, images, and videos are gated at the kick stage
  before any fetch is dispatched; blocked subresources log a
  `CSP blocked: kind url` warning. Nonces, hashes,
  `'unsafe-inline'`, and `'strict-dynamic'` are treated as
  non-matching (conservative).
- **SOP / CORS** for JS-initiated fetches. `fetch()` and XHR
  responses are gated by comparing the document's origin
  against the response URL's origin; cross-origin responses
  without a matching `Access-Control-Allow-Origin` (or `*`)
  are exposed to JS as opaque — `status: 0`, empty body,
  `type: "opaque"`.

Remaining:

- Certificate pinning toggle (off by default)
- No third-party cookies by default
- CSP `report-uri` / `report-to` (not planned — adds network
  traffic in exchange for telemetry)

### Phase 10 — Media

Shipped so far (minimalist slice):

- `<video src="...webm">` and `<video><source src="..." type="video/webm">`
  recognized in HTML, laid out as a replaced block.
- Layout box `ND_BOX_VIDEO` with width/height attrs (defaults 320x180).
- `src/webm.c` — minimal EBML/Matroska demuxer (~330 lines): parses
  Segment, Info (TimecodeScale), Tracks (CodecID, PixelWidth/Height),
  and walks Cluster + SimpleBlock / BlockGroup to yield VP9 frames.
- `src/video.c` — libvpx VP9 decoder wrapper. Optional dependency
  (`vpx`, `required: false`) gated by `ND_HAVE_VPX`. Decodes the first
  frame on document install; YUV→RGBA conversion; result becomes
  `GdkTexture` painted into the video box.
- Paint: video texture if available, otherwise poster image, otherwise
  a dark placeholder with a centered play triangle.

Remaining for full playback:

- Playback timer that advances frames on a g_timeout / g_main loop tied
  to the per-frame timecode. Currently only the first frame paints.
- Play/Pause button overlay; status-bar progress.
- Vorbis or Opus audio decode (libvorbis or libopus); audio output via
  PipeWire (Linux) / CoreAudio (macOS) / WASAPI (Windows).

YouTube the website is intentionally out of scope: their player uses
MSE (Media Source Extensions) + DASH manifests + per-segment fetches,
which assumes a JS API surface much larger than Nordstjernen ships.
Direct `<video src="…webm">` URLs (including those served by
yt-dlp-style extractors) are in scope and work via the path above.

### Phase 11 — Distribution

- **Shareware distribution, Opera-style.** The browser is free
  to download and use. After a grace period a polite, dismissable
  nag appears asking the user to buy a license; the binary keeps
  working either way. No DRM, no online activation, no time bomb,
  no telemetry — the nag is the entire enforcement surface. The
  reference point is how Opera Software shipped Opera in the
  early years (paid registration removed the ad / nag, otherwise
  full functionality). Anything that would punish a non-paying
  user beyond the nag is out of scope.
- **Auto-updater with a monthly nag.** On launch (capped to once
  per 24h), fetch a small JSON manifest from the project's release
  hosting, compare the latest version with the running binary,
  and if older by more than 30 days show a non-blocking popup
  offering to download the newer build. Never auto-installs in
  the background — the user always confirms.
- **Downloadable Windows installer (.exe) and macOS .dmg.** The
  Windows build is wrapped into a signed installer with shortcut
  + uninstaller; the macOS build into a notarized DMG. The Linux
  build ships as a tarball with a desktop file; AUR / nixpkgs /
  homebrew are encouraged.
- **License-key flow.** Buying a license yields a key (string),
  entered in the chrome's About dialog or via the config file
  (`license_key = ...`). When a valid key is present, the nag is
  suppressed. Verification is local (signed key, public key
  baked into the binary) — no phone-home.

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

## jQuery support

Nordstjernen targets "simple jQuery JavaScript" — the kind that
selects elements, mutates the DOM, attaches event handlers, and
makes XHR/`$.ajax` calls. Concretely, the following surface is
wired and known to work end-to-end:

**Selection & traversal**
- `$('#id')`, `$('.cls')`, `$('div p')` — `document.querySelector`,
  `querySelectorAll`.
- `.parent()`, `.children()`, `.find()`, `.next()`, `.prev()`,
  `.siblings()`, `.closest(sel)` — `Element.parentNode`, `children`,
  `nextElementSibling`, `previousElementSibling`, `Element.closest`.
- `.is(sel)`, `.has(sel)` — `Element.matches(sel)`,
  `Element.contains(other)`.

**DOM manipulation**
- `.html()`, `.text()`, `.append()`, `.prepend()`, `.remove()`,
  `.empty()`, `.clone()`, `.replaceWith()` — `innerHTML`,
  `textContent`, `appendChild`, `removeChild`, `cloneNode(deep)`,
  `replaceWith`. Wrapper identity is preserved (`a.firstChild ===
  a.firstChild`); removed nodes invalidate any held JS wrapper
  rather than dangling.
- `.attr()`, `.removeAttr()`, `.prop()` — `getAttribute`,
  `setAttribute`, `removeAttribute`, ~70 reflected IDL attributes
  (`id`, `className`, `href`, `value`, etc.).
- `.addClass()`, `.removeClass()`, `.hasClass()`, `.toggleClass()` —
  `Element.classList.{add,remove,contains,toggle,replace}`.
- `.data('foo')` reads — `Element.dataset.foo` (kebab-to-camel).
- `.css('prop')` reads — `getComputedStyle(el).getPropertyValue()`
  for the styled properties; `el.style.prop` reads/writes for
  inline style.

**Events**
- `.on('click', fn)`, `.off()`, `.trigger('click')`,
  `.click(fn)` — `addEventListener` / `removeEventListener` /
  `dispatchEvent(new Event(type, {bubbles}))`.
- Inline `onclick="..."` / `onload="..."` etc. attribute handlers
  compile and fire during bubble phase, with `this` bound to the
  element and `event` available; returning `false` sets
  `defaultPrevented`.
- `$(function(){...})` / `$(document).ready(...)` —
  `DOMContentLoaded` is dispatched on `document` after the initial
  parse.

**AJAX / Network**
- `$.ajax`, `$.get`, `$.post`, `$(form).ajaxSubmit()` —
  `XMLHttpRequest` with `open` / `send`. `setRequestHeader(name,
  value)` is real: headers flow through to libcurl, and
  `X-Requested-With: XMLHttpRequest` is added automatically so
  server-side framework AJAX detection works.
- `new FormData(formElement)` populates entries from form
  controls (skipping submit/button/reset/file/image, honouring
  `checked` on radio/checkbox). `append`, `set`, `get`, `getAll`,
  `has`, `delete`, `forEach`, `keys`, `values`, `entries` all work.

**Geometry**
- `.width()`, `.height()`, `.offset()`, `.position()`,
  `.outerWidth()`, `.outerHeight()` — backed by
  `Element.offsetWidth`, `offsetHeight`, `offsetTop`,
  `offsetLeft`, `clientWidth`, `clientHeight` and
  `Element.getBoundingClientRect()`. These read the live layout
  tree; in scripts that run synchronously at parse time they
  return zeros (layout hasn't happened yet), but on
  `DOMContentLoaded`, `load`, `setTimeout(...,0)`, or any event
  handler firing after the initial layout, they return real
  pixel values.

**Utility / Promises**
- `Promise`, `Map`, `Set`, `WeakMap`, `WeakSet`, `Array.from`,
  `Array.prototype.includes/find/findIndex`, `Object.assign`,
  `Object.entries`, `Object.values`, `Object.keys`, async/await
  — all native via quickjs-ng.
- `JSON.parse`, `JSON.stringify` — native.
- `$.Deferred()` / jQuery's own utility helpers — pure JS, run
  on the engine.

**Known limitations relative to jQuery 3**

- `MutationObserver` is a no-op shell (some jQuery *plugins* rely
  on it; jQuery itself doesn't).
- `$.ajax({xhrFields:{withCredentials:true}})` — `withCredentials`
  is accepted but cookies don't flow through XHR yet (separate
  cookie-jar work).
- `responseType: 'arraybuffer'` / `'blob'` — only `'text'` and
  `'json'` are wired; binary responses go through a JS string.
- `$.getScript(url)` injects a `<script src=...>` that loads on
  the next layout tick; not via `eval` from the response body
  (CSP enforcement on scripts is still a TODO).
- Animations (`.fadeIn()`, `.animate()`) work as long as they
  drive `setTimeout` + style mutations — pure CSS transitions /
  `animation:` are not supported on the paint side.
- `$.ajax({contentType:'application/json'})` — set via
  `setRequestHeader('Content-Type', ...)`; the default for
  `nd_net_post_async` is still `application/x-www-form-urlencoded`
  but `setRequestHeader` overrides on the libcurl side.

## Test sites

Manual verification corpus. Ordered roughly by ascending pain: the
first ones must work for the browser to be useful at all; the last
ones are aspirational and will probably need future-phase support.

Update this list when a new site exposes a regression, when a
previously-broken site starts working, or when a target site changes
shape enough that the old URL no longer represents the test case.

The Tier 0 + Tier 1 lines below also live in machine-readable form
in `reading-list.txt` at the repo root. The `./dev smoke`
loop iterates that file through `nordstjernen --headless
--dump=text` and diffs the output against committed baselines.

### Tier 0 — should always work (Phase 0–3)

- `https://example.com` — IANA's canonical "is the network working?"
  page. Single short paragraph, one link, minimal styling.
- `https://lite.cnn.com` — text-only news mirror. Sanity check for
  the HTML parser on real-world markup.
- `https://text.npr.org` — same idea, slightly heavier nav.

### Tier 1 — heavier real-world pages

- `https://en.wikipedia.org/wiki/HTML5` — long article, headings,
  paragraphs, inline links, lists, tables.
- `https://news.ycombinator.com` — table-based layout, small CSS,
  user-generated text. Good stress test for inline formatting.
  **Treated as a primary correctness anchor** — the home page is
  representative of the dense, table-heavy, image-light pages
  Nordstjernen exists to render well, so any visible regression on
  HN should block a release. The current known gap is the
  upvote-arrow column: HN's external CSS sizes each
  `<div class="votearrow">` to 10×10 px and supplies the glyph
  via `background-image: url("grayarrow.svg")`. Until the
  renderer honours `width`/`height`/`background-image` on a
  non-replaced block, the column reserves its share of the
  table width and appears as a wide empty cell next to each
  story title.
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

- **Tabs — shipped.** Reverses the 2026-05-11 "no tabs" call.
  Each `GtkApplicationWindow` now hosts a tab strip (custom
  `GtkBox` inside the titlebar's `GtkHeaderBar`) plus a
  `GtkStack` of per-tab pages. Each tab is a full `nd_window`
  with its own toolbar / URL bar / history / DOM / layout / JS
  / CSP / images / videos / find state. `w->window` is the
  shared toplevel; the active tab is tracked via
  `g_object_get_data(toplevel, "nd-window")` and `win.*` actions
  are rebound to it on every switch. Ctrl+T opens a new tab,
  Ctrl+W closes the active tab (destroys the window when the
  last tab closes), Ctrl+N still spawns a separate OS process.
  Open polish: keyboard cycling (Ctrl+PgUp / Ctrl+PgDn,
  Ctrl+1..9), tab context menu (close-other, duplicate),
  drag-to-reorder, undo-close-tab, per-tab process isolation.

- **Enable `-Wcast-qual` cleanly.** Currently ~78 warnings if added to
  the warning set — mostly `(nd_node *)` casts in `js.c` stripping
  `const` from `nd_unwrap_element` results. Fix the offenders one at
  a time (or split into `nd_unwrap_element_mut` for the writable
  path), then add the flag in `meson.build`.
- **Run Claude on Windows — shipped.** The autonomous-dev loop now
  works from a Windows 11 box via MSYS2 / MINGW64 with the same
  packages the CI workflow installs. `meson setup` + `meson compile`
  build a clean `nordstjernen.exe`; `pack-windows.sh` produces a
  redistributable `dist/nordstjernen-win64/` bundle. See
  `docs/Windows.md`.
- **gumbo-parser is now the only HTML parser.** The hand-rolled
  tokenizer in `src/html.c` is gone; `nd_html_parse` and
  `nd_html_parse_for_page` both go through gumbo. `libgumbo` is
  a required dependency (system pkg-config first, wrap fallback).
  The previous `ND_HTML_PARSER=gumbo` toggle and the
  `html_parser` config field were dropped. Rationale: HTML5's
  parsing algorithm is intricate enough that a 500-line
  hand-roll always missed real-world edge cases; the real-world
  pages on the reading list parse more cleanly through gumbo,
  and "one less parser" is one less surface to maintain.
- **muPDF for the PDF viewer.** We already export pages to PDF
  via Cairo. The complement is rendering `application/pdf` pages
  inline rather than handing them to the OS viewer. muPDF is a
  small C library that fits the project's audit-the-deps rule.
  Decide later whether it ships statically linked or vendored as a
  meson subproject.
- **HTTP cache — shipped.** See `src/cache.[ch]` and the
  iteration log below. Plain-file cache under
  `$XDG_CACHE_HOME/nordstjernen/cache/<aa>/<rest>.meta`
  + `.body`. `Cache-Control: max-age` / `no-cache` /
  `no-store` / `immutable`, `Expires`, `ETag`,
  `Last-Modified` all honoured; conditional GETs on
  stale entries; 304s promote the stored body and
  refresh the freshness window; 256 MB LRU cap with
  oldest-mtime eviction. `ND_NO_CACHE=1` disables it.
- **Threads.** The engine is single-threaded today; libcurl
  fetches go via GTask but everything else (HTML parse, CSS
  cascade, layout, paint, JS) runs on the GTK main loop. Identify
  the first thing that's worth moving off — likely image decode
  or large-stylesheet parsing — and introduce a single worker
  thread for it before going wider. Threads are a force multiplier
  *and* a debugging hazard; add them deliberately, not preemptively.
- **Plug `nd_js` teardown leaks.** quickjs-ng v0.14.0
  `JS_FreeRuntime` asserts `list_empty(&rt->gc_obj_list)`. We
  hit this on real-world JS pages when navigation calls
  `nd_js_free` while a `fetch()` promise's `resolve`/`reject`
  JSValues, an XHR's `obj`, or a queued microtask is still
  live. Production builds compile the assertion out via
  `--buildtype=release` (NDEBUG), but the underlying object
  leak is real. Plan: track every in-flight `nd_js_fetch_state` /
  `nd_xhr_state` on the `nd_js` itself, and on `nd_js_free`
  cancel the underlying GTask, JS_FreeValue resolve/reject/obj,
  and drop the state. Same treatment for any cached JSValue
  fields (`document`, `location`, …) that the engine binding
  layer keeps strong refs to outside the listeners array.
- **Shareware (now the project's distribution plan).**
  Promoted from idea to Phase 11 — see that section for the full
  shape. Brief recap: free download, free use, polite nag asking
  the user to buy a license, binary keeps working either way.
  Reference is how Opera Software shipped Opera in its early
  years.
- **Config file — shipped.** `~/.config/nordstjernen/nordstjernen.conf`,
  flat `key = value` lines, `#` comments. See `src/config.[ch]` and the
  iteration log below. Defaults → file → env override order.
  `nordstjernen --print-config` dumps the effective config.
- **Headless mode — shipped.** `--headless --dump=<fmt>
  <url>`. See `src/headless.[ch]` and the iteration log
  below. Drives the existing engine — `nd_net_fetch_async`
  / `nd_js_run_scripts_in_doc` / `nd_css_compute` /
  `nd_layout_build` / `nd_paint` — against a plain
  GMainLoop with no GTK widget. Output formats: text,
  dom, layout, png:<path>, pdf:<path>.

  from `nd_html_decode_body`. uchardet is now a required
  dependency and handles all detection; the function loses its
  `content_type` parameter. The ISO-8859-1 fallback remains as
  a last-ditch path when uchardet can't classify the bytes.
