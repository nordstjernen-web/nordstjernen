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
  An About button in the header bar opens `about:nordstjernen`, which
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
    * Phase 11 — distribution: Opera-style shareware (free
      download, polite nag, license key suppresses the nag),
      monthly-nag auto-updater, signed Windows .exe / macOS
      .dmg.
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
- 2026-05-12 — JS: Element.outerText aliased to
  textContent (read & write).
- 2026-05-12 — Layout: <input type=password> renders
  its value as U+2022 bullets (one per codepoint) so
  the page doesn't leak the password back to the
  viewer.
- 2026-05-12 — CSS: selector type-name match is ASCII
  case-insensitive (HTML tag names).
- 2026-05-12 — CSS: :nth-of-type(an+b | odd | even) —
  same parser as :nth-child, but the index counts
  same-tag-name siblings instead of all siblings.
- 2026-05-12 — JS: document.createElementNS(ns, name)
  ignores the namespace (we're HTML-only) and falls
  through to createElement(name).
- 2026-05-12 — Test sites: motherfuckingwebsite.com
  dropped from the Tier-0 list. (Same idea as
  example.com, profanity in the URL is gratuitous for
  the iteration log.)
- 2026-05-12 — Twenty low-hanging-fruit tasks shipped in
  one batch:
    * window.self / top / parent / globalThis / frames
      / length aliased to window.
    * window.getSelection() returns a stubbed
      Selection (collapsed range count 0, no-op
      methods).
    * window.requestIdleCallback / cancelIdleCallback
      no-ops.
    * document.scripts collection (recursive walk over
      <script> elements).
    * document.styleSheets / embeds / plugins return
      empty arrays.
    * document.designMode getter returns "off".
    * document.write / writeln / open / close /
      execCommand / exitFullscreen no-ops.
    * document.fullscreenElement = null,
      fullscreenEnabled = 0, scrollingElement = body.
    * Element.toggleAttribute(name, force?) (returns
      the resulting boolean per spec).
    * Element.getAttributeNS / hasAttributeNS /
      setAttributeNS / removeAttributeNS — namespace
      argument ignored, forwarded to the non-NS
      variants.
    * Element.requestFullscreen no-op.
    * Element.getAnimations() → []; Element.animate()
      returns a stub Animation with play / pause /
      cancel / finish / reverse / playState / finished.
    * reset-button.click() walks the enclosing form
      and clears value / checked / selected (analogous
      to submit-button.click() submitting).
    * (alert already routes through the same log_cb
      as console — verified, no change needed.)
- 2026-05-12 — A hundred-plus more low-hanging-fruit
  shims landed in one batch. Headlines:
    * window: screen / screenX / screenY / screenLeft /
      screenTop, screen.orientation, isSecureContext,
      origin, name (settable), status, closed, opener,
      event, indexedDB. structuredClone (JSON
      round-trip), reportError, requestIdleCallback /
      cancelIdleCallback, getSelection() Selection stub.
    * Constructors: MessageChannel (port1 / port2 stubs),
      BroadcastChannel, Notification. Worker /
      SharedWorker / WebSocket / EventSource throw
      "not supported" cleanly so feature detection works.
    * CSS object: CSS.supports() (returns true) and
      CSS.escape() (real ASCII escape).
    * crypto.subtle: digest / encrypt / decrypt / sign /
      verify / generateKey / importKey / exportKey /
      deriveBits / deriveKey all return rejected
      Promises.
    * caches global with open / has / delete / keys /
      match (all reject).
    * AbortSignal static methods (abort / timeout / any).
    * PerformanceObserver stub with observe / disconnect
      / takeRecords.
    * Document: lastModified, document.all, anchors,
      applets, fonts (with ready Promise, check, load,
      add, delete, clear, size). implementation
      (hasFeature etc.). write / writeln / open / close
      / execCommand / exitFullscreen no-ops, hasFocus
      → true, elementFromPoint / elementsFromPoint
      degenerate to body, createRange / createTreeWalker
      / createNodeIterator stubs, adoptNode / importNode
      (importNode actually clones).
    * Element: getRootNode, isEqualNode / isSameNode,
      compareDocumentPosition (0 stub), lookupPrefix /
      lookupNamespaceURI (null), isDefaultNamespace
      (true), getClientRects (wraps
      getBoundingClientRect), scrollBy / scrollTo /
      scroll / scrollIntoViewIfNeeded /
      requestPointerLock / releasePointerLock /
      setCapture / releaseCapture no-ops, scrollTop /
      scrollLeft set-no-op so assignments don't error.
    * Text / Comment CharacterData: .length plus
      substringData / appendData / deleteData /
      insertData / replaceData / splitText backed by
      proper UTF-8 offset math.
    * Form / input helpers: select / setSelectionRange
      / setRangeText / stepUp / stepDown no-ops;
      validity / validationMessage / willValidate /
      labels / files / indeterminate / selectionStart
      / selectionEnd / selectionDirection /
      defaultValue / defaultChecked / defaultSelected.
    * HTMLMediaElement stubs: play (resolved Promise) /
      pause / load / canPlayType / fastSeek /
      addTextTrack; currentTime / duration / paused /
      ended / seeking / volume / playbackRate / muted /
      readyState / networkState; seekable / buffered /
      played / textTracks / videoTracks / audioTracks
      empty arrays.
- 2026-05-12 — Plan: HTTP cache promoted into the ideas
  backlog. Every navigation / image / stylesheet /
  script currently re-hits the network; the project
  needs a disk-backed cache under XDG_CACHE_HOME with
  Cache-Control / ETag / Last-Modified, an LRU bound,
  and a back/forward fast-path that doesn't re-fetch.
- 2026-05-12 — A second hundred-plus low-hanging-fruit
  batch landed. Headlines:
    * navigator: deviceMemory, pdfViewerEnabled,
      webdriver, userAgentData (brands/mobile/platform/
      getHighEntropyValues). connection (effectiveType
      / type / downlink / rtt / saveData /
      addEventListener / removeEventListener).
      geolocation (getCurrentPosition / watchPosition /
      clearWatch), clipboard (writeText / readText /
      write / read all reject), permissions
      (query / request reject), mediaDevices
      (getUserMedia / getDisplayMedia / enumerateDevices
      / getSupportedConstraints). share / canShare /
      vibrate / sendBeacon / registerProtocolHandler /
      unregisterProtocolHandler.
    * window: find / moveTo / moveBy / resizeTo /
      resizeBy no-ops.
    * URL: static canParse, parse, createObjectURL,
      revokeObjectURL.
    * performance: timing (21 zeroed fields), navigation
      (type / redirectCount), memory (jsHeapSizeLimit /
      totalJSHeapSize / usedJSHeapSize), eventCounts,
      clearResourceTimings, setResourceTimingBufferSize,
      toJSON.
    * Event subclass aliases: ProgressEvent / ErrorEvent
      / HashChangeEvent / PopStateEvent / MessageEvent /
      StorageEvent / PageTransitionEvent / BeforeUnloadEvent
      / SubmitEvent / InputEvent / TouchEvent / DragEvent
      / WheelEvent / FocusEvent / AnimationEvent /
      TransitionEvent / ClipboardEvent / CompositionEvent
      / PointerEvent / UIEvent / CloseEvent /
      MediaQueryListEvent / BlobEvent /
      FontFaceSetLoadEvent / GamepadEvent /
      DeviceMotionEvent / DeviceOrientationEvent /
      PromiseRejectionEvent /
      SecurityPolicyViolationEvent. All route through
      nd_window_event_ctor.
    * Interface placeholders: EventTarget / Node /
      Element / HTMLElement / Document / HTMLDocument
      / Window (so `instanceof` checks at least walk a
      JS function).
    * Attribute aliases: crossOrigin / referrerPolicy /
      decoding / loading / fetchPriority / sizes /
      srcset / useMap / inputMode / size / cols / rows
      / maxLength / minLength / coords / shape /
      formAction / formMethod / formEnctype /
      formTarget / integrity / kind / hreflang /
      content / httpEquiv / contentEditable / slot /
      role + 13 aria-* aliases.
    * Boolean attribute aliases: isMap / draggable /
      reversed / playsInline / inert / noModule /
      formNoValidate.
    * Element: isContentEditable / translate /
      offsetParent / videoWidth / videoHeight /
      clientInformation, decode() (resolved promise),
      toBlob no-op, attachInternals throws.
    * <option>.index getter (walks select to find
      ordinal), <select>.length (counts options),
      <form>.length (forwards to elements.length).
    * <table>.rows / caption / tHead / tFoot / tBodies;
      <tr>.cells; rowIndex / sectionRowIndex /
      cellIndex / colSpan / rowSpan. createCaption /
      createTHead / createTFoot / createTBody /
      deleteCaption / deleteTHead / deleteTFoot /
      insertRow / deleteRow / insertCell / deleteCell
      / namedItem / item / add no-ops.
    * Document: queryCommandSupported / queryCommandEnabled
      / queryCommandState / queryCommandValue,
      currentScript = null, rootElement aliases
      documentElement.
    * Console: profile / profileEnd / timeStamp /
      context no-ops, console.memory empty object.
- 2026-05-12 — Third round of LHF (~100):
    * <sup> / <sub> render with proper baseline rise +
      0.75× scale via PangoAttr_rise + PangoAttr_scale.
    * font-variant: small-caps maps to
      PangoVariant.SMALL_CAPS for inline runs.
    * CSS border-radius — block backgrounds are drawn
      with cairo arcs at each corner. (Length-only; the
      4-corner shorthand isn't decoded yet.)
    * 60+ interface placeholder constructors for
      instanceof / typeof checks: Range, NodeFilter,
      DOMException, DOMTokenList, NodeList,
      HTMLCollection, CSSStyleSheet, CSSStyleDeclaration,
      CSSRule, CSSStyleRule, MediaList, MediaQueryList,
      ShadowRoot, Selection, Animation, Headers, Request,
      Response, Blob, File, FileReader, FileList, Storage,
      Text, Comment, Attr, DocumentFragment,
      DocumentType, XMLSerializer, XMLDocument,
      XSLTProcessor, and 40+ specific HTMLxxxElement
      classes, plus DOMRect / DOMPoint / DOMMatrix /
      DOMQuad / DOMStringList / DOMStringMap /
      NamedNodeMap / TreeWalker / NodeIterator /
      MutationRecord / IntersectionObserverEntry /
      ResizeObserverEntry / PerformanceEntry / Mark /
      Measure / ResourceTiming / NavigationTiming /
      FontFace / FontFaceSet / ReadableStream /
      WritableStream / TransformStream / *QueuingStrategy
      / ServiceWorker / ServiceWorkerRegistration /
      ServiceWorkerContainer / Geolocation /
      GeolocationPosition / Permissions / PermissionStatus
      / MediaSession / Crypto / SubtleCrypto / CryptoKey
      / CryptoKeyPair / HTMLOptionsCollection /
      HTMLAllCollection / RadioNodeList / TextMetrics /
      CanvasRenderingContext2D / ImageData / ImageBitmap
      / OffscreenCanvas / Path2D / ValidityState.
    * Document: defaultView (alias of window), location,
      ownerDocument = null, nodeName = "#document",
      nodeType = 9, doctype = null, xmlVersion /
      xmlEncoding / xmlStandalone, inputEncoding.
    * CSSStyleDeclaration: length / parentRule /
      cssFloat / item() / getPropertyPriority().
    * Element: valueAsNumber (parses input.value as
      double, NaN on miss), valueAsDate = null,
      encoding (alias of enctype) — plus the existing
      validity surface.
    * navigator: plugins / mimeTypes empty arrays with
      namedItem / refresh. javaEnabled / taintEnabled /
      getAutoplayPolicy no-ops. getBattery /
      requestMIDIAccess / requestMediaKeySystemAccess
      reject. getGamepads returns [].
- 2026-05-12 — Docs sweep. All references to webmosilla,
  Firefox 1.0, Mozilla, and the AI-gated download flag
  removed from README.md / CLAUDE.md / NORDSTJERNEN.md /
  src/net.c. `about:mozilla` renamed to `about:nordstjernen`
  throughout (the about-page itself, the chrome menu, the
  reading-list, the headless test references). Phase 11
  rewritten as Opera-style shareware: free download, polite
  nag, license key suppresses the nag, binary keeps working
  either way. No DRM, no online activation. README rewritten
  to describe the project on its own terms without comparing
  to any other browser.
- 2026-05-12 — Flex layout (row direction). `display: flex`
  containers run a dedicated layout path instead of falling
  through to vertical block flow. Supported: flex-direction
  (row / row-reverse — column falls through to existing
  block flow), gap / column-gap, flex-grow, flex-basis,
  width as an explicit basis. justify-content: flex-start /
  flex-end / center / space-between / space-around /
  space-evenly. align-items: stretch (default) / flex-start
  / center / flex-end. `flex` shorthand parsed
  (flex: <grow> <shrink> <basis> | none | auto | initial).
  `flex-flow` shorthand parsed. Items without explicit
  flex-basis / width use a per-character natural-width
  estimate so non-growing items don't collapse to zero.
  Verified via the headless mode: a four-row flex page
  (flex-grow / space-between / center / `flex: 1` equal
  columns) renders correctly; about:nordstjernen is unchanged.
  Limitations: flex-direction: column still falls through
  to block layout, flex-wrap is parsed but no wrap pass
  yet, flex-shrink is parsed but unused (items overflow
  rather than shrink when total basis > container).
- 2026-05-12 — Headless mode shipped (`src/headless.[ch]`).
  `nordstjernen --headless --dump=<text|dom|layout|png:<path>|
  pdf:<path>> [--viewport=N] [--settle-ms=N] <url>`.
  Drives the existing engine end-to-end against a plain
  GMainLoop with no GTK widget — fetch via
  `nd_net_fetch_async`, parse via `nd_html_parse_for_page`,
  JS via `nd_js_run_scripts_in_doc` (gated by
  cfg->javascript_enabled), cascade via `nd_css_compute`,
  layout via `nd_layout_build`, paint via the existing
  `nd_paint` into either a `cairo_image_surface_create`
  (PNG) or `cairo_pdf_surface_create` (PDF). Verified on
  about:nordstjernen for all five dump formats; PNG matches the
  GUI render. JS shutdown skipped at exit to avoid a
  QuickJS GC assert in one-shot mode; the OS reclaims.
  Unlocks scripted regression testing against a URL
  corpus without violating the no-`tests/` policy.
- 2026-05-12 — Config file shipped (`src/config.[ch]`).
  Flat key/value file at
  `$XDG_CONFIG_HOME/nordstjernen/nordstjernen.conf`, `#`
  comments, `key = value` lines. Loaded once at startup;
  defaults → file → environment-variable override. Keys:
  home_url / user_agent / accept_language / search_engine
  / referer_policy / cookie_policy / html_parser /
  do_not_track / javascript_enabled / images_enabled /
  local_storage_enabled / cache_enabled / cache_cap_mb /
  default_font_size_px / js_eval_budget_ms /
  js_memory_cap_mb. Wired into net.c (UA + Accept-Language
  + DNT + referer policy), cache.c (enabled + cap), js.c
  (eval budget + memory cap + local storage), image.c
  (image fetch gate), html.c (primary vs gumbo). New
  `nordstjernen --print-config` CLI flag dumps the
  effective config with the source file path commented at
  the top. Replaces the older `home.txt` (still read as
  fallback) and the ad-hoc env vars (still honored, but
  now route through config_apply_env).
- 2026-05-12 — HTTP cache shipped (`src/cache.[ch]`).
  Plain-file design: `$XDG_CACHE_HOME/nordstjernen/cache/`
  with a sha256(url) key, partitioned into two-char
  subdirs. Two files per entry: `<key>.meta` (plain text,
  `key: value\n` lines for url / final_url / status /
  content_type / etag / last_modified / expires_at /
  fetched_at) and `<key>.body` (raw response bytes).
  Lookup at the top of nd_fetch_sync — fresh entries
  short-circuit the network. Stale entries with ETag /
  Last-Modified add `If-None-Match` / `If-Modified-Since`
  to the request; 304 promotes the stored body and
  refreshes the freshness window. 200 responses with
  cacheable Cache-Control are stored. Cache-Control:
  no-store skips the put; no-cache stores but forces
  revalidation; max-age / immutable / Expires set
  freshness. 256 MB LRU cap, oldest mtime evicted first.
  `ND_NO_CACHE=1` disables. about: / file: URLs and POST
  responses bypass the cache.
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
- 2026-05-12 — Windows build verified locally end-to-end.
  Toolchain via `winget install MSYS2.MSYS2` plus the same
  mingw-w64-x86_64-{clang,meson,ninja,pkgconf,gtk4,curl,
  gumbo-parser,libvpx} packages the CI workflow installs.
  `meson setup builddir && meson compile -C builddir`
  produces `nordstjernen.exe` (~7 MB) cleanly under
  clang 22.1.4; quickjs-ng v0.14.0 is auto-fetched into
  `subprojects/quickjs-0.14.0/` and static-linked. The
  `--print-config`, `--headless --dump=text`, and the
  GTK 4 GUI paths all work. Promotes the "Run Claude on
  Windows" idea-backlog item from idea to verified.
- 2026-05-12 — Windows portable bundle:
  `pack-windows.sh` builds a redistributable
  `dist/nordstjernen-win64/` (73 DLLs, ~76 MB) that runs
  on a Windows machine with no MSYS2 install. Bundles the
  exe + every transitively-imported mingw64 DLL (resolved
  via `objdump -p`) + GLib schemas + GDK-Pixbuf loaders +
  Adwaita & hicolor icons + libcurl CA bundle, plus a
  `nordstjernen.cmd` launcher that pins the runtime env
  vars to the bundle's own paths. Headless and GUI
  smoke-tested with the MSYS2 mingw64 bin dir removed
  from `PATH`. The full installer + code signing is still
  Phase 11; this is the copy-and-run intermediate. See
  `docs/Windows.md` for the recipe.
- 2026-05-12 — Phase 9: refuse-to-run-as-root extended to
  Windows. `nd_security_refuse_root` now also calls
  `CheckTokenMembership` against the builtin Administrators
  SID; an elevated process exits 77 unless `ND_ALLOW_ROOT=1`
  is set. The Linux/macOS path is unchanged.
- 2026-05-12 — Plan: text selection + copy/paste on the render
  surface added as a Phase 6 polish deliverable. The render
  area is a custom-drawn `GtkDrawingArea`, so the OS-native
  selection affordances don't apply; need a selection model
  over the layout tree's text boxes plus clipboard wiring
  via `gdk_display_get_clipboard()`. See the Phase 6 entry
  for the design sketch.
- 2026-05-12 — Windows portable bundle: resolve the CA bundle
  relative to the exe at `nd_net_init` time and apply via
  `CURLOPT_CAINFO`. Fixes "error adding trust anchors from
  file: ca-bundle.crt" when running the bundled
  `nordstjernen.exe` directly on a fresh Windows machine — the
  mingw libcurl's compile-time default points into
  `C:/msys64/mingw64/...` which doesn't exist outside the dev
  box. Resolution order is `CURL_CA_BUNDLE` /
  `SSL_CERT_FILE` env vars, then four well-known relative
  paths under the exe dir. See `docs/Windows.md` "CA bundle"
  for what the file actually is and why Windows needs it
  bundled when Linux / macOS don't.
- 2026-05-12 — Windows: suppress the benign GLib
  `win32 session dbus binary not found` startup warning.
  `main.c` installs a `g_log_set_writer_func` on Windows that
  filters that exact message and forwards everything else
  through `g_log_writer_default`. Nordstjernen doesn't use
  D-Bus on Windows, so shipping `dbus-daemon.exe` to satisfy
  the lookup would add weight for no functional gain.
- 2026-05-12 — `pack-windows.sh` builds (or reuses) a
  separate `builddir-release/` tree with
  `--buildtype=release` so `NDEBUG` is defined when the
  vendored deps compile. Specifically suppresses the
  unconditional `assert(list_empty(&rt->gc_obj_list))` in
  quickjs-ng v0.14.0 `JS_FreeRuntime` (`quickjs.c:2323`),
  which fires on any leaked JS object at context teardown
  — easy to hit on real-world JS-heavy pages whose
  in-flight `fetch()` promises or event-handler closures
  still hold JSValues when navigation tears the document
  down. The actual leak is tracked in the ideas backlog
  ("Plug nd_js teardown leaks") and is independent of
  shipping.
- 2026-05-12 — Plug in-flight fetch/xhr leaks at JS
  context teardown. `nd_js` gains `pending_fetches` and
  `pending_xhrs` arrays; `fetch()` and XMLHttpRequest
  register their state on dispatch and unregister on
  completion. `nd_js_free` walks both lists,
  `JS_FreeValue`s `resolve`/`reject`/`obj`, and detaches
  `ctx`/`js` so the eventual async callback runs as a
  free-only no-op. Removes the actual leak that
  the release-mode workaround was hiding.
- 2026-05-12 — Phase 6 click handling: when a click lands
  inside a container element with no text-like ancestor,
  walk down into the container and focus the first
  text-like descendant `<input>`/`<textarea>`. Fixes
  click-to-focus on pages whose form input is laid out as
  inline content with no own box (e.g., DuckDuckGo's
  `/lite/` form). The deeper fix — giving text inputs
  their own layout boxes for precise per-input hit
  testing — is still pending.
- 2026-05-12 — Domain registered: `nordstjernen.org`.
  User-Agent updated from
  `+https://github.com/operativsystem42/nordstjernen` to
  `+https://nordstjernen.org`; `about:nordstjernen` page
  adds a Project home + Source code line; README's
  "Project home" line now points at the domain (GitHub
  link reduced to "Source code"). Website plan lives in
  `docs/nordstjernen.org.md` — single static page to start,
  then download / license / manifest endpoints that unblock
  Phase 11 (auto-updater + license-key flow).
- 2026-05-12 — Phase 4 table layout: content-aware
  column widths. The pre-existing "every column gets
  `cw / max_cols`" rule mangled real-world tables — HN's
  three-column story row (rank, votearrow, title) ended
  up with three equal 299 px cells, hiding the title
  behind a giant empty votearrow cell. New
  `measure_natural_width` Pango-measures each cell's
  intrinsic width without wrapping (inlines run
  `pango_layout_get_pixel_size`; blocks recurse and take
  max child width; inline siblings sum). `layout_table`
  then allocates each column its max natural width,
  scales down proportionally if the sum exceeds the
  available width, and donates any leftover space to the
  widest column. HN renders correctly: rank ≈ 21 px,
  votearrow ≈ 14 px, title takes the rest. lite.cnn.com /
  DDG verified unaffected — they don't depend on
  uneven-column tables.
- 2026-05-13 — Per-site compatibility framework. `src/google.{c,h}`
  generalized into `src/compatibility.{c,h}`: a rules table binds
  each host matcher to an optional User-Agent, an optional CSS
  override, and an optional DOM rewriter. Built-in rules cover
  google, duckduckgo, wikipedia, aftenposten, reddit; the matching
  stylesheets live in `compatibility-css/` and are appended to the
  cascade after the page's own inline + external sheets so they
  override on equal specificity. Lookup order: user data dir →
  dev-relative paths → installed `$datadir/nordstjernen/`. Google
  redirect-link unwrap and `consent.google.com` URL unwrap are
  preserved.
- 2026-05-13 — Lexbor as alternative HTML/DOM backend.
  `subprojects/lexbor.wrap` (CMake subproject; system probe first)
  and `-Dlexbor={auto|enabled|disabled}` meson option add
  [lexbor v2.4.0](https://github.com/lexbor/lexbor) alongside
  gumbo. New `src/html_lexbor.c` walks the lexbor DOM into the
  shared `nd_node` tree; `nd_html_parse_with(engine, …)` and
  `nd_html_parse_for_url(url, …)` dispatch between backends.
  Runtime toggle via `ND_HTML_ENGINE=gumbo|lexbor`. Both engines
  share the rest of the pipeline (CSS cascade, layout, paint, JS)
  so cross-checks are apples-to-apples.
- 2026-05-13 — Per-site UA and parser overrides moved to GKeyFiles.
  `compatibility-css/user-agents.conf` (`[user-agents]` keyed by
  rule id) replaces the hard-coded UA constants in compatibility.c.
  `compatibility-css/html-engines.conf` (`[html-engines]`, values
  `gumbo` / `lexbor` / `default`) lets each rule pin the parser for
  its host; the dispatch falls back to the global default when no
  override is set, and silently downgrades lexbor → gumbo if the
  build didn't include lexbor.
- 2026-05-13 — Lexbor walker hardened. The recursive `lxb_to_nd`
  became an iterative `lxb_walk_into` with an explicit GQueue
  stack, so deeply nested DOMs don't risk the C stack. `<template>`
  elements now descend into their `lxb_html_template_element_t::content`
  document fragment, so shadow children show up in the `nd_node`
  tree instead of being dropped (this is a genuine improvement over
  the gumbo path, which still drops them). `LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT`,
  `_DOCUMENT_TYPE`, `_PROCESSING_INSTRUCTION`, `_ATTRIBUTE`,
  `_ENTITY{,_REFERENCE}`, and `_NOTATION` are now enumerated
  explicitly (still skipped, but no longer hide in `default`).
- 2026-05-13 — Gumbo path levelled up to match lexbor. `gumbo_to_nd`
  is now an iterative `GQueue`-driven walker (mirrors the lexbor
  rewrite); pathological deep DOMs can't blow the C stack through
  either backend. `GUMBO_NODE_TEMPLATE` is folded into the element
  case — it carries a `GumboElement` per the gumbo header — so its
  children show up in the `nd_node` tree instead of being dropped.
  `gumbo_convert_self` and `gumbo_children_vector` split node
  conversion from child enumeration, which is the pattern the
  lexbor walker uses; both engines now share the same shape.
- 2026-05-13 — Fragment parser uses HTML5 fragment context. The
  parse-the-whole-document-then-strip trick in
  `nd_html_parse_fragment` is replaced with a context-aware
  `nd_html_parse_fragment_in(context_tag, body, len)` that routes
  through the engine dispatch like the document parser. The gumbo
  backend sets `GumboOptions.fragment_context` so the parser uses
  the right HTML5 insertion modes; the lexbor backend calls
  `lxb_html_parse_fragment_by_tag_id` with the corresponding tag
  id. Default context is `<body>`, matching the previous behaviour
  but at least with correct insertion-mode tracking. Plumbing
  through a real context element from `innerHTML`/`insertAdjacentHTML`
  callsites in `js.c` is the next step and unblocks correct
  `<table>` / `<select>` fragments.
- 2026-05-13 — CI off. Scheduled cron triggers removed from
  `.github/workflows/{linux,macos,windows}.yml`; only
  `workflow_dispatch:` remains. Nothing on GitHub Actions runs
  automatically. CLAUDE.md "Autonomous mode" updated accordingly.
- 2026-05-13 — innerHTML / outerHTML / insertAdjacentHTML pass the
  HTML5 fragment context through. `element.innerHTML = …` parses
  with the element's own tag as context (so `<table>.innerHTML =
  "<tr>…"` slots into `<tbody>` correctly); `outerHTML` parses
  with the element's parent tag as context (since the fragment is
  inserted as a sibling); `insertAdjacentHTML` picks parent vs.
  self based on whether the position is adjacent to the element
  (`beforebegin` / `afterend`) or inside it (`afterbegin` /
  `beforeend`). The context propagates through both gumbo and
  lexbor backends via `nd_html_parse_fragment_in`.
- 2026-05-13 — Lexbor becomes the default HTML engine when built
  in; gumbo stays as the fallback. `ND_HTML_ENGINE=gumbo|lexbor`
  still switches at runtime.
- 2026-05-13 — Gumbo source switched to the maintained codeberg
  fork (`codeberg.org/gumbo-parser/gumbo-parser`) via
  `subprojects/gumbo.wrap`, pinned to release tag `v0.13.2`.
  System pkg-config `gumbo` still satisfies the dep first.
- 2026-05-13 — Ada-url integration: `nd_url_resolve` /
  `nd_url_origin_from` / `nd_url_host_from` in `src/net.c` route
  through the WHATWG URL library when built in (`-Dada=auto`,
  singleheader fetched by `subprojects/ada.wrap`); the
  hand-rolled fallback is preserved for builds without ada.
- 2026-05-13 — Security: `X-Frame-Options` and CSP
  `frame-ancestors` parsed and combined in
  `nd_response_allows_framing()` (`src/net.c`) and
  `nd_csp_frame_ancestors_allows()` (`src/csp.c`). No iframe
  rendering yet, so the check is wired but inert; activates the
  moment iframes ship.
- 2026-05-13 — Tabs: each `GtkApplicationWindow` now hosts a
  per-tab `GtkStack` and a tab strip in the titlebar (custom
  `GtkBox` inside a `GtkHeaderBar`). Each tab is a full
  `nd_window` with its own toolbar / URL bar / history / DOM /
  layout / JS / CSP / images / videos / find state; `w->window`
  is the shared toplevel. Ctrl+T new tab, Ctrl+W close tab,
  Ctrl+N still spawns a separate OS process. `win.*` actions
  are rebound to the active tab on every switch (with
  `g_action_map_remove_action` first to avoid leaking the prior
  action objects).
- 2026-05-13 — Build hygiene: `subprojects/gumbo.wrap` pinned to
  `v0.13.2`. `./dev build` runs `meson setup` (only if needed)
  plus `meson compile -C builddir` in one shot. CLAUDE.md
  documents that meson auto-detects `ccache`, dropping the
  cold-rebuild time from ~35 s to ~1 s once the cache is warm.
- 2026-05-13 — Charset detection simplified: removed the
  hand-rolled BOM / HTTP Content-Type / `<meta>` sniff chain
  from `nd_html_decode_body`. uchardet is now a required
  dependency and handles all detection; the function loses its
  `content_type` parameter. The ISO-8859-1 fallback remains as
  a last-ditch path when uchardet can't classify the bytes.
