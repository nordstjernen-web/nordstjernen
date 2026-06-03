Nordstjernen web browser
========================

Nordstjernen is a web browser, written from scratch in C.
Focused on supporting the HTML and CSS standards.  
Runs on Windows, Mac and Linux, with an Android port in progress (see `android/`).
Nordstjernen is built in Norway. 

Nordstjernen has no JIT so it is much more secure, and can still be fast enough.

![Nordstjernen rendering the Aurora article on Wikipedia](docs/screenshot.png)


## Standards compliance

Nordstjernen is measured against the **spec text**, section by section,
not against any other browser. It covers about **76 %** of the in-scope
WHATWG HTML standard (§1–§16). Highlights:

| Spec area | Status |
|-----------|:------:|
| §2 Common infrastructure — WHATWG URL, IDN, origins, encodings | ✅ |
| §3–§4 Semantics, elements, embedded & tabular content | ✅ |
| §4.10 Forms — controls, validation, `valueAs*` | ✅ |
| §4.12–§4.13 Scripting, custom elements | ✅ |
| §6 User interaction — focus, `contenteditable` | ✅ |
| §7–§8 Loading pages, web application APIs | ✅ |
| §9 Communication — `fetch`, `XHR`, `WebSocket` | ✅ |
| §12 Web storage — `localStorage` / `sessionStorage` | ✅ |
| §13–§14 HTML & XML syntax (lexbor parser) | ✅ |
| §15 Rendering — CSS cascade, flex, grid, transforms | ✅ |

The full section-by-section walk-through lives in
[docs/HTML-compatibility.md](docs/HTML-compatibility.md).

## Browser features

- **HTML/CSS** via the lexbor parser — modern cascade, flex, grid,
  transforms, gradients, `@keyframes`.
- **JavaScript** on the QuickJS interpreter — DOM, Shadow DOM, observer
  APIs.
- **Networking** over HTTP/2 with libcurl — HSTS, CSP, partitioned
  cookies.
- **Media** — images, optional inline PDF; audio and video are handed
  off to an external player; any script the host has fonts for.
- **UI** — tabs, bookmarks, find-in-page, print, JS console, headless
  mode, and a C embedding API.

## Download

Nightly builds, rebuilt from `main` each night. These point at the
latest build — bleeding edge, expect rough edges.

