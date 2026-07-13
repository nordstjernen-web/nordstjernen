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

## Latest run — 2026-07-13 (commit 754d150)

| Standard | Score | Subtests passed | Files 100% |
|----------|-------|-----------------|------------|
| HTML | 84.28% | 142,382 / 168,948 | 921 / 2,418 |
| CSS | 50.17% | 10,156 / 20,243 | 278 / 1,158 |
| JavaScript | 59.14% | 1,139 / 1,926 | 35 / 157 |
| **OVERALL** | **80.41%** | **153,677 / 191,117** | **1,234 / 3,733** |

Progress this cycle (all regression-free):

| Commit | Change | OVERALL |
|--------|--------|---------|
| 24f2e61 | (baseline) | 74.90% — 143,070 |
| 41e59c1 | interface `Symbol.toStringTag` | 75.15% — 143,552 |
| 4e216df | `getHTML()` / declarative shadow serialization | 79.73% — 152,304 |
| 754d150 | shadow roots hidden from traversal; declarative hosts | 80.41% — 153,677 |

### By top-level area

| Area | Subtests passing | |
|------|------------------|--|
| `webstorage` | 1,274 / 1,290 | 98.8% |
| `url` | 8,502 / 8,679 | 98.0% |
| `dom` | 60,530 / 62,556 | 96.8% |
| `ecmascript` | 19 / 21 | 90.5% |
| `js` | 112 / 130 | 86.2% |
| `shadow-dom` | 10,451 / 12,456 | 83.9% |
| `html` | 61,417 / 82,333 | 74.6% |
| `webidl` | 321 / 506 | 63.4% |
| `wasm` | 687 / 1,261 | 54.5% |
| `css` | 10,156 / 20,213 | 50.2% |
| `domparsing` | 208 / 1,572 | 13.2% |

## Top opportunities (non-tentative, most failing subtests)

| Failing / total | Test | Missing capability |
|-----------------|------|--------------------|
| 1886 / 8922 | `html/dom/reflection-embedded.html` | content-attribute ↔ IDL reflection (numeric/boolean/enumerated defaults, null bytes) |
| 1469 / 3897 | `html/dom/idlharness.https.html?include=HTML.+` | HTML interface member coverage |
| 1257 / 1626 | `html/dom/idlharness.https.html?exclude=(Document\|Window\|HTML.+)` | interface member coverage |
| 1229 / 8271 | `html/dom/reflection-forms.html` | form-control reflection gaps |
| 1052 / 6116 | `html/dom/reflection-tabular.html` | table reflection gaps |
| 961 / 1910 | `dom/idlharness.window.html` | interface member coverage |
| 927 / 10202 | `html/dom/reflection-text.html` | text-element reflection gaps |
| 803 / 2621 | `html/dom/reflection-obsolete.html` | obsolete-element reflection gaps |

The `reflection-*` and `idlharness` clusters are now the dominant HTML
headroom: broad but individually small per-attribute / per-member fixes.
`css` and `domparsing` are the largest whole-area gaps.
