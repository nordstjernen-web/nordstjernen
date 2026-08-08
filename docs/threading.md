# Threading model

Nordstjernen runs each tab's engine in its own **renderer process**
(`nordstjernen-renderer`); the GTK app is a separate, thin shell.
This document describes the threading model **inside one renderer
process** — that is where the DOM, CSS, layout, paint, and page
JavaScript live. The shell process is covered briefly at the end.

A renderer process is **single-main-thread**: the DOM, CSS, layout,
Cairo/Pango paint, and page JavaScript all live on its main thread (it
runs a GLib main loop and links GTK only for GDK's GL context — used by
WebGL — and the GDK-pixbuf image loaders; it creates no on-screen GTK
windows or widgets). Background threads
exist only for work that is either inherently blocking (network I/O) or
CPU-heavy and self-contained (HTML/CSS parsing, image decoding). Every
result computed off the main thread is handed back to the main thread
through a GLib main-context invocation before it touches any shared
state.

The guiding rule: **nothing outside the main thread ever touches a
`ns_node` DOM tree, a layout box, or a live `JSContext` that belongs to
the page.** Background threads operate on self-contained inputs
(response bytes, a URL, a script source) and produce self-contained
outputs (decoded pixels, a parsed stylesheet, a detached DOM document)
that are transferred by ownership.

## Threads in a renderer process

| Thread | Created in | Lifetime | Touches DOM/JS? |
|--------|-----------|----------|------------------|
| **Main** | `renderer_http.c` | whole process | yes — owns the DOM, layout, paint, page JS |
| **Fetch pool** (≤32) | `net.c`, GLib `GTask` thread pool | per request | no |
| **Network I/O** (1) | `net.c`, owns the shared `CURLM` | first fetch → shutdown | no |
| **Web Worker** (1 per `Worker`) | `js.c` | until terminated | no (own `JSContext`) |
| **WebSocket** (1 per socket) | `ws.c` | connection lifetime | no |
| **RNG warm-up** (transient) | `net.c` | one-shot at startup | no |

The shell process (`src/gtk/procview.c`) has its
own main/UI thread plus one **per-view worker thread** that runs the
synchronous `rproc_http` IPC calls (open/render/click/key/find/…) off the UI
thread; it touches no DOM or JS — only the shared-memory framebuffer and
GTK widgets.

## How the renderer main thread keeps making progress

Two mechanisms keep the renderer's main loop pumping (so timers,
fetch/XHR completions, and animation frames keep flowing, and the shell
keeps getting fresh frames):

1. **Blocking and heavy work is off-thread.** Network transfers, HTML
   parsing, CSS parsing, and image decoding never run on the main thread.
   Each request is owned by a fetch-pool thread that drives its transfer
   on a single shared `CURLM` (the network I/O thread), then posts its
   result back.

2. **Long synchronous page JS cooperatively pumps the main loop.** Page
   JavaScript necessarily runs on the main thread (it manipulates the
   DOM). To stop a tight script from wedging the renderer,
   `ns_js_interrupt_cb` (`src/js.c`) is installed as the QuickJS
   interrupt handler. While *main-thread* JS runs, every ~100 ms it
   does up to 8 non-blocking iterations of the default `GMainContext`,
   so timers, fetch completions, and animation frames are serviced
   mid-script. This pump is gated to main-thread JS only
   (`!js->worker_host`): a Web Worker has its own thread and
   `GMainContext` and must never iterate the global default context from
   off the main thread.

### Reentrancy contract of the JS pump

Iterating the main context from inside a running script (mechanism 2)
re-enters the GLib event loop while a `JS_Eval`/`JS_Call` frame is still
on the C stack. Two layers keep that safe:

- **`js->in_pump`** is set for the duration of the pump. Every JS entry
  point that the loop could trigger — timers, `requestAnimationFrame`,
  event dispatch, image-load callbacks — early-returns when `in_pump`
  is set, so reentrant *JavaScript* never runs nested inside the pump.
- **`js->dispatch_depth`** is raised around event dispatch;
  `ns_js_orphan_node` then parks freed DOM nodes in
  `orphan_nodes` and sweeps them after dispatch unwinds, so a handler
  can never free a node the outer frame still references.

