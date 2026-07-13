# WPT scores

Tracks Nordstjernen's results on the
[web-platform-tests](https://github.com/web-platform-tests/wpt)
suite over time: a fixed 15-area slice measured exactly (see
"Tracked slice" below), and a sampled estimate of the whole-suite
score browsers are compared by. See `docs/wpt.md` for how the
harness integration works. Scores update via `scripts/wpt-score.sh`
and `scripts/wpt-estimate.sh`, which edit the tables in this file in
place — review the diff and commit it together with the data files
they regenerate.

## History

| Date | Nordstjernen | WPT | Files ok | Subtests passing | Notes |
|------|--------------|-----|----------|------------------|-------|
| 2026-06-12 | 9526465 | 3be6ba111 | 181/696 (26%) | 6921/16067 (43%) | full |
| 2026-06-12 | e000f76 | 3be6ba111 | 181/696 (26%) | 6921/16067 (43%) | partial: dom/lists |
| 2026-06-12 | 07f95d7 | 3be6ba111 | 182/697 (26%) | 10017/19502 (51%) | full |
| 2026-06-12 | 5cd79e7 | d8a8414e5 | 182/696 (26%) | 6991/16067 (43%) | partial: html/webappapis/atob/base64.any.js |
| 2026-06-12 | c752e51 | 3be6ba111 | 187/697 (26%) | 11739/19502 (60%) | full |
| 2026-06-12 | 49de47c | 3be6ba111 | 187/697 (26%) | 12097/19502 (62%) | partial: dom/nodes dom/events html/webappapis/atob |
| 2026-06-12 | f0fb7b7 | 3be6ba111 | 188/697 (26%) | 12269/19502 (62%) | partial: dom/nodes |
| 2026-06-12 | ce53924 | 3be6ba111 | 189/696 (27%) | 15001/19492 (76%) | partial: url |
| 2026-06-12 | 2bba43f | 3be6ba111 | 189/696 (27%) | 15121/19492 (77%) | partial: dom/nodes |
| 2026-06-12 | 7a67688 | 3be6ba111 | 191/696 (27%) | 15757/19492 (80%) | partial: dom/nodes |
| 2026-06-12 | 7110c2f | 3be6ba111 | 191/696 (27%) | 15774/19525 (80%) | partial: dom/events |
| 2026-06-12 | 78da000 | d8a8414e5 | 195/696 (28%) | 15958/19502 (81%) | partial: html/dom/elements html/semantics/forms/the-form-element |
| 2026-06-12 | ffde346 | 3be6ba111 | 196/696 (28%) | 16080/19535 (82%) | partial: dom/lists dom/nodes |
| 2026-06-12 | faf548f | d8a8414e5 | 200/696 (28%) | 15986/19535 (81%) | partial: html/dom/elements |
| 2026-06-12 | d54d3b8 | d8a8414e5 | 199/696 (28%) | 16302/20746 (78%) | full |
| 2026-06-13 | fcfeaf1 | d8a8414e5 | 206/696 (29%) | 16227/20620 (78%) | full |
| 2026-06-13 | fcfeaf1 | d8a8414e5 | 206/696 (29%) | 16327/20746 (78%) | partial: html/dom/elements console html/webappapis/timers |
| 2026-06-13 | 7b9a177 | d8a8414e5 | 210/696 (30%) | 16258/20620 (78%) | full |
| 2026-06-13 | 2639ddc | d8a8414e5 | 210/696 (30%) | 17245/20620 (83%) | partial: url |
| 2026-06-13 | f9fb57c | d8a8414e5 | 212/696 (30%) | 17257/20620 (83%) | partial: url |
| 2026-06-13 | f2448ed | d8a8414e5 | 223/696 (32%) | 18420/21529 (85%) | partial: dom/nodes |
| 2026-06-13 | 726e248 | d8a8414e5 | 227/696 (32%) | 18551/21529 (86%) | partial: dom/nodes |
| 2026-06-13 | f30dab1 | d8a8414e5 | 227/696 (32%) | 18707/21654 (86%) | partial: html/dom/elements/the-innertext-and-outertext-properties |
| 2026-06-14 | 9fe9c25 | f01d00b69 | 276/696 (39%) | 60345/66800 (90%) | full |
| 2026-06-15 | d52628c | d8a8414e5 | 283/696 (40%) | 60603/66800 (90%) | partial: dom/traversal |
| 2026-06-15 | 3a754e8 | d8a8414e5 | 284/696 (40%) | 50415/55389 (91%) | full |
| 2026-06-16 | 5442142 | a72c94d4a | 286/696 (41%) | 50493/55389 (91%) | partial: html/dom/elements |
| 2026-06-16 | fec36c7 | a72c94d4a | 290/696 (41%) | 50646/55464 (91%) | partial: url |
| 2026-06-16 | 9531924 | a72c94d4a | 292/696 (41%) | 61026/66617 (91%) | partial: dom/ranges |
| 2026-06-19 | 129a7ce | 1df6b93 | 325/696 (46%) | 64620/69378 (93%) | partial: dom/nodes dom/collections |
| 2026-06-19 | d3486d7 | 1df6b93 | 330/696 (47%) | 67138/69378 (96%) | partial: dom/ranges |
| 2026-06-19 | 2eb8fe5 | 1df6b93 | 332/696 (47%) | 67248/69378 (96%) | partial: dom/nodes |
| 2026-06-19 | 66d9b68 | 1df6b936e | 332/696 (47%) | 67314/69379 (97%) | partial: url |
| 2026-06-19 | 5d428c4 | 1df6b936e | 338/696 (48%) | 67474/69505 (97%) | partial: html/dom/elements |
| 2026-06-24 | e0a6389 | f6fd39b13 | 341/696 (48%) | 67545/69505 (97%) | partial: html/semantics/forms/the-form-element xhr/formdata dom/events/EventTarget-dispatchEvent.html dom/events/Event-subclasses-constructors.html dom/events/Event-dispatch-bubbles-false.html |
| 2026-06-24 | b8e72fa | f6fd39b13 | 342/696 (49%) | 67568/69508 (97%) | partial: dom/traversal |
| 2026-06-24 | c19759a | f6fd39b13 | 345/696 (49%) | 67576/69508 (97%) | partial: dom/ranges/Range-mutations-insertBefore.html dom/ranges/Range-mutations-replaceChild.html dom/ranges/Range-mutations-removeChild.html |
| 2026-06-24 | c9783fa | f6fd39b13 | 346/696 (49%) | 67593/69508 (97%) | partial: dom/nodes/MutationObserver-characterData.html dom/nodes/MutationObserver-attributes.html |
| 2026-06-24 | e09e8b2 | f6fd39b13 | 352/696 (50%) | 67628/69501 (97%) | partial: dom/nodes/moveBefore |
| 2026-06-25 | 7041e0f | 88deace8f | 358/696 (51%) | 67884/69508 (97%) | partial: dom/nodes dom/collections html/dom/elements |
| 2026-06-25 | c34415a | 88deace8f | 360/696 (51%) | 67908/69508 (97%) | partial: dom/nodes |
| 2026-06-25 | 0be73e3 | 88deace8f | 363/696 (52%) | 67913/69508 (97%) | partial: dom/nodes html/dom/elements |
| 2026-06-25 | 7e49644 | 88deace8f | 372/697 (53%) | 67947/69508 (97%) | partial: dom/events hr-time |
| 2026-06-25 | 003537a | 88deace8f | 392/697 (56%) | 67948/69423 (97%) | partial: dom/events |
| 2026-06-25 | 8100989 | 88deace8f | 397/697 (56%) | 67956/69429 (97%) | partial: dom/events |
| 2026-06-27 | e92c9b1 | d5fb546 | 433/698 (62%) | 65712/69646 (94%) | full; iframe/CE/error-event/touch |
| 2026-06-27 | ab57a20 | d5fb546 | 437/698 (62%) | 67294/69646 (96%) | partial: dom/ranges dom/nodes; name-validation + Range exceptions |
| 2026-06-29 | 48ebb3d | 2a91c2e71 | 437/698 (62%) | 68361/70733 (96%) | partial: url |
| 2026-06-29 | 48ebb3d | 2a91c2e71 | 437/698 (62%) | 67257/69570 (96%) | partial: url |
| 2026-06-29 | 48ebb3d | 2a91c2e71 | 437/698 (62%) | 68408/70739 (96%) | partial: url |
| 2026-06-29 | 9792f92 | 2a91c2e71 | 437/698 (62%) | 68454/70741 (96%) | partial: dom/events html/dom/elements |
| 2026-06-29 | a6a818d | 2a91c2e71 | 437/698 (62%) | 68455/70741 (96%) | partial: html/dom/elements |
| 2026-06-29 | 11918f6 | 2a91c2e71 | 437/698 (62%) | 68455/70741 (96%) | partial: dom/ranges |
| 2026-06-29 | 11918f6 | 2a91c2e71 | 437/698 (62%) | 68455/70741 (96%) | partial: html/dom/elements |
| 2026-07-12 | 8a05dfd | 510951e15 | 423/666 (63%) | 60900/62159 (97%) | partial: url |
| 2026-07-12 | 8a05dfd | 510951e15 | 423/666 (63%) | 60900/62159 (97%) | partial: url |
| 2026-07-12 | 8a05dfd | 510951e15 | 423/666 (63%) | 60900/62159 (97%) | partial: url |
| 2026-07-12 | 8a05dfd | 510951e15 | 423/666 (63%) | 60900/62159 (97%) | partial: url |
| 2026-07-12 | 8a05dfd | 510951e15 | 437/698 (62%) | 68365/70829 (96%) | partial: url |
| 2026-07-12 | 8a05dfd | 510951e15 | 437/698 (62%) | 68363/70829 (96%) | partial: url |
| 2026-07-12 | 8a05dfd | 510951e15 | 438/698 (62%) | 69203/70829 (97%) | partial: url |
| 2026-07-12 | 2b6f8b9 | 510951e15 | 438/698 (62%) | 69391/70829 (97%) | partial: url |
| 2026-07-12 | 5209e99 | 510951e15 | 440/698 (63%) | 69429/70829 (98%) | partial: dom/events |
| 2026-07-12 | cf5c711 | 510951e15 | 440/698 (63%) | 69445/70829 (98%) | partial: dom/events |
| 2026-07-12 | cf5c711 | 510951e15 | 440/698 (63%) | 69450/70829 (98%) | partial: dom/events |
| 2026-07-12 | cae4bcd | 510951e15 | 439/699 (62%) | 69463/70855 (98%) | partial: dom/ranges |
| 2026-07-12 | cae4bcd | 510951e15 | 445/699 (63%) | 69473/70855 (98%) | partial: html/dom/elements |
| 2026-07-12 | cae4bcd | 510951e15 | 408/662 (61%) | 69659/70991 (98%) | partial: html/dom/elements |
| 2026-07-12 | 4a7d4e7 | 510951e15 | 417/662 (62%) | 69731/70991 (98%) | partial: html/dom/elements |
| 2026-07-13 | 130d6da | 510951e15 | 416/665 (62%) | 69785/71141 (98%) | partial: dom/nodes |
| 2026-07-13 | efc4645 | 510951e15 | 474/742 (63%) | 70693/71907 (98%) | full |
| 2026-07-13 | dcd3e8d | 510951e15 | 495/742 (66%) | 70722/71907 (98%) | partial: dom/events |
| 2026-07-13 | bc60318 | 510951e15 | 497/742 (66%) | 70732/71907 (98%) | partial: dom/traversal html/webappapis/timers |
| 2026-07-13 | 14fcea1 | 510951e15 | 501/742 (67%) | 70743/71907 (98%) | partial: dom/events |

"Files ok" counts test files where the harness completed and every
subtest passed; "subtests passing" counts individual testharness.js
results. The Notes column records whether a row came from a full or
partial run — a partial row mixes revisions for the areas it did not
touch.

## Per-area results — 2026-07-13

Per-file detail for this run: `docs/wpt-runs/2026-07-13-14fcea1.tsv`.

| Area | Files ok | Subtests passing | Fail | Timeout | Notrun | Precondition failed |
|------|----------|------------------|------|---------|--------|---------------------|
| `dom/nodes` | 171/278 | 12734/12965 | 206 | 10 | 9 | 6 |
| `dom/events` | 141/192 | 740/829 | 50 | 15 | 24 | 0 |
| `dom/traversal` | 16/18 | 1602/1608 | 6 | 0 | 0 | 0 |
| `dom/ranges` | 38/57 | 44385/44565 | 180 | 0 | 0 | 0 |
| `dom/lists` | 5/5 | 189/189 | 0 | 0 | 0 | 0 |
| `dom/collections` | 5/10 | 43/53 | 10 | 0 | 0 | 0 |
| `url` | 18/35 | 8502/8679 | 177 | 0 | 0 | 0 |
| `console` | 7/12 | 51/56 | 5 | 0 | 0 | 0 |
| `hr-time` | 7/13 | 34/51 | 17 | 0 | 0 | 0 |
| `html/webappapis/atob` | 1/1 | 380/380 | 0 | 0 | 0 | 0 |
| `html/webappapis/timers` | 10/13 | 14/17 | 3 | 0 | 0 | 0 |
| `html/dom/elements` | 58/67 | 1721/1776 | 54 | 1 | 0 | 0 |
| `WebCryptoAPI/digest` | 2/5 | 164/535 | 371 | 0 | 0 | 0 |
| `xhr/formdata` | 15/18 | 77/80 | 3 | 0 | 0 | 0 |
| `html/semantics/forms/the-form-element` | 7/18 | 107/124 | 11 | 2 | 4 | 0 |
| **Total** | **501/742** | **70743/71907** | **1093** | **28** | **37** | **6** |

## ROI by area — 2026-07-13

Where score is cheapest to win, from the same data. Available
gain is the non-passing subtest count (sorted descending);
gain per affected file is its density — high values mean one
root cause likely flips many subtests. Harness-broken files
never report (usually one missing API hangs the page) and their
real gain is understated, since their unreported subtests count
zero. Near-ok files are at most two subtests away from a clean
file.

| Area | Available gain | Affected files | Gain/file | Harness-broken | Near-ok |
|------|----------------|----------------|-----------|----------------|---------|
| `WebCryptoAPI/digest` | 371 | 3 | 123.7 | 0 | 0 |
| `dom/nodes` | 231 | 107 | 2.2 | 13 | 63 |
| `dom/ranges` | 180 | 19 | 9.5 | 0 | 3 |
| `url` | 177 | 17 | 10.4 | 0 | 6 |
| `dom/events` | 89 | 51 | 1.7 | 23 | 25 |
| `html/dom/elements` | 55 | 9 | 6.1 | 1 | 5 |
| `hr-time` | 17 | 6 | 2.8 | 1 | 4 |
| `html/semantics/forms/the-form-element` | 17 | 11 | 1.5 | 2 | 8 |
| `dom/collections` | 10 | 5 | 2.0 | 0 | 4 |
| `dom/traversal` | 6 | 2 | 3.0 | 0 | 1 |
| `console` | 5 | 5 | 1.0 | 0 | 5 |
| `html/webappapis/timers` | 3 | 3 | 1.0 | 0 | 3 |
| `xhr/formdata` | 3 | 3 | 1.0 | 0 | 3 |
| `dom/lists` | 0 | 0 | - | 0 | 0 |
| `html/webappapis/atob` | 0 | 0 | - | 0 | 0 |

## Top 10 improvements — 2026-07-13

Root-cause clusters mined from the 2026-07-13 runs, ranked by expected
subtest gain inside the tracked slice. Unlike the tables above, this list
is analysis, not arithmetic — the scripts do not regenerate it. Refresh it
(re-cluster the failing subtests) whenever the scores move materially, and
date the heading.

NOTE on layering: `NodeIterator`/`TreeWalker`/`Range`/`NodeFilter` and
much of the DOM are implemented in **`data/js/polyfills.js`**, which
OVERRIDES the C in `src/js.c`. Fix the polyfill, not the dead C path —
grep `polyfills.js` first.

| # | Improvement | Evidence | Est. gain |
|---|-------------|----------|-----------|
| 1 | **`dom/events/scrolling` + `scrollend`** — ~26 files need real scroll + `scrollend` infrastructure (programmatic scroll, scroll-snap, `scrollIntoView`, arrow-key scroll, iframe windows). Each waits on a scroll/scrollend event the engine does not generate, so they time out. | `scrolling/scrollend-*` | ~90 |
| 2 | **URL long tail** — `IdnaTestV2` residue (~58), `url-setters` (~33), `a-element` (23), `toascii` (18), and query percent-encoding that must use the document charset for legacy encodings (8). | `url/*` | ~140 |
| 3 | **`moveBefore` CSS animation/transition continuity** — `moveBefore/continue-css-*` and `css-transition-*` keep a running animation/transition alive across a move; they need animation-state preservation through reparenting and currently time out as harness-broken files. | `moveBefore/{continue,css}-*` | ~10 files |
| 4 | **dom/events small clusters** — `relatedTarget.window.html` (6), `EventListener-handleEvent-cross-realm` (5), `event-global-extra.window.html` (5), `AddEventListenerOptions-passive.any.html` (4), `Event-dispatch-detached-input-and-change` (4). | dom/events | ~24 |
| 5 | ~~**Single activation target**~~ — **landed (2026-07-13).** Click activation resolves the spec's activation target (nearest inclusive ancestor, bubbles-gated for dispatchEvent), label forwarding, summary self-only toggle, fragment navigation + hashchange, submit/reset form-boundary propagation stop, javascript: hrefs. | `Event-dispatch-single-activation-behavior` 85→**132/132** (Edge: 120); `Event-dispatch-click` 26→**33/33** | landed (~90) |
| 6 | ~~**Prefixed animation/transition event files**~~ — **landed (2026-07-13).** Generic event-only transition channels for arbitrary properties (`NS_CSS_ANIM_TARGET_OTHER`), `ontransition*` window handler props, and the headless settle-loop mutation-flag fix (relayouts every 200 ms instead of eating mutations behind a 2 s + rAF gate). | `webkit-{animation,transition}-*-event` 22→**52/52** | landed (~30) |
| 7 | ~~**Processing-instruction attributes**~~ — **landed (2026-07-13).** WICG declarative-partial-updates: PI `get/set/has/remove/toggleAttribute` over `data` in polyfills.js; the lexbor fork marks `<?`-origin bogus comments so `<?target data?>` parses to a real PI node (real `<!--?...?-->` comments untouched). | `processing-instruction-attributes` 52→**140/140** (Edge: 124) | landed (~88) |
| 8 | ~~**moveBefore-nodeiterator page freeze**~~ — **landed (2026-07-13).** NodeIterator pre-remove steps now run on all four insertion-move paths, and the traversal's detached-ref retry is capped — previously an unbreakable C-side infinite loop that also froze real pages. | `moveBefore-nodeiterator` TIMEOUT→pass | landed |
| 9 | **`WebCryptoAPI/digest` tentative vectors** — 371 non-passing subtests are `tentative` SHA-3 / cSHAKE / KangarooTwelve / TurboSHAKE vectors for not-yet-standardized algorithms OpenSSL does not provide; the mandatory `digest.https.any.html` surface passes fully. | `digest` tentative vectors | non-goal |
| 10 | **`dom/ranges/tentative/OpaqueRange-*`** — ~170 subtests for a tentative, not-yet-standardized API; the non-tentative dom/ranges residue is small (shadow-tree edge cases). | `tentative/OpaqueRange-*` | non-goal |

Not listed by design: the `navigator.*` device APIs are project non-goals.

## Whole-suite score

Full browsers are compared by total passing subtests across the
entire WPT suite — a scale where Chrome scores roughly 6,000,000.
Running all of WPT through Nordstjernen is impractical (days of
wall-clock at the per-test timeout), so this score is estimated by
sampling: a deterministic random sample of test files drawn from
every browser-runnable testharness.js test in the checkout, run
through the headless mode, with the mean passing-subtest count per
file extrapolated to the whole population. The 95% interval is a
bootstrap over the sample.

```sh
scripts/wpt-estimate.sh --wpt-root=~/wpt
```

Caveats, so the number is read honestly: only browser-runnable
testharness tests are counted (worker and service-worker variants,
reftests, crashtests, and wdspec tests are excluded — Chrome's
headline number includes those, so the "% of Chrome" column slightly
flatters Nordstjernen); tests whose harness never reports (hung page,
missing API) contribute zero even though they contain subtests; and
the subtest-per-file distribution is heavy-tailed, hence the wide
interval. Treat the trend, not the point value, as the signal.

| Date | Nordstjernen | WPT | Sample | Est. passing subtests (95% CI) | % of Chrome (~6M) |
|------|--------------|-----|--------|--------------------------------|-------------------|
| 2026-06-12 | 9526465 | 3be6ba111 | 250 of 29259 | ~44,000 (22,000 – 72,000) | ~0.7% |
| 2026-06-13 | fcfeaf1 | d8a8414e5 | 250 of 29259 | ~255,256 (21,184 – 706,429) | ~4.3% |
| 2026-06-19 | 85e29df | 1df6b93 | 369 of 29346 (area-stratified) | ~292,700 (50,800 – 733,000) | ~4.9% |
| 2026-06-24 | db8b09e | f6fd39b13 | 200 of 29414 | ~76,182 (18,678 – 177,955) | ~1.3% |

The 2026-06-19 row uses an **area-stratified** sample (proportional
allocation across the 20 largest top-level areas with a floor of 8
files each, extrapolated per area and summed) rather than the flat
random sample of the earlier rows, so the css/html majority is
weighted correctly. The estimate is dominated by `html`
(~206k of the total; ~31 passing subtests per file over n=69) with
smaller high-leverage contributions from `wasm` (~73/file, n=8) and
`svg` (~50/file, n=8) — those tiny n drive most of the wide interval.
`css` is 25% of the suite but only ~2.5 passes/file (parsing passes,
layout assertions mostly fail), and `referrer-policy`/`navigation-api`/
`webrtc`/`websockets` contribute ~0. The wide CI means the trend, not
the point value, is the signal.

Runtime estimate: a 696-file tracked-slice run on this Windows/MSYS2
machine took 36m25s, or about 3.1 seconds per file with the 15000 ms
browser timeout. Extrapolated to the 29259 browser-runnable
testharness files counted by the estimator, a testharness-only
all-suite run would take about 25 to 26 hours. The timeout-heavy
upper bound is about 5 days at the browser timeout alone, or about
8.5 days with the script's 25 second outer timeout. Including worker
and service-worker variants, reftests, crashtests, and wdspec would
push a literal all-WPT run toward several days, and some of those
harness types are not wired into Nordstjernen's current headless
runner yet.

## Running the slice

Each run uses the headless runner with default settings (15000 ms
per-test timeout) against a current WPT checkout:

```sh
scripts/dev.sh build
scripts/wpt-score.sh --wpt-root=~/wpt
```

Partial runs rerun just the given areas (or sub-paths of them) after
an engine change, without paying for the full slice:

```sh
scripts/wpt-score.sh --wpt-root=~/wpt dom/nodes url
scripts/wpt-score.sh --wpt-root=~/wpt dom/events/Event-constructors.any.js
```

Fresh results replace the matching rows in `docs/wpt-subtests.tsv`;
everything not rerun carries over from the previous run, and the
regenerated tables always cover the whole slice.

## Data files

`docs/wpt-runs/DATE-REV.tsv` is the per-test-file snapshot of each
run — one row per test file with the harness status and subtest
pass/fail/timeout/notrun/precondition_failed counts. Diffing two of
them shows which test files regressed or improved between revisions.

`docs/wpt-subtests.tsv` is the canonical per-subtest state — one row
per individual subtest (test file, subtest name, status), plus a
`<harness>` row per file. Each run overwrites it, so its git history
is the subtest-level time series: `git log -p --
docs/wpt-subtests.tsv` shows exactly which named subtests flipped
between any two runs. Failure messages are not committed (noisy and
large); reproduce them with `scripts/wpt-run.sh --results=FILE`.

## Improving the score

The loop, drivable end-to-end by a Claude session:

1. Take the highest unclaimed entry from the Top 10 improvements
   list (falling back to the ROI table when the list is stale), or
   find the individual test files with the most failing subtests:

   ```sh
   sort -t$'\t' -k5 -rn docs/wpt-runs/2026-06-27-e92c9b1.tsv | head -20
   ```

2. List the failing subtests by name, and the files whose harness
   never completes (usually a missing API hanging the page):

   ```sh
   awk -F'\t' '$2 != "<harness>" && $3 != "PASS"' docs/wpt-subtests.tsv | grep '^/dom/ranges/'
   awk -F'\t' '$2 == "<harness>" && $3 != "OK"' docs/wpt-subtests.tsv
   ```

3. Reproduce one test with full failure messages:

   ```sh
   ./builddir/src/gtk/nordstjernen --wpt http://web-platform.test:8000/dom/ranges/Range-attributes.html
   ```

4. Fix the engine, rebuild, and rerun just the affected area:

   ```sh
   scripts/wpt-score.sh --wpt-root=~/wpt dom/ranges
   ```

5. Commit the engine change together with the regenerated doc and
   data files. The `docs/wpt-subtests.tsv` diff is the proof of which
   subtests flipped.

## Tracked slice

`dom/nodes`, `dom/events`, `dom/traversal`, `dom/ranges`,
`dom/lists`, `dom/collections`, `url`, `console`, `hr-time`,
`html/webappapis/atob`, `html/webappapis/timers`,
`html/dom/elements`, `WebCryptoAPI/digest`, `xhr/formdata`,
`html/semantics/forms/the-form-element`

The slice is fixed so results stay comparable between runs; WPT
itself moves, so small drifts in totals between WPT revisions are
expected. If the slice ever changes, start a new history table.
