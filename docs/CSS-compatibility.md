# CSS compatibility

How Nordstjernen tracks the **CSS specifications**
(<https://www.w3.org/Style/CSS/specs.en.html>).

This is a module-by-module map of how Nordstjernen behaves against the
CSS specs. Like the [HTML compatibility](HTML-compatibility.md)
document, it is measured against the **spec text**, not against any
other browser — Nordstjernen is a clean-room C / GTK 4 / libcurl
implementation with no upstream engine. It is a living map, not a
guarantee; the browser's runtime behaviour is the source of truth.
Re-check any row by running the browser against a page that exercises
the feature (see [How to re-check](#how-to-re-check-this-document)).

Snapshot: **0.8.2-dev**, 2026-05-31.

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
| `@import` | 🟡 | external stylesheets are fetched and cascaded; `@import` inside a sheet is parsed but only top-level `<link>`/`<style>` sheets are fully chained |
| Origins & specificity ordering | ✅ | UA → presentational hints → author; `!important` honoured; specificity (id/class/type) computed and ordered |
| Inheritance | ✅ | per-property inheritance table (`prop_inherits` in `src/css.c`) |
| `inherit` / `initial` / `unset` / `revert` | 🟡 | `inherit`/`initial` honoured for most properties; `revert` approximates `unset` |
| Shorthand expansion | ✅ | `margin`/`padding`/`border`/`background`/`font`/`flex`/`grid`/`gap`/`place-*`/`columns`/`outline`/`column-rule`/`inset`/`text-decoration` and the logical-property shorthands |
| Custom properties (`--x`) + `var()` | ✅ | registered and substituted; `@property` registers `initial-value`/`inherits`/`syntax` (syntax parsed, not type-validated) |

## Values & units (Values 4)

| Topic | Status | Notes |
|-------|:--:|------|
| `px`, `%`, `em`, `rem` | ✅ | `em`/`rem` resolved against a 16px root in the layout fast path |
| Viewport units `vw`/`vh`/`vmin`/`vmax`, `sv*`/`lv*`/`dv*`, `vi`/`vb` | ✅ | |
| Container units `cqw`/`cqh`/`cqi`/`cqb`/`cqmin`/`cqmax` | ✅ | resolved against the nearest container (`container-type`/`container-name`) |
| Font-relative `cap`, `ic`, `ch`, `ex` | 🟡 | `cap`/`ic` supported; `ch`/`ex` approximated |
| Absolute `Q`, `pt`, `cm`, `mm`, `in`, `pc` | ✅ | exact CSS ratios from `1in = 96px = 2.54cm` (`pt = 96/72`, `cm = 96/2.54`, `mm = 96/25.4`, `Q = 96/101.6`) |
| `calc()` | ✅ | percentage + px mix; nested |
| Math `round()` / `mod()` / `rem()` / `abs()` / `min()` / `max()` / `clamp()` | ✅ | the Values 4 length-math subset |

## Box model & sizing (Box 3, Sizing 3, Overflow 3)

| Topic | Status | Notes |
|-------|:--:|------|
| `display` block/inline/inline-block/flex/grid/table*/list-item/flow-root/none | ✅ | |
| `box-sizing` | ✅ | `content-box`/`border-box` |
| margins / padding / borders / `border-radius` | ✅ | incl. per-corner radii; margin collapsing for adjacent block margins |
| `min/max-width`, `min/max-height` | ✅ | |
| Intrinsic `min-content` / `max-content` / `fit-content` | ✅ | shrink-to-fit via content measurement |
| `aspect-ratio` | ✅ | verified: width-driven height from ratio |
| `overflow` / `overflow-x` / `overflow-y` (`visible`/`hidden`/`clip`/`auto`/`scroll`) | ✅ | scrollable boxes get working scrollbars + hit-testing |
| `scrollbar-width` (`auto`/`thin`/`none`) · `scrollbar-color` | ✅ | `none` hides the overlay scrollbar, `thin` narrows it; `scrollbar-color` themes thumb/track (inherited) |
| `text-overflow: ellipsis` | ✅ | verified on `white-space:nowrap` clipped boxes |
| `object-fit` / `object-position` | ✅ | replaced-element sizing |
| `visibility` (`visible`/`hidden`/`collapse`) | ✅ | |

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
| Auto-placement & implicit rows (`grid-auto-rows`) | ✅ | row-major flow with spanning |
| `justify-items` / `justify-self` (inline axis) | ✅ | start/center/end size the item to content and offset it within its column span (`layout_grid` in `src/layout.c`) |
| `align-items` / `align-self` (block axis) | ✅ | stretch/center/end within the row; rows stretch to a taller container height first |
| `align-content` (row group) | ✅ | stretch (distribute), center, end, and space-between / -around / -evenly all honoured |
| `justify-content` (column group) | ✅ | center / end / space-between / -around / -evenly position the column group when tracks don't fill the container; `1fr` tracks fill, so that case is a no-op |
| CSS Nesting (`&`, nested rules) | ✅ | verified |
| `place-items` / `place-self` / `place-content` shorthands | ✅ | expand to both axes; `display:grid; place-items:center` centres a box, verified |
| Column-axis `subgrid` | 🟡 | adopts the parent grid's spanned columns; row-axis subgrid falls back to auto rows |

## Multi-column (Multicol 1)

| Topic | Status | Notes |
|-------|:--:|------|
| `column-count` | ✅ | distributes block-level children across balanced columns |
| `column-width` / `columns` shorthand | ✅ | used column count derived per spec (`floor((avail+gap)/(width+gap))`); verified on `<ol>`/`<ul>` reference lists |
| `column-gap` | ✅ | |
| `column-rule` (`-width`/`-style`/`-color`) | ✅ | divider painted between filled columns (`src/paint.c`) |
| Fragmenting a single inline/text run across columns | ❌ | columns require ≥2 block-level children to distribute; a single text block renders as one full-width column |

## Tables (Tables 3)

| Topic | Status | Notes |
|-------|:--:|------|
| `display: table` family | ✅ | driven off the `table`/`table-row`/`table-cell`/… display values |
| `table-layout: auto` / `fixed` | 🟡 | fixed uses col/first-row widths; auto is an approximation |
| `border-collapse` / `border-spacing` | ✅ | separate (with spacing) and collapse both render cleanly; in collapse mode shared interior edges are de-duplicated to a single grid line — a cell drops its right/bottom border when a neighbour (honouring colspan/rowspan) owns that edge (`table_collapse_borders` in `src/layout.c`) |
| `caption-side: top` / `bottom` | ✅ | |
| `vertical-align` in cells | ✅ | top/middle/bottom; baseline≈top |
| `<col>` / `<colgroup>` width hints | ✅ | seed column widths in auto and fixed layout |

## Typography & text (Text 3/4, Fonts 4, Writing Modes 4)

| Topic | Status | Notes |
|-------|:--:|------|
| `color`, `font-family`/`size`/`weight`/`style`/`variant`, `line-height` | ✅ | Pango-backed |
| `text-align` (`left`/`right`/`center`/`justify`/`start`/`end`) | ✅ | |
| `text-decoration` (line/style/color), `text-shadow` | ✅ | |
| `text-transform` (uppercase/lowercase/capitalize) | ✅ | |
| `text-indent` (incl. negative sprite-hiding) | ✅ | |
| `letter-spacing` / `word-spacing` / `tab-size` | ✅ | |
| `white-space` (`normal`/`nowrap`/`pre`/`pre-wrap`/`pre-line`/`break-spaces`) | ✅ | |
| `word-break` / `overflow-wrap` / `text-wrap`(`-mode`) | ✅ | wrapping keywords mapped |
| `font-variant: small-caps` | ✅ | |
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
| 2D/3D `transform` (translate/scale/rotate/skew/matrix), `transform-origin` | ✅ | |
| `transition` (`opacity`/`transform`/`color`/`background-color`) | ✅ | `src/anim.c`; respects `prefers-reduced-motion` |
| `@keyframes` + `animation` | 🟡 | opacity/transform/color/bg-color targets; `animation-direction` (normal/reverse/alternate/alternate-reverse) and `animation-fill-mode` (none/forwards/backwards/both) honoured |
| Easing (`linear`/`ease`/`ease-in`/`-out`/`-in-out`, `steps()`, `step-start`/`step-end`, `cubic-bezier()`) | ✅ | steps() jump terms and a Newton-Raphson cubic-bezier solver in `src/anim.c` |

## Visual effects (Filter Effects 1, Compositing 1, Masking 1)

| Topic | Status | Notes |
|-------|:--:|------|
| `opacity` | ✅ | |
| `filter` (grayscale/sepia/invert/brightness/contrast/saturate/blur) | ✅ | on images; `blur()` is a separable box blur; drop-shadow still limited |
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
| Structural (`:first/last/only-child`, `:first/last/only-of-type`, `:nth-child`/`:nth-of-type`/`-last-*`, `:nth-child(… of S)`, `:empty`, `:root`) | ✅ |
| Logical `:is()` / `:where()` / `:not()` / `:has()` | ✅ (bounded) |
| Links/state `:link`/`:visited`/`:any-link`/`:hover`/`:active`/`:focus`/`:focus-within`/`:focus-visible`/`:target` | ✅ |
| Forms `:checked`/`:disabled`/`:enabled`/`:required`/`:optional`/`:valid`/`:invalid`/`:read-only`/`:read-write`/`:placeholder-shown` | ✅ |
| `:lang()` / `:dir()` / `:defined` / `:open` / `:popover-open` / `:scope` | ✅ |
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
| `@page` / `@layer` | 🟡 | `@page` margins limited; `@layer` ordering simplified |

---

## Design constraints

Project non-goals (see `CLAUDE.md` / `README.md`), not defects:

- No CSS that requires **WebGL/WebGPU** or AI-style surfaces.
- No reliance on **Web/Service Workers** for style (e.g. paint worklets,
  `@property` registered via JS Houdini are not a goal).
- **Writing modes** and full bidi are not yet implemented.
- `border-image` and printing (`@page`) beyond basics are out of scope
  for now.

## Highest-leverage CSS gaps for real-world sites

Ordered by how often they block ordinary browsing:

1. `writing-mode` / full bidi — absent; affects CJK and some RTL layouts.
2. Fragmenting a single text block across multiple columns — only
   block-child distribution is supported (a multi-paragraph block works;
   a single text run does not yet split across columns).

(`border-collapse` shared-edge de-duplication and exact absolute units
have since been implemented.)

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
