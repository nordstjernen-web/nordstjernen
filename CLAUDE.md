# Nordstjernen — Claude operating guide

Nordstjernen ("Nordstjernen Web Navigator") is a web
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
- **No** AI-style web APIs. WebGL **is** supported: a working, minimalist
  WebGL 1 / 2 over OpenGL ES (`src/webgl.c`, see `docs/webgl.md`). It is
  opt-in — off by default and gated by a per-site trust prompt — but fully
  functional once a site is trusted. The `WebGLRenderingContext` /
  `WebGL2RenderingContext` interface objects also carry the GL enum
  constants so feature code resolves them without a live context.
- **WebGPU** (`navigator.gpu`) is an **experimental** feature that layers
  `src/webgpu.c` over the external
  [wgpu-native](https://github.com/gfx-rs/wgpu-native) library (the
  `webgpu.h`/`wgpu.h` headers are vendored in-tree under
  `third_party/wgpu-native/`). The `webgpu` meson feature is `auto`: it is
  built whenever wgpu-native is present (its pkg-config file, or
  `-Dwgpu_native_root` pointing at an extracted release) and silently skipped
  otherwise — so a stock build on a machine without wgpu-native still carries
  **no** WebGPU symbol or dependency, exactly as before. `-Dwebgpu=enabled`
  hard-requires it; `-Dwebgpu=disabled` never builds it. Even in a build that
  contains it, WebGPU is **off at runtime** until the browser is started with
  `--enable-webgpu` (which sets `NS_WEBGPU_ALLOW=1`, inherited by the
  sandboxed renderer); without that flag `navigator.gpu.requestAdapter()`
  resolves to `null`. wgpu-native is a large dependency and deliberately
  stays an opt-in build input, never vendored. See `docs/webgpu.md`.
