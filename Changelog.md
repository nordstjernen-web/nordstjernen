Changelog:
=========
Significant changes in each release:

1.0.21:
======

Media and images
* The vendored pl_mpeg no longer reads past a frame plane. Half-pel motion
  compensation samples `s[si + 1]`, `s[si + dw]` and `s[si + dw + 1]`, but
  `plm_video_process_macroblock` bounds only `s[si]`, so a macroblock on
  the bottom row reads up to one row plus one byte beyond the plane it
  samples -- absorbed by the next plane for interior planes, and off the
  end of the allocation for the last one. This is reachable from page
  content: `ns_video_player_new` hands an MPEG-1 `<video>` body to
  `plm_create_with_memory`, and `ns_video_backend_next` decodes it. Fuzzing
  the decoder under AddressSanitizer with mutated streams reported it as a
  heap-buffer overflow read. The three frames are allocated as one chunk,
  which is now padded by that overshoot and zeroed, so the read stays
  inside the allocation and a corrupt stream decodes deterministically.
  Valid video is unaffected: no bound is tightened, so no macroblock that
  decoded before is rejected now.
* Animated images decode as animations on the engine's own fetch path.
  `ns_image_decode_body` routed GIF, APNG and animated WebP to the
  animation decoder, but the two fetch handlers in `engine.c` -- the ones
  headless rendering and the browser's own image pass use -- called
  `ns_image_decode_bytes` instead, which only ever returns a still frame.
  An animated image fetched through those paths therefore froze on frame
  one. Both now go through `ns_image_cache_insert_encoded`, and the
  still-versus-animated decision lives in one function rather than three
  copies that had already drifted.

Media capture and WebRTC
* `MediaStream` and `MediaStreamTrack` are constructors, and the streams
  and tracks `getUserMedia` hands back are instances of them, so
  `stream instanceof MediaStream` holds and `new MediaStream([track])`
  works. They were plain object literals with the right methods, which
  passes a duck-typing check and fails everything else.
* `RTCSessionDescription` and `RTCIceCandidate` exist. Signalling code
  wraps the objects it receives before handing them to the peer
  connection, so their absence stopped a session at the first offer.
  `RTCIceCandidate` rejects an initialiser carrying neither `sdpMid` nor
  `sdpMLineIndex`, as the specification requires.
* `RTCRtpSender`, `RTCRtpReceiver` and `RTCRtpTransceiver` exist, with
  `getCapabilities` on the two that define it.
* `RTCPeerConnection` gains `addTrack`, `removeTrack`, `addTransceiver`,
  `getConfiguration`, `setConfiguration`, `restartIce` and the
  `generateCertificate` static, and `getSenders`, `getReceivers` and
  `getTransceivers` return what was added to the connection instead of
  always returning an empty array.
  This is API surface: there is still no ICE agent, no DTLS and no SRTP,
  so a connection never leaves the `new` state. What changes is that
  feature detection and object construction no longer throw partway
  through a page's setup code.

Adaptive streaming
* HLS and DASH play. `data/js/streaming.js` adopts any `<video>` or
  `<audio>` whose source is an `.m3u8` or `.mpd` -- by extension or by
  `type` -- parses the manifest, picks a rendition, and feeds segments
  through Media Source Extensions, which is how the sites that use these
  formats already expect to be served. HLS covers master and media
  playlists, `EXT-X-MAP` initialisation segments, `EXT-X-BYTERANGE`,
  separate `EXT-X-MEDIA` audio renditions, and live playlists, which are
  re-fetched on the target-duration cadence and merged by media sequence.
  DASH covers `SegmentTemplate` with either `SegmentTimeline` or a fixed
  segment duration, `$Number$`/`$Time$`/`$RepresentationID$`/`$Bandwidth$`
  substitution with `%0Nd` padding, `BaseURL`, and separate audio and
  video adaptation sets. Rendition choice prefers the highest bandwidth at
  1080p or below among the codecs the build can actually decode. The
  player keeps roughly thirty seconds buffered ahead of the playhead and,
  on `QuotaExceededError`, evicts everything more than ten seconds behind
  it and retries rather than giving up.
* `video/mp2t` is an accepted Media Source type. Browsers reject it
  because their Media Source pipelines take fragmented MP4 and WebM only,
  which is why HLS players written in JavaScript transmux MPEG-TS before
  appending. Appended bytes here go to libavformat, which demuxes
  transport streams natively, so the transmuxing step is wasted work and
  segments can be handed over as they arrive.

