# Security Policy

Nordstjernen is a small clean-room web browser. We take security
bugs seriously and ship fixes from `main`.

## Reporting a vulnerability

File a regular GitHub issue:

  https://github.com/nordstjernen-web/nordstjernen/issues

Include the affected version (`nordstjernen --version`), your OS,
a minimal reproducer (URL or self-contained HTML), and your
assessment of the impact.

## Supported versions

Only the latest tagged release on `main` receives fixes.

## Threat model

We treat every byte off the network as hostile. The attacker is
assumed to control fetched HTML, CSS, JavaScript, images, fonts,
media, and PDFs. The user, the local filesystem outside the
sandbox allow-list, and the kernel are trusted.

### In scope

- Memory-safety bugs in C code (OOB, UAF, integer overflow,
  format-string).
- Linux sandbox escapes (Landlock filesystem allow-list, seccomp
  syscall allow-list).
- Same-origin / cookie / cache partitioning bypass.
- HSTS, mixed-content, and CSP enforcement bypass.
- URL-bar spoofing.

### Out of scope

- Bugs in third-party libraries (libcurl, GTK 4, GLib, lexbor,
  QuickJS, Wuffs, librsvg, …). Report upstream; we update when a
  fix is released.
- Features we explicitly don't implement: WebGL, WebGPU, WebRTC,
  MSE / EME / DRM, service workers, browser extensions, JIT, "AI"
  web APIs.
- CPU-level side channels (Spectre-class).
- Attacks requiring local code execution as the same user.

## Defenses

- **Compile-time** (`meson.build`): PIE, full RELRO, non-exec
  stack, `-fstack-protector-strong`, `-fstack-clash-protection`,
  `-fcf-protection=full` (Intel CET / AMD IBT),
  `_FORTIFY_SOURCE=3` (with `=2` fallback), `-Wformat=2
  -Wformat-security`. No JIT — W^X holds process-wide.
- **Startup** (`src/security.c`): refuses to run as root /
  Administrator; sets `PR_SET_NO_NEW_PRIVS` before installing
  seccomp.
- **Linux sandbox**: Landlock default-deny filesystem with a small
  allow-list (XDG config / data / cache, system libs, fonts, CA
  bundle, `/dev/urandom`); seccomp-bpf default-deny with ~180
  whitelisted syscalls and TSYNC.
- **Network**: libcurl with TLS verification, `http,https` only,
  dynamic HSTS (`CURLOPT_HSTS`), Alt-Svc, max 10 redirects.
- **Origin isolation**: cookies partitioned per top-level eTLD+1;
  third-party cookies blocked by default; CSP (`script-src`,
  `style-src`, nonces, hashes); mixed-content blocked.
- **On-disk state**: owner-only permissions (`0600` files /
  `0700` dirs on Unix, equivalent ACL on Windows); cache keyed by
  `SHA-256(URL || partition)`.
- **Image decoders**: PNG / GIF / BMP / JPEG via Wuffs (memory-safe
  transpiled-to-C).
