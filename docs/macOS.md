# Nordstjernen on macOS

Nordstjernen runs natively on macOS through GTK 4's Quartz backend — no
X11, no XQuartz, no WebKit. The same C engine shipped to Linux
and Windows links the same GTK 4 / libcurl / Cairo / Pango / lexbor
libraries here; there is no Xcode project and no CocoaPods. This page
covers both **installing the prebuilt app** and **building from source**.

- **Prebuilt download:** Apple Silicon (`arm64`) only — a Mac with an
  M1 chip or newer.
- **macOS version:** built on macOS 26 (Tahoe); the bundle targets
  macOS 11 (Big Sur) and later.
- **Intel Macs:** not in the prebuilt release — [build from
  source](#build-from-source), which still compiles on an `x86_64`
  Homebrew prefix.

## Install the app

1. Download
   [`nordstjernen-macos.dmg`](https://www.nordstjernen.org/nightly/nordstjernen-macos.dmg).
2. Open the `.dmg` and drag **Nordstjernen** into `/Applications`.
3. Launch it from Launchpad, Spotlight, or Finder.

The released `.dmg` is signed with an Apple **Developer ID** and
**notarised**, so it opens straight away with no Gatekeeper prompt and no
`xattr` step. (If you build your own `.dmg` without a Developer ID it is
signed ad-hoc instead — see [Opening an ad-hoc build](#opening-an-ad-hoc-build).)

### Opening an ad-hoc build

The published `.dmg` is notarised and needs none of this. It only applies
to a `.dmg` you built yourself **without** a Developer ID (`pack-macos.sh`
falls back to an **ad-hoc** signature then). macOS stamps anything
downloaded with a `com.apple.quarantine` flag, and Gatekeeper refuses to
open a quarantined app that isn't notarised — the first launch fails with
*"Nordstjernen is damaged and can't be opened"* or *"cannot be opened
because Apple cannot check it for malicious software."* This is expected
for an ad-hoc build; the download is not corrupt.

Clear the quarantine flag once, after copying the app to `/Applications`:

```sh
xattr -dr com.apple.quarantine /Applications/Nordstjernen.app
```

The app then launches normally on every later run. (Right-click → **Open**
also works on some macOS versions, but the `xattr` command is the reliable
path — recent macOS has narrowed the right-click bypass for un-notarised
apps.) Maintainers producing the release build avoid this entirely by
signing and notarising — see the **Code signing** note under
[Platform notes](#platform-notes).

### Verifying what you downloaded

```sh
codesign -dv --verbose=2 /Applications/Nordstjernen.app   # shows the Developer ID signature
spctl -a -vv /Applications/Nordstjernen.app               # Gatekeeper's assessment
shasum -a 256 ~/Downloads/nordstjernen-macos.dmg          # compare against SHA256SUMS
```

For the published build, `spctl` reports *"accepted"* /
*"source=Notarized Developer ID"* and `codesign` shows the authority
*"Developer ID Application: … (49X98YTK33)"*. A self-built ad-hoc `.dmg`
reports *"rejected"* / *"Unnotarized Developer ID"* instead — that is the
state the `xattr` step above works around. Published checksums are at
<https://www.nordstjernen.org/nightly/SHA256SUMS>.

### Uninstall

```sh
rm -rf /Applications/Nordstjernen.app
rm -rf ~/.config/nordstjernen ~/.cache/nordstjernen ~/.local/share/nordstjernen
```

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| *"…is damaged and can't be opened"* / *"…cannot be opened because Apple cannot check it"* | Download quarantine on a self-built **ad-hoc** `.dmg` (the published build is notarised and unaffected) | `xattr -dr com.apple.quarantine /Applications/Nordstjernen.app` (see [above](#opening-an-ad-hoc-build)) |
| App opens but **no web page loads** — every `https://` site fails | An old build with no bundled CA store, run on a Mac without Homebrew | Update to a current build (the `.dmg` now vendors a CA bundle); or set `CURL_CA_BUNDLE=/path/to/cert.pem` before launch |
| `<video>` / `<audio>` plays but is **silent** | An old build that did not bundle the audio helper | Update to a current build (it ships `nordstjernen-audio` inside the `.app`) |
| Quits immediately with status **77** from a terminal | Refuse-root check — launched via `sudo` | Run as a normal user, or set `NS_ALLOW_ROOT=1` |
| Blank window or GPU glitches | Quartz GL renderer trouble | Relaunch with `GSK_RENDERER=cairo` to force the software renderer |
| Need to see why it won't start | — | Launch from Terminal and read stderr: `/Applications/Nordstjernen.app/Contents/MacOS/Nordstjernen` |

## Build from source

The macOS build uses **Homebrew** for the toolchain and dependencies —
the same packages the CI workflow
([`.github/workflows/macos.yml`](../.github/workflows/macos.yml))
installs, which is the authoritative spec. The meson build produces a
single Mach-O executable; `scripts/pack-macos.sh` wraps it into a `.app`
bundle and `.dmg`.

### One-time setup

Install [Homebrew](https://brew.sh) if you do not already have it, then:

```sh
brew install meson ninja pkg-config cmake gtk4 libepoxy curl \
    uchardet librsvg libpsl sqlite webp sdl2
brew install ccache    # optional, speeds up rebuilds
```

Homebrew installs to `/opt/homebrew` on Apple Silicon and `/usr/local` on
Intel; both are wired through `pkg-config` automatically, so no extra
environment variables are needed for a normal user install.

Optional extras, all auto-detected — the build works without them:

- `brew install enchant` plus a dictionary enables on-screen
  spell-checking of editable text.
- `brew install ffmpeg` enables inline WebM (VP9/VP8 + Opus/Vorbis).
  Stock Homebrew FFmpeg is GPL, so for a *redistributable* `.app` CI
  instead builds a minimal LGPL FFmpeg with
  `scripts/build-ffmpeg-lgpl.sh`; a local developer build can use the
  Homebrew one directly.

### Build

```sh
git clone https://github.com/nordstjernen-web/nordstjernen
cd nordstjernen
meson setup builddir
meson compile -C builddir
```

QuickJS, lexbor, WAMR, Wuffs, pl_mpeg and minimp3 are vendored in-tree,
so `meson setup` only auto-downloads the Wuffs image-decoder subproject
(and llama.cpp for the local-AI feature, unless you pass
`-Dai=disabled`). No git submodules.

To match CI exactly — including the local-AI feature being left out for
speed:

```sh
export CC="ccache clang"
meson setup builddir --werror -Dai=disabled
meson compile -C builddir
```

### Run

```sh
./builddir/src/gtk/nordstjernen https://example.com
```

The first launch creates per-user state under `~/.config/nordstjernen/`,
`~/.cache/nordstjernen/`, and `~/.local/share/nordstjernen/`. GLib does
**not** translate XDG base directories to the macOS-native `~/Library/...`
paths — every GTK app on macOS follows the same Unix convention. Set
`XDG_CONFIG_HOME`, `XDG_CACHE_HOME`, or `XDG_DATA_HOME` to relocate.

Headless rendering works the same as on Linux:

```sh
./builddir/src/gtk/nordstjernen --headless --dump=text https://example.com
./builddir/src/gtk/nordstjernen --headless --dump=png:/tmp/page.png https://example.com
```

### Package the `.app` and `.dmg`

```sh
./scripts/pack-macos.sh                                          # ad-hoc signed
MACOS_SIGN_IDENTITY="Developer ID Application: …" ./scripts/pack-macos.sh
# Signed, notarised and stapled in one shot (stored notarytool profile):
MACOS_SIGN_IDENTITY="Developer ID Application: NAME (TEAMID)" \
    MACOS_NOTARY_PROFILE="nordstjernen-notary" ./scripts/pack-macos.sh
```

The script stages `dist/Nordstjernen.app`, vendors the Homebrew dylibs
with `dylibbundler`, code-signs the bundle, and produces
`dist/nordstjernen-<version>-macos-<arch>.dmg`. When `MACOS_NOTARY_PROFILE`
names a stored notarytool credential profile (see below) and a real
`MACOS_SIGN_IDENTITY` is set, it also submits the `.dmg` to Apple's notary
service, staples the ticket, and runs a `spctl` acceptance check — so a single
invocation produces a ready-to-publish, notarised `.dmg`. Without the profile it
stops after signing and prints the manual `notarytool`/`stapler` commands. What
goes into the bundle and how it is signed is described under
[Platform notes](#platform-notes).

## Distribution

Two channels, with very different feasibility for a multi-process browser:

| | Mac App Store | Developer ID + notarisation (direct `.dmg`) |
|---|---|---|
| Apple review | Full App Review | Automated notarisation only |
| App Sandbox | Mandatory | Not required |
| Fits this architecture | No — see blockers below | Yes — already wired |
| User experience | Install from store | Download `.dmg`, no quarantine prompt |

No major independent browser (Chrome, Firefox, Brave) ships through the
Mac App Store; they all use Developer ID + notarisation, because the App
Sandbox fights a multi-process engine. Nordstjernen is in the same spot,
so the notarised `.dmg` is the realistic target.

### Developer ID + notarisation (recommended)

This makes a *downloaded* `.dmg` open with no `xattr` step and no
re-architecture. It needs a paid Apple Developer account ($99/yr).

1. Create a **Developer ID Application** certificate and install it in your
   login keychain (or export it as a `.p12` for CI). `security find-identity
   -v -p codesigning` should then list `Developer ID Application: NAME
   (TEAMID)` — that whole string is `MACOS_SIGN_IDENTITY`.
2. Store notarytool credentials once so the build never puts secrets on the
   command line (uses an app-specific password minted at
   <https://account.apple.com> → Sign-In and Security → App-Specific
   Passwords):
   ```sh
   xcrun notarytool store-credentials nordstjernen-notary \
       --apple-id you@example.com --team-id TEAMID \
       --password <app-specific-password>
   ```
3. Build signed **and** notarised in one command — `pack-macos.sh` submits the
   `.dmg`, waits, staples the ticket, and runs the `spctl` check:
   ```sh
   MACOS_SIGN_IDENTITY="Developer ID Application: NAME (TEAMID)" \
       MACOS_NOTARY_PROFILE="nordstjernen-notary" \
       ./scripts/pack-macos.sh
   ```
   To keep the two steps separate instead, omit `MACOS_NOTARY_PROFILE` and run
   the notarisation by hand on the produced `$DMG`:
   ```sh
   xcrun notarytool submit "$DMG" --keychain-profile nordstjernen-notary --wait
   xcrun stapler staple "$DMG"
   ```
4. Verify: `spctl -a -vv -t open "$DMG"` reports *accepted — Notarized
   Developer ID*.

**In CI**, `.github/workflows/macos.yml` performs steps 2–4 automatically
when these repository secrets are present — which they are for this
project, so the published `.dmg` is signed and notarised. Without them the
workflow falls back to an ad-hoc `.dmg`:

| Secret | Meaning |
|--------|---------|
| `MACOS_CERTIFICATE_P12_BASE64` | `base64` of the Developer ID Application `.p12` |
| `MACOS_CERTIFICATE_PASSWORD` | password for that `.p12` |
| `MACOS_SIGN_IDENTITY` | `Developer ID Application: NAME (TEAMID)` |
| `MACOS_NOTARY_APPLE_ID` | Apple ID e-mail used for notarisation |
| `MACOS_NOTARY_TEAM_ID` | the 10-character team ID |
| `MACOS_NOTARY_PASSWORD` | an app-specific password for that Apple ID |

Encode the certificate with `base64 -i DeveloperID.p12 | pbcopy`, and
mint the app-specific password at <https://account.apple.com> → Sign-In
and Security → App-Specific Passwords.

### Mac App Store

Allowed in principle — the WebKit-only rule is iOS App Review §2.5.6 and
does **not** apply to macOS — but three things must change first, two of
them large:

1. **App Sandbox.** The store requires the
   `com.apple.security.app-sandbox` entitlement, and a sandboxed app may
   **not** `fork()`/`execv()` a sibling executable. Nordstjernen spawns a
   `nordstjernen-renderer` per tab (and a `nordstjernen-audio` helper)
   exactly that way (`src/rproc_http.c`) for OS-level tab isolation. Those
   helpers would have to be re-built as **XPC services**
   (`Contents/XPCServices/*.xpc`), or the app would ship
   `--single-process` and forfeit the per-tab security boundary. This is
   real engineering, not configuration.
2. **Hardened Runtime + JIT.** WAMR (WebAssembly) maps executable memory,
   so `com.apple.security.cs.allow-jit` is required (already in
   `packaging/macos/entitlements.plist`); QuickJS is interpreter-only and
   needs nothing extra. A browser also needs
   `com.apple.security.network.client`.
3. **Licensing.** The bundle ships **LGPL** GTK 4 / GLib, and Apple's
   App Store terms are widely read as incompatible with the GPL family
   (LGPL is more arguable when dynamically linked and relinkable — a legal
   question to clear). Confirm too that **NSL-1.0** permits store
   distribution; the store does let you supply your own EULA.

Once those are resolved, the submission is the standard flow: enroll in
the Apple Developer Program; create the app record in **App Store
Connect** (bundle id `org.nordstjernen.Nordstjernen`, category
`public.app-category.utilities` — both already in the generated
`Info.plist`); sign the `.app` with an **Apple Distribution** certificate
and a Mac App Store provisioning profile (`Contents/embedded.provisionprofile`);
wrap it with `productbuild --sign "3rd Party Mac Developer Installer: …"`;
upload the `.pkg` with the **Transporter** app; then fill in metadata and
the privacy labels (Nordstjernen collects nothing) and submit for review.
Updates ship only through the store — no self-update.

## Platform notes

- **CA bundle.** Homebrew's `libcurl` links against OpenSSL, which
  has no built-in trust store and — unlike the deprecated SecureTransport
  backend — no bridge to the macOS Keychain. For a *developer* build
  Nordstjernen probes the standard Homebrew and system paths at startup
  (`/opt/homebrew/etc/ca-certificates/cert.pem`,
  `/usr/local/etc/ca-certificates/cert.pem`, `/etc/ssl/cert.pem`,
  and a handful of `openssl@3` variants) and points libcurl at
  whichever exists. A clean Mac has none of these (no Homebrew, and
  Apple ships no `/etc/ssl/cert.pem`), so `scripts/pack-macos.sh`
  **vendors a CA bundle** into the `.app` at
  `Contents/Resources/etc/ssl/certs/ca-bundle.crt` and
  `ns_macos_anchor_gtk_data()` exports `CURL_CA_BUNDLE` / `SSL_CERT_FILE`
  to it — the spawned renderer inherits those — so HTTPS verifies on a
  machine with no Homebrew. Override with `SSL_CERT_FILE` or
  `CURL_CA_BUNDLE` to ship your own bundle.
- **Self-path resolution.** The new-window action re-execs the
  current binary; on macOS the binary path is resolved with
  `_NSGetExecutablePath(3)` and canonicalised with `realpath(3)`.
- **Sandbox.** Landlock and seccomp are Linux-only, but macOS gets a
  **Seatbelt** sandbox (`sandbox_init`) that write-confines both the shell
  and the renderer to the per-user nordstjernen config/data/cache dirs,
  Downloads, and the system temp roots — the same write set the Linux
  Landlock layer allows. It is a filesystem-integrity boundary only (no
  syscall/network confinement) and fails open. Disable with
  `NS_NO_SANDBOX=1`. See [`SECURITY.md`](../SECURITY.md#macos-sandbox). The
  refuse-root check also applies — `nordstjernen` exits with status 77 if
  launched via `sudo` unless `NS_ALLOW_ROOT=1` is set.
- **Keyboard shortcuts.** Every accelerator uses GTK's `<Primary>`
  modifier, which maps to ⌘ on macOS. So `⌘L` focuses the URL bar,
  `⌘T` opens a new tab and `⌘N` a new window, `⌘R` reloads, `⌘W`
  closes the window, `⌘F` opens find-in-page, `⌘+` / `⌘-` / `⌘0` zoom, `⌘P`
  prints, `⌘⇧J` opens the JS console. Toolbar tooltips render the
  Mac glyphs (`⌘N`, `⌘⇧J`) instead of the Linux/Windows `Ctrl+...`
  strings.
- **Display server.** GTK 4 on macOS uses the native Quartz backend.
  There is no X11 or Wayland requirement, and no XQuartz dependency.
  If the GPU (`ngl`) renderer misbehaves, `GSK_RENDERER=cairo` forces the
  software path.
- **Packaging.** The meson output is a plain Mach-O binary in
  `builddir/src/gtk/nordstjernen` that launches from Terminal or Finder
  and shows up in the Dock while it runs. For distribution,
  `scripts/pack-macos.sh` stages a `.app` bundle (with a generated
  `Info.plist`) and produces a `.dmg`. The bundle executable is the real
  Mach-O (`Contents/MacOS/Nordstjernen`), not a wrapper script —
  `dylibbundler` rewrites every dependency to
  `@executable_path/../Frameworks`, so no `DYLD_LIBRARY_PATH` shim is
  needed and the whole `.app` can be code-signed. The renderer
  (`nordstjernen-renderer`), the audio helper (`nordstjernen-audio`,
  present whenever SDL2 was found at build time) and the MSE video-decode
  helper (`nordstjernen-video`, present whenever libav/FFmpeg was found)
  ship beside it and are run through `dylibbundler` (and the same
  rpath-dedup and inside-out code-signing) too — without `nordstjernen-video`
  in the bundle the shell can't spawn it, so MSE video would fall back to
  in-process decoding in the renderer. Beyond the dylibs, the bundle carries
  the data GTK 4 reads at runtime so the `.dmg` works on a Mac with no
  Homebrew: the app's own SVG toolbar icons under
  `Contents/Resources/share/icons`, the GdkPixbuf loader modules plus a
  `loaders.cache` (the SVG loader is what renders those icons), the
  compiled GSettings schemas, and the vendored CA bundle. At startup
  `ns_macos_anchor_gtk_data()` (in `src/gtk/appmain.c`) points
  `GSETTINGS_SCHEMA_DIR`, `GDK_PIXBUF_MODULE_FILE`,
  `GDK_PIXBUF_MODULEDIR`, and `CURL_CA_BUNDLE` at those bundled copies
  when it detects it is running from inside an `.app`.
- **Architecture.** CI builds an Apple Silicon (`arm64`) `.dmg` on the
  `macos-26` (Tahoe) runner; that is the only published build, matching the
  modern macOS versions Nordstjernen targets. An Intel Mac is not covered by CI —
  the engine still compiles there, so build from source (the steps above
  work unchanged on an `x86_64` Homebrew prefix).
- **Code signing.** `pack-macos.sh` signs the bundle inside-out (nested
  dylibs and helpers first, the `.app` last). With no Apple Developer ID
  it signs **ad-hoc** (`codesign --sign -`), which seals the bundle so it
  launches once the download quarantine is cleared (see [Opening an
  ad-hoc build](#opening-an-ad-hoc-build)). Set `MACOS_SIGN_IDENTITY`
  to a `Developer ID Application: …` identity to produce a
  hardened-runtime, notarisation-ready bundle instead; the JIT
  entitlements in `packaging/macos/entitlements.plist` are applied on that
  path. Notarising still needs a maintainer's credentials: set
  `MACOS_NOTARY_PROFILE` to a stored `notarytool` profile and the script
  submits the `.dmg`, waits, and staples the ticket itself; without it the
  build stops after signing and prints the manual
  `xcrun notarytool submit "$DMG" --keychain-profile … --wait` then
  `xcrun stapler staple "$DMG"` commands.

## Maintaining the macOS port from Linux / Windows

Most development happens on Linux and Windows, and macOS only gets exercised
when CI runs or someone builds the `.dmg`. The platform differs in ways that a
Linux/Windows change can silently break, so this section collects the failure
modes that have actually bitten the macOS build and how to stay ahead of them.

### macOS links a *different* curl / TLS stack than you expect

Homebrew's `curl` is **keg-only**, so a plain `meson setup` does **not** pick it
up — `pkg-config` resolves the macOS SDK's **system** `libcurl`
(`/usr/lib/libcurl.4.dylib`), which is **LibreSSL / SecureTransport**, not the
OpenSSL build the app is designed around (vendored CA bundle, HTTP/3, modern TLS
options). Put the keg on the path before configuring:

```sh
PKG_CONFIG_PATH="$(brew --prefix curl)/lib/pkgconfig:$PKG_CONFIG_PATH" \
    meson setup builddir
otool -L builddir/src/gtk/nordstjernen | grep curl   # expect .../opt/curl/...
```

`scripts/pack-macos.sh` and `.github/workflows/macos.yml` already do this; a
manual `meson setup builddir` per the build instructions above does not, so a
local dev binary links system curl unless you add the keg yourself.

**The wider lesson — any curl/TLS option must degrade on the oldest backend
macOS might link.** `CURLOPT_SSL_EC_CURVES`, cipher lists, and similar options
are rejected **wholesale** if the linked backend doesn't recognise *one* entry.
A post-quantum curve (`X25519MLKEM768`) hard-pinned in `src/net.c` once broke
**every HTTPS connection** on a Mac whose curl was LibreSSL — it failed the
whole curve list, not just that curve. The code now probes the backend
(`curl_version_info`) and only requests features the active backend supports.
When you touch `src/net.c` TLS setup, assume the runtime curl may be older or a
different backend than your Linux box, and gate accordingly.

### Test the *bundled* binary, not just the build-tree one

CI's macOS smoke test runs `--headless --dump=text about:start` on the
**builddir** binary. That page is local, so it proves neither **networking**
(no HTTPS is fetched) nor anything **bundle-specific**. Two whole classes of bug
sail past it:

- HTTPS regressions (the curve bug above) — `about:start` never hits the
  network.
- Bundle-only failures — the build-tree binary uses Homebrew rpaths and runs
  fine, while the relocated `.app` does not.

So after any change to networking, the bundle layout, dylib dependencies, or
rpaths, **build the `.dmg` and launch the bundled binary against a real URL**:

```sh
PKG_CONFIG_PATH="$(brew --prefix curl)/lib/pkgconfig:$PKG_CONFIG_PATH" \
    BUILDDIR="$PWD/builddir" NS_PACK_AI=disabled ./scripts/pack-macos.sh
dist/Nordstjernen.app/Contents/MacOS/Nordstjernen \
    --headless --dump=text https://example.com    # must print page text
```

(Reusing an existing `BUILDDIR` skips the from-scratch compile; `dylibbundler`
still takes a few minutes.) Worth adding to CI as a real gate.

### `dylibbundler` + meson build-rpaths → duplicate `LC_RPATH` (fatal)

The build binary carries one `LC_RPATH` per Homebrew dependency directory, and
`dylibbundler` adds a `@executable_path/../Frameworks/` rpath for each of them —
leaving many identical entries. **Modern macOS dyld treats a duplicate
`LC_RPATH` as a fatal error**, so the `.app` fails to launch with
`dyld: duplicate LC_RPATH`. `pack-macos.sh` now collapses them to a single
entry after bundling; if you rework the packaging, keep that step (and re-sign
*after* it — `install_name_tool` invalidates the signature).

### Other macOS-only behaviour to keep in mind

- **The Dock icon needs the `.app` or the programmatic setter.** GTK's Quartz
  backend does **not** map `gtk_window_set_default_icon_name()` to the Dock
  tile. The `.app` carries an `.icns`; an unbundled binary additionally renders
  the embedded SVG and calls `[NSApp setApplicationIconImage:]`
  (`src/gtk/macos_dock.m`). The icon is otherwise generic.
- **AI (llama.cpp) is left out of macOS builds** (`-Dai=disabled` in CI and the
  pack step), so `about:start` uses the **non-AI** start page on macOS
  (`#if defined(NS_HAVE_AI) && !defined(__APPLE__)` in `src/net.c`). Don't wire
  the start-page chat to assume AI on macOS.
- **Sandbox is a Seatbelt write-confinement** (`sandbox_init`,
  `security.c` `__APPLE__` arm), not Landlock+seccomp — filesystem writes
  only, no syscall/network filter, fails open. If a new feature needs to
  write outside the nordstjernen config/data/cache dirs or the system temp
  roots, extend the SBPL profile (or call `ns_security_add_writable_dir`)
  or it will be silently denied on macOS. The refuse-root check also applies
  (exit 77 unless `NS_ALLOW_ROOT=1`).
- **`__APPLE__` is the platform guard** used across the tree (see `src/media.c`,
  `src/security.c`, `src/ext.c`) — match it for new macOS-specific code rather
  than introducing a new macro.
- **Objective-C** is added as a meson language only on darwin
  (`add_languages('objc')` in `src/gtk/meson.build`), guarded so Linux/Windows
  builds never see it; macOS-only Cocoa code (`*.m`) goes there with the
  relevant `-framework` link arg.

### Quick triage when "it builds on Linux but macOS CI / the `.dmg` is broken"

| Symptom | Likely cause | Where |
|---------|--------------|-------|
| Every HTTPS page fails | a TLS/curl option the linked backend rejects | `src/net.c` `ns_net_apply_curl_tls` |
| `.dmg` app won't launch (`duplicate LC_RPATH`) | dylibbundler rpath duplication | `scripts/pack-macos.sh` |
| App launches but no page loads on a clean Mac | missing/!vendored CA bundle, or system-curl SecureTransport ignoring `CAINFO` | `pack-macos.sh` CA step + curl linkage |
| Generic Dock icon | running the unbundled binary, or the SVG/`.icns` path broke | `src/gtk/macos_dock.m`, `pack-macos.sh` icon step |
| `glib`/`gtk4` schema or pixbuf-loader abort at runtime | bundle missing compiled schemas / `loaders.cache` | `pack-macos.sh` schema + pixbuf steps |

## Definition of done on macOS

Same as everywhere else (`CLAUDE.md`):

1. `meson compile -C builddir` finishes without new warnings under
   the configured Clang flags.
2. The browser launches and the affected UI path works manually.
3. The change is committed and pushed to `origin/main`.

The macOS CI workflow runs on every push and pull request to `main`
plus manual `workflow_dispatch`, and exists to catch regressions that
the local Linux build misses (Apple Silicon ABI, BSD libc, GTK 4
Quartz backend).

## iPhone / iPad — not yet, and what would it take

There is no iOS build today. The blockers are real, not speed-of-light:

1. **No GTK 4 on iOS.** GTK 4 has no UIKit backend; the Quartz
   backend on macOS uses the same desktop AppKit APIs (`NSWindow`,
   `NSEvent`) that iOS does not expose. The window/toolbar/keyboard
   layer in `src/gtk/procwindow.c` / `src/gtk/procview.c` and the entry
   point in `src/gtk/appmain.c` would have to be replaced wholesale by a
   UIKit / SwiftUI shell.
2. **App Review until very recently required WebKit.** Apple's
   App Store Review Guidelines §2.5.6 historically forced every
   browser-like app to render web content through WebKit's
   `WKWebView`. The EU's Digital Markets Act has cracked this open
   for users in the EU as of 2024, but the global rule still
   stands and the entitlements / notarisation paperwork for an
   alt-engine browser is non-trivial.
3. **No `fork`/`exec` for per-tab renderers.** iOS apps run a single
   process. The per-tab renderer spawn (`ns_rproc_http_spawn`) that
   re-execs the binary for OS-level isolation simply cannot exist;
   isolation would need to be inside one process, or shelved.
4. **Cairo and Pango are not first-class on iOS.** They build,
   but no one ships them in App Store apps; the native path is
   CoreGraphics + CoreText, which means re-targeting the paint
   layer.

A realistic incremental path, if it ever gets prioritised:

- Keep the parser / CSS / layout / paint engine portable C
  (already true today). Compile it as a static library for the
  `arm64-apple-ios` triple.
- Wrap the rendering output in a thin UIKit (or SwiftUI) shell —
  `UIScrollView` containing a single `CALayer`-backed view that
  the engine paints into via Cairo's image surface, copied to a
  `CGImage`.
- Networking via `libcurl` continues to work on iOS as long as it
  is linked statically; `_NSGetExecutablePath` and CA bundle
  discovery already do the right thing.
- Skip Landlock, skip the refuse-root check (iOS apps are not
  root), skip the JS console UI for v1.

None of the above is on the roadmap; this section exists so the
next person who asks "could Nordstjernen run on iPhone?" has the
short answer in one place.
