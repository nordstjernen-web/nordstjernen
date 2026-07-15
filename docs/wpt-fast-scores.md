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

## Latest run — 2026-07-15 (commit 464cb9c)

| Standard | Score | Subtests passed | Files 100% |
|----------|-------|-----------------|------------|
| HTML | 91.73% | 156,004 / 170,061 | 1,014 / 2,418 |
| CSS | 63.37% | 12,823 / 20,235 | 326 / 1,158 |
| JavaScript | 59.50% | 1,146 / 1,926 | 35 / 157 |
| **OVERALL** | **88.43%** | **169,973 / 192,222** | **1,375 / 3,733** |

Continued HTML reflection cleanup: OVERALL 88.17% → 88.43%, **HTML 91.45% →
91.73%** (+488 subtests, 0 regressions — html-only). **66c7c30** — the obsolete
presentational dimension attributes reflect as DOMString on the tabular/obsolete
elements: `ns_dimension_is_string` gained table/colgroup/col/td/th/hr (so their
`width`/`height` are strings), and `hr.size` / `frameset.cols`/`rows` reflect a
DOMString through the integer getter (input.size / textarea.cols stay unsigned
long) (+374). **ee05efd** — `li.value` reflects the value attribute as a `long`
(default 0), not a string (+71). **464cb9c** — `href` resolves against the base
URL for non-anchor URL elements (`link`/`base`), so `link.href = ""` yields the
document URL and whitespace/protocol-relative values resolve (+40). Landing just
after this run (`7e6b169`, not yet measured): `form.encoding` gained a real
setter reflecting to `enctype` (~+34).

## Earlier run — 2026-07-15 (commit 96de106)

| Standard | Score | Subtests passed | Files 100% |
|----------|-------|-----------------|------------|
| HTML | 91.45% | 155,518 / 170,061 | 1,014 / 2,418 |
| CSS | 63.36% | 12,821 / 20,235 | 326 / 1,158 |
| JavaScript | 59.50% | 1,146 / 1,926 | 35 / 157 |
| **OVERALL** | **88.17%** | **169,485 / 192,222** | **1,375 / 3,733** |

Continuing the HTML reflection push (see the 36a28fc write-up below), this run
took OVERALL 87.47% → 88.17%, **HTML 90.66% → 91.45%** (+1,464 subtests, 0 real
regressions — html-only, css/dom/shadow-dom byte-identical). Four fixes:
**9c6bde0** — `document.body` returns the first `body` *or* `frameset` child of
`html` (it only looked for a body, so a frameset document's `document.body` was
null and the frameset event-handler tests all threw; +362 webappapis).
**96de106** — the dedicated IDL setters for `dir`/`hidden`/enumerated
attributes read the value with `JS_ToCStringLen`, so `element.dir = "ltr\0"`
stores the NUL byte and `element.hidden = "\0"` sets rather than removes the
attribute (~+390). **aa3c51d** — `document.fgColor`/`bgColor`/`linkColor`/
`vlinkColor`/`alinkColor` reflect the body's presentational attributes
(LegacyNullToEmptyString, ~+190). **865b075** — the input `.value` getter runs
the per-type value-sanitization algorithm, so a fresh `type=color` reads
`#000000` and `type=range` its midpoint (+34 forms).

## Earlier run — 2026-07-15 (commit 36a28fc)

| Standard | Score | Subtests passed | Files 100% |
|----------|-------|-----------------|------------|
| HTML | 90.66% | 154,054 / 169,922 | 1,010 / 2,418 |
| CSS | 63.36% | 12,821 / 20,235 | 326 / 1,158 |
| JavaScript | 59.50% | 1,146 / 1,926 | 35 / 157 |
| **OVERALL** | **87.47%** | **168,021 / 192,083** | **1,371 / 3,733** |