Media Source Extensions
* `navigator.mediaCapabilities.decodingInfo()` answered from a hardcoded
  substring list -- WebM, VP8, VP9, Opus and WAV -- so it reported
  `supported: false` for every MP4 and AAC configuration even though
  `canPlayType` reported `probably` for the same string. Adaptive players
  ask `decodingInfo` which rendition to fetch, so a browser that denies
  H.264 there selects nothing and never starts. Container and codec
  support now resolve through one shared table that `canPlayType`,
  `decodingInfo` and `MediaSource.isTypeSupported` all consult, and a
  configuration is supported only when every stream in it is -- audio and
  video both, not whichever one was inspected first.
* `MediaSource.isTypeSupported` is a native call rather than a
  round-trip through `document.createElement('video').canPlayType`, and
  answers are memoised. Adaptive players probe it hundreds of times while
  building the format ladder; each probe used to allocate an element.
  It also answers for the segmented containers only, as the specification
  requires, instead of inheriting `canPlayType`'s whole-file container
  list.
* A single failed `appendBuffer` no longer wedges a `SourceBuffer` for
  the rest of the page's life. Any native rejection set a `_quotaFull`
  latch that made every later append throw `QuotaExceededError`, and the
  latch cleared only on a successful `remove()`. Quota is now checked
  against the buffer's real byte count before the append is queued, so it
  throws `QuotaExceededError` synchronously the way the specification
  says and the way players expect when they run their eviction path; a
  genuine decode failure runs the append-error steps instead, ending the
  media source. A zero-length append is a no-op rather than an error.
* `SourceBuffer` reports `audioTracks`, `videoTracks` and `textTracks`,
  and `AudioTrackList`, `VideoTrackList` and `TextTrackList` exist as
  constructors.
* `MediaSource.readyState`, `sourceBuffers` and `activeSourceBuffers`,
  and `SourceBuffer.updating`, moved from per-instance properties to
  prototype accessors, where feature detection looks for them.
* `MediaSourceHandle`, `MediaSource.prototype.handle`,
  `MediaSource.canConstructInDedicatedWorker`, `ManagedMediaSource` and
  `ManagedSourceBuffer` are present.
* AC-3, E-AC-3, FLAC, ALAC, MP3-in-MP4 (`mp4a.69`, `mp4a.6b`) and the
  AAC object types beyond `mp4a.40` resolve to their decoders, and
  `video/quicktime`, `audio/aac`, `audio/flac` and `audio/wav` are
  recognised containers.
* The headless renderer builds a video cache, wires the Media Source
  callbacks, ticks the cache from the settle loop and forwards media
  events back to the document, so appended segments reach the demuxer
  there instead of every `appendBuffer` failing for want of a callback,
  and `video.buffered` reports the real demuxed range. Media Source
  behaviour is now reproducible from `--headless`.

Images and graphics
* Animated PNG plays. Wuffs already decoded APNG frames and the
  animation loop is format-agnostic, but the callers only routed GIF
  magic to it and the animation decoder itself hardcoded the GIF
  signature check and the GIF decoder, so a PNG was rejected inside the
  function meant to decode it. Both now use the same format detection
  the still path uses. An APNG is recognised as the spec defines it, by
  an `acTL` chunk before the first `IDAT`, so a still PNG never pays for
  the animation decoder. The decode-pipeline documentation had listed
  APNG as supported already; it is now accurate.
* The vendored Wuffs moves to v0.4.0-alpha.10, nine months newer than
  the alpha.9 the tree carried. The release adds the VP8 decoder, so
  lossy WebP -- the common case on the web -- now decodes through the
  memory-safe path instead of libwebp. `MODULE__VP8` was already set in
  the subproject's build flags, where it had been a no-op because the
  module did not exist in alpha.9. libwebp and libwebpdemux are left
  serving animated WebP and nothing else.
