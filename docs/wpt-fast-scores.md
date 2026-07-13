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

## Latest run — 2026-07-13 (commit 41e59c1)

| Standard | Score | Subtests passed | Files 100% |
|----------|-------|-----------------|------------|
| HTML | 78.30% | 132,286 / 168,946 | 911 / 2,418 |
| CSS | 50.25% | 10,127 / 20,155 | 278 / 1,158 |
| JavaScript | 59.14% | 1,139 / 1,926 | 35 / 157 |
| **OVERALL** | **75.15%** | **143,552 / 191,027** | **1,224 / 3,733** |

Previous full run (commit 24f2e61) was 74.90% (143,070 / 191,027). The
`Symbol.toStringTag` fix (41e59c1) added 482 passing subtests with no
regressions.

### By top-level area

| Area | Subtests passing | |
|------|------------------|--|
| `dom` | 60,530 / 62,556 | 96.8% |
| `webstorage` | 1,274 / 1,290 | 98.8% |
| `url` | 8,502 / 8,679 | 98.0% |
| `ecmascript` | 19 / 21 | 90.5% |
| `js` | 112 / 130 | 86.2% |
| `html` | 61,394 / 82,333 | 74.6% |
| `webidl` | 321 / 506 | 63.4% |
| `wasm` | 687 / 1,261 | 54.5% |
| `css` | 10,127 / 20,124 | 50.3% |
| `domparsing` | 208 / 1,572 | 13.2% |
| `shadow-dom` | 378 / 12,453 | 3.0% |

## Top opportunities (non-tentative, most failing subtests)

| Failing / total | Test | Missing capability |
|-----------------|------|--------------------|
| 6908 / 6908 | `shadow-dom/declarative/gethtml.html` | `Element`/`ShadowRoot` `getHTML()`, `setHTMLUnsafe()`, shadow-root serialization, `attachShadow` `serializable`/`clonable`/`delegatesFocus` |
| 1886 / 8922 | `html/dom/reflection-embedded.html` | content-attribute ↔ IDL reflection gaps |
| 1469 / 3897 | `html/dom/idlharness.https.html?include=HTML.+` | interface member coverage |
| 1263 / 1626 | `html/dom/idlharness.https.html?exclude=(Document\|Window\|HTML.+)` | interface member coverage |
| 1229 / 8271 | `html/dom/reflection-forms.html` | form-control reflection gaps |
| 1052 / 6116 | `html/dom/reflection-tabular.html` | table reflection gaps |
| 961 / 1910 | `dom/idlharness.window.html` | interface member coverage |
| 927 / 10202 | `html/dom/reflection-text.html` | text-element reflection gaps |

`shadow-dom` as a whole is dominated by declarative shadow DOM
serialization (`getHTML`) and the tentative `reference-target` tree; it
is the single largest area headroom. The `reflection-*` and `idlharness`
clusters are broad but individually small per-attribute fixes.
