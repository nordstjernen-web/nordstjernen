# Nordstjernen on Ubuntu — the Snap Store package

This document records how Nordstjernen is packaged as a **strictly
confined snap** for the Ubuntu Snap Store (snapcraft.io). The recipe
lives in `snap/snapcraft.yaml` at the repository root; everything below
explains how to build it, why it is configured the way it is, and how to
publish it.

For the plain Debian/Ubuntu source build (no snap), see `Linux.md`. The
snap reuses the same meson build — it just wraps it in the snap
toolchain, bundles the runtime libraries, and runs under snap
confinement.

## Why a snap

Snaps give Nordstjernen a single, distro-agnostic binary that runs on
any Ubuntu release (and most other Linux distributions) with automatic
updates from the store and no dependency hunting. The snap ships its own
copies of the non-platform libraries (libcurl, OpenSSL, uchardet, libpsl,
SQLite, libseccomp, libwebp, SDL2, Enchant) and pulls GTK 4, Pango, Cairo, and
librsvg from the shared **GNOME platform snap** via the `gnome`
extension, so the package stays small.

## Prerequisites

Install `snapcraft` and a build backend (LXD is the recommended,
rootless backend):

    sudo snap install snapcraft --classic
    sudo snap install lxd
    sudo lxd init --auto
    sudo usermod -aG lxd "$USER"      # log out/in once after this

`snapcraft` builds in a clean, isolated LXD container, so the host does
not need any of Nordstjernen's `-dev` packages — only `snapcraft` and
LXD.

## Build

From a clean checkout:

    snapcraft

This reads `snap/snapcraft.yaml`, spins up a `core24` (Ubuntu 24.04)
build container, compiles the engine with meson/ninja, stages the
runtime libraries, and emits a single artefact:

    nordstjernen_<version>_amd64.snap

Install and run the locally built snap to smoke-test it before
publishing:

    sudo snap install --dangerous nordstjernen_*.snap
    nordstjernen

`--dangerous` is required because a locally built snap is unsigned. To
exercise it under the store's confinement before upload, connect the
interfaces by hand (the store does this automatically on install for
auto-connected plugs; see *Interfaces* below):

    snap connections nordstjernen

Iterate faster with:

    snapcraft --debug          # drop into the build container on failure
    snapcraft clean nordstjernen   # rebuild just the engine part

`arm64` builds run the same way on an `arm64` host, or remotely with
`snapcraft remote-build`.

## How the recipe is put together

`snap/snapcraft.yaml` has three moving parts.

### Metadata via `adopt-info`

The snap name, summary, description, and license are declared
statically, but the **version** is adopted from `meson.build` at build
time (`override-pull` parses the `version:` field and calls
`craftctl set version`). Bumping the project version in `meson.build` is
therefore enough — the snap version follows automatically.

### The `nordstjernen` part

A `plugin: meson` part that configures the release distro build with the
project's default feature set.

`--prefix=/usr` makes meson install the three executables that the
browser ships —

  * `nordstjernen`          — the GTK 4 shell
  * `nordstjernen-renderer` — the sandboxed per-tab engine process
  * `nordstjernen-audio`    — the MP2 audio helper

— into `usr/bin` alongside the data, icons, `.desktop` file, and
AppStream metainfo. The shell locates the two helper binaries relative
to its own executable (`/proc/self/exe`), so they resolve correctly
inside the snap's `$SNAP/usr/bin` with no path patching. (The
`NS_RENDERER` environment variable can still override the renderer path
if needed.)

`build-packages` are the `-dev` packages from `Linux.md`;
`stage-packages` are the matching runtime shared libraries that the
GNOME platform snap does **not** provide, so they get bundled into the
snap.

### Confinement and the `gnome` extension

    confinement: strict
    grade: stable

The `apps.nordstjernen` entry uses `extensions: [gnome]`, which wires up
the GNOME platform snap (GTK 4, Pango, Cairo, librsvg, themes, icons,
fontconfig, the GTK environment, and the desktop launch helpers) and the
correct `command-chain`. `common-id` and `desktop` point at the
installed AppStream id and `.desktop` file so the store listing and the
desktop menu entry render correctly.

## Interfaces (plugs)

Nordstjernen runs under **strict** confinement. The interfaces it plugs
into:

