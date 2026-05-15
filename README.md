Nordstjernen web browser
=======================

Nordstjernen is a web browser written from scratch in C.

> *A north star, small and faithful — light enough to read by,*
> *slow enough to think with; built one line at a time.*

![Nordstjernen on the about:start home page](docs/screenshot.png)


- Targets to supports HTML 5, CSS, and JavaScript.

- Runs on Linux, macOS, Windows.

- Source-available under the Functional Source License v1.1 (FSL-1.1-MIT).
  Free for any non-competing use; converts to the MIT license on the
  second anniversary of each release. See `LICENSE`.

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

## License

Nordstjernen is licensed under the
[Functional Source License, Version 1.1, MIT Future License](https://fsl.software/)
(`FSL-1.1-MIT`). You may use, copy, modify, and redistribute the source for
any purpose other than offering a competing browser product or service.
Two years after each release, that release converts to the MIT license.
See [`LICENSE`](LICENSE) for the full text.

Copyright 2026 Andreas Røsdal.

---

Project home: https://nordstjernen.org

Source code: https://github.com/nordstjernen-web/nordstjernen
