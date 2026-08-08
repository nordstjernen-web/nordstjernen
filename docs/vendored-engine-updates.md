# Vendored engine updates — QuickJS & Lexbor

Nordstjernen carries in-tree forks of two upstream C libraries that we
modify freely for browser integration:

- **QuickJS** (`src/quickjs/`) — forked from
  [quickjs-ng](https://github.com/quickjs-ng/quickjs).
- **Lexbor** (`src/lexbor/`) — forked from
  [lexbor](https://github.com/lexbor/lexbor), trimmed to the modules we
  use (`core dom encoding html ns ports punycode tag unicode url utils`;
  upstream's `css engine selectors style test utils/*` tooling is not
  carried).

We track the upstream **main** branch. This document records each refresh:
the upstream point we rebased onto, the upstream changes that landed, and
the local modifications that were preserved on top.

## How an update is done

For each fork, the true fork-base commit is recovered (the upstream commit
whose tree equals ours minus our deliberate edits), then every carried file
is classified:

- **copy** — we never touched it, upstream did → take upstream's version.
- **keep** — only we touched it (upstream unchanged in range) → keep ours.
- **merge** — both touched it → 3-way merge (`git merge-file ours BASE
  theirs`), reviewing every hunk.

Sources are listed explicitly in each `meson.build`, so new upstream files
in unused areas are simply not compiled; the build is the final gate.

## 2026-08-08 — QuickJS e2c45218 → 954dc53, Lexbor cf07699 → de1d07a

### QuickJS

| | value |
|---|---|
| Fork base | `e2c45218` (master, "Fix integer multiplication cast to unsigned long in js_realloc_array()") |
| Updated to | `954dc53` (master, "Set version to 0.16.1") |
| Reported version | `0.15.1` → `0.16.1` (`QJS_VERSION_*` in `quickjs.h`) |

Nineteen upstream commits. The security fix is an out-of-bounds write in
libregexp's string-set emitter (`c2f37ec`): a `v`-flag class containing the
empty string, `/[\q{}]/v`, reached `memcmp`/`memcpy` with a NULL buffer, and
`re_emit_string_list` patched byte-code positions that a failed allocation had
never produced.

The language work is the iterator proposals — `Iterator.concat` returning a
proper iterator helper and closing correctly on a throwing value getter,
`Iterator.prototype.join`, `includes`, and the chunking proposal's `chunks` /
`windows`, with `take`/`drop` rejecting out-of-range limits — plus the throw
paths of the existing helpers. Elsewhere: externally managed ArrayBuffers can
now be resizable, the parser no longer rescans the line to find an
identifier's column, `libunicode-table.h` is reproducible from `unicode_gen`,
and `js_realloc2`'s allocation-slack feedback is gone.

**Local modifications preserved.** `quickjs.c`, `libregexp.c` and `quickjs.h`
all merged without a conflict, and the local delta against upstream is
byte-identical in content before and after — every browser hook listed in the
2026-06-26 entry, `JS_RepointArrayBuffer` through the regexp legacy captures,
carries over untouched.

**Adaptation required.** Upstream replaced the externally managed ArrayBuffer
free callback with a realloc-shaped one and gave `JS_NewArrayBuffer` a
`max_len` parameter:

- `JS_RepointArrayBuffer` tests `abuf->realloc_func` rather than the removed
  `free_func` to decide the memory is not quickjs's to manage.
- `src/wasm.c` (WASM linear memory) and `src/webgpu.c` (a mapped GPU buffer)
  pass `max_len = 0`, keeping both fixed-length; the WebGPU callback returns
  NULL for any non-zero size, which upstream reads as "cannot resize", so
  wgpu-owned memory is never moved or freed by the engine.

### Lexbor

| | value |
|---|---|
| Fork base | `cf07699` (main, "URL: fixed setters for empty hosts") |
| Updated to | `de1d07a` (main, "Test: added README.md") |
| Version | `3.1.0` (unchanged upstream) |

Fourteen upstream commits; three touch the modules we carry (the CSS::Syntax
and Style fixes are in modules this fork does not build).

- **URL** — a caret in a path is percent-encoded (#394), and a URL with
  userinfo but an empty host is rejected (#393). We had already made the caret
  fix locally, so the refresh takes upstream's spelling of it and the local
  delta shrinks accordingly.
- **HTML** — processing instructions, which the HTML Standard added after this
  fork was taken. `<?target data?>` now produces a real
  `lxb_dom_processing_instruction_t` instead of a bogus comment, with the
  specification's disallowed targets (`xml`, `xml-stylesheet`) still falling
  back to one.

**Local modifications preserved:** `unicode/idna*` (our IDNA layer),
`core/mraw.h` (unaligned-load fix), `url/url.c` (a port under a state override
keeps the hostname), and our in-tree `meson.build`.

**Local modification dropped:** the `LXB_HTML_TOKEN_TYPE_FROM_QM` token flag
and `lxb_dom_comment::from_bogus_qm`, which marked a comment as having come
from `<?` so that `src/html_lexbor.c` could scrape a target and data back out
of the comment text. Upstream's parser-level support supersedes it, and the
engine now reads the target and data directly off the DOM node.

### Verification

- `meson compile -C builddir` — the full browser builds and links cleanly with
  no new warnings.
- Headless runs over pages exercising both forks: the preserved QuickJS
  changes (`RegExp.$1`, the always-called sort comparator, the regexp `v`
  flag, `with`-object re-probe and `@@unscopables`, `Function.prototype.caller`),
  the new 0.16 iterator helpers, `WebAssembly.Memory` allocation and `grow`
  across the changed ArrayBuffer API, the processing-instruction node type,
  target, data and serialization, and the two URL fixes.

## 2026-07-28 — QuickJS 4d6fe60 → e2c45218

| | value |
|---|---|
| Fork base | `4d6fe60` (main, "Add Unicode license to libunicode-table.h", #1547) |
| Updated to | `e2c45218` (master, "Fix integer multiplication cast to unsigned long in js_realloc_array()") |
| Reported version | `0.15.1` (unchanged upstream; `QJS_VERSION_*` in `quickjs.h`) |

This refresh brings in 49 upstream commits. The security and robustness work
includes fixes for use-after-free in suspended coroutines, promise capability
creation, array growth and `DisposableStack`; double-free fixes for CallSite
and detached ArrayBuffer paths; integer-overflow fixes in Proxy `ownKeys`,
TypedArray sorting and array allocation; reference/leak fixes across Atomics,
iterators, JSON ropes and Set methods; hash-collision hardening; and a named
capture scope limit.

Other upstream changes include the arena allocator, the register-based regexp
engine, faster mixed numeric arithmetic and fast-array reads, full import
attribute module identity, TypedArray resize/overlap corrections, async
iterator closing fixes, improved module export/error behavior,
`CallSite.prototype.isConstructor()`, and corrected source positions.

The three-way merge preserves Nordstjernen's browser hooks and compatibility
work, including `JS_RepointArrayBuffer`, caller/function realm accessors,
browser-shaped stack traces and TypeErrors, native-facade formatting,
cross-frame caller handling, regexp legacy captures and `RGI_Emoji`, the
optimized prototype scan for `for…in`, and the local in-tree Meson build. The
local regexp string-property emitter was adapted to the new opcode set, and
the `for…in` optimization now accesses properties through the arena-aware
shape helper. The upstream async-iterator normal-completion fix was reconciled
with the local rejection-safe iterator stack layout. Upstream's generalized
import-attribute cache supersedes the local type-only cache while preserving
its behavior.

## 2026-06-26 — QuickJS 66adc82 → 4d6fe60, Lexbor 3.0.0 → 3.1.0

### QuickJS

| | value |
|---|---|
| Fork base | `66adc82` ("Only export API symbols for shared builds", #1525) |
| Updated to | `4d6fe60` (main, "Add Unicode license to libunicode-table.h", #1547) |
| Reported version | `0.15.1` (unchanged upstream; `QJS_VERSION_*` in `quickjs.h`) |

**Upstream changes pulled in** (15 commits since the fork base; those that
touch carried files):

- Implement the *nonextensible-applies-to-private* proposal.
- ArrayBuffer immutable-method semantics fixes (`sliceToImmutable` /
  `transferToImmutable` error types and coercion order).
- Enforce immutability on TypedArray write paths — adds a `require_mutable`
  argument to `js_typed_array___speciesCreate` and the related create paths.
- Implement the `Error.prototype.stack` accessor proposal.
- Add `JS_Free{Atoms,Values}` vararg macros (#1535).
- `-Wmaybe-uninitialized` fix in `js_binary_logic_slow`; test262, unicode
  table (+license), CI and meson-default-build housekeeping.

**Local modifications preserved** (`quickjs.c`, `quickjs.h`, `libregexp.c`):

- `JS_RepointArrayBuffer()` — repoint an externally-managed ArrayBuffer at a
  new backing store and update live views (browser-side hook).
- TypedArray `[[Set]]` only coerces the value when the receiver is the typed
  array itself (out-of-bounds index with a foreign receiver leaves the value
  untouched).
- `with`-statement object Environment Record semantics: `GetBindingValue` /
  `SetMutableBinding` re-probe the binding and raise `ReferenceError` in
  strict mode (`OP_with_get_var/_ref/_ref_undef/_put_var`).
- `Function.prototype.caller` getter resolving the live caller for
  non-strict functions (legacy web behaviour); `arguments` stays poisoned.
- Parser: `get`/`set` on its own line before a generator method is treated
  as a class field (ASI); computed property keys run through `ToPropertyKey`
  before the value expression.
- Module resolution: `export * as ns` ambiguity compared by canonical
  (module, binding) so the same namespace re-exported via different modules
  compares equal.
- Array sort always invokes a user comparator, even for bitwise-identical
  values (matches V8/JSC/SM; `jQuery.uniqueSort` relies on it).
- RegExp legacy static captures `RegExp.$1`…`$9`, updated on every successful
  `exec`.
- `%AsyncFromSyncIteratorPrototype%` closes the sync iterator on a rejected
  value promise (`closeOnRejection`).
- TypedArray `subarray` species-create omits the length argument (rather than
  passing `undefined`) for length-tracking results.
- `DOMException` prototype stored via `set_value` (drops a leaked reference).
- `libregexp.c`: the regexp `v` flag (unicodeSets) also enables full Unicode
  semantics (`is_unicode`).
- `quickjs.h`: `JS_EXTERN` / `JS_MODULE_EXTERN` only get default visibility
  for shared/module builds; `JS_RepointArrayBuffer` declaration.

The merge produced one conflict (subarray species-create) where our
length-omission edit met upstream's new `require_mutable` parameter; resolved
by keeping our branch and threading the new `false` (read-access) argument.

### Lexbor

| | value |
|---|---|
| Fork base | post-3.0.0 main (around `6772a05`) |
| Updated to | `cf07699` (main, "URL: fixed setters for empty hosts") |
| Version | `3.0.0` → `3.1.0` (`LEXBOR_VERSION_*`, `version`) |

**Upstream changes pulled in** (carried modules only):

- HTML: nobr adoption-agency fallback fix
  (`html/tree/insertion_mode/in_body.c`).
- URL: empty-host setter fixes — `lxb_url_host_is_empty()` helper, empty-host
  short-circuit in host copy/parse (`url/url.c`).
- DOM: expanded attribute constant/lookup tables
  (`dom/interfaces/attr_const.h`, `attr_res.h`).
- Version bump to 3.1.0 (`core/base.h`, `version`).

**Local modifications preserved:**

- `unicode/idna.c`, `unicode/idna.h`, `unicode/idna_validity.c` — our IDNA
  processing/validity layer (custom file plus heavy local edits).
- `core/mraw.h` — `lexbor_mraw_data_size` reads the size header via `memcpy`
  to avoid an unaligned load.
- `url/url.c` — port state under a state override returns `LXB_STATUS_OK`
  with an empty buffer, so `url.host = "example.com:invalid"` keeps the
  hostname instead of rejecting the assignment. This sits in a different
  region from upstream's empty-host fix and was combined cleanly by the
  3-way merge.
- `meson.build` — our in-tree build description (sources listed explicitly).

`url/url.c` merged with zero conflicts (our edit and upstream's empty-host
fix occupy disjoint regions); all other carried files were pure copy/keep.

### Verification

- `meson compile -C builddir` — full browser builds and links cleanly, no
  new warnings on the QuickJS/Lexbor objects.
- Headless run of a page whose inline script exercises the preserved engine
  changes: `RegExp.$1` static captures, always-called sort comparator, and
  the regexp `v` flag all behave correctly, and the Lexbor DOM dump parses
  `<select size>` / `<nobr>` as expected.
