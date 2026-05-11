# Nordstjernen — Development Plan

Live roadmap for the clean-room implementation. See `README.md` for
product vision and `CLAUDE.md` for working agreements.

Nordstjernen is a clean-room reimplementation inspired by Firefox 1.0
(via the `webmosilla` fork) — AI reads that code for orientation only;
no source is copied. The plan is to first match the feature set of
Firefox 1.0, then modernize to a usable browser with HTML5 and a
pragmatic subset of modern JavaScript and CSS.

## Current status

**Phase 1 — Networking.** Phase 0 scaffold is in. `src/net.c` wraps
libcurl with an async GTask-based `nd_net_fetch_async`; the GTK 4
shell has an address bar + Load/Stop and renders the raw response
body in a monospace `GtkTextView`.

## Guiding principles

- **One very competent human's worth of code.** Whenever a phase risks ballooning, cut
  scope, not corners. A working subset > an unfinished superset.
- **Vertical slices.** Each phase should end with something that can be
  run and demoed end-to-end, not just a library waiting for a caller.
- **No third-party engines we haven't read.** Vendor dependencies must
  be small, embeddable, and auditable.

## Phases

### Phase 0 — Bootstrap (current)

Deliverables:

- [x] README, CLAUDE.md, .gitignore, .gitattributes
- [x] `linux.yml` CI verifying GTK 4 + libcurl + meson toolchain
- [x] `meson.build` producing a `nordstjernen` binary
- [x] `src/main.c` opening an empty GTK 4 `GtkApplicationWindow`
- [x] CI builds and uploads the binary as an artifact
- [x] macOS and Windows CI stubs (toolchain check only)

Done when: `meson compile -C builddir && ./builddir/src/nordstjernen`
opens a blank window on Linux, and CI is green.

### Phase 1 — Networking

Deliverables:

- [x] libcurl wrapper: `nd_net_fetch_async` → `nd_response`
      (status, final URL, content-type, body bytes, error)
- [x] TLS via system OpenSSL (no bundling); CA store from the system
- [x] HTTP redirect handling (cap at `ND_MAX_REDIRECTS` = 10 hops)
- [x] Protocols restricted to http/https (incl. redirects)
- [x] Address bar widget + Load/Stop buttons; raw response printed
      to a monospace `GtkTextView`, UTF-8 with latin1 fallback
- [x] Per-fetch `GCancellable`, stop button cancels in flight

Done when: typing `https://lite.cnn.com` into the address bar shows
the raw HTML in the window. ✓

### Phase 2 — HTML tokenizer + DOM

Deliverables:

- HTML5 tokenizer covering the common-case state machine (no
  exhaustive spec compliance)
- DOM tree (`struct nd_node`) with parent/child/sibling pointers
- Treebuilder for the major insertion modes
- Debug renderer: indented text dump of the DOM

Done when: lite.cnn.com produces a recognizable DOM tree.

### Phase 3 — CSS parser + style resolution

Deliverables:

- CSS tokenizer + selector parser (type, class, id, descendant, child)
- Property parsers for the layout-relevant subset (display, position,
  margin/padding/border, width/height, color, background-color,
  font-family/size/weight, text-align)
- Cascade + specificity + inheritance
- Computed style attached to each DOM node

### Phase 4 — Layout

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

Deliverables:

- Tabs (`GtkNotebook` or hand-rolled)
- Back / forward / reload / stop
- URL bar with history dropdown
- Bookmarks (single file on disk, plain text)

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

## Non-goals (explicit, won't change)

- WebGL, WebGPU, WebRTC, WebUSB, WebBluetooth, WebHID, WebMIDI.
- Service workers, push notifications, background sync.
- DRM / EME / Widevine.
- Browser extensions / WebExtensions API.
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
