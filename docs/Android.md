# Nordstjernen on Android — build & Google Play release

Nordstjernen is **live on the Google Play Store**:
<https://play.google.com/store/apps/details?id=org.nordstjernen.WebBrowser>
— free, ad-free, no telemetry.

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
- The same engine, cross-compiled to a per-ABI `libnordstjernen.so`.
  On Android it drops GTK 4, librsvg and gdk-pixbuf (see `android/README.md`),
  so its only native deps are the GLib/cairo/pango stack plus
  libcurl/sqlite3/uchardet/libpsl — all plain C, no Rust.
- Targets: `compileSdk`/`targetSdk` **36**, `minSdk` **34** (Android 14); ABIs
  **arm64-v8a** + **x86_64**.
  AGP 8.11.1, Gradle 8.14.5, NDK r27, JDK 17.

## Play Store release checklist

Nordstjernen is now **published on Google Play** at
<https://play.google.com/store/apps/details?id=org.nordstjernen.WebBrowser>.
The code path — `targetSdk 36`, `minSdk 34`, `arm64-v8a` + `x86_64`, 16 KB
page-size linker flags, edge-to-edge handling, Play upload-key wiring, a manual
release AAB workflow, and the minimal `INTERNET` / `ACCESS_NETWORK_STATE`
permissions — is all in-tree. The checklist below records the Play Console setup
and release paperwork that took the app to production, and is the reference for
each subsequent release:

1. **Finish developer account verification.** Decide personal vs organisation
   account. Organisation accounts need D-U-N-S-backed business details; personal
   accounts need verified identity and contact details. Keep the public
   developer email/phone operational because Play verifies those fields.
2. **Publish the privacy policy.** Publish `docs/privacy-policy.md` at
   `https://nordstjernen.org/privacy`, add that URL in Play Console, and make
   the same policy reachable from inside the Android app before production
   review. The Play policy requires both the store-field link and an in-app link
   or text, even when the app collects no project-side user data.
3. **Create production store assets.** Prepare the app name, 80-character short
   description, full description, category **Browsers**, 512x512 icon, 1024x500
   feature graphic, and 2-8 phone screenshots captured from a real-engine
   Android build. Avoid screenshots that imply filtering, VPN, privacy proxying,
   sync, or ad blocking unless those features are actually present.
4. **Complete Play Console App content forms.** Data safety should say the
   Nordstjernen project collects and shares no user data; browser traffic goes
   only to sites the user visits and to the configured search engine. Also
   complete content rating, target audience, ads, app access, financial/health/
   government/news declarations, and any permissions declarations Play asks for.
5. **Create and protect the upload key.** Use Play App Signing, store the upload
   keystore offline, and set the four `ANDROID_*` GitHub secrets documented
   below so `.github/workflows/android-release.yml` emits a signed `.aab`.
6. **Build the release bundle.** Trigger the `android-release` workflow or run
   `gradle bundleRelease` locally, then upload the resulting `.aab` to an
   internal testing track first. Play requires Android App Bundles for new apps.
7. **Run Play-delivered smoke tests.** Install from the internal track on at
   least a real Pixel and an emulator. Re-check startup, default-browser role,
   `about:start`, DuckDuckGo search, Hacker News login/cookies, Wikipedia
   mobile routing, rotation, text input, back/reload/home, and crash-free launch.
8. **Run closed testing if required.** New personal developer accounts created
   after 13 November 2023 must run a closed test with at least 12 testers opted
   in for 14 continuous days before requesting production access. Organisation
   accounts are not subject to that personal-account production-access gate.
9. **Request production access / submit production.** Answer the production
   access questionnaire if Play shows it, upload the first production release,
   and use staged rollout (for example 10% -> 50% -> 100%) while watching Play
   Console vitals and crash reports.

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

`<api>` is the NDK platform level and defaults to **34** (Android 14) to match
`minSdk`. It must be `<=` `minSdk`: a `.so` built at a higher level can bind
bionic symbols the device lacks, so it fails to load on Android 14. The prebuilt
dependency sysroot is staged into the APK alongside the engine, so it must be
cross-built at the same API level (`<= 34`) for the app to run on Android 14.

The dependency sysroot is built by
`nordstjernen-web/nordstjernen-dependencies-build` and published as the public
`sysroot-latest` release. `android/scripts/fetch-prebuilt-deps.ps1` downloads
those release assets and verifies them against `SHA256SUMS`.

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

(Checked against Google documentation on 14 June 2026.)