The deliberately conservative consequence is that timers/rAF/events do
not fire *during* a long synchronous script until the script yields.

Page teardown does not race a live `JS_Eval` frame in the
process-per-tab model: the renderer services one IPC message at a time,
so the page is only opened or closed (`ns_browser_open` /
`ns_browser_close`) between messages — never while a script the pump is
running is still on the stack. (The shell drives close/navigation/zoom
by sending IPC messages, which the renderer handles after the current
one returns.)

## Timeouts and budgets

Every blocking or unbounded operation has a bound:

| Operation | Bound | Where |
|-----------|-------|-------|
| HTTP connect | 15 s navigation / 6 s subresource | `CURLOPT_CONNECTTIMEOUT`, `net.c` |
| HTTP transfer | 30 s default, 60 s max (`X-ND-Timeout-Seconds`) | `CURLOPT_TIMEOUT`, `net.c` |
| HTTP redirects | 10 | `CURLOPT_MAXREDIRS`, `net.c` |
| WebSocket connect | 15 s | `CURLOPT_CONNECTTIMEOUT`, `ws.c` |
| WebSocket transfer | none (long-lived); 10 ms poll cadence | `ws.c` |
| Page JS eval slice | `js_eval_budget_ms` (config default 60 s, max 60 s; 5 s no-config fallback) | `ns_js_budget_push`, `js.c` |
| Page JS hard monitor | 60 s of wall-clock per top-level entry | `NS_JS_MONITOR_LIMIT_US`, `js.c` |
| JS heap (page runtime) | `js_memory_cap_mb` (config default 2048; 2048 no-config fallback, no clamp) | `JS_SetMemoryLimit`, `js.c` |
| JS heap (worker runtime) | `js_memory_cap_mb` clamped to ≤512; 256 no-config fallback | `JS_SetMemoryLimit`, `js.c` |
| Per-origin in-flight requests | 6 | `NS_NET_MAX_PER_ORIGIN`, `net.c` |
| Total in-flight requests | 32 | `NS_MAX_CONCURRENT_FETCHES`, `net.c` |

The origin-slot wait (`ns_net_acquire_origin_slot`) uses
`g_cond_wait_until` with a 250 ms re-check so a cancelled request never
blocks a pool thread indefinitely.

## Subsystem details

### Network (`src/net.c`)

`ns_net_fetch_async` / `ns_net_request_async` build a `GTask`,
duplicate every input into a heap `ns_fetch_ctx`, and enqueue it. A
mutex-guarded throttle (`g_fetch_throttle_mutex`, `g_fetch_queue`,
`g_fetch_active`) caps concurrency at `NS_MAX_CONCURRENT_FETCHES` and
dispatches via `g_task_run_in_thread`. `ns_fetch_thread` builds a curl
easy handle (redirects, TLS, cache, and FTP handling all stay inline,
hop by hop) and drives the actual transfer through
**`ns_net_multi_perform`**, then `g_task_return_pointer` delivers the
`ns_response` to the `GTask` callback **on the main thread** (the task
was created there). On completion the active count is decremented under
the mutex and the queue is re-pumped.

**Shared multi handle and multiplexing.** A pool thread does not call
the blocking `curl_easy_perform`; it hands its easy handle to a single
**network I/O thread** that owns one process-wide `CURLM`
(`ns_net_multi_loop`) and blocks on a per-transfer `GCond` until that
handle reports `CURLMSG_DONE`. Because every concurrent transfer rides
the same multi handle — created with
`CURLMOPT_PIPELINING = CURLPIPE_MULTIPLEX` — requests to the same
HTTP/2 (or HTTP/3) origin share one connection and run as parallel
streams instead of opening a separate TCP+TLS connection each. The easy
interface multiplexes nothing; only the multi interface does, which is
the entire reason for the I/O thread. Cancellation is unchanged: the
`CURLOPT_XFERINFOFUNCTION` progress callback still fires under
`curl_multi_perform` and aborts the transfer when its `GCancellable`
trips, which surfaces as a normal `CURLMSG_DONE`. The driver state — the
incoming queue, the `easy → ns_multi_xfer` active table, and the quit
flag — is guarded by `g_multi_lock`; `curl_multi_wakeup` interrupts the
poll the instant a new handle is queued. Shutdown
(`ns_net_multi_shutdown`) sets the quit flag, wakes the loop, aborts any
still-active transfers, and joins the thread before
`curl_global_cleanup`.

