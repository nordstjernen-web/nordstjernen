# Nordstjernen on openSUSE — packaging and distribution

This document records how Nordstjernen is packaged for openSUSE through
the [Open Build Service (OBS)](https://build.opensuse.org), where it can
live, and how the git-backed build is wired up. For plain
build-from-source instructions (any distro) see `Linux.md`; this document
is specifically about the openSUSE RPM and OBS.

The build recipe lives at the repository **root** (`nordstjernen.spec`),
because OBS clones the whole repo and looks for the recipe at the top of
the synced tree. Notes for maintainers are in `packaging/obs/README.md`.

## Install it (users)

Nordstjernen is built in the OBS home project
[`home:andreasrosdal`](https://build.opensuse.org/package/show/home:andreasrosdal/Nordstjernen).
Add the repository and install; updates then arrive through `zypper`:

```sh
# openSUSE Tumbleweed
sudo zypper addrepo https://download.opensuse.org/repositories/home:/andreasrosdal/openSUSE_Tumbleweed/home:andreasrosdal.repo
sudo zypper refresh
sudo zypper install nordstjernen
```

For Leap, replace `openSUSE_Tumbleweed` with your release (e.g. `16.0`).
The package builds for `x86_64`, `aarch64`, and `i586`.

Prefer a one-off install without adding the repo? Grab the nightly RPM
directly: [`nordstjernen-opensuse-x86_64.rpm`](https://www.nordstjernen.org/nightly/nordstjernen-opensuse-x86_64.rpm)
(rebuilt from `main` each night; `sudo zypper install ./…rpm`).

## Read this first: the licensing reality

**Nordstjernen cannot go into openSUSE:Factory (Tumbleweed's main / OSS
distribution).** Factory only accepts free / OSI-approved licenses, and
its automated [Cavil](https://github.com/openSUSE/cavil) legal scan plus
the legal team will reject anything else. Nordstjernen ships under the
**Nordstjernen Source License v1.0 (NSL-1.0)**, a source-available license
that:

* forbids a **"Competing Use"** — you may not offer the software (or
  something substantially similar) as a commercial product or service; and
* restricts education and research use to **non-commercial** contexts.

Either restriction makes NSL-1.0 non-free, so the OSS door is closed.
(The license does convert each release to MIT ten years after it ships,
but a delayed grant does not make today's package free.)

That leaves these paths, in order of effort:

1. **The OBS home project (recommended, fully under our control).** We
   build and publish RPMs; users add the repo and `zypper install`. No
   Factory review, no legal gate. This is the practical, permanent channel
   and is what the install section above uses.
2. **openSUSE non-OSS (`openSUSE:Factory:NonFree` → the `non-oss` repo,
   enabled by default).** openSUSE *does* ship a non-free component for
   redistributable-but-not-open-source software. NSL-1.0 permits
   redistribution of copies (with notice, no Competing Use), so this is the
   only realistic *in-distribution* path — but it still needs the openSUSE
   legal team to accept NSL-1.0's terms. **Ask first** on the
   opensuse-factory mailing list before preparing a submit request.
3. **Relicense.** If a release ever adopts a free license, Factory becomes
   possible. Not planned.

## Git-backed OBS package (scmsync)

The package is bound to this git repo with the OBS
[`obs-scm-bridge`](https://github.com/openSUSE/obs-scm-bridge): OBS clones
`main` and builds it. Nothing is uploaded to OBS by hand and no source
tarball is stored — git is authoritative.

It is set via the package meta (package → *Advanced* → *Meta* in the web
UI, or `osc meta pkg home:andreasrosdal Nordstjernen -e`), adding one line:

```xml
<scmsync>https://github.com/nordstjernen-web/nordstjernen-browser?trackingbranch=main</scmsync>
```

After this, edit the spec in this repo and push — do not edit files in
OBS. To pick up a push, the bridge must re-fetch: re-save the meta, or set
up a `runservice` token as a GitHub push webhook so it re-syncs
automatically.

### Why there is no fetch `_service`

build.opensuse.org runs its build workers in **secure mode with no network
access**, so source services that clone over the network (`tar_scm`,
`obs_scm`) produce nothing there — the build dies at `recompress`
("no such file … `nordstjernen-*.tar`") or at the buildtime `tar`
("no .obsinfo file found"). The scmsync bridge clones git on OBS
infrastructure *outside* the workers, which is why it is the only
git-backed path that works. The spec therefore has **no** `Source0` and no
`_service`; its `%prep` simply locates the source the bridge laid down and
assembles the build tree:

```spec
%prep
%setup -q -c -T
top=$(find "%{_sourcedir}" -name meson.build 2>/dev/null \
      | awk '{ print length, $0 }' | sort -n | head -1 | cut -d' ' -f2-)
cp -a "$(dirname "$top")"/. .
test -f meson.build
```

## Build options and dependencies

The spec configures meson with two features off:

    -Dwebgpu=disabled   # needs external wgpu-native, not packaged
    -Dns-pango=disabled # OBS workers have no network to clone the subproject

On 32-bit x86 the bundled WebAssembly interpreter (WAMR, `src/wamr/`) fails
to build, so the spec additionally passes `-Dwasm=disabled` there:

```spec
%ifarch i386 i486 i586 i686
%global extra_meson -Dwasm=disabled
%endif
```

Everything else builds from the declared `BuildRequires`:

    gcc gcc-c++ meson ninja pkgconfig update-desktop-files
    pkgconfig(gtk4) pkgconfig(epoxy) pkgconfig(libcurl) pkgconfig(libcrypto)
    pkgconfig(uchardet) pkgconfig(libpsl) pkgconfig(sqlite3)
    pkgconfig(libwebp) pkgconfig(libavif) pkgconfig(sdl2)
    pkgconfig(libavcodec) pkgconfig(libavformat) pkgconfig(libavutil)
    pkgconfig(libswresample) pkgconfig(libswscale)
    pkgconfig(fontconfig) pkgconfig(pango) pkgconfig(pangocairo)
    pkgconfig(pangoft2)
    pkgconfig(libseccomp) pkgconfig(enchant-2)

The FFmpeg `libav*` packages are required, not optional: meson fails without
them on Linux, and they carry inline WebM (VP9/VP8 + Opus/Vorbis). The Pango
packages cover the system-Pango path that `-Dns-pango=disabled` selects.

The in-tree engines (lexbor, QuickJS, WAMR) and vendored single-file
libraries (Wuffs, pl_mpeg) build via meson `subdir()` / wraps — no `cmake`,
no system copies. `mpv` and `myspell-en_US` are `Recommends` (external
media playback and a spell-check dictionary).

The package is tagged `License: SUSE-NonFree` (the conventional marker for
a non-free license); `License.md` is shipped as `%license`.

## Local RPM build

Off OBS, you can build the same RPM from a checkout with `rpmbuild`, or use
the helper scripts `scripts/pack-rpm.sh` (portable repackage) and
`scripts/pack-srpm.sh` (source RPM). For OBS itself nothing is needed
beyond the `<scmsync>` meta line — pushing to `main` is the build trigger.

## Build the package set wider

Add more targets in the project's **Repositories** tab: openSUSE
Tumbleweed and Leap 16.0 are known good. Skip Debian targets (they expect
`debian/` packaging, not an RPM spec) and only keep the architectures you
care about.

## Troubleshooting

* **Build keeps using an old commit** — the bridge has not re-fetched.
  Re-save the package meta or trigger the `runservice` token; set up a
  GitHub push webhook so it syncs automatically.
* **`%prep` fails / meson can't find `meson.build`** — the bridge laid the
  source out somewhere the `%prep` search did not reach; check the
  `%{_sourcedir}` listing in the log.
* **`recompress` / buildtime `tar` errors about a missing tarball or
  `.obsinfo`** — a network fetch `_service` crept back in; remove it, the
  scmsync bridge provides the source.
* **i586 fails compiling `src/wamr/…`** — `-Dwasm=disabled` is not being
  applied for that arch; confirm the `%ifarch` block is present.
* **Sandbox warnings at runtime** (`seccomp: load failed`) — harmless
  outside a confined environment; the per-tab Landlock + seccomp sandbox
  logs and continues. See `tab-isolation.md`.