Full whole-tree run (all 3733 test URLs, HTML+CSS+JS measured together, not
carried forward). **HTML crossed 90%.** This session's HTML reflection work
took OVERALL 84.55% → 87.47% (**+5,608 subtests, 0 real regressions** — a −2
webstorage timer flake was the only non-html delta; css/dom/shadow-dom were
byte-identical). The decisive change was **ea5301c** — length-aware attribute
storage: `ns_attr` now carries an explicit `value_len` and stores its value
with `memcpy` rather than `g_strdup`, so an attribute value containing an
embedded NUL (U+0000) or other control bytes round-trips exactly through
`getAttribute`/`setAttribute`, `className`/`id`/`title` and the generic
reflected string get/set (read with `JS_ToCStringLen`, returned with
`JS_NewStringLen`); non-NUL values stay byte-identical, so every other
attribute consumer is unaffected. That unblocked ~5,000 reflection subtests
across all `reflection-*` files at once. **ef55d68** then made enumerated
keyword matching length-aware (`ns_enum_kw_eq` compares exact byte length), so
a value like `"ltr\0"` correctly resolves to the invalid-value-default rather
than matching `ltr` — `dir` alone recovered ~960 subtests. **36a28fc** carried
the NUL fidelity into the dedicated DOMString getters for
iframe/embed/object/marquee `width`/`height` and `type`. **454c1b7** switched
`meter`/`progress` double reflection to the engine's ECMAScript
Number-to-string (so `meter.value = -10000000000` stores `-10000000000`, not
`-1e+10`). Landing just after this run (`aa3c51d`, not yet measured):
`document.fgColor`/`bgColor`/`linkColor`/`vlinkColor`/`alinkColor` reflecting
the body's presentational attributes (~+190).

Earlier this session (measured in the 84.55% baseline above → superseded here):
enumerated-attribute reflection (`e79f9a8`, +773: enctype/method/scope/as/kind/
inputmode), limited integer reflection (`aacba16`, +297), preload enum + URL
resolution (`70e62d0`, +214), Web-IDL integer setters (`50786a7`, +141), the
Windows/LLP64 build fix (`1c80fc1`), and canvas width/height defaults
(`73fb48a`, +92).

Earlier CSS math work — 7da707c (serialize a
calc() that resolves to a non-finite length in the canonical
`calc(NaN * 1px)` / `calc(infinity * 1%)` product form instead of passing
the authored text through: `calc_term_scale` no longer pollutes absent
`pct`/`em`/`rem` buckets to NaN when scaling by a non-finite factor, and the
percent guard is relaxed only for non-finite results) and 155b8fb (resolve
`sign()` over an argument carrying a time/angle/frequency/resolution/flex
unit to a plain number so it is accepted where an `<integer>`/`<number>` is
expected, and preserve a negative zero across the calc resolver so
`1 / sign(sign(-0px))` is `-infinity`) — took `calc-infinity-nan-serialize-
length` 3 → 38/41, `calc-catch-divide-by-0` +17, and `signs-abs-computed`
151 → 181/233, for **+84 subtests overall (CSS 62.59% → 63.00%, 0
regressions)**.

Earlier, this session's event-handler work — 6e00aaa (complete
the GlobalEventHandlers `onX` name table: `oncommand`/`oncopy`/`oncut`/
`onload`/`onpaste`/`onresize`/`onscroll`, so they exist on window and the
element/document prototypes) and f7d50b5 (compile an element's `on<event>`
content attribute into a function on read, per the event-handler-content-
attributes spec, and set `event.currentTarget` around `onX` handler
invocations so a handler sees the right `currentTarget` even with no
`addEventListener` listeners) — took `event-handler-all-global-events`
211 → 343/375, for **+158 subtests overall (0 regressions)**.

Earlier, d997fcb landed two shadow-DOM fixes (shadow-dom
83.9% → 86.4%, +308 subtests, 0 regressions; these count in the HTML
standard bucket): `attachShadow` now throws `NotSupportedError` when the host
element is not in the HTML namespace (SVG/MathML/XML/custom/null-namespace
hosts), per the DOM "attach a shadow root" step 1; and a shadow root returned
by `attachShadow` is now `instanceof ShadowRoot` (ShadowRoot registered with
the `instanceof` machinery, keyed on the internal shadow-root marker).

