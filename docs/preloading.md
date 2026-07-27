# Subresource preloading and fetch deduplication

How Nordstjernen decides to fetch a page's scripts and stylesheets early,
and how it guarantees each one is fetched exactly once.

Source: `src/engine.c` (the scan), `src/net.c` (the key, the coalescer,
the preload map), `src/cache.c` (the HTTP cache).

## The problem

A page's subresources are reachable from several places in the engine.
The stylesheet loader in `engine.c` and the script loader in `js.c` enter
the network through different functions, and the speculative preloader
enters through a third. Without a shared notion of "this request is
already happening", each of those paths issues its own request and the
browser fetches the same bytes two or three times.

The fix is not a cache in front of the cache. It is a single key that
describes *what a request is*, and one place that decides whether a
request with that identity is already in flight or already answered.

## The request key

`ns_net_request_key(url, top_url, method, extra_headers)` builds the
identity of a fetch:

```
GET \x1f <url> \x1f top=<site> \x1f ua=<user-agent> \x1f al=<accept-language> \x1f <header>...
```

The `top=…ua=…al=…` segment is produced by `ns_net_partition_key`, the
same helper `ns_fetch_sync_hop` uses for the HTTP cache partition, so the
two can never disagree about what counts as the same request. `<site>` is
the registrable domain of the top-level document — not the subresource's
own origin — which is what keeps one site from reading a resource another
site fetched under its own cookies.

The function returns `NULL` — meaning "do not share this fetch with
anyone" — for anything that is not a bodyless HTTP(S) GET. Requests with
a body, non-GET methods, and cancellable requests always run alone.

Two requests share bytes only if their keys are byte-identical. A script
and a stylesheet at the same URL send different `Accept` headers, so they
produce different keys, and neither the coalescer nor the preload map
will hand one of them the other's bytes.

That is necessary for `Vary: Accept` but it is **not** sufficient, and
the browser does not handle `Vary` correctly today. The HTTP cache below
these two layers keys on URL and partition only — `cache.c` does not
implement `Vary` in any form — so a variant stored for one `Accept` is
still served to a request carrying another. See Known limitations.

## Independent of the HTTP backend

Everything in this document sits above the transport. The `nghttp2`
backend selected with `-Dhttp_backend=nghttp2` replaces `ns_hop_transport`
*inside* a single hop; the request key, the coalescer, the preload map
and the cache partition are all computed above that call, so preloading
and deduplication behave identically on either backend. See
[http-backends.md](http-backends.md).

## The four layers

Every subresource fetch passes through these in order. The first one that
can answer does.

| Layer | Answers when | Lives in |
| --- | --- | --- |
| Preload map | a preload for this exact key already finished | `src/net.c` |
| In-flight coalescer | a fetch with this exact key is running right now | `src/net.c` |
| HTTP cache | a fresh, correctly partitioned entry exists on disk | `src/cache.c` |
| Network | otherwise | libcurl |

The first two share one mutex (`g_fetch_mutex`) and one decision
function, `ns_fetch_claim_locked`. That matters: it means a resource is
never momentarily absent from both. When a fetch finishes,
`ns_fetch_coalesce_deliver` stores the response in the preload map and
removes the coalescer entry *in the same critical section*, so a loader
arriving at any instant sees exactly one of them. An earlier version
populated the map from the async completion callback instead, which left
a window in which a just-finished preload was in neither structure — and
a script that landed in that window was fetched twice.

Callers do not consult the preload map themselves. `ns_net_request_async`
and `ns_net_request_blocking` do it, so `fetch_css_bytes` and
`ns_js_fetch_resource` simply ask for a URL and transparently get
preloaded bytes when they exist.

### Joining, from async and blocking callers

`ns_net_fetch_async` delegates to `ns_net_request_async`, so every
asynchronous entry point coalesces. A blocking caller that finds a fetch
already running does not start a second one: `ns_fetch_join_sync`
registers a stack-allocated waiter on the group and blocks on a
`GCond` until the leader delivers. All blocking callers run on dedicated
threads (worker threads, the download thread), never on the GTask pool,
so a blocked joiner cannot starve the pool that its leader needs.

Whichever caller creates the group is the *leader* and is responsible for
delivering to it. `ns_net_request_blocking` delivers even when the fetch
fails, so waiters are never stranded.

## The scan

`ns_engine_speculative_preload` runs from `browser_build_from_doc` once
per top-level navigation. `preload_collect` walks the parsed document and
classifies each URL by destination:

| Element | Destination | Enters the preload map |
| --- | --- | --- |
| `<script src>` | script | yes |
| `<link rel=stylesheet>` | style | yes |
| `<link rel=preload as=script\|style>`, `rel=modulepreload` | script / style | yes |
| `<link rel=preload>` with any other `as` | default | no |
| `<link rel=prefetch>` | default | no |
| `<img src>` (only when the caller asks for images; `loading=lazy` excluded) | default | no |
| `<link rel=preconnect>`, `rel=dns-prefetch` | — | connection only |

Script preloads are issued with the same `Accept` header the script
loader sends, via `ns_net_accept_headers_for`. Style preloads send the
default `Accept`, matching the stylesheet loader. That alignment is what
makes the preloaded response usable at all — the key includes the
headers, so a preload sent with the wrong `Accept` would simply never be
found.

Destinations marked "no" above are still fetched. They warm the HTTP
cache and their connections, but no map slot is reserved for them,
because nothing in the engine consumes a preloaded image or a
next-navigation prefetch. Reserving slots for resources no consumer ever
claims is what made the previous implementation fill up and quietly stop
working.