* gdk-pixbuf no longer decodes page images. Every format the web
  actually uses is already handled in-tree -- ICO, then Wuffs for PNG,
  GIF, BMP and JPEG, then libwebp, then libavif, then the in-engine SVG
  renderer -- so the pixbuf fallback had been reduced to TIFF, TGA, PPM
  and ICNS, none of which Chrome or Firefox render either. What it cost
  was the ability to know what parses untrusted bytes:
  `gdk_pixbuf_get_formats` enumerates loader plugins installed on the
  user's machine, so the set of decoders reachable from a web page was
  decided at runtime, varied per system, and could not be audited from
  the build. The decode chain now ends after SVG: an unsupported format
  fails to decode instead of falling through to a plugin. GTK 4 still
  depends on gdk-pixbuf for its icon theme, so a desktop build links it
  either way -- what goes away is the browser feeding it. The mobile
  builds, which never had it, are unaffected.
  `ns_image_pixbuf_supports_mime` is renamed `ns_image_supports_mime`.
* libavif is optional on the desktop builds too. It was a hard
  `dependency()` off the mobile path, so a desktop tree without it would
  not configure at all, even though every AVIF call site already sat
  behind `NS_HAVE_AVIF` and `image_avif.c` was already compiled
  conditionally. The new `avif` meson feature defaults to `auto`, so a
  host that has libavif is unchanged; `-Davif=disabled` drops it and
  AVIF images fail to decode like any other unsupported format. libavif
  pulls in a complete AV1 decoder for a format that is rare on the web.
  The README listed libavif as both required and optional; it is now
  listed once, as optional.
* `var()` resolves inside SVG presentation attributes. A custom property
  set by a stylesheet rule now reaches `r="var(--radii)"` or
  `fill="var(--tint)"`, so a class can retheme an inline icon's colour
  and geometry the way it does for ordinary CSS properties.
* `mask` is honoured on SVG elements. The referenced `<mask>` renders to
  an offscreen surface whose sRGB luminance becomes the alpha the
  element is composited through, so a white mask shows the element,
  black hides it, and a gradient fades it. Group opacity and masking
  combine.
* `marker-start`, `marker-mid` and `marker-end` draw their `<marker>` on
  path, line, polyline and polygon vertices. Vertices and their tangents
  come from the built Cairo path, so arcs and curves orient the same way
  straight segments do, and a mid vertex uses the bisector of its two
  tangents. `markerUnits="strokeWidth"` scales the marker with the
  stroke, `orient="auto"` and `auto-start-reverse` rotate it, and
  `refX`/`refY` are mapped through the marker's own `viewBox` before
  positioning.
* `vector-effect: non-scaling-stroke` keeps a stroke's width in device
  space instead of scaling it with the current transform.
* SVG is rendered by the engine instead of librsvg. `src/svg.c` walks
  the SVG DOM and paints it through the same Cairo surface, cascade and
  font stack that HTML uses, and `librsvg` is gone from the dependency
  list, the packaging manifests and the CI images. Inline `<svg>` was
  previously re-serialised to XML and handed to librsvg as an opaque
  raster, so the document's own stylesheet could never reach inside it:
  `fill: currentColor`, `svg .icon { fill: … }` and script-driven
  geometry changes were invisible. SVG elements now take part in the
  normal cascade, so `fill`, `stroke`, `stroke-width`,
  `stroke-dasharray`, `fill-rule`, `stop-color`, `text-anchor`,
  `paint-order` and the SVG geometry properties `x`, `y`, `cx`, `cy`,
  `r`, `rx`, `ry` are real CSS properties that inherit like the rest.
  Covered: paths including elliptical arcs and smooth-curve
  continuation, rect/circle/ellipse/line/polyline/polygon, `viewBox`
  and `preserveAspectRatio`, nested `<svg>`, `<g>`, `<use>`,
  `<symbol>`, `<switch>`, `<defs>`, linear and radial gradients with
  `href` inheritance, `spreadMethod`, `gradientUnits` and
  `gradientTransform`, `clipPath`, group opacity, dashing, and `<text>`
  shaped through Pango. A standalone `.svg` document sizes to the
  viewport rather than to a 300x150 default. Android and iOS, which
  dropped librsvg with the rest of the desktop stack, gain SVG for the
  first time.

CSS
* `text-decoration-color` reaches the painted line. The colour was only
  read from the *block's* style, never from the inline run that carries
  the decoration, so an underline set on an `<a>` was always drawn in
  the text colour -- and a fully transparent one, the idiom behind every
  "underline grows in on hover" teaser, was drawn as a solid line. On
  Tidens Krav and the other Amedia fronts that put an underline under
  every headline and nav link. The decoration attributes now carry the
  style of the element that turned them on, so
  `text-decoration-color: #e00` paints red, and a decoration whose
  resolved colour is fully transparent is not emitted at all. A
  decoration propagated from an ancestor still paints in the ancestor's
  colour, as the spec requires.
