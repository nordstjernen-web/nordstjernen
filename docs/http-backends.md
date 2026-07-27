# HTTP client backends: curl vs nghttp2

Nordstjernen can perform its page and subresource fetches through one of
two HTTP client backends, chosen at build time:

```sh
meson setup builddir                          # default: curl
meson setup builddir -Dhttp_backend=curl      # explicit curl
meson setup builddir -Dhttp_backend=nghttp2   # in-tree nghttp2 client
```

Selecting `nghttp2` additionally needs the libnghttp2 and brotli-decoder
development packages (Debian/Ubuntu `libnghttp2-dev libbrotli-dev`,
Fedora/RHEL `libnghttp2-devel brotli-devel`); OpenSSL and zlib are already
required by the default build. brotli is optional — without it the nghttp2
backend simply advertises `gzip, deflate`.

libnghttp2 1.60.0 introduced `nghttp2_ssize` and a set of `…2` entry points
that replace the deprecated `ssize_t`-based ones. `src/net_http2.c` defines
`NGHTTP2_NO_SSIZE_T` on those versions so the deprecated declarations are
compiled out of the header altogether and only the `…2` API is reachable; on
older libnghttp2 it falls back to the original names. Both paths build warning
free, so the backend spans the versions distributions actually ship.

**HTTP/3 is an auto-detected sub-feature of the nghttp2 backend on
non-Windows systems.** When the QUIC stack — ngtcp2, its gnutls crypto
binding, and libnghttp3 — is present alongside gnutls, the nghttp2 build
also speaks HTTP/3 over QUIC; when it is absent the same source compiles and
links to an HTTP/2-only client. Windows uses the in-tree HTTP/2 client over
Winsock and leaves HTTP/3 to the curl backend. On
Debian/Ubuntu install `libngtcp2-dev libngtcp2-crypto-gnutls-dev
libnghttp3-dev libgnutls28-dev`. gnutls is the QUIC-capable TLS stack here
because system OpenSSL 3.0 exposes no QUIC API (ngtcp2's OpenSSL binding
needs OpenSSL 3.5+ or quictls); the HTTP/2 path keeps using OpenSSL. Nothing
is vendored, and a machine without the QUIC packages carries no ngtcp2,
nghttp3 or gnutls symbol or dependency — exactly as before.

Both backends plug into the same seam. `src/net.c` owns everything above a
single HTTP hop — the redirect loop, HSTS upgrades, the referer policy, the
HTTP cache, cookie partitioning, per-origin throttling and the async thread
pool — and delegates one transfer to `ns_hop_transport()`. Swapping the
backend swaps only *how one request goes over the wire*, not the browser
policy around it, so the two produce the same responses through the same
high-level logic.

`libcurl` is a hard dependency in **both** configurations. WebSocket
(`src/ws.c`), Server-Sent Events (`src/eventsource.c`), the AI chat client
(`src/ai.c`) and the audio helper (`src/audio/main.c`) call it directly, and
the nghttp2 backend deliberately hands a few request kinds back to it (see
below). The option therefore changes which library performs the
document/subresource transfer, not what is linked.

## The curl backend (default)

`ns_hop_transport_curl()` in `src/net.c` drives a `libcurl` easy handle on
the shared multi-handle event thread (`src/net.c`, `ns_net_multi_loop`).
This is the mature, full-featured path:

- HTTP/1.0, HTTP/1.1, **HTTP/2**, and **HTTP/3** (when the linked libcurl was
  built with it), negotiated automatically.
- A process-wide `CURLSH` share plus a multi handle with
  `CURLPIPE_MULTIPLEX`: **connection pooling, HTTP/2 multiplexing across
  requests, TLS-session reuse and a shared DNS cache**. A page with fifty
  subresources reuses a handful of connections and pays the TLS handshake
  once per origin.
- gzip / deflate / brotli / zstd decompression, alt-svc, DoH, every proxy
  type curl supports, FTP, and ECH — all for free.

On most systems libcurl is itself built on **libnghttp2** for HTTP/2 framing
and OpenSSL for TLS, so the default backend already links those libraries;
the curl path just lets curl orchestrate them.

## The nghttp2 backend (`-Dhttp_backend=nghttp2`)

`src/net_http2.c` (~2300 lines) implements the same `ns_hop_transport()`
seam from scratch for one hop:

1. `getaddrinfo` for DNS, then a non-blocking `connect()` with a deadline and
   cancellation polling through POSIX sockets or Winsock.