To confirm the multiplexing empirically, `--debug=net` emits a per-origin
`[net conn]` line on every completed transfer carrying the negotiated
protocol, `CURLINFO_NUM_CONNECTS` for that transfer (`new=`), and the
running per-origin `reqs`/`conns` tally. A healthy HTTP/2 origin shows
many requests collapsing onto one connection (e.g. `reqs=166 conns=1`),
which is the signal that same-origin subresources share a single
connection as parallel streams rather than opening one apiece.

A headless run with `--debug=net` also prints a one-line `[net perf]`
summary at the end of the load comparing the network-active wall span to
main-thread layout time, e.g.
`fetches=167 net_span=794ms net_sum=1431ms | relayouts=1 layout=97ms`.
`net_span` is the wall-clock window from the first fetch start to the
last completion (so it accounts for fetch concurrency), `net_sum` is the
overlapping per-fetch total, and `layout` is the accumulated time inside
`ns_engine_relayout`. When `net_span` dominates `layout` the load is
network-bound — the regime the shared multi handle is meant to speed up;
when `layout` dominates, the main thread is the bottleneck and network
concurrency is not the lever to pull.

Shared state and its protection:

- **Per-origin slots** (`g_origin_slots`, `g_origin_slots_lock`,
  per-slot `GCond`) — bounds concurrent connections per origin.
- **HSTS cache** (`g_hsts_cache`, `g_hsts_lock`) — reloaded from the
  curl HSTS file under lock, mtime-checked; read by every fetch.
- **curl share** (`g_share`, `g_share_locks[]`) — DNS/TLS-session/
  connection sharing across easy handles, with per-data-class locks
  installed via `CURLOPT_SHARE`.
- **Disk + memory response cache** (`cache.c`, `g_cache_mutex`;
  `bytecode_cache.c`, `g_lock`) — fully locked; safe to call from pool threads.
- **Config-derived globals** (`g_accept_encoding`, `g_ca_bundle`,
  `g_proxy_override`, `g_has_http3`, `g_allow_file_urls`) — written
  once at init / argument-parsing time, before any fetch is issued, and
  treated as read-only thereafter.

### Web Workers (`src/js.c`, `ns_worker_*`)

A `Worker` spawns a dedicated thread with its **own `JSRuntime`,
`JSContext`, and `GMainContext`** (`ns_worker_host`). The worker thread
pushes its context as thread-default and runs its own `GMainLoop`;
timers and messages are sources on that context. The host is
reference-counted (`ref_count`) and uses atomics for cross-thread
flags (`closing`, `owner_alive`, `joined`); `host->lock` guards the
`worker_js`/`loop`/`thread` pointers.

Message passing is structured-clone over `JS_WriteObject` /
`JS_ReadObject` (no shared JS heap, transfer lists rejected):

- owner → worker: `g_main_context_invoke_full(host->context, …)` runs
  `ns_worker_deliver_worker` **on the worker thread**.
- worker → owner: `g_main_context_invoke_full(NULL, …)` runs
  `ns_worker_deliver_owner` **on the main thread**, guarded by
  `owner_alive`.

Teardown (`ns_js_free`) clears `owner_alive`, detaches owner pointers,
then `ns_worker_host_stop(host, TRUE)` sets `closing`, asks the loop to
quit, and joins. The interrupt handler observes `closing` and halts the
worker promptly.

### WebSockets (`src/ws.c`)

One thread per socket using curl `CONNECT_ONLY` plus `curl_ws_recv` /
`curl_ws_send`. The out-queue is a `GMutex`/`GCond` pair; `state`,
`exit_requested`, and `detached` are atomics. Inbound frames are
reassembled on the worker thread, then marshalled to the main thread
via `ns_ws_post` → `g_idle_add` → `ns_ws_dispatch_run`, which invokes
the JS callbacks. The `detached` flag (set when the owning JS object
goes away) makes any in-flight dispatch a no-op and is checked both at
post time and dispatch time; the dispatch holds a ref on `ns_ws` so the
struct outlives queued events.