- **Target API level.** Google's currently published upload requirement says
  that, from **31 August 2025**, new apps and updates must target **Android 15
  (API 35)** or higher ([policy](https://developer.android.com/google/play/requirements/target-sdk)).
  The project already targets **API 36**, so this gate is done, but re-check the
  policy page immediately before upload because Google usually moves the target
  API bar annually.
- **16 KB page sizes.** Since **1 November 2025** (final extension 31 May
  2026), all submissions targeting Android 15+ must run on devices with 16 KB
  memory pages ([docs](https://developer.android.com/guide/practices/page-sizes)).
  Pure-JVM apps pass automatically; ours ships native `.so`s, so every ELF
  segment must be 16 KB-aligned. Wired in three places: AGP ≥ 8.5.1 zip-aligns
  packaging (covered by 8.11.1), the CMake bridge build passes
  `-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON` (needed on NDK r27; r28+ aligns
  by default), and `build-deps.sh` puts `-Wl,-z,max-page-size=16384` in the
  meson cross-file for the engine and expects the dependency sysroot to be
  built the same way. Verify with APK Analyzer or
  `llvm-readelf -lW libnordstjernen.so` (LOAD segments' `Align` ≥ 0x4000)
  before uploading.
- **Edge-to-edge.** Targeting API 36 enforces edge-to-edge drawing and
  disables the Android 15 opt-out attribute on Android 16+ devices
  ([behavior changes](https://developer.android.com/about/versions/16/behavior-changes-16)).
  `MainActivity` calls `enableEdgeToEdge()` and applies system-bar / cutout /
  IME insets as root padding, so the toolbar and page never sit under bars.
- **Developer verification.** Verify the developer account identity (D-U-N-S
  number for organisations). From **September 2026** unverified developers'
  apps stop installing on certified Android devices in the first wave of
  countries — this now covers distribution *outside* Play too, so it gates the
  F-Droid/sideload story as well, not just the Play listing.
- **Data safety form.** Declare *no data collected, no data shared, encrypted
  in transit, deletion via uninstall* — all true. Play surfaces this on the
  listing. Internal testing is exempt from showing Data safety; closed, open,
  and production tracks require the form to be complete.
- **Permissions.** Only `INTERNET` + `ACCESS_NETWORK_STATE` (already in the
  manifest). No notifications, location, contacts, camera, or microphone. Add
  `READ_MEDIA_IMAGES` only when `<input type=file>` upload lands.
- **Content rating.** The IARC questionnaire puts an open-web browser at
  **Teen** / **PEGI 12**. Answer truthfully; don't claim Everyone.
- **Browsers category.** Don't impersonate other browsers in name/icon; let the
  user change the default search engine; keep the `http`/`https` `VIEW` filter
  (already present). The app requests `RoleManager.ROLE_BROWSER` once on first
  launch (Android 10+), which shows the system default-browser chooser; the
  user can change it any time in Settings, so it never re-prompts.
- **Privacy policy.** Every app needs a privacy policy URL in Play Console and
  a privacy policy link or text inside the app, even with an all-"No" data
  safety form ([policy](https://support.google.com/googleplay/android-developer/answer/17105854)).
  Point it at `https://nordstjernen.org/privacy`, using
  `docs/privacy-policy.md` as the source. Cloud backup is disabled in the
  manifest (`allowBackup="false"`), so history and cookies never leave the
  device through Google's backups either.
- **Don't wire** the Play Integrity API or any ads/advertising-ID SDK.

## Submission & rollout

Follow the checklist above. The key sequencing is: account verification,
privacy policy, signed release AAB, internal testing, closed testing if Play
requires it, then production staged rollout. Personal accounts created after
13 November 2023 need **at least 12 testers opted in for 14 consecutive days**
before applying for production access ([Play Console Help](https://support.google.com/googleplay/android-developer/answer/14151465)).

Versioning: `versionCode` is a monotonic int; `versionName` tracks the desktop
version ("1.0.x"). For a critical security fix, halt rollout, then re-submit
on a fast rollout.

## CI

`.github/workflows/android.yml` fetches the public prebuilt dependency sysroot,
cross-compiles the native engine for `arm64-v8a` and `x86_64`, and builds the
debug APK on every push/PR.

`.github/workflows/android-release.yml` is the Play build: trigger it manually
(`workflow_dispatch`), and it runs `gradle bundleRelease` and uploads the
`.aab` artifact. It signs with the upload key when these repository secrets
are set, and builds an unsigned bundle otherwise:

| Secret | Content |
| --- | --- |
| `ANDROID_UPLOAD_KEYSTORE_BASE64` | `base64 -w0 nordstjernen-upload.jks` |
| `ANDROID_KEYSTORE_PASSWORD` | keystore password |
| `ANDROID_KEY_ALIAS` | `upload` |
| `ANDROID_KEY_PASSWORD` | key password |

Nothing auto-publishes — download the artifact and upload it to the Play
Console internal track by hand.

## Out of scope

- **F-Droid** — separate track; reproducible builds are the only twist.
- **Samsung / Huawei / Amazon stores** — year-two store-listing ports.
- **iOS** — different document, much worse cost/benefit.
