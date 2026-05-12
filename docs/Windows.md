# Nordstjernen on Windows — build, run, package

This document records the working setup for building and packaging
Nordstjernen on Windows. The Windows CI workflow
(`.github/workflows/windows.yml`) is the authoritative spec; what
follows is the same recipe verified on a local Windows 11 box.

The Windows build uses **MSYS2 / MINGW64** with the same toolchain
the CI workflow installs. No Visual Studio, no MSVC, no vcpkg.
The rationale is identical to the macOS Homebrew path: one
package source, audited DLLs, and the resulting binary is a
single PE32+ executable that links the same GTK 4 / libcurl /
Cairo / Pango / Gumbo / libvpx libraries shipped to Linux users.

## One-time setup

Install MSYS2 (use `winget` if you have it):

```powershell
winget install -e --id MSYS2.MSYS2
```

Open the **MSYS2 MINGW64** shell (not the plain MSYS shell — the
window title and prompt must say `MINGW64`) and install the
packages the CI workflow installs:

```sh
pacman -Sy --noconfirm --needed \
    base-devel \
    mingw-w64-x86_64-clang \
    mingw-w64-x86_64-pkgconf \
    mingw-w64-x86_64-meson \
    mingw-w64-x86_64-ninja \
    mingw-w64-x86_64-ccache \
    mingw-w64-x86_64-gtk4 \
    mingw-w64-x86_64-curl \
    mingw-w64-x86_64-gumbo-parser \
    mingw-w64-x86_64-libvpx
```

This pulls in roughly 600 MB of runtime + headers. `pacman -Syu`
is intentionally avoided: we want to match what CI installs, not
the rolling latest.

## Build

From the MINGW64 shell, in the checkout:

```sh
cd /c/dev/nordstjernen
export CC="ccache clang"
meson setup builddir
meson compile -C builddir
```

The first `meson setup` downloads
[quickjs-ng v0.14.0](https://github.com/quickjs-ng/quickjs/releases/tag/v0.14.0)
via `subprojects/quickjs.wrap`. Build artifacts:

- `builddir/src/nordstjernen.exe` — the main binary.
- `builddir/subprojects/quickjs-0.14.0/libqjs.a` — the static QuickJS
  library that linked into the exe.

The exe itself is ~7 MB. Running it directly out of `builddir`
works as long as the MSYS2 mingw64 `bin` directory is on `PATH`
(it is inside the MINGW64 shell; from PowerShell, prepend
`C:\msys64\mingw64\bin`).

Smoke test:

```sh
./builddir/src/nordstjernen.exe --print-config | head
./builddir/src/nordstjernen.exe --headless --dump=text about:nordstjernen
./builddir/src/nordstjernen.exe https://lite.cnn.com   # opens a GTK 4 window
```

## Package — redistributable bundle

`pack-windows.sh` produces a self-contained `dist/nordstjernen-win64/`
folder that runs on a Windows machine with no MSYS2 install. It:

1. Copies `nordstjernen.exe` and transitively resolves every imported
   DLL via `objdump -p`, pulling each one from `/mingw64/bin/` into
   the bundle (system DLLs like `KERNEL32.dll` are skipped because
   they aren't found there).
2. Copies the GTK runtime data the binary needs at startup:
   `share/glib-2.0/schemas/gschemas.compiled`,
   `lib/gdk-pixbuf-2.0/` (loader DLLs + `loaders.cache`),
   `share/icons/Adwaita/`, `share/icons/hicolor/`.
3. Copies the CA bundle to `etc/ssl/certs/ca-bundle.crt` so libcurl
   can verify TLS certificates.
4. Writes a `nordstjernen.cmd` launcher that sets
   `GTK_DATA_PREFIX` / `GDK_PIXBUF_MODULE_FILE` / `CURL_CA_BUNDLE`
   / `SSL_CERT_FILE` to the bundle's own paths and invokes the exe.

Run it from the MINGW64 shell:

```sh
./pack-windows.sh
```

Typical output: 73 DLLs, ~76 MB. The bundle is portable — copy
the folder to another Windows box (or hand it to a user) and
double-click `nordstjernen.cmd` to launch.

The bundle is intentionally *not* a signed installer. Phase 11
(Distribution) covers signing + an actual `.exe` installer with
shortcuts and an uninstaller. The bundle here is the
"copy-and-run" intermediate.

## Known Windows-specific differences

- **No Landlock sandbox.** `src/security.c` guards the Landlock
  syscalls behind `#ifdef __linux__`; on Windows the sandbox init
  is a no-op. The intentional analogue (AppContainer / Job Object)
  is not yet implemented.
- **Refuses to run as Administrator.** Mirrors the Linux refuse-root
  check (`src/security.c::nd_security_refuse_root`). Elevation is
  detected via `CheckTokenMembership` against the builtin
  Administrators SID; if the token is a member, the process prints a
  warning to stderr and exits 77. `ND_ALLOW_ROOT=1` bypasses, same
  as Linux.
- **Self-exe path** is resolved via `GetModuleFileNameW`, so
  Ctrl+N / target=_blank / middle-click correctly re-spawn the same
  binary path (no `/proc/self/exe` equivalent needed).
- **No `fork+exec`.** Per-window OS processes use the Win32
  `CreateProcessW` path implicitly through GIO's `g_spawn_async`.

## Troubleshooting

- *"The application was unable to start correctly (0xc000007b)"* —
  PATH is picking up a 32-bit DLL from elsewhere. Either run from
  the bundle directory or via `nordstjernen.cmd`.
- *Icons missing / buttons blank in the header bar* —
  `share/icons/Adwaita` didn't make it into the bundle. Re-run
  `pack-windows.sh`.
- *TLS errors (`SSL certificate problem`)* —
  `etc/ssl/certs/ca-bundle.crt` isn't being picked up. The
  launcher `.cmd` sets `CURL_CA_BUNDLE`; running `nordstjernen.exe`
  directly from PowerShell bypasses that. Set the env var
  manually or use the `.cmd`.
