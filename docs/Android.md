# Nordstjernen on Android — Google Play release plan

This document is the plan for shipping Nordstjernen to the regular
Google Play Store, not F-Droid. F-Droid is a separate (and easier)
track and is out of scope here.

The Play Store imposes constraints that conflict with the desktop
shareware model. This document spells out what changes and what
doesn't.

## Business model on Android

The desktop "shareware with a nag" model **cannot ship on Play**.
Google's Payments policy requires that any digital goods or
functionality unlocked inside the app go through **Google Play
Billing**, and Google takes 15% (small developers, first $1M/year)
to 30% of every transaction. Self-hosted license keys for in-app
unlocks are explicitly prohibited.

The Play-compatible model for Nordstjernen is:

- **Free, ad-free, no-telemetry base browser** — fully functional,
  no nag screen, no time bomb.
- **"Nordstjernen Plus" — a single one-time in-app product** sold
  through Google Play Billing at ~$9.99. Unlocks:
  - Larger bookmark sync slots
  - Reader mode export to PDF
  - A handful of cosmetic themes
  - Optional "support the developer" badge in the about box
  - Whatever genuinely lives only on mobile
- **No subscription.** Subscription on a browser tanks trust.
- **No ads, ever.** Ad SDKs require telemetry; the brand dies.
- **Desktop license holders get Plus free** via an out-of-band
  "I already paid on desktop" entitlement check at first launch
  (one-time call to your own server with the desktop license, which
  returns a signed token). This is allowed by Play because the
  unlock was paid for outside the app and is being honored, not
  sold.

Realistic Plus conversion on a free FOSS-adjacent browser: **0.3–1.5%**
of installs. At 100k installs lifetime that is 300–1500 paying users
× $9.99 × 0.85 (Play cut, small-dev tier) = **$2,500–$12,000** total
gross from Play. Treat Play as **reach and reputation**, not the
main revenue line — desktop and embedded licensing remain the
income.

## Account and key setup (do once, never lose)

1. **Google Play Console developer account** — $25 one-time, requires
   government ID verification (D-U-N-S number if you incorporate).
   Plan ~5 business days for ID verification in 2026.
2. **Google merchant account** linked to the Play Console — required
   to receive Play Billing payouts. Norwegian bank account is fine;
   Google withholds VAT for EU buyers automatically.
3. **App signing key**:
   - Use **Play App Signing**. Generate the upload key locally with
     `keytool -genkey -v -keystore nordstjernen-upload.jks -alias
     upload -keyalg RSA -keysize 4096 -validity 25000`.
   - Google holds the signing key; you hold the upload key. If you
     lose the upload key you can rotate via Play Console support.
     If you opted out of Play App Signing and lose the signing key,
     **the app is dead** — you cannot ship updates. Don't opt out.
4. **Backup**: the upload key + its password live in two places
   (encrypted USB stick + password manager). Test the backup once
   a year by signing a dummy build from it.

## Policy compliance — the non-negotiables

Play rejects apps for any of these. Get them right before first
submission, not after.

### Target API level

Play requires apps to target an API level no older than the
**current Android minus 1** as of August each year. In 2026 that
means **target API 35 (Android 15)**. The NDK `targetSdkVersion`
in `AndroidManifest.xml` and the gradle `targetSdk` must both be
35. The minSdk can be 24 (Android 7.0); that covers >97% of active
devices.

### Data safety form

Mandatory. You must declare:

- **Data collected**: *none*. Truthful for Nordstjernen.
- **Data shared**: *none*.
- **Encrypted in transit**: yes (all browser traffic is TLS).
- **User can request deletion**: trivially yes — uninstall.

The form is also the marketing pitch — fill it out aggressively
("Does not collect any data") because Play surfaces this on the
store page and FOSS-adjacent users read it.

### Permissions

Declare in `AndroidManifest.xml`, request at runtime where required:

- `INTERNET` — required, declared at install time.
- `ACCESS_NETWORK_STATE` — for the offline indicator.
- `POST_NOTIFICATIONS` — *don't* request unless you actually fire
  notifications. Today you don't.
- `READ_MEDIA_IMAGES` — only if/when you add file-upload via
  `<input type=file accept="image/*">`. Skip for v1.
- **No location, no contacts, no camera, no microphone.** Each one
  triggers a separate Play review and a row on the data safety
  form. Don't add them.

### Content rating

Run the IARC questionnaire in Play Console. A browser is rated by
"what users might encounter on the open web" — Play knows this and
you'll land at **Teen** (US/ESRB) / **PEGI 12**. Answer truthfully:
"users can navigate to any website" → Teen. Don't try to claim
Everyone; Play will reject.

### Browser-specific category

There is a specific "Browsers" category and Play has stricter rules
for it:

- Must not impersonate Chrome, Firefox, Safari, Edge, Opera in name
  or icon.
- Must declare default search engine clearly and let the user
  change it (you already support this via config).
- Must support the system intent filter for `android.intent.action.VIEW`
  with `http`/`https` schemes so the user can set Nordstjernen as
  default browser. This is a 10-line manifest entry plus a `RoleManager`
  request flow on Android 10+.
