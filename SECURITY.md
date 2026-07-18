# Security policy

Nordstjernen is a small independent web browser. Security fixes ship from
`main`; only the latest tagged release is supported.

## Reporting
Report security issues by e-mail to:  andreas.rosdal (at) hotmail.com

Secondary backup method is to report issues to Github here: 
https://github.com/nordstjernen-web/nordstjernen/issues

Please include the version (shown in the About Nordstjernen dialog),
your OS, a minimal reproducer (URL or self-contained HTML), and your
assessment of the impact.

## Threat model

Nordstjernen treats the Internet with the outmost suspicion. The attacker controls fetched
HTML, CSS, JavaScript, images, fonts, media, and PDFs. The user, the
kernel, and the local filesystem outside the sandbox allow-list are
trusted.

**In scope**

- Memory-safety bugs in our C code: out-of-bounds reads/writes,
  use-after-free, integer overflow, format-string.
- Linux sandbox escapes — both layers: the Landlock filesystem
  allow-list and the seccomp-bpf syscall allow-list.
- Windows process-mitigation bypass — the policies set via
  `SetProcessMitigationPolicy` at startup (ASLR, strict handle
  checks, extension-point disable, image-load restrictions,
  dynamic-code prohibition, child-process block).
- Same-origin, cookie, and HTTP-cache partitioning bypass.
- HSTS, mixed-content, and CSP enforcement bypass.
- URL-bar spoofing (IDN homograph, scheme confusion, etc.).

**Out of scope**

- Bugs in third-party libraries (libcurl, GTK 4, GLib, lexbor, QuickJS,
  Wuffs, librsvg, …). Report upstream; we update when fixes ship.
- Features we deliberately don't implement: WebGPU, WebRTC,
  MSE/EME/DRM, service workers, browser extensions, JIT, "AI" web APIs.
  (WebGL *is* implemented, but is off by default and gated behind a
  per-site trust prompt — see `docs/webgl.md`.)
- CPU-level side channels (Spectre-class).
- Attacks that already require local code execution as the same user.

## Defenses

The browser renders each tab's untrusted content in its own
sandboxed renderer process (`nordstjernen-renderer`); the GTK shell
is a thin, engine-free display/input process that spawns the renderers
and blits their shared-memory framebuffers. Defenses are layered so that
a memory-safety bug in the engine is confined to a per-tab renderer
process that holds the strongest sandbox, and does not immediately yield
arbitrary code execution outside the user's data directory. (Per-origin
*site* isolation — one renderer per site rather than per tab — and a
networking/storage broker are the next steps; today the renderer still
does its own fetching and persistence. See `docs/tab-isolation.md`.)

### Compile-time hardening (`meson.build`)

PIE, full RELRO, non-executable stack, separate-code segments,
`-fstack-protector-strong`, `-fstack-clash-protection`,
`-fcf-protection=full` (Intel CET / AMD IBT), `_FORTIFY_SOURCE=3`
(`=2` fallback), `-Wformat=2 -Wformat-security`. No JIT is used or
linked — W^X holds for the whole process, so any RCE primitive has to
work without writable executable pages.

### Privilege drop (`src/security.c`)

- Linux/macOS: refuses to start as root — prints a diagnostic and exits
  before any page is loaded.
- Windows: when launched elevated it *drops* Administrator rights by
  relaunching itself with the desktop shell's medium-integrity token
  (`CreateProcessWithTokenW`) and exiting the elevated instance, so the
  browsing session runs unprivileged. It verifies the shell token is not
  itself elevated first, so a fully elevated session cannot loop. When
  de-elevation is impossible it falls back to a plain-language dialog
  offering to quit (default) or run as Administrator anyway.
- `NS_ALLOW_ROOT=1` overrides on every platform — keep the elevated
  process and skip the de-elevation and the prompt.
- Sets `PR_SET_NO_NEW_PRIVS` before installing the seccomp filter, so
  setuid binaries cannot be used to gain privileges after a compromise.

### Linux sandbox

Two independent layers, both default-deny, both installed before any HTML
is parsed. They describe the **per-tab renderer process**
(`ns_browser_sandbox`, applied by the `nordstjernen-renderer` entry point
in `src/renderer_http.c`), which parses all untrusted
content and therefore holds the strongest confinement. The thin UI shell
parses no untrusted bytes but must `fork`/`execve` the renderer processes,
so it runs under a **widened Landlock** (executable renderer directory, with
`/dev/shm` writable to cover the non-`memfd` shared-memory fallback) and
**no seccomp** — the real syscall confinement lives in the renderers. The
renderer framebuffer itself is an anonymous `memfd` passed over the control
socket, so it normally needs no `/dev/shm` name at all.

