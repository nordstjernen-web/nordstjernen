// Build-time smoke test: load polyfills.js (passed via -I) and assert
// every public API it claims to define actually loaded.
//
// Run as: qjs -I data/js/polyfills.js scripts/verify-polyfills.js
//
// Exits 1 (with details on stderr) on the first missing API, 0 when
// every check passes.

var fails = [];
function need(name, expr, why) {
    var ok = false;
    try { ok = expr(); } catch (e) { fails.push(name + ': threw ' + e); return; }
    if (!ok) fails.push(name + (why ? ' (' + why + ')' : ''));
}

need('URLSearchParams ctor',  function () { return typeof URLSearchParams === 'function'; });
need('URLSearchParams roundtrip', function () {
    var u = new URLSearchParams('a=1&b=two&a=3');
    return u.get('b') === 'two' && u.getAll('a').length === 2 &&
           u.toString() === 'a=1&b=two&a=3';
});
need('URLSearchParams.append/delete', function () {
    var u = new URLSearchParams();
    u.append('k', 'v1'); u.append('k', 'v2');
    if (u.toString() !== 'k=v1&k=v2') return false;
    u.delete('k');
    return u.toString() === '';
});

need('Headers ctor', function () { return typeof Headers === 'function'; });
need('Headers normalize', function () {
    var h = new Headers({'X-A': '1'});
    h.append('x-a', '2');
    return h.get('X-A') === '1, 2' && h.has('x-A');
});

need('Blob ctor', function () { return typeof Blob === 'function'; });
need('Blob size/type', function () {
    var b = new Blob(['hello'], {type: 'text/plain'});
    return b.size === 5 && b.type === 'text/plain';
});
need('Blob utf-8 length (multi-byte)', function () {
    var b = new Blob(['é']);
    return b.size === 2;
});
need('Blob utf-8 length (4-byte)', function () {
    var b = new Blob(['😀']);
    return b.size === 4;
});
need('Blob bytes are utf-8 encoded', function () {
    var b = new Blob(['héllo']);
    // 'h' 0x68, 'é' 0xc3 0xa9, 'l' 0x6c, 'l' 0x6c, 'o' 0x6f
    var bytes = b._b;
    return bytes.length === 6 &&
           bytes[0] === 0x68 && bytes[1] === 0xc3 && bytes[2] === 0xa9 &&
           bytes[3] === 0x6c && bytes[4] === 0x6c && bytes[5] === 0x6f;
});

need('File ctor', function () { return typeof File === 'function'; });
need('File.name', function () {
    var f = new File(['x'], 'a.txt');
    return f.name === 'a.txt';
});

need('queueMicrotask is callable', function () {
    return typeof queueMicrotask === 'function';
});

need('XMLSerializer ctor', function () {
    return typeof XMLSerializer === 'function';
});

need('AbortSignal ctor', function () { return typeof AbortSignal === 'function'; });
need('AbortController ctor', function () { return typeof AbortController === 'function'; });

need('NodeFilter constants', function () {
    return typeof NodeFilter === 'object' &&
           typeof NodeFilter.SHOW_ELEMENT === 'number' &&
           typeof NodeFilter.FILTER_ACCEPT === 'number';
});

need('ReadableStream ctor',   function () { return typeof ReadableStream === 'function'; });
need('WritableStream ctor',   function () { return typeof WritableStream === 'function'; });
need('TransformStream ctor',  function () { return typeof TransformStream === 'function'; });
need('TextEncoderStream ctor', function () { return typeof TextEncoderStream === 'function'; });
need('TextDecoderStream ctor', function () { return typeof TextDecoderStream === 'function'; });

need('Intl is object', function () { return typeof Intl === 'object' && Intl !== null; });
need('Intl.DateTimeFormat', function () { return typeof Intl.DateTimeFormat === 'function'; });
need('Intl.NumberFormat',  function () { return typeof Intl.NumberFormat  === 'function'; });

need('CompressionStream ctor',   function () { return typeof CompressionStream === 'function'; });
need('DecompressionStream ctor', function () { return typeof DecompressionStream === 'function'; });

need('Object.hasOwn', function () {
    return typeof Object.hasOwn === 'function' &&
           Object.hasOwn({a: 1}, 'a') &&
           !Object.hasOwn({a: 1}, 'b');
});

need('Object.fromEntries', function () {
    if (typeof Object.fromEntries !== 'function') return false;
    var o = Object.fromEntries([['a', 1], ['b', 'two']]);
    if (o.a !== 1 || o.b !== 'two') return false;
    var m = new Map([['x', 9]]);
    var o2 = Object.fromEntries(m);
    return o2.x === 9;
});

need('Promise.withResolvers', function () {
    var r = Promise.withResolvers();
    return r && r.promise instanceof Promise &&
           typeof r.resolve === 'function' &&
           typeof r.reject  === 'function';
});