| Platform | Download |
|----------|----------|
| Windows | [`nordstjernen-windows-x86_64.zip`](https://www.nordstjernen.org/nightly/nordstjernen-windows-x86_64.zip)  |
| macOS | [`nordstjernen-macos.dmg`](https://www.nordstjernen.org/nightly/nordstjernen-macos.dmg) |
| Debian | [`nordstjernen-debian-amd64.deb`](https://www.nordstjernen.org/nightly/nordstjernen-debian-amd64.deb) |
| Ubuntu | [`nordstjernen-ubuntu-amd64.deb`](https://www.nordstjernen.org/nightly/nordstjernen-ubuntu-amd64.deb) |
| openSUSE | [`nordstjernen-opensuse-x86_64.rpm`](https://www.nordstjernen.org/nightly/nordstjernen-opensuse-x86_64.rpm) |
| Linux (portable) | [`nordstjernen-linux-x86_64.zip`](https://www.nordstjernen.org/nightly/nordstjernen-linux-x86_64.zip) |
| Alpine (musl) | [`nordstjernen-alpine-x86_64.apk`](https://www.nordstjernen.org/nightly/nordstjernen-alpine-x86_64.apk) (`apk add`) · [`.zip`](https://www.nordstjernen.org/nightly/nordstjernen-alpine-x86_64.zip) (portable) |
| Java API (JDK 21) | [`nordstjernen-java.jar`](https://www.nordstjernen.org/nightly/nordstjernen-java.jar) · [sources](https://www.nordstjernen.org/nightly/nordstjernen-java-sources.jar) · [javadoc](https://www.nordstjernen.org/nightly/nordstjernen-java-javadoc.jar) · [API docs](https://www.nordstjernen.org/nightly/java/apidocs/) |
| Source | [`nordstjernen-src.tar.xz`](https://www.nordstjernen.org/nightly/nordstjernen-src.tar.xz) |

[Checksums](https://www.nordstjernen.org/nightly/SHA256SUMS) ·
[all nightly files](https://www.nordstjernen.org/nightly/)

The Java API embeds the engine on the JVM (requires JDK 21); see
[`java/README.md`](java/README.md).

## Build

```sh
sudo apt install build-essential pkg-config meson ninja-build cmake \
    libgtk-4-dev libcurl4-openssl-dev libuchardet-dev librsvg2-dev \
    libpsl-dev libsqlite3-dev libseccomp-dev webp-pixbuf-loader
meson setup builddir && meson compile -C builddir
./builddir/src/nordstjernen
```

lexbor, QuickJS and Wuffs are vendored in-tree — no submodules, no
downloads. Windows, Fedora, openSUSE and macOS instructions are in
[docs/](docs/).

## Dependencies

Nordstjernen is a clean-room engine — no upstream browser code. The
moving parts:

**Vendored in-tree** (built from the main tree, no submodules):

| Component | Role |
|-----------|------|
| [lexbor](https://github.com/lexbor/lexbor) | HTML5 → DOM parser, CSS, and the WHATWG URL module (`nd_url_*`) |
| [QuickJS](https://github.com/quickjs-ng/quickjs) (quickjs-ng fork) | JavaScript engine — no JIT, browser-side hooks added in-tree |
| [Wuffs](https://github.com/google/wuffs) v0.4 | Memory-safe image decoding — PNG, GIF, BMP, JPEG, WebP (lossless) |

**Required system libraries:**

| Library | Min version | Role |
|---------|-------------|------|
| GTK 4 | **≥ 4.22.4 on Windows** (MSYS2 stock), ≥ 4.14 elsewhere (≥ 4.22 preferred) | UI toolkit, GSK renderer |
| GLib / GModule | (ships with GTK) | core types, dynamic module loading |
| Pango | (ships with GTK) | text shaping and layout |
| libcurl | ≥ 7.85 | HTTP/2 networking, HSTS, cookies |
| uchardet | — | charset detection for `nd_html_decode_body` |
| libpsl | — | public-suffix list for cookie scoping |
| SQLite | — | history, bookmarks, storage backends |
| librsvg | ≥ 2.46 | SVG rendering / icons |
| webp-pixbuf-loader | — | lossy (VP8) WebP via gdk-pixbuf |
| libseccomp | — (Linux only) | syscall sandbox; no-op on macOS/Windows |

**Optional** (auto-detected; feature compiled in when present):

| Library | Enables |
|---------|---------|
| poppler-glib | inline PDF viewing |
| libavif | AVIF images |
| fontconfig / pangoft2 | extra font discovery backends |

**Audio / video.** Nordstjernen ships no media codecs. `<audio>` and
`<video>` render a poster and a play overlay; clicking hands the source
URL to an external player — `mpv`, `VLC`, `celluloid`, `totem`,
`mplayer` or `ffplay` on Linux, otherwise the desktop's default handler
for the media type (found via `GAppInfo`, so Flatpak players work too),
the default app via `open` on macOS, and the registered handler on
Windows. If none is found, a dialog suggests installing
[mpv](https://mpv.io). A media player is therefore a *recommended
runtime dependency*, not a build dependency: the `.deb` and `.rpm`
packages `Recommend` one (defaulting to `mpv`) so playback works out of
the box, while source builds need none. On Linux the launch is performed
by a small broker process forked before the seccomp/Landlock sandbox is
installed, so the player runs unconfined while the browser stays locked
down.

Streaming sites (YouTube and friends) drive `<video>` through MSE/`blob:`
with no plain file URL. For those, clicking hands the **page URL** to the
player instead, so `mpv`/`VLC` resolve it with
[yt-dlp](https://github.com/yt-dlp/yt-dlp) — install yt-dlp alongside the
player to watch them.

## License

Nordstjernen Source License v1.0 — use, modify and redistribute freely,
except as a competing browser; each release becomes MIT after ten years.
See [License.md](License.md). Commercial licenses by agreement.

Project home: <https://nordstjernen.org> · Copyright 2026 Andreas Røsdal.

<img src="docs/nordstjernen-now.png" alt="Nordstjernen Now!" width="140">
<img src="docs/best-viewed-in-nordstjernen.png" alt="Best viewed in Nordstjernen" width="140">


## Builds
[![linux](https://github.com/nordstjernen-web/nordstjernen/actions/workflows/linux.yml/badge.svg?branch=main)](https://github.com/nordstjernen-web/nordstjernen/actions/workflows/linux.yml)
[![macos](https://github.com/nordstjernen-web/nordstjernen/actions/workflows/macos.yml/badge.svg?branch=main)](https://github.com/nordstjernen-web/nordstjernen/actions/workflows/macos.yml)
[![windows](https://github.com/nordstjernen-web/nordstjernen/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/nordstjernen-web/nordstjernen/actions/workflows/windows.yml)
[![android](https://github.com/nordstjernen-web/nordstjernen/actions/workflows/android.yml/badge.svg?branch=main)](https://github.com/nordstjernen-web/nordstjernen/actions/workflows/android.yml)
[![java](https://github.com/nordstjernen-web/nordstjernen/actions/workflows/java.yml/badge.svg?branch=main)](https://github.com/nordstjernen-web/nordstjernen/actions/workflows/java.yml)
[![codeql](https://github.com/nordstjernen-web/nordstjernen/actions/workflows/codeql.yml/badge.svg)](https://github.com/nordstjernen-web/nordstjernen/actions/workflows/codeql.yml)
[![Semgrep](https://img.shields.io/badge/semgrep-scan-success?logo=semgrep)](https://semgrep.dev/orgs/nordstjerna/projects/6111979)