- **Landlock (filesystem).** Read-only access to system libraries
  (`/usr`, `/lib`, `/lib64`), `/etc`, the CA bundle, font caches,
  `/dev/urandom`, and the X11 / Wayland sockets. Read+write access to
  the per-user XDG config, data, and cache directories under
  `~/.config/nordstjernen`, `~/.local/share/nordstjernen`,
  `~/.cache/nordstjernen`. The rest of `$HOME` — `~/.ssh`, `~/.aws`,
  `~/.netrc`, other browsers' state, shell history — is **not**
  reachable. No directory the renderer can write to is also
  executable.
- **seccomp-bpf (syscalls).** Default-deny allow-list: the filter is
  built with `SCMP_ACT_ERRNO(EPERM)` as the default action and then
  permits only the ~266 syscalls the browser actually needs
  (`ns_seccomp_allowed_names[]` in `src/security.c`); every other
  syscall returns `EPERM`. `execve` / `execveat` are not on the list, so
  a compromised renderer cannot pivot to another interpreter or binary
  even if Landlock would have allowed reading it. `ptrace`, `bpf`,
  `keyctl`, `mount`, `unshare`, `userfaultfd`, the `io_uring_*` family,
  `perf_event_open`, `kexec_load`, and the module syscalls are likewise
  absent from the allow-list. TSYNC propagates the filter to every
  thread.
- **Media launcher.** Nordstjernen ships no audio/video codecs; playback
  is handed off to an external player. The seccomp-confined renderer has
  `execve` blocked, so it cannot launch anything: when the user clicks an
  `<audio>`/`<video>` element the renderer only *resolves* the media URL
  (`ns_browser_media_at`) and hands it to the UI shell over IPC. The
  shell — which is not seccomp-confined, because it must `execve` the
  renderer processes — validates the scheme (`http`/`https`/`ftp`/`rtsp`/
  `rtmp`, or `file://` / an absolute path), rejecting anything with a
  leading `-` or control characters, then picks a player from a fixed
  allow-list (`mpv`, `vlc`, `celluloid`, `totem`, `mplayer`, `ffplay`)
  **itself** and `execve`s it with the URL as the single argument
  (`ns_media_try_launch` in `src/media.c`). A compromised renderer can
  therefore at most ask a known media player to open a scheme-checked URL
  — it cannot choose the binary, inject extra arguments, or `execve`
  anything itself. (A pre-sandbox broker — `ns_media_broker_start` — is
  retained for callers that seccomp-confine the *launching* process, such
  as the embedding library; the default shell uses the direct path.)

Both layers can be disabled for debugging with `NS_NO_SANDBOX=1` /
`NS_NO_SECCOMP=1`. Don't use those in normal operation.

### macOS sandbox

macOS has no Landlock or seccomp, but it ships the **Seatbelt** sandbox
(`sandbox_init(3)`, `<sandbox.h>`) — a per-process, voluntary, post-launch
confinement applied from an inline Sandbox Profile Language (SBPL) policy.
This is the same mechanism Chromium and Firefox use for their macOS renderer
sandboxes. Nordstjernen applies it from the `__APPLE__` arm of
`ns_security_sandbox_init` (`src/security.c`), to **both** the per-tab
`nordstjernen-renderer` and the UI shell, before any HTML is parsed.

- **Filesystem write-confinement.** The profile is `(allow default)` then
  `(deny file-write*)` then a re-allow of the same write set the Linux
  Landlock layer permits: the per-user `~/.config/nordstjernen`,
  `~/.local/share/nordstjernen`, `~/.cache/nordstjernen`, the GLib runtime
  dir, the user's Downloads directory, the system temp roots
  (`/private/var/folders`, `/private/tmp`, `/tmp`) and `/dev`. The rest of
  `$HOME` — `~/.ssh`, `~/.aws`, other browsers' state, shell history, the
  user's documents — is **not writable**, so a compromised renderer cannot
  tamper with files, drop persistence, or modify the user's data. Writable
  directories added at runtime (`ns_security_add_writable_dir`) are folded in
  the same way.
- **What it does *not* cover.** Unlike the Linux pairing, there is no
  syscall-level filter (no seccomp analogue is applied), and reads, network,
  and `exec` are left to `(allow default)` — the renderer does its own
  networking, so a blanket network deny is not possible there. This is a
  filesystem-integrity boundary, narrower than the Linux renderer's
  read+syscall confinement; it is the macOS half of the same intent, not a
  full equivalent.