* A flex item that is itself a flex or grid container re-aligns its own
  children after the cross-axis stretch resizes it. The item laid its
  children out at its content height, and the stretch then overwrote
  that height in place without a second pass, so anything the item
  centred or bottom-aligned stayed where the pre-stretch height had put
  it. VG's masthead is the shape that shows it: a 56px-tall `header`
  flex row, a logo link inside it that is a flex container with
  `align-items: center`, and a 24px logo that rendered flush against the
  top of the bar instead of centred on it. The column-flex path already
  re-ran layout for a resized item; the row and wrapped-row paths now do
  the same, and only when the stretch actually changed the height.
* A `container-type: inline-size` (or `size`) element no longer sizes
  itself from its own contents. CSS Contain 3 gives such an element
  inline-size containment, so its intrinsic inline sizes are computed as
  if it had no children; the engine measured the children anyway. On a
  page whose container queries feed back into the container -- headlines
  sized in `cqw`, the pattern VG, Aftenposten and the other Schibsted
  fronts use -- that closed a loop: wide contents made the container
  measure wide, `cqw` then resolved against the inflated width and made
  the contents wider still. VG's lead teaser laid out 1245px wide inside
  a 734px column and its headline computed to 276px where the site asks
  for 157px. The three intrinsic-width paths (`measure_natural_width`,
  `measure_min_width`, `estimate_natural_width`) now return zero content
  contribution for such a box, so an explicit `width` still wins and a
  flex or grid item shrinks to the space its parent gives it.
* A square border is painted inside its border box rather than centred
  on the edge. Each side was stroked along the border-box boundary with
  the line width set to the border width, and Cairo centres a stroke on
  its path, so every bordered element rendered half a border wider than
  it laid out on each side -- a 4px border occupied 6..9 and 60..63
  where the box model puts it at 8..11 and 58..61. Layout was always
  right; only the paint was wrong, so borders overlapped whatever sat
  next to them. Rounded borders and border-image already inset
  correctly and are unchanged.
* An `<iframe>` becomes visible as soon as its document loads, on a
  quiet page as well as a busy one. The UA sheet hides frames until the
  engine stamps `data-nd-frame-loaded` on them, but that stamp is
  written by the loader rather than through the scripted attribute
  path, so it never invalidated style. The frame kept the cached
  `display: none` and produced no box at all — its document parsed and
  its scripts ran, entirely unpainted — until some unrelated mutation
  happened to force a restyle. Pages with continuous script activity
  masked it; a page whose only content was a frame never showed it. The
  three places that add or remove the attribute now mark it dirty.
* Inline atomic boxes contribute their full height to the individual
  wrapped line that contains them. Multi-line form controls and table
  cells now reserve the correct vertical space instead of allowing later
  lines to overlap following content, fixing the Google footer position.
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
* Media queries inside a frame evaluate against the frame's own size,
  not a 300x150 guess. The viewport pushed while collecting a frame's
  stylesheets came from the frame's inline `style` attribute or its
  `width`/`height` content attributes, so a frame sized by a stylesheet
  rule -- `iframe { width: 100% }`, the common responsive-embed pattern --
  was measured as 300x150 and its `@media (min-width: ...)` blocks
  resolved against a size the frame never had. Layout now records each
  frame's content box, collection prefers it over the default, and when
  the recorded size disagrees with the one a viewport-dependent frame
  sheet was collected under, style and layout run once more so the frame
  settles on its real size. Frames whose CSS carries no width, height,
  aspect-ratio or orientation query never trigger the extra pass. Acid3
  goes from 98/100 to 99/100.
* A frame document's own stylesheet can style its root element. Sheets
  inside an iframe are rewritten to be scoped to the frame's root, and
  every selector whose subject was not literally `html` or `:root` got a
  descendant combinator — so `* { … }` or `.cls { … }` in a framed
  document matched everything inside the frame except the frame's own
  `<html>`, and `getComputedStyle` on that element reported no value for
  any property. The scope marker now also attaches directly to the
  subject compound, and lands before a pseudo-element rather than after
  it. Shadow scopes are unchanged: a shadow host is still not styled by
  its own shadow tree. Acid3 goes from 97/100 to 98/100.
