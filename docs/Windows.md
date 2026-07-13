# Nordstjernen on Windows — build, run, package

https://apps.microsoft.com/detail/9nw8t7w5z4pl

This document records the working setup for building and packaging
Nordstjernen on Windows. The Windows CI workflow
(`.github/workflows/windows.yml`) is the authoritative spec; what
follows is the same recipe verified on a local Windows 11 box.

The Windows build uses **MSYS2 / MINGW64** with the same toolchain
the CI workflow installs. No Visual Studio, no MSVC, no vcpkg.
The rationale is identical to the macOS Homebrew path: one
package source, audited DLLs, and the resulting binary is a
single PE32+ executable that links the same GTK 4 / libcurl /
Cairo / Pango / lexbor libraries shipped to Linux users.

## One-time setup

Install MSYS2 (use `winget` if you have it):

```powershell
winget install -e --id MSYS2.MSYS2
```

Open the **MSYS2 MINGW64** shell (not the plain MSYS shell — the
window title and prompt must say `MINGW64`) and install the build
and runtime dependencies:

```sh
pacman -Sy --noconfirm --needed \
    base-devel \
    mingw-w64-x86_64-clang \
    mingw-w64-x86_64-pkgconf \
    mingw-w64-x86_64-meson \
    mingw-w64-x86_64-ninja \
    mingw-w64-x86_64-ccache \
    mingw-w64-x86_64-gtk4 \
    mingw-w64-x86_64-libepoxy \
    mingw-w64-x86_64-curl \
    mingw-w64-x86_64-ca-certificates \
    mingw-w64-x86_64-uchardet \
    mingw-w64-x86_64-librsvg \
    mingw-w64-x86_64-libpsl \
    mingw-w64-x86_64-sqlite3 \
    mingw-w64-x86_64-libwebp \
    mingw-w64-x86_64-SDL2
```

This pulls in roughly 600 MB of runtime + headers. `pacman -Syu`
is intentionally avoided: pin to the packages above rather than the
rolling latest.

A few of these pull in transitively but are listed explicitly so a
fresh box gets them in one command:

- `mingw-w64-x86_64-ca-certificates` is a hard dependency of
  `mingw-w64-x86_64-curl`, so it always arrives with curl. It's
  what makes HTTPS work — `src/net.c::ns_net_resolve_ca_bundle`
  hardcodes `C:/msys64/mingw64/etc/ssl/certs/ca-bundle.crt` in
  the Windows fallback list. With the package installed, dev
  builds reach `https://` without any env-var fiddling.
- `mingw-w64-x86_64-uchardet` is the charset detector used by
  `ns_html_decode_body`. Per `CLAUDE.md` it's a hard dependency;
  without it `meson setup` fails the `uchardet` pkg-config check.
- `mingw-w64-x86_64-libwebp` is a hard build dependency for
  WebP. Both lossy VP8 — used by the BBC, Wikipedia thumbnails,
  and most modern CDNs — and lossless VP8L decode in-tree via
  `src/image_webp.c`; no gdk-pixbuf loader or `loaders.cache`
  registration is involved.

## Build

From the MINGW64 shell, in the checkout:

```sh
cd /c/dev/nordstjernen
export CC="ccache clang"
meson setup builddir
meson compile -C builddir
```

