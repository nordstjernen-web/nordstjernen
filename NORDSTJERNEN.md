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

### Positioning: a good, simple,f fast web browser.

Nordstjernen is built on GTK 4 and targets Linu, Windows and Mac.

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

` + small DOM mutations.

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