2. OpenSSL TLS with SNI, ALPN advertising `h2` and `http/1.1`, the same
   cipher list and EC / MLKEM curve list as the curl path, and hostname +
   chain verification against the resolved CA bundle
   (`ns_net_ca_bundle_path()`). The opt-in insecure-certificate retry
   (`tls_allow_insecure_override`) is honoured.
3. **libnghttp2** for HTTP/2 when ALPN selects `h2`; otherwise a compact
   HTTP/1.1 client (Content-Length, chunked, and read-to-close), which also
   serves plaintext `http://`.
4. gzip / deflate (zlib), brotli and zstd response decompression, streamed
   into the same budget-enforced body sink the curl path uses.
5. Cookies through the shared jar (`ns_net_cookies_for_request()` for the
   request, `ns_net_store_set_cookie()` for `Set-Cookie`), and per-hop
   timing / remote-IP metrics.
6. **A connection pool with multiplexing.** HTTP/2 connections are kept
   alive and reused, keyed by `scheme://host:port`. Each pooled connection is
   driven by its own I/O thread; worker threads submit a request and block on
   a condvar until their stream completes, so **many concurrent requests to
   the same origin multiplex over a single connection with one handshake** —
   the way curl's multi handle does. Connection creation is serialized per
   origin (a "connecting" gate), so a page's subresource burst shares one
   connection instead of opening one per request; reused hops skip DNS,
   connect and the TLS handshake entirely. Cold connections also **resume
   TLS** from a per-host `SSL_SESSION` cache, server push is disabled, and the
   I/O thread RST_STREAMs a request whose deadline passes or whose
   `GCancellable` trips (detaching the stream from the session first, so a
   frame still in flight cannot reach the released request). Both the stream
   and the **connection** flow-control windows are raised to 8 MB — the
   connection window is not covered by `SETTINGS_INITIAL_WINDOW_SIZE` and at
   its 65535-byte default would cap aggregate throughput at one window per
   round trip. A connection idle for a minute stops its I/O thread and is
   dropped from the pool, as is one past the reuse ceiling or the per-origin
   cap. A stream the server closes with `REFUSED_STREAM` is retried on a
   fresh connection.
7. **HTTP/3 over QUIC** on non-Windows systems, when the QUIC stack is compiled in
   (`NS_HTTP_HAVE_HTTP3`). A hop upgrades to HTTP/3 when the origin has
   advertised it: an `Alt-Svc: h3=…` response header (over HTTP/2 or
   HTTP/1.1) caches the origin, and the next hop to it opens a QUIC
   connection — ngtcp2 for the transport, gnutls for the TLS 1.3 handshake
   (ALPN `h3`), and libnghttp3 for the HTTP/3 framing — connecting to the
   origin's authority port. Response headers and body flow through the same
   status/header/body sinks and decompression the HTTP/2 path uses, so a
   fetch is byte-identical whichever version carried it. If the QUIC
   connection cannot be established the hop **falls back to HTTP/2**, so
   HTTP/3 never makes a request fail that HTTP/2 would have served.
   `NS_FORCE_HTTP3=1` forces the first hop onto HTTP/3 (for testing without
   waiting for an alt-svc round trip).

It deliberately does **not** reimplement everything curl does. A hop is
handed back to `ns_hop_transport_curl()` when it uses a **configured proxy**
(curl does the CONNECT tunnelling) or **FTP**. Everything else — the common
`http`/`https` case — goes through the nghttp2 client.

### What it does not do

- **HTTP/3 does not pool or multiplex.** Where the HTTP/2 path keeps a
  connection per origin and runs every request over it, `ns_h3_perform()`
  opens a QUIC connection, issues one request, and tears it down. A page with
  many subresources on an alt-svc origin therefore pays a QUIC handshake per
  subresource instead of one for the page. HTTP/3 still wins on a cold
  single fetch (1-RTT against TCP+TLS's three), and the fetches run
  concurrently, but a subresource-heavy page is better served by HTTP/2 until
  the QUIC path grows the same pooled, multiplexed I/O-thread architecture.
- **HTTP/3 needs a non-Windows host and the QUIC stack at build time.** On
  Windows the in-tree backend is HTTP/2-only. Without ngtcp2 + nghttp3 +
  gnutls the nghttp2 backend is HTTP/2-only and HTTP/3-preferring hops run
  over HTTP/2. When the stack is present, HTTP/3 follows an origin's alt-svc
  advertisement to the origin's own port (the near-universal `h3=":443"`
  deployment); an alt-svc that relocates HTTP/3 to a *different* port is not
  followed.
