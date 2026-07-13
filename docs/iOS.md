# Nordstjernen on iOS — build & architecture

The iOS port lives in [`ios/`](../ios). This document is the engineering guide:
how the port is structured, how to build it, and what remains. For the app
layout and a status snapshot see [`ios/README.md`](../ios/README.md).

## The port in brief

- **UIKit/Swift UI shell** over the C embedding API (`src/libnordstjernen.h`)
  through a thin C bridge (`ios/App/Bridge/ns_ios.*`) — URL bar, back/forward
  history, reload, a scrolling `PageView`, and tap-to-follow-link.
- The **same engine**, cross-compiled to a static
  `libnordstjernen.a`. Like Android, iOS drops GTK 4, librsvg and gdk-pixbuf,
  so its only native dependencies are the GLib/cairo/pango stack plus
  libcurl/sqlite3/uchardet/libpsl — all plain C, no Rust.
- **Targets:** iOS 15+, `arm64` device and `arm64` simulator. No bitcode.

## Why iOS is an Android-style port, not a macOS one

The macOS build ships the full **GTK 4 desktop app**. GTK 4 has no iOS backend
(iOS renders through UIKit/Core Animation, not X11 or Wayland), so "builds on
macOS" does not carry to iOS. What *is* portable is the engine underneath the
toolkit — the same `libnordstjernen` that Android already runs headless. iOS
therefore reuses the Android split: the GTK-free engine as a library, plus a
native shell that renders its ARGB output and forwards input.

In `meson.build` this is the `is_mobile = is_android or is_ios` predicate. An
iOS configure (`-Dios=true`, set by the meson cross file) builds only the
embeddable engine library — no desktop shell, out-of-process renderer, or
audio/video helper executables — exactly as Android does.

## Rendering

`ns_browser_render_argb32()` paints a viewport region into a caller buffer in
Cairo-native ARGB32 (premultiplied, host byte order). On little-endian arm64
that is exactly a Core Graphics `byteOrder32Little` + `premultipliedFirst`
image, so `PageView` wraps the buffer as a `CGImage` with no channel swizzle.
The engine lays the page out at a phone-width CSS viewport and renders scaled by
the display density (`UIScreen.scale`), so text is crisp at native resolution —
re-rendered by the engine on scroll rather than bitmap-stretched.

## Building

The engine is not compiled inside Xcode; a script cross-compiles it against a
prebuilt dependency sysroot and writes the static libraries plus a generated
`xcconfig` that the Xcode project links against.

```sh
# 1. Fetch the prebuilt iOS dependency sysroot (device + simulator):
export NORDSTJERNEN_IOS_SYSROOT="$HOME/.cache/nordstjernen-ios-sysroot"
ios/scripts/fetch-prebuilt-deps.sh --sysroot "$NORDSTJERNEN_IOS_SYSROOT"

# 2. Cross-compile the engine (writes ios/App/vendor/ + nordstjernen.xcconfig):
ios/scripts/build-engine.sh device
ios/scripts/build-engine.sh simulator

# 3. Generate and build the Xcode project:
cd ios/App
xcodegen generate
xcodebuild -project Nordstjernen.xcodeproj -scheme Nordstjernen \
    -sdk iphonesimulator -configuration Release CODE_SIGNING_ALLOWED=NO build
```

Requires macOS + Xcode, `meson`/`ninja`, and `xcodegen`. On-device builds also
need a signing identity and provisioning profile.

## The dependency sysroot

The bulk of the porting work is cross-compiling glib, gobject, gio, gmodule,
cairo, pango, pangocairo (and their transitive deps: harfbuzz, freetype,
fontconfig, pixman, fribidi, libffi, pcre2, expat, zlib, libpng), libcurl (with
openssl, nghttp2, brotli), sqlite3, uchardet, libpsl and libwebp for iOS —
`arm64` device and `arm64` simulator, static.

This mirrors the Android sysroot exactly, and is produced the same way: the
[`nordstjernen-dependencies-build`](https://github.com/nordstjernen-web/nordstjernen-dependencies-build)
repo's `build-ios-deps` workflow cross-compiles the whole stack on a macOS
runner and publishes it as the rolling `ios-sysroot-latest` release, which
`fetch-prebuilt-deps.sh` downloads and verifies.

## CI

`.github/workflows/ios.yml`:

- **engine-portability** — cross-checks every engine translation unit against
  the real iOS SDK (device + simulator) on every push, catching iOS-portability
  regressions early. Needs no sysroot; the iOS analogue of the Android source
  check.
- **app** — fetches the published sysroot, cross-compiles the engine for device
  and simulator, and assembles the unsigned UIKit app for the iOS Simulator,
  uploading the resulting `.app` as the `nordstjernen-ios-simulator-app`
  artifact. (It still skips with a notice if the sysroot release is ever
  unavailable.)

## Status

- **Builds end-to-end in CI (macOS):** the published iOS dependency sysroot is
  fetched and relocated to the runner, the engine cross-compiles to
  `libnordstjernen.a` for both device and simulator, and `xcodebuild` assembles
  the unsigned UIKit app for the iOS Simulator — uploaded as the
  `nordstjernen-ios-simulator-app` artifact. The engine iOS-portability source
  check (device + simulator) also runs on every push.
- **Verified (Linux):** the GTK-free `is_mobile` engine build (engine library
  only under `-Dios=true`); the desktop build is unchanged by the refactor; the
  `TARGET_OS_IPHONE` sandbox guard.
- **Not yet done:** a signed on-device build (needs a signing identity and a
  provisioning profile); booting and driving the app in a simulator, and
  real-world browsing validation (CI builds the app but does not run it); App
  Store submission.

## Distribution (future)

Like the desktop and Android builds: free, ad-free, no telemetry, no in-app
purchase. App Store submission (signing, provisioning, review) is out of scope
until the app builds and runs on device.
