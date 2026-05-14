Nordstjernen web browser
=======================

Nordstjernen is a web browser written from scratch in C.

> *A north star, small and faithful — light enough to read by,*
> *slow enough to think with; built one line at a time.*

![Latest build screenshot](docs/screenshot.png)

## What this is and why it exists

Nordstjernen is a clean-room web browser for people who want to *read
the web* — Wikipedia, news, documentation, search results, light
forms — without donating a few hundred million lines of someone else's
engine and a few hundred kilobytes of telemetry to the experience.
It's around 27,000 lines of C, small enough that one person can read
the whole thing in a long afternoon, hardened with the modern
exploit-mitigation toolkit, sandboxed on Linux, and shipped with no
telemetry, no DRM, no plugins, no extensions, and no AI surface.

It will not match Chrome or Firefox feature-for-feature, and doesn't
try to. It competes on a different axis:

- **Auditability.** A single human can read the whole engine. There
  is no Blink, no Gecko, no V8 JIT, no WebKit. Bugs are findable
  by the user, not just by Google's fuzzing fleet.
- **Privacy by construction.** Nothing phones home. No update pinger,
  no crash reporter, no "studies," no Google Safe Browsing fetches,
  no Mozilla telemetry. The default search engine is DuckDuckGo Lite.
  HTTP cache and connections are partitioned by top-level site so
  the same CDN URL can't track you across sites via cache timing.
- **Smaller attack surface.** No WebGL, no WebGPU, no MSE, no Web
  Bluetooth, no Web USB, no service workers, no PNaCl, no Flash,
  no PDF viewer, no JIT. QuickJS is an interpreter — the most
  prolific category of in-the-wild browser RCEs (V8 / SpiderMonkey
  JIT bugs) is foreclosed entirely.
- **Network is locked down by default.** TLS verification on,
  HTTP/HTTPS only (no `file://` redirect targets, no `ftp://`),
  built-in HSTS preload list, libcurl-enforced HSTS across redirect
  chains, max 10 redirects per request, max 60 seconds per HTTP
  request, max 60 seconds per JS execution slice.
- **Linux Landlock sandbox.** The renderer cannot read `~/.ssh`,
  `~/.gnupg`, password-manager files, or other browsers' profiles.
  Only the nordstjernen-specific subdirs under `$XDG_*_HOME` are
  writable. Refuses to run as root on Linux/macOS or as
  Administrator on Windows.
- **Compiled hard.** PIE, full RELRO, `-fstack-protector-strong`,
  `-fstack-clash-protection`, `-fcf-protection=full` (Intel CET),
  `_FORTIFY_SOURCE=2`, no-exec stack, separate-code.
- **Shareware, not adware.** Free to download, free to keep using.
  The browser eventually nags you to buy a license; that nag is
  the entire enforcement surface. No DRM, no online check, no
  fingerprinting.

### How it compares

|  | Nordstjernen | Chrome | Firefox |
|---|---|---|---|
| Engine origin | clean-room, ~27 kLOC | Blink + V8, ~30 M LOC | Gecko + SpiderMonkey, ~30 M LOC |
| Readable by one person | yes | no | no |
| JIT in JS engine | no (QuickJS interpreter) | yes (V8) | yes (SpiderMonkey) |
| WebGL / WebGPU | no | yes | yes |
| Extensions / plugins | no | WebExtensions | WebExtensions |
| Telemetry | none | extensive | extensive |
| Update pinger | none | yes | yes |
| HSTS preload | bundled list + libcurl | full Chromium list | full Mozilla list |
| Cache partitioning | by top-level site | by top-level site | by top-level site |
| Sandbox (Linux) | Landlock + seccomp-bpf, narrow | namespace + seccomp-bpf | namespace + seccomp |
| Default search | DuckDuckGo Lite | Google | Google |

This is not a Chromium-grade adversary-resistant browser, and it
doesn't pretend to be. It's a browser whose threat model is
*sloppy or mildly hostile websites running on top of a trusted
operating-system user*, and which keeps the design small enough
to defend honestly.

- Targets to supports HTML 5, CSS, and JavaScript.

- Runs on Linux, macOS, Windows.

- Nordstjernen is shareware with a free trial period, with a commercial
  license, similar to what the Opera browser was like in the 1990s.

