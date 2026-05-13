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
    mingw-w64-x86_64-cmake \
    mingw-w64-x86_64-gtk4 \
    mingw-w64-x86_64-curl \
    mingw-w64-x86_64-ca-certificates \
    mingw-w64-x86_64-uchardet \
    mingw-w64-x86_64-gumbo-parser \
    mingw-w64-x86_64-libvpx
```

This pulls in roughly 600 MB of runtime + headers. `pacman -Syu`
is intentionally avoided: we want to match what CI installs, not
the rolling latest.

A few of these pull in transitively but are listed explicitly so a
fresh box gets them in one command:

- `mingw-w64-x86_64-ca-certificates` is a hard dependency of
  `mingw-w64-x86_64-curl`, so it always arrives with curl. It's
  what makes HTTPS work — `src/net.c::nd_net_resolve_ca_bundle`
  hardcodes `C:/msys64/mingw64/etc/ssl/certs/ca-bundle.crt` in
  the Windows fallback list. With the package installed, dev
  builds reach `https://` without any env-var fiddling.
- `mingw-w64-x86_64-cmake` is only needed when the lexbor HTML
  engine falls back to its `subprojects/lexbor.wrap` source build
  (no system pkg-config `lexbor` is available on MSYS2). meson
  invokes cmake to configure the lexbor subproject; without it,
  `meson setup` aborts with `ERROR: Unable to find CMake`.
- `mingw-w64-x86_64-uchardet` is the charset detector used by
  `nd_html_decode_body`. Per `CLAUDE.md` it's a hard dependency;
  without it `meson setup` fails the `uchardet` pkg-config check.

## Build

From the MINGW64 shell, in the checkout:

```sh
cd /c/dev/nordstjernen
export CC="ccache clang"
meson setup builddir
meson compile -C builddir
```

The first `meson setup` downloads three source subprojects into
`subprojects/`, configures them, and builds them into static libs
that link into the main binary:

- [quickjs-ng v0.14.0](https://github.com/quickjs-ng/quickjs/releases/tag/v0.14.0)
  via `subprojects/quickjs.wrap` (the JS engine).
- [ada-url v2.9.2](https://github.com/ada-url/ada) singleheader via
  `subprojects/ada.wrap` (WHATWG URL parsing — a small C++17
  amalgamation, hence clang++ also being a dependency).
- [lexbor v3.0.0](https://github.com/lexbor/lexbor) via
  `subprojects/lexbor.wrap`. This one builds through a CMake
  subproject, so `mingw-w64-x86_64-cmake` must be installed first.

Build artifacts:

- `builddir/src/nordstjernen.exe` — the main binary (~15 MB).
- `builddir/subprojects/quickjs-0.14.0/libqjs.a`
- `builddir/subprojects/ada-2.9.2/libada.a`
- `builddir/subprojects/lexbor-3.0.0/liblexbor_static.a`

Running the exe directly out of `builddir` works as long as the
MSYS2 mingw64 `bin` directory is on `PATH` (it is inside the
MINGW64 shell; from PowerShell, prepend `C:\msys64\mingw64\bin`).

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

The script builds (or reuses) a separate `builddir-release/`
tree configured with `--buildtype=release`, so `NDEBUG` is
defined when QuickJS and friends compile. That matters because
quickjs-ng v0.14.0 has an unconditional `assert(list_empty(&rt->gc_obj_list))`
in `JS_FreeRuntime` (`quickjs.c:2323`) that fires on any leaked
JS object at context-teardown time — which is easy to hit when
real-world JS-heavy pages (e.g., DuckDuckGo) navigate while
event handlers / in-flight `fetch()` promises still hold
JSValues. Production builds need that assertion compiled out;
debugging the actual leak is a separate task.

Typical output: 73 DLLs, ~71 MB. The bundle is portable — copy
the folder to another Windows box (or hand it to a user) and
double-click `nordstjernen.cmd` to launch.

The bundle is intentionally *not* a signed installer. Phase 11
(Distribution) covers signing + an actual `.exe` installer with
shortcuts and an uninstaller. The bundle here is the
"copy-and-run" intermediate.

## CA bundle (what `etc/ssl/certs/ca-bundle.crt` is)

A **CA bundle** is a plain-text file containing the
PEM-encoded X.509 root certificates of the public Certificate
Authorities that the browser trusts to sign HTTPS server
certificates. When the browser opens `https://example.com`,
libcurl + OpenSSL verifies that the server's certificate chains
up to a root in this file; if it doesn't, the connection is
refused as untrusted. Without a trusted CA store, every HTTPS
fetch fails with `error adding trust anchors from file:
ca-bundle.crt` (or the equivalent OpenSSL error).

The file `ca-bundle.crt` we ship is the one packaged by MSYS2 as
`mingw-w64-x86_64-ca-certificates`, which itself is sourced from
the Mozilla NSS / `certdata.txt` curated root list — the same
source Firefox uses. We bundle it because:

- **Windows itself stores roots in the registry (the Windows
  Certificate Store), not as a PEM file**, and the mingw build
  of libcurl + OpenSSL we link against does not consult that
  store. Without the bundled file, libcurl has nowhere to find
  trusted anchors and refuses every HTTPS fetch.
- The mingw libcurl is compiled with a default CA path of
  `C:/msys64/mingw64/etc/ssl/certs/ca-bundle.crt`. That path
  only exists on a machine with MSYS2 installed; on a fresh
  user box it is missing.

`src/net.c::nd_net_resolve_ca_bundle` resolves the file at
`nd_net_init()` time, in this order:

1. `$CURL_CA_BUNDLE` env var, if set and the path exists.
2. `$SSL_CERT_FILE` env var, same condition.
3. On Windows: `<exe_dir>/etc/ssl/certs/ca-bundle.crt`, then
   `<exe_dir>/ssl/certs/ca-bundle.crt`, then
   `<exe_dir>/ca-bundle.crt`, then `<exe_dir>/cert.pem`.

If found, the resolved path is applied to every `curl_easy`
handle via `CURLOPT_CAINFO` before `curl_easy_perform`. The
resolution is per-process and cached for the lifetime of the
process. The launcher `.cmd` also exports `CURL_CA_BUNDLE` /
`SSL_CERT_FILE` for the same path, but those are redundant
now that the binary self-resolves; they only matter for
third-party tooling that spawns from the same env.

On Linux / macOS the env-var path applies; otherwise libcurl's
own system-default resolution wins (`/etc/ssl/certs/...` and
friends). Those distros maintain CA stores out of the box,
so we don't ship a bundled copy there.

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
- **No D-Bus session bus.** GLib on Windows prints a `win32 session
  dbus binary not found` warning at startup when no
  `dbus-daemon.exe` is on `PATH`. We intentionally don't ship
  D-Bus — none of the browser's surface uses it on Windows; the
  warning is benign noise. `main.c` installs a
  `g_log_set_writer_func` that filters this specific message
  through `g_log_writer_default`; every other warning still
  prints.

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