`rel=preconnect` and `rel=dns-prefetch` open a connection through
`ns_net_preconnect_async` and fetch nothing.

Image preloading is a parameter of the scan, not a setting: the only
caller, `browser_build_from_doc` in `libnordstjernen.c`, passes `include_images = FALSE`, so
`<img>` is not preloaded today. The branch is kept because the scan is
the right place for it if that changes.

## What is stored, and for how long

`ns_net_preload_expect(key)` records that a preload is coming. Only keys
that were expected are stored on completion, so the map never becomes a
general-purpose second cache. A response is stored only if it is a
`200`, has a body, carries no `no-store`, and fits the budget:

- at most `NS_PRELOAD_MAX_ENTRIES` (64) entries
- at most `NS_PRELOAD_MAX_BYTES` (16 MiB) of bodies in total

There is no expiry timer. The map is cleared at the start of every
top-level navigation, by `ns_net_preload_clear()` at the top of
`ns_engine_speculative_preload`, which is the only lifetime that makes
sense for something scoped to one page load. Entries are removed when a
loader takes them; anything unclaimed is dropped at the next navigation.

Reuse is deliberately narrower than the HTTP cache: the map holds a
response only until the page that predicted it consumes it. Anything
worth keeping longer is the HTTP cache's job, and cacheable preloads land
there through the normal `ns_cache_put` path anyway.

## Scope under process-per-tab

The preload map and the coalescer are process-global. In this edition the
engine runs inside the per-tab renderer process (`nordstjernen-renderer`
links the engine library), so each tab gets its own map and coalescer and
no state is shared between tabs even before the key's site partition is
considered. Under `--single-process` they are shared by every tab in the
one process, and the site partition in the key is then what keeps
documents from different sites apart. See
[tab-isolation.md](tab-isolation.md) and
[single-process-mode.md](single-process-mode.md).

## Concurrency limits

Preload fetches share the global fetch throttle:
`NS_MAX_CONCURRENT_FETCHES` (32) overall and `NS_MAX_FETCHES_PER_HOST`
(6) per host. A page with many subresources queues rather than opening an
unbounded number of connections.

## Configuration

| Setting | Default | Effect |
| --- | --- | --- |
| `speculative_preload` | `true` | Runs the scan. With it off nothing is preloaded, nothing enters the map, and the coalescer and HTTP cache still deduplicate normally. |
| `NS_NO_PRELOAD_SCAN` (env) | unset | Setting it to any value forces `speculative_preload` to `false`. |

## How this compares to other browsers

Chrome and Firefox both run their preload scanner *ahead of the parser*
on the raw token stream — Chrome in `HTMLPreloadScanner` on the
background parser thread, Firefox by emitting `nsHtml5SpeculativeLoad`
ops from the parser thread. Nordstjernen's scan runs after the document is
parsed, so it is a parallel-fetch pass rather than a true look-ahead. It
still overlaps the subresource fetches with cascade and script work, but
it does not start them before the parser has seen the whole document.
Moving the scan onto the token stream is the remaining latency win.

Where this implementation does follow both engines is the part that
matters for correctness: neither of them keeps a private byte store
keyed on the URL. Chrome's `ResourceFetcher` hands preloads to the same
`MemoryCache` as real loads and matches them with `Resource::CanReuse()`,
which compares URL, type, CORS mode, credentials mode, integrity and
`Accept`. Firefox keys `PreloadService` on a `PreloadHashKey` of URL,
destination, CORS mode, credentials, charset and integrity. Both scope
the entry to the `Document` and discard it when parsing ends. The key and
lifetime described above are the same idea expressed with this engine's
primitives; HTML's own `rel=preload` section calls the structure a
*preload map* and keys it on URL, destination, mode and credentials mode.

## Known limitations

- The scan runs after parsing, not ahead of the parser (above).
- The map keys on the top-level *site*, matching the HTTP cache
  partition. Two documents on the same site can therefore hand each
  other a preloaded response, exactly as they can share a cache entry.
  Chrome and Firefox scope more tightly, to the `Document`. Under
  process-per-tab this only ever applies within one tab.
- `crossorigin` and `integrity` are not part of the key. A preload issued
  without CORS and a consumer that requires it produce the same key, so
  the scan should not be extended to destinations where that distinction
  changes what the consumer is allowed to do.
- A preload that returns a non-`200` (a 404, a redirect chain ending in
  an error) is not stored, so the loader that follows fetches it again.
  This is one wasted request for a resource that was already broken.
- **`Vary` is not implemented.** The request key separates variants at
  the preload map and the coalescer, but `cache.c` keys only on URL and
  partition, so the disk cache below them ignores `Vary` entirely. A
  resource served with `Vary: Accept` and referenced as both a script
  and a stylesheet is mishandled: one variant is fetched and the cache
  serves it to both consumers. Fixing this means storing the response's
  `Vary` header with the cache entry and including the named request
  headers in the cache key.
- `preload_collect` deduplicates candidate URLs by URL alone, ignoring
  destination, so a URL referenced both as a stylesheet and as a script
  is preloaded only for whichever element the scan reaches first. The
  other consumer falls through to the cache — which is where the `Vary`
  gap above then bites.

## Measuring it

Set `NS_NET_LOG=1` to print every request the network layer issues:

```sh
NS_NET_LOG=1 ./builddir/src/gtk/nordstjernen --headless https://example.com/
```

Counting at the origin is the more trustworthy check, since it also
catches requests served from the HTTP cache. Point the browser at a local
server that logs each request path and `Accept` header, and confirm a
page issues exactly one request per subresource, with scripts carrying
the JavaScript `Accept` list and stylesheets the default.