The prior CSS work — 933ca91 — taught the selector engine the HTML
standard's attribute case-sensitivity rules: a fixed set of attributes
(`align`, `lang`, `dir`, `type`, `rel`, …) match their values
ASCII-case-insensitively when the element is an HTML-namespace element in an
HTML document, unless the selector carries an explicit `s` (case-sensitive)
flag — SVG/MathML/XML elements stay case-sensitive (+9 css/dom subtests, 0
regressions). The residual `attribute-case` failures are namespaced-attribute
selectors (`[x|lang]`, `[*|lang]` with `@namespace`) and NUL-byte values, both
separate.

The prior selector work — 1f45731 — added the
Selectors-5 `:heading` / `:heading(<integer>#)` pseudo-classes (matching
`h1`–`h6`, `heading.html` 0 → 182/182) and implemented
`CSS.supports("selector(...)")`, previously unwired in the JS supports
evaluator so it reported `false` for every selector (even `:hover`). The
`selector()` argument is now parsed in **non-forgiving** mode via a
`g_sel_strict` flag: invalid arguments to `:is()`/`:where()`/`:has()` and a
`:has()` nested inside `:has()` propagate as parse errors so support reports
the selector unsupported, while ordinary selector matching stays forgiving.
This flipped the trailing `CSS.supports()` assertion of `test_valid_selector`
across `css/selectors/parsing/*`, for **+340 css subtests** (0 regressions),
taking CSS past 62% and OVERALL to 84.22%.

