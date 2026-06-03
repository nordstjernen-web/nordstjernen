# HTML compatibility

How Nordstjernen tracks the **WHATWG HTML Living Standard**
(<https://html.spec.whatwg.org/>).

This is a section-by-section walk through the entire spec, from §1
*Introduction* to §16 *Obsolete features*, recording how Nordstjernen
behaves against each. It is measured against the **spec text**, not
against any other browser — Nordstjernen is a clean-room C / GTK 4 /
libcurl implementation with no upstream engine. It is a living map,
not a guarantee; the browser's runtime behaviour is the source of
truth. Re-check any row by running the browser against a page that
exercises the feature.

This document focuses on the HTML spec proper and summarises adjacent
CSS, DOM, networking, media, and security surfaces where the spec
references them.

Snapshot: **0.8.2-dev**, 2026-05-31.

**Legend:** ✅ implemented · 🟡 partial / stubbed · ❌ absent ·
🚫 absent by design (a project non-goal — see
[Design constraints](#design-constraints-the-spec-we-deliberately-do-not-implement)).

**Engine map** (the files referenced throughout):

| Concern | Source |
|---------|--------|
| HTML tokenizer + tree construction | in-tree lexbor, `src/html_lexbor.c` |
| Parse dispatch, charset decode | `src/html.c` |
| DOM tree + node APIs | `src/dom.c`, exposed in `src/js.c` |
| WHATWG URL | lexbor URL module via `src/net.c` (`nd_url_*`) |
| CSS cascade / selectors | `src/css.c`, `src/css.h` |
| Layout (block/inline/flex/grid/table) | `src/layout.c`, `src/layout.h` |
| Paint (Cairo) / text (Pango) | `src/paint.c`, `src/render.c`, `src/font.c` |
| JavaScript (QuickJS-ng, interpreter) | `src/js.c`, `src/js.h` |
| Networking | `src/net.c`, cookies/cache in `src/cache.c` |
| Images / media | `src/image*.c`, `src/video.c`, `src/media.c` |
| Security (CSP/SOP/sandbox) | `src/csp.c`, `src/security.c` |

---

## §1 Introduction

Informative; nothing to implement. Nordstjernen targets the HTML5
*standards* processing model — documents are parsed and laid out in
standards mode (see [§13](#13-the-html-syntax)).

## §2 Common infrastructure

| Topic | Status | Notes |
|-------|:--:|------|
| WHATWG URL parsing & serialisation | ✅ | lexbor URL module via `nd_url_resolve`, `nd_url_parts_new`, `nd_url_host_from`, `nd_url_origin_from` in `src/net.c` |
| IDN / Punycode | ✅ | handled inside the lexbor URL module |
| Origin & same-origin/same-site | ✅ | `nd_url_same_origin`, `nd_url_is_same_site` (`src/net.c`) |
| Character encodings → UTF-8 | ✅ | uchardet detection in `nd_html_decode_body` (`src/html.c`), `g_convert` to UTF-8, Latin-1 last-resort |
| Content-type sniffing | 🟡 | charset sniffing delegated to uchardet; no full MIME sniffing standard |
| Reflected content attributes / IDL | ✅ | typed reflection in `src/js.c`: string, URL (resolved to absolute), boolean (presence), `long`/`unsigned long` with defaults and spec clamping (e.g. `colSpan` → [1,1000], `rowSpan` → [0,65534]), numeric `progress`/`meter` range getters (`value`/`max`/`position`, `min`/`low`/`high`/`optimum`), and **enumerated** attributes canonicalised to known keywords with missing-/invalid-value defaults (`type`, `loading`, `decoding`, `method`, `crossOrigin`, `referrerPolicy`) |
| Microsyntaxes (numbers, dates/times, colours, tokens) | 🟡 | integer/non-negative-integer parsing drives reflection; date/month/week/time/local-date-time parsing & serialisation back the form `valueAsNumber`/`valueAsDate` APIs (`src/js.c`); the legacy-colour-value algorithm drives presentational hints (`bgcolor`/`text`/`<font color>`, `parse_legacy_color` in `src/css.c`); space/comma-separated tokens handled |
| `DOMTokenList` (`classList`, `relList`) | ✅ | generic backing-attribute token list in `src/js.c`; ASCII-whitespace tokenisation per spec; empty/whitespace tokens throw `SyntaxError`/`InvalidCharacterError`; `relList` reflects `rel` live; full iterator protocol — `values()`, `keys()`, `entries()`, `forEach`, and `Symbol.iterator`, so `for (const t of el.classList)`, `[...el.relList]`, and `Array.from(...)` all work |
| `DOMStringMap` (`dataset`) | ✅ | live `Proxy` over `data-*` attributes (read/write/delete/enumerate) with camelCase↔dash conversion; invalid names (`-` + lowercase) throw `SyntaxError` |

## §3 Semantics, structure and APIs of HTML documents

| Topic | Status | Notes |
|-------|:--:|------|
| DOM tree construction | ✅ | lexbor (`src/html_lexbor.c`); spec-faithful even for malformed input |
| `Document`, `documentElement`, `body`, `head` | ✅ | exposed in `src/js.c` |
| `document.title` | ✅ | reflects `<title>` |
| DOM tree accessors (`getElementsBy{Id,TagName,TagNameNS,ClassName,Name}`, `forms`/`images`/`links`/`scripts`) | ✅ | `getElementsByTagNameNS` applies the spec namespace filter — empty string normalised to `null`, `null` matches nothing (our DOM is XHTML-only), `*` matches any, the XHTML URI matches by local name; exposed on both `Document` and `Element` |
| `document.currentScript` | ✅ | the running classic `<script>` element during execution; `null` for module scripts (`src/js.c`) |
| `document.compatMode` | ✅ | reflects the parser's quirks state (`BackCompat` in quirks mode, else `CSS1Compat`); plumbed from lexbor's `compat_mode` via the document node flags |
| Void / raw-text / escapable element classes | ✅ | void set in `src/html.c` (`area base br col embed hr img input link meta param source track wbr`) |
| Quirks / limited-quirks / no-quirks | 🟡 | DOCTYPE consumed; `document.compatMode` now reflects the mode, but quirks-specific layout deltas are still not applied |
| `dir` global attribute (`Element.dir`, `document.dir`) | 🟡 | reflected as an enumerated IDL attribute (canonicalised to `ltr`/`rtl`/`auto`/`""`); drives Pango base direction and `text-align:start/end`, but full bidi override handling is still partial (see §6) |
| `lang`, `translate`, `accessKey` global attributes | ✅ | reflected (`translate` resolves the inherited yes/no translation mode); `lang` not yet used for font/locale selection |

### §4.2 Document metadata

| Element | Status | Notes |
|---------|:--:|------|
| `title` | ✅ | window/tab title; `title.text` IDL attribute returns/sets the child text content per spec |
| `base` (`href`, `target`) | ✅ | `href` feeds URL resolution via `nd_url_resolve` |
| `link rel="stylesheet"` | ✅ | fetched and cascaded (`src/css.c`) |
| `link rel="icon"` | ✅ | favicon fetched as image |
| `link rel="preload"`/`prefetch`/`dns-prefetch` | 🟡 | parsed as a generic link; no dedicated preload queue |
| `meta charset` | ✅ | feeds charset decode |
| `meta name="viewport"` | 🟡 | parsed; viewport width/height come from `nd_css_set_viewport` (`src/css.c`); not all directives enforced |
| `meta http-equiv` (CSP, refresh, etc.) | 🟡 | CSP/Referrer-Policy honoured where reflected; `refresh` follows the WHATWG shared-declarative-refresh parsing (digit time, `;`/`,` separators, optional `url`/`=` keyword, quoted/whitespace-trimmed URL) and is also honoured from the HTTP `Refresh` response header (`src/net.c` → `nd_window_apply_meta_refresh` in `src/main.c`) |
| `meta name="referrer"` | ✅ | Referrer-Policy applied in `src/net.c` |
| `style` (inline sheet) | ✅ | parsed and cascaded by `src/css.c` |

## §4.3 Sections · §4.4 Grouping · §4.5 Text-level semantics

All flow/sectioning/grouping/phrasing elements parse into the DOM and
lay out through the generic block/inline engine in `src/layout.c`.
They have **no element-specific code path** — their appearance comes
from a built-in **UA stylesheet** (the `kUa` sheet embedded in
`src/css.c`) plus author CSS. This matches the spec model, where these
elements are defined in terms of [§15 Rendering](#15-rendering) UA CSS.

| Group | Examples | Status |
|-------|----------|:--:|
| Sectioning | `body article section nav aside header footer address h1`–`h6` `hgroup` | ✅ via UA CSS |
| Grouping | `p hr pre blockquote ol ul menu li dl dt dd figure figcaption main div` | ✅ via UA CSS |
| Text-level | `a em strong small s cite dfn abbr code var samp kbd b i u mark span` | ✅ via UA CSS |
| Dedicated inline handling | `q` `sub` `sup` `br` `wbr` | ✅ explicit in `src/layout.c` |
| Ruby annotations | `ruby rt rp` | 🟡 parsed; rendered inline without ruby positioning |
| `data` / `time` | | ✅ reflected `data.value` and `time.dateTime` |
| `bdi` / `bdo` | | 🟡 parsed; bidi override not applied |

The UA stylesheet defines the heading scale, list markers, default
margins/padding, form-control baselines, and sets non-rendered
elements (`head title meta link style script noscript template`) to
`display:none`.

## §4.6 Links · §4.7 Edits

| Topic | Status | Notes |
|-------|:--:|------|
| `a href` navigation | ✅ | link ranges tracked in layout; navigation dispatched from `src/main.c` |
| `target` | ✅ | stored on the link range |
| `rel` (`noopener`/`noreferrer`/`nofollow`) | 🟡 | parsed; `noreferrer` interacts with Referrer-Policy; `noopener` semantics limited (single browsing context) |
| `download` | 🟡 | recognised; save flow limited |
| `HTMLHyperlinkElementUtils` (`href`/`protocol`/`host`/`hostname`/`port`/`pathname`/`search`/`hash`/`origin`) | ✅ | typed URL decomposition via `nd_element_anchor_part_get` / `nd_element_anchor_href_set` (`src/js.c`); `href` resolves to absolute on read, parses on write |
| `a.text` (descendant text content) | ✅ | per-spec dispatch in `nd_element_get_text` |
| `ins` / `del` | 🟡 | styled by UA CSS (ins green, del dark red); no edit semantics |

## §4.8 Embedded content

| Element | Status | Notes |
|---------|:--:|------|
| `img` | ✅ | layout + decode pipeline |
| `img srcset` / `sizes` | 🟡→✅ | descriptor parsing (`first_url_from_srcset_sized`) + `sizes` evaluation (`nd_css_sizes_resolve`); width & density descriptors selected by viewport/density (recent layout work) |
| `picture` / `source` | ✅ | `pick_picture_source_url` matches `media`/`type` via `nd_css_media_query_matches` |
| `img loading="lazy"` | ✅ | fetch deferred |
| Decode pipeline | ✅/🟡 | Wuffs (PNG/APNG, GIF, BMP, JPEG) → GDK-Pixbuf (WebP/TIFF/ICO) → librsvg (static SVG); AVIF if built with libavif |
| `iframe` | 🟡 | `src` loads; `sandbox` parsed but **not enforced** |
| `iframe srcdoc` | 🟡 | attribute and DOM reflection; embedded rendering still limited |
| `embed` / `object` | 🚫 | no NPAPI/PPAPI plugin dispatch |
| `video` | 🟡 | poster + play overlay; click hands the source URL to the system media player (`nd_media_launch_external`) — no in-process codec. Streaming `<video>` (MSE/`blob:`, no file URL) hands the *page* URL instead, resolved by mpv/VLC + yt-dlp |
| `audio` | 🟡 | click hands the source URL to the system media player; no in-process codec |
| `track` (captions) | ❌ | parsed; `kind`/`src`/`srclang`/`label`/`default` reflected via the standard typed-reflection path; not rendered |
| `map` / `area` (image maps) | ✅ | `<img usemap>` clicks are hit-tested against the referenced `<map>`'s `<area>` elements — `rect`/`circle`/`poly`/`default` shapes in image-local coordinates — and the first matching area's `href` is navigated (`nd_image_map_resolve` in `src/dom.c`, wired into the GUI and headless click paths) |
| MathML | ❌ | parsed into DOM; not laid out |

## §4.9 Tabular data

`table caption colgroup col thead tbody tfoot tr td th` are laid out
by the table code in `src/layout.c`, driven off the CSS
`display:table | table-caption | table-row | table-cell | …` family.

| Feature | Status | Notes |
|---------|:--:|------|
| Basic grid layout | ✅ | rows/cells sized from content |
| `colspan` / `rowspan` | ✅ | parsed and applied |
| `thead`/`tbody`/`tfoot` grouping | 🟡 | header rows rendered first and footer rows last regardless of source order (`collect_rows` in `src/layout.c`); not repeated across fragments |
| `caption` | ✅ | rendered as a table caption; `caption-side:top/bottom` supported |
| Cell `vertical-align` (`valign`) | ✅ | `vertical-align: top/middle/bottom` on table cells positions content within the row's resolved height (`src/layout.c`); the legacy `valign` attribute maps onto it. `baseline` (the default) is approximated as `top` |
| `col`/`colgroup` width hints | ✅ | `<col>`/`<colgroup>` `width` (length or percentage, honouring `span`) and explicit per-cell `width` now seed column widths in **auto** layout too, not just `table-layout:fixed` (`src/layout.c`): a column with an explicit width is pinned (clamped to its min-content), flexible columns absorb the remaining width, and when all columns are pinned the leftover is distributed proportionally |
| Fixed vs auto `table-layout` | 🟡 | `table-layout:fixed` uses `col`/`colgroup` hints, first-row cell widths, then equal remaining columns; auto layout remains an approximation |
| `border-spacing` / `cellspacing` | ✅ | inherited `border-spacing` (one or two lengths) consumed by the auto and fixed table layout (`table_border_spacing` in `src/layout.c`): horizontal spacing sits before/between/after columns and is reserved out of the available width, vertical spacing before/between/after rows; the UA sheet defaults `table` to `border-spacing: 2px` so plain tables get the spec inter-cell gap, and the legacy `cellspacing` attribute maps onto it |
| Border-collapse model | ✅ | `border-collapse: separate` (default) vs `collapse` honoured: collapse forces `border-spacing` to zero **and** de-duplicates shared interior edges to a single grid line via a cell-occupancy grid (`table_collapse_borders` in `src/layout.c`, honouring colspan/rowspan) |

## §4.10 Forms

A first-class, functional surface: `src/layout.c` renders the
controls; `src/js.c` wires up submission, `FormData`, and constraint
validation.

| Control / feature | Status |
|-------------------|:--:|
| `input` text/password/search/email/url/tel/number | ✅ (editable with a rendered caret and a highlighted text selection; keyboard caret/selection navigation — arrows, Home/End, Shift+move to extend, Ctrl+A select-all, Ctrl+C/X copy/cut — and selection-replacing edits. **Rendering & CSS** (`collect_walk` in `src/layout.c`): the author `color`, `font-size`, and `font-family` of the control apply to the value text and field metrics; an unfocused value longer than the field is clipped from its **start** (head shown), while a focused field scrolls to keep the caret in view; disabled controls render their text greyed. Author `border`/`background`/`background-image`/`border-radius`/`padding` are honoured via the CSS-chrome path. The **`::placeholder`** pseudo-element is supported (parsed in `src/css.c`, resolved like the other pseudo-elements): placeholder text takes the pseudo's `color`, `font-style`, and `font-weight`, falling back to a muted UA grey (`#757575`) when unstyled; the legacy `::-webkit-input-placeholder` / `::-moz-placeholder` / `::-ms-input-placeholder` aliases map to it. When the control is positioned, a flex/grid item, or a `display:block` box, it gets a real layout box and the field chrome fills the assigned width (so e.g. a `flex:1` or `width:100%` search input fills the bar instead of staying at its intrinsic `size` width — `paint_inline` in `src/paint.c`). `text-align: center`/`right` position the value within the field (the field's filler is distributed before/after the value), and the value is kept on a single line. Pending: a per-field `text-align` does not yet re-position a value that is wider than the field) |
| `input` checkbox/radio | ✅ |
| `input` button/submit/reset | ✅ |
| `input type=number` | ✅ (Up/Down arrow keys step by `step`, default 1, clamped to `min`/`max`, firing input/change; non-numeric keystrokes filtered out; no rendered spin buttons) |
| `maxlength` while editing | ✅ (user typing/paste cannot grow a text input or textarea past `maxlength`; programmatic `.value` unrestricted per spec) |
| `input` file/color/range | ✅ (`<input type=file>.files` returns a live `FileList` of `File` objects after the user picks — each carries `name`, `size`, `type` from `g_content_type_guess`, and `Blob`-shaped bytes, so `await file.text()` / `await file.arrayBuffer()` work and `new FormData()` over the input serialises the bytes through the shared multipart path) |
| `input` date/time/datetime-local/month/week | 🟡 (text-style entry; no native picker) |
| `textarea` | ✅ (multi-line: newlines preserved as line breaks, height from `rows`, border box grows to enclose content; caret and text selection rendered; an empty textarea shows its `placeholder` in the UA grey / author `::placeholder` colour, and the control's `color`/`font-size`/`font-family` apply to the value — shared with the `<input>` path via `emit_control_text_style` in `src/layout.c`) |
| `select` / `option` / `optgroup` | 🟡 (rendered; DOM options/selectedOptions collections, add/remove, spec-compliant `option.text` — descendant text minus script subtrees, ASCII-whitespace-stripped + collapsed — `option.label` (label attr, falling back to `option.text`), `optgroup.label` reflected, `option.value` (value attr, falling back to `option.text`), single/multiple `value`; the select popup and form submission both consult `option.label` / `option.value` so legacy markup with extra whitespace or inline children now submits the same string a browser would; a `multiple` or `size>1` select renders as a multi-line listbox (selected options marked), a plain select as a popup-style field; no native popup picker) |
| `datalist` | 🟡 (parsed; no suggestion UI) |
| `button` (submit/reset/button) | ✅ |
| `output` | 🟡 (`value` maps to text content) |
| `progress` | ✅ (determinate + indeterminate rendered bars; numeric `value`, `max`, `position` IDL getters) |
| `meter` | ✅ (spec min/max/value/low/high/optimum gauge algorithm; optimum/suboptimal/less-good regions reflected in rendering and numeric IDL getters) |
| `fieldset` / `legend` | 🟡 (UA-styled) |
| Constraint validation (`required` `readonly` `pattern` `min` `max` `step` `minlength` `maxlength`) | 🟡 (JS bindings incl. form-wide `checkValidity()`, custom validity, built-in validation messages, non-bubbling `invalid`, and spec-scoped required/read-only/pattern/length handling; native submit checks required text/select/checkbox/radio controls, email lists with `multiple`, URL-parser-backed `type=url`, pattern/type/length, numeric and temporal `min`/`max`/`step`, and custom validity) |
| `FormData` (`append`/`set`/`entries`/…) | ✅ | `append(name, blob, filename)` honours the third-argument filename per spec |
| Form ownership / successful controls | ✅ (`form="id"` owners, disabled fieldsets, default checkbox/radio `"on"` values, multi-select values, form.elements named lookup/RadioNodeList, associated submit/reset activation with cancelable reset events) |
| Submission, `application/x-www-form-urlencoded` | ✅ (HTML `+` space encoding; `requestSubmit()` validates/fires `SubmitEvent` with `submitter`, while `submit()` bypasses both) |
| Submission, `multipart/form-data` | ✅ (full UTF-8 serialiser: native form submit (`src/main.c`), and `fetch`/`XMLHttpRequest` bodies of `FormData` and `URLSearchParams` (`src/js.c` `nd_js_form_data_serialize` / `nd_js_usp_serialize`) — CSPRNG boundary, per-entry `Content-Disposition`, Blob/File parts get a `Content-Type` from `blob.type` (default `application/octet-stream`) and the entry's filename (or `blob` if unspecified), name/filename quoted per WHATWG (only LF/CR/`"` escaped), and `URLSearchParams` bodies auto-pick `application/x-www-form-urlencoded;charset=UTF-8` — caller-set `Content-Type` always wins) |
| `formaction`/`formmethod`/`formenctype` overrides | ✅ |

## §4.11 Interactive elements

| Element | Status | Notes |
|---------|:--:|------|
| `details` / `summary` | 🟡 | open/close transitions dispatch the spec's `ToggleEvent` pair — a (non-cancelable) `beforetoggle` then a `toggle`, each carrying `oldState` and `newState` ∈ `"open"`/`"closed"`; opening one `<details name="X">` closes the rest of the group per the exclusive-accordion rule (`nd_js_details_toggle_open` in `src/js.c`, hooked from the click path, the `open` IDL setter, and `setAttribute`/`removeAttribute`); UA-styled disclosure; no animation |
| `dialog` | ✅ | `open`/`show()`/`showModal()`/`close(result)` and `returnValue` implemented (`src/js.c`); `method="dialog"` forms close the dialog with the submitter's value; `requestClose(returnValue?)` (and an Escape press on the topmost open modal) fires a cancelable `cancel` event and, if not prevented, closes the dialog and fires `close` — matching the spec's close-watcher semantics. `showModal()` now puts the dialog in the top layer (painted on top of an author `::backdrop` fill, `src/paint.c`), moves focus to its `autofocus`/first focusable descendant, traps focus by making the rest of the document inert, and restores focus to the opener on close |
| `popover` attribute | 🟡 | open/closed state, `showPopover`/`hidePopover`/`togglePopover`, `popovertarget` activation, and target/action reflection; open/close transitions dispatch the spec `ToggleEvent` pair — a `beforetoggle` (cancelable on open, so `preventDefault()` keeps the popover closed) then a `toggle`, each with `oldState`/`newState`; limited top-layer behaviour |

## §4.12 Scripting

| Topic | Status | Notes |
|-------|:--:|------|
| `script` inline / external | ✅ | `nd_js_run_scripts_in_doc` (`src/js.c`); `script.text` returns/sets the child text content per spec |
| `async` / `defer` | ✅ | `defer` delays to end of parse |
| `type="module"` / `nomodule` | 🟡 | modules detected and run; full module graph/`import` resolution limited |
| Engine | ✅ | QuickJS-ng (in-tree, **interpreter only, no JIT** — W^X holds); ≈ES2020+; per-call eval budget plus a 60 s absolute execution monitor that halts a runaway page (armed on the outermost JS entry, enforced in the interrupt callback — `src/js.c`) |
| `noscript` | ✅ | `display:none` when JS enabled |
| `template` (`.content`) | ✅ | `template.content` is a `DocumentFragment` (snapshot clone of the parsed children); descendants of a `<template>` are hidden from the document's tree-walk: `document.querySelectorAll`, `getElementsByTagName`/`ClassName`/`Name`, `getElementById`, the CSS selector engine, and the id/class/tag indexes all stop at a `<template>` and don't descend into its children. The child accessors on the template element — `firstChild`, `lastChild`, `firstElementChild`, `lastElementChild`, `children`, `childNodes`, `childElementCount`, `hasChildNodes()` — all report empty/null, matching the spec model where template content lives in the content fragment rather than as children of the template element. Only `template.content`-rooted queries see the parsed nodes |
| `slot` / shadow projection | 🟡 | `attachShadow` + slot assignment (bounded) |
| `canvas` 2D context | ✅ | full Cairo-backed `CanvasRenderingContext2D` (paths, text, `drawImage`, gradients/patterns, `get/putImageData`, compositing, shadows) |
| `canvas` WebGL/WebGPU context | 🚫 | by design |
| `OffscreenCanvas` | 🟡 | constructs; no worker thread |

## §4.13 Custom elements

| Topic | Status | Notes |
|-------|:--:|------|
| `customElements.define` / `get` | ✅ | `src/js.c` |
| Autonomous + customized built-in | ✅ | name validation (hyphen required) |
| Lifecycle (`connected`/`disconnected`/`adopted`/`attributeChanged`) callbacks | ✅ | |
| `observedAttributes` | ✅ | |
| Shadow DOM (`attachShadow`, slots) | 🟡 | see §4.12 |

## §4.14–4.16 Idioms · disabled elements · selector matching

| Topic | Status | Notes |
|-------|:--:|------|
| Common idioms (`rel` keywords, etc.) | 🟡 | as above |
| `disabled` / inert disabling | 🟡 | HTML disabled-capable controls, disabled `fieldset`, and disabled `optgroup` inheritance feed selectors/form submission; `inert` blocks basic focus/click activation |
| Matching elements via Selectors/CSS | ✅ | rich selector engine (`src/css.c`) with broad structural, state, and functional selector coverage |

## §5 Microdata

✅ **Implemented.** The full microdata DOM API lives in `src/js.c`:

- `element.itemScope` / `itemId` — reflected (get + set).
- `element.itemType` / `itemProp` / `itemRef` — live `DOMTokenList`s.
- `element.itemValue` — element-type-aware getter **and** setter (`meta`
  content, `src`/`href`/`data` resolved to absolute URLs, `data` value,
  `time` datetime, nested item → the element itself, else text); setting
  on an item throws `InvalidAccessError`.
- `element.properties` — an `HTMLPropertiesCollection` crawled per the
  spec's properties algorithm (honours `itemref`, stops at nested items,
  results in tree order), with `.names`, `.namedItem(name)`, named access
  (`coll.foo`), and `PropertyNodeList.getValues()`.
- `document.getItems(typeNames)` — top-level items filtered by type.

No JSON/RDF serialisation beyond this API (not part of the spec's DOM
surface).

## §6 User interaction

| Topic | Status | Notes |
|-------|:--:|------|
| `hidden` attribute | ✅ | mapped to `display:none` |
| `inert` attribute | ✅ | excludes the subtree from focus (`focus()`, sequential navigation) and click activation, and an open modal dialog makes the rest of the document inert (`nd_dom_set_active_modal` → `nd_element_effectively_inert`) |
| Event dispatch / cancellation | 🟡 | DOM listeners, bubbling, `preventDefault()`, and inline `return false`; cancellation now honours `event.cancelable` |
| `contenteditable` | ✅ | in-page plaintext editing: a `contenteditable` host (`true`, the empty string, or `plaintext-only`) is focusable by click or Tab and edits as a single plaintext run — on focus its content is flattened to text, then the shared text-entry machinery (`nd_node_editable_value`/`nd_node_set_editable_value` in `src/dom.c`) drives a rendered caret, text selection, keyboard caret navigation (arrows, Home/End), insertion, Backspace/Delete, Enter→newline, and copy/cut/paste. Newlines in the host render as forced line breaks (`collect_walk` in `src/layout.c`); `beforeinput`/`input` fire and `document.activeElement`/`:focus` stay in sync. Rich inline structure is not preserved across an edit (the plaintext model); there is no per-range rich-text formatting |
| `tabindex` / focus order | ✅ | sequential focus navigation (`nd_js_sequential_focus_target` in `src/js.c`) honours `tabindex` ordering — positive values first in ascending order, then `0`/auto in tree order, negative excluded — skipping disabled/inert/hidden controls; Tab / Shift+Tab walk it (`src/main.c`), and `focus()`/`blur()` route through the canonical `nd_js_set_focus`, keeping `document.activeElement` and `:focus` in sync |
| `accesskey` | 🟡 | parsed; no key binding |
| `spellcheck` | 🟡 | parsed; no spell-check UI |
| `autocapitalize` / `enterkeyhint` | 🟡 | reflected in IDL; advisory only |
| Drag and drop (`DataTransfer`, drag events) | ❌ | no HTML DnD API (internal viewport panning only) |

## §7 Loading web pages

| Topic | Status | Notes |
|-------|:--:|------|
| Browsing context / `Window` | ✅ | single top-level context per tab; no per-tab process isolation |
| `WindowProxy` / named access | 🟡 | basic |
| Origins & same-origin policy | ✅ | enforced for fetch/XHR/storage/cookies |
| `iframe` sandboxing | 🟡 | attribute parsed; not enforced |
| Cross-origin (`postMessage`) | 🟡 | `postMessage` largely stubbed for cross-context |
| Session history & navigation | ✅ | back/forward |
| History API (`pushState`/`replaceState`/`popstate`) | ✅ | same-origin checked, `popstate` dispatched |
| `location` (all members) | ✅ | `href`/`protocol`/`host`/`hostname`/`port`/`pathname`/`search`/`hash` |
| `Navigation` API (`navigation.navigate`) | ❌ | not implemented |
| `BroadcastChannel` | 🟡 | stub |
| Application cache / offline | 🚫 | obsolete; not implemented |

## §8 Web application APIs

| API | Status | Notes |
|-----|:--:|------|
| `Window` / `Navigator` (`userAgent`, `platform`) | ✅ | |
| `setTimeout` / `setInterval` / `clearX` | ✅ | |
| `queueMicrotask` | ✅ | |
| `requestAnimationFrame` / `cancelAnimationFrame` | ✅ | tied to repaint |
| `requestIdleCallback` | ✅ | |
| `atob` / `btoa` | ✅ | |
| `structuredClone` (§2.7) | ✅ | true serialize/deserialize in `src/js.c`: cycles & shared references, `Map`/`Set`/`Date`/`RegExp`, `ArrayBuffer`/typed arrays/`DataView`, `Blob`/`File`, `Error` subtypes (name/message/stack), `undefined`; `DataCloneError` for functions/symbols (no transfer list — there are no worker threads to transfer to) |
| `DOMParser` / `XMLSerializer` | ✅ | The returned `Document` carries the live spec accessors — `documentElement`, `body`, `head`, `title`, `nodeType` (`= 9`) — populated by `nd_attach_document_view` (`src/js.c`); `text/html` parses through the full HTML document parser (auto-wraps `<html><head><body>`); MIME types with `xml` or `svg` parse through the fragment parser so the supplied root (e.g. `<svg>`) becomes `documentElement` rather than being wrapped |
| `fetch` / `Response` body | ✅ | binary-safe: response bytes are attached as an `ArrayBuffer` on `_bodyBuffer` and the body consumers (`text` / `json` / `blob` / `arrayBuffer` / `bytes` / `formData`) read from it through `TextDecoder` / `Uint8Array`, so non-UTF-8 bytes survive round-tripping (PNG, MP4, etc.) instead of being mangled by JS-string conversion |
| `XMLHttpRequest.responseType` | ✅ | `""`/`"text"`, `"json"`, `"arraybuffer"` (`ArrayBuffer`), `"blob"` (`Blob`, `type` from `Content-Type`), `"document"` (delegates to `DOMParser`, XML mode picked when the response is `*xml*` or SVG); `responseText` stays populated regardless for compatibility |
| `AbortSignal` + `fetch(url, {signal})` | ✅ | `AbortSignal.abort(reason)` and `AbortController().abort(reason)` cancel an in-flight `fetch` — the in-progress curl transfer is interrupted via `GCancellable` (xferinfo callback), the promise is rejected with the signal's `reason` (or a synthetic `AbortError`), and the abort listener is cleaned up when the state is freed. A pre-aborted signal rejects synchronously without touching the network. `AbortSignal.timeout(ms)` schedules a real `g_timeout_add` that flips the signal and fires `abort` with a `TimeoutError`. `AbortSignal.any([s1, s2, …])` returns a composite signal that aborts as soon as any input does, copying the firing signal's `reason` (and is immediately aborted if any input is already aborted) |
| `navigator.sendBeacon(url, body?)` | ✅ | fire-and-forget HTTP POST through the regular fetch pipeline (`nd_navigator_sendBeacon` in `src/js.c`); body type is inferred and the Content-Type chosen accordingly — string → `text/plain;charset=UTF-8`, `Blob`/`File` → `blob.type`, `URLSearchParams` → urlencoded, `FormData` → multipart with CSPRNG boundary, `ArrayBuffer`/typed array → raw bytes. Same-origin policy and the rest of the fetch security pipeline apply. Returns `false` on non-string URL or non-`http(s)` scheme; otherwise `true` |
| `navigator.clipboard.writeText(text)` | 🟡 | in a windowed context, writes through the GTK clipboard (`gdk_clipboard_set_text`) and resolves; in headless / no-clipboard contexts rejects with `NotAllowedError`. Other clipboard methods (`readText`/`read`/`write`) still reject — read access is intentionally not exposed |
| `navigator.*` device APIs (geolocation, sensors, …) | 🚫 | by design |

## §9 Communication

| API | Status | Notes |
|-----|:--:|------|
| `MessageEvent` | ✅ | |
| `WebSocket` (`ws://`/`wss://`) | ✅ | libcurl WebSocket; subprotocol negotiation |
| Cross-document `postMessage` | 🟡 | stub for cross-context |
| `MessageChannel` / `MessagePort` | 🟡 | constructs two ports; same-context delivery |
| Server-sent events (`EventSource`) | 🚫 | throws unsupported |

## §10 Web workers · §11 Worklets

🚫 **Absent by design.** `Worker` and `SharedWorker` throw an
unsupported error; `navigator.serviceWorker` is not exposed; no
worklets. This keeps the engine single-threaded and the W^X / no-JIT
guarantee simple.

## §12 Web storage

| API | Status | Notes |
|-----|:--:|------|
| `localStorage` | ✅ | persistent, origin-partitioned, **no quota enforced** |
| `sessionStorage` | ✅ | per-session |
| `storage` event | 🟡 | limited (single context) |
| IndexedDB | 🟡 | SQLite-backed, origin-partitioned; supports databases, version upgrades, object stores, indexes, key ranges, cursors, requests/events, and structured-clone persistence through QuickJS serialization. Transaction rollback/blocking semantics and cross-context versionchange coordination are simplified |

## §13 The HTML syntax

The heart of spec conformance. Tokenisation and tree construction —
the bulk of §13.2 — are delegated to in-tree **lexbor**, a
from-scratch WHATWG-conformant C implementation
(`src/html_lexbor.c`). Because the parser is a conformant engine, the
DOM tree Nordstjernen builds for a given byte stream is spec-faithful
**even for malformed input**; the compatibility gaps elsewhere in this
document are in *rendering and behaviour*, not parsing.

| Topic | Status |
|-------|:--:|
| Tokenizer state machine (§13.2.5) | ✅ |
| Tree construction / insertion modes (§13.2.6) | ✅ |
| Foreign content (SVG/MathML namespaces) | 🟡 (SVG to DOM; MathML to DOM, not laid out) |
| Adoption agency / error recovery | ✅ |
| Named & numeric character references | ✅ |
| Fragment parsing (`innerHTML`) | ✅ |
| Serialisation (`outerHTML`) | ✅ |
| `document.write` | 🟡 (limited) |

## §14 The XML syntax

🟡 **Partial.** There is no XHTML *document* navigation path; however
`DOMParser.parseFromString(s, "application/xml" | "image/svg+xml" |
"text/xml")` routes through the fragment parser (lexbor in HTML
fragment mode) so the supplied root element becomes the result's
`documentElement` rather than being wrapped in `<html><body>`. The
parser is still HTML-shaped — namespaces aren't tracked per-element
and XML well-formedness is not enforced — but the practical SVG /
RSS / feed use case (parse, query, read attributes) works. Served
`application/xhtml+xml` pages are handled via the HTML path rather
than a strict XML processor.

## §15 Rendering

Layout lives in `src/layout.c` (block/inline flow, flexbox, grid,
positioning, floats, stacking); painting is Cairo (`src/paint.c`,
`src/render.c`) with Pango text. The UA stylesheet (`kUa` in
`src/css.c`) supplies the default rendering the spec mandates for each
element.

CSS support (abridged):

- ✅ Box model, `display` (block/inline/flex/grid/table/table-caption/
  list-item/flow-root), sizing incl. `aspect-ratio` and the intrinsic
  `width` keywords `min-content` / `max-content` / `fit-content` on
  block boxes (shrink-to-fit via the same content-measurement used by
  tables; `getComputedStyle` reports the resolved pixel width),
  margins/padding, borders,
  `border-radius`, outline, `box-shadow`, `text-shadow`.
- ✅ Flexbox and Grid (incl. `fr`, `minmax()`, `auto-fit/fill`, `gap`).
- ✅ Positioning (`static/relative/absolute/fixed/sticky`), `float`/
  `clear`, `z-index` stacking contexts.
- ✅ CSS Values & Units viewport/container/logical unit subset including
  `em`/`rem`, `sv*`/`lv*`/`dv*`, `vi`/`vb`, `cqi`/`cqb`, `cap`, `ic`,
  and `Q`.
- ✅ Typography, CSS Color 4 `rgb()`/`hsl()`/`hwb()` space/slash syntax
  plus `lab()`/`lch()`/`oklab()`/`oklch()` conversion to sRGB, and
  CSS Color 5 `color-mix(in srgb, ...)`,
  backgrounds (incl. linear/radial/conic gradients and external
  stylesheet-relative `url(...)` resources),
  transforms (2D/3D), transitions/animations (`opacity`/`transform`/
  `color`/`background-color`), `object-fit`, `mask-image`,
  `accent-color`, `caret-color`, `tab-size`, `pointer-events`, custom properties + `calc()` and
  the Values 4 length math subset (`round()`/`mod()`/`rem()`/`abs()`).
- ✅ `text-indent` — indents the first formatted line of a block's
  inline content (length or percentage of the content width), including
  large negative sprite-hiding indents used with clipped inline-block
  logos (`nd_text_indent_px` in `src/layout.c`).
- ✅ `white-space`: `normal`/`nowrap` collapse, `pre`/`pre-wrap`/
  `break-spaces` preserve, and `pre-line` collapses runs of spaces/tabs
  while preserving newlines as forced breaks (`white_space_mode` in
  `src/layout.c`); `text-wrap`/`text-wrap-mode` map `nowrap` to the same
  no-wrap path and wrapping keywords to normal wrapping.
- ✅ Selectors: type/class/id, combinators, attribute, structural &
  state pseudo-classes including `:any-link`, `:lang()`, `:dir()`,
  `:open`, `:popover-open`, `:nth-last-*`, `:only-of-type`, and
  `:nth-child(... of selector)`,
  `:is()/:where()/:has()` (bounded),
  `::before/::after`, `::first-letter`, `::first-line`, `::placeholder`.
- ✅ At-rules: `@media`, `@font-face`, `@keyframes` (subset),
  `@supports` (now evaluated, incl. `selector()`), `@scope`
  (selector-form roots/limits, relative scoped selectors, `:scope`,
  and scope proximity), `@property`
  (registers custom-property `initial-value` and the `inherits`
  descriptor — registered properties resolve to their initial value
  when unset, and `inherits: false` properties do not inherit from the
  parent; `syntax` is parsed but not yet type-validated).
- ✅ Logical sizing/spacing aliases and shorthands including
  `margin-inline/block`, `padding-inline/block`, `inset-inline/block`,
  `border-inline/block`, logical border width/style/color pairs, and
  logical corner-radius aliases.
- ✅ Media Queries: width/height/orientation, range syntax, aspect-ratio,
  resolution, color-gamut, scripting, display-mode, update,
  dynamic-range, overflow-block/inline, grid/monochrome/color, and user
  preference features including reduced data and inverted colors.
- ✅ `filter` (grayscale/sepia/invert/brightness/contrast/saturate),
  `clip-path` (basic shapes), `mix-blend-mode`, `-webkit-line-clamp`.
- ✅ `:focus`, `:focus-within`, `:focus-visible`, `:placeholder-shown`,
  `:checked`, `:valid`, `:invalid`.
- ✅ `::selection` — author `background-color`/`color` honoured for the
  on-page text selection (`src/selection.c`, pseudo style resolved in
  `src/css.c`).
- ✅ `::first-line` — the first formatted line of a block adopts the
  pseudo's `color`, `background-color`, `font-size`/`weight`/`style`/
  `family`, `font-variant: small-caps`, and `text-decoration:
  underline`. The first line's extent is taken from the wrapped Pango
  layout at paint time (`apply_first_line_attrs` in `src/paint.c`), so
  it tracks the actual wrap width; cascades correctly with other
  `::first-line` rules and composes with `::first-letter`.
- ✅ `::marker` — author `color`/`font-size` honoured for list-item
  markers (`paint_marker` in `src/paint.c`); `display:none` hides the
  marker, and string / `counter(list-item)` `content` overrides the
  generated bullet/number.
- 🟡 `::backdrop` — author `background`/`background-color` painted as a
  full-viewport fill behind a modal dialog's top-layer box
  (`paint_top_layer` in `src/paint.c`); only the modal-`dialog` top
  layer is covered (popovers not yet), and only the backdrop's
  background paints.
- 🟡 `subgrid` — a grid item that is itself a grid container with
  `grid-template-columns: subgrid` adopts its parent grid's spanned
  column tracks (sizes, positions, and gap), so its own children align
  to the outer column grid (`g_pending_subgrid_cols` /
  `layout_grid` in `src/layout.c`); honours partial spans
  (`grid-column: 1/3`) and falls back to a single auto column when
  there is no parent grid. Row-axis `grid-template-rows: subgrid`
  still falls back to auto rows.
- ❌ Writing modes.

Replaced elements (`img`/`video`/`canvas`) are sized via the media-box
path in `src/layout.h`.

## §16 Obsolete features

| Element | Status | Notes |
|---------|:--:|------|
| `frame` / `frameset` | 🚫 | `display:none`; not rendered |
| `applet` | 🚫 | no Java; `display:none` |
| `marquee` | 🟡 | renders as static block; no scrolling |
| `basefont` `noembed` `plaintext` `xmp` `isindex` | 🚫 | `display:none` / inert |
| Presentational attributes (`align`, `bgcolor`, `<font>`) | 🟡 | a subset mapped to CSS where common (`presentational_hints_css` in `src/css.c`): `bgcolor`/`text`/`<font color/face/size>`, `width`/`height`, `hspace`/`vspace`, `<hr align/color/size>`, and the table family — `cellspacing`→`border-spacing`, `cellpadding`→cell `padding`, `<td/th align/valign/nowrap>`, `<table align=left/right>`→`float` and `align=center`→auto side margins (centred by `layout_table`), and **`<table border=N>`** maps the legacy grid: an outer table border plus a `1px solid` border on every `td`/`th` (the classic `border="1"` grid), while `border="0"` stays gridless. The legacy **list attributes** are honoured in `src/paint.c`: `<ol start>`/`<ol reversed>`/`<li value>` drive ordinal numbering (`list_item_ordinal` — start seeds the first number, reversed counts down, a `value` resets the running count), and `<ol type>`/`<ul type>`/`<li type>` map to `list-style-type` as a presentational hint (`presentational_hints_css` in `src/css.c`, applied at `ND_CSS_ORIGIN_PRESENTATIONAL` so it overrides the UA default but loses to author CSS): ol/li `1`/`a`/`A`/`i`/`I` → decimal / lower- and upper-alpha / lower- and upper-roman (case-sensitive), ul/li `disc`/`circle`/`square` (case-insensitive) |

---

## Design constraints (the spec we deliberately do not implement)

These are project non-goals (see `CLAUDE.md` / `README.md`), not
defects, and will not be added:

- **WebGL / WebGPU** rendering contexts and AI-style web APIs.
- **Web/Service Workers**, Worklets, server-sent events.
- **WebRTC**, **MSE/EME** (DRM media).
- Plugin content (`embed`/`object` via NPAPI/PPAPI).
- **In-process audio/video codecs** — playback is handed off to an
  external media player instead.
- **JIT** (interpreter-only, to preserve W^X process-wide).
- Telemetry, crash reporters, update pingers, "studies".
- Localisation beyond English (for now).

## Highest-leverage HTML/CSS gaps for real-world sites

Ordered by how often they block ordinary browsing:

1. Row-axis `subgrid` (`grid-template-rows: subgrid`) — the column
   axis is now adopted from the parent grid; the row axis still falls
   back to auto rows.
2. Remaining `table-layout` edge cases (advanced auto-layout sizing and shared-edge border collapsing; inter-cell `border-spacing`/`border-collapse` spacing is now honoured).
3. HTML drag-and-drop API — absent.

(Column-axis `subgrid`, `multipart/form-data` for `fetch`/`XHR`
bodies, the Microdata DOM API, table captions with `caption-side`,
`::selection`, `::marker`, `:focus-visible`, `:focus-within`,
sequential focus navigation (`tabindex`/`inert`), the modal-dialog
top layer / focus trap, and `::backdrop` have since been implemented.)

## Mobile-variant routing

A handful of mainstream sites ship document trees so heavy with
proprietary runtime scaffolding that they are not practical to render
through a clean-room engine. Nordstjernen handles them not with
site-specific HTML rewrites — those were removed — but by routing the
top-level navigation to the site's own **mobile** variant, which the
operators maintain for low-resource clients and which sticks much
closer to plain HTML.

`src/mobile.c` performs two things, both keyed on the request host:

- `nd_mobile_rewrite_url` swaps the host before fetch:
  `*.facebook.com` → `m.facebook.com`,
  `www.youtube.com` / `youtube.com` / `music.youtube.com` →
  `m.youtube.com`,
  `www.reddit.com` / `reddit.com` / `m.reddit.com` /
  `new.reddit.com` → `old.reddit.com` (the legacy server-side
  rendered variant Reddit still maintains; the post-2023 Shreddit
  front-end is unrenderable without a full custom-element runtime).
- `nd_mobile_force_host` makes the network layer (`src/net.c`) send a
  current iOS Safari `User-Agent` for the Facebook/YouTube hosts plus
  YouTube's CDN hosts (`*.googlevideo.com`, `*.ytimg.com`,
  `*.ggpht.com`), and exposes the same UA through
  `navigator.userAgent` / `platform` / `vendor` to scripts on those
  origins. `old.reddit.com` does not need UA spoofing — it serves
  plain HTML to anything — so Reddit hosts are not on this list.

No DOM is synthesised, no JSON blobs are mined, no per-site CSS is
injected, no site-specific custom elements are fetched. If a routed
variant stops working, the fix is in the generic engine — not in a
workaround file.

## How to re-check this document

There is **no automated conformance suite** (by project policy).
Validate any row by running the browser against a page that exercises
the feature and observing the result. Treat this file as a living map
and update it whenever behaviour changes.

Interaction can be scripted in headless mode with `--act`, a
`;`-separated list of `click X,Y`, `type TEXT`, `key NAME`, and `wait N`
(`Enter`/`Backspace`/`Delete`/`Left`/`Right`/`Up`/`Down`/`Home`/`End`)
steps run after the page settles, before the dump. `wait` settles the main
loop for `N` milliseconds. The clicks drive the
real hit-test, focus, checkbox/radio toggling, `<label>`→control
activation, `<summary>` disclosure of `<details>`, link navigation, and
JS `click`/`input`/`change` dispatch; `type` honours `maxlength` and
filters `type=number` input, and `Up`/`Down` step a focused number
input — so e.g.

```sh
nordstjernen --headless --url=FILE --viewport=800 \
  --dump=png:out.png --act='click 120,180; type hello; key Enter; wait 500'
```

renders the result of focusing a control, typing, and pressing Enter —
useful for exercising forms and `contenteditable` without a window.

Layout can be inspected — like a browser's "Inspect" panel — with
`--inspect=SELECTOR` and `--inspect-at=X,Y`:

```sh
nordstjernen --headless --url=URL --inspect='.cdx-search-input'
nordstjernen --headless --url=URL --inspect-at=400,18
```

`--inspect` matches a CSS selector and, for each match, prints the box
model (content / border-box / margin / border / padding in px), the key
computed styles (`display`, `position`, `box-sizing`, sizing, the flex
properties, `font-size`), the laid-out children with their geometry and
`flex-grow`, and the full DOM path; elements that match but were not laid
out (e.g. `display:none`) are reported as such. `--inspect-at` hit-tests
the point and prints the element stack there (innermost first) followed
by the hit element's details. Used alone, either flag suppresses the
default text dump; combine with `--dump=` to get both.
