# Nordstjernen on Debian — packaging and distribution

This document records how Nordstjernen is packaged as a Debian `.deb`,
and — importantly — where in the Debian ecosystem it can actually live.
The packaging sources are in the `debian/` directory at the repository
root. For the plain build-from-source instructions (any distro) see
`Linux.md`; this document is specifically about the Debian package.

## Read this first: the licensing reality

**Nordstjernen cannot go into Debian `main`.** Debian `main` only accepts
software whose license satisfies the [Debian Free Software Guidelines
(DFSG)](https://www.debian.org/social_contract#guidelines). Nordstjernen
ships under the **Nordstjernen Source License v1.0 (NSL-1.0)**, a
source-available license that:

* forbids a **"Competing Use"** — you may not offer the software (or
  something substantially similar) as a commercial product or service;
  and
* restricts education and research use to **non-commercial** contexts.

Either restriction alone violates DFSG §6 ("No Discrimination Against
Fields of Endeavor"), so NSL-1.0 is **not** DFSG-free. (The license does
include a *Grant of Future License* converting each release to MIT ten
years after it ships — but a delayed grant does not make the current
package free today.)

That leaves three realistic paths, in order of effort:

1. **A third-party APT repository (recommended, fully under our
   control).** We host signed `.deb`s and an `apt` repo; users add it
   and `apt install nordstjernen`. No Debian FTP-master review, no DFSG
   gate, works on Debian and every Debian derivative (Ubuntu, Mint, …).
   This is the practical distribution channel and is documented below.
2. **Debian `non-free`.** Debian *does* ship a `non-free` component for
   exactly this situation. It requires a Debian Developer to sponsor and
   maintain the package, and an ITP bug, but the package can be
   redistributed by Debian. The `debian/` tree here is already targeted
   at `non-free` (`Section: non-free/web`).
3. **Relicense.** If the project ever adopts a DFSG-free license for a
   release, `main` becomes possible. Not planned.

> Debian distinguishes `non-free` from `contrib`. `contrib` is for
> DFSG-free software that *depends on* non-free things; Nordstjernen's
> own license is the non-free part, so it belongs in `non-free`, not
> `contrib`.

The rest of this document covers building the `.deb` and shipping it
through paths (1) and (2).

## The `debian/` packaging tree

```
debian/
├── changelog       # version history; top entry sets the package version
├── control         # Section: non-free/web, build-deps, binary package
├── copyright       # DEP-5; documents NSL-1.0 + bundled-engine licenses
├── rules           # dh sequencer; configures meson with ai disabled
├── source/format   # 3.0 (quilt)
└── watch           # tracks upstream GitHub tags
```

Key choices:

* **`Section: non-free/web`** and a `control` long-description that
  states the license restriction outright, so the archive placement is
  unambiguous.
* **`debian/rules`** is a minimal `dh` file. Hardening is on via
  `DEB_BUILD_MAINT_OPTIONS = hardening=+all`; the
  meson build already enables PIE, stack protector, and FORTIFY itself.
* **`debian/copyright`** is DEP-5 and spells out NSL-1.0 *plus* the free
  licenses of the in-tree engines (lexbor — Apache-2.0; QuickJS — Expat;
  WAMR — Apache-2.0) and vendored single-file libraries (Wuffs —
  Apache-2.0; pl_mpeg — Expat).

## Build dependencies

The Debian build needs (these mirror `Linux.md`):

    sudo apt install build-essential debhelper devscripts meson ninja-build \
        pkg-config libgtk-4-dev libepoxy-dev libcurl4-openssl-dev libssl-dev \
        libuchardet-dev librsvg2-dev libpsl-dev libsqlite3-dev libseccomp-dev \
        libwebp-dev libsdl2-dev libenchant-2-dev

`libseccomp-dev` is marked `[linux-any]` in `debian/control`; all Debian
release architectures are Linux, so it always applies. `cmake` is **not**
a build dependency: lexbor, QuickJS, and WAMR build in-tree via meson
`subdir()` and there are no CMake subprojects.

Runtime dependencies are computed automatically by `dh_shlibdeps`
(`${shlibs:Depends}`) from the linked shared libraries — nothing is
hand-listed or bundled. `mpv | ffmpeg` and `hunspell-en-us` are
`Recommends` (external media playback and spell-check dictionary).

## Build the package

From a checkout that contains the `debian/` directory:

    dpkg-buildpackage -us -uc -b

`-b` builds a binary-only package; drop it for a full source+binary
build. The result lands in the parent directory:

    ../nordstjernen_<version>_<arch>.deb

Install and smoke-test it:

    sudo apt install ../nordstjernen_*.deb
    nordstjernen

### Clean, reproducible builds with sbuild/pbuilder

Archive-quality builds happen in a minimal chroot so build-dependency
mistakes surface immediately. With `sbuild`:

    sudo sbuild-createchroot --include=eatmydata,ccache \
        unstable /srv/chroot/unstable-amd64 http://deb.debian.org/debian
    sbuild -d unstable

or with `pbuilder`:

    sudo pbuilder create
    pdebuild

### Lint before publishing

    lintian -i -I --show-overrides ../nordstjernen_*.changes

Expect (and accept) lintian to confirm the package is non-free. Any
`license-problem-*` tag on the **bundled** engines would be a real issue;
the NSL-1.0 placement in `non-free` is intentional, not a defect.

## Path 1 — host a third-party APT repository

This is the route that gets Nordstjernen onto users' Debian/Ubuntu
machines without Debian's archive process.

1. **Build the `.deb`** as above (build once per architecture: `amd64`,
   `arm64`).

2. **Create a signed repository.** [`aptly`](https://www.aptly.info) is
   the simplest tool:

       aptly repo create -distribution=stable -component=main nordstjernen
       aptly repo add nordstjernen ../nordstjernen_*.deb
       aptly publish repo -gpg-key=<KEYID> nordstjernen

   Serve `~/.aptly/public` over HTTPS (e.g. at
   `https://apt.nordstjernen.org`).

3. **Users add the repo** with a keyring (the modern, `signed-by`
   form — never `apt-key`):

       curl -fsSL https://apt.nordstjernen.org/nordstjernen.gpg \
         | sudo tee /usr/share/keyrings/nordstjernen.gpg >/dev/null
       echo "deb [signed-by=/usr/share/keyrings/nordstjernen.gpg] \
         https://apt.nordstjernen.org stable main" \
         | sudo tee /etc/apt/sources.list.d/nordstjernen.list
       sudo apt update && sudo apt install nordstjernen

Updates ship by adding the new `.deb` to the repo and re-publishing;
`apt upgrade` then picks it up. Ship a `.deb` per release tag so
`debian/changelog`'s top version matches the upstream version.

## Path 2 — Debian `non-free`

If a Debian Developer is willing to sponsor it:

1. **File an ITP** (Intent To Package) bug against `wnpp`:

       reportbug --email <you> wnpp

   Title it `ITP: nordstjernen -- small, hand-written web browser` and
   note in the body that it targets `non-free` because of NSL-1.0.

2. **Polish the source package.** Build cleanly in `sbuild`, get
   `lintian` quiet apart from the expected non-free placement, and make
   sure `debian/copyright` is complete and accurate (FTP-masters review
   this closely — the bundled-engine licenses must all be listed).

3. **Note the embedded code copies.** Debian discourages bundled library
   copies. Nordstjernen *forks* lexbor, QuickJS, and WAMR in-tree and
   modifies them for tight integration, so they cannot simply be swapped
   for system packages. This is acceptable for `non-free` but the
   `debian/copyright` accounting (already present) must stay exhaustive,
   and the reasoning should be explained to the sponsor.

4. **Upload via the sponsor.** A DD signs and uploads to the archive;
   the package then lives in `non-free` and is mirrored by Debian.

`debian/watch` lets `uscan` track upstream GitHub tags so the sponsor
can spot new releases.

## Updating the package

For both paths the per-release flow is:

1. Tag the upstream release and update `version:` in `meson.build`.
2. Add a top entry to `debian/changelog`
   (`dch -v <version>-1 "New upstream release"`); set the distribution
   (`unstable` for Debian, or your repo's codename).
3. Rebuild with `dpkg-buildpackage`/`sbuild`.
4. For Path 1, add the new `.deb` to the `aptly` repo and re-publish; for
   Path 2, hand the new source package to the sponsor.

## Troubleshooting

* **`dpkg-shlibdeps: warning: ... not found`** — a runtime library's
  `-dev` package is missing from `Build-Depends`; add it and rebuild in a
  clean chroot to catch the rest.
* **`lintian: license-problem-*`** on an engine under `src/` or
  `subprojects/` — a bundled component's license is misdeclared; fix
  `debian/copyright` to match `THIRD-PARTY-LICENSES.md`.
* **Sandbox warnings at runtime** (`seccomp: load failed`) — harmless
  outside a confined environment; the per-tab Landlock + seccomp sandbox
  logs and continues if the host policy blocks it. See `tab-isolation.md`.
