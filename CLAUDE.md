# Nordstjernen — Claude operating guide

Nordstjernen ("Nordstjernen Web Navigator") is a clean-room web
browser written from scratch in **C**, using **GTK 4** for the UI and
**libcurl** for networking. Targets Linux, macOS, and Windows.

See `README.md` for the product vision. Nordstjernen is a fresh
implementation — there is no upstream browser engine, no fork,
nothing imported.

## Design constraints

- Minimalistic, compact, secure. Source should be readable and
  maintainable by a single human.
- HTML5 + modern CSS + modern JavaScript, supported pragmatically as
  far as is feasible without bloat.
- **No** WebGL, WebGPU, or AI-style web APIs.
- At most one video codec active at a time.
- English UI only (for now).
- Does not phone home, does not telemeter the user.

## Comments policy

**The code is self-explaining. Don't write code comments.**

- Each source file gets exactly one short header comment at the top
  naming the file and (at most) one sentence on what it does. That's
  it.
- No inline `/* … */` or `//` comments inside functions, in struct
  declarations, around tricky branches, or anywhere else. Rename a
  variable or extract a function instead.
- No "section banner" comments (`/* ---------- helpers ---------- */`).
  Group code by file or function instead.
- No `TODO`/`FIXME`/`XXX` markers — file a real task instead.

## Autonomous mode — read this every session

This repo is driven by Claude in long uninterrupted sessions.
**Default to acting, not asking.**

- **Don't ask "do you want me to proceed?", "should I continue?",
  "ready to commit?"** — just do it. The user interrupts if they
  disagree.
- **Don't summarize after every step.** One-line status is enough.
- **Don't pause for path/file/branch confirmation when context is
  unambiguous.** Grep, pick, proceed.
- **Commit and push aggressively.** Small commits, push to
  `origin/main` as soon as a logical unit lands.
- **Run for hours.** Diagnose, fix, retry. Only stop on genuine
  external blockers. When stopping: one line on what's blocked.
- **Never ask the user to run the build.** Run it yourself.
- **Local machine is the build *and* run oracle.** This repo
  lives on a Linux box with GTK 4 / libcurl / meson / clang
  installed and an X session at `DISPLAY=:0`. Every commit must
  pass `meson compile -C builddir` locally before pushing.
  Smoke-launch the browser (`./builddir/src/nordstjernen <url>`
  in the background, then kill it) on material changes — that's
  the per-change correctness gate, not CI.
- **CI is disabled.** The Linux / macOS / Windows workflows only
  trigger on manual `workflow_dispatch` — the daily cron was
  removed. Nothing runs automatically on push or PR. Local Linux
  is the only correctness gate; the workflows are kept around for
  the rare ad-hoc cross-platform sanity run.

## Build / verify locally

The intended build system is **meson + ninja**. From a clean checkout:

```sh
meson setup builddir
meson compile -C builddir
./builddir/src/nordstjernen
```

`meson setup` will auto-download the QuickJS engine
([quickjs-ng v0.14.0](https://github.com/quickjs-ng/quickjs/releases/tag/v0.14.0))
from its release zip into `subprojects/quickjs-0.14.0/`, as
declared by `subprojects/quickjs.wrap`. No git submodules.

### HTML engines: Lexbor (default) and Gumbo (fallback)

Nordstjernen ships two HTML→DOM backends. The default is
[lexbor](https://github.com/lexbor/lexbor): if `lexbor/html/html.h`
and `liblexbor_static` are present on the system, the lexbor
backend is built in; otherwise configure falls through to a CMake
subproject (`subprojects/lexbor.wrap`). Force-disable it with
`-Dlexbor=disabled`; force-require it with `-Dlexbor=enabled`.

The fallback / cross-check backend is gumbo. The canonical source
is the maintained fork at
[codeberg.org/gumbo-parser/gumbo-parser](https://codeberg.org/gumbo-parser/gumbo-parser),
pulled in via `subprojects/gumbo.wrap` (git wrap, packagefile build
in `subprojects/packagefiles/gumbo/`). System pkg-config `gumbo`
satisfies the dep first when present; otherwise meson clones the
codeberg fork.

At runtime, set `ND_HTML_ENGINE=lexbor` (the default when lexbor
was built in) or `ND_HTML_ENGINE=gumbo` to pick a parser without
rebuilding. The two backends share the `nd_node` DOM, so layout /
paint / JS are engine-agnostic — this is purely a parse-to-DOM swap,
useful for cross-checking conformance.

### URL parsing: ada-url

Optional dependency. When the [ada](https://github.com/ada-url/ada)
WHATWG URL library is available (either as a system pkg-config
`ada`, or via the `subprojects/ada.wrap` singleheader fallback
which compiles a small C++17 amalgamation into a static lib), the
`nd_url_*` helpers in `src/net.c` route through ada for spec-compliant
parsing / resolution / origin extraction. Without it, the hand-rolled
fallback in `net.c` is used. Toggle with `-Dada=enabled|disabled|auto`.

### Charset detection: uchardet

Required dependency (Debian/Ubuntu `libuchardet-dev`,
Fedora/RHEL `uchardet-devel`). `nd_html_decode_body` hands the
response body to [uchardet](https://www.freedesktop.org/wiki/Software/uchardet/)
to identify the charset, then `g_convert`s to UTF-8. No
hand-rolled BOM / HTTP-charset / meta-charset sniffing — uchardet
handles all of that internally. The Latin-1 fallback only fires
if uchardet can't classify the bytes at all.

System packages required on Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config meson ninja-build \
    libgtk-4-dev libcurl4-openssl-dev libuchardet-dev
```

On Fedora/RHEL:

```sh
sudo dnf install gcc pkgconf meson ninja-build gtk4-devel libcurl-devel \
    uchardet-devel
```

### Fast iteration (recommended for AI/Claude loops)

`ccache` is the single biggest build-time win and meson picks it up
automatically. With `ccache` installed, a clean `meson setup builddir
&& meson compile -C builddir` drops from ~35s to ~1s once the cache
is warm — subprojects (lexbor, gumbo, quickjs, ada) hit the cache
and re-link in negligible time. Install once:

```sh
sudo apt install ccache       # Debian/Ubuntu
sudo dnf install ccache       # Fedora/RHEL
```

Optionally use the `lld` linker for faster final links
(`CC_LD=lld meson setup builddir`). Not required.

`./dev build` runs `meson setup` (only if needed) and
`meson compile -C builddir` in one shot — use it instead of typing
the two commands separately.

## Definition of done

A change is done when:

1. It compiles cleanly (no new warnings) with the configured GCC and
   Clang flags.
2. The browser launches and the affected UI path works manually.
3. The change is committed and pushed to `origin/main`.

Note: this project has **no automated test suite** and no plans to
add one. Verify behavior by running the browser. Don't add unit /
integration / property / fuzz tests, don't add a `tests/` directory,
don't add `meson test` targets.

## Don't

- Don't introduce Mozilla/Gecko code, WebKit code, or any other
  upstream browser engine source. Nordstjernen is a clean-room
  implementation, not a fork.
- Don't add WebGL/WebGPU/AI surface area, even as stubs.
- Don't add translations, telemetry, crash reporters, update pingers,
  or "studies" infrastructure.
- Don't write planning docs unless asked.
