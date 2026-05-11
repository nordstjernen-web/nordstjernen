# Nordstjernen — Development Plan

Live roadmap for the clean-room implementation. See `README.md` for
product vision and `CLAUDE.md` for working agreements.

## Current status

**Phase 0 — Bootstrap.** Greenfield. Repo contains only docs, git
hygiene files, and a Linux CI workflow. No source code yet.

## Guiding principles

- **One human's worth of code.** Whenever a phase risks ballooning, cut
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
- [ ] `meson.build` producing a `nordstjernen` binary
- [ ] `src/main.c` opening an empty GTK 4 `GtkApplicationWindow`
- [ ] CI builds and uploads the binary as an artifact
- [ ] macOS and Windows CI stubs (toolchain check only)

Done when: `meson compile -C builddir && ./builddir/nordstjernen`
opens a blank window on Linux, and CI is green.

### Phase 1 — Networking

Deliverables:

- libcurl wrapper: `net_fetch(url) -> bytes + headers + status`
- TLS via system OpenSSL (no bundling)
- HTTP redirect handling (cap at 10 hops)
- Address bar widget + "load" button that prints raw response bytes
  to a `GtkTextView`

Done when: typing `https://example.com` into the address bar shows
the raw HTML in the window.

### Phase 2 — HTML tokenizer + DOM

Deliverables:

- HTML5 tokenizer covering the common-case state machine (no
  exhaustive spec compliance)
- DOM tree (`struct nd_node`) with parent/child/sibling pointers
- Treebuilder for the major insertion modes
- Debug renderer: indented text dump of the DOM

Done when: example.com produces a recognizable DOM tree.

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

Candidate engines: **QuickJS** (small, MIT, embeddable) or **Duktape**
(small, MIT). Both are realistically audit-readable. SpiderMonkey and
V8 are out — too large, drag in too much.

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