`ns_ws_free` runs on the main thread and joins the worker, so the worker
must exit promptly. The recv/send poll loop checks `exit_requested`
every 10 ms, and the connecting handshake (`curl_easy_perform` with
`CONNECT_ONLY`) installs a transfer-info callback
(`ns_ws_handshake_progress`) that aborts the moment `exit_requested` is
set. Without it, closing a socket mid-handshake would block the GUI on
join for up to the 15 s connect timeout.

### Watchdog (`src/watchdog.c`)

Two parts (GTK; see `docs/watchdog.md`). A **supervisor process**
(`ns_watchdog_run_supervisor`) `g_spawn`s the GUI-shell child, watches it
with `g_child_watch_add`, and restarts it on crash/hang with capped burst
control plus session recovery. Inside the shell, a **hang-monitor thread**
(`ns_watchdog_hang_thread`) compares an atomic heartbeat (`g_beat`, bumped
by a 2 s shell-main-loop timeout) against a deadline of the JS eval budget
plus a 60 s floor; if the loop
stops beating it `_Exit`s with code 70 so the supervisor restarts. The
shell runs no page JS, so the loop is never legitimately blocked long.
This supervises the *shell*; per-tab engine crashes are already contained
to their renderer process.

### Debug log (`src/debuglog.c`)

The in-process event log is mutex-guarded (`g_dlog_mutex`) and is
emitted into from **any thread** — notably the fetch pool, which logs
every request from `ns_fetch_thread`. `ns_dlog_dispatch` snapshots the
subscriber list under the lock, then calls each listener **on the
emitting thread**. Listeners therefore must be thread-safe: they must
not touch the DOM directly and must not dereference an object that
another thread can free. Page `console.*` output is captured by the JS
log callback into a bounded per-page buffer, which the shell drains over
the IPC `CONSOLE` message for the DevTools console panel (see
`docs/tab-isolation.md`); it is never handed to a UI object from a worker
thread.

### SQLite storage (`cache.c`, `history.c`, `idb.c`)

Three SQLite databases back the browser, each opened `WAL` +
`synchronous=NORMAL` with a 2.5 s `busy_timeout`:

- **HTTP cache** (`cache.c`) — read and written from the **fetch pool
  threads** (`ns_fetch_thread`); all access is serialised by
  `g_cache_mutex`.
- **History** (`history.c`) — a single persistent handle guarded by
  `g_history_mutex`, touched from the main thread.
- **IndexedDB** (`idb.c`) — **main thread only**. The backend primitives
  (`__nd_idb.*`) are called synchronously by the IndexedDB polyfill,
  which fakes async with `setTimeout`. Workers do not get IndexedDB, so
  no locking is needed.

IndexedDB previously **opened and closed the SQLite file on every
operation** (`open` + full `CREATE TABLE` schema + `close` per
`get`/`put`/…), which dominated the per-op cost and was the real
main-thread stall. Connections are now **cached** in a process-global
`key → handle` table (main-thread-only, so unlocked), opened and
schema-checked once; `deleteDatabase` evicts (closes) the cached handle
before unlinking the file and its `-wal`/`-shm`. The cache key is the
cheap raw `partition\x1fname` string, so a cache hit (the common case)
costs only a hash-table lookup — the expensive on-disk path (two SHA-256
hashes plus a `mkdir`/`chmod` of the partition directory) is computed
only on a miss, when the file is actually opened. Committed data persists
without an explicit close because WAL recovers it on the next open. The
cache is bounded to `NS_IDB_MAX_OPEN` (16) connections, LRU-evicted (each
handle carries a `last_used` stamp), and each connection caps its page
cache at 512 KiB (`PRAGMA cache_size=-512`), so a long session that
touches many origins cannot accumulate unbounded file descriptors or
SQLite page-cache memory; an evicted database simply reopens on next use. This
collapses a typical IndexedDB op from a file open/close round-trip to a
single indexed query — sub-millisecond on the main thread — so a
dedicated DB worker thread would only add dispatch overhead for the
common case; it is reserved for pathological bulk operations and is not
currently warranted.

