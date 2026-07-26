Changelog:
=========
Significant changes in each release:

1.0.21:
======

CSS
* Inherited properties set on the root element reach the rest of the
  page. The UA stylesheet declared `color`, `font-family`, `font-size`
  and `line-height` on `html, body` together, and a UA declaration on
  `body` outranks inheritance from `html` — so a page styling only
  `html` (`html{font-family:"Helvetica Neue","Segoe UI",Arial,
  sans-serif}` on lite.duckduckgo.com) had its font, colour and size
  dropped at `body` and rendered in the UA serif default. The
  declarations now sit on `html` alone and `body` inherits them.
* A concrete font family is used when the system actually has it.
  `Arial`, `Helvetica`, `Segoe UI`, `Roboto` and the SF Pro names were
  rewritten to generic `sans-serif` unconditionally, which resolved
  through fontconfig to whatever the default sans happened to be —
  Noto Sans rather than the requested Segoe UI or Arial. Each name is
  now resolved against the installed families first, and substituted
  by `sans-serif` only when it is missing.
* `calc()` serializes per CSS Values 4 instead of being echoed back as
  authored. A typed math sum — one coefficient per unit, sitting beside
  the px/pct/em/rem value layout uses — sums terms of the same type,
  folds absolute lengths, angles, times, frequencies and resolutions to
  their canonical unit, and distributes products and quotients by a
  number. A sum that cannot reduce to a single term serializes sorted:
  number, then percentage, then dimensions in ASCII-alphabetical unit
  order. `calc(1px + 1%)` is `calc(1% + 1px)`,
  `calc(1px + 2em + 3rem + 4%)` is `calc(4% + 2em + 1px + 3rem)`,
  `calc(2 * (1px + 1em))` is `calc(2em + 2px)`, and a single-argument
  `min()`/`max()` reduces to `calc()`. The quad shorthands serialize the
  same sum rather than dropping every term but px, so
  `margin: calc(1px + 1em) 2px` no longer reads back as `1px 2px`.
  Comparisons that need layout, such as `min(20px, 10%)`, still stay as
  authored.
* `border-image` is implemented (CSS Backgrounds 3): the five longhands
  (`border-image-source`/`-slice`/`-width`/`-outset`/`-repeat`), the
  `border-image` shorthand and its `-webkit-` alias parse, cascade,
  serialize canonically and reach `getComputedStyle`; the `border`
  shorthand resets them. The painter nine-slices the source — raster
  `url()` images and gradients alike — honouring `fill`, percentage and
  number slices, `auto`/length/percentage/number widths, outsets and all
  four `stretch`/`repeat`/`round`/`space` tiling modes, and replaces the
  element's border style while it renders.
* Media Queries Level 4: the heuristic matcher is replaced by a real
  evaluation engine (`src/css_media.c`) with grammar-complete parsing
  (range syntax, boolean context, nested conditions, `and`/`or`/`not`,
  general-enclosed), Kleene three-valued logic and CSSOM media-list
  serialization (`not all` for unparseable queries). Iframe documents
  evaluate against their own viewport, `matchMedia` and
  `CSSMediaRule.media` expose the serialized form, `CSSMediaRule.media`
  is a real `MediaList`, and changing a `media` attribute restyles.
* The native `sheet` / `document.styleSheets` stubs that shadowed the
  real CSSOM are gone. `style.sheet.cssRules` now returns the parsed
  rule tree and `style.sheet === document.styleSheets[i]` holds.
* A script read of a resolved value flushes every pending mutation, not
  just the first one in the task. `getComputedStyle`,
  `getBoundingClientRect`, `offsetWidth`, `scrollIntoView` and the rest
  force a synchronous reflow whenever the document is dirty; the
  wall-clock interval and the oscillation dampener now apply only to the
  rendering tick, where they belong. Mutating a style and reading it
  back in the same task returns the new value, and `:has()`
  invalidation, inset resolution and stylesheet insertion are observable
  immediately.
  Two gaps this makes visible, which the frozen styles had been hiding:
  `scrollWidth`/`scrollHeight` are wrong on inline-level boxes
  (`inline-block`, `inline-flex`, `inline-grid`), and `attr()` is
  substituted when generated content is rendered but not when
  `getComputedStyle().content` is serialized.
* Cascade layers are ordered as a tree rather than by first-declaration
  order across the whole document: sublayers sort inside their parent,
  a layer's own declarations act as its implicit final sublayer, and
  nested anonymous layers stay nested instead of escaping to the top
  level.
* The incremental restyle pass identifies stylesheets by a parse-time
  serial instead of by address. A reparsed `<style>` reusing the freed
  block of the sheet it replaced used to look unchanged, which froze the
  page at stale styles.
* `@scope` preludes are parsed against the grammar and invalid ones drop
  the rule; the prelude is serialized canonically.
* `StyleSheet.media` is a live `MediaList` that writes back to the
  owner node's `media` attribute, and `ShadowRoot.styleSheets` is empty
  for a disconnected tree.