| Plug | Why |
| --- | --- |
| `browser-support` (`allow-sandbox: true`) | **The important one.** Each tab's engine runs in its own process locked down with Landlock + a seccomp allow-list (`src/security.c`). Under strict confinement the snap's own seccomp/AppArmor profile sits on top; `browser-support` with `allow-sandbox: true` permits the `seccomp`, `landlock_*`, and process-spawning syscalls the per-tab sandbox needs. Without it the browser still runs, but the in-tree sandbox degrades (it logs a warning and continues unconfined) — so this plug is what keeps Nordstjernen's defence-in-depth intact inside the snap. |
| `network`, `network-bind` | libcurl networking and local IPC sockets between the shell and renderer processes. |
| `opengl` | WebGL (enabled by default) and GTK's GL-accelerated rendering. |
| `audio-playback` | the `nordstjernen-audio` helper (MP2 over PulseAudio/PipeWire). |
| `home`, `removable-media` | open/save files (downloads, local `file://` pages) the user explicitly chooses. |
| `password-manager-service` | optional integration with the desktop secret store. |
| `mount-observe` | lets the engine resolve mounted paths cleanly. |

Most of these auto-connect on install. `password-manager-service` and
`mount-observe` are **not** auto-connected by default and require either
a manual `snap connect` or a store request (see below). Check what is
wired up with:

    snap connections nordstjernen

> Note: Nordstjernen's per-tab sandbox is built on Landlock + seccomp,
> **not** on user namespaces, so it does not need `system-files` or a
> classic-confinement carve-out the way some Chromium-derived snaps do.
> Strict confinement with `browser-support: allow-sandbox` is sufficient
> and is the preferred posture for a security-focused browser.

## Publishing to the Snap Store

1. **Register the name** (once). Names are first-come on the store:

       snapcraft register nordstjernen

2. **Log in:**

       snapcraft login

3. **Build, then upload to a channel.** Start on a non-default risk
   level so it can be tested before it reaches the `stable` audience:

       snapcraft upload --release=edge nordstjernen_<version>_amd64.snap

   Promote through the risk levels as confidence grows:

       snapcraft release nordstjernen <revision> beta
       snapcraft release nordstjernen <revision> candidate
       snapcraft release nordstjernen <revision> stable

4. **Request manual interface auto-connection** if you want
   `password-manager-service` (or any non-auto plug) connected for
   end users without a manual step. File a request on the snapcraft
   forum under *store-requests*, or via the store dashboard, referencing
   this snap. Until granted, those interfaces work only after a manual
   `snap connect`.

5. **Store listing assets.** The summary, description, license, and
   links come from `snapcraft.yaml`. Upload the icon
   (`data/icons/hicolor/scalable/apps/nordstjernen.svg`) and a
   screenshot (`docs/screenshot.png`) through the store dashboard's
   listing page. The AppStream metainfo
   (`data/org.nordstjernen.WebBrowser.metainfo.xml`) supplies the rich
   description and release notes.

## Multi-architecture builds

The store serves `amd64` and `arm64`. Build each on matching hardware,
or let Launchpad build them remotely:

    snapcraft remote-build

`remote-build` builds for every architecture listed under
`platforms`/`architectures` (add an `architectures` stanza to
`snapcraft.yaml` if you want to pin the set) and returns one `.snap` per
arch to upload.

## Updating the package

The snap auto-tracks the project version through `adopt-info`, so the
release flow for a new Nordstjernen version is:

1. Bump `version:` in `meson.build` (and add a `<release>` to the
   AppStream metainfo).
2. `snapcraft` to rebuild.
3. `snapcraft upload --release=<channel>` the new revision.

The store delivers the update to installed clients automatically — no
user action required.

## Troubleshooting

* **Black window / no rendering, or "seccomp ... load failed" in the
  log.** The `browser-support` plug with `allow-sandbox: true` is not
  connected. Check `snap connections nordstjernen` and
  `snap connect nordstjernen:browser-support`.
* **No audio.** Confirm the `audio-playback` plug is connected and a
  PipeWire/PulseAudio server is running on the host.
* **Cannot open local files.** Files outside `$HOME` and removable media
  need the `home` / `removable-media` plugs connected; sandbox confines
  the browser to those paths plus the user's explicit picks.
* **Inspect the runtime sandbox denials.** `journalctl -xe | grep
  audit` shows AppArmor/seccomp denials from the snap layer; the
  browser's own Landlock/seccomp messages appear on its stderr
  (run `nordstjernen` from a terminal to see them).
