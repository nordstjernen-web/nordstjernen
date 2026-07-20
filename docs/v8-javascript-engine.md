# V8 JavaScript engine (experimental backend)

Nordstjernen binds its DOM to one of two JavaScript engines, chosen at
build time:

```sh
meson setup builddir                                        # default: quickjs
meson setup builddir -Djs_engine=quickjs                    # explicit quickjs
meson setup builddir -Djs_engine=v8 -Dv8_root=~/dev/v8-14.3 # experimental V8
```

Both engines implement the same seam: the `ns_js_*` contract declared in
`src/js.h`. Nothing outside the js layer sees an engine type — the
embedder (`src/libnordstjernen.c`, `src/headless.c`) drives page scripting
entirely through that C API, so swapping the engine swaps the whole
implementation behind one header.

## quickjs (default)

The in-tree QuickJS binding is `src/js.c` plus its satellites
(`js_canvas.c`, `js_date.c`, `js_intl.c`, `js_perf.c`, `js_realm.c`,
`ext.c`, `idb.c`, `wasm.c`, `webgl.c`, `webgpu.c`). It carries the full
Web API surface Nordstjernen has: live DOM bindings, events, canvas,
WebCrypto marshalling, workers, MSE, IndexedDB, WebAssembly, WebGL/WebGPU
and the WebExtensions host. The engine itself is the forked quickjs-ng
tree at `src/quickjs/`, built into every configuration.

## v8 (experimental)

`-Djs_engine=v8` swaps all of the above out and compiles `src/js_v8.cc`,
a single C++ file implementing the same `js.h` contract over the V8
embedder API. V8 is never vendored: the build needs a pkg-config `v8`, or
`-Dv8_root` pointing at an extracted monolith build with `include/v8.h`
and `libv8_monolith.a` — the prebuilt releases at
<https://github.com/just-js/v8> work directly. Those releases use LLVM
CREL relocations, so link with lld (`CC_LD=lld CXX_LD=lld meson setup …`).
The monolith's objects also use local-exec TLS and cannot enter a shared
library, so this configuration skips `libnordstjernen.so` and builds the
static engine and executables only.

What the V8 backend does today:

- Full modern-JS execution on V8 14 (the language itself, Promises,
  microtask ordering) for inline and external classic scripts, fetched
  through the browser's own network stack (cookie jar, HSTS, cache).
- `console.*`, timers on the GLib main loop, `requestAnimationFrame`
  driven by the render tick, `queueMicrotask`, `atob`/`btoa`,
  `location` (navigation routes through the embedder's navigate
  callback), `navigator`, `performance.now`, in-memory
  `localStorage`/`sessionStorage`, `matchMedia`/`getComputedStyle`
  stubs, window/document event listeners with correct
  `DOMContentLoaded` → async scripts → `load` → `pageshow` ordering,
  uncaught-exception and unhandled-rejection reporting with stacks.

What it deliberately does not do yet: **live DOM bindings**. `document`
is a small static object — query APIs return null/empty, there is no
`createElement`, no element wrappers, no dispatch of input events into
JS. Pages therefore render exactly as their static HTML, scripts run
until their first DOM touch, and the resulting TypeErrors are reported
to the console rather than crashing the page. Module scripts are
skipped with a console notice. WebGL, WebGPU, the WebAssembly JS API,
IndexedDB and WebExtensions are stubbed out of the build.

`about:nordstjernen` reports which engine a binary carries
("QuickJS 0.15.1" vs "V8 14.3.x (experimental backend)").

## Why keep both

The QuickJS binding is ~50k lines whose value is the *binding* work, not
the interpreter: every DOM/Web API is hand-marshalled against QuickJS
types. The V8 backend proves the `js.h` seam is real — a second engine
drops in without touching the embedder — and gives a yardstick for pure
JS-engine behaviour (spec-current language features, error quality,
performance) independent of our bindings. Growing it into a full
alternative would mean porting the binding layer to V8's handle/template
model piece by piece; the seam now exists to do that incrementally.