- The **one vendored, in-tree** video codec is MPEG-1, decoded by the
  vendored MIT-licensed [pl_mpeg](https://github.com/phoboslab/pl_mpeg)
  single-file decoder (`subprojects/plmpeg/`, wrapped by
  `src/video_decode.c`). A `<video>` whose source is an `.mpg`/`.mpeg`/`.m1v`
  stream plays **inline** — frames are decoded in the sandboxed renderer
  and advanced off the animation tick (`src/video.c`), honouring
  `autoplay`/`loop`/`muted`/`poster` and click-to-play/pause. Audio plays
  via the unsandboxed `nordstjernen-audio` helper (`src/audio/main.c`),
  which decodes in-tree — pl_mpeg for the MPEG-1/MP2 track, the vendored
  CC0 [minimp3](https://github.com/lieff/minimp3) (`src/audio/minimp3.h`)
  for standalone `.mp3` files — and outputs through SDL2's audio device
  (WASAPI/CoreAudio/ALSA), mixing and resampling the streams itself. The
  renderer emits `open`/`play`/`pause`/`seek`/`stop`/`loop`/`volume`
  commands that ride the render-response `X-Audio` side-channel to the
  shell, which spawns and pumps the helper (`src/gtk/procview.c`).
  MSE video frames decode in a third process, `nordstjernen-video`
  (`src/videoproc/main.c`, built when libav is present): the renderer
  materializes the growing stream to `~/.cache/nordstjernen/msvideo/`
  and drives it with `video …` lines on the same side-channel; the
  helper writes BGRA frames into a shm ring that the shell composites
  over the page surface each tick (see `docs/media.md`). Without the
  helper (headless, Windows) the renderer decodes in-process as before. The
  in-tree decoders stay pl_mpeg (MPEG-1 video + MP2) + minimp3 (MP3) — don't
  vendor further single-file codecs. **WebM is the one FFmpeg-backed
  extension**, over `libav\*` (`libavformat`/`libavcodec`/`libavutil`/
  `libswscale`/`libswresample`, system packages — never vendored): the
  `libav` build path (`-DNS_HAVE_LIBAV`) adds inline
  **VP9/VP8 video** (libav demux+decode → swscale → texture, in
  `src/video_decode.c`) and **Opus/Vorbis audio** (decoded in the helper,
  `src/audio/main.c`) for `.webm`/`.opus`/`.ogg` sources. It is **required on
  Linux and Windows** (`libav_required` in `meson.build` — YouTube and most
  modern sites serve VP9/WebM, so the external-player fallback there is
  unacceptable; the Windows CI/packaging builds a minimal LGPL FFmpeg via
  `scripts/build-ffmpeg-lgpl.sh` and the `--werror` build fails without it)
  and **auto-detected on macOS** (a stock build there without libav carries no
  libav symbol or dependency and behaves exactly as before). The version floor
  is FFmpeg 8.0's library sonames (libavcodec ≥ 62, libavutil ≥ 60, …). Android
  stays on the external-player path — its dependency sysroot does not
  cross-build FFmpeg. Other `<audio>` and other `<video>`
  codecs render a
  poster and play overlay; clicking resolves the media URL in the renderer
  (`ns_browser_media_at`) and reports it over the renderer protocol for
  embedders — the GTK shell does not launch an external player.
- UI strings are English-source and translated to the operating-system
  language at startup through the in-tree catalogue lookup (`src/i18n.c`,
  `data/i18n/*.lang`); English is the fallback for any string a catalogue
  does not cover. No gettext dependency.
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

- **Permissions: run in `bypassPermissions` mode.** `.claude/settings.json`
  sets `defaultMode: bypassPermissions` plus a broad allow-list for the
  build/run/git/inspect workflow, so routine commands must never prompt.
  If the session is still prompting, it was started in a more restrictive
  mode — start with `--dangerously-skip-permissions` (or pick bypass in the
  trust dialog). Don't burn turns getting individual commands approved.
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
- **Local machine is the build *and* run oracle.** The repo can be
  driven from either a Linux box (GTK 4 / libcurl / meson / clang +
  an X session at `DISPLAY=:0`) or a Windows 11 box via MSYS2
  MINGW64 (same toolchain, same meson/ninja invocation; the binary
  is `./builddir/src/gtk/nordstjernen.exe`). Every commit must pass
  `meson compile -C builddir` locally before pushing. Smoke-launch
  the browser (in the background, then kill it) on material changes
  — that's the per-change correctness gate, not CI. See
  `docs/Windows.md` for the MSYS2 setup; the rest of this guide
  uses Unix-style invocations that work in either shell.
- **CI is enabled.** The Linux / macOS / Windows workflows run on
  every push to `main` and every PR targeting `main`, plus manual
  `workflow_dispatch`. Local Linux is still the primary
  correctness gate before pushing; CI provides cross-platform
  sanity coverage.

## Build / verify locally

The intended build system is **meson + ninja**. From a clean checkout:

```sh
meson setup builddir
meson compile -C builddir
./builddir/src/gtk/nordstjernen
```

The QuickJS engine is integrated into the main tree at `src/quickjs/`
(forked from [quickjs-ng](https://github.com/quickjs-ng/quickjs); we
modify it freely — JIT, profiling, and other browser-side hooks live
there). It is not a meson subproject — `src/quickjs/meson.build` is
loaded via `subdir()` from the top-level and exposes `libquickjs` as
a declared dependency directly in the parent scope.

### HTML engine: Lexbor

The single HTML→DOM backend is
[lexbor](https://github.com/lexbor/lexbor). It is integrated into
the main tree at `src/lexbor/` (forked from upstream; we modify it
freely for tight browser integration). It is not a meson subproject
— `src/lexbor/meson.build` is loaded via `subdir()` from the
top-level and exposes `liblexbor` as a declared dependency directly
in the parent scope. No system lexbor or CMake fallback is consulted
— the in-tree copy is always built.

### Image decoding: Wuffs

PNG, GIF, BMP, and JPEG bytes are decoded through
[Wuffs](https://github.com/google/wuffs), a memory-safe
transpiled-to-C image-decoder library. The single-file release is
vendored at `subprojects/wuffs/wuffs-v0.4.c` and built as a static
subproject. `src/image_wuffs.c::ns_image_decode_wuffs` is tried
first; it returns NULL for any other format, in which case
`src/image.c::ns_image_decode_bytes` falls back to GDK-Pixbuf
(for TIFF / ICO / WebP / etc.) and, last, to librsvg for SVG.

### URL parsing: lexbor URL module

The `ns_url_*` helpers in `src/net.c` route URL resolution, origin
extraction, and host extraction through `lxb_url_parse` /
`lxb_url_serialize` from lexbor's WHATWG URL module. No separate URL
library or build option — it's part of the same `liblexbor_static.a`
that the HTML parser uses.

### Charset detection: uchardet

Required dependency (Debian/Ubuntu `libuchardet-dev`,
Fedora/RHEL `uchardet-devel`). `ns_html_decode_body` hands the
response body to [uchardet](https://www.freedesktop.org/wiki/Software/uchardet/)
to identify the charset, then `g_convert`s to UTF-8. No
hand-rolled BOM / HTTP-charset / meta-charset sniffing — uchardet
handles all of that internally. The Latin-1 fallback only fires
if uchardet can't classify the bytes at all.

### WebP: libwebp

Required dependency (Debian/Ubuntu `libwebp-dev`, Fedora/RHEL
`libwebp-devel`, openSUSE `libwebp-devel`, Alpine `libwebp-dev`,
MSYS2 `mingw-w64-x86_64-libwebp`, Homebrew `webp`). WebP —
lossy VP8 (the dominant variant served by the BBC, Wikipedia
thumbnails, and most modern CDNs), lossless VP8L, and **animated
WebP** (via libwebpdemux's `WebPAnimDecoder`, playing like animated
GIFs) — is decoded in-tree by `src/image_webp.c`, tried right after
the Wuffs decoders. No gdk-pixbuf loader, no
`loaders.cache` registration, and no sandbox interaction is
involved; the old `webp-pixbuf-loader` runtime dependency is gone.

### Web Cryptography: OpenSSL libcrypto

Required dependency (Debian/Ubuntu `libssl-dev`, Fedora/RHEL
`openssl-devel`, openSUSE `libopenssl-devel`). `crypto.subtle` (the
WebCrypto SubtleCrypto surface) is implemented in `src/webcrypto.c`
directly over OpenSSL's EVP/`OSSL_PARAM` APIs — hashing, HMAC, AES
(GCM/CBC/CTR/KW), RSA (PKCS1/PSS/OAEP), ECDSA/ECDH (P-256/384/521),
Ed25519/X25519, PBKDF2 and HKDF. The QuickJS `CryptoKey` class and `subtle.*` argument
marshalling live in `src/js.c`. OpenSSL is already linked transitively
through libcurl's TLS backend on Linux and Windows/MSYS2; `meson`
depends on `libcrypto` explicitly so the headers resolve.

System packages required on Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config meson ninja-build \
    libgtk-4-dev libepoxy-dev libcurl4-openssl-dev libssl-dev libuchardet-dev librsvg2-dev \
    libpsl-dev libsqlite3-dev libseccomp-dev libwebp-dev libavif-dev libsdl2-dev
```

Optional: `libenchant-2-dev` (plus a dictionary such as `hunspell-en-us`)
enables on-screen spell-checking of editable text. It is auto-detected —
the build works without it and simply does no spell-checking.

The FFmpeg libav\* dev packages (Debian/Ubuntu `libavformat-dev
libavcodec-dev libavutil-dev libswscale-dev libswresample-dev`;
MSYS2 `mingw-w64-x86_64-ffmpeg`, or the LGPL build from
`scripts/build-ffmpeg-lgpl.sh`) enable inline WebM playback (VP9/VP8 video
+ Opus/Vorbis audio). **Required on Linux and Windows** (`meson setup` fails
without them, or with a pre-8.0 FFmpeg); auto-detected on macOS, where without
them the build carries no libav dependency and WebM falls back to the
external-player path. FFmpeg 8.0 or newer is required.

On Fedora/RHEL:

```sh
sudo dnf install gcc pkgconf meson ninja-build gtk4-devel libepoxy-devel libcurl-devel \
    openssl-devel uchardet-devel librsvg2-devel libpsl-devel sqlite-devel \
    libseccomp-devel libwebp-devel libavif-devel SDL2-devel
```

On openSUSE:

```sh
sudo zypper install gcc pkgconf meson ninja gtk4-devel libepoxy-devel libcurl-devel \
    libopenssl-devel libuchardet-devel librsvg-devel libpsl-devel sqlite3-devel \
    libseccomp-devel libwebp-devel libavif-devel libSDL2-devel
```

`libseccomp` is required on Linux — `meson setup` fails without it.
On macOS and Windows it is not used and the syscall filter is a no-op.

### Fast iteration (recommended for AI/Claude loops)

`ccache` is the single biggest build-time win and meson picks it up
automatically. With `ccache` installed, a clean `meson setup builddir
&& meson compile -C builddir` drops from ~35s to ~1s once the cache
is warm — in-tree libraries (lexbor, quickjs) hit the cache
and re-link in negligible time. Install once:

```sh
sudo apt install ccache       # Debian/Ubuntu
sudo dnf install ccache       # Fedora/RHEL
```

Optionally use the `lld` linker for faster final links
(`CC_LD=lld meson setup builddir`). Not required.

`./scripts/dev.sh build` runs `meson setup` (only if needed) and
`meson compile -C builddir` in one shot — use it instead of typing
the two commands separately.

### WPT scoreboard

`docs/wpt-scores.md` tracks web-platform-tests scores over time and
documents the improvement loop: pick the highest-ROI area from its
"ROI by area" table, find the failing subtests in
`docs/wpt-subtests.tsv`, fix the engine, then rerun just that area
with `scripts/wpt-score.sh --wpt-root=~/wpt AREA` — it updates the
doc and data files in place. Commit the regenerated files together
with the engine change. When asked to improve the WPT score, start
from the "Top 10 improvements" list there (or the ROI table if the
list is stale), and re-cluster the list when the scores move.

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
  upstream browser engine source. Nordstjernen is an independent
  implementation, not a fork.
- **Don't add site-specific hacks.** No per-site rendering shims, no
  hardcoded hostnames, no grepping a site's private JSON (e.g.
  `ytInitialPlayerResponse`, `mediaDefinitions`, `ytimg`, `movie_player`).
  Always improve the **generic** engine so it runs the **real** site: read
  web standards (OpenGraph, JSON-LD, WHATWG DOM/JS APIs) and aim to execute
  the site's own JavaScript rather than reverse-engineering its data. When a
  page renders wrong, fix the engine capability it exercises — never
  special-case the host. The standards-based media metadata extractor in
  `src/html_lexbor.c` is the pattern; the deleted YouTube scraper was the
  anti-pattern.
- Don't add AI-style web-API surface area, even as stubs. WebGL is a
  deliberate opt-in exception — extend `src/webgl.c`, don't re-architect it.
- WebGPU is an experimental exception layered over external wgpu-native
  (`src/webgpu.c`, `docs/webgpu.md`). The `webgpu` feature is `auto`: built
  only when wgpu-native is actually present, so a machine without it still
  gets a build with no WebGPU surface or dependency. Keep it behind the
  `--enable-webgpu` / `NS_WEBGPU_ALLOW` runtime gate, and don't make
  wgpu-native a hard/default dependency or vendor its library into the tree
  (headers only) — a stock `meson setup builddir` on a clean machine must
  still produce a WebGPU-free binary.
- Don't add telemetry, crash reporters, update pingers, or "studies"
  infrastructure. UI translation goes through `src/i18n.c` and the
  `data/i18n/*.lang` catalogues — don't introduce gettext or `.po`
  tooling.
- Don't write planning docs unless asked.
