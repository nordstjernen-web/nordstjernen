# Nordstjernen on Linux — build, run, package

This document records the working setup for building and packaging
Nordstjernen on Linux. The primary supported targets are Debian /
Ubuntu, Fedora / RHEL, and openSUSE on `x86_64`. Local Linux is the
correctness gate: every commit must pass `meson compile -C builddir`
locally before pushing.

## Build dependencies

System packages required on Debian / Ubuntu:

    sudo apt install build-essential pkg-config meson ninja-build \
        libgtk-4-dev libcurl4-openssl-dev libuchardet-dev librsvg2-dev \
        libpsl-dev libsqlite3-dev libseccomp-dev webp-pixbuf-loader

On Fedora / RHEL:

    sudo dnf install gcc pkgconf meson ninja-build gtk4-devel libcurl-devel \
        uchardet-devel librsvg2-devel libpsl-devel sqlite-devel libseccomp-devel \
        webp-pixbuf-loader

On openSUSE:

    sudo zypper install gcc pkgconf meson ninja gtk4-devel libcurl-devel \
        libuchardet-devel librsvg-devel libpsl-devel sqlite3-devel libseccomp-devel \
        webp-pixbuf

On Alpine (musl libc):

    sudo apk add build-base linux-headers pkgconf meson ninja gtk4.0-dev \
        curl-dev uchardet-dev librsvg-dev libpsl-dev sqlite-dev libseccomp-dev

Alpine builds against musl rather than glibc, so the resulting binary
is not interchangeable with the glibc portable zip — run a musl build
on a musl system. The `webp-pixbuf-loader` package is not in Alpine's
repositories, so lossy WebP images fall back to the broken-image box;
everything else builds and runs the same. Add `clang cmake git zip` if
you are packaging with the nightly scripts.

`ccache` is the biggest build-time win — `meson` picks it up
automatically. With ccache warm, a clean `meson setup builddir &&
meson compile -C builddir` drops from ~35 s to ~1 s. Install once
with the distro package manager.

## Develop

    meson setup builddir
    meson compile -C builddir
    ./builddir/src/nordstjernen

`./scripts/dev.sh build` runs `meson setup` (only if needed) and
`meson compile -C builddir` in one shot.

## Package — portable zip

`./scripts/pack-linux.sh` produces a redistributable, stripped,
LTO-optimised x86_64 build:

    dist/nordstjernen-<version>-linux-x86_64.zip       # ~1.5 MB
    dist/nordstjernen-<version>-linux-x86_64/          # unpacked bundle

The zip contains the `nordstjernen` binary, the application icon,
the desktop entry, `README.md`, `THIRD-PARTY-LICENSES.md`, and a
generated `INSTALL.md` listing the runtime requirements.

The in-tree browser engine — lexbor, quickjs, and the
uchardet wrapper — is statically linked. The GTK desktop stack stays
dynamic because it expects to find pixbuf loaders, IM modules, and
font/theme data on the host at runtime; fully-static GTK isn't
practical. Runtime requirements:

- glibc 2.31+ (Ubuntu 20.04 / Fedora 34 / Debian 11 era and later)
- GTK 4.6+ with gio, gobject, pango, cairo, gdk-pixbuf
- libcurl with a TLS backend
- libuchardet
- librsvg
- fontconfig + a font set, harfbuzz, freetype, libstdc++
- An X11 or Wayland session

Smoke test the bundled binary headlessly without installing:

    ./dist/nordstjernen-<version>-linux-x86_64/nordstjernen \
        --headless --url=https://example.com --dump=text

## Package — RPM

`./scripts/pack-rpm.sh` repackages the same staged bundle as a
binary RPM. It calls `pack-linux.sh` first if the bundle is missing,
then drives `rpmbuild` against a generated spec under
`dist/rpmbuild/SPECS/`. The resulting RPM lands in `dist/`.

    sudo zypper install rpm-build       # openSUSE
    sudo dnf install rpm-build          # Fedora / RHEL
    sudo apt install rpm                # Debian / Ubuntu (ships rpmbuild)
    ./scripts/pack-rpm.sh

Output:

    dist/nordstjernen-<version>-1.x86_64.rpm           # ~1.3 MB

The spec uses `AutoReqProv: yes` so `rpmbuild` extracts the actual
SONAME dependencies (`libgtk-4.so.1`, `libcurl.so.4`,
`librsvg-2.so.2`, `libuchardet.so.0`, the GLib stack, etc.) directly
from the binary's ELF dynamic section. The same RPM file therefore
installs on Fedora, RHEL, and openSUSE without per-distro tweaks —
each distro's resolver maps the SONAMEs to its own provider
packages. (Cross-installing into Debian / Ubuntu uses `alien`.)

Install layout:

    /usr/bin/nordstjernen
    /usr/share/icons/hicolor/scalable/apps/nordstjernen.svg
    /usr/share/applications/nordstjernen.desktop
    /usr/share/doc/packages/nordstjernen/{README.md,THIRD-PARTY-LICENSES.md}

Inspect, install, remove:

    rpm -qpi dist/nordstjernen-<version>-1.x86_64.rpm   # metadata
    rpm -qpR dist/nordstjernen-<version>-1.x86_64.rpm   # required SONAMEs
    sudo dnf install ./dist/nordstjernen-<version>-1.x86_64.rpm
    sudo rpm -e nordstjernen

The `%post` / `%postun` scriptlets refresh `gtk-update-icon-cache`
and `update-desktop-database` if those tools are available, so the
application picker picks up the new entry without a re-login.

## Notes

The lower-bound glibc version pinned to `libc.so.6(GLIBC_2.x)` in
auto-generated requires reflects whatever the build host ships. If
you want broader portability than the build host's libc allows,
build inside an older base container (e.g. Rocky 8) and re-run
`pack-linux.sh` + `pack-rpm.sh` from there. AppImage / Flatpak
packaging is future work.
