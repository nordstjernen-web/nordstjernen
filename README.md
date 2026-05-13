Nordstjernen web browser
=======================

Version 0.4.0 — Linux first; macOS and Windows ports build from the
same tree. (CI workflows under `.github/workflows/` are
`workflow_dispatch`-only — run on demand, not on every push.)

> *A north star, small and faithful — light enough to read by,*
> *slow enough to think with; built one line at a time.*

![Latest build screenshot](docs/screenshot.png)


Nordstjernen is a web browser written from scratch in C.

- Supports HTML5, modern CSS, and a pragmatic subset of modern
  JavaScript — as far as is feasible without bloat.

- Runs on Linux, macOS, and Windows.

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
  mixed-content blocking for subresources, CSP enforcement,
  SOP/CORS for JS fetch+XHR, per-window OS process
  separation, refuses to run as root on Linux/macOS, Linux
  Landlock filesystem sandbox, no plugins, no extensions,
  no telemetry.

- Minimalistic, good for reading: Wikipedia, news, search
  results, documentation, light forms.

- English UI only for now.

- Source code is minimalistic, compact, correct, secure, and
  meant to stay readable by a single human.

- Includes a headless rendering mode:
  `nordstjernen --headless --dump=<text|dom|layout|png:PATH|pdf:PATH> URL`.

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

Nordstjernen is **shareware**, in the spirit of how Opera Software
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