* CSS Display Level 3: `display` is a structured computed value — outer
  type, inner type, list-item flag and layout-internal kind — resolved
  once in the cascade instead of a keyword string that layout, paint and
  the CSSOM each re-read with `strcmp`. Multi-word canonical forms now
  reach layout, so `display: flow-root list-item` keeps its box instead
  of losing it; blockification of floated and absolutely positioned
  boxes has one implementation rather than three; `-webkit-box` and
  `-webkit-inline-box` map to flex and inline-flex.
* Anonymous table boxes are generated around any run of table-internal
  siblings, per CSS 2.1 17.2.1, so `display: table-row` and
  `display: table-row-group` outside a table lay out as tables instead
  of collapsing into the surrounding inline content.
* CSSOM: declaration blocks are canonicalized, rule mutations apply
  synchronously, constructed stylesheets are backed by live rules,
  at-rules are exposed on declarations, and shorthand serialization
  covers the quad shorthands, `all` and the complete shorthand
  families.
* `getComputedStyle` resolved values: insets absolutize against the
  correct containing block, static insets follow the writing mode,
  automatic minimum sizes resolve, and pseudo-element styles compute.
* Declaration grammar is enforced: values with unbalanced brackets or
  quotes, stray `!` or `;`, out-of-place `auto`/`normal`, negative
  values on non-negative properties, or transform functions with bad
  units are dropped instead of being half-parsed.
* `content-visibility: hidden` applies size containment, and
  `writing-mode: vertical-rl/lr` with `text-orientation`
  `upright`/`mixed`/`sideways` measures and paints vertical inline runs.
* Flexbox honours the automatic (content-based) minimum size, and
  column stretch no longer double-counts item margins.

Layout and rendering
* Box `x`/`y` uniformly means the margin-box origin, which fixes flex
  and grid items with margins rendering and measuring double-shifted;
  `getBoundingClientRect` derives the border box the same way the
  painter does. Grid row placement was still adding the item's top
  margin on top of that origin, so a negative margin moved the item the
  wrong way by twice the amount.
* Flex items are sized by the flex algorithm rather than by their own
  `width`. `layout_block` used to read `width` back out of the style and
  ignore the main size the container had assigned, so nothing ever
  shrank — `flex-shrink: 1` is the initial value, so every
  over-constrained flex row overflowed instead of fitting.
* The flex main axis is reversed when exactly one of
  `flex-direction: row-reverse` and `direction: rtl` applies, and items
  are then packed from the opposite edge. `row-reverse` used to reverse
  the item order but still pack against the left edge, and `rtl` was
  ignored for the main axis entirely.
* `scrollWidth`/`scrollHeight` measure the real scrollable overflow
  region from descendant border boxes, including overflow from
  negative margins on non-scrolling boxes.
* Escaping floats and compact line boxes lay out correctly, the
  non-rendering elements (`area`, `base`, `link`, `meta`, `param`,
  `source`, `track`, …) are excluded from inline layout, anonymous
  table cells lay out as blocks, and malformed legacy declarations are
  skipped rather than derailing the rest of the block.

HTML, DOM and JavaScript
* Core content-attribute reflection is complete, and the text-control
  selection APIs (`selectionStart`/`End`/`Direction`,
  `setSelectionRange`, `textLength`) match the spec including per-type
  applicability and the `IndexSizeError`/`InvalidStateError` cases.
* Mutation observers and shadow trees align with the spec: old-value
  records for attributes and character data, `attachShadow` options,
  `assignedSlot`, and `ShadowRoot.styleSheets`/`activeElement`/
  `elementFromPoint`.
* Frames are isolated per document: events, event handlers and the
  `Performance` objects belong to the frame's own realm, so scripts
  inside an iframe see their own `window` and `document`.
* Speedometer 3.1 fixes: nested custom-element upgrades during
  construction, unhandled rejections queued to the microtask
  checkpoint, module scripts in iframes seeing the frame realm,
  `ownerDocument` returning the realm wrapper, frame windows in the
  event propagation path, images in frames resolving relative URLs
  against the frame document, and the always-rejecting
  `navigator.wakeLock` stub removed so feature detection falls back
  cleanly.
* Promise rejection events: cancelable `unhandledrejection` carrying
  `promise`/`reason`, with `rejectionhandled` for rejections handled
  later.
* `XMLHttpRequest` gains the upload object, the full progress event
  set, `responseXML` and method/state validation; `MessageEvent` and
  `ExtendableMessageEvent` follow the messaging spec.
* `Navigator` and friends (`MimeTypeArray`, `PluginArray`,
  `NetworkInformation`, `StorageManager`, `UserActivation`,
  `NavigatorUAData`, `MediaCapabilities`, `MediaDevices`) are real
  interfaces with non-constructible prototypes, so brand and
  `instanceof` checks agree.
* Cookie Store API: promise-based `cookieStore.get`/`getAll`/`set`/
  `delete` over the document cookie jar.
