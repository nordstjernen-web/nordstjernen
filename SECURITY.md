# Security Policy

Nordstjernen is a small, clean-room web browser written in C with
GTK 4, libcurl, lexbor, and QuickJS. This document describes the
threat model the implementation targets, what is and isn't in
scope, and how to report a security issue.

## Reporting a vulnerability

**Please do not file a public GitHub issue for security problems.**

Open a private report via GitHub's *Security → Report a
vulnerability* button on the repository:

  https://github.com/nordstjernen-web/nordstjernen/security/advisories/new

That creates a private advisory thread visible only to the
maintainers and the reporter. If you can't use GitHub advisories,
contact the maintainer through the address listed on
https://nordstjernen.org/.

Please include:

1. Affected version (`nordstjernen --version`) and OS.
2. A minimal reproducer — ideally a URL or self-contained HTML
   snippet plus the steps that trigger the issue.
3. Your assessment of the impact (information disclosure, sandbox
   escape, remote code execution, denial of service, etc.).
4. Whether the issue has been disclosed to anyone else.

We aim to acknowledge reports within **5 business days** and to
ship a fix or a written mitigation plan within **30 days** for
high-severity issues. Coordinated disclosure timelines are
negotiable — propose a date in your report.

## Supported versions

Only the latest tagged release on `main` receives security fixes.
Nordstjernen is pre-1.0 and ships from `main`; once 1.0 lands this
policy will widen to the current and previous minor series.

## Threat model

Nordstjernen treats every byte that comes off the network as
hostile. The attacker is assumed to control:

- The contents of every fetched HTML, CSS, JavaScript, image, font,
  video, audio, or PDF resource.
- DNS responses for any host the user navigates to (within the
  bounds of what libcurl + the system resolver accept).
- TLS server hellos, certificates, and HTTP responses (within the
  bounds of the system trust store).
- Any cross-origin response that the same-origin policy + CORS lets
  reach JavaScript.

The user, the local filesystem outside the sandbox's allow-list,
and the kernel are trusted. The browser is **not** a multi-user
isolation boundary on a shared host.

### In scope

| Category | What we defend against |
| --- | --- |
| Memory safety in C code | Out-of-bounds, use-after-free, integer overflow, format-string bugs in `src/`. Fix is "fix the bug." |
| Sandbox escape (Linux) | Process breaks out of the Landlock filesystem allow-list or the seccomp syscall allow-list. |
| Same-origin / cookie scoping | Cross-origin reads via DOM, `fetch`, XHR, or cookie smuggling. Cookies are partitioned per top-level site; third-party cookies are blocked by default. |
| HSTS bypass | `http://` navigation to a host on libcurl's dynamic HSTS cache. |
| Mixed-content escape | Passive or active `http://` subresources on an `https://` page. |
| CSP bypass | `script-src`, `style-src`, nonce / hash enforcement, `frame-ancestors`. |
| URL spoofing | Address-bar host display, IDN homograph confusables, javascript: paste guards. |
| Persistent local state | Cookie jar, cache, bookmarks, history written with owner-only permissions and never shared across origins. |

### Out of scope

- Vulnerabilities in third-party libraries (libcurl, GTK 4, GLib,
  Cairo, Pango, lexbor, QuickJS, Wuffs, libvpx, librsvg, uchardet,
  poppler). Report those upstream; we ship the fix once an upstream
  release exists.
- Vulnerabilities in the system trust store or in CAs we don't
  control.
- Vulnerabilities that require local code execution as the same
  user already (e.g. a malicious `~/.config/nordstjernen/`
  configuration file). The configuration is trusted input.
- Side-channel attacks against the host (Spectre-class CPU bugs,
  timing channels in cryptographic libraries). Mitigations live in
  the kernel and CPU microcode, not the browser.
- Denial of service that requires the attacker to already control
  the network path (an attacker who can MitM the connection can
  always drop packets).
- "Pop-up spam" or social-engineering vectors that don't violate
  origin isolation. We don't ship Safe Browsing.
- Anything depending on a feature we explicitly don't implement:
  WebGL, WebGPU, WebRTC, MSE / EME / DRM, service workers, push
  notifications, browser extensions, JIT, or "AI" web APIs.

## Defenses currently in place

These are the security-relevant moving parts as of the current
release. File / line citations are stable enough to follow.

### Compile-time hardening (`meson.build`)

- `-fstack-protector-strong`, `-fstack-clash-protection`.
- `-fcf-protection=full` — Intel CET + AMD IBT shadow stack and
  indirect-branch tracking.
- `-D_FORTIFY_SOURCE=3` when the compiler supports it (GCC 12+,
  Clang 17+), falling back to `-D_FORTIFY_SOURCE=2`.