The QuickJS JS engine (forked from
[quickjs-ng](https://github.com/quickjs-ng/quickjs)) and the lexbor
HTML / WHATWG-URL parser are both vendored in-tree at
`src/quickjs/` and `src/lexbor/` respectively, so the first
`meson setup` no longer touches the network for either of them. The
only remaining source subproject is Wuffs (image decoders), shipped
as a single-file release under `subprojects/wuffs/`.

Build artifacts:

- `builddir/src/gtk/nordstjernen.exe` — the main binary (the thin GTK
  shell; ~15 MB).
- `builddir/src/nordstjernen-renderer.exe` — the out-of-process,
  sandboxed renderer the shell spawns per tab. Ship it next to
  `nordstjernen.exe` (the packaging scripts do).
- `builddir/src/quickjs/libqjs.a`
- `builddir/src/lexbor/liblexbor_static.a`

Running the exe directly out of `builddir` works as long as the
MSYS2 mingw64 `bin` directory is on `PATH` (it is inside the
MINGW64 shell; from PowerShell, prepend `C:\msys64\mingw64\bin`).
PowerShell smoke launches should use `scripts\smoke-windows.ps1`,
which sets that DLL search path before creating the process and
suppresses Windows loader dialog boxes if a dependency is still
missing.

Smoke test:

```sh
./builddir/src/gtk/nordstjernen.exe --print-config | head
./builddir/src/gtk/nordstjernen.exe --headless --dump=text about:start
./builddir/src/gtk/nordstjernen.exe https://lite.cnn.com   # opens a GTK 4 window
powershell -ExecutionPolicy Bypass -File scripts\smoke-windows.ps1 about:start
```

## Package — redistributable bundle

`scripts/pack-windows.sh` produces a self-contained `dist/nordstjernen-win64/`
folder that runs on a Windows machine with no MSYS2 install. It:

1. Copies a tiny Win32 launcher as `nordstjernen.exe`, copies the
   real GTK shell as `app/nordstjernen-ui.exe`, copies
   `app/nordstjernen-renderer.exe`, and transitively resolves every
   imported DLL via `objdump -p`, pulling each one from `/mingw64/bin/`
   into `app/` (system DLLs like `KERNEL32.dll` are skipped because
   they aren't found there).
2. Copies the GTK runtime data the binary needs at startup:
   `app/share/glib-2.0/schemas/gschemas.compiled`,
   `app/lib/gdk-pixbuf-2.0/` (loader DLLs + `loaders.cache`),
   `app/share/icons/Adwaita/`, `app/share/icons/hicolor/`,
   `app/etc/fonts/`.
3. Copies the CA bundle to `app/etc/ssl/certs/ca-bundle.crt` so libcurl
   can verify TLS certificates.
4. Leaves one user-facing executable at the bundle root. The launcher
   starts `app/nordstjernen-ui.exe`; that process bootstraps
   `GTK_DATA_PREFIX`, `GDK_PIXBUF_MODULE_FILE`, `CURL_CA_BUNDLE`, and
   `SSL_CERT_FILE` from `app/` on startup, so no launcher script is
   needed. Earlier bundles shipped a `nordstjernen.cmd` wrapper for
   this; it flashed a brief console window on launch and has been
   removed. The current `nordstjernen.exe` launcher exists to preserve
   the public executable name and to show a clear error if a user runs
   it from inside the ZIP before extracting the whole folder.

Run it from the MINGW64 shell:

```sh
./scripts/pack-windows.sh
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

Typical output: 77 DLLs, ~86 MB. The bundle is portable — extract
the whole `nordstjernen-win64` folder to another Windows box and
double-click `nordstjernen.exe` to launch. Running the executable
directly from inside File Explorer's ZIP view is not supported;
Explorer extracts only the clicked file, and the launcher displays
an "Extract All" message instead of failing with Windows'
`0xC0000135` missing-DLL status.

The bundle is intentionally *not* code-signed. Authenticode signing
is a separate, manual step (see Phase 11 / Distribution).
`scripts/pack-windows-installer.sh` (below) wraps this bundle in a proper
`.exe` installer with shortcuts and an uninstaller.

## Package — `.exe` installer (NSIS)

`scripts/pack-windows-installer.sh` produces a single redistributable
`dist/nordstjernen-${VERSION}-win64-setup.exe` (~21 MB,
LZMA-compressed). It runs `scripts/pack-windows.sh` first to populate
`dist/nordstjernen-win64/`, then feeds that directory to
[NSIS](https://nsis.sourceforge.io/) via
`data/installer/nordstjernen.nsi` (Modern UI 2).

One-time tooling install — NSIS only, the bundle deps cover the rest:

```sh
pacman -S --noconfirm --needed mingw-w64-x86_64-nsis
```

Build the installer from the MINGW64 shell:

```sh
./scripts/pack-windows-installer.sh
```

The installer is intentionally **per-user**:

- `RequestExecutionLevel user` — no UAC prompt, no Administrator
  rights. This matches `src/security.c::ns_security_refuse_root`:
  if the browser is launched with an elevated token it drops it by
  relaunching de-elevated (see below), so it never keeps running in an
  Administrator context and a Program-Files install would buy nothing.
- Default install dir: `%LOCALAPPDATA%\Programs\Nordstjernen`.
  Overridable in the wizard (Directory page) or with `/D=<path>`
  for silent installs.
- ARP entry (Add/Remove Programs) is registered under
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\Nordstjernen`
  — so it appears in the user's Settings → Apps list without
  needing admin rights.

### What the installer does

1. Extracts the entire `dist/nordstjernen-win64/` tree into
   `$INSTDIR` (preserving the root `nordstjernen.exe` plus the
   `app/` runtime directory).
2. Creates a Start Menu group `Nordstjernen` with shortcuts to
   `nordstjernen.exe` and the uninstaller.
3. Optional desktop shortcut (component is selected by default on
   the Components page; opt out in the wizard).
4. Writes the ARP keys: `DisplayName`, `DisplayVersion`,
   `Publisher`, `URLInfoAbout`, `DisplayIcon`, `InstallLocation`,
   `UninstallString`, `QuietUninstallString`, `EstimatedSize`,
   `NoModify`, `NoRepair`.
5. Drops a Modern UI uninstaller (`uninstall.exe`) that reverses
   every step: shortcuts, ARP keys, the entire `$INSTDIR`.

### Silent install / uninstall

NSIS Modern UI installers accept `/S` for silent mode. Useful for
testing and for unattended deployment:

```sh
# Silent install to a custom path
./dist/nordstjernen-1.0.17-win64-setup.exe /S /D=C:\Tools\Nordstjernen

# Silent uninstall
"%LOCALAPPDATA%\Programs\Nordstjernen\uninstall.exe" /S
```

`/D=<path>` is NSIS-special: it must be the **last** argument, no
quotes around the path, and only literal — no environment-variable
expansion. The uninstaller forks itself into `%TEMP%` so the
original `$INSTDIR` can be removed; in silent mode this returns
immediately, so script the next step with a short delay or poll
the install dir.

### Shortcuts target `nordstjernen.exe` directly

The public exe is a launcher that starts `app/nordstjernen-ui.exe`.
The UI exe self-bootstraps the runtime env (`GTK_DATA_PREFIX`,
`GTK_EXE_PREFIX`, `XDG_DATA_DIRS`, `GDK_PIXBUF_MODULE_FILE`,
`CURL_CA_BUNDLE`, `SSL_CERT_FILE`) from its own install directory
inside `ns_win32_anchor_gtk_data` (`src/gtk/appmain.c`). Earlier bundles
shipped a `nordstjernen.cmd` wrapper that did this in a batch
script, but launching a `.cmd` from Explorer flashes a console
window for the lifetime of the script. Removing the wrapper and
doing the env setup in C means clean GUI startup with no console.

### NSIS script — what to edit when

`data/installer/nordstjernen.nsi` is small (~150 lines) and parameterised
by `-D` flags from `scripts/pack-windows-installer.sh`:

- `-DVERSION=…` — propagates into the installer file name, the
  `Name` directive, `VIProductVersion`, and the ARP `DisplayVersion`.
- `-DSRCDIR=…` — the directory NSIS recursively bundles. Defaults
  to the `scripts/pack-windows.sh` output. Override to test a custom tree.
- `-DOUTFILE=…` — the produced installer path. Defaults to
  `dist/nordstjernen-${VERSION}-win64-setup.exe`.

If you change install layout, edit `Section "Nordstjernen"` and the
matching `Section "Uninstall"` together — the uninstaller must
reverse exactly what the installer wrote. NSIS provides no
generic uninstall log; it's a hand-rolled inverse.

## Package — MSIX (Microsoft Store)

`scripts/pack-msix.sh` wraps the same `dist/nordstjernen-win64/`
bundle into an unsigned `dist/nordstjernen-${VERSION}-win64.msix`
using `data/msix/AppxManifest.xml.in` and tile assets rendered from
the SVG logo. The Store policy situation (an independent engine
conflicts with policy 10.2.1), the manifest decisions, identity
overrides, local sideload testing, and the full submission
procedure are documented in `docs/windows-store.md`.

## CA bundle (what `app/etc/ssl/certs/ca-bundle.crt` is)

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

`src/net.c::ns_net_resolve_ca_bundle` resolves the file at
`ns_net_init()` time, in this order:

1. `$CURL_CA_BUNDLE` env var, if set and the path exists.
2. `$SSL_CERT_FILE` env var, same condition.
3. On Windows: `<exe_dir>/etc/ssl/certs/ca-bundle.crt`, then
   `<exe_dir>/ssl/certs/ca-bundle.crt`, then
   `<exe_dir>/ca-bundle.crt`, then `<exe_dir>/cert.pem`. In the
   redistributable bundle, `<exe_dir>` is `app/`.

If found, the resolved path is applied to every `curl_easy`
handle via `CURLOPT_CAINFO` before `curl_easy_perform`. The
resolution is per-process and cached for the lifetime of the
process. The UI exe also exports `CURL_CA_BUNDLE` /
`SSL_CERT_FILE` for the same path in `ns_win32_anchor_gtk_data`,
but those are redundant now that the binary self-resolves; they
only matter for third-party tooling that spawns from the same env.

On Linux / macOS the env-var path applies; otherwise libcurl's
own system-default resolution wins (`/etc/ssl/certs/...` and
friends). Those distros maintain CA stores out of the box,
so we don't ship a bundled copy there.

## Known Windows-specific differences

- **No Landlock sandbox.** `src/security.c` guards the Landlock
  syscalls behind `#ifdef __linux__`; on Windows the sandbox init
  is a no-op. The intentional analogue (AppContainer / Job Object)
  is not yet implemented.
- **Drops Administrator rights.** Elevation is detected via
  `CheckTokenMembership` against the builtin Administrators SID
  (`src/security.c::ns_security_refuse_root`). Instead of the Linux
  refuse-and-exit, the Windows build relaunches itself with the desktop
  shell's medium-integrity token (`CreateProcessWithTokenW`) and lets
  the elevated instance exit, so the browsing session runs unprivileged.
  It first checks the shell token is not itself elevated, so a fully
  elevated session cannot loop. When de-elevation is not possible it
  falls back to a plain-language `MessageBox` offering to quit (default)
  or run as Administrator anyway. `NS_ALLOW_ROOT=1` bypasses all of
  this, same as Linux.
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

- *Double-clicking `nordstjernen.exe` does nothing — no window, no
  error.* The bundled binary is a GUI-subsystem executable with the
  crash-restart watchdog enabled by default, so a child that dies during
  startup leaves no console output to read. Two things now make this
  diagnosable:
  - A diagnostic log is written to
    `%LOCALAPPDATA%\Nordstjernen\nordstjernen-debug.log` (override with
    `NS_LOG_FILE=<path>`, disable with `NS_NO_LOG_FILE=1`). GTK/GLib
    startup warnings and errors land there even with no console.
  - When the watchdog gives up after repeated child crashes — or can't
    spawn the browser at all — it pops a message box pointing at that
    log instead of exiting silently.

  The most common cause on a fresh Windows 11 box (especially a VM, an
  RDP session, or a machine with no GPU vendor driver) is GSK renderer
  initialisation. Set `NS_GSK_RENDERER=cairo` (or `gsk_renderer = cairo`
  in the config) to force the software renderer, which works without GL.
- *"The application was unable to start correctly (0xc000007b)"* —
  PATH is picking up a 32-bit DLL from elsewhere. Run from the
  bundle directory so Windows finds the bundled 64-bit DLLs first.
- *Running from inside the ZIP shows "Extract All"* — extract the
  whole `nordstjernen-win64` folder first. Windows' ZIP view launches
  a temporary copy of only the clicked file, without the `app/` folder
  the browser needs.
- *Icons missing / buttons blank in the header bar* —
  `app/share/icons/Adwaita` didn't make it into the bundle. Re-run
  `scripts/pack-windows.sh`.
- *TLS errors (`SSL certificate problem`)* —
  `app/etc/ssl/certs/ca-bundle.crt` isn't being picked up. Re-run
  `scripts/pack-windows.sh` and keep the root `nordstjernen.exe`
  together with its `app/` directory.
