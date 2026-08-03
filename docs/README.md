# Nordstjernen documentation

Index of the docs in this directory. The project overview is in the
top-level [README.md](../README.md); the development plan is
[NORDSTJERNEN.md](../NORDSTJERNEN.md); the AI/Claude working rules are in
[CLAUDE.md](../CLAUDE.md).

## Using the browser

- [Controls.md](Controls.md) — keyboard, mouse, and touch controls.
- [media.md](media.md) — how `<video>`/`<audio>` play (MPEG-1, optional WebM, the audio helper, WebVTT `<track>` captions, external-player fallback).
- [Proxy.md](Proxy.md) — proxies and VPNs.
- [http-backends.md](http-backends.md) — the curl vs nghttp2 HTTP client backends (`-Dhttp_backend`).
- [V8.md](V8.md) — the experimental V8 JavaScript engine backend (`-Djs_engine=v8`).
- [privacy-policy.md](privacy-policy.md) — what the browser does and does not collect.

## Install, build & packaging

- [Linux.md](Linux.md) — build, run, and package on Linux.
- [Windows.md](Windows.md) · [windows-store.md](windows-store.md) — Windows build and the Microsoft Store package.
- [macOS.md](macOS.md) — macOS install (first-launch quarantine step, troubleshooting), build, `.app`/`.dmg`, and distribution (Developer ID notarisation, Mac App Store).
- [Android.md](Android.md) — Android build and Google Play release.
- [iOS.md](iOS.md) — the iOS port: structure, build, and what remains.
- [Debian.md](Debian.md) · [Ubuntu.md](Ubuntu.md) · [opensuse.md](opensuse.md) · [Alpine.md](Alpine.md) — per-distro packaging.
- [Nightly.md](Nightly.md) — the nightly build server and artifact matrix.
- [Embedding.md](Embedding.md) — embedding the engine in a C application.
- [vendored-engine-updates.md](vendored-engine-updates.md) — pulling upstream into the in-tree QuickJS and lexbor forks.

## Architecture & internals

- [Software-Architecture.md](Software-Architecture.md) — the whole system, from the process model down to each engine subsystem.
- [Rendering.md](Rendering.md) — rendering and scrolling.
- [preloading.md](preloading.md) — the speculative preload scan, the request key that identifies a fetch, and the four layers that keep a subresource from being fetched twice.
- [tab-isolation.md](tab-isolation.md) — process-per-tab renderers and the sandbox boundary.
- [single-process-mode.md](single-process-mode.md) — the `--single-process` fallback.
- [threading.md](threading.md) — the threading model.
- [watchdog.md](watchdog.md) — the watchdog supervisor.

## Web platform & standards

- [HTML-compatibility.md](HTML-compatibility.md) — section-by-section WHATWG HTML coverage.
- [CSS-compatibility.md](CSS-compatibility.md) — CSS feature coverage.
- [webgl.md](webgl.md) — WebGL 1/2 over OpenGL ES, enabled by default.
- [webgpu.md](webgpu.md) — experimental WebGPU over wgpu-native.
- [webassembly.md](webassembly.md) — the WebAssembly JS API over WAMR.
- [extensions.md](extensions.md) — the scoped WebExtensions support (content scripts and a slice of `browser.*`).
- [i18n.md](i18n.md) — UI translation (the in-tree catalogue, no gettext).
- [quickjs-ecma-specification-compliance.md](quickjs-ecma-specification-compliance.md) · [quickjs-libjs-compare.md](quickjs-libjs-compare.md) — JavaScript engine compliance and comparison.

## Testing & benchmarks

- [wpt.md](wpt.md) — running web-platform-tests against the browser.
- [wpt-scores.md](wpt-scores.md) — tracked WPT scores over time.
- [wpt-fast-scores.md](wpt-fast-scores.md) — scores from the wpt-fast tree (`scripts/wpt-fast.sh`).
- [Benchmarking.md](Benchmarking.md) — Speedometer benchmarking.