* `getComputedStyle(el).someUnknownName` is `undefined` rather than the
  empty string. The proxy in front of a computed declaration answered
  every string key through `getPropertyValue`; its `has` trap already
  distinguished supported properties from unknown ones, and `get` now
  draws the same line. jQuery's `css()` returns
  `computed.getPropertyValue(name) || computed[name]` and expects
  `undefined` for a property the engine does not know.

Scripting
* Removed the IE-only `attachEvent` and `detachEvent`. They were exposed
  on Element, Document and Window as no-op stubs that returned true and
  registered nothing. Libraries still feature-detect them to select a
  legacy path: RequireJS, finding a native-looking `attachEvent`, bound
  its script-load callback to `onreadystatechange` instead of
  `addEventListener`, the stub swallowed it, and every module load ended
  in "Load timeout for modules". jQuery's test suite could not get past
  its RequireJS bootstrap before this.
* `DOMParser` reports the line and column of an XML parse error. The
  synthesized `parsererror` document carried the bare text "XML parsing
  error"; it now names the position the parser stopped at.

Scripting
* `Intl.DateTimeFormat` names months and weekdays in the requested
  language, and picks the clock the locale actually uses. The month and
  weekday tables held English names only and the hour cycle defaulted to
  12-hour whatever the locale, so `new Date().toLocaleTimeString()` on a
  Norwegian desktop read "2:47:00 PM" instead of "14:47:00", and
  `toLocaleDateString('nb-NO', {weekday: 'long', month: 'long'})` read
  "Tuesday, July 28" instead of "tirsdag 28. juli". Every Nordic news
  front page shows a formatted date, so this was visible on all of them
  -- Aftonbladet's masthead read "TUESDAY, JULY 28, 2026". Month and
  weekday names are now carried for the fourteen languages whose date
  *patterns* the formatter already knew (the Nordic five plus German,
  Dutch, French, Spanish, Italian, Portuguese, Polish and Russian);
  `short` and `narrow` are derived from the long name by UTF-8-safe
  truncation rather than byte truncation, which previously cut a
  multi-byte name mid-character. The 12-hour default is now restricted
  to the locales that use one, `en` (outside GB/IE/ZA) and a dozen
  others; everything else formats h23. Swedish and Lithuanian numeric
  dates serialize in ISO order (`2026-07-28`), and the day-month-year
  languages get their own literals -- the ordinal period in Norwegian,
  Danish, German, Finnish and Icelandic, ` de ` in Spanish and
  Portuguese -- instead of the English comma layout. An explicit
  `hour12`/`hourCycle` option still wins, and `en-US` output is
  unchanged.

Networking
* A top-level navigation follows a redirect that leaves HTTPS. Any
  redirect off `https://` was refused outright, which is the right rule
  for a subresource -- that is mixed content -- but not for a document
  the user asked for. Sunnmørsposten is the common shape: `smp.no`
  answers 302 to `http://www.smp.no/`, whose server immediately sends
  302 back to `https://www.smp.no/`, so the whole site was unreachable
  and rendered as "That address looks malformed". Navigations now follow
  the hop and report the resulting scheme in the security indicator;
  subresource fetches are still blocked exactly as before.
* HTTP/3 can receive a response larger than a megabyte. nghttp3's
  `nghttp3_conn_read_stream()` returns the bytes it consumed *excluding* the
  DATA frame payload — the application is required to extend QUIC's stream
  and connection flow-control credit for the body itself, from the
  `recv_data` callback. The backend extended only for what nghttp3 reported,
  so credit for body bytes was never returned: every HTTP/3 transfer
  deadlocked the moment it reached the 1 MB
  `initial_max_stream_data_bidi_local` advertised at connection setup, and
  sat there until the request timeout expired 30 seconds later. A 10.7 MB
  script that HTTP/2 fetched in 140 ms failed outright over HTTP/3; it now
  completes.
* HTTP/3 resolves the origin over IPv6 as well as IPv4. The QUIC socket
  asked `getaddrinfo` for `AF_INET` only and used the first result, so on an
  IPv6-only network every HTTP/3 hop failed to connect and fell back to
  HTTP/2. It now asks for `AF_UNSPEC` and tries each address in turn, the
  way the TCP path does.
* A UDP socket that reports `EAGAIN` no longer fails the HTTP/3 request. The
  QUIC socket is non-blocking, so a full send buffer is an ordinary
  condition under load; it was treated as a fatal write error and abandoned
  the connection. The datagram is now dropped and left to QUIC loss
  recovery, which is what it is for.