- **Caveats.** SBPL is undocumented and varies across macOS releases, and
  `sandbox_init` is marked deprecated (since 10.7) yet remains the API every
  major browser relies on; Apple keeps it working. The call **fails open** —
  if the profile is rejected the process logs a warning and continues
  unconfined rather than refusing to start. Disable for debugging with
  `NS_NO_SANDBOX=1`.

The App Sandbox / entitlements container is deliberately **not** used: it
forbids `fork`/`execve` of sibling executables, which is exactly how each
tab's renderer is spawned, so adopting it would require re-architecting the
helpers as XPC services (see [`docs/macOS.md`](docs/macOS.md)).

### Windows process mitigations

Windows has no direct Landlock or seccomp-bpf equivalent that a
user-space GTK process can apply to itself. Instead the browser
hardens itself at startup via `SetProcessMitigationPolicy`, called
from `ns_security_win32_mitigations_init` in `src/security.c`
**before** any DLL we don't statically link is touched. Six
policies, all best-effort (an unsupported policy on an older
Windows just returns `FALSE` and is skipped); the untrusted
renderer gets all six, the GUI shell gets the first five (it must
spawn renderer subprocesses):

- **ASLR** (`ProcessASLRPolicy`, flags `0x0F`) — force relocate
  images, force bottom-up randomization, high-entropy 64-bit
  layout, disallow stripped images. Belt-and-braces on top of the
  PE header's `IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE`.
- **StrictHandleCheck** (`ProcessStrictHandleCheckPolicy`, flags
  `0x03`) — raise an exception on invalid handle use and lock the
  setting permanently. Catches double-close, UAF-of-handle bugs.
- **DisableDynamicCode** (`ProcessDynamicCodePolicy`, flags
  `0x01`) — refuse `VirtualAlloc`/`VirtualProtect` with
  `PAGE_EXECUTE_*`. Pairs with the no-JIT QuickJS guarantee: the
  process has no legitimate need for writable-executable memory,
  so any RCE that depends on allocating one is denied at the
  kernel boundary.
- **DisableExtensionPoints**
  (`ProcessExtensionPointDisablePolicy`, flags `0x01`) — block
  `AppInit_DLLs`, WinSock Layered Service Providers, Image File
  Execution Options debuggers, and a few other legacy injection
  vectors that load DLLs into every process on the box.
- **ImageLoad restrictions** (`ProcessImageLoadPolicy`, flags
  `0x07`) — `NoRemoteImages` (no DLL loads from UNC/mapped
  network drives), `NoLowMandatoryLabelImages` (no DLL loads from
  Low-IL filesystem locations), `PreferSystem32Images` (resolve
  ambiguous DLL names against system32 first). Mitigates DLL
  planting and search-order hijacks.
- **ChildProcess block** (`ProcessChildProcessPolicy`, flags
  `0x01`) — no `CreateProcess` from this process, so a compromised
  renderer cannot directly pivot to `cmd.exe` / `powershell` / another
  binary. Applied **only to the untrusted renderer** (via
  `ns_browser_sandbox`); the process-per-tab GUI shell legitimately
  spawns renderer subprocesses and so passes `allow_child_processes`
  to skip this one policy while keeping the other five. Audio/video
  handoff goes through `ShellExecuteW`, which the shell performs
  out-of-process (not as our child) and only after the URL passes the
  same remote-scheme check used elsewhere
  (`http`/`https`/`ftp`/`rtsp`/`rtmp`; `file://` and local paths are
  refused on Windows so a media link can never launch a local
  executable).

There is no per-path filesystem sandbox; Windows AppContainer
would provide one but requires a manifest and code-signing
integration we don't have yet. The closest equivalents — Low
Integrity Level drop and AppContainer — are tracked as future
work.

Plus `ns_security_refuse_root`: elevation is detected with
`CheckTokenMembership` against the Built-in Administrators SID.
Unlike the Linux path it does not merely refuse — it relaunches the
browser under the desktop shell's token (`CreateProcessWithTokenW`)
and exits the elevated instance, so the session ends up unprivileged.
A `MessageBox` with a run-anyway option and a `stderr` diagnostic are
the fallback when de-elevation cannot be done; `NS_ALLOW_ROOT=1` skips
both.

The whole mitigation suite can be disabled for debugging with
`NS_NO_WIN32_MITIGATIONS=1`. Don't use that in normal operation.