- If you want to be in the **default browser chooser**, you must
  implement Custom Tabs (or implement the `BROWSER` role properly).
  Recommend skipping Custom Tabs in v1, just take the `VIEW` intent.

### Play Integrity API

**Don't use it.** It's free up to 10k requests/day, but it requires
Google Play Services, which is itself a telemetry surface, and FOSS
users will mark you down for it. The cost of skipping is that
license keys are spoofable on rooted devices. For a $9.99 IAP this
is fine — the people who'd bother rooting to crack a $9.99 license
were never going to pay.

### Ads policy

You ship zero ads. Confirm on the form. Skip AdMob SDK entirely.

### "Deceptive behavior" policy

This is where the *desktop* nag-screen would get rejected. On
Android you have no nag — Plus is a Play Billing IAP. You are fine.

### Tracking IDs

`AdvertisingIdClient` — don't link it. Don't include the
`play-services-ads-identifier` library. The data safety form
question "does your app collect Advertising ID" must be answered
"no" and it must be true.

## Technical build — APK / AAB

Play requires **Android App Bundles (.aab)**, not APKs, for new
apps since 2021. Gradle Android Plugin handles this.

### Repository layout addition

```
android/
  app/
    build.gradle.kts
    src/main/
      AndroidManifest.xml
      java/com/nordstjernen/browser/
        NordstjernenActivity.kt    # NativeActivity wrapper
        Billing.kt                  # Play Billing v6 client
        Paths.kt                    # JNI bridge for getFilesDir, etc.
      res/
        drawable/ic_launcher_*.png
        values/strings.xml
      jniLibs/                      # populated by meson cross-build
        arm64-v8a/libnordstjernen.so
        armeabi-v7a/libnordstjernen.so
        x86_64/libnordstjernen.so
  meson-cross/
    android-arm64.txt
    android-armv7.txt
    android-x86_64.txt
  build-android.sh                  # one script to rule them all
  gradle/                           # gradle wrapper
  build.gradle.kts
  settings.gradle.kts
```

The C build remains meson. Gradle's only job is wrapping the `.so`
files plus a thin Kotlin shell into an `.aab`.

### Native build

Three NDK cross-files driving meson, one per ABI. minSdk 24, NDK
r27 (the current LTS in 2026). Output goes to
`android/app/src/main/jniLibs/<abi>/libnordstjernen.so`.

Dependency builds (libcurl, lexbor, quickjs-ng, libvpx, glib, gtk4,
pango, cairo, harfbuzz, freetype, fontconfig, gdk-pixbuf) live in
`subprojects/` as meson wraps or as a prebuilt-archive bootstrap.
Realistic: maintain a small `build-android.sh` that builds each
dep once into `build-android/sysroot-<abi>/` and points the
cross-file at it. Don't try to build deps from gradle.

### Kotlin shell

Minimum viable shell, ~150 lines:

```kotlin
class NordstjernenActivity : NativeActivity() {
    companion object { init { System.loadLibrary("nordstjernen") } }
    external fun ndOnLicenseGranted(token: String)
    private lateinit var billing: Billing
    override fun onCreate(s: Bundle?) {
        super.onCreate(s)
        billing = Billing(this) { token -> ndOnLicenseGranted(token) }
    }
}
```

`Billing.kt` wraps `com.android.billingclient:billing:6.x`, queries
the user's purchases on launch, presents the buy flow on demand,
and notifies the C side via JNI when Plus is owned.

### Engine changes required

The list below assumes Path A from the previous porting
discussion — GTK 4 Android backend, engine almost unchanged.

1. `src/paths_android.c` (~80 LOC) — JNI shim for
   `Context.getFilesDir()` / `getCacheDir()` replacing
   `g_get_user_config_dir()` and friends. Conditionalized via
   `#ifdef ANDROID`.
2. `src/lifecycle_android.c` (~150 LOC) — `onPause`/`onResume` /
   `onTrimMemory` hooks. Aggressive state-flush on pause:
   bookmarks, HSTS, cookies, history.
3. `src/input_android.c` (~200 LOC) — touch + soft keyboard glue,
   IME bridge.
4. `src/license_android.c` (~120 LOC) — receives the entitlement
   token from `Billing.kt`, verifies signature on cached
   server-issued JWTs, gates Plus features behind
   `nd_license_has_plus()`.
5. Default UA on Android builds becomes phone-like (already covered
   in the porting notes).
6. Viewport default in `config.c` drops from 1000 to 412 on Android
   builds (the standard Android phone width in dp).

All of the above is ~600 LOC of new code, gated by an `is_android`
meson option.

### Gradle build

```kotlin
android {
    namespace = "com.nordstjernen.browser"
    compileSdk = 35
    defaultConfig {
        applicationId = "com.nordstjernen.browser"
        minSdk = 24
        targetSdk = 35
        versionCode = gitCommitCount.toInt()
        versionName = "0.1.0"
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64") }
    }
    signingConfigs { create("release") { /* upload key */ } }
    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            signingConfig = signingConfigs.getByName("release")
        }
    }
}
dependencies {
    implementation("com.android.billingclient:billing-ktx:6.2.0")
}
```