* HTTP/3 loss-recovery and idle timers fire while packets are arriving.
  `ngtcp2_conn_handle_expiry()` was called only when the poll timed out, so
  a connection with steady inbound traffic never processed an expired PTO or
  ACK timer. It is now called every iteration, which is a no-op when nothing
  is due.
* The nghttp2 backend reads the final response of a request that begins
  with an informational one. A `103 Early Hints` — what Cloudflare, Fastly
  and Shopify send ahead of the real response — arrives as a first HEADERS
  block, and libnghttp2 categorises the *final* HEADERS that follows as
  `NGHTTP2_HCAT_HEADERS` rather than `NGHTTP2_HCAT_RESPONSE`, the same
  category it gives trailers. The header callback accepted only
  `HCAT_RESPONSE`, so the page kept the interim status and every real
  header was discarded: no `Content-Type` (an HTML document rendered as
  plain text, its source visible), no `Content-Encoding` (a gzip body
  handed to the sink still compressed), no `Set-Cookie`. The callback now
  admits the block that follows an informational response and drops the
  interim headers instead of the final ones; trailers are still ignored.
  The HTTP/1.1 fallback had the same defect and worse — it treated the
  blank line ending the interim response as the end of all headers, so the
  entire second response, status line included, became the body. It now
  skips interim blocks and parses the response after them.
* A timed-out or cancelled HTTP/2 stream is detached from its session.
  `ns_h2_io_scan_timeouts` sent RST_STREAM and released the request, which
  lives on the requesting thread's stack, but left the session's
  `stream_user_data` pointing at it. DATA or HEADERS already in flight for
  that stream — the ordinary case, since RST_STREAM races a response — then
  drove the header and body callbacks through a dangling pointer and wrote
  into a returned stack frame. Streams are now detached with
  `nghttp2_session_set_stream_user_data()` before the request is released.
* HTTP/2 downloads are no longer capped by the default connection-level
  flow-control window. The session advertised an 8 MB
  `SETTINGS_INITIAL_WINDOW_SIZE`, but per RFC 9113 §6.9.2 that setting
  governs streams only: the connection window stayed at the protocol
  default of 65535 bytes, which throttles *aggregate* throughput on a
  connection to one window per round trip — about 640 KB/s at 100 ms RTT no
  matter how many streams are multiplexed over it. The connection window is
  now raised to match with `nghttp2_session_set_local_window_size()`.
* A request the server refused is retried on a fresh connection. When a
  pooled connection goes away, libnghttp2 closes the streams the server
  never processed with `NGHTTP2_REFUSED_STREAM` — the code exists precisely
  so the request can be sent again — but the retry only covered streams
  that had not been submitted, so a subresource lost this race and failed
  outright instead of being refetched.
* Idle HTTP/2 connections are closed. The pool defined an idle timeout, a
  reuse ceiling and a per-origin cap and enforced none of them: every
  origin visited kept a connection, its TLS state, three file descriptors
  and a live I/O thread polling four times a second until the browser
  exited. A connection with no streams for a minute now stops its I/O
  thread, and the pool drops connections that are dead, idle-expired, over
  the reuse ceiling or beyond the per-origin cap.
* The nghttp2 backend uses the `nghttp2_ssize` API on the versions that
  have it. Upstream deprecated the `ssize_t`-based entry points in favour
  of `…2` variants in 1.60.0 and lets an application compile the old ones
  out entirely with `NGHTTP2_NO_SSIZE_T`; the backend now defines that
  macro and calls `nghttp2_submit_request2()`,
  `nghttp2_session_mem_recv2()` and
  `nghttp2_session_callbacks_set_send_callback2()` when the headers are new
  enough, keeping the deprecated names only as the fallback for older
  libnghttp2. This is what a toolchain without `ssize_t` needs, and it
  makes a future upstream removal a non-event.
* An informational response no longer contributes headers to the HTTP/3
  response that follows it, matching the HTTP/2 and HTTP/1.1 paths.
* The request identity a fetch coalesces and preloads on no longer depends
  on the order its `Accept`-style headers happen to be listed in; the header
  lines are sorted into the key.
