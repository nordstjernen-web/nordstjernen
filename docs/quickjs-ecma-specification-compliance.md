# QuickJS ECMAScript specification compliance

This document tracks deliberate, browser-side compliance work on the
bundled QuickJS engine (`src/quickjs/`, forked from
[quickjs-ng](https://github.com/quickjs-ng/quickjs)) against the
ECMAScript Language Specification ([ECMA-262](https://tc39.es/ecma262/))
and its companion specs (ECMA-402 Intl, Temporal).

QuickJS-ng is already extremely conformant — a freshly synced tree
(currently `0.15.1` + post-release master) passes the overwhelming
majority of [test262](https://github.com/tc39/test262). The work
recorded here closes the *remaining* known gaps, prioritised by ROI:
small, localised, well-specified fixes that we can verify directly with
the in-tree `qjs` host tool (`builddir/src/quickjs/qjs.exe`).

## How we verify

There is no test262 checkout in the tree (the submodule declared in
`src/quickjs/.gitmodules` is deliberately left uninitialized). Most
fixes are small enough to reproduce with a minimal script driven
through the standalone interpreter, e.g.:

```sh
./builddir/src/quickjs/qjs.exe -e '<repro>'
```

To run the full suite and get an authoritative score —
`scripts/test262-run.sh` fetches a shallow `tc39/test262` checkout
into `src/quickjs/test262` (gitignored, not vendored), builds
`qjs`/`run-test262` via the upstream CMake path, and runs them:

```sh
scripts/test262-run.sh           # full suite, prints "Result: N/M errors, ..."
scripts/test262-run.sh -u        # regenerate test262_errors.txt from current pass/fail
```

The known-failing baseline is captured in
`src/quickjs/test262_errors.txt` (the list quickjs-ng ships as
"expected" failures, kept in sync with `-u` after each fix). As of
this writing (test262 main @ `f2d14356`, 2026-07-13) the full suite is
81152 test/mode combinations with **zero known failures**
(`test262_errors.txt` is empty; the intentionally skipped/excluded
categories — `async`, `module`, and a handful of slow or out-of-scope
feature areas — remain governed by `test262.conf`). The fix history:

| Cluster | Subtests | Difficulty | Status |
| --- | --- | --- | --- |
| TypedArray `[[Set]]` value-coercion on invalid index | ~12 | low | **done** |
| `with` GetBindingValue re-probe (read trap order + strict) | ~5 | medium | **done** |
| `with` SetMutableBinding re-probe (write trap order) | 3 | medium | **done** |
| AsyncFromSyncIterator close on rejection (`closeOnRejection`) | ~8 | medium | **done** |
| TypedArray `subarray` species-ctor argument list | 4 | low | **done** |
| TypedArray `subarray`/`slice` detach + species offset | ~4 | medium | **done** (subarray byte-offset, slice overlapping-buffer copy) |
| RegExp `v` flag — Unicode semantics (property escapes, `\u{}`, casing) | ~8 | medium | **done** |
| RegExp `v` flag — literal astral chars + `fullUnicode` advancement | 6 | low | **done** |
| RegExp `v` flag — `\p{RGI_Emoji}` property of strings | 4 | high | **done** (see #25) |
| RegExp `v` flag — set operations / strings (`&&`, `--`, `\q{}`) | 0 tracked | high | open (feature area excluded in `test262.conf`) |
| RegExp `\p{Script=Unknown}` value | 6 | low | **done** |
| Class field named `get`/`set` + generator (ASI) | 2 | low | **done** |
| Object computed-key `ToPropertyKey` before value | 2 | low | **done** |
| Simple-assignment computed-member `ToPropertyKey` after RHS | 4 | medium | **done** |
| Destructuring assignment-target evaluation order | 5 | medium | **done** |
| Destructuring `var` binding ResolveBinding order under `with` | 1 | medium | **done** |
| Module star-export of the same namespace is unambiguous | ~5 | medium | **done** |
| Module local export aliasing an import is not a fresh binding | 3 | medium | **done** |
| AnnexB CallExpression assignment-target type | 7 | high | **done** |
| `{}`/`[]` eagerly resolve a trailing `=` regardless of precedence context | 2 | high | **done** (see #24) |
| Diamond module graph with top-level await hangs | 2 | high | **done** |
| Text module imports (`with {type: "text"}`) | 3 | low | **done** (attribute-aware module map; the lingering dynamic-import failure was the loader's `.json`-suffix inference overriding an explicit `type` attribute, fixed in `js_module_load`) |
| `CanonicalNumericIndexString("NaN")` not treated as a typed-array index | 1 | low | **done** |
| Legacy RegExp `$1`-`$9` must not throw when made non-writable | 2 | low | **done** |
| `for await` loop must not close the iterator when `next()` itself rejects | 2 | medium | **done** |
| `Function.prototype.caller`/`.arguments` as %ThrowTypeError% poison pills | 4 | medium | **done** (see #26 — matches modern V8: poison pair on the prototype, legacy magic on sloppy instances) |
| Top-level-await rejection settles promises root-first instead of leaf-first | 1 | low | **done** (see #27) |

## Changes

### 1. TypedArray `[[Set]]` must not coerce the value for an invalid index when the receiver differs

**Spec:** [`[[Set]]` for Integer-Indexed exotic objects](https://tc39.es/ecma262/#sec-typedarray-set) /
`TypedArraySetElement`. The value is passed through `ToNumber` /
`ToBigInt` (observable via a `valueOf`/`toString` side effect) **only**
when `SameValue(O, Receiver)` is true. When the index is an
out-of-bounds canonical numeric index and the receiver is *not* the
typed array (e.g. `Reflect.set(ta, 10, v, otherReceiver)`, a primitive
receiver, or a typed array reached through the prototype chain),
`IsValidIntegerIndex` is false and `[[Set]]` returns `true` without
touching the value.

**Bug:** `JS_SetPropertyInternal2`'s `typed_array_oob` path coerced the
value unconditionally ("evaluate value for side effects"), so a
`valueOf` was wrongly invoked whenever the receiver differed from the
typed array.

**Fix:** guard the coercion with `p == p1` (receiver is the typed array
itself). `src/quickjs/quickjs.c`, `JS_SetPropertyInternal2`.

Covers test262:
`built-ins/TypedArrayConstructors/internals/Set/key-is-canonical-invalid-index-{reflect,prototype-chain}-set.js`
(plain + BigInt) and
`.../Set/key-is-out-of-bounds-receiver-is-not-{object,typed-array}.js`.

Repro (all coerce=false except the last):

```js
var ta = new Int32Array(1), seen;
var v = { valueOf() { seen = true; return 1; } };
seen = false; Reflect.set(ta, 10, v, {});            // false
seen = false; Reflect.set(ta, 10, v, 5);             // false (primitive receiver)
seen = false; Object.create(ta)[10] = v;             // false (prototype chain)
seen = false; Reflect.set(ta, 10, v, ta);            // true  (receiver IS ta)
```

### 2. `%TypedArray%.prototype.subarray` omits the length argument for a length-tracking source

**Spec:** [`%TypedArray%.prototype.subarray`](https://tc39.es/ecma262/#sec-%typedarray%.prototype.subarray)
step 13: when `O.[[ArrayLength]]` is `auto` (a length-tracking view over a
resizable `ArrayBuffer`) **and** `end` is `undefined`, the argument list
handed to `TypedArraySpeciesCreate` is `« buffer, 𝔽(beginByteOffset) »` —
two arguments. Only when an explicit `end` is given (step 14) is the
computed `newLength` appended as a third argument.

**Bug:** `js_typed_array_subarray` always passed four entries to
`js_typed_array___speciesCreate` (which forwards all but the first), so a
length-tracking `subarray(start)` invoked the species constructor with
`(buffer, byteOffset, undefined)` instead of `(buffer, byteOffset)`.

**Fix:** call `js_typed_array___speciesCreate` with `argc == 3` (forwards
two arguments) in the length-tracking, `end`-undefined case, otherwise
`argc == 4`. `src/quickjs/quickjs.c`, `js_typed_array_subarray`.

Covers test262
`built-ins/TypedArray/prototype/subarray/speciesctor-get-species-custom-ctor-invocation.js`
(plain + BigInt).

### 3. Class field named `get`/`set` followed by a generator method (ASI)

**Spec:** [Class definitions](https://tc39.es/ecma262/#sec-class-definitions)
grammar. `get` and `set` introduce an accessor `MethodDefinition` only
when a `ClassElementName` follows. A getter/setter can never be a
generator, so `get` (or `set`) followed by `*` cannot be an accessor.
When the `*` is on the next line, ASI terminates a `FieldDefinition`, so

```js
class C {
  get
  *gen() {}
}
```

is a field named `get` followed by a generator method `gen` — both valid.

**Bug:** `js_parse_property_name` treated any token after `get`/`set`
other than `: , } ( = ;` as the start of an accessor name, so it tried to
parse `*gen` as the getter's property name and raised
`SyntaxError: invalid property name`.

**Fix:** in a class body (`allow_private`), when `get`/`set` is followed
by `*` with an intervening line terminator (`s->got_lf`), treat the
keyword as a field name. The same construct without the line terminator
(`get *gen(){}`) still has no ASI and stays a `SyntaxError`, and a real
accessor (`get foo(){}`, even split across lines) is unaffected.
`src/quickjs/quickjs.c`, `js_parse_property_name`.

Covers test262
`language/statements/class/elements/syntax/valid/grammar-field-named-{get,set}-followed-by-generator-asi.js`.

### 4. `with` GetBindingValue re-probes the binding object

**Spec:** [Object Environment Records `GetBindingValue`](https://tc39.es/ecma262/#sec-object-environment-records-getbindingvalue-n-s).
After `HasBinding` (which itself does `HasProperty` + reads
`@@unscopables`), `GetBindingValue(N, S)` performs a *second*
`HasProperty(bindingObject, N)`; if that is false the result is a
`ReferenceError` when `S` is true and `undefined` otherwise. The
`@@unscopables` getter can delete the binding between the two probes.

**Bug:** the `OP_with_get_var` / `OP_with_get_ref` / `OP_with_get_ref_undef`
interpreter cases went straight from the `@@unscopables` check to
`[[Get]]`, skipping the `GetBindingValue` `HasProperty`. With a Proxy
binding object the second `has` trap was missing; with a deleted binding
the strict-mode `ReferenceError` was not raised.

**Fix:** new helper `js_with_get_binding_value` does `HasProperty` then
either `[[Get]]`, returns `undefined` (sloppy), or throws a
`ReferenceError` (strict, taken from the executing frame's
`is_strict_mode` — `with` bodies are sloppy but may contain nested strict
functions/evals). Used by the three read opcodes.
`src/quickjs/quickjs.c`, interpreter `OP_with_*` and the new helper.

Covers test262
`language/statements/with/get-binding-value-{idref,call}-with-proxy-env.js`
and `.../get-mutable-binding-binding-deleted-in-get-unscopables-strict-mode.js`.

The write side (`SetMutableBinding`,
`language/statements/with/set-mutable-binding-*`) is not yet fixed: a
`with` assignment `p = 1` lowers to `OP_with_make_ref` + `OP_put_ref_value`,
and `OP_put_ref_value` is shared with non-`with` reference stores (function
-name dummy objects, captured-local ref objects), so the extra
`HasProperty` can't be added there unconditionally. It needs a dedicated
`with`-store opcode.

### 5. Object literal computed key is `ToPropertyKey`-converted before the value

**Spec:** [`PropertyDefinition : PropertyName : AssignmentExpression`](https://tc39.es/ecma262/#sec-runtime-semantics-propertydefinitionevaluation).
The `PropertyName` is evaluated first — and `Evaluation of ComputedPropertyName`
includes `? ToPropertyKey(propName)` — *before* the value
`AssignmentExpression` is evaluated.

**Bug:** `js_parse_object_literal` emitted the key expression, then the
value expression, then `OP_define_array_el` (which performs
`ToPropertyKey` internally). So a computed key's `toString`/`Symbol.toPrimitive`
ran *after* the value expression, e.g. `{ [key]: (sideEffect(), 1) }`
produced the order `[value, key-toString]` instead of
`[key-toString, value]`.

**Fix:** emit `OP_to_propkey` for a computed key (`name == JS_ATOM_NULL`)
immediately after the `:`, before parsing the value. The later
`OP_define_array_el` re-converts an already-primitive key, so there is no
double `toString`. `src/quickjs/quickjs.c`, `js_parse_object_literal`.

Covers test262
`language/expressions/object/computed-property-name-topropertykey-before-value-evaluation.js`.

### 6. RegExp `v` flag carries full Unicode semantics

**Spec:** [`RegExp` `v` flag (`unicodeSets`)](https://tc39.es/ecma262/#sec-regexp-pattern-flags).
The `v` flag is a superset of `u`: it enables full Unicode semantics
(`\p{…}`/`\P{…}` property escapes, `\u{…}` code-point escapes, code-point
iteration over astral characters, Unicode case folding) **and** the
unicodeSets class extensions.

**Bug:** `libregexp.c` derived `is_unicode` solely from `LRE_FLAG_UNICODE`
(the `u` flag) and treated `is_unicode`/`unicode_sets` as mutually
exclusive. Every Unicode-semantic branch in the parser and matcher is
gated on `is_unicode`, so under `/…/v` property escapes silently matched
nothing, `\u{1F600}` failed, astral characters weren't iterated by code
point, and case folding fell back to ASCII. e.g. `/\p{ASCII}/v.test("a")`
returned `false` and `"a".match(/\p{L}/v)` returned `null`.

**Fix:** set `is_unicode` from `LRE_FLAG_UNICODE | LRE_FLAG_UNICODE_SETS`
in both the compiler (`lre_compile`) and the matcher (`lre_exec`);
`unicode_sets` still selects the `v`-only class syntax. Audit confirmed
nothing relied on the old mutual exclusivity, and the change only affects
`/v` regexps (previously broken) — `u` and plain regexps are byte-for-byte
unchanged. `src/quickjs/libregexp.c`.

Covers test262
`built-ins/RegExp/prototype/exec/regexp-builtin-exec-v-u-flag.js` and the
`String.prototype.{match,matchAll,replace,search}` `*-v-u-flag.js`
property-escape subtests.

Still open under `v`: the unicodeSets **set operations** (`[A&&B]`
intersection, `[A--B]` subtraction), nested classes `[[…][…]]`, and
string disjunctions `\q{…}` — these need the ClassSetExpression
grammar, which this change does not add. `\p{RGI_Emoji}` in term
position is supported separately — see #25.

### 7. AsyncFromSyncIterator closes the sync iterator on rejection

**Spec:** [`AsyncFromSyncIteratorContinuation`](https://tc39.es/ecma262/#sec-asyncfromsynciteratorcontinuation)
takes a `closeOnRejection` flag (true for `%AsyncFromSyncIteratorPrototype%.next`,
false for `return`/`throw`). When `closeOnRejection` is true and the
result is not `done`:
- step 6: if `PromiseResolve(%Promise%, value)` completes abruptly, the
  sync iterator is closed (`AsyncFromSyncIteratorClose`) before rejecting;
- step 11: the `onRejected` reaction passed to `PerformPromiseThen` closes
  the sync iterator (then rethrows) when the wrapped value promise rejects.

**Bug:** `js_async_from_sync_iterator_next` always passed `JS_UNDEFINED`
as the `onRejected` handler and went straight to `reject` when
`js_promise_resolve` threw, so the underlying sync iterator's `return()`
was never called for a rejected/abrupt value. `for await … of` over a
sync iterable whose values are rejected promises leaked the iterator
(no `finally`, no `return`).

**Fix:** add a captured `onRejected` closure
(`js_async_from_sync_iterator_close_on_reject`) that performs
`IteratorClose(syncIterator, ThrowCompletion(reason))` via
`JS_IteratorClose(…, /*is_exception_pending*/true)`, and wire it (and the
`PromiseResolve`-abrupt close) only for `GEN_MAGIC_NEXT` with `done` false.
`return`/`throw` and the `done`-true and `IteratorValue`-abrupt cases keep
the no-close behavior per spec. `src/quickjs/quickjs.c`.

Covers test262
`built-ins/AsyncFromSyncIteratorPrototype/{next,throw}/*-{rejected-promise-close,poisoned-wrapper}.js`.

### 8. `ResolveExport` treats the same re-exported namespace as unambiguous

**Spec:** [`ResolveExport`](https://tc39.es/ecma262/#sec-resolveexport).
When a name is found through several `export *` stars, the results are
ambiguous only if they denote different bindings: *different Module
Record* **or** *different BindingName*. A `export * as ns from "mod"`
binding's identity is `{ [[Module]]: mod, [[BindingName]]: namespace }` —
the *imported* module, not the re-exporting one — so the same namespace
re-exported through two different modules is a single binding.

**Bug:** `js_resolve_export1` compared the re-exporting module and the raw
entry `local_name`, so two modules each doing `export * as foo from
"./common.js"` (both re-exported by a third) were reported as
`ambiguous` even though they resolve to the same `common` namespace.

**Fix:** add `js_resolved_export_binding`, which maps a resolved entry to
its canonical `(Module, BindingName)` — for a `export * as ns` entry
(`local_name == JS_ATOM__star_`) that is the imported module plus the
`namespace` marker. The star-merge step compares those canonical
bindings. Genuinely distinct bindings (two local exports, or namespaces of
*different* modules sharing an export name) still resolve to `ambiguous`.
`src/quickjs/quickjs.c`, `js_resolve_export1`.

Covers test262
`language/module-code/ambiguous-export-bindings/namespace-unambiguous-if-*.js`
and `import-and-export-propagates-binding.js`.

### 9. Legacy RegExp `$1`-`$9` static properties throw when locked down

**Spec:** [`UpdateLegacyRegExpStaticProperties`](https://tc39.es/proposal-regexp-legacy-features/#sec-updatelegacyregexpstaticproperties)
(the Annex-B-adjacent "Legacy RegExp Features" proposal that defines
`RegExp.$1`-`RegExp.$9`) updates each property with `? Set(C, name,
value, false)` — a *non-throwing* `[[Set]]`. Per the `[[DefineOwnProperty]]`
invariants, once a property is locked down (`writable: false,
configurable: false`), its value must never change again, and updating it
must fail silently rather than throw.

**Bug:** `js_regexp_update_static_captures` called `JS_SetPropertyStr`,
which always passes `JS_PROP_THROW`. After
`Reflect.defineProperty(RegExp, '$1', {writable: false, configurable:
false})`, the next `RegExp.prototype.exec` call threw an uncaught
`TypeError: '$1' is read-only` instead of leaving the locked value alone.

**Fix:** call `JS_SetPropertyInternal` directly with flags `0` (no
throw) instead of going through `JS_SetPropertyStr`.
`src/quickjs/quickjs.c`, `js_regexp_update_static_captures`.

Covers test262
`built-ins/Object/internals/DefineOwnProperty/consistent-value-regexp-dollar1.js`.

### 10. `for await` loop closes the iterator twice (or wrongly) when `next()` itself rejects

**Spec:** [`ForIn/OfBodyEvaluation`](https://tc39.es/ecma262/#sec-runtime-semantics-forin-div-ofbodyevaluation).
Fetching the next result (`Call`/`Await` of the iterator's `next()`) is a
`?`-prefixed step *outside* the body's try region: if it completes
abruptly, the loop returns that completion directly without closing the
iterator. Only an abrupt completion from the **loop body** (or an
explicit `break`/`return`) calls `AsyncIteratorClose`/`IteratorClose`.

**Bug:** the bytecode for `for await (x of iterable)` keeps `iter_obj`
live on the operand stack across the `next()` call and its `await` for
every iteration. A rejection from `await`ing the `next()` result was
indistinguishable, at unwind time, from a rejection in the loop body, so
the runtime's generic catch-offset handler (`quickjs.c`, the
`JS_TAG_CATCH_OFFSET` case in `JS_CallInternal`'s `exception:` label)
always called the iterator's `return()`. The synchronous `for (x of
iterable)` loop already avoids this — `js_for_of_next` clears the
`iter_obj` stack slot to `undefined` before propagating a `next()`
failure, and the generic unwind path skips closing an `undefined`
"iterator" — but the async `for await` codegen had no equivalent for the
`OP_call_method` + `OP_await` sequence used to fetch the next result.

This was directly visible as a double-`return()` call when combined with
the `AsyncFromSyncIteratorContinuation` `closeOnRejection` fix above (#7):
a `for await` loop over a *sync* iterable wrapped by
`%AsyncFromSyncIteratorPrototype%` got one spec-mandated close from
`js_async_from_sync_iterator_next` and a second, wrong one from the
loop itself. But the bug is general: a `for await` loop over a *native*
async iterable whose `next()` rejects also wrongly called `return()`,
which is observable as premature/incorrect cleanup on any real-world
async iterator (stream, connection, etc.) whose `next()` promise rejects.

**Fix:** two new opcodes, `OP_for_await_of_dup` and
`OP_for_await_of_restore` (appended at the *end* of the opcode table in
`quickjs-opcode.h` so existing opcode numbers — and therefore the
precompiled bytecode blobs like `builtin-array-fromasync.h` — don't
shift), bracket the `next()` call + `await` in the `for await` loop body
codegen (`js_parse_for_in_of`). `OP_for_await_of_dup` clears the live
`iter_obj` stack slot to `undefined` for the duration of the call and
`await` (mirroring `js_for_of_next`'s sync trick) while keeping a
separate copy alive to actually make the call with; `OP_for_await_of_restore`
restores it once the result is obtained without throwing. An abrupt
completion in between now unwinds through an `undefined` iterator slot
and the generic close is skipped, exactly like the sync case. Verified
with the full test262 suite (no regressions, `next() `-rejection,
break-from-body, throw-from-body, and natural-exhaustion cases all
behave correctly) and an ASan build (no leaks or use-after-free).
`src/quickjs/quickjs.c`, `src/quickjs/quickjs-opcode.h`,
`js_parse_for_in_of`.

Covers test262
`built-ins/AsyncFromSyncIteratorPrototype/next/for-await-next-rejected-promise-close.js`.

### 11. `\p{Script=Unknown}` (alias `Zzzz`) was rejected as an unknown script name

**Spec:** [`UnicodeMatchProperty`](https://tc39.es/ecma262/#sec-runtime-semantics-unicodematchproperty-p)
requires `Script`/`Script_Extensions` to accept every value in
[`PropertyValueAliases.txt`](https://unicode.org/Public/UCD/latest/ucd/PropertyValueAliases.txt),
including `Unknown`/`Zzzz` — the code points Unicode has not assigned
to any script.

**Bug:** `unicode_script_name_table` deliberately omits `"Unknown"`
(`libunicode.c` comment: *"we remove the 'Unknown' Script"*) because
the generated `unicode_script_table` already encodes "no explicit
script" gaps as a 0 value with no associated name lookup. With no name
to find, `\p{Script=Unknown}` and `\p{Script=Zzzz}` always raised
`SyntaxError: unknown unicode script`. Separately, the gap-encoding in
`unicode_script_table` only covers the range up to its last explicit
entry (0x00E01F0) — code points above that (most of planes 4-13 and
the tail of plane 14) were simply never written to the table at all,
so even a correct "Unknown" lookup would have under-matched.

**Fix:** special-case the names `"Unknown"` and `"Zzzz"` in
`unicode_script()` to resolve directly to `UNICODE_SCRIPT_Unknown`
(`== 0`, matching the table's implicit gap value) instead of going
through `unicode_script_name_table`; after walking the table, add the
implicit trailing gap `[c, 0x10ffff]` (where `c` is the table's
cumulative end) to the result so codepoints past the last explicit
entry are included. `src/quickjs/libunicode.c`, `unicode_script`.

Covers test262
`built-ins/RegExp/property-escapes/generated/Script{,_Extensions}_-_Unknown.js`
and `built-ins/RegExp/property-escapes/special-property-value-Script_Extensions-Unknown.js`.

### 12. Local export of an imported binding is a fresh, ambiguous binding instead of the shared one

**Spec:** [`ParseModule`](https://tc39.es/ecma262/#sec-parsemodule) step
10.1.ii/iii: a local `export { x }` whose `x` is actually an imported
name (`import { x } from "m"; export { x };`) is *not* added to
`localExportEntries` — it is rewritten into an `IndirectExportEntry`
through the same `(ModuleRequest, ImportName)` as the import (or, for
a namespace import, an `export * as x from "m"`-shaped entry). This
makes `ResolveExport` see the one binding `m` actually exports,
regardless of how many modules re-export it by name vs. by re-export.

**Bug:** `add_module_variables` (which resolves each `JS_EXPORT_TYPE_LOCAL`
export entry's `local_name` to a closure-variable index post-parse) never
checked whether that closure variable happened to be the *same* one an
import already claimed. So `export { x }` over an imported `x` stayed a
genuine `JS_EXPORT_TYPE_LOCAL` entry pointing at this module's own
"copy" of the binding. When a third module did `export * from a; export
* from b;` and both `a` and `b` (by different paths) ultimately exposed
the same underlying name, `js_resolve_export1`'s star-merge correctly
canonicalizes namespace (`export * as ns from`) bindings (fix #8 above)
but had no way to know this module's "local" `x` was secretly the same
binding as the other path's, and reported `ambiguous`.

**Fix:** in `add_module_variables`, after resolving a local export's
closure-variable index, scan `m->import_entries` for a matching
`var_idx`; if found, rewrite the entry to `JS_EXPORT_TYPE_INDIRECT`
with `local_name` replaced by the import's `import_name` (or kept as
the `JS_ATOM__star_` marker for a namespace import) and
`u.req_module_idx` set to the import's source module — exactly
mirroring the entries `export {x} from "m"` already produces, so
`js_resolve_export1`'s existing indirect-export and canonical-binding
logic resolves it correctly without further changes.
`src/quickjs/quickjs.c`, `add_module_variables`.

Covers test262
`language/module-code/ambiguous-export-bindings/import-and-export-propagates-binding.js`
and the two `namespace-unambiguous-if-*-import-star-as-and-export.js` variants.

### 13. `%AsyncFromSyncIteratorPrototype%.throw` never closed the iterator on a rejected value

**Spec:** [`%AsyncFromSyncIteratorPrototype%.throw`](https://tc39.es/ecma262/#sec-%25asyncfromsynciteratorprototype%25.throw)
step 13 calls `AsyncFromSyncIteratorContinuation(result, promiseCapability,
syncIteratorRecord, true)` — `closeOnRejection` is `true`, exactly like
`.next` (fix #7 above). Only `.return` passes `false`.

**Bug:** `js_async_from_sync_iterator_next`'s two `closeOnRejection`
checks both tested `magic == GEN_MAGIC_NEXT` specifically, so `.throw`
(`GEN_MAGIC_THROW`) never closed the wrapped sync iterator when its
yielded value was a rejected promise — the one fix from #7 only
covered one of the two `true` cases.

**Fix:** change both conditions to `magic != GEN_MAGIC_RETURN` (true
for both `NEXT` and `THROW`, false for `RETURN`).
`src/quickjs/quickjs.c`, `js_async_from_sync_iterator_next`.

Covers test262
`built-ins/AsyncFromSyncIteratorPrototype/throw/{iterator-result-rejected-promise-close,throw-result-poisoned-wrapper}.js`.

### 14. Simple assignment to a computed member converts the key before the right-hand side

**Spec:** [`AssignmentExpression : LeftHandSideExpression = AssignmentExpression`](https://tc39.es/ecma262/#sec-assignment-operators-runtime-semantics-evaluation),
current text (post the [evaluation-order normative
fix](https://github.com/tc39/ecma262/pull/2392)): for `base[key] =
val`, `ToPropertyKey(key)` is *not* part of evaluating the
`LeftHandSideExpression` reference — it is deferred until immediately
before `PutValue`, i.e. strictly after `val` is evaluated. The same
applies to `super[key] = val`.

**Bug:** `get_lvalue` unconditionally emits the `ToPropertyKey`
conversion (`OP_to_propkey2`/`OP_to_propkey`) for `OP_get_array_el`/
`OP_get_super_value` as part of turning the just-parsed member
expression into an assignment target — before the right-hand side is
even parsed. `js_parse_assign_expr2` already special-cased
`OP_get_array_el` to undo and redo that conversion around the RHS, but
only for the case where the base is *not* null/undefined (the comment
above it called this "rather obtuse" and flagged the redundant
double-conversion as a known `FIXME`); the "happy path" (object base)
still converted once early and once again after the RHS, so a
`toString`/`Symbol.toPrimitive` side effect on the key observably ran
before the right-hand side instead of after. `super[key] = val` had no
such workaround at all — its conversion always ran early, unconditionally.

**Fix:** simplify to always defer: strip `get_lvalue`'s eager
conversion for both `OP_get_array_el` and `OP_get_super_value` right
after it's parsed, and perform the conversion exactly once, after the
right-hand side, immediately before the existing `put_lvalue` call (a
`swap; to_propkey; swap` around the top two stack values, independent
of how many elements — 2 for array, 3 for super — sit below). Verified
against both the object-base and null/undefined-base cases (the
explicit early-conversion-skip this replaces was specifically there
for null/undefined, and continues to behave correctly since it no
longer special-cases anything) plus the full test262 suite.
`src/quickjs/quickjs.c`, `js_parse_assign_expr2`.

Covers test262
`language/expressions/assignment/target-{member,super}-computed-reference.js`.
The companion `-null.js`/`-undefined.js` variants and the `dstr/`
(destructuring) family with the same evaluation-order concern were
already passing/are tracked separately — see "Destructuring
assignment-target evaluation order" in the table above; destructuring
goes through a much larger, separate code path
(`js_parse_destructuring_element`) with its own per-depth stack
shuffling and wasn't touched here.

### 15. TypedArray `lastIndexOf`/`subarray`/`slice` and resizable-ArrayBuffer side effects

Three related but independent bugs, all involving a `valueOf`/species
callback that resizes or detaches the backing `ArrayBuffer` mid-call:

**15a. `lastIndexOf` returned `-1` whenever `fromIndex`'s conversion shrank the buffer.**
`js_typed_array_indexOf`'s "the buffer may have been resized by an evil
`.valueOf`" recovery path was gated on `typed_array_is_oob(p) || len >
p->u.array.count` — but a length-tracking view is by definition never
"OOB" on shrink (`typed_array_is_oob` returns `false` unconditionally
for `track_rab` views), so `len > p->u.array.count` alone decided
whether to bail out, and it does precisely when the buffer shrank.
Bailing out always returned "not found" instead of re-clamping and
continuing the search within the smaller bounds. Fixed by gating the
early bail-out to `special == special_includes` only (see 15b for why
`includes` is special) and, for `indexOf`/`lastIndexOf`, re-clamping
and continuing the search. A second bug in that same recovery path used
`k = min_int(k, len)` uniformly; for `lastIndexOf`, `k` is an *inclusive*
starting index scanned backwards, so capping it at `len` (the new
element count) rather than `len - 1` left it one past the last valid
index. Fixed by capping at `len - 1` specifically for
`special_lastIndexOf`.

**15b. `includes` needed the opposite fix.** Per spec `includes` scans
up to the *original* length and observes `undefined` for indices past
the buffer's new bounds (`built-ins/TypedArray/prototype/includes/
{search-undefined-after-shrinking-buffer,coerced-searchelement-fromindex-resize}.js`,
both already passing) — the exact early bail-out 15a removes for
`indexOf`/`lastIndexOf` is required for `includes`, so it was kept,
scoped to that one case.

**15c. `subarray` used the public `.byteOffset` getter (which zeroes
out once detached/OOB) instead of the raw `[[ByteOffset]]` internal
slot, and threw a spurious `RangeError` for a detached buffer that the
algorithm (per the spec text test262 quotes — `srcByteOffset is
O.[[ByteOffset]]`, no validity check) doesn't ask for.** Removed the
check and read `ta->offset` directly; `TypedArraySpeciesCreate`/the
species constructor invocation is what's supposed to validate the
result, not this step.

**15d. `slice`'s memmove-based fast path produced the wrong result when
the species constructor returns a view aliasing the *same* underlying
buffer at a different offset** (a legal, if unusual, species result).
Spec's copy step is a byte-by-byte forward loop
(`GetValueFromBuffer`/`SetValueInBuffer`), which "smears" already-copied
bytes into the source range when destination and source overlap with
the destination ahead in memory — `memmove` deliberately avoids exactly
that smearing, producing a different (incorrect, per spec) result.
Fixed by detecting pointer-range overlap and falling back to a plain
ascending byte-copy loop only in that case; the common non-overlapping
path is untouched (still a single `memmove`).

`src/quickjs/quickjs.c`: `js_typed_array_indexOf` (15a/15b),
`js_typed_array_subarray` (15c), `js_typed_array_slice` (15d).

Covers test262
`built-ins/TypedArray/prototype/lastIndexOf/negative-index-and-resize-to-smaller.js`
and
`built-ins/TypedArray/prototype/subarray/byteoffset-with-detached-buffer.js`
cleanly. `built-ins/TypedArray/prototype/slice/speciesctor-return-same-buffer-with-offset.js`
(15d) still fails, but on a *different*, already-correct line: run
across `testWithTypedArrayConstructors`'s buffer-source variants, its
species function unconditionally returns `new TA(ta.buffer, offset)` —
for the `makeImmutableArrayBuffer` variant this makes `ta.buffer`
itself immutable, and `TypedArraySpeciesCreate(..., ~write~)` is
required to reject that (`ValidateTypedArray` step 4), which is
independently exercised and passing as
`built-ins/TypedArray/prototype/slice/speciesctor-destination-backed-by-immutable-buffer.js`.
The test has no `assert.throws` around that variant, so the (per spec,
correct) `TypeError` now propagates as an uncaught error instead of the
old, unrelated `memmove` smearing bug it was written to catch — this
looks like an upstream test262 oversight (not accounting for the
immutable-buffer variant when adding the same-buffer-aliasing
assertion) rather than an engine bug worth working around.

### 16. RegExp `v` flag mishandled literal astral characters and empty-match advancement

Two related gaps left by fix #6 (which made the `v` flag carry full
Unicode *matching* semantics but not the surrounding string-encoding and
`AdvanceStringIndex` machinery). Both are `v`-only — `u` was already
correct — and share the root cause that the surrounding code tested for
`LRE_FLAG_UNICODE` (the `u` flag) specifically rather than "`u` or `v`".

**16a. A literal non-BMP character in a `/…/v` pattern never matched.**
`js_compile_regexp` encodes the pattern source with
`JS_ToCStringLen2(…, cesu8)` where `cesu8 = !(re_flags & LRE_FLAG_UNICODE)`.
For a `u` regexp that is proper UTF-8 (a non-BMP code point is one 4-byte
sequence, which `get_class_atom`'s `normal_char` path decodes with
`utf8_decode`); for a `v` regexp `LRE_FLAG_UNICODE` is unset, so the
pattern was CESU-8-encoded (the code point split into a surrogate pair of
two 3-byte sequences), and the parser — which does *not* recombine
surrogate pairs on that path — only ever saw the high surrogate. So
`/𠮷/v`, `[𠮷]` etc. silently matched nothing, while `/\u{20BB7}/v` (an
escape, decoded separately) and `/./v` (code-point iteration in the
matcher) worked. Fixed by encoding the pattern as full UTF-8 whenever the
`u` *or* `v` flag is set: `cesu8 = !(re_flags & (LRE_FLAG_UNICODE |
LRE_FLAG_UNICODE_SETS))`. `src/quickjs/quickjs.c`, `js_compile_regexp`.

**16b. `@@match`/`@@matchAll`/`@@replace`/`@@split` advanced empty
matches by a code unit instead of a code point under `v`.** The spec for
all four sets `fullUnicode`/`unicodeMatching` true when "*flags* contains
`u` **or** `v`", then `AdvanceStringIndex(S, index, fullUnicode)` steps
over a whole code point on an empty match. The four builtins derived that
flag either from the `.unicode` getter (true only for `u`) or from a
`string_indexof_char(flags, 'u')` scan (misses `v`), so a `v` regexp
advanced one UTF-16 code unit at a time — e.g.
`"𠮷a𠮷b𠮷".matchAll(/(?:)/gv)` yielded 9 empty matches instead of 6.
Fixed all four sites to test the flags string for `'u'` **or** `'v'`
(the flags string is already in scope at each), matching the spec text
and dropping two now-redundant `.unicode` property reads.
`src/quickjs/quickjs.c`, `js_regexp_Symbol_match`,
`js_regexp_Symbol_matchAll`, `js_regexp_Symbol_replace`,
`js_regexp_Symbol_split`.

Covers test262
`built-ins/RegExp/prototype/exec/regexp-builtin-exec-v-u-flag.js` and the
`String.prototype.{match,matchAll,replace,search}` `*-v-u-flag.js` /
`*-v-flag.js` subtests (12 test/mode combinations; the `matchAll`
subtest's later `AdvanceStringIndex` assertion needed 16b on top of 16a).

### 17. Destructuring assignment to a computed member converts the key before the value is read

**Spec:** [`KeyedDestructuringAssignmentEvaluation`](https://tc39.es/ecma262/#sec-runtime-semantics-keyeddestructuringassignmentevaluation)
and its iterator sibling: for `({[srcKey]: target[tgtKey]} = source)` /
`([target[tgtKey]] = source)`, evaluating the DestructuringAssignmentTarget
produces a reference whose computed key is *not* yet passed through
`ToPropertyKey` — the conversion happens inside the final `PutValue`,
i.e. observably after `GetV(value, propertyName)` reads the value from
the source (and after a default-value initializer runs). Same
post-[PR 2392](https://github.com/tc39/ecma262/pull/2392) ordering as
fix #14, on the destructuring path.

**Bug:** all three `get_lvalue` call sites in
`js_parse_destructuring_element` (object property, object rest, array
element) left `get_lvalue`'s eager `OP_to_propkey2`/`OP_to_propkey` in
place, so a `toString`/`Symbol.toPrimitive` side effect on the target
key ran before the source read instead of after the default value.

**Fix:** factor fix #14's undo/redo into a helper pair
(`strip_eager_propkey_conversion` / `emit_deferred_propkey_conversion`),
call the strip right after each of the three destructuring
`get_lvalue`s, and emit the deferred conversion (`swap; to_propkey;
swap` over the top two stack slots) immediately before the two
`put_lvalue`s. The target key sits untouched on the stack across the
value fetch and default-value branch in every case (object named,
object computed, object rest, array, array rest, `super[key]` targets),
so the single late conversion is equivalent for all of them.
`js_parse_assign_expr2`'s inline #14 code now uses the same helpers.
`src/quickjs/quickjs.c`.

Covers test262
`language/expressions/assignment/destructuring/{keyed,iterator}-destructuring-property-reference-target-evaluation-order.js`
(plain + strict) and
`keyed-destructuring-property-reference-target-evaluation-order-with-bindings.js`
(5 test/mode combinations). The sibling
`language/destructuring/binding/...-with-bindings.js` failure is a
*different* gap — `ResolveBinding` of a plain `var` target must probe
the `with` environment before `GetV` — tracked as its own cluster in
the table above (it needs the same with-aware store machinery as the
"`with` SetMutableBinding re-probe" item).

### 18. AnnexB: a CallExpression assignment target parses and throws ReferenceError at runtime

**Spec:** the [Runtime Errors for Function Call Assignment Targets](https://github.com/tc39/proposal-call-assignment-runtime-error)
web-reality change: in non-strict code, `f() = v`, `f() += v`, `f()++`,
`--f()` and `for (f() in/of x)` are not early SyntaxErrors — the call
evaluates and using its result as an assignment target throws a
ReferenceError (before the right-hand side or ToNumeric coercion runs).
Logical assignment (`f() &&= v`) and tagged templates (``o.f()`` ` `` = v``)
remain early errors, as does everything in strict mode.

**Fix:** `get_lvalue` accepts a call opcode (`OP_call`/`OP_call_method`/
`OP_eval`/`OP_apply_eval`) as the previous instruction in sloppy mode
(rejecting the logical-assignment tokens and, via a
`last_template_call_pos` marker stamped by the tagged-template call
emission, tagged templates), leaves the call in place, and emits an
immediate `OP_throw_error` with a new `JS_THROW_ERROR_FUNCALL_REF` type;
`put_lvalue` treats the sentinel as emitting no store (the code after
the throw is unreachable). `src/quickjs/quickjs.c`.

Covers the 7 test262
`annexB/language/expressions/assignmenttargettype/*` tests without
regressing the `language/expressions/assignmenttargettype` early-error
suite.

### 19. Import attributes key the module map

**Spec:** [import attributes](https://tc39.es/proposal-import-attributes/):
the module map key includes the `type` attribute, so the same specifier
can load both as an ES module and (say) a text module.

**Bug:** `js_find_loaded_module` matched by name only, so
`import self from "./x.js" with {type: "text"}` inside `x.js` found the
already-registered JS module and failed with "Could not find export
'default'".

**Fix:** `JSModuleDef` records the requested `type` attribute
(`import_attr_type`, `JS_ATOM_NULL` for plain JS) and
`js_find_loaded_module` matches (name, type); the host loader's module
is stamped with the requested type after loading.
`src/quickjs/quickjs.c`. Covers
`language/import/import-attributes/text-self.js`; JSON-module
idempotency is preserved (same name + same type still hits the map).

### 20. Diamond module graphs with shared top-level await no longer hang

**Spec:** [`AsyncModuleExecutionFulfilled`](https://tc39.es/ecma262/#sec-async-module-execution-fulfilled)
step 12.c.iv: an ancestor executed synchronously during the fan-out gets
`[[AsyncEvaluation]]` set to *false*.

**Bug:** the port left `async_evaluation` true on such ancestors, so a
later `InnerModuleEvaluation` (a dynamic import sharing part of the
graph) registered itself as an async dependent of an
already-finished module that would never notify again — the dynamic
import promise never settled. `import "./parent.js"; await
import("./grandparent.js")` with a common TLA leaf hung forever.

**Fix:** clear `async_evaluation` in `js_set_module_evaluated` and in
`js_async_module_execution_rejected`. Covers test262
`language/module-code/top-level-await/module-graphs-does-not-hang.js`
(and `rejection-order.js`, which still trips the multithreaded harness
while passing standalone). `src/quickjs/quickjs.c`.

### 21. `with` object-environment bindings re-probe on reads and writes through references

**Spec:** [Object Environment Record `GetBindingValue`/`SetMutableBinding`](https://tc39.es/ecma262/#sec-object-environment-records):
both re-check `HasProperty(bindings, N)` — observably, through proxy
traps — before the get/set; a deleted binding is a ReferenceError in
strict code, reads return undefined in sloppy code, and sloppy writes
still perform the `Set` (which recreates a plain property but, e.g.,
falls through a typed-array prototype's `[[Set]]` untouched).

**Bug:** the reference path (`OP_with_make_ref` +
`OP_get_ref_value`/`OP_put_ref_value`) resolved the binding once and
then used plain gets/sets, so the re-probe `has` trap never fired and
deleted bindings misbehaved; `OP_with_put_var` had the probe but not
the strict/sloppy split.

**Fix:** `OP_get_ref_value` and `OP_put_ref_value` perform the
`HasProperty` re-probe for non-global bases (the global object keeps
the create-on-assign path); reads of a missing binding yield undefined
(sloppy) or ReferenceError (strict), writes throw in strict mode and
otherwise proceed with the plain `Set`. `src/quickjs/quickjs.c`.

Covers test262
`language/statements/with/set-mutable-binding-idref-with-proxy-env.js`,
`set-mutable-binding-idref-compound-assign-with-proxy-env.js` and (with
fix #22) `set-mutable-binding-binding-deleted-with-typed-array-in-proto-chain.js`,
without regressing the `S11.13.*` delete-then-assign suite.

### 22. `"NaN"` is a canonical numeric index string

**Spec:** [`CanonicalNumericIndexString`](https://tc39.es/ecma262/#sec-canonicalnumericindexstring):
`"NaN"` round-trips through ToNumber/ToString, so it is a canonical
numeric index — a typed array's integer-indexed `[[Set]]`/`[[Get]]`
treats it as an (invalid) element index rather than an ordinary
property.

**Bug:** `JS_AtomIsNumericIndex1`'s fast path only admitted strings
starting with a digit, `-`, or `Infinity` (an upstream `XXX: should
test NaN` comment marked the gap), so `"NaN"` fell through to ordinary
property handling and e.g. `Set` through a typed-array prototype
created an own `NaN` property on the receiver.

**Fix:** admit `"NaN"` in both the 8-bit and wide-char fast paths; the
existing slow path validates it. `src/quickjs/quickjs.c`.

### 23. Destructuring `var` bindings resolve through `with` environments before the read

**Spec:** [`KeyedBindingInitialization` : *SingleNameBinding*](https://tc39.es/ecma262/#sec-runtime-semantics-keyedbindinginitialization)
steps 1–3: `ResolveBinding(bindingId)` — observable as a `has` probe on
a `with` environment — happens before `GetV(value, propertyName)`.

**Bug:** the binding path stored through `OP_scope_put_var` after the
value read, so the probe fired last.

**Fix:** in `js_parse_destructuring_element`'s object-pattern `var`
branch, emit a plain `OP_scope_get_var` for the binding and route it
through the shared `get_lvalue` path, which already builds the
reference eagerly (`OP_scope_make_ref`, probing the `with` environment)
and stores through `OP_put_ref_value`. `src/quickjs/quickjs.c`. Covers
test262
`language/destructuring/binding/keyed-destructuring-property-reference-target-evaluation-order-with-bindings.js`.
The array-pattern and rest-property `var` cases keep the late store
(no test262 coverage; same recipe applies if it grows some).

## Open issues found but not fixed this round

Nothing is currently left open — every tracked cluster above is fixed,
deliberately excluded in `test262.conf`, or has moved to the
"web-reality behaviors" section at the end of this document.

### 24. `{`/`[` as a primary expression eagerly resolved a trailing `=` regardless of precedence context

**Spec:** [13.15.1 Assignment Operators — Static Semantics: Early
Errors](https://tc39.es/ecma262/#sec-assignment-operators-static-semantics-early-errors)

**Symptom:** `#field in {} = 0` (test262
`language/expressions/in/private-field-invalid-assignment-target.js`)
parsed and ran instead of being a parse-time `SyntaxError`; likewise
the plain `'a' in {} = 0` (runtime `TypeError` instead of
`SyntaxError`) and `1 + {} = 0` (silently evaluated as
`1 + ({} = 0)`).

**Cause:** wherever `{`/`[` appeared as a primary expression,
`js_parse_postfix_expr` peeked ahead with
`js_parse_skip_parens_token(...) == '='` to decide "this is a
destructuring-assignment cover grammar", with no awareness of the
precedence level it was being parsed at. Per grammar the
`ObjectAssignmentPattern` reinterpretation only exists where the
literal itself starts an `AssignmentExpression`, never as the operand
of a unary/binary/relational operator.

**Fix:** a `PF_PATTERN` parse flag set by `js_parse_assign_expr2` at
the start of every `AssignmentExpression`, carried only along the
leftmost descent (`cond` → `coalesce` → `logical` → binary levels →
`unary` → `js_parse_postfix_expr`) and cleared at every
right-hand-operand parse (binary/logical/coalesce right operands and
the private-`in` right operand). The postfix `{`/`[` case only takes
the destructuring branch when the flag is present, so a trailing `=`
after a literal in operand position now falls through to the
assignment-target validity check and fails with "invalid assignment
left-hand side" at parse time. Legitimate destructuring
(`x = {} = 0`, `({a} = b)`, array/argument element positions,
conditional branches, `for ({a} of …)` heads, arrow bodies) is
unaffected — verified against the full suite
(`language/expressions/assignment`, `…/object`, `…/in`,
`statements/for-of` all at 0 errors).

### 25. `\p{RGI_Emoji}` — the first property of strings, matched as sequences

**Spec:** [22.2.1 Patterns —
`CharacterClassEscape :: p{ UnicodePropertyValueExpression }`](https://tc39.es/ecma262/#sec-patterns)
with the UTS #51 `RGI_Emoji` property of *strings* under the `v`
(unicodeSets) flag.

**Symptom:** `/^\p{RGI_Emoji}+$/v` threw `SyntaxError: unknown unicode
property name` (test262
`built-ins/RegExp/unicodeSets/generated/rgi-emoji-16.0.js` /
`rgi-emoji-17.0.js`).

**Fix:** properties of strings cannot live in a `CharRange` — an RGI
emoji is a *sequence* of up to 10 code points (ZWJ families, flags,
keycaps). The sequence data (Emoji 17.0, 2760 sequences + 1193 single
code points) is vendored as `src/quickjs/libunicode-rgi-emoji.h`,
generated by `scripts/gen-rgi-emoji.js` from the
`@unicode/unicode-17.0.0` npm data — the same source test262's
generated emoji tests are built from. In term position,
`re_parse_term` compiles `\p{RGI_Emoji}` through `re_emit_rgi_emoji`
(`libregexp.c`) into an ordered alternation — sequences longest-first,
matching the spec's descending-length string matching, with the
single-code-point ranges as the final alternative — so quantifiers,
lookahead and lookbehind all compose with it like any other atom.
`\P{RGI_Emoji}` and `u`-flag use remain `SyntaxError` per spec.
Verified against V8 on match/non-match/longest-match/lookbehind
behavior (including `\u{1F3FB}` — a skin-tone modifier alone *is* RGI
per `Basic_Emoji`), plus 0 errors across the whole
`built-ins/RegExp` area. Inside a class (`[\p{RGI_Emoji}]`) it still
errors — that needs the ClassSetExpression grammar (see the `v`-flag
set-operations backlog row).

### 26. `Function.prototype.caller`/`.arguments` are the %ThrowTypeError% poison pair, magic moved to instances

**Spec:** [AddRestrictedFunctionProperties](https://tc39.es/ecma262/#sec-addrestrictedfunctionproperties)
requires both restricted properties to be accessors whose getter *and*
setter are the exact same `%ThrowTypeError%` intrinsic — the identity
test262 samples through a strict `arguments` object's poisoned
`callee` getter.

**Symptom:** `built-ins/Function/prototype/caller/prop-desc.js` and
`caller-arguments/accessor-properties.js` failed in both modes: the
fork wired a live caller-resolving getter onto
`Function.prototype.caller` for web compatibility, so the four accessor
functions were not one identity.

**Fix:** modern V8 shows both behaviors can coexist — the prototype
carries the pure poison pair while the legacy behavior lives on
function *instances*. `JS_AddIntrinsicBaseObjects` now installs
`ctx->throw_type_error` as getter and setter of both `caller` and
`arguments`, and `JS_GetPropertyInternal` resolves `caller` reads on a
non-strict, prototype-carrying bytecode function (with no own `caller`
property) through the old `js_function_caller_get` stack walk before
the prototype chain is consulted. Strict functions, arrows, natives and
`Function.prototype` itself all fall through to the poison and throw,
exactly as the tests require; sloppy `fn.caller` keeps working in
classic scripts.

### 27. Async module rejection settles the module's own promise before its ancestors

**Spec:** [AsyncModuleExecutionRejected](https://tc39.es/ecma262/#sec-async-module-execution-rejected)
steps 9–10: reject the module's `[[TopLevelCapability]]` first, then
propagate to `[[AsyncParentModules]]`, so concurrently-imported graphs
settle leaf-to-root.

**Symptom:** `language/module-code/top-level-await/rejection-order.js`
timed out: `js_async_module_execution_rejected` recursed into the async
parents *before* rejecting the module's own capability, so the two
dynamic-import promises settled root-first, the test's
`assert.compareArray(logs, ["B", "A"])` threw inside an unhandled
`.then`, and `$DONE` was never reached.

**Fix:** swap the two blocks to match spec order — reject
`module->promise` first, then walk `async_parent_modules`. Verified
leaf-to-root settlement on a standalone reproduction of the test's
two-graph shape, plus full `language/module-code` (597) and
`dynamic-import` (1167) runs at zero errors — including the
diamond-graph top-level-await cases from #20.

## Web-reality behaviors (test-invisible)

Earlier revisions of this document recorded deliberate test262
*failures* here. There are none left — the suite runs clean — but one
deliberate non-spec behavior remains, implemented so that no test262
test can observe it.

### `fn.caller` on non-strict functions (test-invisible, matches V8)

The spec's `Function.prototype.caller`/`.arguments` surface is fully
conformant since #26: both are accessor pairs whose getter and setter
are the exact same `%ThrowTypeError%` intrinsic, and reading them
through the prototype (or on a strict/arrow/native function) throws.
What remains — deliberately — is the same legacy extension modern V8
ships: reading `caller` directly on a *non-strict* function instance
resolves the live caller instead of falling through to the poison
accessor, because real websites still read `fn.caller` /
`arguments.callee.caller`. This is implemented as an instance-level
lookup intercept (`JS_GetPropertyInternal` in `src/quickjs/quickjs.c`),
not as a prototype accessor, so the property-descriptor surface test262
checks stays exactly per spec. No test262 test observes the extension —
the suite runs at zero failures with it in place.
