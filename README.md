Nordstjernen web browser
=======================

Nordstjernen is a web browser written from scratch in C.

> *A north star, small and faithful — light enough to read by,*
> *slow enough to think with; built one line at a time.*

![Nordstjernen on the about:start home page](docs/screenshot.png)

## Why another browser?

Almost every browser on the web today renders pages with one of three
engines — Blink (Chrome, Edge, Opera, Brave, Vivaldi, Arc, and every
Electron app), WebKit (Safari, and on iOS *everything*), or Gecko
(Firefox and its handful of forks). Blink and WebKit share a common
ancestor; Gecko is the last fully independent engine in serious
production. When one of those three vendors decides a web API ships or
doesn't, that's the web — there is no second opinion. The standards
process is downstream of whatever Google ships in Chrome.

A monoculture this complete is bad for the web in the same way a
monoculture is bad for a forest: a single bug, a single business
decision, a single ad-tech mandate, a single anti-feature, propagates
to every user at once. The web's resilience depends on independent
implementations actually existing — implementations that can disagree,
say no, and refuse to render the bloat. That is what Nordstjernen is.

Compared to **Chrome**, Nordstjernen is a few thousand times smaller,
ships no telemetry, no Safe Browsing pinger, no ad-tech extension
points, no DRM module, no JIT, no GPU compositor, no signed-in account,
no "experiments". You can read the entire source over a weekend.
Compared to **Firefox**, Nordstjernen is independent of Mozilla's
funding from Google search-default payments, doesn't ship Pocket /
sponsored tiles / studies / Normandy, and is small enough to be
audited end-to-end by one human. Neither Chrome nor Firefox is
something you can host yourself, fork in a single Saturday, or strip
down to the minimum you actually need.

Nordstjernen is also built **in Norway**, by a Norwegian developer. We
think a free internet needs browsers that aren't all designed inside
the same square mile of California. Norway has its own legal traditions
around privacy (the *personvernforordningen* / GDPR implementation),
its own consumer-protection authority that routinely pushes back on
Big Tech, and a long history of building open, public-good
infrastructure — from the postal service to NRK to Altinn. A browser
made here defaults to those values: privacy on, telemetry off,
advertising-free, the user's data stays on the user's machine. The
North Star the name refers to is *the* fixed point in the Norwegian
night sky; the browser tries to be the same kind of thing on the
web — small, steady, and pointed in one direction.

## Why Nordstjernen?

- **Small enough to audit.** The entire browser is ~30 kLOC of C —
  engine, JS bindings, chrome, and sandbox. Read it in an afternoon.
- **Fast and light.** Cold-starts in milliseconds, idles at a few
  tens of MB. No background services, no compositor process zoo.
- **No telemetry, ever.** Doesn't phone home, doesn't ping for
  updates in the background, doesn't fetch Safe Browsing, doesn't
  collect crash reports. Server logs only when *you* navigate.
- **No ads, no nags, no paywall.** Free under FSL-1.1-MIT, no
  in-app purchase, no license-key flow, no upsell screen.
- **Hardened by construction.** No JIT, no WebGL/WebGPU, no MSE, no
  service workers, no extensions — the entire exploit-prolific
  surface area of modern browsers is foreclosed by design.
- **Honest sandbox.** Linux builds enforce a Landlock filesystem
  sandbox plus a default-deny libseccomp syscall filter, refuse to
  run as root, and are compiled with PIE, full RELRO,
  stack-protector-strong, stack-clash protection, Intel CET, and
  `_FORTIFY_SOURCE=2`.
- **Privacy by default.** Cookies partitioned per top-level site;
  third-party cookies blocked by default; DNT sent; HSTS enforced
  with a bundled preload list; CSP and mixed-content blocking on.
- **Pragmatic web compatibility.** HTML5 via lexbor, modern CSS
  cascade (flex, basic grid, media queries, gradients, shadows),
  pragmatic JavaScript via QuickJS — enough to read the text-heavy
  web (Wikipedia, news, search, docs) without bloat.
- **Cross-platform, single binary.** Linux, macOS, and Windows
  builds from one meson tree, redistributable bundles produced by
  one script per OS.
- **Source-available, future-MIT.** Read it, build it, fork it,
  ship it inside your product. Each release converts to MIT ten
  years after publication.

## Details

- Targets to supports HTML 5, CSS, and JavaScript.

- Runs on Linux, macOS, Windows.