- `-Wformat=2`, `-Wformat-security`, `-Wnull-dereference`,
  `-Wvla`, `-Wredundant-decls`, `-Wshadow`, and friends — treated
  as warnings (and errors under CI's `--werror`).
- PIE (`b_pie=true`), Full RELRO (`-Wl,-z,relro -Wl,-z,now`),
  non-executable stack, `-Wl,-z,separate-code`.
- Release builds add LTO + `-Wl,--gc-sections` + strip.

No JIT is present — QuickJS is a bytecode interpreter. The
W^X invariant holds across the whole address space.

### Process-startup checks (`src/security.c`)

- Refuses to run as UID 0 on Unix or as Administrator on Windows
  unless the user explicitly opts in with `ND_ALLOW_ROOT=1`.
- Applies `PR_SET_NO_NEW_PRIVS` before installing seccomp so SUID
  binaries cannot regain privileges via `execve`.

### Linux filesystem sandbox (Landlock)

The Landlock ruleset is installed early in `main` and is
default-deny. The allow-list:

- Read+execute: `/usr`, `/lib`, `/lib64`, `/etc/fonts`, system CA
  bundles under `/etc/ssl` and `/etc/pki`, `/var/cache/fontconfig`.
- Read-only: `/etc/resolv.conf`, `/etc/hosts`,
  `/etc/nsswitch.conf`, locale data, `/proc/self`, `/dev/urandom`,
  `/dev/null`, `/tmp/.X11-unix`, the Wayland socket directory.
- Read+write+create: the user's
  `$XDG_CONFIG_HOME/nordstjernen/`, `$XDG_DATA_HOME/nordstjernen/`,
  and `$XDG_CACHE_HOME/nordstjernen/` trees.
- Optional read access to `~/Downloads` (the default download
  target) and to whatever directory the user picks in the file
  dialog for the duration of an upload.

`ND_NO_SANDBOX=1` disables the Landlock layer. Use only for
debugging it.

### Linux syscall sandbox (seccomp-bpf)

Installed via libseccomp with `SCMP_ACT_ERRNO(EPERM)` as the
default action and TSYNC enabled (filter applies to every thread).
The allow-list is roughly 180 syscalls covering the libcurl,
GTK 4, GLib, fontconfig, and Cairo runtime needs. Notably absent:
`ptrace`, `process_vm_readv` / `process_vm_writev`, `perf_event_open`,
`bpf`, `kexec_load`, `keyctl`, `add_key`, `request_key`,
`init_module`, `finit_module`, `delete_module`, `pivot_root`,
`mount`, `umount2`, `swapon`, `reboot`, `kcmp`, `userfaultfd`.

`execve` and `execveat` are present but harmless thanks to
`PR_SET_NO_NEW_PRIVS` — any child inherits the same Landlock +
seccomp policy.

### Network and origin isolation (`src/net.c`)

- libcurl is built against OpenSSL and verifies peers (`SSL_VERIFY_PEER`,
  `SSL_VERIFYHOST=2`) using the system CA bundle.
- HTTP and HTTPS are the only allowed application protocols
  (`CURLOPT_PROTOCOLS_STR = "http,https"`).
- HSTS via libcurl's dynamic cache (`CURLOPT_HSTS`), persisted to
  the user data directory with owner-only permissions.
- Alt-Svc via `CURLOPT_ALTSVC`.
- Redirects capped at 10 (`CURLOPT_MAXREDIRS = 10`).
- Cookies partitioned per top-level eTLD+1; third-party cookies
  blocked by default; `SameSite=Strict` and `=Lax` honoured.
- Mixed-content blocked: an `https://` page never loads `http://`
  subresources unless the user explicitly enables it.
- An `Origin:` header is sent on cross-origin requests; CORS
  preflights are honoured; `Access-Control-Allow-Origin: *`
  responses are accepted only for credential-less requests.

### Content Security Policy (`src/csp.c`)

`script-src`, `style-src`, `connect-src`, `img-src`, `frame-src`,
`frame-ancestors`, `default-src`, plus `'nonce-...'`,
`'sha256-...'` / `'sha384-...'` / `'sha512-...'`, `'unsafe-inline'`,
`'unsafe-eval'`, `'self'`, and host-source matching. Both
`Content-Security-Policy` and `Content-Security-Policy-Report-Only`
headers are honoured; meta-equiv variants are picked up during
parse.

### JavaScript runtime (`src/js.c`, QuickJS)

QuickJS is a small, JIT-free interpreter; there is no
generated-code surface. The engine runs with conservative resource
limits (stack size, allocation hook) and is destroyed on every
navigation, so no per-tab JS state survives a same-origin load.
DOM bindings perform same-origin checks at the access boundary,
not after.

### On-disk state (`src/cache.c`, `src/bookmarks.c`, cookie jar)

- All app files are created mode `0600` (files) / `0700`
  (directories) on Unix, with an equivalent owner-only ACL on
  Windows (`set_owner_only_w32`).
- The cache honours `Cache-Control`, `ETag`, and `Last-Modified`;
  aged-out and over-cap entries are evicted lazily.
- The cache is keyed by `SHA-256(URL || partition)` and partitioned
  by top-level site; a third-party request never sees a first-party
  cache entry.

### Image and font decoders

PNG, GIF, BMP, and JPEG bytes are decoded by Wuffs — a
memory-safe transpiled-to-C decoder. SVG goes through librsvg;
fonts and the residual TIFF / ICO / WebP paths go through GTK's
GDK-Pixbuf. AVIF, if present, goes through libavif.

## Hardening expectations for contributors

If you submit a PR that touches code reachable from network input:

- Read `CLAUDE.md` first; the comment policy and the
  no-test-suite stance are real constraints.
- Bounds-check everything that comes off `nd_net_*` or
  `nd_html_*`. Treat `size_t` arithmetic as adversarial.
- Don't introduce `system()`, `popen()`, or any other shell. The
  seccomp filter would block them anyway, but they shouldn't be in
  the source.
- Prefer GLib's `g_*` helpers (`g_string_*`, `g_strdup_printf`,
  `g_file_set_contents_full`, `g_autofree`, `g_autoptr`) over
  hand-rolled C-string manipulation.
- Never log raw URLs, cookies, or page content at `g_debug` level
  without an opt-in env var. Defaults must be quiet.
- New external dependencies must be either vendored as a meson
  subproject or available as a stable system package; new
  dependencies that pull in JIT, WebGL, or networking-by-default
  daemons are rejected on principle.

## Coordinated disclosure

We follow a 90-day coordinated-disclosure window by default,
shortened to 30 days for issues that are already being exploited
in the wild and extended on request when a fix requires upstream
coordination. Public credit appears in the release notes of the
fix unless the reporter prefers to remain anonymous.

---

Last updated: 2026-05-16.
