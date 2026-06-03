# Nordstjernen for Android

An Android host app that drives the Nordstjernen browser engine through its C
embedding API (`src/libnordstjernen.h`). The engine — HTML parsing (lexbor),
CSS cascade + layout, JavaScript (QuickJS), image decoding (Wuffs), and cairo
painting — is the same clean-room code used on the desktop. The Android UI is a
thin Kotlin shell: a URL bar over a `PageView` that renders the engine's RGBA
output and re-renders as you scroll (with fling), plus pinch- and
double-tap-to-zoom (2D pan when zoomed), back/forward history, reload,
tap-to-follow-link (with a touch-tolerant hit radius), long-press for a link
menu (open / copy), page-title display, scroll position preserved across
rotation, and `http(s)` `VIEW`-intent handling so the app can be set as the
system browser. Pages are laid out at a phone-width CSS viewport (device width
÷ display density) and painted scaled by the density (× the zoom), so text is
mobile-sized and crisp at any zoom level — re-rendered by the engine rather
than bitmap-stretched.

## Architecture

```
 Kotlin UI (MainActivity, PageView)
        │  JNI
 nd_jni.c  ──►  libnordstjernen.so  (engine: net, dom, css, layout, js, paint)
                       │
                       └─► glib · cairo · pango · pangocairo · harfbuzz ·
                           freetype · fontconfig · libcurl · sqlite3 ·
                           uchardet · libpsl       (cross-compiled deps)
```

The engine drops **GTK 4, librsvg and gdk-pixbuf** on Android: `GdkTexture`
is replaced by the `nd_texture` abstraction (`src/texture.c`), and the SVG /
fallback image decoders are gated out. So the Android dependency set is just
the GLib/cairo/pango graphics stack plus networking/storage — all plain C, no
Rust toolchain required. PNG/JPEG/GIF/BMP still decode via the in-tree Wuffs;
SVG and uncommon formats render as a broken-image box for now.

`nd_browser_render_rgba()` (added to the embedding API for Android) paints a
viewport region straight into an Android `ARGB_8888` Bitmap via
`AndroidBitmap_lockPixels`, so there is no PNG round-trip.

The engine does **not** use GTK widgets — only the GLib/cairo/pango graphics
stack — so no GTK is needed on Android. `src/env.c` and `src/net.c` carry small
`__ANDROID__` guards (GTK version reporting is skipped; the CA bundle is taken
from `CURL_CA_BUNDLE`, which the host app points at a bundled `cacert.pem`).

## Layout

```
android/
  app/
    build.gradle                       app module (AGP + Kotlin + CMake)
    src/main/AndroidManifest.xml
    src/main/cpp/CMakeLists.txt         builds the JNI bridge
    src/main/cpp/nd_jni.c               real bridge → engine
    src/main/cpp/nd_jni_stub.c          fallback when the engine isn't bundled
    src/main/java/.../NativeBrowser.kt  JNI facade
    src/main/java/.../PageView.kt       scrolling render surface
    src/main/java/.../MainActivity.kt   URL bar + navigation
    src/main/res/raw/cacert.pem         CA bundle for libcurl
  scripts/build-deps.sh                 cross-compile engine → jniLibs/<abi>/
  settings.gradle · build.gradle · gradle.properties
```

## Building the APK

```sh
cd android
gradle wrapper          # once, to generate ./gradlew (or use a system gradle)
./gradlew assembleDebug  # -> app/build/outputs/apk/debug/app-debug.apk
```

Requires JDK 17, the Android SDK (compileSdk 35), and CMake 3.22+ from the SDK.
`minSdk` is 26, `targetSdk` 35.

### Two build modes

`CMakeLists.txt` checks for a prebuilt engine at
`app/src/main/jniLibs/<abi>/libnordstjernen.so`:

* **present** — the real JNI bridge (`nd_jni.c`) is compiled and linked against
  it. The app loads and renders real pages.
* **absent** — the stub bridge (`nd_jni_stub.c`) is built so the APK still
  assembles and runs; `NativeBrowser.available` is `false` and the UI shows a
  banner. This is what CI produces today and what lets UI work proceed before
  the dependency stack is cross-built.

### Producing the engine .so

```sh
ANDROID_NDK_HOME=~/Android/Sdk/ndk/26.3.11579264 \
NORDSTJERNEN_ANDROID_SYSROOT=~/nd-android-sysroot/arm64-v8a \
android/scripts/build-deps.sh arm64-v8a 26
```

The script generates a meson cross-file for the NDK toolchain, cross-compiles
the `nordstjernen` shared library, and stages it plus its `.so` dependencies
into `jniLibs/arm64-v8a/`.

## Status

* **Done & verified on desktop:** engine `__ANDROID__` guards, the
  `nd_browser_render_rgba` (density-scaled) / `nd_browser_page_size` /
  `nd_browser_link_at` / `nd_browser_title` embedding API (compiles clean under
  the project's `--werror` flags, and the render/title paths are exercised by a
  link-test against the built library), the JNI bridge, the full Kotlin app
  (history, reload, link following, fling, intent handling, mobile-width
  rendering, title), and the CMake auto-detect/stub wiring.
* **Wired, CI-built as the stub:** the Android Gradle/NDK build assembles an
  APK on every push via `.github/workflows/android.yml`.
* **Done & verified on desktop (dependency shrink):** the engine no longer
  needs GTK 4, librsvg or gdk-pixbuf — `GdkTexture` is abstracted behind
  `nd_texture` (`src/texture.c`; a GDK wrapper on desktop, a BGRA buffer on
  Android) and the SVG/fallback decoders are gated behind `ND_HAVE_LIBRSVG` /
  `ND_HAVE_GDK_PIXBUF`. The desktop build is byte-for-byte behaviourally
  identical (renders images to PNG as before).
* **Android sources verified-compiling:** `scripts/check-android-sources.sh`
  re-checks every engine translation unit under the Android configuration
  (`__ANDROID__`, no gdk-pixbuf/librsvg) with `clang -fsyntax-only`, reusing
  the desktop `compile_commands.json` — no NDK required. It runs in the Linux
  CI job, so Android-source regressions are caught on every build. All 36
  engine sources pass.
* **Remaining:** cross-compiling the native dependency sysroot (the
  GLib/cairo/pango stack). It is all plain C now — no Rust — and cross-builds
  with meson against the NDK. Once a sysroot exists, run `build-deps.sh` and
  the same APK ships the real engine. This step needs the NDK + network access
  (wrapdb / dependency sources) and so is driven in CI / on an NDK box.