* `Vary: Origin` no longer defeats the HTTP cache. `Origin` is now one of
  the headers the cache can resolve at lookup time: `net.c` computes the
  value it will send once and uses that same string both as the request
  header and as the cache selector, so the two can never disagree, and
  the absence of an `Origin` selects distinctly from any present one.
  Google serves its stylesheets `public, immutable, max-age=31536000`
  with `Vary: Origin`; those were being refetched on every load and are
  now cached.
* ES module fetches join the same request identity as every other
  subresource. The module loader passed no top-level URL, so a module was
  partitioned in the HTTP cache under its own site rather than the
  document's — two unrelated sites importing the same module shared one
  cache entry — and it neither coalesced with nor consumed the preload
  issued for the same `<script type=module src>`, since that preload
  carries the JavaScript `Accept` and the module fetch did not. It now
  passes the document URL and the script `Accept`.
* Shutting down no longer hangs a caller waiting on a coalesced fetch.
  `ns_net_drain` discarded queued fetch tasks without telling the
  coalescer, so a task that led a group left the group behind: blocking
  joiners waited on a condition nobody would signal again, and
  asynchronous joiners never had their callback run. A blocking joiner
  now also gives up when the network layer starts aborting, and in any
  case five seconds past the longest transfer timeout a leader can have,
  instead of waiting without a bound. These waits happen on worker
  threads that teardown joins, so one that never returned took the
  joining thread down with it.
* The HTTP cache selects the right variant of a negotiated response.
  `cache.c` keyed entries on URL and partition and stored nothing about
  `Vary`, so a resource served `Vary: Accept` and referenced both as a
  stylesheet and as a script was fetched once and that single variant
  handed to both — a `<script>` element could receive CSS. Entries now
  carry the response's `Vary` and are keyed on a selector built from the
  request headers it names, with an indexed base key so a lookup can walk
  the variants stored for a URL and match the right one. `Accept`,
  `Accept-Language` and `User-Agent` are resolved; `Accept-Encoding` is
  ignored because bodies are stored decoded, which keeps the web's most
  common `Vary` from fragmenting the cache; anything else, including
  `Vary: *`, is not stored rather than stored wrongly. The preload scan
  deduplicates candidates on (URL, destination) instead of URL alone, so
  both variants are preloaded. The cache schema is versioned through
  `PRAGMA user_version` and an upgrade discards the old cache.
* The speculative preloader hands its bytes to the loader that needs
  them through a single deduplication point keyed on the request's
  identity. Preload responses used to be parked in a private store
  keyed on the bare URL and consulted ahead of the HTTP cache. That
  store ignored the cache partition, so within its 20-second window one
  site could be served bytes another site had fetched with that site's
  cookies; it ignored `no-store`; it recorded a placeholder for every
  fetch it started but only removed entries when a loader consumed one,
  so failed preloads and preloaded images — which nothing consumed —
  permanently occupied its 32 slots until the preloader silently
  stopped preloading anything. Deduplication now happens in one place.
  The in-flight coalescer keys on method, URL, cache partition and
  request headers rather than URL plus referrer, and every entry point
  joins it — `ns_net_request_async` and the blocking fetchers as well
  as `ns_net_fetch_async` — so a loader that arrives while a preload is
  still in flight waits for it instead of issuing a second request. A
  preload that finishes first is held in a preload map under that same
  key, handed over by the fetch layer itself so there is no window in
  which a resource is in neither place, and dropped when the next
  navigation begins. The preloader now sends the `Accept` header its
  consumer will send, so content-negotiated resources match. The
  separate external-script prefetcher, a third path over the same URLs,
  is gone. A page with six scripts and five stylesheets issues exactly
  one request per resource, counted at the origin.

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
* Nested flex rows no longer lay out in exponential time. A flex item
  that stretches to the line's cross size was laid out once against its
  natural height and then, because its own children had been aligned
  against the wrong height, laid out a second time. `align-items:
  stretch` is the default, so this doubled the work at every level of
  flex nesting: layout cost 2^depth. A synthetic page of nested
  stretched rows took 6.4 s at depth 14, 71 s at depth 16, and killed
  the renderer at depth 18. The stretched cross size is known before the
  item is laid out, so it is now handed down as the item's definite
  height on the first pass and the second pass is skipped. The same page
  takes 39 ms at depth 14, 89 ms at depth 16, and 1.3 s at depth 20.
  Real pages are shallower but wide: this is layout work removed from
  every flex row on every page, not only deep ones.
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