### Network

libcurl drives every fetch with TLS verification enabled
(`CURLOPT_SSL_VERIFYPEER=1`, `CURLOPT_SSL_VERIFYHOST=2`), `http,https`
as the only allowed protocols, redirects clamped to HTTPS once the
initial scheme is HTTPS, max ten redirects, an explicit response-size
cap, and `CURLOPT_NOSIGNAL`. HSTS state is loaded and persisted via
`CURLOPT_HSTS`; Alt-Svc is honoured. Mixed-content sub-resources
(http inside an https document) are blocked.

### Origin isolation

- Cookies and the HTTP cache are partitioned per top-level **site**
  (scheme + registrable domain + port), where the registrable domain
  comes from the Public Suffix List via libpsl. All subdomains within
  the same registrable domain share one cookie jar and one cache
  partition; everything else is isolated. Third-party cookies are
  blocked by default.
- CSP (`default-src`, `script-src`, `style-src`, `img-src`,
  `media-src`, `connect-src`, `font-src`, `frame-src`,
  `frame-ancestors`) is parsed and enforced for both inline and
  external resources, including nonce and hash matches. Host source
  expressions match scheme, host (with `*.` wildcard), port (defaulting
  to the scheme's default), and path (left-anchored if the source ends
  in `/`, exact otherwise). `*` follows CSP3 semantics — it matches
  network schemes only, never `data:`, `blob:`, `filesystem:`, or
  `javascript:`.
- Subresource Integrity (`integrity="sha256-…"` / `sha384-` / `sha512-`)
  is verified against the response body before scripts or stylesheets
  are applied.
- IDN labels are accepted for display only under a Unicode TR-39
  "Highly Restricted"–style profile: each label must be either pure
  ASCII, a single non-Latin script, or one of the three standard CJK
  combinations (Japanese / Traditional Chinese / Korean). Anything
  else is shown as punycode in the URL bar, defeating most
  Latin-look-alike homograph attacks.

### On-disk state

Config, cookies, cache, HSTS, Alt-Svc, and bookmarks live under the
XDG dirs above with owner-only permissions (`0700` directories, `0600`
files on Unix; ACL-tightened on Windows). The HTTP cache is keyed on
`SHA-256(URL || partition)`, so cache filenames never embed
attacker-controlled bytes and no path-traversal is possible.

### Parsers