need('Promise.any exists', function () {
    return typeof Promise.any === 'function';
});

need('Promise.allSettled exists', function () {
    return typeof Promise.allSettled === 'function';
});

need('Array.prototype.findLast', function () {
    return [1, 2, 3, 4].findLast(function (x) { return x < 3; }) === 2;
});
need('Array.prototype.findLastIndex', function () {
    return [1, 2, 3, 4].findLastIndex(function (x) { return x < 3; }) === 1;
});
need('Array.prototype.toSorted', function () {
    var a = [3, 1, 2];
    var s = a.toSorted();
    return s[0] === 1 && s[2] === 3 && a[0] === 3;
});
need('Array.prototype.toReversed', function () {
    var a = [1, 2, 3];
    var r = a.toReversed();
    return r[0] === 3 && r[2] === 1 && a[0] === 1;
});
need('Array.prototype.toSpliced', function () {
    var a = [1, 2, 3, 4];
    var s = a.toSpliced(1, 2, 9);
    return s.length === 3 && s[0] === 1 && s[1] === 9 && s[2] === 4 &&
           a.length === 4;
});
need('Array.prototype.with', function () {
    var a = [1, 2, 3];
    var w = a.with(1, 99);
    return w[1] === 99 && a[1] === 2;
});

need('Object.groupBy', function () {
    var g = Object.groupBy([1, 2, 3, 4, 5], function (n) { return n % 2 ? 'odd' : 'even'; });
    return g.odd.length === 3 && g.even.length === 2;
});
need('Map.groupBy', function () {
    var g = Map.groupBy([1, 2, 3], function (n) { return n > 1; });
    return g.get(false).length === 1 && g.get(true).length === 2;
});

need('String.prototype.replaceAll (string)', function () {
    return 'aaa'.replaceAll('a', 'b') === 'bbb';
});
need('String.prototype.replaceAll (function replacement)', function () {
    return 'a-b-c'.replaceAll('-', function () { return '|'; }) === 'a|b|c';
});
need('String.prototype.replaceAll (non-global RegExp throws)', function () {
    try { 'abc'.replaceAll(/a/, 'x'); return false; }
    catch (e) { return e instanceof TypeError; }
});

need('structuredClone primitive', function () {
    return structuredClone(42) === 42 &&
           structuredClone('hi') === 'hi';
});
need('structuredClone deep', function () {
    var src = {a: [1, 2, {b: 'x'}], d: new Date(123), m: new Map([['k', 'v']])};
    var c = structuredClone(src);
    return c.a[2].b === 'x' && c.a !== src.a && c.m !== src.m &&
           c.m.get('k') === 'v' && c.d.getTime() === 123;
});
need('structuredClone handles cycles', function () {
    var a = {}; a.self = a;
    var c = structuredClone(a);
    return c !== a && c.self === c;
});

need('Symbol.dispose exists', function () {
    return typeof Symbol.dispose === 'symbol' || typeof Symbol.dispose === 'string';
});

need('DOMException ctor', function () {
    var e = new DOMException('boom', 'AbortError');
    return e.name === 'AbortError' && e.message === 'boom' && e.code === 20;
});
need('DOMException default name', function () {
    var e = new DOMException('x');
    return e.name === 'Error' && e.code === 0;
});
need('DOMException is throwable / catchable as Error', function () {
    try { throw new DOMException('nope', 'NotFoundError'); }
    catch (e) { return e instanceof Error && e.name === 'NotFoundError' && e.code === 8; }
});

need('setImmediate / clearImmediate exist', function () {
    return typeof setImmediate === 'function' &&
           typeof clearImmediate === 'function';
});

need('scheduler.postTask returns a Promise', function () {
    var p = scheduler.postTask(function () { return 1; });
    return p instanceof Promise;
});
need('scheduler.yield returns a Promise', function () {
    return scheduler.yield() instanceof Promise;
});

need('XMLSerializer text node', function () {
    var s = new XMLSerializer();
    return s.serializeToString({ nodeType: 3, nodeValue: 'hi & bye' }) === 'hi & bye';
});
need('XMLSerializer comment node', function () {
    var s = new XMLSerializer();
    return s.serializeToString({ nodeType: 8, nodeValue: 'note' }) === '<!--note-->';
});
need('XMLSerializer document fragment', function () {
    var s = new XMLSerializer();
    var t = { nodeType: 3, nodeValue: 'X', nextSibling: null };
    var frag = { nodeType: 11, firstChild: t };
    return s.serializeToString(frag) === 'X';
});

if (fails.length) {
    console.log('polyfills.js verification FAILED:');
    for (var i = 0; i < fails.length; i++) console.log('  - ' + fails[i]);
    throw new Error(fails.length + ' polyfill check(s) failed');
}
console.log('polyfills.js: ' + 'all checks passed');
