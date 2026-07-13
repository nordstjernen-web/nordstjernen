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

## Latest run — 2026-07-13 (commit 9bb4d06)

| Standard | Score | Subtests passed | Files 100% |
|----------|-------|-----------------|------------|
| HTML | 85.16% | 143,882 / 168,948 | 919 / 2,418 |
| CSS | 51.07% | 10,339 / 20,243 | 278 / 1,158 |
| JavaScript | 59.35% | 1,143 / 1,926 | 35 / 157 |
| **OVERALL** | **81.29%** | **155,364 / 191,117** | **1,233 / 3,733** |

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

### By top-level area

| Area | Subtests passing | |
|------|------------------|--|
| `url` | 8,502 / 8,679 | 98.0% |
| `webstorage` | 1,273 / 1,290 | 98.7% |
| `dom` | 60,530 / 62,556 | 96.8% |
| `ecmascript` | 19 / 21 | 90.5% |
| `js` | 112 / 130 | 86.2% |
| `shadow-dom` | 10,452 / 12,456 | 83.9% |
| `html` | 62,830 / 82,333 | 76.3% |
| `webidl` | 325 / 506 | 64.2% |
| `wasm` | 687 / 1,261 | 54.5% |
| `css` | 10,339 / 20,213 | 51.1% |
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

`css` (51%) is the largest whole-area gap. The biggest clusters:
**math functions** — `sin`/`cos`/`tan`/`acos`/`asin`/`atan`/`atan2` are
unimplemented and `round`/`mod`/`rem`/`sign`/`-0` have serialization gaps
(~1,000 subtests across `css/css-values`); **layout-precision** —
`getComputedStyle-insets-*` (calc/`auto` resolved against the containing
block) and `scrollWidthHeight` need real layout (~1,200); **multi-keyword
`display`** parsing/canonical serialization (`block ruby`, `flow-root
list-item`); and CSSOM interface surface (`length`/indexed access/`item`
on computed declarations). `domparsing` (19%) is gated on per-realm
DOMParser constructors (iframes share the parent's) and the tentative
streaming/positional APIs.
