# Nordstjernen documentation

Index of the docs in this directory. The project overview is in the
top-level [README.md](../README.md); the development plan is
[NORDSTJERNEN.md](../NORDSTJERNEN.md); the AI/Claude working rules are in
[CLAUDE.md](../CLAUDE.md).

## Using the browser

- [Controls.md](Controls.md) — keyboard, mouse, and touch controls.
- [media.md](media.md) — how `<video>`/`<audio>` play (MPEG-1, optional WebM, the audio helper, WebVTT `<track>` captions, external-player fallback).
- [ai.md](ai.md) — the local AI start page (`about:start`), on-device via llama.cpp.
- [Proxy.md](Proxy.md) — proxies and VPNs.
- [http-backends.md](http-backends.md) — the curl vs nghttp2 HTTP client backends (`-Dhttp_backend`).
- [privacy-policy.md](privacy-policy.md) — what the browser does and does not collect.

## Install, build & packaging

- [Linux.md](Linux.md) — build, run, and package on Linux.
- [Windows.md](Windows.md) · [windows-store.md](windows-store.md) — Windows build and the Microsoft Store package.
- [macOS.md](macOS.md) — macOS install (first-launch quarantine step, troubleshooting), build, `.app`/`.dmg`, and distribution (Developer ID notarisation, Mac App Store).
- [Android.md](Android.md) — Android build and Google Play release.
- [Debian.md](Debian.md) · [Ubuntu.md](Ubuntu.md) · [opensuse.md](opensuse.md) · [Alpine.md](Alpine.md) — per-distro packaging.
- [Nightly.md](Nightly.md) — the nightly build server and artifact matrix.
- [Embedding.md](Embedding.md) — embedding the engine in a C application.

## Architecture & internals

- [Rendering.md](Rendering.md) — rendering and scrolling.
- [tab-isolation.md](tab-isolation.md) — process-per-tab renderers and the sandbox boundary.
- [single-process-mode.md](single-process-mode.md) — the `--single-process` fallback.
- [threading.md](threading.md) — the threading model.
- [watchdog.md](watchdog.md) — the watchdog supervisor.
- [ipc-http-experiment.md](ipc-http-experiment.md) — the renderer IPC design experiment.

## Web platform & standards

- [HTML-compatibility.md](HTML-compatibility.md) — section-by-section WHATWG HTML coverage.
- [CSS-compatibility.md](CSS-compatibility.md) — CSS feature coverage.
- [webgl.md](webgl.md) — opt-in WebGL 1/2 over OpenGL ES.
- [webgpu.md](webgpu.md) — experimental WebGPU over wgpu-native.
- [webassembly.md](webassembly.md) — the WebAssembly JS API over WAMR.
- [i18n.md](i18n.md) — UI translation (the in-tree catalogue, no gettext).
- [quickjs-ecma-specification-compliance.md](quickjs-ecma-specification-compliance.md) · [quickjs-libjs-compare.md](quickjs-libjs-compare.md) — JavaScript engine compliance and comparison.

## Testing & benchmarks

- [wpt.md](wpt.md) — running web-platform-tests against the browser.
- [wpt-scores.md](wpt-scores.md) — tracked WPT scores over time.
- [Benchmarking.md](Benchmarking.md) — Speedometer benchmarking.

## Frontends & project

- [nordstjernen.org.md](nordstjernen.org.md) — the website plan.