Earlier this session's `<time>`-longhand work — af61a52
(validate `transition-delay`/`-duration` and `animation-delay`/`-duration`
as `<time>` longhands, +57) and e4ca851 (canonical specified/computed
serialization of time math — `min(1s, 2s, 3s)` → `calc(1s)` specified,
`1s` computed — plus spec NaN propagation through `min()`/`max()`/
`clamp()`, +91) — added +148 `css` subtests, taking the CSS area past 60%
and OVERALL past 84%. 91f6915 then registered `orphans`/`widows` as
`<integer>`, `max-lines` (`none | <integer>`), `hyphenate-limit-lines`
(`no-limit | <integer>`) and `column-span` (`none | all`), rejecting the
non-integer values these previously accepted verbatim (+6, and
`column-span`'s integer subtests now correctly skip). 7ff25f9 then clamps
a NaN component to 0 in computed `scale`/`rotate`/`translate` serialization
(`scale: calc(sin(infinity))` computes to `0`), fixing sin-cos-tan-serialize
(234 → 270/270, +36). 8ae9336 then taught the `scale()`/`scale3d()`
transform functions to evaluate `calc()`/`min()`/`max()` arguments
(`scale(calc(1.5))` was silently rendering as `scale(0)`) and clamped
computed `opacity` to `[0,1]` with NaN → 0 (+20). HTML and JavaScript are
unchanged (the small HTML delta is headless-timer run-to-run noise).
Earlier session changes — f26deba
(canonicalize math functions inside transform functions), dcc7e8d
(simplify resolvable `min()`/`max()`/`clamp()` to `calc()`), cd91b51
(validate selectors in stylesheet rules and `cssRules`), ae84fb3
(child-indexed pseudo-classes match without a parent element) and d9c0fcb
(validate/round `<integer>` properties) — added +131, +34, +514, +42 and
+22 `css` subtests respectively.

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
| cd91b51 | validate selectors in stylesheet rules and `cssRules` | 83.90% — 161,166 |
| ae84fb3 | child-indexed pseudo-classes match without a parent element | 83.92% — 161,206 |
| d9c0fcb | validate/round `<integer>` properties (z-index/order/column-count) | 83.93% — 161,229 |
| af61a52 | validate `<time>` longhands (transition/animation delay & duration) | 83.96% — 161,288 |
| e4ca851 | serialize/compute `<time>` longhands; NaN propagation in min/max/clamp | 84.01% — 161,378 |
| 91f6915 | validate `<integer>`/keyword orphans/widows/max-lines/hyphenate-limit-lines/column-span | 84.02% — 161,384 |
| 7ff25f9 | clamp NaN to 0 in computed scale/rotate/translate serialization | 84.04% — 161,420 |
| 8ae9336 | evaluate calc() in scale()/scale3d() transform args; clamp computed opacity | 84.05% — 161,441 |
| 1f45731 | `:heading`/`:heading(<integer>#)` selector; `CSS.supports(selector())` non-forgiving parse | 84.22% — 161,781 |
| 933ca91 | HTML case-insensitive attribute matching (align/lang/dir/type/…) | 84.23% — 161,788 |
| d997fcb | attachShadow throws on non-HTML-namespace hosts; ShadowRoot instanceof | 84.39% — 162,098 |
| 6e00aaa | complete GlobalEventHandlers onX name table | — 162,120 |
| f7d50b5 | compile event-handler content attributes; currentTarget for onX handlers | 84.47% — 162,256 |
| 7da707c | canonical calc() serialization for non-finite lengths (`calc(NaN * 1px)`) | — 162,308 |
| 155b8fb | sign() over non-length dimensions; preserve signed zero in calc | 84.52% — 162,340 |
| a144eca | implement the `progress()` function | 84.55% — 162,413 |
| e79f9a8 | enumerated-attribute reflection (enctype/method/scope/as/kind/inputmode) | — 163,186 |
| aacba16 | limited integer reflection (maxlength/minlength/size/cols/rows) | — 163,483 |
| 70e62d0 | preload enum reflection; URL-attr resolution fixes | — 163,697 |
| 50786a7 | Web-IDL-correct setters for reflected integer attributes | — 163,838 |
| 73fb48a | canvas width/height default to 300/150 for invalid values | — 163,930 |
| ea5301c | length-aware attribute storage (embedded NUL / control bytes) | — 168,949 |
| 454c1b7 | ECMAScript number serialization for reflected double attributes | — 169,029 |
| ef55d68 | length-aware enumerated-attribute keyword matching (`dir` etc.) | — 170,129 |
| 36a28fc | NUL-preserving DOMString reflection (width/height/type) | 87.47% — 168,021 |
| 865b075 | input `.value` getter returns the sanitized value (color/range/number) | — |
| aa3c51d | `document.fgColor`/`bgColor`/`linkColor`/`vlinkColor`/`alinkColor` | — |
| 9c6bde0 | `document.body` returns the frameset element | — |
| 96de106 | NUL-preserving dedicated setters (`dir`/`hidden`/enum) | 88.17% — 169,485 |
| 66c7c30 | DOMString width/height/size/cols for table/hr/frameset | — |
| ee05efd | `li.value` reflects the value attribute as a long | — |
| 464cb9c | resolve `href` against base for link/base elements | 88.43% — 169,973 |
| 7e6b169 | `form.encoding` setter reflects to `enctype` | — |

Note: the per-commit subtest counts above 163,930 are cumulative estimates
from per-area runs; the whole-tree total is re-measured only at full-run
commits, and the 36a28fc row is the authoritative full-tree figure (the
intermediate estimates do not net exactly against it because the headless
harness reports a slightly different subtest population per run).

### By top-level area

| Area | Subtests passing | |
|------|------------------|--|
| `url` | 8,502 / 8,679 | 98.0% |
| `webstorage` | 1,272 / 1,290 | 98.6% |
| `dom` | 60,530 / 62,556 | 96.8% |
| `ecmascript` | 19 / 21 | 90.5% |
| `html` | 74,646 / 83,463 | 89.4% |
| `shadow-dom` | 10,760 / 12,456 | 86.4% |
| `js` | 112 / 130 | 86.2% |
| `webidl` | 328 / 506 | 64.8% |
| `css` | 12,821 / 20,205 | 63.5% |
| `wasm` | 687 / 1,261 | 54.5% |
| `domparsing` | 294 / 1,572 | 18.7% |

## Top opportunities (non-tentative, most failing subtests)

| Failing / total | Test | Missing capability |
|-----------------|------|--------------------|
| 1361 / 3897 | `html/dom/idlharness.https.html?include=HTML.+` | HTML interface member coverage |
| 1255 / 1626 | `html/dom/idlharness.https.html?exclude=(Document\|Window\|HTML.+)` | interface member coverage |
| 961 / 1910 | `dom/idlharness.window.html` | interface member coverage |
| 332 / 6116 | `html/dom/reflection-tabular.html` | residual table reflection (obsolete string props, per-attr clamps) |
| 327 / 5604 | `html/dom/reflection-sections.html` | residual section reflection |
| 250 / 5358 | `html/dom/reflection-grouping.html` | residual grouping reflection |
| 242 / 298 | `html/dom/idlharness.https.html?include=(Document\|Window)` | Document/Window member coverage |

The `reflection-*` cluster is **no longer the dominant headroom** — the
length-aware attribute storage (`ea5301c`) and enum matching (`ef55d68`)
cleared the ~6,000-subtest NUL / enumerated-default backlog, and the
`reflection-*` files are now 90–97% each. **`idlharness`** (~3,800 non-css
failing) is now the single largest HTML gap: it enumerates every interface
member, and each missing attribute/operation costs one subtest, so the work
is broad but mechanical (add the reflected/computed member with the correct
type — a stub does not pass). The reflection residue is per-attribute numeric
clamping, a few obsolete presentational string props (`document.*Color`
landed post-run in `aa3c51d`; `body.text`, `script.htmlFor` remain), and
`link`/anchor `href` URL-getter empty-string resolution.

`css` (63%) is the largest whole-area gap. **Selector validity** now
matches the spec's all-or-nothing rule: a style rule (both in the C
engine and the CSSOM `cssRules` polyfill) is dropped when any selector in
its list is invalid — invalid attribute case-flags (`[a=b i i]`,
`[a i]`), unknown pseudo-classes, and malformed `:has()` no longer leak
into the cascade, while `:has(> .x)` relative arguments, escaped case
flags (`\i`, `\73`), and namespace prefixes (`x|div`, kept as never-match
so `@namespace` stylesheets round-trip) parse correctly, and child-indexed
pseudos (`:first-child`, `:nth-child()`, of-type variants) now match the
root element, detached elements and DocumentFragment children (no
parent-element required). The biggest remaining clusters:
**math functions** — trig/`round`/`mod`/`rem`/`sign`/`abs` now evaluate,
including the `pi`/`e`/`infinity`/`NaN` constants and NaN/infinity domain
edges, `scale`/`rotate`/`translate` compute to their own serialization,
`min`/`max`/`clamp` reject malformed and type-mixed arguments, honour
`none` clamp bounds, and simplify to `calc()` when their operands are a
single resolvable absolute-length or number type (mixed comparisons like
`min(20px, 10%)` stay as authored), and a math function used where a
`transform` function expects an `<angle>`/`<number>`/`<length>` now
serializes as `calc(...)` — `rotate(acos(1))` → `rotate(calc(0deg))`,
`scale(min(1,2))` → `scale(calc(1))`; a calc() that resolves to a
**non-finite length** serializes in the canonical `calc(NaN * 1px)` /
`calc(infinity * 1%)` product form, `sign()` resolves over a
time/angle/frequency/resolution/flex argument to a plain number, a
negative zero survives the calc resolver (`1 / sign(sign(-0px))` is
`-infinity`), and the CSS Values 5 **`progress()`** function is
implemented (a strictly-typed number: `clamp((A - B) / (C - B), 0, 1)`,
or the unclamped ratio with `no-clamp`), simplifying to `calc(<n>)` when
its operands resolve and staying authored under a relative unit; the
remaining `css/css-values`
gaps are the **multi-term calc serialization** (`calc(1% + 1px)`, the
sorted-unit dimension order of `calc-dimension-serialization-order`),
which needs a typed sum representation the current px/pct/em/rem
`NS_CSS_V_CALC` cannot hold — this also blocks the single-argument
`min(1% + 1px)` → `calc(1% + 1px)` percent-first ordering and the
non-finite **resolution** serialization (`calc(NaN * 1dppx)`); **time-typed math** is now complete —
`transition-delay`/`-duration` and `animation-delay`/`-duration` are
`<time>` longhands that reject malformed or wrong-typed `min()`/`max()`/
`calc()` via a dedicated CSS-math type checker, simplify a resolvable
math value to its canonical `calc(<n>s)` specified form (and `<n>s`
computed form), and serialize the non-finite `calc(NaN * 1s)` /
`calc(infinity * 1s)` cases, with `min()`/`max()`/`clamp()` propagating
NaN per spec; the residual **angle type-mixing** inside `rotate()` (e.g.
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