- Uses GTK 4 for the UI, libcurl for networking, Cairo and Pango
  for rendering, and the [quickjs-ng](https://github.com/quickjs-ng/quickjs/)
  fork of QuickJS for JavaScript. quickjs-ng is pinned to
  v0.14.0 via a meson wrap (downloaded as a release zip into
  `subprojects/quickjs-0.14.0/`) and static-linked into the
  browser binary — no git submodules.

- HTML is parsed by [lexbor](https://github.com/lexbor/lexbor) by
  default — it's a required dependency, picked up from a system
  install or built from `subprojects/lexbor.wrap` via CMake.
  [gumbo](https://codeberg.org/gumbo-parser/gumbo-parser) ships as a
  second cross-check backend; switch to it at runtime with
  `ND_HTML_ENGINE=gumbo`, globally or per-site through
  `data/compatibility-css/html-engines.conf`. Both engines produce the
  same internal `nd_node` DOM, so layout, paint, and JS are
  engine-agnostic.

- CSS parsing has the same two-engine arrangement. The default is
  Nordstjernen's own selector/cascade engine; setting
  `ND_CSS_ENGINE=lexbor` swaps in lexbor's selector matcher. The
  cascade is sheet-index aware, so last-loaded sheets win ties
  against same-specificity rules in earlier (page-author) sheets —
  this is what makes the `data/compatibility-css/*.css` overrides
  actually override.

- WHATWG URL parsing is handled by lexbor's URL module, which is
  always available since lexbor is a required dependency. The
  `nd_url_*` helpers in `src/net.c` are thin wrappers over
  `lxb_url_parse` / `lxb_url_serialize`.

- Charset detection uses
  [uchardet](https://www.freedesktop.org/wiki/Software/uchardet/)
  — required dependency. We hand the response body to uchardet, then
  `g_convert` to UTF-8. No BOM / meta sniffing in the browser itself.

- A per-site compatibility framework supplies CSS overrides
  (`data/compatibility-css/*.css`), per-site `User-Agent` strings
  (`data/compatibility-css/user-agents.conf`), per-site parser choices
  (`data/compatibility-css/html-engines.conf`), and DOM rewriters for
  sites that need light surgery (Google search result link
  unwrapping, consent.google.com redirects, etc.). Users can drop
  overrides into `$XDG_DATA_HOME/nordstjernen/compatibility-css/`
  to win over the bundled defaults without rebuilding.

- No WebGL, WebGPU, or AI-style web APIs. At most one video
  codec is active at a time. The `<video>` element decodes VP9
  via libvpx (optional dependency) — `<video src="...webm">` paints
  in-page. YouTube's player isn't supported (it requires MSE), but
  direct WebM URLs work.

- Secure by default: TLS-verified fetches, dynamic HSTS,
  mixed-content blocking, CSP origin/host matching (no
  nonce/hash yet), SOP/CORS for fetch+XHR, best-effort
  per-window process separation (not Chromium-style
  per-origin site isolation), refuses to run privileged,
  Linux Landlock filesystem sandbox + libseccomp syscall
  deny-list (no equivalent on macOS / Windows yet), no
  plugins, no extensions, no telemetry. Built with PIE,
  full RELRO, stack-protector-strong, stack-clash
  protection, CET, `_FORTIFY_SOURCE=2`, NX stack.

- Minimalistic, good for reading: Wikipedia, news, search
  results, documentation, light forms.

- English UI only for now.

- Source code is minimalistic, compact, correct, secure, and
  meant to stay readable by a single human.

- Includes a headless rendering mode:
  `nordstjernen --headless --dump=<text|dom|layout|png:PATH|pdf:PATH> URL`.

- Linux builds use meson + ninja against system GTK 4 / libcurl /
  libuchardet / librsvg. Redistributable artefacts are produced by
  `./scripts/pack-linux.sh` (stripped + LTO `nordstjernen` plus
  runtime data, zipped as `dist/nordstjernen-<v>-linux-x86_64.zip`)
  and `./scripts/pack-rpm.sh` (the same bundle repackaged as a
  binary RPM that auto-extracts its SONAME requirements, so the
  one file installs on Fedora, RHEL, and openSUSE). See
  [docs/Linux.md](docs/Linux.md) for the full recipe.

- Windows builds use MSYS2 / MINGW64 with the same toolchain CI
  installs. A redistributable `dist/nordstjernen-win64/` bundle
  (exe + the mingw64 DLLs it imports + GLib schemas + GDK-Pixbuf
  loaders + Adwaita icons + CA bundle) is produced by
  `./scripts/pack-windows.sh`. See [docs/Windows.md](docs/Windows.md) for
  the full recipe.

- macOS builds use Homebrew on Apple Silicon and Intel MacBooks
  with the same toolchain CI installs. See
  [docs/macOS.md](docs/macOS.md) for the full recipe.

- Includes a plain-file HTTP cache under `$XDG_CACHE_HOME/nordstjernen/`
  honouring `Cache-Control` / `ETag` / `Last-Modified`.

- Configurable via `~/.config/nordstjernen/nordstjernen.conf`.
  Run `nordstjernen --print-config` to see the effective config.
  Defaults: home page `https://duckduckgo.com/lite/`, search engine
  `https://lite.duckduckgo.com/lite/?q=%s`, `Accept-Language`
  auto-detected from the OS locale (`g_get_language_names()`
  on top of `GetUserDefaultUILanguage` / `LC_MESSAGES`), rendered
  to BCP-47 with descending `q=` values.

- Built-in **JavaScript console** (toolbar button) opens with a
  short environment banner — browser version, HTML parser, CSS
  engine, JS engine, GTK version, OS, last layout time — and lets
  you evaluate JS inline against the current page.

- Security environment switches: `ND_ALLOW_ROOT=1` to bypass the
  no-root refusal (containers / non-interactive use only),
  `ND_NO_SANDBOX=1` to disable the Linux Landlock filesystem
  sandbox (only useful when debugging it).

Developed by Andreas Røsdal, with extensive use of AI tooling.

## Distribution model

Nordstjernen is shareware, in the spirit of how Opera Software
shipped Opera in its early years. The browser is free to download
and try; a polite nag eventually appears asking the user to buy a
license. The binary keeps working either way. There is no DRM,
no online check, no usage telemetry — the nag is the entire
enforcement surface, and it relies on the goodwill of users who
get value from the browser to pay for it.

License: Copyright 2026 Andreas Røsdal. Source-available for
audit and contribution; redistribution and commercial use require
a license. See NORDSTJERNEN.md for the development plan.

---

Project home: https://nordstjernen.org

Source code: https://github.com/nordstjernen-web/nordstjernen
