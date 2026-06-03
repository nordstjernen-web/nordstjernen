# Nordstjernen on Android — build & Google Play release

The Android port lives in [`android/`](../android). This document is the
**release** guide: how to build, sign, and ship to the Google Play Store.
For the engineering architecture, see [`android/README.md`](../android/README.md).
F-Droid is a separate, easier track and is out of scope here.

## Distribution model

Free, ad-free, no-telemetry, no in-app purchase, no subscription — the
Android build matches the desktop build. There is nothing to bill for, so
there is no Play Billing, no AdMob, and no Advertising ID. Donations and
commercial support are arranged off-Play at `nordstjernen.org`; Play ships
the free binary only. Treat Play as reach and reputation.

## The port in brief

- Kotlin UI shell over the C embedding API (`src/libnordstjernen.h`) through a
  thin JNI bridge — URL bar, history, reload, scroll/fling, tap-to-follow-link,
  and `http(s)` `VIEW` intents.
- The same clean-room engine, cross-compiled to a per-ABI `libnordstjernen.so`.
  On Android it drops GTK 4, librsvg and gdk-pixbuf (see `android/README.md`),
  so its only native deps are the GLib/cairo/pango stack plus
  libcurl/sqlite3/uchardet/libpsl — all plain C, no Rust.
- Targets: `compileSdk`/`targetSdk` **35**, `minSdk` **26**; ABIs
  **arm64-v8a** + **x86_64** (add `armeabi-v7a` once its sysroot builds).
  AGP 8.6, Gradle 8.9, JDK 17.

## Building

```sh
cd android
gradle wrapper                 # once, to generate ./gradlew (or use a system gradle)
./gradlew assembleDebug        # APK -> app/build/outputs/apk/debug/
./gradlew bundleRelease        # AAB -> app/build/outputs/bundle/release/  (Play upload)
```

`bundleRelease` is signed only when `android/keystore.properties` exists (see
[Signing](#signing)); otherwise it is unsigned. Play requires the App Bundle
(`.aab`) for new apps.

### Native engine (.so)

`android/scripts/build-deps.sh <abi> <api>` generates an NDK meson cross-file,
cross-compiles the engine against a dependency sysroot
(`NORDSTJERNEN_ANDROID_SYSROOT`), and stages it into `jniLibs/<abi>/`. While
that file is absent, `CMakeLists.txt` links a **stub** bridge so the APK still
builds and runs (engine reported unavailable); once present, the real bridge is
linked and pages render.

Cross-building the sysroot (glib, gobject, gio, gmodule, cairo, pango,
pangocairo, harfbuzz, freetype, fontconfig and their transitive C deps, plus
libcurl, sqlite3, uchardet, libpsl) is the remaining porting work. It is all
plain C and builds with meson against the NDK.

## Signing

Use **Play App Signing** (the default for new apps): Google holds the signing
key, you hold the *upload* key. A lost upload key is recoverable via Play
support; if you opt out and lose the signing key the app is dead, so don't opt
out. Back the upload key + passwords up in two places and test the backup
yearly.

1. Generate the upload key once:

   ```sh
   keytool -genkey -v -keystore nordstjernen-upload.jks \
       -alias upload -keyalg RSA -keysize 4096 -validity 25000
   ```

2. Create `android/keystore.properties` — **git-ignored, never commit**:

   ```properties
   storeFile=/secure/path/nordstjernen-upload.jks
   storePassword=…
   keyAlias=upload
   keyPassword=…
   ```

   `app/build.gradle` picks this up and signs `release` builds automatically.

## Play policy — the non-negotiables

- **Target API level.** Play requires `targetSdk` no older than current Android
  minus one (API 35 in 2026). The project targets 35.
- **Data safety form.** Declare *no data collected, no data shared, encrypted
  in transit, deletion via uninstall* — all true. Play surfaces this on the
  listing.
- **Permissions.** Only `INTERNET` + `ACCESS_NETWORK_STATE` (already in the
  manifest). No notifications, location, contacts, camera, or microphone. Add
  `READ_MEDIA_IMAGES` only when `<input type=file>` upload lands.
- **Content rating.** The IARC questionnaire puts an open-web browser at
  **Teen** / **PEGI 12**. Answer truthfully; don't claim Everyone.
- **Browsers category.** Don't impersonate other browsers in name/icon; let the
  user change the default search engine; keep the `http`/`https` `VIEW` filter
  (already present). To appear in the system default-browser chooser, add a
  `RoleManager.ROLE_BROWSER` request flow on Android 10+ (small follow-up).
- **Don't wire** the Play Integrity API or any ads/advertising-ID SDK.

## Submission & rollout

1. **Store listing** — name, 80-char short and 4000-char full descriptions,
   512×512 icon, 1024×500 feature graphic, 2–8 phone screenshots (captured from
   a real-engine build), category **Browsers**.
2. **Forms** — complete data safety + content rating (above).
3. **Internal testing** — upload the first signed AAB; review takes minutes.
4. **Closed testing** — a new app needs ≥14 days with ≥12 testers before
   production.
5. **Production** — first browser review takes 3–7 days; roll out in stages
   (10% → 100%) watching the crash dashboard.

Versioning: `versionCode` is a monotonic int; `versionName` tracks the desktop
"0.8.x". For a critical security fix, halt rollout, then re-submit on a fast
rollout.

## CI

`.github/workflows/android.yml` builds the debug APK on every push/PR (stub
engine until a sysroot is wired) and captures an emulator screenshot. To
produce a signed AAB for Play, add a manually-triggered job that decodes a
base64 keystore secret, writes `keystore.properties`, runs
`./gradlew bundleRelease`, and uploads the `.aab` — do **not** auto-publish;
upload to the internal track by hand.

## Out of scope

- **F-Droid** — separate track; reproducible builds are the only twist.
- **Samsung / Huawei / Amazon stores** — year-two store-listing ports.
- **iOS** — different document, much worse cost/benefit.