- Source-available under the Functional Source License v1.1 (FSL-1.1-MIT).
  Free for any non-competing use; converts to the MIT license on the
  tenth anniversary of each release. See `LICENSE`.

- Uses GTK 4 for the UI, libcurl for networking, Cairo and Pango
  for rendering, and the [quickjs-ng](https://github.com/quickjs-ng/quickjs/)
  fork of QuickJS for JavaScript. quickjs-ng is pinned to
  v0.14.0 via a meson wrap (downloaded as a release zip into
  `subprojects/quickjs-0.14.0/`) and static-linked into the
  browser binary — no git submodules.

- HTML is parsed by [lexbor](https://github.com/lexbor/lexbor) —
  a required dependency, picked up from a system install or built
  from `subprojects/lexbor.wrap` via CMake.

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
  (`data/compatibility-css/user-agents.conf`), and DOM rewriters for
  sites that need light surgery (Google search result link
  unwrapping, consent.google.com redirects, etc.). Users can drop
  overrides into `$XDG_DATA_HOME/nordstjernen/compatibility-css/`
  to win over the bundled defaults without rebuilding.

- No WebGL, WebGPU, or AI-style web APIs. At most one video
  codec is active at a time. The `<video>` element decodes VP9
  via libvpx (optional dependency) — `<video src="...webm">` paints
  in-page. YouTube's player isn't supported (it requires MSE), but
  direct WebM URLs work.

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

## Dependencies

Nordstjernen is built on a small, hand-picked set of libraries. Each
one is either a vendored subproject (built from source as part of the
meson tree, no system package needed) or a required system package
fetched via `pkg-config`. There are no optional plug-in points and no
runtime downloads.

### Vendored (built into the binary, no system install)

| Library | Version | Role |
| --- | --- | --- |
| [lexbor](https://github.com/lexbor/lexbor) | 3.0.0 | HTML5 parser, CSS selector matcher, WHATWG URL parser |
| [quickjs-ng](https://github.com/quickjs-ng/quickjs) | v0.14.0 | JavaScript engine (no JIT) |
| [Wuffs](https://github.com/google/wuffs) | v0.4 | Memory-safe PNG / GIF / BMP / JPEG decoder |

### Required system packages (Linux)

| Package (Debian/Ubuntu) | Role |
| --- | --- |
| `libgtk-4-dev` | Window, widget, and input layer |
| `libcurl4-openssl-dev` | HTTP / HTTPS networking, TLS |
| `libuchardet-dev` | Charset detection for response bodies |
| `librsvg2-dev` | SVG image decoding |
| `libseccomp-dev` | Default-deny syscall sandbox (Linux only) |
| `libcairo2-dev`, `libpango1.0-dev` | 2D drawing and text shaping (pulled in by GTK) |
| `libgdk-pixbuf-2.0-dev` | Fallback image decoding (TIFF / ICO / WebP) |
| `meson`, `ninja-build`, `cmake`, `pkg-config` | Build system |
| `build-essential` (gcc / clang) | C compiler |

### Optional system packages

| Package | Role |
| --- | --- |
| `libvpx-dev` | `<video>` VP9 decoding for in-page WebM playback |
| `libpoppler-glib-dev` | Inline PDF rendering (`application/pdf`) |
| `libavif-dev` | AVIF image decoding |
| `ccache` | Compiler cache for faster rebuilds |
| `lld` | Faster linker for development builds |

### Build-time tools

| Tool | Used for |
| --- | --- |
| Python 3 | Meson runs Python during configuration |
| CMake | Configuring the lexbor subproject |
| pkg-config | Locating system libraries |

See `CLAUDE.md` for the per-distro one-liner install commands
(Debian / Ubuntu, Fedora / RHEL, openSUSE) and `docs/Windows.md`
/ `docs/macOS.md` for the MSYS2 and Homebrew equivalents.

## License

Nordstjernen is licensed under the
[Functional Source License, Version 1.1, MIT Future License](https://fsl.software/)
(`FSL-1.1-MIT`). You may use, copy, modify, and redistribute the source for
any purpose other than offering a competing browser product or service.
Ten years after each release, that release converts to the MIT license.
See [`LICENSE`](LICENSE) for the full text.

Copyright 2026 Andreas Røsdal.

---

Project home: https://nordstjernen.org

Source code: https://github.com/nordstjernen-web/nordstjernen