* Legacy media and timing surfaces: the `HTMLMediaElement`/
  `MediaError` constants, `PerformanceTiming` and
  `PerformanceNavigation`; `blob:` URLs work as module script sources.
* `<input type=email>` validation follows the HTML email grammar, and
  Trusted Types plus void-element serialization are tightened.
* Reading an `<iframe>`'s `contentDocument` or `contentWindow` more than
  once no longer aborts the process. The realm document's API was
  installed onto the per-node wrapper every time it was requested, and
  the second install hit QuickJS's "property already exists" abort in
  `JS_DefineAutoInitProperty`. The install now runs once per wrapper, so
  repeat reads return the same document, as the DOM requires.

Identity and privacy
* `Sec-CH-UA` and `navigator.userAgentData` report `"Nordstjernen"`
  instead of impersonating Chromium and Google Chrome;
  `getHighEntropyValues` fills in `formFactors`, `platformVersion`,
  `architecture`, `bitness`, `wow64`, `model`, `uaFullVersion` and
  `fullVersionList`.
* New `--private` command-line flag starts the browser in private
  browsing mode.
* The Chrome compatibility token in the user agent moves to 150.

Media
* MSE segments are tracked with their byte offset and probed time
  range. `SourceBuffer.remove()` rebuilds the helper input from the
  initialization segment plus the retained segments, which keeps
  long-running adaptive playback (YouTube, Vimeo) inside its byte
  quota, and `SourceBuffer.buffered` reports the retained demux range
  instead of a synthetic zero-based one. `remove()` no longer refuses
  ranges that are not a clean prefix.
* `@font-face` sources are only fetched when their `unicode-range`
  covers a codepoint the page actually uses, so font-heavy pages stop
  competing with streaming playback for bandwidth.
* `<video>`/`<audio>` expose `seeking` and `played`, and fire
  `ratechange` and the seek events.

Layout
* A flex item with padding or a border is no longer sized smaller than
  its content. The automatic flex base size came from
  measure_natural_width(), which already reports a content width, and
  then subtracted the item's own padding and border a second time — the
  surrounding code tracks those separately. Items came out exactly one
  padding-and-border narrower than they should be, so buttons and labels
  in a flex row wrapped mid-phrase for no reason. A row of consent
  buttons that Chrome lays out as two single-line pills was wrapping to
  two lines each; it now matches. Items without padding were unaffected,
  which is why this survived so long.

Headless
* `--dump=png` no longer crashes on pages that keep scripting busy while
  media is fetched. The video prefetch held box pointers across a
  blocking fetch, and the nested main loop that fetch runs can relayout
  the page and free them underneath it. It now resolves the URLs, then
  re-finds the box before attaching, so no box outlives a fetch.

Performance
* Container queries no longer defeat incremental restyle. The second
  cascade pass — the one that runs with container sizes known — took the
  branch that throws the previous pass's computed styles away, so every
  page using `@container` re-cascaded every element from scratch on
  every relayout, forever. The cache now survives that pass, and rules
  carrying a container condition no longer force conservative
  invalidation keys either: those rules are inert in the first pass,
  which is the only one incremental restyle runs in. On a 1610-element
  container-query page churning through 13 relayouts, cascade time drops
  from 47ms to 12ms and style reuse goes from 0 to 1609 of 1610
  elements per pass.
* A `:has()` selector whose subject is matched by an attribute — say
  `[data-state]:has(...)` — keys invalidation on that attribute instead
  of switching incremental restyle off for the whole page. `class`, `id`
  and `style` are excluded, since keying on those matches nearly every
  element and floods the document anyway.
* `:nth-child`/`:nth-last-child` sibling indices are computed once per
  selector-matching batch instead of per element.
* Live DOM collections use the QuickJS array-index atom fast path, and
  childlist invalidation is narrowed to the affected parent.

User interface
* The `about:start` splash is redesigned as a 1997 Netscape release
  screen — beveled chrome, dithered sky, receding cyberspace grid —
  then reworked into a daytime scene whose ground is a procedurally
  generated planet surface sampled through the real pinhole
  projection, with an ocean-to-snow terrain ramp, clouds, a specular
  sun column and atmospheric haze into the limb. The loading bar is
  gone and the animation is 322 KB, down from 1.8 MB.
* Android: the kebab icon is readable on the toolbar, nested consent
  dialogs scroll, and the renderer thread no longer frees the
  shell-owned framebuffer.

Documentation, build and CI
* A whole-system architecture poster (`docs/Software-Architecture.png`)
  maps the process tree, IPC boundaries, engine pipeline, module
  dependencies and information flows; the generators live in
  `scripts/arch-diagram/`.
* The CSS and HTML compatibility documents are refreshed, and
  `docs/media.md` describes the MSE eviction model.
* CI hardens the V8 artifact download with retries and integrity
  checks, and the V8 smoke test now covers microtasks and timers.
* The readme links the openSUSE RPM directly.