All three databases process **attacker-influenced bytes** (response
bodies, visited URLs/titles, page-controlled IndexedDB values), so each
connection is hardened immediately after open: `DEFENSIVE` mode, load
extensions disabled, `TRUSTED_SCHEMA` off, double-quoted string literals
off (`DQS_DDL`/`DQS_DML`), and `SQLITE_OPEN_NOFOLLOW` where available.
IndexedDB additionally caps each database at `max_page_count` (256 MiB)
so a page cannot fill the disk.

## Invariants for contributors

- New blocking or >~10 ms CPU work belongs on the fetch pool or the
  per-tab worker, never inline on the main thread.
- A subscriber/callback that can be invoked from a background thread
  must capture identifiers by value (e.g. a window id), never a raw
  pointer to an object the main thread may free.
- A background thread returns results by **transferring ownership**
  through `g_main_context_invoke_full` / `g_idle_add`; it must not
  write GTK widgets, the DOM, layout, or the page `JSContext`.
- Every blocking call gets a timeout; every wait loop gets a bounded
  re-check or a cancellation check.
- Cross-thread flags are `g_atomic_*`; pointers shared across threads
  are guarded by the owning struct's mutex.
- Process-global state read by pool threads is either mutex-guarded
  (caches, HSTS, origin slots, curl share) or write-once-at-init.

## Synchronous sub-resource loads (pumped)

A handful of sub-resource loads are *synchronous from the script's point
of view* — the script that triggers them must see the result inline:
classic external `<script src>`, classic module imports (the QuickJS
module loader resolves synchronously), and `<iframe>` `src` plus its
inline scripts. (`XMLHttpRequest` is **not** here — it always uses the
async fetch pool, `ns_net_request_async`.)

Rather than block the main thread inside `curl_easy_perform`, these route
through `ns_js_fetch_resource`, which dispatches the request to the
async fetch pool and spins a **nested `GMainLoop`** on the main thread
until completion, with `js->in_pump` set for the duration. The window
keeps painting; reentrant JS (timers/rAF/events) is suppressed by the
existing `in_pump` guards, which preserves the synchronous-to-the-script
semantics; and every GTK teardown path defers (see the pump reentrancy
contract above), so the context cannot be freed mid-fetch. Completion is
a plain C callback, so it fires and quits the nested loop even while
`in_pump` defers everything else. The wait is bounded by the curl
transfer timeout, and concurrency still flows through the 5-slot
throttle. On a worker thread (`js->worker_host`) there is no GUI to keep
alive, so `ns_js_fetch_resource` falls back to a direct blocking fetch.

**Deferring without busy-spin.** Because the nested loop holds `in_pump`
for the whole sub-resource fetch (not just the ~8 iterations of the
interrupt-handler pump), any *other* JS-invoking completion that lands
during it must defer — and must do so without re-arming an
always-ready idle, which would spin the loop at 100 % CPU. The rule is
now uniform across **every** asynchronous source whose callback runs
page JS: each, when `in_pump` holds, re-arms with a 4 ms `g_timeout`
(matching the existing image-load path) so the loop can sleep in
`poll()` between checks. The covered set:

- `fetch()` completion (`ns_on_js_fetch_deliver_idle`),
- `XMLHttpRequest` completion (`ns_on_xhr_deliver_idle`) and the
  blocked/CORS-error path (`ns_xhr_emit_blocked_idle`),
- WebSocket open/message/close/error delivery, deferred in
  `ns_ws_dispatch_run` via an optional `busy` predicate on
  `ns_ws_callbacks` (so `ws.c` stays GTK-free and the queued payload is
  held, not copied, until the consumer is ready),
- `AbortSignal.timeout()` firing (`ns_abort_signal_timeout_fire`),
- `FileReader` completion (`ns_filereader_complete`),
- DOM timers (`ns_timer_fire`) and `requestAnimationFrame`/event entry
  points, which already early-out under `in_pump`.

This also closed a latent reentrancy gap: XHR, WebSocket, AbortSignal,
and FileReader callbacks previously ran immediately with no `in_pump`
check, so a completion arriving during *any* pump (including the brief
interrupt-handler pump) could run reentrant JS. With the set above,
no asynchronous source can run page JS while another JS frame is on the
stack.