- No DoH, no ECH.

## Measured comparison

All figures were taken on Linux against real HTTP/2 origins, cache busted,
bodies compared byte-for-byte.

| Aspect | curl | nghttp2 |
| --- | --- | --- |
| `pypi.org/simple/pip/` (105 KB, gzip, h2) | 54 ms | 55 ms |
| `registry.npmjs.org/left-pad` (22 KB, h2) | 68 ms | 79 ms |
| Response body bytes | — | **identical** to curl on every URL tested |
| Protocol for `https` | HTTP/2 (or /3 if libcurl built with it) | HTTP/2, HTTP/1.1 fallback, **HTTP/3** on non-Windows hosts when QUIC stack present |
| Connection reuse across a page | yes | yes |
| Single-connection multiplexing | yes | yes |
| gzip / deflate / brotli / zstd | yes | yes |
| TLS session resumption | yes | yes (HTTP/2) |
| HTTP/3 (QUIC) | only if the linked libcurl was built with it | yes on non-Windows hosts when ngtcp2 + nghttp3 + gnutls are present |
| alt-svc HTTP/3 upgrade | yes | yes |
| DoH / ECH | yes | no |
| Proxy / FTP | yes | delegated to curl |
| Extra runtime `.so` dependencies | — | none for HTTP/2 (curl already pulls nghttp2, OpenSSL, brotli, zstd); HTTP/3 adds libngtcp2 + libnghttp3 + gnutls |
| Extra code in the shell binary | — | ~30 KB (HTTP/2) + ~25 KB (HTTP/3) |

Bodies decoded identically in every case — gzipped (105 KB), identity
(2.2 MB), JSON (22 KB), 404 pages, and redirect chains
(`github.com/…/raw/…` → `raw.githubusercontent.com`). Cookies, including
`HttpOnly` ones, are stored and replayed in the same Netscape jar the curl
path and the `document.cookie` API use.

**Connection reuse** — five distinct fetches to one origin, in one process:

| | total | per fetch | handshakes |
| --- | --- | --- | --- |
| before the pool | 337 ms | 67 ms | 5 (every hop ~31 ms TLS) |
| with the pool | 218 ms | 44 ms | 1 (hops 2–5 reuse: 0 ms) |

**Multiplexing** — concurrent fetches to one origin share a single
connection: 6-way and 20-way concurrent bursts each complete over **one
connection with one TLS handshake** (the rest reuse it, 0 ms
DNS/connect/TLS), byte-identical to curl, with no errors across repeated
runs and clean under valgrind. Different origins keep separate multiplexed
connections. This matches curl's multi-handle behaviour.

**HTTP/3** — against a QUIC/HTTP/3 origin the in-tree client completes the
ngtcp2 handshake (gnutls TLS 1.3, ALPN `h3`), issues the request through
libnghttp3, and reports `HTTP/3` with the response body **byte-identical**
to the HTTP/2 and curl transfers of the same resource. The whole path —
QUIC transport, the HTTP/3 control and QPACK streams, and the request/
response streams — runs on one UDP socket driven by an ngtcp2 event loop,
with the same header and body sinks the HTTP/2 path feeds. Note that the
distro libcurl the default backend links (e.g. Ubuntu's `libcurl 8.5.0`) is
commonly built **without** HTTP/3, so a `-Dhttp_backend=nghttp2` build with
the QUIC stack can negotiate HTTP/3 where the stock curl backend cannot.

## When to pick which

- **curl (default)** — the right choice for general use: mature,
  full-featured, DoH/ECH-capable, and HTTP/3-capable *when the linked
  libcurl was built with it*. Nothing about the browser's behaviour changes.
- **nghttp2** — a smaller, self-contained transport that speaks directly to
  libnghttp2 + OpenSSL with no new runtime dependency, with connection
  pooling, single-connection multiplexing, TLS resumption and
  gzip/deflate/brotli/zstd. With the optional QUIC stack (ngtcp2 + nghttp3 +
  gnutls) it also speaks **HTTP/3**, following an origin's alt-svc
  advertisement and falling back to HTTP/2 if QUIC can't connect. Useful
  when you want the fetch path auditable in-tree end to end. It keeps
  libcurl only for WebSocket/SSE/AI/audio and for proxied and FTP hops; the
  remaining gaps versus curl are **DoH** and **ECH**.
