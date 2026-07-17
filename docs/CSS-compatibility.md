# CSS compatibility

How Nordstjernen tracks the **CSS specifications**
(<https://www.w3.org/Style/CSS/specs.en.html>).

This is a module-by-module map of how Nordstjernen behaves against the
CSS specs. Like the [HTML compatibility](HTML-compatibility.md)
document, it is measured against the **spec text**, not against any
other browser — Nordstjernen is a C / GTK 4 / libcurl
implementation with no upstream engine. It is a living map, not a
guarantee; the browser's runtime behaviour is the source of truth.
Re-check any row by running the browser against a page that exercises
the feature (see [How to re-check](#how-to-re-check-this-document)).

Snapshot: **1.0.10**, 2026-06-18.

**Legend:** ✅ implemented · 🟡 partial / approximated · ❌ absent ·
🚫 absent by design (a project non-goal — see
[Design constraints](#design-constraints)).

**Engine map** (the files referenced throughout):

| Concern | Source |
|---------|--------|
| Tokenizer, parser, cascade, selector matching | `src/css.c`, `src/css.h` |
| Value/unit resolution, `calc()` | `src/css.c` (`length_resolve` in `src/layout.c`) |
| Box layout (block/inline/flex/grid/table/multicol/float/position) | `src/layout.c`, `src/layout.h` |
| Paint (Cairo): backgrounds, borders, shadows, gradients, filters | `src/paint.c`, `src/render.c` |
| Text / fonts (Pango) | `src/font.c`, `src/paint.c` |
| Transitions / `@keyframes` animation | `src/anim.c` |
| UA stylesheet | the `kUa` sheet embedded in `src/css.c` |

---

## Syntax & cascade (CSS Syntax 3, Cascade 5)

| Topic | Status | Notes |
|-------|:--:|------|
| Tokenizer / declaration & rule parsing | ✅ | `src/css.c`; tolerant of malformed input per spec error-recovery |
| Selector lists, declaration blocks | ✅ | |
| `@import` | ✅ | external stylesheets are fetched and cascaded; nested `@import` chains are expanded before the importing sheet, `media` filters are honoured, and `<link>` / `<style>` author sheets cascade in document order |
| Origins & specificity ordering | ✅ | UA → presentational hints → author; `!important` honoured; specificity (id/class/type) computed and ordered |
| Inheritance | ✅ | per-property inheritance table (`prop_inherits` in `src/css.c`) |
| `inherit` / `initial` / `unset` / `revert` | ✅ | CSS-wide keywords honoured in the cascade; `revert` rolls author declarations back to lower-origin UA results, while `revert-layer` remains folded into the simplified layer model |
| Shorthand expansion | ✅ | `margin`/`padding`/`border`/`background`/`font`/`flex`/`grid`/`gap`/`place-*`/`columns`/`outline`/`column-rule`/`inset`/`text-decoration` and the logical-property shorthands |
| Custom properties (`--x`) + `var()` | ✅ | registered and substituted; `@property` registers `initial-value`/`inherits`/`syntax` (syntax parsed, not type-validated) |

## Values & units (Values 4)

| Topic | Status | Notes |
|-------|:--:|------|
| `px`, `%`, `em`, `rem` | ✅ | `em`/`rem` resolved against a 16px root in the layout fast path |
| Viewport units `vw`/`vh`/`vmin`/`vmax`, `sv*`/`lv*`/`dv*`, `vi`/`vb` | ✅ | |
| Container units `cqw`/`cqh`/`cqi`/`cqb`/`cqmin`/`cqmax` | ✅ | resolved against the nearest container (`container-type`/`container-name`) |
| Font-relative `cap`, `ic`, `ch`, `ex` | ✅ | font-measured at cascade time (`resolve_em_units` in `src/css.c` via a Pango metrics callback in `src/paint.c`): `ch` is the advance of `0`, `ex` the x-height of `x`, `cap` the cap-height of `H`, `ic` the advance of `水`, all measured for the element's computed family/size/weight/style; they respond to the actual font (e.g. `1ch` differs between monospace, serif, and bold). Falls back to the old `0.5/0.7/1.0em` factors if no metrics provider is registered, and `calc()`/`background-size`/box-shadow keep the factor-based approximation |
| Absolute `Q`, `pt`, `cm`, `mm`, `in`, `pc` | ✅ | exact CSS ratios from `1in = 96px = 2.54cm` (`pt = 96/72`, `cm = 96/2.54`, `mm = 96/25.4`, `Q = 96/101.6`) |
| `calc()` | ✅ | percentage + px mix; nested |
| Math `round()` / `mod()` / `rem()` / `abs()` / `min()` / `max()` / `clamp()` | ✅ | the Values 4 length-math subset |

## Box model & sizing (Box 3, Sizing 3, Overflow 3)

| Topic | Status | Notes |
|-------|:--:|------|
| `display` block/inline/inline-block/flex/grid/table*/list-item/flow-root/none | ✅ | |
| `box-sizing` | ✅ | `content-box`/`border-box` |
| margins / padding / borders / `border-radius` | ✅ | incl. per-corner radii; margin collapsing for adjacent block margins; a side whose `border-style` is `none`/`hidden` contributes zero used border width per Backgrounds 3. `getComputedStyle` returns the CSSOM **resolved (used) value** in px for `margin-*`/`padding-*`/`border-*-width` and for `top`/`right`/`bottom`/`left` on positioned boxes (percentages resolved against the containing block), and `line-height`/`font-weight` resolve to used px / the numeric weight (`ns_computed_lookup` in `src/js.c`) |
| `min/max-width`, `min/max-height` | ✅ | |
| Intrinsic `min-content` / `max-content` / `fit-content` | ✅ | shrink-to-fit via content measurement |
| `aspect-ratio` | ✅ | verified: width-driven height from ratio |
| `overflow` / `overflow-x` / `overflow-y` (`visible`/`hidden`/`clip`/`auto`/`scroll`) | ✅ | scrollable boxes get working scrollbars + hit-testing |
| `scrollbar-width` (`auto`/`thin`/`none`) · `scrollbar-color` | ✅ | `none` hides the overlay scrollbar, `thin` narrows it; `scrollbar-color` themes thumb/track (inherited) |
| `text-overflow: ellipsis` | ✅ | verified on `white-space:nowrap` clipped boxes |
| `object-fit` / `object-position` | ✅ | replaced-element sizing |
| `visibility` (`visible`/`hidden`/`collapse`) | ✅ | |
| `content-visibility` | ❌ | not parsed or laid out yet; HTML `hidden="until-found"` fragment reveal is handled in navigation code rather than through skipped-content layout |

## Positioned layout (Position 3)

| Topic | Status | Notes |
|-------|:--:|------|
| `position: static / relative / absolute / fixed` | ✅ | |
| `position: sticky` | ✅ | verified: header pins within its scroll container |
| `top`/`right`/`bottom`/`left`, `inset` shorthand | ✅ | |
| `z-index` & stacking contexts | ✅ | opacity/transform/filter establish contexts |
| Top layer (`dialog` modal, `::backdrop`) | 🟡 | modal `<dialog>` painted in the top layer; popover top-layer limited |

## Flexbox (Flexbox 1)

| Topic | Status | Notes |
|-------|:--:|------|
| `flex-direction` row/row-reverse/column/column-reverse | ✅ | |
| `flex-wrap` | ✅ | |
| `flex-grow` / `flex-shrink` / `flex-basis` / `flex` shorthand | ✅ | |
| `justify-content` (start/end/center/space-between/around/evenly) | ✅ | |
| `align-items` / `align-self` (start/end/center/stretch/baseline) | ✅ | baseline approximated |
| `align-content` for wrapped lines | ✅ | verified: center/flex-end/space-*/stretch pack the cross-axis line group when the container has spare cross size (`layout_flex_row_wrap` in `src/layout.c`) |
| `gap` / `row-gap` / `column-gap` | ✅ | |
| `order` | ✅ | |

## Grid (Grid 1/2)

| Topic | Status | Notes |
|-------|:--:|------|
| `grid-template-columns` / `-rows` (px, %, `fr`, `auto`, `minmax()`, `repeat()`, `auto-fit`/`auto-fill`) | ✅ | |
| `grid-template-areas` | ✅ | named-area placement |
| `grid-column` / `grid-row` / `-start` / `-end` / `grid-area` (line numbers & spans) | ✅ | |
| `gap` / `row-gap` / `column-gap` | ✅ | |
| Auto-placement (`grid-auto-flow`, `grid-auto-rows`, `grid-auto-columns`) | ✅ | row-major flow with spanning; `grid-auto-flow: dense` back-fills earlier holes; `grid-auto-flow: column` flows column-major down the explicit rows, creating implicit columns sized by `grid-auto-columns` (cycled, `auto` fallback) after the template tracks (`layout_grid` in `src/layout.c`); the legacy `grid-gap`/`grid-row-gap`/`grid-column-gap` aliases map to `gap`/`row-gap`/`column-gap` |
| `justify-items` / `justify-self` (inline axis) | ✅ | start/center/end size the item to content and offset it within its column span (`layout_grid` in `src/layout.c`) |
| `align-items` / `align-self` (block axis) | ✅ | stretch/center/end within the row; rows stretch to a taller container height first |
| `align-content` (row group) | ✅ | stretch (distribute), center, end, and space-between / -around / -evenly all honoured |
| `justify-content` (column group) | ✅ | center / end / space-between / -around / -evenly position the column group when tracks don't fill the container; `1fr` tracks fill, so that case is a no-op |
| CSS Nesting (`&`, nested rules) | ✅ | verified |
| `place-items` / `place-self` / `place-content` shorthands | ✅ | expand to both axes; `display:grid; place-items:center` centres a box, verified |
| `subgrid` | 🟡 | column-axis subgrid adopts the parent grid's spanned columns; row-axis subgrid adopts concrete parent row tracks (`grid-template-rows` / fixed `grid-auto-rows`) and gap, while fully auto-sized parent rows still fall back to the regular auto-row path |

## Multi-column (Multicol 1)

| Topic | Status | Notes |
|-------|:--:|------|
| `column-count` | ✅ | distributes block-level children across balanced columns |
| `column-width` / `columns` shorthand | ✅ | used column count derived per spec (`floor((avail+gap)/(width+gap))`); verified on `<ol>`/`<ul>` reference lists |
| `column-gap` | ✅ | |
| `column-rule` (`-width`/`-style`/`-color`) | ✅ | divider painted between filled columns (`src/paint.c`) |
| Fragmenting a single inline/text run across columns | ✅ | plain inline text runs are split by wrapped line into balanced column fragments; links and text-style ranges are preserved |

## Tables (Tables 3)

| Topic | Status | Notes |
|-------|:--:|------|
| `display: table` family | ✅ | driven off the `table`/`table-row`/`table-cell`/… display values |
| `table-layout: auto` / `fixed` | 🟡 | fixed uses col/first-row widths; auto is an approximation |
| `border-collapse` / `border-spacing` | ✅ | separate (with spacing) and collapse both render cleanly; in collapse mode shared interior edges are de-duplicated to a single grid line — a cell drops its right/bottom border when a neighbour (honouring colspan/rowspan) owns that edge (`table_collapse_borders` in `src/layout.c`) |
| `caption-side: top` / `bottom` | ✅ | |
| `vertical-align` in cells | ✅ | top/middle/bottom; baseline≈top |
| `<col>` / `<colgroup>` width hints | ✅ | seed column widths in auto and fixed layout |

## Lists (Lists 3)

| Topic | Status | Notes |
|-------|:--:|------|
| `list-style-type` (disc/circle/square, decimal & leading-zero, lower/upper alpha/latin/roman, lower-greek, `none`) | ✅ | `format_ordered_label` in `src/paint.c` |
| `list-style-position` (`outside`/`inside`) | ✅ | |
| `list-style-image` | ✅ | `url(...)` markers fetched through the page image pipeline and painted in place of the bullet (`paint_marker` in `src/paint.c`), scaled down to the line's font size when larger; falls back to the generated marker until the image loads |
| `list-style` shorthand | ✅ | type/position/`url()`/`none` |
| `::marker` | ✅ | see Selectors; `content` overrides win over `list-style-image` |
| Legacy `<ol start/reversed/type>` / `<li value/type>` / `<ul type>` | ✅ | presentational hints + ordinal numbering |

## Generated content (Content 3)

| Topic | Status | Notes |
|-------|:--:|------|
| `content` on `::before`/`::after` (strings, `attr()`, counters) | ✅ | see Selectors |
| `quotes` + `open-quote`/`close-quote`/`no-open-quote`/`no-close-quote` | ✅ | author quote pairs (with CSS string escapes like `"\00AB"`) drive both `<q>` rendering — nesting depth picks the pair, the last pair repeating — and `open-quote`/`close-quote` in pseudo-element `content`; `quotes: none` suppresses the marks; the default pairs are `“ ” ‘ ’` (`quotes_string_for` in `src/layout.c`) |

## User interface (UI 4)

| Topic | Status | Notes |
|-------|:--:|------|
| `cursor` | ✅ | parsed and inherited; the computed keyword at the hovered point is resolved by the engine (`ns_browser_cursor_at` in `src/libnordstjernen.c`: hit-test, inherited `cursor`, the last recognised keyword of a `url(...)`-fallback list, validated against the CSS Basic UI keyword set) and carried over the renderer IPC (the per-motion `/hover` reply, and the `/link` reply); the GTK shell passes the keyword straight to GDK's named-cursor lookup. `auto` resolves per the spec's UA behaviour: pointer over links, a text I-beam over selectable text (`user-select: none` text excluded), text inputs / textareas, and `contenteditable` hosts, default elsewhere; custom `url(...)` images fall back to their keyword |
| `pointer-events: none` | ✅ | |
| `user-select: none` | ✅ | text in the subtree is skipped by drag selection, select-all, highlight painting, and clipboard collection (`src/selection.c`); treated as inherited, approximating the spec's `auto` resolution. Other values (`all`, `contain`) behave as `auto` |
| `accent-color` / `caret-color` | ✅ | see Color |

## Typography & text (Text 3/4, Fonts 4, Writing Modes 4)

| Topic | Status | Notes |
|-------|:--:|------|
| `color`, `font-family`/`size`/`weight`/`style`/`stretch`/`kerning`/`variant`, `line-height` | ✅ | Pango-backed; inline font/style ranges participate in width and line-height measurement; `font-stretch` keywords and percentages map to Pango's stretch buckets; `font-kerning`, ligature controls, raw `font-feature-settings`, and `font-variation-settings` map to OpenType features |
| `text-align` (`left`/`right`/`center`/`justify`/`start`/`end`) | ✅ | |
| `text-decoration` (line/style/color), `text-shadow` | ✅ | the `text-decoration-line` longhand is parsed and feeds the same line state as the shorthand |
| `text-transform` (uppercase/lowercase/capitalize) | ✅ | |
| `text-indent` (incl. negative sprite-hiding) | ✅ | |
| `letter-spacing` / `word-spacing` / `tab-size` | ✅ | |
| `white-space` (`normal`/`nowrap`/`pre`/`pre-wrap`/`pre-line`/`break-spaces`) | ✅ | |
| `word-break` / `overflow-wrap` / `text-wrap`(`-mode`) / `hyphens` | ✅ | wrapping keywords mapped; `hyphens:auto` enables Pango inserted hyphens using the element language, while `manual`/`none` keep automatic insertion disabled |
| `font-variant: small-caps`, `font-variant-ligatures`, `font-feature-settings`, `font-variation-settings` | ✅ | common/discretionary/historical/contextual ligature keywords, quoted four-character feature tags, and variable-font axes map to Pango OpenType settings |
| `direction` / `unicode-bidi`, `:dir()` | 🟡 | base direction drives Pango + `text-align:start/end`; full bidi override partial |
| `writing-mode` (vertical-rl/lr, sideways) | ❌ | text always lays out horizontally |
| Ruby (`display:ruby`) | 🟡 | parsed; no ruby positioning |

## Backgrounds & borders (Backgrounds 3)

| Topic | Status | Notes |
|-------|:--:|------|
| `background-color` / `-image` / `-repeat` / `-position` / `-size` | ✅ | incl. multiple background layers and stylesheet-relative `url(...)` |
| Gradients: `linear-gradient` / `radial-gradient` / `conic-gradient` | ✅ | incl. `repeating-*` variants (stop pattern tiles via `CAIRO_EXTEND_REPEAT` / angular modulo), `%` and `px` colour-stop positions, the double-position shorthand (`#222 0 20px`), and `at <position>` centring (radial sizes to the farthest corner from the centre) |
| `background-clip` (`border-box`/`padding-box`/`content-box`) | ✅ | clips background colour/image/gradient to the chosen box; `text` falls back to border-box |
| `box-shadow` (incl. inset, multiple) | ✅ | |
| `border-image` | ❌ | |
| `outline` (`-width`/`-style`/`-color`/`-offset`) | ✅ | |

## Color (Color 4/5)

| Topic | Status | Notes |
|-------|:--:|------|
| Named, `#hex`, `rgb()`/`rgba()`, `hsl()`/`hsla()` | ✅ | modern space/slash syntax |
| `hwb()`, `lab()`/`lch()`, `oklab()`/`oklch()` | ✅ | converted to sRGB |
| `color-mix(in srgb, …)` | ✅ | |
| `currentColor`, `transparent` | ✅ | |
| `accent-color` / `caret-color` | ✅ | |

## Transforms, transitions, animation (Transforms 1/2, Transitions 1, Animations 1)

| Topic | Status | Notes |
|-------|:--:|------|
| 2D/3D `transform` (translate/scale/rotate/skew/matrix), `transform-origin` | ✅ | `getComputedStyle().transform` returns the CSSOM **resolved value** — the function list is composed into a single `matrix(...)` (2D) or `matrix3d(...)` (3D, column-order) via `ns_css_transform_to_mat4`, with translate percentages resolved against the border box (`ns_computed_transform_matrix` in `src/js.c`); `none` when no transform |
| `transition` (`opacity`/`transform`/`color`/`background-color`) | ✅ | `src/anim.c`; respects `prefers-reduced-motion` |
| `@keyframes` + `animation` | 🟡 | opacity/transform/color/bg-color targets; `animation-direction` (normal/reverse/alternate/alternate-reverse) and `animation-fill-mode` (none/forwards/backwards/both) honoured |
| Easing (`linear`/`ease`/`ease-in`/`-out`/`-in-out`, `steps()`, `step-start`/`step-end`, `cubic-bezier()`) | ✅ | steps() jump terms and a Newton-Raphson cubic-bezier solver in `src/anim.c` |

## Visual effects (Filter Effects 1, Compositing 1, Masking 1)

| Topic | Status | Notes |
|-------|:--:|------|
| `opacity` | ✅ | |
| `filter` (grayscale/sepia/invert/brightness/contrast/saturate/blur/drop-shadow) | ✅ | on images; `blur()` is a separable box blur; `drop-shadow()` uses the image alpha mask, CSS lengths, and parsed colors/currentColor |
| `mix-blend-mode` | ✅ | |
| `clip-path` (basic shapes) | ✅ | |
| `mask-image` (`url()` + linear/radial gradient) | ✅ | gradient masks composite the whole element through the gradient's alpha (fade effects); `mask`/`-webkit-mask` aliased; conic masks skipped |
| `image-rendering` (`pixelated`/`crisp-edges`) | ✅ | nearest-neighbour upscaling for raster `<img>` |
| `-webkit-line-clamp` | ✅ | |

## Selectors (Selectors 4)

| Group | Status |
|-------|:--:|
| Type / `*` / `.class` / `#id` / `[attr]` (all matchers, `i` flag) | ✅ |
| Combinators (descendant, `>`, `+`, `~`) | ✅ |
| Structural (`:first/last/only-child`, `:first/last/only-of-type`, `:nth-child`/`:nth-of-type`/`-last-*`, `:nth-child(… of S)`, `:empty`, `:root`) | ✅ | `:empty` follows Selectors 4: comments and document whitespace do not block a match; element children and non-whitespace text do |
| Logical `:is()` / `:where()` / `:not()` / `:has()` | ✅ (bounded) |
| Links/state `:link`/`:visited`/`:any-link`/`:hover`/`:active`/`:focus`/`:focus-within`/`:focus-visible`/`:target`/`:target-within` | ✅ | `:hover` and `:active` are live in the GUI: hover tracks the pointer, and `:active` matches the pressed element and its ancestors between the primary-button press (`ns_browser_click`) and release (`ns_browser_release`); restyles only run when the page has `:hover`/`:active` rules |
| Forms `:checked`/`:default`/`:indeterminate`/`:disabled`/`:enabled`/`:required`/`:optional`/`:valid`/`:invalid`/`:in-range`/`:out-of-range`/`:read-only`/`:read-write`/`:placeholder-shown`/`:blank` | ✅ | `:checked` matches an `option` by its live selectedness — including the implicitly selected first option of a non-`multiple` select — not just the `selected` content attribute |
| `:lang()` / `:dir()` / `:defined` / `:open` / `:popover-open` / `:modal` / `:scope` | ✅ |
| Pseudo-elements `::before`/`::after`/`::first-letter`/`::first-line`/`::marker`/`::placeholder`/`::selection`/`::backdrop` | ✅ (`::backdrop` partial) |

## At-rules & queries (Conditional 3/4, MQ 4/5, Containment 3, `@scope`)

| Topic | Status | Notes |
|-------|:--:|------|
| `@media` (width/height/orientation/aspect-ratio/resolution, range syntax, `prefers-*`, color-gamut, etc.) | ✅ | broad MQ4/5 feature coverage |
| `@supports` (incl. `selector()`) | ✅ | evaluated |
| `@font-face` | ✅ | |
| `@keyframes` | 🟡 | see animation |
| `@property` | ✅ | `initial-value` + `inherits` honoured; `syntax` parsed |
| `@scope` | ✅ | roots/limits, `:scope`, proximity |
| `@container` + `container-type`/`container-name` | ✅ | container query units resolve |
| `@layer` | 🟡 | ordering simplified (named-layer rank tracking in `src/css.c`) |
| `@page` | 🚫 | no paged/print path; the at-rule is not parsed |

## CSSOM (object model — CSSOM 1)

Scripted access to stylesheets and rules (`src/js.c`, `data/js/polyfills.js`).
`getComputedStyle` resolved values are covered in the rows above.

| Topic | Status | Notes |
|-------|:--:|------|
| `document.styleSheets` | ✅ | live `StyleSheetList` of `CSSStyleSheet` for `<style>` / `<link rel=stylesheet>`; the same object is returned across reads |
| `sheet.cssRules` / `.rules` | ✅ | a stable `CSSRuleList` of real rule objects parsed from the owner node, with `.item()` and indexed access |
| `CSSRule` / `CSSStyleRule` types | ✅ | proper prototype chain (`CSSStyleRule` → `CSSRule`) and `type` constants (`STYLE_RULE`=1, `MEDIA_RULE`=4, …); read-only `type` / `parentRule` / `parentStyleSheet` |
| `CSSStyleRule.style` | ✅ | a live `CSSStyleDeclaration` (`[SameObject]`, `[PutForwards=cssText]`); `cssText` serializes the declaration block canonically |
| `CSSStyleRule.selectorText` | 🟡 | getter + validating setter (invalid selectors rejected, re-cascades on change); the selector text is kept largely verbatim |
| `insertRule` / `deleteRule` (sheet + grouping) | ✅ | index validation (`IndexSizeError`), parse errors (`SyntaxError`), `@import`/`@namespace` constraints; mutates the live `CSSRuleList` in place |
| `CSSGroupingRule` / `CSSMediaRule` / `CSSSupportsRule` | ✅ | nested `cssRules`; `CSSMediaRule` serializes its media query list (lower-cased types/features, leading `all and` dropped) |
| `CSSStyleDeclaration` (`el.style`) | ✅ | `getPropertyValue` / `setProperty` / `removeProperty` / `getPropertyPriority`, indexed `item()` + `length`, `Symbol.iterator`, `cssText` round-trip |
| `new CSSStyleSheet()` + `adoptedStyleSheets` | 🟡 | `replace` / `replaceSync` apply styles and `adoptedStyleSheets` works on documents and shadow roots; rule introspection on a *constructed* sheet is limited |
| Full selector serialization | ❌ | the CSSOM "serialize a group of selectors" canonicalisation (e.g. `*\|div` → `div`, `:after` → `::after`) is not implemented |

---

## Design constraints

Project non-goals (see `CLAUDE.md` / `README.md`), not defects:

- No CSS that requires **WebGL/WebGPU** or AI-style surfaces.
- No reliance on **Web/Service Workers** for style (e.g. paint worklets,
  `@property` registered via JS Houdini are not a goal).
- **Writing modes** and full bidi are not yet implemented.
- `border-image` and printing (`@page`) are out of scope for now —
  neither is implemented.

## Highest-leverage CSS gaps for real-world sites

Ordered by how often they block ordinary browsing:

1. `writing-mode` / full bidi — absent; affects CJK and some RTL layouts.

(`border-collapse` shared-edge de-duplication and exact absolute units
and single-text-run multi-column fragmentation have since been implemented.)

## How to re-check this document

There is **no automated conformance suite** (by project policy).
Validate any row by running the browser against a page that exercises
the feature and observing the result, e.g.:

```sh
nordstjernen --headless --url=FILE --viewport=900 --dump=png:out.png
```

The fixtures under `data/render-tests/` (`grid-align.html`,
`multicol-columnwidth.html`, `flex.html`, `grid-markers.html`,
`table.html`, `selectors-color4.html`, `units.html`, …) exercise much of
the surface above. `--inspect=SELECTOR` / `--inspect-at=X,Y` print the
box model and key computed styles for any element, like a browser's
inspector. Treat this file as a living map and update it whenever
behaviour changes.
