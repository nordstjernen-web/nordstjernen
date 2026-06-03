# Benchmarking Nordstjernen on Speedometer 3.1

The official [Speedometer 3.1](https://browserbench.org/Speedometer3.1/)
harness loads every workload inside an `<iframe>` and drives it through
`frame.contentDocument` / `frame.contentWindow` after `frame.onload`.
Nordstjernen now supports this: setting an iframe's `src`/`srcdoc`
fetches and parses the document, splices it under the iframe node so it
styles, lays out, and runs its own scripts, marks the iframe
`data-nd-frame-loaded` so the UA stylesheet renders it, and dispatches
`load`. `HTMLIFrameElement` exposes `contentDocument` (documentElement /
body / head / getElementById / querySelector / querySelectorAll /
defaultView) and a scoped `contentWindow` facade with frame-local
`location` and `history`. Individual official suite runs produce scores.

Multi-suite aggregate runs now drive to a geomean score as well — the
runner reloads its one test iframe per suite/iteration and the engine
keeps up. (Earlier these appeared to hang; that was the per-reload
document-index and orphan/pinned-wrapper growth turning each reload
O(n^2), since fixed, plus the headless settle running ~2x the
`--settle-ms` budget, so a too-short timeout killed the run before it
finished.) Example aggregate over the eleven light TodoMVC frameworks
(ES5, ES6-Webpack, WebComponents, React, React-Redux, Backbone, Vue,
jQuery, Preact, Svelte, Lit), `iterationCount=1`:

```sh
./builddir/src/nordstjernen \
  'http://localhost:8124/index.html?suite=TodoMVC-JavaScript-ES5,TodoMVC-JavaScript-ES6-Webpack,TodoMVC-WebComponents,TodoMVC-React,TodoMVC-React-Redux,TodoMVC-Backbone,TodoMVC-Vue,TodoMVC-jQuery,TodoMVC-Preact,TodoMVC-Svelte,TodoMVC-Lit&startAutomatically=true&iterationCount=1'
```

`window` is still the one shared global, so cross-suite state (framework
globals, window listeners) can still leak between suites in a single
aggregate run; treat the aggregate as indicative, and prefer per-suite or
fresh-process runs when comparing the engine over time.

```sh
# Serve the harness and run it headless to a score:
git clone --depth 1 --branch release/3.1 \
    https://github.com/WebKit/Speedometer.git /tmp/spdm
( cd /tmp/spdm && python3 -m http.server 8124 & )
./builddir/src/nordstjernen --headless --dump=text --settle-ms=60000 \
    'http://localhost:8124/index.html?suite=TodoMVC-JavaScript-ES5&startAutomatically=true&iterationCount=1'
```

For per-suite engine tracking we still also drive each Speedometer 3.1
*workload* directly via `scripts/speedometer-bench.sh`, which fetches
Speedometer's `release/3.1` sources at runtime, generates a page driver
from them, replays Speedometer's own per-suite interaction steps against
the top-level document, and times each suite. Nothing third-party is
vendored into this repository.

## Running

```sh
scripts/dev.sh build
scripts/speedometer-bench.sh            # every loadable TodoMVC suite
scripts/speedometer-bench.sh complex    # only paths containing "complex"
```

Each suite is run `ND_ITERS` times (default 3) and the median total is
reported. Per phase the driver records `sync` time (the interaction loop) plus
`async` time (a forced layout flush on the next frame), matching the real
runner's `measurementMethod = "raf"` measurement. `domNodes` is the live
element count at the end of the run.

## What runs

Suites that drive end to end and produce a score in the **official iframe
harness** (run individually, `?suite=<name>&startAutomatically=true`):

- All seven light-DOM TodoMVC frameworks and their `-Complex-DOM` variants:
  vanilla JS (ES5, ES6-Webpack), React, Angular, Vue, Preact, Svelte.
- **React-Redux** and **WebComponents** — after the window-event-handler and
  custom-element fixes (`src/js.c`); WebComponents builds its list with `new
  TodoItem()` into shadow roots, which now yields real element instances.
- The non-TodoMVC **Charts-chartjs**, **Charts-observable-plot**,
  **Editor-CodeMirror**, **Editor-TipTap**, and **NewsSite-Nuxt** suites.
- **Lit** - after custom-element construction stopped reusing the upgrading
  element for ordinary `EventTarget` subclasses created inside custom element
  constructors.
- **jQuery** and **Backbone** — after giving iframe scripts their own scoped
  `document` (`src/js.c`), running an iframe's classic scripts in one shared
  scope (so Backbone's cross-file `var app` is shared), and serializing
  `<script>` innerHTML unescaped (`src/dom.c`) so Handlebars/Underscore
  templates compile to real markup.

Each iframe's classic `<script>`s run concatenated inside one
`(function(document){...})` wrapper, with `document` bound to a facade scoped
to the iframe's subtree (a per-call current-document swap); module scripts
keep their own scope. This also dropped Next.js's head-manager errors to
zero. `window` is still the shared global, so the remaining gaps are:

Still imperfect:

- **NewsSite-Next** — past the head errors, but Next.js hydration doesn't
  drive to completion. **React-Stockcharts-SVG** currently renders its
  Stockcharts fit wrapper placeholder but never creates the SVG crosshair
  cursor, and **Perf-Dashboard** still reaches the harness UI but returns no
  score. Multi-suite aggregate runs are also polluted by stale window
  `load`/`hashchange` listeners and framework globals left by previous suites.

Current fresh-process official-suite snapshot on Windows (2026-06-03):
17 of 20 suites score, geomean **2.4340**. This is not the official
single-page aggregate score; it is the comparable per-suite score while the
shared global limitation remains.

## Snapshot (release build, Linux, GTK 4.14)

Median total ms, lower is faster. The "before" column is the pre-optimization
baseline; "total" is after the cascade work described under Profiling.

| Suite                         | before | total |   add | complete | delete | DOM   |
|-------------------------------|-------:|------:|------:|---------:|-------:|------:|
| TodoMVC-Angular               |    224 |   230 |   200 |       19 |     10 |   641 |
| TodoMVC-JavaScript-ES5        |    240 |   228 |   205 |       12 |     11 |   543 |
| TodoMVC-React                 |    352 |   352 |   204 |       21 |    127 |    24 |
| TodoMVC-Svelte                |    366 |   361 |   155 |      133 |     73 |    20 |
| TodoMVC-Preact                |   2692 |  2610 |   693 |     1263 |    654 |    40 |
| TodoMVC-React-Complex-DOM     |  10522 |  8681 |  8519 |       26 |    136 |  6632 |
| TodoMVC-Angular-Complex-DOM   |  10723 |  8052 |  8019 |       22 |     11 |  7249 |
| TodoMVC-JavaScript-ES5-Complex|  10817 |  8607 |  8581 |       16 |     11 |  7151 |
| TodoMVC-Svelte-Complex-DOM    |  11268 |  8663 |  8381 |      132 |     70 |  6628 |
| TodoMVC-Preact-Complex-DOM    |  12937 | 10353 |  8316 |     1347 |    690 |  6653 |

The numbers are not comparable to an official Speedometer score — this driver
has no warmup suite and no geomean aggregation. They are a self-consistent
per-workload measurement for tracking the engine over time.

## Profiling

The `*-Complex-DOM` variants cost ~30-40x their small-DOM twins, and the cost
is almost entirely the `Adding100Items` phase. The ES5-Complex suite uses no
framework, so its time is pure engine. The shape is a **full-document restyle
on every DOM mutation**: each of the 100 inserts (each forced layout flush)
re-cascades the whole tree, so the cost scales as O(items x nodes x rules).
The small-DOM suites stay fast because their trees are tiny; the complex-DOM
suites carry ~7000 static nodes that are re-styled 100 times over.

A 150-sample gdb profile of the add phase originally attributed ~41% to glibc
allocator churn (`_int_malloc` / `free` / `malloc_consolidate`) and ~30% to
GLib hashing behind selector matching, under `cascade_walk` →
`gather_matches_impl` (`src/css.c`). Two changes cut the allocator churn:

- **Skip empty pseudo-element passes.** `cascade_walk` ran eight pseudo-element
  gather passes (`::before`/`::after`/`::first-letter`/…) per element, each
  allocating four arrays, even when no stylesheet styled that pseudo-element.
  Each stylesheet now records a bitmask of the pseudo-elements its selectors
  target (built with the rule index), and passes for absent pseudo-elements
  are skipped entirely.
- **Reuse cascade scratch arrays.** The four per-element match arrays were
  `g_array_new`/`g_array_free`d for every node on every relayout; they are now
  cleared-and-reused scratch buffers.
- **Skip the per-element variable map** when no custom properties are in scope
  (`build_vars_for_element` returns NULL instead of allocating an empty hash
  table for every node).
- **Build the box-lookup index lazily.** `getBoundingClientRect` no longer
  builds a whole-document box index to satisfy a single query per reflow.

Together these drop the complex-DOM suites ~20-29% (see the table).

A fresh 60-sample profile after these changes shows the remaining cost is the
**inherent full-document re-cascade**: leaf time is dominated by the glibc
allocator (`_int_malloc`/`free`/`malloc_consolidate`, ~45%) and `g_str_hash` /
`g_hash_table_lookup` from selector-index matching, with `nd_style_free` high
in the inclusive frames — i.e. allocating and freeing ~7000 computed styles
(and their `nd_css_value`s) on each of the 100 reflows. The two levers that
remain, both larger and correctness-sensitive, are:

1. **Incremental restyle** — re-cascade only the mutated subtree (plus the
   siblings/ancestors that structural selectors depend on) instead of the whole
   document, turning the add phase from O(items x nodes) toward O(items).
2. **Refcount `nd_css_value`** — `cascade_for` deep-copies every matched and
   inherited value into each element's style and `nd_style_free` frees them
   all; sharing immutable values by refcount would remove most of the per-node
   alloc/free churn. This needs an audit that no site mutates a value in place.

To reproduce the profile:

```sh
meson setup builddir-prof --buildtype=debugoptimized -Db_lto=false
meson compile -C builddir-prof
# in one shell, start a long-settle run of a complex-DOM suite, then:
scripts/sample-profile.sh <pid> 150
```
