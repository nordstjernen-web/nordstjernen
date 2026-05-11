# Nordstjernen — Claude operating guide

Nordstjernen ("Nordstjernen Web Navigator") is a closed-source web
browser written from scratch in **C**, using **GTK 4** for the UI and
**libcurl** for networking. Targets Linux, macOS, and Windows.

See `README.md` for the product vision. webmosilla (a Firefox 1.0
fork in `~/dev/webmosilla`) exists only as spiritual inspiration —
Nordstjernen does not share its source tree, build system, or
dependencies.

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
- **CI runs once a day on schedule.** Workflows do not trigger
  on push or PR; the daily schedule + manual
  `workflow_dispatch` are the only triggers, and they exist to
  catch macOS / Windows regressions that local Linux builds
  miss. Do not push code that hasn't compiled locally.

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

System packages required on Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config meson ninja-build \
    libgtk-4-dev libcurl4-openssl-dev
```

On Fedora/RHEL:

```sh
sudo dnf install gcc pkgconf meson ninja-build gtk4-devel libcurl-devel
```

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

- Don't reintroduce Mozilla/Gecko code, autoconf2.13, libIDL, GTK 2,
  or anything else from the webmosilla tree. Nordstjernen is a
  clean-room rewrite, not a fork.
- Don't add WebGL/WebGPU/AI surface area, even as stubs.
- Don't add translations, telemetry, crash reporters, update pingers,
  or "studies" infrastructure.
- Don't write planning docs unless asked.