The AAB size target is **under 30 MB per ABI split** (which is
realistic — GTK + Cairo + Pango weigh ~20 MB compressed).

## Submission process

1. **First-time setup in Play Console** — store listing, screenshots
   (you need 2–8, at least one phone, one tablet), feature graphic
   (1024×500), short description (80 char), long description (4000
   char). Allow 1–2 days for writing.
2. **Internal testing track** — upload your first AAB here. 1–100
   testers by email. Reviews take **minutes**, not hours. Use this
   for the first month.
3. **Closed testing track** — invite ~50 real users. Required
   minimum 14 days before you can promote to production for new
   apps in the Play Store as of late 2023. Plan for this delay.
4. **Production release** — first review can take **3–7 days** for
   a new browser app; Google reviews browsers more carefully than
   most categories. Subsequent updates are typically 1–24 hours.
5. **Staged rollout** — 10% → 25% → 50% → 100% over 2–4 days.
   Watch the Play Console crash dashboard.

## Updates and versioning

- `versionCode` is monotonic int (use commit count or unix days).
- `versionName` is the human "0.1.4" string.
- Ship at most **every two weeks** during stable life; more
  frequent updates trigger Play's "spammy" heuristic.
- Critical security fix? Use the Play Console "halt rollout +
  emergency rollback" flow, then re-submit the fix on a fast
  rollout (24h to 100%).

## CI

Add a new GitHub Actions job `android.yml` running on the same
daily schedule as the existing platform jobs. It must:

- Build all three ABIs into an AAB.
- Run the existing `--headless --dump=text` checks against
  `reading-list.txt` on the *host* (not on emulator) since the
  engine is the same code.
- Optionally smoke-test in an emulator (slow, flaky, skip for v1).
- Upload the AAB as a workflow artifact for manual download to
  the internal testing track.

Do **not** wire automatic Play Console uploads via service account
in v1. Manual upload via web UI keeps the release blast radius
small.

## Costs

| Item | Cost |
|---|---|
| Play Console developer account | $25 one-time |
| Merchant account | $0 (Google's revenue share covers it) |
| D-U-N-S number for company verification | $0 (free if you go through Dun & Bradstreet directly) |
| Signing key HSM (optional, recommended) | $50–$80 one-time (YubiKey 5) |
| Translation services for ≥5 store listings (Spanish, German, French, Japanese, Portuguese) | $200–$600 one-time |
| Beta-tester recruitment (~50 people for closed testing) | $0 if Mastodon/HN works; ~$100 in Reddit ads if not |

Total to first production release: **~$300–$700** out of pocket.

## Timeline (calendar weeks, solo developer)

| Phase | Weeks |
|---|---|
| NDK toolchain + dependency cross-builds | 2 |
| Engine port (paths, lifecycle, input, license) | 3 |
| Kotlin shell + Play Billing wiring | 1 |
| Touch UX polish (tap targets, soft keyboard, zoom, kinetic scroll) | 2 |
| Store listing assets (screenshots, copy, translations) | 1 |
| Internal + closed testing + Play review buffer | 3 |
| **Total to production** | **12 weeks** |

If you have not shipped Android before, add **4 weeks** of toolchain
learning. Realistic worst case: **16 weeks**.

## Risks to watch

- **Play Store browser review tightening.** Google has periodically
  delisted small browsers en masse (2018, 2021, 2023). Mitigation:
  have the F-Droid build ready as a fallback channel, and never
  let your only distribution channel be Play.
- **Play Billing changes.** Google changed billing terms in 2022,
  2023, and (rumored) 2026 again. The IAP code must be isolated
  enough to swap behind an interface.
- **Upload key compromise.** Defense in depth: hardware-backed key
  on YubiKey, second copy in offline cold storage.
- **A CVE in libcurl/lexbor/quickjs/libvpx.** Android updates are
  fast (1-day Play review for fixes), but you must be able to ship
  a patch within 72h. Pin dependency versions; subscribe to
  oss-security; have a rehearsed "emergency release" runbook
  before you ship.
- **DMCA / takedown bots scraping your Play page.** Trademarks
  (don't use "Star" anything, don't use blue compass icons, etc.).
- **Google account suspension.** Single point of failure. Read the
  Play Developer Distribution Agreement, follow it, keep evidence
  (purchase records, signing logs) — if your account is suspended
  without notice (it happens) you'll need it to appeal.

## What this document does not cover

- **F-Droid release** — separate track, easier (no Play Billing, no
  data-safety form, no API-level pressure, reproducible-build
  requirement is the only twist). Plan that separately.
- **Samsung Galaxy Store, Huawei AppGallery, Amazon Appstore** —
  consider in year two only if Play data warrants. Each is a
  6–8 week port of the store-listing and a different billing SDK.
- **iOS** — different document. Short version: Apple's policy on
  third-party browser engines on iOS is changing as of 2024–2026
  in the EU only; nowhere else. The cost-benefit on iOS is much
  worse than Android.
