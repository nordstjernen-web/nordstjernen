# Nordstjernen on macOS — build and run

This document records the working setup for building and running
Nordstjernen on modern macOS (Apple Silicon and Intel). The macOS CI
workflow (`.github/workflows/macos.yml`) is the authoritative spec;
what follows is the same recipe verified on a local MacBook.

The macOS build uses **Homebrew** for the toolchain and dependencies,
the same source the CI workflow installs. No Xcode project, no
CocoaPods, no `.app` bundle plumbing. The resulting binary is a
single Mach-O executable linking the same GTK 4 / libcurl / Cairo /
Pango / Gumbo / libvpx libraries shipped to Linux and Windows users.

Tested against macOS 14 (Sonoma) and macOS 15 (Sequoia) on
`arm64` (M1/M2/M3/M4) and `x86_64` MacBooks.

## One-time setup

Install [Homebrew](https://brew.sh) if you do not already have it,
then:

```sh
brew install meson ninja pkg-config gtk4 curl gumbo-parser libvpx
brew install ccache    # optional, speeds up rebuilds
```

Homebrew installs to:

- `/opt/homebrew` on Apple Silicon (arm64).
- `/usr/local` on Intel (x86_64).

Both prefixes are wired up through `pkg-config` automatically; no
extra environment variables are needed for a normal user install.

## Build

```sh
git clone https://github.com/operativsystem42/nordstjernen
cd nordstjernen
meson setup builddir
meson compile -C builddir
```

`meson setup` auto-downloads quickjs-ng v0.14.0 into
`subprojects/quickjs-0.14.0/` via a wrap file. No git submodules.

If you prefer the same flags CI uses:

```sh
export CC="ccache clang"
meson setup builddir --werror
meson compile -C builddir
```

## Run

```sh
./builddir/src/nordstjernen https://example.com
```

The first launch creates the per-user state directories under
`~/Library/Application Support/nordstjernen`,
`~/Library/Caches/nordstjernen`, and
`~/Library/Preferences/nordstjernen`, following GLib's standard XDG
mapping on macOS.

Headless rendering works the same as on Linux:

```sh
./builddir/src/nordstjernen --headless --dump=text https://example.com
./builddir/src/nordstjernen --headless --dump=png:/tmp/page.png https://example.com
```

## Platform notes

- **CA bundle.** Homebrew's `libcurl` links against OpenSSL, which
  has no built-in trust store. Nordstjernen probes the standard
  Homebrew and system paths at startup
  (`/opt/homebrew/etc/ca-certificates/cert.pem`,
  `/usr/local/etc/ca-certificates/cert.pem`, `/etc/ssl/cert.pem`,
  and a handful of `openssl@3` variants) and points libcurl at
  whichever exists. Override with `SSL_CERT_FILE` or
  `CURL_CA_BUNDLE` if you ship your own bundle.
- **Self-path resolution.** The new-window action re-execs the
  current binary; on macOS the binary path is resolved with
  `_NSGetExecutablePath(3)` and canonicalised with `realpath(3)`.
- **Sandbox.** The Linux Landlock sandbox is Linux-only. On macOS,
  the only startup-side restriction is the same refuse-root check
  used on Linux — `nordstjernen` exits with status 77 if launched
  via `sudo` unless `ND_ALLOW_ROOT=1` is set.
- **Keyboard shortcuts.** Every accelerator uses GTK's `<Primary>`
  modifier, which maps to ⌘ on macOS. So `⌘L` focuses the URL bar,
  `⌘T` / `⌘N` open a new window, `⌘R` reloads, `⌘W` closes the
  window, `⌘F` opens find-in-page, `⌘+` / `⌘-` / `⌘0` zoom, `⌘P`
  prints, `⌘⇧J` opens the JS console.
- **Display server.** GTK 4 on macOS uses the native Quartz backend.
  There is no X11 or Wayland requirement, and no XQuartz dependency.
- **No `.app` bundle.** The output is a plain Mach-O binary in
  `builddir/src/nordstjernen`. There is currently no
  `Info.plist` / `.app` packaging step; the binary launches from
  Terminal or Finder and shows up in the Dock while it runs.

## Definition of done on macOS

Same as everywhere else (`CLAUDE.md`):

1. `meson compile -C builddir` finishes without new warnings under
   the configured Clang flags.
2. The browser launches and the affected UI path works manually.
3. The change is committed and pushed to `origin/main`.

The macOS CI workflow runs once a day on schedule and on manual
`workflow_dispatch`, and exists to catch regressions that the local
Linux build misses (Apple Silicon ABI, BSD libc, GTK 4 Quartz
backend). It is not a per-push gate.
