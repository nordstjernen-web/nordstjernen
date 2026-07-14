# wpt-fast scores

Results from the [wpt-fast](https://github.com/nordstjernen-web/wpt-fast)
checkout, run through the headless `--wpt` harness with
`scripts/wpt-fast.sh` (testharness.js tests only; reftests and
crashtests do not run headless). Unlike `docs/wpt-scores.md` — which
tracks a fixed 15-area slice of the upstream `~/wpt` — this is the full
wpt-fast tree (3733 test URLs). Re-run with:

```sh
scripts/wpt-fast.sh                 # whole tree
scripts/wpt-fast.sh dom css/selectors   # subtrees only
```

## Latest run — 2026-07-14 (commit dcc7e8d)

| Standard | Score | Subtests passed | Files 100% |
|----------|-------|-----------------|------------|
| HTML | 87.08% | 147,975 / 169,922 | 1,004 / 2,418 |
| CSS | 56.95% | 11,528 / 20,243 | 291 / 1,158 |
| JavaScript | 59.50% | 1,146 / 1,926 | 35 / 157 |
| **OVERALL** | **83.63%** | **160,649 / 192,091** | **1,330 / 3,733** |

Full whole-tree run (all 3733 test URLs, HTML+CSS+JS measured together,
not carried forward). This session's CSS-only changes — f26deba
(canonicalize math functions inside transform functions) and dcc7e8d
(simplify resolvable `min()`/`max()`/`clamp()` to `calc()`) — added +131
and +34 `css` subtests respectively; HTML and JavaScript are unchanged
from a92b7c7 (the lone HTML subtest delta is headless-timer run-to-run
noise).

Progress (all regression-free):

| Commit | Change | OVERALL |
|--------|--------|---------|
| 24f2e61 | (baseline) | 74.90% — 143,070 |
| 41e59c1 | interface `Symbol.toStringTag` | 75.15% — 143,552 |
| 4e216df | `getHTML()` / declarative shadow serialization | 79.73% — 152,304 |
| 754d150 | shadow roots hidden from traversal; declarative hosts | 80.41% — 153,677 |
| 6dbbb7a | obsolete boolean attribute reflection | — 154,283 |
| ceb5167 | iframe/embed/object/marquee width/height DOMString | 80.87% — 154,563 |
| eb098fc | DOMParser documents inherit the creator's URL | — 154,649 |
| 0ac6f3b | re-parse style sheet cssRules on text change | — 154,739 |
| 0944533 | hspace/vspace/scrollAmount/scrollDelay reflection | — 155,231 |
| 2c5645c | font.face reflection | 81.24% — 155,265 |
| 9bb4d06 | `in` operator on getComputedStyle() declarations | 81.29% — 155,364 |
| 3699e0a | document.write into an iframe survives to its load event | 82.52% — 158,520 |
| 3a7be44 | simplify resolvable math functions to `calc()` on serialize | 82.59% — 158,649 |
| bb8aedd | canonicalize/validate An+B in selector serialization | 82.65% — 158,758 |
| 5c8b72c | input value sanitization per type on set / type change | 82.80% — 159,052 |
| 5f941e5 | GlobalEventHandlers onX as prototype accessors | 83.08% — 159,589 |
| f2f8d8c | XML serialization for outerHTML of XML-document elements | 83.14% — 159,700 |
| 572ac50 | individual scale/rotate/translate computed values; calc NaN/infinity/constants | 83.25% — 159,912 |
| 5bf7f50 | full `display` grammar: multi-keyword parsing, canonical serialization, blockification | 83.34% — 160,090 |
| 57733b8 | validate min/max/clamp arguments; clamp() `none` bounds | 83.45% — 160,298 |
| 953653e | reject invalid transform functions (rotate angle math) | 83.52% — 160,439 |
| a92b7c7 | validate font-weight as keyword or `<number>` | 83.55% — 160,485 |
| f26deba | canonicalize math inside transform functions to `calc()` | — 160,616 |
| dcc7e8d | simplify resolvable `min()`/`max()`/`clamp()` to `calc()` | 83.63% — 160,649 |

### By top-level area

| Area | Subtests passing | |
|------|------------------|--|
| `url` | 8,502 / 8,679 | 98.0% |
| `webstorage` | 1,273 / 1,290 | 98.7% |
| `dom` | 60,530 / 62,556 | 96.8% |
| `ecmascript` | 19 / 21 | 90.5% |
| `js` | 112 / 130 | 86.2% |
| `shadow-dom` | 10,452 / 12,456 | 83.9% |
| `html` | 66,924 / 83,323 | 80.3% |
| `webidl` | 328 / 506 | 64.8% |
| `wasm` | 687 / 1,261 | 54.5% |
| `css` | 11,528 / 20,213 | 57.0% |
| `domparsing` | 294 / 1,572 | 18.7% |

## Top opportunities (non-tentative, most failing subtests)

| Failing / total | Test | Missing capability |
|-----------------|------|--------------------|
| 1469 / 3897 | `html/dom/idlharness.https.html?include=HTML.+` | HTML interface member coverage |
| 1460 / 8922 | `html/dom/reflection-embedded.html` | remaining reflection gaps (mostly null-byte attribute values) |
| 1257 / 1626 | `html/dom/idlharness.https.html?exclude=(Document\|Window\|HTML.+)` | interface member coverage |
| 1161 / 8271 | `html/dom/reflection-forms.html` | form-control reflection gaps |
| 978 / 6116 | `html/dom/reflection-tabular.html` | table reflection gaps |
| 961 / 1910 | `dom/idlharness.window.html` | interface member coverage |
| 927 / 10202 | `html/dom/reflection-text.html` | text-element reflection gaps |

The `reflection-*` and `idlharness` clusters remain the dominant HTML
headroom. A large share of the surviving `reflection-*` failures (~5,000)
are `setAttribute()`/`getAttribute()` round-trips of values containing NUL
and other control bytes — attribute values are stored as C strings, so
fixing those needs length-aware attribute storage (a wide, higher-risk
change). The rest are per-attribute numeric clamping/enumerated-default
work.

`css` (57%) is the largest whole-area gap. The biggest clusters:
**math functions** — trig/`round`/`mod`/`rem`/`sign`/`abs` now evaluate,
including the `pi`/`e`/`infinity`/`NaN` constants and NaN/infinity domain
edges, `scale`/`rotate`/`translate` compute to their own serialization,
`min`/`max`/`clamp` reject malformed and type-mixed arguments, honour
`none` clamp bounds, and simplify to `calc()` when their operands are a
single resolvable absolute-length or number type (mixed comparisons like
`min(20px, 10%)` stay as authored), and a math function used where a
`transform` function expects an `<angle>`/`<number>`/`<length>` now
serializes as `calc(...)` — `rotate(acos(1))` → `rotate(calc(0deg))`,
`scale(min(1,2))` → `scale(calc(1))`; the remaining `css/css-values`
gaps are the **multi-term calc serialization** (`calc(1% + 1px)`, the
sorted-unit dimension order of `calc-dimension-serialization-order`),
which needs a typed sum representation the current px/pct/em/rem
`NS_CSS_V_CALC` cannot hold; **time-typed validation**
(`transition-delay`/`-duration` are not longhand properties yet, so
malformed `min()`/`max()` there are accepted verbatim); the residual
**angle/time type-mixing** inside `rotate()`/time properties (e.g.
`min(1deg, 0)`); full signed-zero propagation; and unsupported
tree-counting functions like `sibling-index()`; **layout-precision** —
`getComputedStyle-insets-*` (calc/`auto` resolved against the containing
block) and `scrollWidthHeight` need real layout (~1,200); the full
multi-keyword `display` grammar (`block ruby`, `flow-root list-item`,
run-in, blockification) now parses, serializes canonically, and
blockifies floated/positioned boxes; and CSSOM interface surface
(`length`/indexed access/`item` on computed declarations). `domparsing`
(19%) is gated on per-realm
DOMParser constructors (iframes share the parent's) and the tentative
streaming/positional APIs.
