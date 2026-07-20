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

`src/net_http2.c` (~1090 lines) implements the same `ns_hop_transport()`
seam from scratch for one hop:

1. `getaddrinfo` for DNS, then a non-blocking `connect()` with a deadline and
   cancellation polling.
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
6. **A connection pool.** HTTP/2 connections are kept alive and reused,
   keyed by `scheme://host:port`: a hop checks a live connection out of the
   pool (health-probed on checkout, GOAWAY-aware, one active request at a
   time, capped at 8 per origin / 100 reuses / 60 s idle) and returns it, so
   reused hops skip DNS, connect and the TLS handshake entirely. Cold
   connections also **resume TLS** from a per-host `SSL_SESSION` cache, and
   server push is disabled so a pooled connection never receives an
   unsolicited stream.

It deliberately does **not** reimplement everything curl does. A hop is
handed back to `ns_hop_transport_curl()` when it uses a **configured proxy**
(curl does the CONNECT tunnelling) or **FTP**. Everything else — the common
`http`/`https` case — goes through the nghttp2 client.

### What it does not do

- **No HTTP/3.** `libnghttp3` is only the HTTP/3 *application* layer; it
  cannot fetch anything without a QUIC transport (e.g. ngtcp2), which is a
  separate, large dependency and is out of scope here. HTTP/3-preferring
  hops therefore run over HTTP/2.
- **No single-connection multiplexing.** The pool reuses connections but runs
  one request per connection at a time (like an HTTP/1.1 browser's 6
  connections per host), rather than fanning many concurrent streams over one
  connection the way curl's multi handle does. Connection *reuse* is captured;
  peak *multiplexing* is not.
- No alt-svc, no DoH, no ECH.

## Measured comparison

All figures were taken on Linux against real HTTP/2 origins, cache busted,
bodies compared byte-for-byte.

| Aspect | curl | nghttp2 |
| --- | --- | --- |
| `pypi.org/simple/pip/` (105 KB, gzip, h2) | 54 ms | 55 ms |
| `registry.npmjs.org/left-pad` (22 KB, h2) | 68 ms | 79 ms |
| Response body bytes | — | **identical** to curl on every URL tested |
| Protocol for `https` | HTTP/2 (or /3) | HTTP/2, HTTP/1.1 fallback |
| Connection reuse across a page | yes (pool + mux) | yes (pool, per origin) |
| Single-connection multiplexing | yes | no (one req/conn) |
| gzip / deflate / brotli / zstd | yes | yes |
| TLS session resumption | yes | yes |
| HTTP-3 / DoH / alt-svc | yes | no |
| Proxy / FTP | yes | delegated to curl |
| Extra runtime `.so` dependencies | — | none (curl already pulls nghttp2, OpenSSL, brotli, zstd) |
| Extra code in the shell binary | — | ~24 KB |

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

Across fifteen sequential same-origin fetches, fourteen reuse a live
connection (0 ms DNS/connect/TLS); fifteen concurrent fetches open parallel
connections, thread-safely, and reuse them on later waves. This is the same
class of win curl gets from its shared multi handle — the residual gap is
that curl multiplexes many streams over *one* connection whereas this pool
uses a few connections per origin, reused.

## When to pick which

- **curl (default)** — the right choice for general use: mature,
  full-featured, HTTP/3-capable, and it multiplexes over a single connection.
  Nothing about the browser's behaviour changes.
- **nghttp2** — a smaller, self-contained transport that speaks directly to
  libnghttp2 + OpenSSL with no new runtime dependency, now with connection
  pooling, TLS resumption and gzip/deflate/brotli/zstd. Useful when you want
  the fetch path auditable in-tree end to end. It keeps libcurl only for
  WebSocket/SSE/AI/audio and for proxied and FTP hops; the main things it
  still lacks versus curl are HTTP/3 and single-connection multiplexing.
