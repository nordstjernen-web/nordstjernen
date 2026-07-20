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
4. gzip / deflate (zlib) and brotli response decompression, streamed into the
   same budget-enforced body sink the curl path uses.
5. Cookies through the shared jar (`ns_net_cookies_for_request()` for the
   request, `ns_net_store_set_cookie()` for `Set-Cookie`), and per-hop
   timing / remote-IP metrics.

It deliberately does **not** reimplement everything curl does. A hop is
handed back to `ns_hop_transport_curl()` when it uses a **configured proxy**
(curl does the CONNECT tunnelling) or **FTP**. Everything else — the common
`http`/`https` case — goes through the nghttp2 client.

### What it does not do (yet)

- **No connection pooling or cross-request multiplexing.** Each hop opens a
  fresh TCP + TLS connection and closes it (`Connection: close`). This is the
  one behaviour that differs meaningfully at runtime: every subresource pays
  its own TLS handshake instead of reusing an open HTTP/2 connection.
- **No HTTP/3.** `libnghttp3` is only the HTTP/3 *application* layer; it
  cannot fetch anything without a QUIC transport (e.g. ngtcp2), which is a
  separate, large dependency and is out of scope here. HTTP/3-preferring
  hops therefore run over HTTP/2.
- No alt-svc, no DoH, no zstd (gzip/deflate/brotli only).

## Measured comparison

All figures were taken on Linux against real HTTP/2 origins, one fresh
connection per request (cache busted each time), median of 8 runs. Response
bodies were compared byte-for-byte.

| Aspect | curl | nghttp2 |
| --- | --- | --- |
| `pypi.org/simple/pip/` (105 KB, gzip, h2) | 54 ms | 55 ms |
| `registry.npmjs.org/left-pad` (22 KB, h2) | 68 ms | 79 ms |
| Response body bytes | — | **identical** to curl on every URL tested |
| Protocol for `https` | HTTP/2 (or /3) | HTTP/2, HTTP/1.1 fallback |
| Connection reuse across a page | yes (pool + mux) | no (one conn/hop) |
| gzip / deflate / brotli | yes | yes |
| zstd / HTTP-3 / DoH / alt-svc | yes | no |
| Proxy / FTP | yes | delegated to curl |
| Extra runtime `.so` dependencies | — | none (curl already pulls nghttp2, OpenSSL, brotli) |
| Extra code in the shell binary | — | ~24 KB |

Bodies decoded identically in every case — gzipped (105 KB), identity
(36 KB), JSON (22 KB), 404 pages, and redirect chains
(`github.com/…/raw/…` → `raw.githubusercontent.com`). Cookies, including
`HttpOnly` ones, are stored and replayed in the same Netscape jar the curl
path and the `document.cookie` API use.

For a single request the two are within measurement noise. The real-world
difference is **connection reuse**: on a page with many same-origin
subresources the curl backend multiplexes them over one HTTP/2 connection
and handshakes once, while the nghttp2 backend currently handshakes per hop
(~35–40 ms of TLS each). Adding an HTTP/2 connection pool is the obvious next
step for the nghttp2 backend and would close most of that gap.

## When to pick which

- **curl (default)** — the right choice for general use: mature,
  full-featured, HTTP/3-capable, and it reuses connections. Nothing about the
  browser's behaviour changes.
- **nghttp2** — a smaller, self-contained transport that speaks directly to
  libnghttp2 + OpenSSL with no new runtime dependency. Useful when you want
  the fetch path to be auditable in-tree end to end, or as the basis for
  future work (a connection pool, or an ngtcp2 + nghttp3 HTTP/3 path). It
  keeps libcurl only for WebSocket/SSE/AI/audio and for proxied and FTP hops.