- HTML is parsed exclusively by [lexbor](https://github.com/lexbor/lexbor);
  there is no hand-rolled HTML tokenizer.
- URL parsing routes through lexbor's WHATWG URL module.
- PNG, GIF, BMP, and JPEG bytes are decoded by
  [Wuffs](https://github.com/google/wuffs) (memory-safe,
  transpiled-to-C). GdkPixbuf and librsvg handle the remaining formats
  inside the same sandbox.
- Charset sniffing is delegated to uchardet, not hand-rolled.
- The engine's own parsers bound attacker-controlled nesting and sizes.
  The recursive CSS parsers — selectors, `@supports`, `@media` queries,
  `var()` fallbacks, and `color-mix()` — all carry depth caps, the
  background-layer list is torn down iteratively, and layout's box-tree
  walkers stop at a fixed depth, so a pathologically nested stylesheet or
  DOM cannot exhaust the stack. Sizes from untrusted sources — decoded
  image dimensions and the renderer's `X-W`/`X-H`/`X-Stride` reply
  headers — are clamped before any `width × height`/`stride × height`
  multiplication, so a crafted dimension cannot integer-overflow a
  bounds check or allocation. `filter: blur()` likewise clamps its
  radius before building the convolution window.

### JavaScript

JavaScript runs in [QuickJS](https://github.com/quickjs-ng/quickjs), an
interpreter — no JIT, no machine-code generation. The DOM/JS bridge
invalidates opaque pointers on node free and re-validates on every
call, so DOM mutation cannot dangle a JS-held handle.

Each tab has its own QuickJS runtime and context; tabs do not share JS
state. Within a single tab, navigating across origins (e.g. from
`news.example.com` to `evil.com`) tears down the runtime and starts a
fresh one, so attacker-controlled globals (`window.foo = secret;`),
prototype pollution, leftover module state, and any other in-memory
JS residue from the previous origin cannot reach the new origin's
scripts. Same-origin navigation reuses the existing runtime so
sessionStorage and history work as expected.

Iframes are rendered. An `<iframe src>` is fetched through the same
hardened network pipeline as any other resource (TLS verification,
`http`/`https` only, mixed-content blocking, redirect clamp, response-size
cap, CSP `frame-src`); `srcdoc` is parsed inline. The content document is
parsed by lexbor and laid out in place.

A loaded frame gets a JavaScript realm, but **within the parent tab's
single QuickJS runtime** — there is no separate runtime, context, or OS
process per frame. The realm is a synthetic scope: the frame sees its own
`window`, `document`, `location`, and `history`, and `top`/`parent`/`self`/
`frames` are redirected to the frame itself rather than exposing the parent's
real global. This is a best-effort JavaScript-level boundary for ordinary
content, **not** a hard security boundary the way per-tab runtimes or the
cross-origin top-level navigation teardown (above) are: a memory-safety bug
or a Proxy escape in one frame is not contained from the rest of the
document's origin. Treat frame isolation as defence-in-depth, not as an
origin sandbox.

The `sandbox` attribute is parsed and enforced, with nested frames
inheriting the intersection of their ancestors' sandboxes:

- No `allow-scripts` (or no `sandbox` allowing it) blocks the frame's
  scripts from running at all.
- No `allow-same-origin` makes the frame opaque-origin: `localStorage`
  and `sessionStorage` throw `SecurityError`, and `document.cookie` reads
  empty and ignores writes.
- No `allow-forms` blocks form submission; no `allow-modals` neutralises
  `alert`/`confirm`/`prompt`/`print`; no `allow-popups` makes
  `window.open` return `null`.

A plain `<iframe>` with no `sandbox` attribute runs its scripts. Cross-
document `postMessage` between a frame and its parent is currently limited
(see Known gaps).

### Cookies and the `document.cookie` surface

Network cookies live in libcurl's per-site cookie jar on disk. They
are parsed, scoped, and re-sent by libcurl, honouring `HttpOnly`,
`Secure`, `SameSite`, `Path`, `Domain`, and expiry. At navigation the
non-`HttpOnly` cookies for the document's origin are read back out of
the jar (`ns_net_cookies_for_js`) and seeded into `document.cookie`,
and the `document.cookie` setter writes back into that same per-site
jar (`ns_net_cookie_store_from_js`) — so a cookie set from JS is sent
on the next request, and a cookie set over the network is visible to a
later `document.cookie` read. `HttpOnly` cookies are written by libcurl
with a `#HttpOnly_` line prefix that the JS read path skips, so they
stay invisible to script.

The `document.cookie` setter:

- Caps input length at 4 KiB.
- Requires a non-empty `name`.
- Maintains a per-tab in-memory mirror for synchronous read-back, then
  persists to the network jar.
- Parses attributes after the first `;`. `Max-Age` (seconds) and
  `Expires` (HTTP-date, via `curl_getdate`) set the jar expiry;
  `Max-Age<=0` or a past `Expires` deletes the named cookie. `Secure`
  is rejected outright from non-HTTPS origins. `Domain` is range-checked
  against the document host before it widens scope; absent, the cookie
  is stored host-only. `Path` defaults to `/`.

## Known gaps

- **Iframe isolation is JS-level, not a runtime or process boundary.**
  A loaded frame shares the parent tab's QuickJS runtime and global
  prototypes; its separate `window`/`document`/`location` and redirected
  `top`/`parent` are a synthetic scope, not a true cross-origin sandbox.
  Cross-document `postMessage` is correspondingly limited. A
  per-origin/per-frame runtime would close this and is tracked as future
  work; until then, do not rely on a cross-origin frame being contained
  from the embedding origin.
- **No per-path filesystem sandbox on Windows.** The mitigation
  suite restricts the *process* (no remote DLL loads, no dynamic
  code, no child processes, etc.) but does not allow-list the
  files the renderer can read or write the way Landlock does on
  Linux. AppContainer or Low-Integrity-Level drop would close
  this; both require additional integration work (manifest /
  capability declarations / re-routed config paths) and are
  tracked as future work.
- **`document.cookie` writes to the jar without file locking.** A
  JS cookie write and a concurrent libcurl jar flush from an
  in-flight transfer are not serialised against each other, so a
  write can occasionally be lost to a racing flush. A shared,
  locked cookie store is the long-term fix; in practice script
  cookie writes happen between transfers, so the window is small.
- **`HttpOnly` name collisions from JS are not rejected.** Setting
  `document.cookie` with the same name as an existing `HttpOnly`
  cookie adds a second entry rather than being refused; the
  `HttpOnly` line is preserved untouched.
