# Alpine Linux packaging

`data/packaging/APKBUILD` builds Nordstjernen from source on Alpine
(musl) with `abuild`. It produces a minimal package: the `nordstjernen`,
`nordstjernen-renderer`, `nordstjernen-audio` and `nordstjernen-video`
binaries plus icons, the desktop file, the
i18n catalogues and the license — **nothing is bundled**. Every runtime
library (GTK 4, libcurl, OpenSSL, libwebp, libavif, FFmpeg, poppler-glib, …)
is resolved from Alpine system packages, and `abuild`'s `tracedeps` derives
the `depends=` automatically from the linked shared objects, so the
dependency list is always exactly what the binary needs.

The build is fully offline: Wuffs and pl_mpeg are vendored in the source
tarball, and `build()` passes `-Dns-pango=disabled` so the one remaining
subproject — ns-pango, which meson clones with git — is never reached.
Text shapes through Alpine's `pango-dev` instead, and nothing is fetched
during Alpine's network-isolated `build()` phase.

GitHub names the archive root after the repository, so the tarball unpacks
to `nordstjernen-browser-$pkgver` rather than `$pkgname-$pkgver`; the
APKBUILD sets `builddir` accordingly.

## License — important

Nordstjernen is released under the **Nordstjernen Source License v1.0**
(`License.md`), a Functional-Source-License-style *source-available*
license with a "Competing Use" restriction (it converts to MIT only on
the tenth anniversary of each release). This is **not** an OSI-approved
or free-software license.

Alpine's official `aports` (main/community/testing) require free /
OSI-approved licenses, so Nordstjernen **cannot be accepted into the
official Alpine repositories as-is**. This APKBUILD is therefore intended
for a **personal / custom Alpine repository**. Official inclusion would
require relicensing the project under an OSI-approved license first.

## Build and install locally (custom repo)

```sh
# On Alpine, as a user in the abuild group:
sudo apk add alpine-sdk
abuild-keygen -a -i

# From a checkout of this repo:
cd data/packaging
abuild -r              # fetches the pkgver source tarball, builds, packages

# Install the result:
sudo apk add --allow-untrusted \
    ~/packages/*/$(uname -m)/nordstjernen-<pkgver>-r0.apk
```

`abuild -r` installs `makedepends`, downloads the source during the
network-allowed fetch phase, then builds offline.

## Updating the version

Bump `pkgver` in `data/packaging/APKBUILD` to the new release tag, reset
`pkgrel=0`, then refresh the checksum:

```sh
cd data/packaging
abuild checksum        # rewrites sha512sums from the fetched tarball
```

The `source=` URL points at the GitHub release tarball
`.../archive/refs/tags/$pkgver.tar.gz`. Note that GitHub's
auto-generated archive checksums are not guaranteed stable forever; if a
checksum mismatch appears, re-run `abuild checksum`.

## Submitting to official aports (only after relicensing)

If the project is relicensed under an OSI-approved license, the steps to
propose it for `aports` would be:

1. Fork <https://gitlab.alpinelinux.org/alpine/aports>.
2. Add the APKBUILD under `community/nordstjernen/APKBUILD` (new packages
   start in `testing/`, then move to `community/` after review).
3. Replace `license="custom"` with the correct SPDX identifier.
4. Verify it builds in a clean chroot with `abuild rootbld` and passes
   `apkbuild-lint` / `apkbuild-shellcheck`.
5. Commit with the message `testing/nordstjernen: new aport` and open a
   merge request against `aports`.

See <https://wiki.alpinelinux.org/wiki/Creating_an_Alpine_package> for
the full contributor workflow.
