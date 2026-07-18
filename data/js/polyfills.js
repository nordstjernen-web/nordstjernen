(function (global) {
    'use strict';

    var nativeize = (function () {
        var origToString = Function.prototype.toString;
        var faked = new WeakMap();
        function nativeSrc(name) {
            return 'function ' + name + '() { [native code] }';
        }
        var patched = function toString() {
            if (faked.has(this)) return faked.get(this);
            return origToString.call(this);
        };
        try {
            Object.defineProperty(patched, 'name',
                { value: 'toString', configurable: true });
        } catch (e) {}
        faked.set(patched, nativeSrc('toString'));
        try { Function.prototype.toString = patched; } catch (e) {}
        return function (fn, name) {
            if (typeof fn !== 'function') return fn;
            var nm = name || fn.name || '';
            try {
                Object.defineProperty(fn, 'name',
                    { value: nm, configurable: true });
            } catch (e) {}
            faked.set(fn, nativeSrc(nm));
            return fn;
        };
    })();
    global.__ndNativeize = nativeize;

    function patchFaceplatePartial(name, ctor) {
        if (name !== 'faceplate-partial' || !ctor || !ctor.prototype) return;
        var proto = ctor.prototype;
        if (proto.__ndFaceplatePartialPatched ||
            typeof proto._loadContent !== 'function') return;
        var render = proto._renderContent;
        if (typeof render === 'function') {
            proto._renderContent = function (text) {
                if (this.__ndRenderedPartial === text) return undefined;
                this.__ndRenderedPartial = text;
                return render.call(this, text);
            };
        }
        try {
            Object.defineProperty(proto, '__ndFaceplatePartialPatched', {
                value: true, configurable: true
            });
        } catch (e) { proto.__ndFaceplatePartialPatched = true; }
        proto._loadContent = function () {
            var el = this;
            if (!el.src) return Promise.reject(new Error('No src attribute specified on faceplate-partial element.'));
            if (el.__ndPartialLoading)
                return Promise.reject(new Error('Request already in progress on faceplate-partial element.'));
            var method = String(el.method || 'GET').toUpperCase();
            var body = null;
            if (method === 'POST') {
                var form = new FormData();
                var inputs = el.querySelectorAll ? el.querySelectorAll('input[type=hidden]') : [];
                for (var i = 0; i < inputs.length; i++) {
                    var input = inputs[i];
                    if (!input.disabled && input.name) form.append(input.name, input.value);
                }
                body = new URLSearchParams(form).toString();
            }
            try {
                if (el._slotCapture) {
                    if (typeof el._shouldShowLoadingSlot === 'function' &&
                        el._shouldShowLoadingSlot()) {
                        var loading = el.querySelector && el.querySelector('[slot=loading]');
                        if (loading) loading.remove();
                    } else {
                        el.innerHTML = '';
                    }
                    el.appendChild(el._slotCapture);
                }
            } catch (e) {}
            el.__ndPartialLoading = true;
            if (el.partialRequest) el.partialRequest.isRequestInProgress = true;
            if (el.loading === 'action' && typeof el.requestUpdate === 'function')
                el.requestUpdate();
            return new Promise(function (resolve, reject) {
                var xhr = new XMLHttpRequest();
                el.__ndPartialXHR = xhr;
                var partialUrl = new URL(el.src, global.location && global.location.origin || undefined).href;
                xhr.open(method, partialUrl, true);
                try { xhr.withCredentials = true; } catch (e) {}
                try { xhr.setRequestHeader('Accept', 'text/vnd.reddit.partial+html, text/html;q=0.9'); } catch (e) {}
                if (method !== 'GET')
                    try { xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded'); } catch (e) {}
                xhr.onload = function () {
                    el.__ndPartialLoading = false;
                    el.__ndPartialXHR = null;
                    if (el.partialRequest) el.partialRequest.isRequestInProgress = false;
                    if (el.loading === 'action' && typeof el.requestUpdate === 'function')
                        el.requestUpdate();
                    var text = xhr.responseText || '';
                    try {
                        if (xhr.status >= 200 && xhr.status < 300 &&
                            typeof el._renderContent === 'function')
                            el._renderContent(text);
                        resolve(text);
                    } catch (err) {
                        reject(err);
                    }
                };
                xhr.onerror = function () {
                    el.__ndPartialLoading = false;
                    el.__ndPartialXHR = null;
                    if (el.partialRequest) el.partialRequest.isRequestInProgress = false;
                    reject(new Error('faceplate-partial request failed'));
                };
                xhr.send(method === 'GET' ? null : (body || ''));
            });
        };
    }

    try {
        var ndCE = global.customElements;
        if (ndCE && !ndCE.__ndFaceplatePatchInstalled &&
            typeof ndCE.define === 'function') {
            var ndDefine = ndCE.define.bind(ndCE);
            ndCE.define = function (name, ctor, opts) {
                patchFaceplatePartial(name, ctor);
                return ndDefine(name, ctor, opts);
            };
            try {
                Object.defineProperty(ndCE, '__ndFaceplatePatchInstalled', {
                    value: true, configurable: true
                });
            } catch (e) { ndCE.__ndFaceplatePatchInstalled = true; }
            if (typeof ndCE.get === 'function')
                patchFaceplatePartial('faceplate-partial', ndCE.get('faceplate-partial'));
        }
    } catch (e) {}

    if (global.__ND_FP_DEBUG) {
        try {
            var __log = function (m) { console.log('[FP] ' + m); };
            var __desc = function (el) {
                if (!el || !el.getAttribute) return String(el);
                return el.tagName + '[' + (el.getAttribute('loading') || '') + ' src=' +
                    (el.getAttribute('src') || '') + ' fn=' + (el.getAttribute('feature-name') || '') + ']';
            };
            var __patch = function (proto, names, label) {
                names.forEach(function (m) {
                    if (proto && typeof proto[m] === 'function') {
                        var orig = proto[m];
                        proto[m] = function () {
                            __log(label + '.' + m + ' ' + __desc(this));
                            return orig.apply(this, arguments);
                        };
                    }
                });
            };
            var __ce = global.customElements;
            var __origDefine = __ce.define.bind(__ce);
            __ce.define = function (name, ctor, opts) {
                __log('define ' + name);
                if (ctor && ctor.prototype) {
                    if (name === 'auth-flow-manager') {
                        var amp = ctor.prototype;
                        if (typeof amp.show === 'function') {
                            var origShow = amp.show;
                            amp.show = function (e) {
                                var found = null;
                                try { found = this.querySelector('[slot="' + e + '"]'); } catch (x) {}
                                __log('auth-flow-manager.show(' + e + ') step=' + this.getAttribute('step-name') +
                                    ' found=' + (found ? __desc(found) : 'NULL'));
                                return origShow.apply(this, arguments);
                            };
                        }
                    }
                    if (name === 'faceplate-partial')
                        __patch(ctor.prototype, ['load', '_load', '_loadContent', 'connectedCallback'], 'partial');
                    if (name === 'faceplate-loader')
                        __patch(ctor.prototype, ['load', '_load', 'connectedCallback'], 'loader');
                }
                return __origDefine(name, ctor, opts);
            };
            var __origWhen = __ce.whenDefined.bind(__ce);
            __ce.whenDefined = function (name) {
                var pr = __origWhen(name);
                if (/faceplate|auth/.test(name)) {
                    __log('whenDefined(' + name + ')');
                    pr.then(function () { __log('whenDefined(' + name + ') RESOLVED'); });
                }
                return pr;
            };
        } catch (e) { console.log('[FP] instrument err ' + e); }
    }

    if (typeof global.PerformanceObserver === 'function' &&
        !Array.isArray(global.PerformanceObserver.supportedEntryTypes)) {
        try {
            global.PerformanceObserver.supportedEntryTypes = [
                'mark', 'measure', 'navigation', 'resource', 'paint'
            ];
        } catch (e) {}
    }



    function defineCtor(name, ctor) {
        if (typeof global[name] === 'function' || typeof global[name] === 'object'
            && global[name] !== null) return;
        try {
            Object.defineProperty(global, name, {
                value: ctor, writable: true, configurable: true, enumerable: false
            });
        } catch (e) { global[name] = ctor; }
    }

    function replaceCtor(name, ctor) {
        try {
            Object.defineProperty(global, name, {
                value: ctor, writable: true, configurable: true, enumerable: false
            });
        } catch (e) { global[name] = ctor; }
    }

    function defineMethod(proto, name, fn) {
        if (typeof proto[name] === 'function') return;
        try {
            Object.defineProperty(proto, name, {
                value: fn, writable: true, configurable: true, enumerable: false
            });
        } catch (e) { proto[name] = fn; }
    }

    function encodeKV(s) {
        return encodeURIComponent(String(s == null ? '' : s)).replace(/%20/g, '+');
    }
    function decodeKV(s) {
        return decodeURIComponent(String(s == null ? '' : s).replace(/\+/g, ' '));
    }

    function USP(init) {
        if (!(this instanceof USP)) return new USP(init);
        this._p = [];
        if (init == null) return;
        if (init instanceof USP) {
            for (var i = 0; i < init._p.length; i++)
                this._p.push([init._p[i][0], init._p[i][1]]);
            return;
        }
        if (typeof init === 'string') {
            var s = init.charAt(0) === '?' ? init.slice(1) : init;
            if (!s) return;
            var parts = s.split('&');
            for (var j = 0; j < parts.length; j++) {
                if (!parts[j]) continue;
                var eq = parts[j].indexOf('=');
                if (eq < 0) this._p.push([decodeKV(parts[j]), '']);
                else this._p.push([decodeKV(parts[j].slice(0, eq)),
                                   decodeKV(parts[j].slice(eq + 1))]);
            }
            return;
        }
        if (typeof init === 'object') {
            if (typeof init.length === 'number' &&
                typeof init !== 'function') {
                for (var k = 0; k < init.length; k++) {
                    var pair = init[k];
                    if (pair && typeof pair.length === 'number' && pair.length >= 2)
                        this._p.push([String(pair[0]), String(pair[1])]);
                }
                return;
            }
            var keys = Object.keys(init);
            for (var n = 0; n < keys.length; n++)
                this._p.push([keys[n], String(init[keys[n]])]);
        }
    }
    USP.prototype.append = function (k, v) { this._p.push([String(k), String(v)]); };
    USP.prototype.delete = function (k) {
        k = String(k);
        this._p = this._p.filter(function (p) { return p[0] !== k; });
    };
    USP.prototype.get = function (k) {
        k = String(k);
        for (var i = 0; i < this._p.length; i++)
            if (this._p[i][0] === k) return this._p[i][1];
        return null;
    };
    USP.prototype.getAll = function (k) {
        k = String(k);
        var out = [];
        for (var i = 0; i < this._p.length; i++)
            if (this._p[i][0] === k) out.push(this._p[i][1]);
        return out;
    };
    USP.prototype.has = function (k) { return this.get(k) !== null; };
    USP.prototype.set = function (k, v) {
        k = String(k); v = String(v);
        var found = false, out = [];
        for (var i = 0; i < this._p.length; i++) {
            if (this._p[i][0] === k) {
                if (!found) { out.push([k, v]); found = true; }
            } else out.push(this._p[i]);
        }
        if (!found) out.push([k, v]);
        this._p = out;
    };
    USP.prototype.sort = function () {
        this._p.sort(function (a, b) {
            return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0;
        });
    };
    USP.prototype.toString = function () {
        var parts = [];
        for (var i = 0; i < this._p.length; i++)
            parts.push(encodeKV(this._p[i][0]) + '=' + encodeKV(this._p[i][1]));
        return parts.join('&');
    };
    USP.prototype.forEach = function (fn, thisArg) {
        for (var i = 0; i < this._p.length; i++)
            fn.call(thisArg, this._p[i][1], this._p[i][0], this);
    };
    USP.prototype.keys = function () {
        var arr = this._p.map(function (p) { return p[0]; });
        return arr[Symbol.iterator]();
    };
    USP.prototype.values = function () {
        var arr = this._p.map(function (p) { return p[1]; });
        return arr[Symbol.iterator]();
    };
    USP.prototype.entries = function () {
        var arr = this._p.map(function (p) { return [p[0], p[1]]; });
        return arr[Symbol.iterator]();
    };
    if (typeof Symbol !== 'undefined' && Symbol.iterator) {
        USP.prototype[Symbol.iterator] = USP.prototype.entries;
    }
    Object.defineProperty(USP.prototype, 'size', {
        get: function () { return this._p.length; },
        configurable: true
    });
    defineCtor('URLSearchParams', USP);

    function normHeader(k) { return String(k).toLowerCase(); }
    var HTTP_WS = /^[\t\n\r ]+|[\t\n\r ]+$/g;
    var HDR_TOKEN = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
    var HDR_BADVAL = /[\0\n\r]/;
    function checkHeaderName(k) {
        var s = String(k);
        if (!HDR_TOKEN.test(s))
            throw new TypeError("Invalid header name: '" + s + "'");
        return s.toLowerCase();
    }
    function checkHeaderValue(v) {
        var s = String(v).replace(HTTP_WS, '');
        if (HDR_BADVAL.test(s))
            throw new TypeError("Invalid header value");
        return s;
    }
    function hdrByteString(x) {
        var s = String(x);
        for (var i = 0; i < s.length; i++)
            if (s.charCodeAt(i) > 0xFF)
                throw new TypeError("Header contains a character outside the ByteString range");
        return s;
    }
    var HDR_ITER_PROTO = Object.create(
        Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]())));
    Object.defineProperty(HDR_ITER_PROTO, 'next', {
        configurable: true, enumerable: true, writable: true,
        value: function () {
            var s = this._h, keys = Object.keys(s._m).sort();
            if (this._i >= keys.length) return { value: undefined, done: true };
            var k = keys[this._i++], v = s._m[k];
            var out = this._k === 0 ? k : this._k === 1 ? v : [k, v];
            return { value: out, done: false };
        }
    });
    function headersIterator(h, kind) {
        var it = Object.create(HDR_ITER_PROTO);
        it._h = h; it._i = 0; it._k = kind;
        return it;
    }

    function Headers(init) {
        if (!(this instanceof Headers))
            throw new TypeError("Constructor Headers requires 'new'");
        this._m = Object.create(null);
        if (init === undefined) return;
        if (init === null || (typeof init !== 'object' && typeof init !== 'function'))
            throw new TypeError("Failed to construct 'Headers': invalid init");
        var self = this;
        if (typeof init[Symbol.iterator] !== 'undefined') {
            if (typeof init[Symbol.iterator] !== 'function')
                throw new TypeError("Headers init is not iterable");
            var it = init[Symbol.iterator](), step;
            while (!(step = it.next()).done) {
                var pair = step.value, arr = [];
                if (pair == null || typeof pair[Symbol.iterator] !== 'function')
                    throw new TypeError("Header pair is not iterable");
                var pit = pair[Symbol.iterator](), ps;
                while (!(ps = pit.next()).done) arr.push(ps.value);
                if (arr.length !== 2)
                    throw new TypeError("Header pair must contain exactly two items");
                self.append(arr[0], arr[1]);
            }
            return;
        }
        var keys = Reflect.ownKeys(init);
        var rec = [];
        for (var j = 0; j < keys.length; j++) {
            var key = keys[j];
            var d = Reflect.getOwnPropertyDescriptor(init, key);
            if (d === undefined || !d.enumerable) continue;
            if (typeof key === 'symbol')
                throw new TypeError("Header name cannot be a Symbol");
            var nm = hdrByteString(key);
            rec.push([nm, hdrByteString(init[key])]);
        }
        for (var r = 0; r < rec.length; r++) self.append(rec[r][0], rec[r][1]);
    }
    Headers.prototype.append = function (k, v) {
        var key = checkHeaderName(k);
        var val = checkHeaderValue(v);
        if (this._m[key] != null) this._m[key] += ', ' + val;
        else this._m[key] = val;
    };
    Headers.prototype.set = function (k, v) {
        this._m[checkHeaderName(k)] = checkHeaderValue(v);
    };
    Headers.prototype.get = function (k) {
        var v = this._m[checkHeaderName(k)];
        return v == null ? null : v;
    };
    Headers.prototype.has = function (k) { return this._m[checkHeaderName(k)] != null; };
    Headers.prototype.delete = function (k) { delete this._m[checkHeaderName(k)]; };
    Headers.prototype.forEach = function (fn, thisArg) {
        var keys = Object.keys(this._m).sort();
        for (var i = 0; i < keys.length; i++)
            fn.call(thisArg, this._m[keys[i]], keys[i], this);
    };
    Headers.prototype.keys = function () { return headersIterator(this, 0); };
    Headers.prototype.values = function () { return headersIterator(this, 1); };
    Headers.prototype.entries = function () { return headersIterator(this, 2); };
    if (typeof Symbol !== 'undefined' && Symbol.iterator) {
        Headers.prototype[Symbol.iterator] = Headers.prototype.entries;
    }
    nativeize(Headers, 'Headers');
    try { Object.defineProperty(Headers, 'length', { value: 0 }); } catch (e) {}
    defineCtor('Headers', Headers);

    function utf8Encode(s) {
        var str = String(s);
        var out = [];
        for (var i = 0; i < str.length; i++) {
            var c = str.charCodeAt(i);
            if (c < 0x80) {
                out.push(c);
            } else if (c < 0x800) {
                out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
            } else if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length) {
                var c2 = str.charCodeAt(i + 1);
                if (c2 >= 0xdc00 && c2 <= 0xdfff) {
                    var cp = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00);
                    out.push(0xf0 | (cp >> 18),
                             0x80 | ((cp >> 12) & 0x3f),
                             0x80 | ((cp >> 6)  & 0x3f),
                             0x80 | (cp & 0x3f));
                    i++;
                    continue;
                }
                out.push(0xef, 0xbf, 0xbd);
            } else {
                out.push(0xe0 | (c >> 12),
                         0x80 | ((c >> 6) & 0x3f),
                         0x80 | (c & 0x3f));
            }
        }
        return new Uint8Array(out);
    }

    function blobPartBytes(part) {
        if (part == null) return new Uint8Array(0);
        if (part instanceof Uint8Array) return part;
        if (part instanceof ArrayBuffer) return new Uint8Array(part);
        if (ArrayBuffer.isView && ArrayBuffer.isView(part))
            return new Uint8Array(part.buffer, part.byteOffset, part.byteLength);
        if (part instanceof Blob) return part._b || new Uint8Array(0);
        if (typeof TextEncoder === 'function')
            return new TextEncoder().encode(String(part));
        return utf8Encode(part);
    }

    function Blob(parts, options) {
        if (!(this instanceof Blob)) return new Blob(parts, options);
        var chunks = [];
        var total = 0;
        if (parts && typeof parts.length === 'number') {
            for (var i = 0; i < parts.length; i++) {
                var b = blobPartBytes(parts[i]);
                chunks.push(b); total += b.length;
            }
        }
        var buf = new Uint8Array(total);
        var off = 0;
        for (var k = 0; k < chunks.length; k++) {
            buf.set(chunks[k], off); off += chunks[k].length;
        }
        this._b = buf;
        Object.defineProperty(this, 'size', { value: total, configurable: true });
        Object.defineProperty(this, 'type', {
            value: options && options.type ? String(options.type).toLowerCase() : '',
            configurable: true
        });
    }
    Blob.prototype.slice = function (start, end, type) {
        var s = start == null ? 0 : start | 0;
        var e = end == null ? this.size : end | 0;
        if (s < 0) s = Math.max(0, this.size + s);
        if (e < 0) e = Math.max(0, this.size + e);
        s = Math.min(s, this.size); e = Math.min(e, this.size);
        if (e < s) e = s;
        var slice = this._b.slice(s, e);
        var out = new Blob([], {type: type || this.type});
        out._b = slice;
        Object.defineProperty(out, 'size', { value: slice.length, configurable: true });
        return out;
    };
    function utf8Decode(bytes) {
        var s = '';
        for (var i = 0; i < bytes.length;) {
            var b1 = bytes[i++];
            if (b1 < 0x80) {
                s += String.fromCharCode(b1);
            } else if (b1 < 0xc0) {
                s += '�';
            } else if (b1 < 0xe0) {
                var b2 = bytes[i++] & 0x3f;
                s += String.fromCharCode(((b1 & 0x1f) << 6) | b2);
            } else if (b1 < 0xf0) {
                var c2 = bytes[i++] & 0x3f;
                var c3 = bytes[i++] & 0x3f;
                s += String.fromCharCode(((b1 & 0x0f) << 12) | (c2 << 6) | c3);
            } else {
                var d2 = bytes[i++] & 0x3f;
                var d3 = bytes[i++] & 0x3f;
                var d4 = bytes[i++] & 0x3f;
                var cp = ((b1 & 0x07) << 18) | (d2 << 12) | (d3 << 6) | d4;
                cp -= 0x10000;
                s += String.fromCharCode(0xd800 | (cp >> 10),
                                         0xdc00 | (cp & 0x3ff));
            }
        }
        return s;
    }
    Blob.prototype.text = function () {
        var b = this._b;
        var text = (typeof TextDecoder === 'function')
            ? new TextDecoder().decode(b) : utf8Decode(b);
        return Promise.resolve(text);
    };
    Blob.prototype.arrayBuffer = function () {
        var b = this._b;
        var buf = new ArrayBuffer(b.length);
        new Uint8Array(buf).set(b);
        return Promise.resolve(buf);
    };
    Blob.prototype.stream = function () {
        var bytes = this._b;
        if (typeof ReadableStream === 'function') {
            return new ReadableStream({
                start: function (controller) {
                    if (bytes && bytes.length)
                        controller.enqueue(new Uint8Array(bytes));
                    controller.close();
                }
            });
        }
        var done = false;
        return {
            getReader: function () {
                return {
                    read: function () {
                        if (done)
                            return Promise.resolve({ done: true, value: undefined });
                        done = true;
                        return Promise.resolve({ done: false, value: new Uint8Array(bytes) });
                    },
                    releaseLock: function () {},
                    cancel: function () { done = true; return Promise.resolve(); }
                };
            }
        };
    };
    defineCtor('Blob', Blob);

    function File(parts, name, options) {
        if (!(this instanceof File)) return new File(parts, name, options);
        Blob.call(this, parts, options);
        Object.defineProperty(this, 'name', { value: String(name), configurable: true });
        Object.defineProperty(this, 'lastModified', {
            value: options && options.lastModified ? +options.lastModified : Date.now(),
            configurable: true
        });
    }
    File.prototype = Object.create(Blob.prototype);
    File.prototype.constructor = File;
    defineCtor('File', File);

    if (typeof global.queueMicrotask !== 'function') {
        defineCtor('queueMicrotask', function (cb) {
            Promise.resolve().then(cb);
        });
    }

    function ndMediaTask(fn) {
        if (typeof queueMicrotask === 'function') queueMicrotask(fn);
        else setTimeout(fn, 0);
    }

    function ndMediaEvent(type, target) {
        var ev;
        try { ev = new Event(type); } catch (e) { ev = { type: String(type) }; }
        try {
            Object.defineProperty(ev, 'target', { configurable: true, value: target });
            Object.defineProperty(ev, 'currentTarget', { configurable: true, value: target });
        } catch (e) {
            ev.target = target;
            ev.currentTarget = target;
        }
        return ev;
    }

    function ndEventMethods(proto) {
        proto.addEventListener = function (type, cb) {
            if (!cb) return;
            type = String(type);
            if (!this._listeners) this._listeners = {};
            if (!this._listeners[type]) this._listeners[type] = [];
            if (this._listeners[type].indexOf(cb) < 0)
                this._listeners[type].push(cb);
        };
        proto.removeEventListener = function (type, cb) {
            type = String(type);
            var list = this._listeners && this._listeners[type];
            if (!list) return;
            var i = list.indexOf(cb);
            if (i >= 0) list.splice(i, 1);
        };
        proto.dispatchEvent = function (ev) {
            if (!ev || !ev.type) return true;
            return ndFireEvent(this, ev.type, ev);
        };
    }

    function ndFireEvent(target, type, ev) {
        ev = ev || ndMediaEvent(type, target);
        var handler = target && target['on' + type];
        if (typeof handler === 'function') {
            try { handler.call(target, ev); } catch (e) {}
        }
        var list = target && target._listeners && target._listeners[type];
        if (list) {
            list = list.slice();
            for (var i = 0; i < list.length; i++) {
                try {
                    if (typeof list[i] === 'function') list[i].call(target, ev);
                    else if (list[i] && typeof list[i].handleEvent === 'function')
                        list[i].handleEvent(ev);
                } catch (e) {}
            }
        }
        return true;
    }

    function ndTimeRanges(start, end) {
        this.length = end > start ? 1 : 0;
        this._start = start || 0;
        this._end = end || 0;
    }
    ndTimeRanges.prototype.start = function (index) {
        if (index !== 0 || this.length === 0) throw new Error('IndexSizeError');
        return this._start;
    };
    ndTimeRanges.prototype.end = function (index) {
        if (index !== 0 || this.length === 0) throw new Error('IndexSizeError');
        return this._end;
    };
    if (typeof global.TimeRanges === 'function' && global.TimeRanges.prototype) {
        try {
            Object.setPrototypeOf(ndTimeRanges.prototype,
                                  global.TimeRanges.prototype);
        } catch (e) {}
    }

    function SourceBufferList() {
        if (!(this instanceof SourceBufferList)) return new SourceBufferList();
        this._items = [];
        this.length = 0;
        this.onaddsourcebuffer = null;
        this.onremovesourcebuffer = null;
    }
    ndEventMethods(SourceBufferList.prototype);
    SourceBufferList.prototype.item = function (index) {
        return this._items[index >>> 0] || null;
    };
    SourceBufferList.prototype._sync = function () {
        for (var i = 0; i < this.length; i++) {
            try { delete this[i]; } catch (e) {}
        }
        this.length = this._items.length;
        for (var j = 0; j < this._items.length; j++) this[j] = this._items[j];
    };
    SourceBufferList.prototype._push = function (buffer) {
        this._items.push(buffer);
        this._sync();
        ndFireEvent(this, 'addsourcebuffer');
    };
    SourceBufferList.prototype._remove = function (buffer) {
        var i = this._items.indexOf(buffer);
        if (i < 0) return false;
        this._items.splice(i, 1);
        this._sync();
        ndFireEvent(this, 'removesourcebuffer');
        return true;
    };

    function ndSupportedMediaType(type) {
        var raw = String(type || '').toLowerCase();
        var mime = raw.split(';')[0].trim();
        if (!mime) return false;
        var probe = global.document && global.document.createElement &&
            global.document.createElement(mime.indexOf('audio/') === 0 ? 'audio' : 'video');
        if (probe && typeof probe.canPlayType === 'function' &&
            probe.canPlayType(raw))
            return true;
        if (mime === 'audio/mpeg' || mime === 'audio/mp3' ||
            mime === 'video/mpeg')
            return true;
        return false;
    }

    function ndRetargetMediaSourceUrl(from, to, audioUrl) {
        if (!from || !to || from === to ||
            !global.document || !global.document.querySelectorAll)
            return;
        var nodes;
        try { nodes = global.document.querySelectorAll('video,audio,source'); }
        catch (e) { nodes = []; }
        for (var i = 0; i < nodes.length; i++) {
            var node = nodes[i];
            var attr = '';
            try { attr = node.getAttribute && node.getAttribute('src'); } catch (e) {}
            var prop = '';
            try { prop = node.src || ''; } catch (e) {}
            if (attr === from || prop === from) {
                try {
                    if (audioUrl && node.setAttribute)
                        node.setAttribute('data-audio-src', audioUrl);
                    if (node.setAttribute) node.setAttribute('src', to);
                    else node.src = to;
                    if (typeof node.load === 'function') node.load();
                } catch (e) {}
            }
        }
    }

    var ndMseNative = typeof global.__ndMseAppend === 'function' &&
                      typeof global.__ndMseEos === 'function';
    var ndMseNextId = 0;

    function MediaSource() {
        if (!(this instanceof MediaSource)) return new MediaSource();
        this.sourceBuffers = new SourceBufferList();
        this.activeSourceBuffers = new SourceBufferList();
        this.readyState = 'closed';
        this.onsourceopen = null;
        this.onsourceended = null;
        this.onsourceclose = null;
        this._ndDuration = NaN;
        this._ndUrl = '';
        this._ndObjectURL = '';
        this._ndVersion = 0;
        this._ndBytes = 0;
        this._ndMseId = 0;
    }
    ndEventMethods(MediaSource.prototype);
    MediaSource.isTypeSupported = ndSupportedMediaType;
    Object.defineProperty(MediaSource.prototype, 'duration', {
        configurable: true,
        get: function () { return this._ndDuration; },
        set: function (value) {
            value = Number(value);
            if (isNaN(value) || value < 0)
                throw new TypeError('invalid duration');
            if (this.readyState !== 'open')
                throw new Error('InvalidStateError');
            this._ndDuration = value;
            var url = this._ndUrl;
            if (!url || !global.document ||
                !global.document.querySelectorAll)
                return;
            var nodes;
            try { nodes = global.document.querySelectorAll('video,audio'); }
            catch (e) { nodes = []; }
            for (var i = 0; i < nodes.length; i++) {
                var el = nodes[i];
                var src = '';
                try { src = el.src || el.getAttribute('src') || ''; }
                catch (e) {}
                if (src !== url) continue;
                try {
                    var prev = el._nd_duration;
                    if (!(prev > value)) {
                        el._nd_duration = value;
                        if (typeof el.dispatchEvent === 'function' &&
                            typeof Event === 'function')
                            el.dispatchEvent(new Event('durationchange'));
                    }
                } catch (e) {}
            }
        }
    });
    MediaSource.prototype.addSourceBuffer = function (type) {
        type = String(type || '');
        if (!MediaSource.isTypeSupported(type))
            throw new Error('NotSupportedError');
        var buffer = new SourceBuffer(this, type);
        this.sourceBuffers._push(buffer);
        this.activeSourceBuffers._push(buffer);
        this._ndRefreshBlob();
        return buffer;
    };
    MediaSource.prototype.removeSourceBuffer = function (buffer) {
        if (!this.sourceBuffers._remove(buffer)) throw new Error('NotFoundError');
        this.activeSourceBuffers._remove(buffer);
        buffer._removed = true;
        this._ndRefreshBlob();
    };
    MediaSource.prototype.endOfStream = function () {
        if (this.readyState !== 'open') throw new Error('InvalidStateError');
        for (var i = 0; i < this.sourceBuffers.length; i++) {
            var b = this.sourceBuffers.item(i);
            if (b && b.updating) throw new Error('InvalidStateError');
        }
        this.readyState = 'ended';
        if (ndMseNative && this._ndMseId)
            global.__ndMseEos(this._ndMseId);
        else
            this._ndRefreshBlob();
        ndFireEvent(this, 'sourceended');
    };
    MediaSource.prototype.setLiveSeekableRange = function () {};
    MediaSource.prototype.clearLiveSeekableRange = function () {};
    MediaSource.prototype._ndOpen = function () {
        if (this.readyState !== 'closed') return;
        this.readyState = 'open';
        ndFireEvent(this, 'sourceopen');
    };
    MediaSource.prototype._ndRefreshBlob = function () {
        if (!this._ndUrl || typeof Blob !== 'function' ||
            typeof global.__ndUpdateBlobURL !== 'function')
            return false;
        var parts = [];
        var type = '';
        var bytes = 0;
        var selected = null;
        for (var i = 0; i < this.sourceBuffers.length; i++) {
            var buffer = this.sourceBuffers.item(i);
            if (!buffer) continue;
            if (!selected || buffer._type.indexOf('video/') === 0)
                selected = buffer;
            if (selected && selected._type.indexOf('video/') === 0)
                break;
        }
        if (selected) {
            type = selected._type || '';
            bytes = selected._bytes || 0;
            for (var j = 0; j < selected._parts.length; j++)
                parts.push(selected._parts[j]);
        }
        var blob = new Blob(parts, { type: type || 'application/octet-stream' });
        var audioSel = null;
        for (var ai = 0; ai < this.sourceBuffers.length; ai++) {
            var ab = this.sourceBuffers.item(ai);
            if (ab && ab !== selected && ab._type.indexOf('audio/') === 0) {
                audioSel = ab;
                break;
            }
        }
        var oldUrl = this._ndUrl;
        var eos = this.readyState === 'ended';
        if (this._ndObjectURL &&
            (bytes !== this._ndBytes ||
             (eos && this._ndUrl.indexOf('&eos') < 0))) {
            this._ndBytes = bytes;
            this._ndVersion++;
            this._ndUrl = this._ndObjectURL + '#ndms=' + this._ndVersion +
                          (eos ? '&eos=1' : '');
        }
        var ok = !!global.__ndUpdateBlobURL(this._ndUrl, blob);
        if (this._ndObjectURL && this._ndObjectURL !== this._ndUrl)
            global.__ndUpdateBlobURL(this._ndObjectURL, blob);
        if (audioSel && audioSel._parts.length && this._ndObjectURL) {
            var audioBlob = new Blob(audioSel._parts,
                                     { type: audioSel._type });
            this._ndAudioUrl = this._ndObjectURL + '#ndmsa=' + this._ndVersion;
            global.__ndUpdateBlobURL(this._ndAudioUrl, audioBlob);
        }
        this._ndScheduleRetarget(oldUrl);
        return ok;
    };
    MediaSource.prototype._ndScheduleRetarget = function (oldUrl) {
        var self = this;
        var now = Date.now();
        var last = this._ndLastRetarget || 0;
        var wait = 2500 - (now - last);
        if (this.readyState === 'ended' || !last || wait <= 0) {
            if (this._ndRetargetTimer) {
                clearTimeout(this._ndRetargetTimer);
                this._ndRetargetTimer = 0;
            }
            var from = this._ndRetargetFrom || oldUrl;
            this._ndRetargetFrom = '';
            this._ndLastRetarget = now;
            ndRetargetMediaSourceUrl(from, this._ndUrl, this._ndAudioUrl);
            return;
        }
        if (!this._ndRetargetFrom) this._ndRetargetFrom = oldUrl;
        if (this._ndRetargetTimer) return;
        this._ndRetargetTimer = setTimeout(function () {
            self._ndRetargetTimer = 0;
            var deferredFrom = self._ndRetargetFrom;
            self._ndRetargetFrom = '';
            self._ndLastRetarget = Date.now();
            ndRetargetMediaSourceUrl(deferredFrom, self._ndUrl, self._ndAudioUrl);
        }, wait);
    };

    function SourceBuffer(mediaSource, type) {
        if (!(this instanceof SourceBuffer)) return new SourceBuffer(mediaSource, type);
        this.updating = false;
        this.mode = 'segments';
        this.timestampOffset = 0;
        this.appendWindowStart = 0;
        this.appendWindowEnd = Infinity;
        this.onabort = null;
        this.onerror = null;
        this.onupdate = null;
        this.onupdatestart = null;
        this.onupdateend = null;
        this._mediaSource = mediaSource || null;
        this._type = String(type || '').split(';')[0].trim().toLowerCase();
        this._fullType = String(type || '');
        this._parts = [];
        this._removed = false;
        this._bytes = 0;
        this._buffered = new ndTimeRanges(0, 0);
        this._taskSeq = 0;
        this._quotaFull = false;
    }
    ndEventMethods(SourceBuffer.prototype);
    Object.defineProperty(SourceBuffer.prototype, 'buffered', {
        configurable: true,
        get: function () {
            var ms = this._mediaSource;
            if (ndMseNative && ms && ms._ndMseId &&
                typeof global.__ndMseBuffered === 'function') {
                var end = global.__ndMseBuffered(ms._ndMseId,
                    this._type.indexOf('audio/') === 0 ? 'a' : 'v');
                return new ndTimeRanges(0, end > 0 ? end : 0);
            }
            return this._buffered;
        }
    });
    SourceBuffer.prototype.appendBuffer = function (data) {
        if (this._removed || !this._mediaSource ||
            this._mediaSource.readyState === 'closed' || this.updating)
            throw new Error('InvalidStateError');
        if (this._quotaFull) {
            var qe = new Error('QuotaExceededError');
            qe.name = 'QuotaExceededError';
            throw qe;
        }
        if (this._mediaSource.readyState === 'ended') {
            this._mediaSource.readyState = 'open';
            ndFireEvent(this._mediaSource, 'sourceopen');
        }
        var bytes = blobPartBytes(data);
        var copy = new Uint8Array(bytes.length);
        copy.set(bytes);
        this.updating = true;
        ndFireEvent(this, 'updatestart');
        var self = this;
        var seq = ++this._taskSeq;
        ndMediaTask(function () {
            if (seq !== self._taskSeq) return;
            var ms = self._mediaSource;
            var ok = true;
            if (ndMseNative && ms && ms._ndMseId) {
                ok = !!global.__ndMseAppend(ms._ndMseId,
                    self._type.indexOf('audio/') === 0 ? 'a' : 'v', copy);
            } else {
                self._parts.push(copy);
            }
            self.updating = false;
            if (!ok) {
                self._quotaFull = true;
                ndFireEvent(self, 'error');
                ndFireEvent(self, 'updateend');
                return;
            }
            self._bytes += copy.length;
            if (!(ndMseNative && ms && ms._ndMseId)) {
                var seconds = self._bytes > 0 ?
                    Math.max(0.001, self._bytes / 262144) : 0;
                self._buffered = new ndTimeRanges(0, seconds);
                if (ms) ms._ndRefreshBlob();
            }
            ndFireEvent(self, 'update');
            ndFireEvent(self, 'updateend');
        });
    };
    SourceBuffer.prototype.appendBufferAsync = function (data) {
        var self = this;
        return new Promise(function (resolve, reject) {
            function done() {
                self.removeEventListener('updateend', done);
                self.removeEventListener('error', fail);
                resolve();
            }
            function fail(ev) {
                self.removeEventListener('updateend', done);
                self.removeEventListener('error', fail);
                reject(ev);
            }
            self.addEventListener('updateend', done);
            self.addEventListener('error', fail);
            try { self.appendBuffer(data); } catch (e) { fail(e); }
        });
    };
    SourceBuffer.prototype.remove = function (start, end) {
        if (this._removed || this.updating) throw new Error('InvalidStateError');
        var ms = this._mediaSource;
        if (ms && ms.readyState === 'ended') {
            ms.readyState = 'open';
            ndFireEvent(ms, 'sourceopen');
        }
        start = +start || 0;
        end = +end || 0;
        this.updating = true;
        ndFireEvent(this, 'updatestart');
        var self = this;
        var seq = ++this._taskSeq;
        ndMediaTask(function () {
            if (seq !== self._taskSeq) return;
            if (start <= 0 && end > 0 &&
                !(ndMseNative && self._mediaSource &&
                  self._mediaSource._ndMseId)) {
                self._parts = [];
                self._bytes = 0;
                self._buffered = new ndTimeRanges(0, 0);
            }
            self.updating = false;
            if (self._mediaSource &&
                !(ndMseNative && self._mediaSource._ndMseId))
                self._mediaSource._ndRefreshBlob();
            ndFireEvent(self, 'update');
            ndFireEvent(self, 'updateend');
        });
    };
    SourceBuffer.prototype.removeAsync = function (start, end) {
        var self = this;
        return new Promise(function (resolve, reject) {
            function done() {
                self.removeEventListener('updateend', done);
                self.removeEventListener('error', fail);
                resolve();
            }
            function fail(ev) {
                self.removeEventListener('updateend', done);
                self.removeEventListener('error', fail);
                reject(ev);
            }
            self.addEventListener('updateend', done);
            self.addEventListener('error', fail);
            try { self.remove(start, end); } catch (e) { fail(e); }
        });
    };
    SourceBuffer.prototype.abort = function () {
        if (!this._mediaSource ||
            this._mediaSource.readyState === 'closed')
            throw new Error('InvalidStateError');
        this._taskSeq++;
        if (this.updating) {
            this.updating = false;
            ndFireEvent(this, 'abort');
            ndFireEvent(this, 'updateend');
        }
    };
    SourceBuffer.prototype.changeType = function (type) {
        type = String(type || '');
        if (!MediaSource.isTypeSupported(type)) throw new Error('NotSupportedError');
        this._fullType = type;
        this._type = type.split(';')[0].trim().toLowerCase();
        if (this._mediaSource) this._mediaSource._ndRefreshBlob();
    };

    replaceCtor('MediaSource', MediaSource);
    replaceCtor('SourceBuffer', SourceBuffer);
    replaceCtor('SourceBufferList', SourceBufferList);

    if (typeof global.URL === 'function' &&
        typeof global.URL.createObjectURL === 'function' &&
        !global.URL.__ndMediaSourceObjectURL) {
        var ndCreateObjectURL = global.URL.createObjectURL;
        var ndRevokeObjectURL = global.URL.revokeObjectURL;
        global.URL.createObjectURL = function (obj) {
            if (obj instanceof MediaSource) {
                var url;
                if (ndMseNative) {
                    obj._ndMseId = ++ndMseNextId;
                    url = 'blob:nd-mse/' + obj._ndMseId;
                    obj._ndUrl = url;
                    obj._ndObjectURL = url;
                } else {
                    url = ndCreateObjectURL.call(this,
                        new Blob([], { type: 'application/octet-stream' }));
                    obj._ndUrl = url;
                    obj._ndObjectURL = url;
                    obj._ndRefreshBlob();
                }
                ndMediaTask(function () { obj._ndOpen(); });
                return url;
            }
            return ndCreateObjectURL.apply(this, arguments);
        };
        global.URL.revokeObjectURL = function () {
            return ndRevokeObjectURL.apply(this, arguments);
        };
        try {
            Object.defineProperty(global.URL, '__ndMediaSourceObjectURL', {
                value: true, configurable: true
            });
        } catch (e) { global.URL.__ndMediaSourceObjectURL = true; }
    }

    if (typeof global.URL === 'function' &&
        typeof global.URL.canParse !== 'function') {
        global.URL.canParse = function (url, base) {
            try { new global.URL(url, base); return true; }
            catch (e) { return false; }
        };
    }
    if (typeof global.URL === 'function' &&
        typeof global.URL.parse !== 'function') {
        global.URL.parse = function (url, base) {
            try { return new global.URL(url, base); }
            catch (e) { return null; }
        };
    }

    function XMLSerializer() {
        if (!(this instanceof XMLSerializer)) return new XMLSerializer();
    }
    function xmlSerializeNode(node) {
        if (!node) return '';
        if (node.nodeType === 3) return String(node.nodeValue == null ? '' : node.nodeValue);
        if (node.nodeType === 8) return '<!--' + String(node.nodeValue || '') + '-->';
        if (typeof node.outerHTML === 'string') return node.outerHTML;
        if (node.nodeType === 9) {
            if (node.documentElement &&
                typeof node.documentElement.outerHTML === 'string')
                return node.documentElement.outerHTML;
        }
        if (node.nodeType === 11) {
            var s = '';
            var c = node.firstChild;
            while (c) { s += xmlSerializeNode(c); c = c.nextSibling; }
            return s;
        }
        if (typeof node.innerHTML === 'string') return node.innerHTML;
        return String(node);
    }
    XMLSerializer.prototype.serializeToString = function (node) {
        return xmlSerializeNode(node);
    };
    defineCtor('XMLSerializer', XMLSerializer);

    function AbortSignal() {
        if (!(this instanceof AbortSignal)) return new AbortSignal();
        this.aborted = false;
        this.reason = undefined;
        this._cbs = [];
        this.onabort = null;
    }
    AbortSignal.prototype.addEventListener = function (type, cb) {
        if (type !== 'abort' || typeof cb !== 'function') return;
        this._cbs.push(cb);
    };
    AbortSignal.prototype.removeEventListener = function (type, cb) {
        if (type !== 'abort') return;
        var i = this._cbs.indexOf(cb);
        if (i >= 0) this._cbs.splice(i, 1);
    };
    AbortSignal.prototype.dispatchEvent = function (ev) {
        if (ev && ev.type === 'abort') this._fire(ev);
        return true;
    };
    AbortSignal.prototype.throwIfAborted = function () {
        if (this.aborted) {
            var r = this.reason;
            if (r === undefined) {
                var e = new Error('AbortError');
                e.name = 'AbortError';
                r = e;
            }
            throw r;
        }
    };
    AbortSignal.prototype._fire = function (ev) {
        if (typeof this.onabort === 'function') {
            try { this.onabort.call(this, ev); } catch (e) {}
        }
        var cbs = this._cbs.slice();
        for (var i = 0; i < cbs.length; i++) {
            try { cbs[i].call(this, ev); } catch (e) {}
        }
    };
    AbortSignal.abort = function (reason) {
        var s = new AbortSignal();
        s.aborted = true;
        s.reason = reason === undefined ? new Error('AbortError') : reason;
        return s;
    };
    AbortSignal.timeout = function (ms) {
        var s = new AbortSignal();
        setTimeout(function () {
            if (!s.aborted) {
                s.aborted = true;
                var e = new Error('TimeoutError'); e.name = 'TimeoutError';
                s.reason = e;
                s._fire({type: 'abort', target: s});
            }
        }, ms);
        return s;
    };
    AbortSignal.any = function (signals) {
        var s = new AbortSignal();
        function onAny(src) {
            if (s.aborted) return;
            s.aborted = true;
            s.reason = src.reason;
            s._fire({type: 'abort', target: s});
        }
        for (var i = 0; i < signals.length; i++) {
            var sig = signals[i];
            if (sig.aborted) { onAny(sig); break; }
            (function (sig) { sig.addEventListener('abort', function () { onAny(sig); }); })(sig);
        }
        return s;
    };
    defineCtor('AbortSignal', AbortSignal);

    function AbortController() {
        if (!(this instanceof AbortController)) return new AbortController();
        this.signal = new AbortSignal();
    }
    AbortController.prototype.abort = function (reason) {
        var s = this.signal;
        if (s.aborted) return;
        s.aborted = true;
        s.reason = reason === undefined ? new Error('AbortError') : reason;
        s._fire({type: 'abort', target: s});
    };
    defineCtor('AbortController', AbortController);

    try {
        var probe = global.document && global.document.createElement('div');
        var eventTargetProto = probe && Object.getPrototypeOf(probe);
        if (eventTargetProto) {
            var origAEL = eventTargetProto.addEventListener;
            if (typeof origAEL === 'function') {
                eventTargetProto.addEventListener = function (type, cb, opts) {
                    if (opts && typeof opts === 'object' && opts.signal) {
                        var sig = opts.signal;
                        if (sig && sig.aborted) return;
                        var self = this;
                        origAEL.call(self, type, cb, opts);
                        if (sig && typeof sig.addEventListener === 'function') {
                            sig.addEventListener('abort', function once() {
                                self.removeEventListener(type, cb, opts);
                            });
                        }
                        return;
                    }
                    return origAEL.call(this, type, cb, opts);
                };
            }
        }
    } catch (e) { /* ignore */ }

    if (!global.NodeFilter || typeof global.NodeFilter.SHOW_ALL !== 'number') {
        var NF = global.NodeFilter || {};
        NF.SHOW_ALL                  = 0xFFFFFFFF;
        NF.SHOW_ELEMENT              = 0x1;
        NF.SHOW_ATTRIBUTE            = 0x2;
        NF.SHOW_TEXT                 = 0x4;
        NF.SHOW_CDATA_SECTION        = 0x8;
        NF.SHOW_ENTITY_REFERENCE     = 0x10;
        NF.SHOW_ENTITY               = 0x20;
        NF.SHOW_PROCESSING_INSTRUCTION = 0x40;
        NF.SHOW_NOTATION             = 0x800;
        NF.SHOW_COMMENT              = 0x80;
        NF.SHOW_DOCUMENT             = 0x100;
        NF.SHOW_DOCUMENT_TYPE        = 0x200;
        NF.SHOW_DOCUMENT_FRAGMENT    = 0x400;
        NF.FILTER_ACCEPT             = 1;
        NF.FILTER_REJECT             = 2;
        NF.FILTER_SKIP               = 3;
        defineCtor('NodeFilter', NF);
    }

    try {
        var doc = global.document;
        if (doc && doc.createElement) {
            var probe = doc.createElement('div');
            var elementProto = Object.getPrototypeOf(probe);
            if (elementProto) {
                var onProps = [
                    'click','dblclick','mousedown','mouseup','mousemove','mouseenter',
                    'mouseleave','mouseover','mouseout','contextmenu','wheel',
                    'keydown','keyup','keypress',
                    'focus','blur','focusin','focusout',
                    'input','change','submit','reset','select',
                    'load','error','abort','loadstart','loadend','progress',
                    'animationstart','animationend','animationiteration',
                    'transitionstart','transitionend','transitionrun','transitioncancel',
                    'webkitanimationstart','webkitanimationend','webkitanimationiteration',
                    'webkittransitionend',
                    'pointerdown','pointerup','pointermove','pointerenter',
                    'pointerleave','pointerover','pointerout','pointercancel',
                    'touchstart','touchend','touchmove','touchcancel',
                    'drag','dragstart','dragend','dragenter','dragleave','dragover','drop',
                    'scroll','resize',
                    'copy','cut','paste',
                    'beforeinput','compositionstart','compositionend','compositionupdate',
                    'invalid'
                ];
                function makeOnAccessor(propName) {
                    var slot = Symbol.for('nd.on.' + propName);
                    var cslot = Symbol.for('nd.onc.' + propName);
                    var sslot = Symbol.for('nd.ons.' + propName);
                    return {
                        configurable: true, enumerable: false,
                        get: function () {
                            var h = this[slot];
                            if (h) return h;
                            if (!this || typeof this.getAttribute !== 'function')
                                return null;
                            var code = this.getAttribute(propName);
                            if (code == null) return null;
                            if (this[sslot] === code && this[cslot])
                                return this[cslot];
                            var fn;
                            try { fn = new Function('event', code); }
                            catch (e) { return null; }
                            this[cslot] = fn;
                            this[sslot] = code;
                            return fn;
                        },
                        set: function (v) {
                            this[slot] = (typeof v === 'function') ? v : null;
                        }
                    };
                }
                for (var i = 0; i < onProps.length; i++) {
                    var p = 'on' + onProps[i];
                    if (Object.getOwnPropertyDescriptor(elementProto, p)) continue;
                    Object.defineProperty(elementProto, p, makeOnAccessor(p));
                }
            }
            if (elementProto) {
                function camelToAttr(key) {
                    return 'data-' + String(key).replace(/[A-Z]/g, function (c) {
                        return '-' + c.toLowerCase();
                    });
                }
                function attrToCamel(name) {
                    return name.slice(5).replace(/-([a-z])/g, function (_, c) {
                        return c.toUpperCase();
                    });
                }
                function defineFrameAccessor(name, getter) {
                    if (Object.getOwnPropertyDescriptor(elementProto, name)) return;
                    var nativeGet = null;
                    for (var anc = Object.getPrototypeOf(elementProto); anc;
                         anc = Object.getPrototypeOf(anc)) {
                        var d = Object.getOwnPropertyDescriptor(anc, name);
                        if (d && d.get) { nativeGet = d.get; break; }
                    }
                    Object.defineProperty(elementProto, name, {
                        configurable: true, get: nativeGet || getter
                    });
                }
                function isFrameElement(el) {
                    var tag = el && el.nodeName ? String(el.nodeName).toLowerCase() : '';
                    return tag === 'iframe' || tag === 'frame' ||
                           tag === 'object' || tag === 'embed';
                }
                defineFrameAccessor('contentDocument', function () {
                    return isFrameElement(this) ? null : null;
                });
                defineFrameAccessor('contentWindow', function () {
                    if (!isFrameElement(this)) return null;
                    return {
                        document: null,
                        location: { href: '', replace: function () {}, assign: function () {} },
                        postMessage: function () {},
                        addEventListener: function () {},
                        removeEventListener: function () {},
                        focus: function () {},
                        blur: function () {},
                        close: function () {},
                        closed: true
                    };
                });
                if (!('dataset' in probe)) {
                    Object.defineProperty(elementProto, 'dataset', {
                        configurable: true,
                        get: function () {
                            var el = this;
                            return new Proxy({}, {
                                get: function (t, key) {
                                    if (typeof key !== 'string') return undefined;
                                    var v = el.getAttribute(camelToAttr(key));
                                    return v == null ? undefined : v;
                                },
                                set: function (t, key, value) {
                                    if (typeof key !== 'string') return false;
                                    if (/-[a-z]/.test(key))
                                        throw new SyntaxError(
                                            "'-' must not be followed by a lowercase " +
                                            'letter in a dataset name');
                                    el.setAttribute(camelToAttr(key), String(value));
                                    return true;
                                },
                                has: function (t, key) {
                                    if (typeof key !== 'string') return false;
                                    return el.hasAttribute(camelToAttr(key));
                                },
                                deleteProperty: function (t, key) {
                                    if (typeof key !== 'string') return false;
                                    el.removeAttribute(camelToAttr(key));
                                    return true;
                                },
                                ownKeys: function () {
                                    var out = [];
                                    var attrs = el.attributes;
                                    var n = attrs ? attrs.length : 0;
                                    for (var i = 0; i < n; i++) {
                                        var nm = attrs[i].name;
                                        if (nm.indexOf('data-') === 0)
                                            out.push(attrToCamel(nm));
                                    }
                                    return out;
                                },
                                getOwnPropertyDescriptor: function (t, key) {
                                    if (typeof key !== 'string') return undefined;
                                    if (!el.hasAttribute(camelToAttr(key))) return undefined;
                                    return {
                                        enumerable: true, configurable: true,
                                        writable: true,
                                        value: el.getAttribute(camelToAttr(key))
                                    };
                                }
                            });
                        }
                    });
                }
            }
        }
    } catch (e) { /* prototype may be locked; tolerate */ }

    function ReadableStream(underlying, strategy) {
        if (!(this instanceof ReadableStream))
            return new ReadableStream(underlying, strategy);
        var self = this;
        self._buf = [];
        self._closed = false;
        self._error = null;
        self._cancelled = false;
        self._waiters = [];
        self.locked = false;
        function wake() {
            var ws = self._waiters;
            self._waiters = [];
            for (var i = 0; i < ws.length; i++) ws[i]();
        }
        var controller = {
            enqueue: function (chunk) {
                if (!self._closed && !self._cancelled) {
                    self._buf.push(chunk);
                    wake();
                }
            },
            close: function () { self._closed = true; wake(); },
            error: function (e) { self._error = e; self._closed = true; wake(); },
            get desiredSize() { return self._closed ? 0 : 1; }
        };
        self._controller = controller;
        if (underlying && typeof underlying.start === 'function') {
            try { underlying.start(controller); } catch (e) { /* ignore */ }
        }
        self._underlying = underlying || {};
    }
    function rsReadOnce(self) {
        if (self._error) return Promise.reject(self._error);
        if (self._buf.length > 0)
            return Promise.resolve({ value: self._buf.shift(), done: false });
        if (self._closed)
            return Promise.resolve({ value: undefined, done: true });
        return new Promise(function (resolve, reject) {
            self._waiters.push(function () {
                if (self._error) { reject(self._error); return; }
                if (self._buf.length > 0)
                    resolve({ value: self._buf.shift(), done: false });
                else
                    resolve({ value: undefined, done: true });
            });
        });
    }
    ReadableStream.prototype.getReader = function () {
        var self = this;
        self.locked = true;
        return {
            read: function () { return rsReadOnce(self); },
            cancel: function () { self._cancelled = true; return Promise.resolve(); },
            releaseLock: function () { self.locked = false; },
            closed: Promise.resolve()
        };
    };
    ReadableStream.prototype.cancel = function () {
        this._cancelled = true; return Promise.resolve();
    };
    ReadableStream.prototype.pipeTo = function (writable) {
        var self = this;
        if (!writable || !writable._writeChunk) return Promise.resolve();
        function pump() {
            return rsReadOnce(self).then(function (r) {
                if (r.done) {
                    if (writable._closeStream) writable._closeStream();
                    return;
                }
                writable._writeChunk(r.value);
                return pump();
            });
        }
        return pump();
    };
    ReadableStream.prototype.pipeThrough = function (transform) {
        if (!transform || !transform.readable) return new ReadableStream();
        if (transform.writable && transform.writable._writeChunk) {
            this.pipeTo(transform.writable);
        }
        return transform.readable;
    };
    ReadableStream.prototype.tee = function () { return [this, this]; };
    if (typeof Symbol !== 'undefined' && Symbol.asyncIterator) {
        ReadableStream.prototype[Symbol.asyncIterator] = function () {
            var self = this;
            return {
                next: function () { return rsReadOnce(self); },
                return: function () { return Promise.resolve({value: undefined, done: true}); }
            };
        };
    }
    if (typeof global.ReadableStream !== 'function' ||
        typeof global.ReadableStream.prototype.getReader !== 'function')
        replaceCtor('ReadableStream', ReadableStream);

    function WritableStream(underlying, strategy) {
        if (!(this instanceof WritableStream))
            return new WritableStream(underlying, strategy);
        var self = this;
        var u = underlying || {};
        self._underlying = u;
        self.locked = false;
        var controller = { error: function () {}, signal: undefined };
        if (typeof u.start === 'function') {
            try { u.start(controller); } catch (e) { /* ignore */ }
        }
        if (typeof u.write === 'function' && !self._writeChunk)
            self._writeChunk = function (chunk) { return u.write(chunk, controller); };
        if (typeof u.close === 'function' && !self._closeStream)
            self._closeStream = function () { return u.close(); };
        if (typeof u.abort === 'function' && !self._abortStream)
            self._abortStream = function (reason) { return u.abort(reason); };
    }
    function wsInvoke(fn, arg) {
        if (typeof fn !== 'function') return Promise.resolve();
        try { return Promise.resolve(fn(arg)); }
        catch (e) { return Promise.reject(e); }
    }
    WritableStream.prototype.getWriter = function () {
        var self = this;
        self.locked = true;
        return {
            write: function (chunk) { return wsInvoke(self._writeChunk, chunk); },
            close: function () { return wsInvoke(self._closeStream); },
            abort: function (reason) { return wsInvoke(self._abortStream, reason); },
            releaseLock: function () { self.locked = false; },
            ready: Promise.resolve(),
            closed: Promise.resolve(),
            desiredSize: 1
        };
    };
    WritableStream.prototype.abort = function (reason) {
        return wsInvoke(this._abortStream, reason);
    };
    WritableStream.prototype.close = function () {
        return wsInvoke(this._closeStream);
    };
    if (typeof global.WritableStream !== 'function' ||
        typeof global.WritableStream.prototype.getWriter !== 'function')
        replaceCtor('WritableStream', WritableStream);

    function TransformStream(transformer, writableStrategy, readableStrategy) {
        if (!(this instanceof TransformStream))
            return new TransformStream(transformer, writableStrategy, readableStrategy);
        var readable = new ReadableStream();
        var writable = new WritableStream();
        var controller = readable._controller;
        var t = transformer || {};
        var transformCtl = {
            enqueue: function (chunk) { controller.enqueue(chunk); },
            terminate: function () { controller.close(); },
            error: function (e) { controller.error(e); }
        };
        writable._writeChunk = function (chunk) {
            if (typeof t.transform === 'function') {
                try { t.transform(chunk, transformCtl); }
                catch (e) { controller.error(e); }
            } else {
                controller.enqueue(chunk);
            }
        };
        writable._closeStream = function () {
            if (typeof t.flush === 'function') {
                try { t.flush(transformCtl); }
                catch (e) { controller.error(e); }
            }
            controller.close();
        };
        this.readable = readable;
        this.writable = writable;
        if (typeof t.start === 'function') {
            try { t.start(transformCtl); } catch (e) { /* ignore */ }
        }
    }
    defineCtor('TransformStream', TransformStream);

    function TextEncoderStream() {
        if (!(this instanceof TextEncoderStream)) return new TextEncoderStream();
        var enc = new TextEncoder();
        TransformStream.call(this, {
            transform: function (chunk, controller) {
                controller.enqueue(enc.encode(String(chunk == null ? '' : chunk)));
            }
        });
        Object.defineProperty(this, 'encoding', { value: 'utf-8', configurable: true });
    }
    TextEncoderStream.prototype = Object.create(TransformStream.prototype);
    TextEncoderStream.prototype.constructor = TextEncoderStream;
    defineCtor('TextEncoderStream', TextEncoderStream);

    function TextDecoderStream(label, options) {
        if (!(this instanceof TextDecoderStream)) return new TextDecoderStream(label, options);
        var dec = new TextDecoder(label || 'utf-8', options);
        TransformStream.call(this, {
            transform: function (chunk, controller) {
                var out;
                try { out = dec.decode(chunk, { stream: true }); }
                catch (e) { out = String(chunk); }
                if (out) controller.enqueue(out);
            },
            flush: function (controller) {
                try {
                    var rest = dec.decode();
                    if (rest) controller.enqueue(rest);
                } catch (e) { /* ignore */ }
            }
        });
        Object.defineProperty(this, 'encoding', {
            value: label ? String(label).toLowerCase() : 'utf-8',
            configurable: true
        });
    }
    TextDecoderStream.prototype = Object.create(TransformStream.prototype);
    TextDecoderStream.prototype.constructor = TextDecoderStream;
    defineCtor('TextDecoderStream', TextDecoderStream);

    // Intl (ECMA-402) is implemented natively in C; see src/js_intl.c.

    var zlibCreate = global.__ns_zlib_create;
    var zlibPush   = global.__ns_zlib_push;
    var zlibFinish = global.__ns_zlib_finish;
    try {
        delete global.__ns_zlib_create;
        delete global.__ns_zlib_push;
        delete global.__ns_zlib_finish;
    } catch (e) { /* ignore */ }

    var ZLIB_FORMATS = { 'gzip': 1, 'deflate': 1, 'deflate-raw': 1 };

    function zlibTransformer(format, decompress) {
        var fmt = String(format);
        if (!ZLIB_FORMATS[fmt])
            throw new TypeError("Unsupported compression format: '" + fmt + "'");
        if (typeof zlibCreate !== 'function') {
            return { transform: function (chunk, ctl) { ctl.enqueue(chunk); } };
        }
        var codec = zlibCreate(fmt, decompress);
        return {
            transform: function (chunk, ctl) {
                var u8 = chunk instanceof Uint8Array ? chunk
                       : ArrayBuffer.isView(chunk)
                         ? new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength)
                         : new Uint8Array(chunk);
                var out = zlibPush(codec, u8);
                if (out && out.byteLength) ctl.enqueue(new Uint8Array(out));
            },
            flush: function (ctl) {
                var out = zlibFinish(codec);
                if (out && out.byteLength) ctl.enqueue(new Uint8Array(out));
            }
        };
    }

    function CompressionStream(format) {
        if (!(this instanceof CompressionStream)) return new CompressionStream(format);
        TransformStream.call(this, zlibTransformer(format, false));
    }
    CompressionStream.prototype = Object.create(TransformStream.prototype);
    CompressionStream.prototype.constructor = CompressionStream;
    defineCtor('CompressionStream', CompressionStream);

    function DecompressionStream(format) {
        if (!(this instanceof DecompressionStream)) return new DecompressionStream(format);
        TransformStream.call(this, zlibTransformer(format, true));
    }
    DecompressionStream.prototype = Object.create(TransformStream.prototype);
    DecompressionStream.prototype.constructor = DecompressionStream;
    defineCtor('DecompressionStream', DecompressionStream);

    if (typeof Request === 'function' && typeof Response === 'function') {
        var cacheStores = new Map();

        function cacheKey(request, ignoreSearch) {
            var url;
            if (request && typeof request === 'object' && request.url)
                url = request.url;
            else { try { url = new Request(request).url; } catch (e) { url = String(request); } }
            if (ignoreSearch) {
                var q = url.indexOf('?');
                if (q >= 0) url = url.slice(0, q);
            }
            return url;
        }

        function headerPairs(h) {
            var out = [];
            if (!h) return out;
            try {
                if (typeof h.forEach === 'function') {
                    h.forEach(function (v, k) { out.push([k, v]); });
                } else if (typeof h.entries === 'function') {
                    var it = h.entries(), e;
                    while (!(e = it.next()).done) out.push([e.value[0], e.value[1]]);
                }
            } catch (err) { /* tolerate */ }
            return out;
        }

        function NSCache() { this._entries = new Map(); }

        function requestMethod(request) {
            if (request && typeof request === 'object' && request.method)
                return String(request.method).toUpperCase();
            return 'GET';
        }

        NSCache.prototype.put = function (request, response) {
            if (requestMethod(request) !== 'GET')
                return Promise.reject(new TypeError('Cache.put: only GET requests can be cached'));
            if (response && response.bodyUsed)
                return Promise.reject(new TypeError('Response body is already used'));
            if (response && (response.status === 206))
                return Promise.reject(new TypeError('Partial response (206) cannot be cached'));
            var self = this, key = cacheKey(request);
            var src = (response && typeof response.clone === 'function')
                ? response.clone() : response;
            return Promise.resolve(src.arrayBuffer()).then(function (ab) {
                self._entries.set(key, {
                    body: ab,
                    status: response.status === undefined ? 200 : response.status,
                    statusText: response.statusText || '',
                    headers: headerPairs(response.headers),
                    url: response.url || key
                });
            });
        };

        function entryToResponse(entry) {
            var resp = new Response(new Uint8Array(entry.body), {
                status: entry.status,
                statusText: entry.statusText,
                headers: entry.headers
            });
            try { resp.url = entry.url; } catch (e) { /* read-only? tolerate */ }
            return resp;
        }

        NSCache.prototype.match = function (request, options) {
            options = options || {};
            var entry = this._entries.get(cacheKey(request, options.ignoreSearch));
            if (!entry && options.ignoreSearch) {
                var want = cacheKey(request, true), it = this._entries.entries(), e;
                while (!(e = it.next()).done) {
                    var k = e.value[0], q = k.indexOf('?');
                    if ((q >= 0 ? k.slice(0, q) : k) === want) { entry = e.value[1]; break; }
                }
            }
            return Promise.resolve(entry ? entryToResponse(entry) : undefined);
        };

        NSCache.prototype.matchAll = function (request, options) {
            if (request === undefined) {
                var all = [];
                this._entries.forEach(function (entry) { all.push(entryToResponse(entry)); });
                return Promise.resolve(all);
            }
            return this.match(request, options).then(function (m) {
                return m ? [m] : [];
            });
        };

        NSCache.prototype.add = function (request) {
            var self = this;
            return fetch(request).then(function (resp) {
                if (!resp.ok)
                    throw new TypeError('Request failed with status ' + resp.status);
                return self.put(request, resp);
            });
        };

        NSCache.prototype.addAll = function (requests) {
            var self = this;
            return Promise.all(Array.prototype.map.call(requests, function (r) {
                return self.add(r);
            })).then(function () { return undefined; });
        };

        NSCache.prototype.delete = function (request, options) {
            var key = cacheKey(request, options && options.ignoreSearch);
            return Promise.resolve(this._entries.delete(key));
        };

        NSCache.prototype.keys = function () {
            var reqs = [];
            this._entries.forEach(function (entry, key) { reqs.push(new Request(key)); });
            return Promise.resolve(reqs);
        };

        var cacheStorage = {
            open: function (name) {
                name = String(name);
                var c = cacheStores.get(name);
                if (!c) { c = new NSCache(); cacheStores.set(name, c); }
                return Promise.resolve(c);
            },
            has: function (name) {
                return Promise.resolve(cacheStores.has(String(name)));
            },
            delete: function (name) {
                return Promise.resolve(cacheStores.delete(String(name)));
            },
            keys: function () {
                return Promise.resolve(Array.from(cacheStores.keys()));
            },
            match: function (request, options) {
                var stores = Array.from(cacheStores.values());
                return (function next(i) {
                    if (i >= stores.length) return Promise.resolve(undefined);
                    return stores[i].match(request, options).then(function (m) {
                        return m || next(i + 1);
                    });
                })(0);
            }
        };

        try { global.caches = cacheStorage; } catch (e) { /* tolerate */ }
        try { global.Cache = NSCache; } catch (e) { /* tolerate */ }
    }

    if (typeof Object.hasOwn !== 'function') {
        Object.hasOwn = function (obj, prop) {
            if (obj == null) throw new TypeError('Object.hasOwn: null/undefined');
            return Object.prototype.hasOwnProperty.call(Object(obj), prop);
        };
    }

    if (typeof Object.fromEntries !== 'function') {
        Object.fromEntries = function (iter) {
            var out = {};
            if (iter == null) throw new TypeError('Object.fromEntries: null/undefined');
            var it = iter[Symbol.iterator] ? iter[Symbol.iterator]() : null;
            if (it) {
                for (var step = it.next(); !step.done; step = it.next()) {
                    var pair = step.value;
                    if (pair == null) throw new TypeError('Object.fromEntries: bad pair');
                    out[String(pair[0])] = pair[1];
                }
            } else if (typeof iter.length === 'number') {
                for (var i = 0; i < iter.length; i++) {
                    var p = iter[i];
                    if (p == null) continue;
                    out[String(p[0])] = p[1];
                }
            } else {
                var keys = Object.keys(iter);
                for (var k = 0; k < keys.length; k++) out[keys[k]] = iter[keys[k]];
            }
            return out;
        };
    }

    if (typeof Promise.withResolvers !== 'function') {
        Promise.withResolvers = function () {
            var resolve, reject;
            var promise = new Promise(function (res, rej) {
                resolve = res; reject = rej;
            });
            return { promise: promise, resolve: resolve, reject: reject };
        };
    }

    if (typeof Promise.any !== 'function') {
        Promise.any = function (iter) {
            var arr = [];
            var it = iter[Symbol.iterator] ? iter[Symbol.iterator]() : null;
            if (it) {
                for (var step = it.next(); !step.done; step = it.next()) arr.push(step.value);
            } else {
                for (var i = 0; i < iter.length; i++) arr.push(iter[i]);
            }
            return new Promise(function (resolve, reject) {
                if (arr.length === 0) {
                    var err = new Error('All promises were rejected');
                    err.name = 'AggregateError';
                    err.errors = [];
                    reject(err);
                    return;
                }
                var errors = new Array(arr.length);
                var remaining = arr.length;
                arr.forEach(function (p, idx) {
                    Promise.resolve(p).then(resolve, function (e) {
                        errors[idx] = e;
                        if (--remaining === 0) {
                            var aerr = new Error('All promises were rejected');
                            aerr.name = 'AggregateError';
                            aerr.errors = errors;
                            reject(aerr);
                        }
                    });
                });
            });
        };
    }

    if (typeof Promise.allSettled !== 'function') {
        Promise.allSettled = function (iter) {
            var arr = [];
            var it = iter[Symbol.iterator] ? iter[Symbol.iterator]() : null;
            if (it) {
                for (var step = it.next(); !step.done; step = it.next()) arr.push(step.value);
            } else {
                for (var i = 0; i < iter.length; i++) arr.push(iter[i]);
            }
            return Promise.all(arr.map(function (p) {
                return Promise.resolve(p).then(
                    function (v) { return { status: 'fulfilled', value: v }; },
                    function (e) { return { status: 'rejected',  reason: e }; }
                );
            }));
        };
    }

    if (typeof Array.prototype.findLast !== 'function') {
        defineMethod(Array.prototype, 'findLast', function (pred, thisArg) {
            for (var i = this.length - 1; i >= 0; i--)
                if (pred.call(thisArg, this[i], i, this)) return this[i];
            return undefined;
        });
    }
    if (typeof Array.prototype.findLastIndex !== 'function') {
        defineMethod(Array.prototype, 'findLastIndex', function (pred, thisArg) {
            for (var i = this.length - 1; i >= 0; i--)
                if (pred.call(thisArg, this[i], i, this)) return i;
            return -1;
        });
    }
    if (typeof Array.prototype.toSorted !== 'function') {
        defineMethod(Array.prototype, 'toSorted', function (cmp) {
            return this.slice().sort(cmp);
        });
    }
    if (typeof Array.prototype.toReversed !== 'function') {
        defineMethod(Array.prototype, 'toReversed', function () {
            return this.slice().reverse();
        });
    }
    if (typeof Array.prototype.toSpliced !== 'function') {
        defineMethod(Array.prototype, 'toSpliced', function (start, count) {
            var copy = this.slice();
            var args = Array.prototype.slice.call(arguments);
            copy.splice.apply(copy, args);
            return copy;
        });
    }
    if (typeof Array.prototype.with !== 'function') {
        defineMethod(Array.prototype, 'with', function (idx, value) {
            var len = this.length;
            if (idx < 0) idx += len;
            if (idx < 0 || idx >= len) throw new RangeError('with: index out of range');
            var copy = this.slice();
            copy[idx] = value;
            return copy;
        });
    }

    if (typeof Object.groupBy !== 'function') {
        Object.groupBy = function (iter, keyFn) {
            var out = Object.create(null);
            var idx = 0;
            var arr = iter && typeof iter.length === 'number' && typeof iter !== 'string'
                ? iter : Array.from(iter);
            for (var i = 0; i < arr.length; i++) {
                var key = keyFn(arr[i], idx++);
                if (!Object.prototype.hasOwnProperty.call(out, key)) out[key] = [];
                out[key].push(arr[i]);
            }
            return out;
        };
    }
    if (typeof Map !== 'undefined' && typeof Map.groupBy !== 'function') {
        Map.groupBy = function (iter, keyFn) {
            var m = new Map();
            var idx = 0;
            var arr = iter && typeof iter.length === 'number' && typeof iter !== 'string'
                ? iter : Array.from(iter);
            for (var i = 0; i < arr.length; i++) {
                var key = keyFn(arr[i], idx++);
                if (!m.has(key)) m.set(key, []);
                m.get(key).push(arr[i]);
            }
            return m;
        };
    }

    if (typeof String.prototype.replaceAll !== 'function') {
        defineMethod(String.prototype, 'replaceAll', function (search, replacement) {
            if (search instanceof RegExp) {
                if (!search.global)
                    throw new TypeError('replaceAll called with a non-global RegExp');
                return this.replace(search, replacement);
            }
            var s = String(this);
            var needle = String(search);
            if (needle === '') {
                if (typeof replacement === 'function') {
                    var out = '';
                    for (var i = 0; i <= s.length; i++) {
                        out += String(replacement('', i, s));
                        if (i < s.length) out += s.charAt(i);
                    }
                    return out;
                }
                return Array.prototype.join.call(s, replacement) + replacement;
            }
            var parts = s.split(needle);
            if (typeof replacement === 'function') {
                var pos = 0, idx = 0;
                var result = '';
                for (var j = 0; j < parts.length; j++) {
                    result += parts[j];
                    if (j < parts.length - 1) {
                        pos += parts[j].length;
                        result += String(replacement(needle, pos, s));
                        pos += needle.length;
                    }
                }
                return result;
            }
            return parts.join(String(replacement));
        });
    }

    if (typeof globalThis.structuredClone !== 'function') {
        defineCtor('structuredClone', function (value) {
            return structClone(value, new Map());
        });
    }
    function structClone(v, seen) {
        if (v == null || typeof v !== 'object') return v;
        if (seen.has(v)) return seen.get(v);
        if (v instanceof Date) return new Date(v.getTime());
        if (v instanceof RegExp) return new RegExp(v.source, v.flags);
        if (v instanceof ArrayBuffer) {
            var c = new ArrayBuffer(v.byteLength);
            new Uint8Array(c).set(new Uint8Array(v));
            return c;
        }
        if (ArrayBuffer.isView && ArrayBuffer.isView(v))
            return new v.constructor(v);
        if (v instanceof Map) {
            var nm = new Map(); seen.set(v, nm);
            v.forEach(function (val, key) {
                nm.set(structClone(key, seen), structClone(val, seen));
            });
            return nm;
        }
        if (v instanceof Set) {
            var ns = new Set(); seen.set(v, ns);
            v.forEach(function (val) { ns.add(structClone(val, seen)); });
            return ns;
        }
        if (Array.isArray(v)) {
            var na = new Array(v.length); seen.set(v, na);
            for (var i = 0; i < v.length; i++) na[i] = structClone(v[i], seen);
            return na;
        }
        var no = {};
        seen.set(v, no);
        for (var k in v) {
            if (Object.prototype.hasOwnProperty.call(v, k))
                no[k] = structClone(v[k], seen);
        }
        return no;
    }

    (function () {
        var backend = global.__nd_idb;
        if (!backend) return;
        if (global.indexedDB) {
            try { delete global.__nd_idb; } catch (e) {
                try { global.__nd_idb = undefined; } catch (e2) {}
            }
            return;
        }
        try { delete global.__nd_idb; } catch (e) {
            try { global.__nd_idb = undefined; } catch (e2) {}
        }

        function ex(name, message) {
            try { return new DOMException(message || name, name); }
            catch (e) {
                var err = new Error(message || name);
                err.name = name;
                return err;
            }
        }

        function task(fn) { setTimeout(fn, 0); }

        function names(list) {
            var a = (list || []).slice().sort();
            Object.defineProperty(a, 'contains', {
                value: function (name) { return a.indexOf(String(name)) >= 0; },
                configurable: true
            });
            Object.defineProperty(a, 'item', {
                value: function (i) { return i >= 0 && i < a.length ? a[i] : null; },
                configurable: true
            });
            return a;
        }

        function parseStoredKeyPath(s) {
            try { return JSON.parse(s); }
            catch (e) { return null; }
        }

        function storedKeyPath(v) {
            return JSON.stringify(v === undefined ? null : v);
        }

        function isView(v) {
            return typeof ArrayBuffer !== 'undefined' &&
                ArrayBuffer.isView && ArrayBuffer.isView(v);
        }

        function bytesOf(v) {
            var u;
            if (v instanceof ArrayBuffer) u = new Uint8Array(v);
            else if (isView(v)) u = new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
            else return null;
            var out = [];
            for (var i = 0; i < u.length; i++) out.push(u[i]);
            return out;
        }

        function canonKey(v, seen) {
            if (typeof v === 'number') {
                if (!isFinite(v)) throw ex('DataError', 'Invalid IndexedDB key');
                return { t: 'n', v: v };
            }
            if (typeof v === 'string') return { t: 's', v: v };
            if (v instanceof Date) {
                var t = v.getTime();
                if (!isFinite(t)) throw ex('DataError', 'Invalid IndexedDB key');
                return { t: 'd', v: t };
            }
            var b = bytesOf(v);
            if (b) return { t: 'b', v: b };
            if (Array.isArray(v)) {
                if (seen.indexOf(v) >= 0) throw ex('DataError', 'Invalid IndexedDB key');
                seen.push(v);
                var a = [];
                for (var i = 0; i < v.length; i++) a.push(canonKey(v[i], seen));
                seen.pop();
                return { t: 'a', v: a };
            }
            throw ex('DataError', 'Invalid IndexedDB key');
        }

        function encodeKey(v) { return JSON.stringify(canonKey(v, [])); }

        function decodeCanon(c) {
            if (!c) return undefined;
            if (c.t === 'n') return c.v;
            if (c.t === 's') return c.v;
            if (c.t === 'd') return new Date(c.v);
            if (c.t === 'b') {
                var u = new Uint8Array(c.v.length);
                for (var i = 0; i < c.v.length; i++) u[i] = c.v[i];
                return u.buffer;
            }
            if (c.t === 'a') {
                var a = [];
                for (var j = 0; j < c.v.length; j++) a.push(decodeCanon(c.v[j]));
                return a;
            }
            return undefined;
        }

        function decodeKey(s) {
            return decodeCanon(JSON.parse(s));
        }

        function typeRank(t) {
            if (t === 'n') return 1;
            if (t === 'd') return 2;
            if (t === 's') return 3;
            if (t === 'b') return 4;
            if (t === 'a') return 5;
            return 0;
        }

        function cmpCanon(a, b) {
            var ra = typeRank(a.t), rb = typeRank(b.t);
            if (ra !== rb) return ra < rb ? -1 : 1;
            if (a.t === 'n' || a.t === 'd') return a.v === b.v ? 0 : (a.v < b.v ? -1 : 1);
            if (a.t === 's') return a.v === b.v ? 0 : (a.v < b.v ? -1 : 1);
            if (a.t === 'b') {
                var n = Math.min(a.v.length, b.v.length);
                for (var i = 0; i < n; i++)
                    if (a.v[i] !== b.v[i]) return a.v[i] < b.v[i] ? -1 : 1;
                return a.v.length === b.v.length ? 0 : (a.v.length < b.v.length ? -1 : 1);
            }
            if (a.t === 'a') {
                var m = Math.min(a.v.length, b.v.length);
                for (var j = 0; j < m; j++) {
                    var c = cmpCanon(a.v[j], b.v[j]);
                    if (c) return c;
                }
                return a.v.length === b.v.length ? 0 : (a.v.length < b.v.length ? -1 : 1);
            }
            return 0;
        }

        function compareEncoded(a, b) {
            return cmpCanon(JSON.parse(a), JSON.parse(b));
        }

        function validKey(v) {
            try { encodeKey(v); return true; }
            catch (e) { return false; }
        }

        function unsafeKeyPathPart(p) {
            return p === '__proto__' || p === 'prototype' || p === 'constructor';
        }

        function keyPathGet(v, path) {
            if (path === null || path === undefined) return undefined;
            if (Array.isArray(path)) {
                var out = [];
                for (var i = 0; i < path.length; i++) out.push(keyPathGet(v, path[i]));
                return out;
            }
            if (path === '') return v;
            var cur = v;
            var parts = String(path).split('.');
            for (var j = 0; j < parts.length; j++) {
                if (unsafeKeyPathPart(parts[j])) return undefined;
                if (cur == null || !(parts[j] in Object(cur))) return undefined;
                cur = cur[parts[j]];
            }
            return cur;
        }

        function keyPathSet(v, path, key) {
            if (!path || Array.isArray(path)) return;
            var parts = String(path).split('.');
            for (var p = 0; p < parts.length; p++)
                if (unsafeKeyPathPart(parts[p])) return;
            var cur = v;
            for (var i = 0; i < parts.length - 1; i++) {
                if (cur[parts[i]] == null || typeof cur[parts[i]] !== 'object')
                    cur[parts[i]] = {};
                cur = cur[parts[i]];
            }
            cur[parts[parts.length - 1]] = key;
        }

        function inRangeEncoded(encoded, range) {
            if (!range) return true;
            if (range._lowerEncoded !== null) {
                var cl = compareEncoded(encoded, range._lowerEncoded);
                if (cl < 0 || (cl === 0 && range.lowerOpen)) return false;
            }
            if (range._upperEncoded !== null) {
                var cu = compareEncoded(encoded, range._upperEncoded);
                if (cu > 0 || (cu === 0 && range.upperOpen)) return false;
            }
            return true;
        }

        function asRange(query) {
            if (query === undefined || query === null) return null;
            if (query instanceof IDBKeyRange) return query;
            return IDBKeyRange.only(query);
        }

        function sortedRecords(records, keyName, direction) {
            records = records || [];
            records.sort(function (a, b) {
                var c = compareEncoded(a[keyName], b[keyName]);
                if (!c && a.primaryKey && b.primaryKey)
                    c = compareEncoded(a.primaryKey, b.primaryKey);
                return direction && direction.indexOf('prev') === 0 ? -c : c;
            });
            if (direction === 'nextunique' || direction === 'prevunique') {
                var out = [], last = null;
                for (var i = 0; i < records.length; i++) {
                    if (last !== records[i][keyName]) {
                        out.push(records[i]);
                        last = records[i][keyName];
                    }
                }
                records = out;
            }
            return records;
        }

        function IDBEventTarget() { this._idbListeners = {}; }
        IDBEventTarget.prototype.addEventListener = function (type, cb) {
            if (!cb) return;
            type = String(type);
            (this._idbListeners[type] || (this._idbListeners[type] = [])).push(cb);
        };
        IDBEventTarget.prototype.removeEventListener = function (type, cb) {
            var list = this._idbListeners[String(type)];
            if (!list) return;
            for (var i = list.length - 1; i >= 0; i--)
                if (list[i] === cb) list.splice(i, 1);
        };
        IDBEventTarget.prototype.dispatchEvent = function (ev) {
            if (!ev || !ev.type) return true;
            fire(this, ev.type, ev);
            return !ev.defaultPrevented;
        };

        function makeEvent(type, fields) {
            var ev;
            try { ev = new Event(type, { bubbles: false, cancelable: type === 'error' }); }
            catch (e) { ev = { type: type, defaultPrevented: false }; }
            if (fields) for (var k in fields) ev[k] = fields[k];
            if (typeof ev.preventDefault !== 'function')
                ev.preventDefault = function () { ev.defaultPrevented = true; };
            return ev;
        }

        function fire(target, type, fields) {
            var ev = fields && fields.type ? fields : makeEvent(type, fields);
            try { ev.target = target; ev.currentTarget = target; } catch (e) {}
            var handler = target['on' + type];
            if (typeof handler === 'function') handler.call(target, ev);
            var list = target._idbListeners && target._idbListeners[type];
            if (list) {
                list = list.slice();
                for (var i = 0; i < list.length; i++) {
                    if (typeof list[i] === 'function') list[i].call(target, ev);
                    else if (list[i] && typeof list[i].handleEvent === 'function')
                        list[i].handleEvent(ev);
                }
            }
            return ev;
        }

        function IDBRequest() {
            IDBEventTarget.call(this);
            this.result = undefined;
            this.error = null;
            this.source = null;
            this.transaction = null;
            this.readyState = 'pending';
            this.onsuccess = null;
            this.onerror = null;
        }
        IDBRequest.prototype = Object.create(IDBEventTarget.prototype);
        IDBRequest.prototype.constructor = IDBRequest;

        function IDBOpenDBRequest() {
            IDBRequest.call(this);
            this.onblocked = null;
            this.onupgradeneeded = null;
        }
        IDBOpenDBRequest.prototype = Object.create(IDBRequest.prototype);
        IDBOpenDBRequest.prototype.constructor = IDBOpenDBRequest;

        function succeed(req, result) {
            req.result = result;
            req.error = null;
            req.readyState = 'done';
            fire(req, 'success');
        }

        function fail(req, err) {
            req.result = undefined;
            req.error = err && err.name ? err : ex('UnknownError', String(err || 'IndexedDB error'));
            req.readyState = 'done';
            fire(req, 'error');
        }

        function IDBKeyRange(lower, upper, lowerOpen, upperOpen) {
            this.lower = lower;
            this.upper = upper;
            this.lowerOpen = !!lowerOpen;
            this.upperOpen = !!upperOpen;
            this._lowerEncoded = lower === undefined ? null : encodeKey(lower);
            this._upperEncoded = upper === undefined ? null : encodeKey(upper);
        }
        IDBKeyRange.only = function (value) { return new IDBKeyRange(value, value, false, false); };
        IDBKeyRange.lowerBound = function (lower, open) { return new IDBKeyRange(lower, undefined, open, false); };
        IDBKeyRange.upperBound = function (upper, open) { return new IDBKeyRange(undefined, upper, false, open); };
        IDBKeyRange.bound = function (lower, upper, lowerOpen, upperOpen) {
            if (cmpCanon(canonKey(lower, []), canonKey(upper, [])) > 0)
                throw ex('DataError', 'Lower bound is greater than upper bound');
            return new IDBKeyRange(lower, upper, lowerOpen, upperOpen);
        };
        IDBKeyRange.prototype.includes = function (key) {
            return inRangeEncoded(encodeKey(key), this);
        };

        function IDBRecord(key, primaryKey, value) {
            this.key = key;
            this.primaryKey = primaryKey;
            this.value = value;
        }

        function IDBDatabase(name, version, info) {
            IDBEventTarget.call(this);
            this.name = name;
            this.version = version;
            this.onabort = null;
            this.onerror = null;
            this.onclose = null;
            this.onversionchange = null;
            this._closed = false;
            this._upgradeTx = null;
            this._load(info);
        }
        IDBDatabase.prototype = Object.create(IDBEventTarget.prototype);
        IDBDatabase.prototype.constructor = IDBDatabase;
        IDBDatabase.prototype._load = function (info) {
            this._stores = {};
            var list = [];
            var stores = info && info.stores || [];
            for (var i = 0; i < stores.length; i++) {
                var s = stores[i];
                var meta = {
                    name: s.name,
                    keyPath: parseStoredKeyPath(s.keyPath),
                    autoIncrement: !!s.autoIncrement,
                    indexes: {}
                };
                var idxNames = [];
                for (var j = 0; j < (s.indexes || []).length; j++) {
                    var ix = s.indexes[j];
                    meta.indexes[ix.name] = {
                        name: ix.name,
                        keyPath: parseStoredKeyPath(ix.keyPath),
                        unique: !!ix.unique,
                        multiEntry: !!ix.multiEntry
                    };
                    idxNames.push(ix.name);
                }
                meta.indexNames = names(idxNames);
                this._stores[s.name] = meta;
                list.push(s.name);
            }
            this.objectStoreNames = names(list);
        };
        IDBDatabase.prototype._refresh = function () {
            var info = backend.info(this.name);
            this.version = info.version;
            this._load(info);
        };
        IDBDatabase.prototype.close = function () {
            this._closed = true;
            fire(this, 'close');
        };
        IDBDatabase.prototype.createObjectStore = function (name, options) {
            if (!this._upgradeTx) throw ex('InvalidStateError', 'Not in a versionchange transaction');
            name = String(name);
            options = options || {};
            var kp = options.keyPath === undefined ? null : options.keyPath;
            backend.createStore(this.name, name, storedKeyPath(kp), !!options.autoIncrement);
            this._refresh();
            if (this._upgradeTx._scope.indexOf(name) < 0) this._upgradeTx._scope.push(name);
            return this._upgradeTx.objectStore(name);
        };
        IDBDatabase.prototype.deleteObjectStore = function (name) {
            if (!this._upgradeTx) throw ex('InvalidStateError', 'Not in a versionchange transaction');
            name = String(name);
            if (!this._stores[name]) throw ex('NotFoundError', 'Object store not found');
            backend.deleteStore(this.name, name);
            this._refresh();
        };
        IDBDatabase.prototype.transaction = function (storeNames, mode, options) {
            if (this._closed) throw ex('InvalidStateError', 'Database is closed');
            if (typeof storeNames === 'string') storeNames = [storeNames];
            else storeNames = Array.prototype.slice.call(storeNames || []);
            if (!storeNames.length) throw ex('InvalidAccessError', 'Transaction scope is empty');
            for (var i = 0; i < storeNames.length; i++)
                if (!this._stores[storeNames[i]]) throw ex('NotFoundError', 'Object store not found');
            return new IDBTransaction(this, storeNames, mode || 'readonly', options || {});
        };

        function IDBTransaction(db, scope, mode, options) {
            IDBEventTarget.call(this);
            this.db = db;
            this.mode = mode || 'readonly';
            this.durability = options && options.durability || 'default';
            this.error = null;
            this.onabort = null;
            this.oncomplete = null;
            this.onerror = null;
            this.objectStoreNames = names(scope);
            this._scope = scope.slice();
            this._pending = 0;
            this._done = false;
            this._aborted = false;
            this._completeQueued = false;
        }
        IDBTransaction.prototype = Object.create(IDBEventTarget.prototype);
        IDBTransaction.prototype.constructor = IDBTransaction;
        IDBTransaction.prototype.objectStore = function (name) {
            name = String(name);
            if (this._scope.indexOf(name) < 0 || !this.db._stores[name])
                throw ex('NotFoundError', 'Object store not in transaction scope');
            return new IDBObjectStore(this, this.db._stores[name]);
        };
        IDBTransaction.prototype._request = function (req, op) {
            if (this._done || this._aborted) throw ex('TransactionInactiveError', 'Transaction is inactive');
            var tx = this;
            tx._pending++;
            task(function () {
                if (tx._aborted) {
                    fail(req, tx.error || ex('AbortError', 'Transaction aborted'));
                    tx._pending--;
                    tx._maybeComplete();
                    return;
                }
                try {
                    succeed(req, op());
                } catch (e) {
                    fail(req, e);
                    tx._abort(e);
                }
                tx._pending--;
                tx._maybeComplete();
            });
        };
        IDBTransaction.prototype._maybeComplete = function () {
            var tx = this;
            if (tx._pending !== 0 || tx._done || tx._aborted || tx._completeQueued) return;
            tx._completeQueued = true;
            task(function () {
                if (tx._pending || tx._done || tx._aborted) return;
                tx._done = true;
                fire(tx, 'complete');
            });
        };
        IDBTransaction.prototype._abort = function (err) {
            if (this._done || this._aborted) return;
            this._aborted = true;
            this.error = err && err.name ? err : ex('AbortError', 'Transaction aborted');
            fire(this, 'abort');
            fire(this.db, 'abort');
        };
        IDBTransaction.prototype.abort = function () {
            if (this._done || this._aborted) throw ex('InvalidStateError', 'Transaction already finished');
            this._abort(ex('AbortError', 'Transaction aborted'));
        };
        IDBTransaction.prototype.commit = function () { this._maybeComplete(); };

        function requestFrom(source, tx, op) {
            var req = new IDBRequest();
            req.source = source;
            req.transaction = tx || null;
            if (tx) tx._request(req, op);
            else task(function () { try { succeed(req, op()); } catch (e) { fail(req, e); } });
            return req;
        }

        function IDBObjectStore(tx, meta) {
            this.transaction = tx;
            this.name = meta.name;
            this.keyPath = meta.keyPath;
            this.autoIncrement = !!meta.autoIncrement;
            this.indexNames = meta.indexNames;
            this._meta = meta;
        }
        IDBObjectStore.prototype._writeable = function () {
            if (this.transaction.mode === 'readonly') throw ex('ReadOnlyError', 'Transaction is readonly');
        };
        IDBObjectStore.prototype._keyFor = function (value, key) {
            var inline = this.keyPath !== null && this.keyPath !== undefined;
            if (inline && key !== undefined)
                throw ex('DataError', 'Inline key stores do not accept explicit keys');
            if (inline) key = keyPathGet(value, this.keyPath);
            if (key === undefined) {
                if (!this.autoIncrement) throw ex('DataError', 'A key is required');
                key = backend.nextKey(this.transaction.db.name, this.name);
                if (inline) keyPathSet(value, this.keyPath, key);
            }
            var encoded = encodeKey(key);
            var numeric = typeof key === 'number' && isFinite(key) && key >= 1 ? Math.floor(key) : undefined;
            return { key: key, encoded: encoded, numeric: numeric };
        };
        IDBObjectStore.prototype._indexEntries = function (value) {
            var out = [];
            for (var n in this._meta.indexes) {
                var ix = this._meta.indexes[n];
                var raw = keyPathGet(value, ix.keyPath);
                if (raw === undefined) continue;
                if (ix.multiEntry && Array.isArray(raw)) {
                    var seen = {};
                    for (var i = 0; i < raw.length; i++) {
                        if (!validKey(raw[i])) continue;
                        var ek = encodeKey(raw[i]);
                        if (!seen[ek]) {
                            out.push({ name: n, key: ek });
                            seen[ek] = true;
                        }
                    }
                } else if (validKey(raw)) {
                    out.push({ name: n, key: encodeKey(raw) });
                }
            }
            return out;
        };
        IDBObjectStore.prototype._checkUnique = function (entries, primary) {
            for (var i = 0; i < entries.length; i++) {
                var ix = this._meta.indexes[entries[i].name];
                if (!ix || !ix.unique) continue;
                var rows = backend.indexRecords(this.transaction.db.name, this.name, ix.name);
                for (var j = 0; j < rows.length; j++)
                    if (rows[j].key === entries[i].key && rows[j].primaryKey !== primary)
                        throw ex('ConstraintError', 'Unique index constraint failed');
            }
        };
        IDBObjectStore.prototype._records = function (query, direction) {
            var range = asRange(query);
            var rows = backend.records(this.transaction.db.name, this.name);
            var out = [];
            for (var i = 0; i < rows.length; i++)
                if (!range || inRangeEncoded(rows[i].key, range)) out.push(rows[i]);
            return sortedRecords(out, 'key', direction || 'next');
        };
        IDBObjectStore.prototype.put = function (value, key) { return this._store(value, key, false); };
        IDBObjectStore.prototype.add = function (value, key) { return this._store(value, key, true); };
        IDBObjectStore.prototype._store = function (value, key, addOnly) {
            this._writeable();
            var os = this;
            return requestFrom(os, os.transaction, function () {
                var k = os._keyFor(value, key);
                var entries = os._indexEntries(value);
                os._checkUnique(entries, k.encoded);
                backend.put(os.transaction.db.name, os.name, k.encoded, value,
                            !!addOnly, entries, k.numeric);
                return k.key;
            });
        };
        IDBObjectStore.prototype.get = function (query) {
            var os = this;
            return requestFrom(os, os.transaction, function () {
                if (!(query instanceof IDBKeyRange)) return backend.get(os.transaction.db.name, os.name, encodeKey(query));
                var r = os._records(query, 'next');
                return r.length ? r[0].value : undefined;
            });
        };
        IDBObjectStore.prototype.getKey = function (query) {
            var os = this;
            return requestFrom(os, os.transaction, function () {
                var r = os._records(query instanceof IDBKeyRange ? query : IDBKeyRange.only(query), 'next');
                return r.length ? decodeKey(r[0].key) : undefined;
            });
        };
        IDBObjectStore.prototype.getAll = function (query, count) {
            var os = this;
            return requestFrom(os, os.transaction, function () {
                var r = os._records(query, 'next');
                if (count !== undefined) r = r.slice(0, Number(count) >>> 0);
                return r.map(function (x) { return x.value; });
            });
        };
        IDBObjectStore.prototype.getAllKeys = function (query, count) {
            var os = this;
            return requestFrom(os, os.transaction, function () {
                var r = os._records(query, 'next');
                if (count !== undefined) r = r.slice(0, Number(count) >>> 0);
                return r.map(function (x) { return decodeKey(x.key); });
            });
        };
        IDBObjectStore.prototype.getAllRecords = function (options) {
            var os = this;
            options = options || {};
            return requestFrom(os, os.transaction, function () {
                var r = os._records(options.query, options.direction || 'next');
                if (options.count !== undefined) r = r.slice(0, Number(options.count) >>> 0);
                return r.map(function (x) {
                    var k = decodeKey(x.key);
                    return new IDBRecord(k, k, x.value);
                });
            });
        };
        IDBObjectStore.prototype.count = function (query) {
            var os = this;
            return requestFrom(os, os.transaction, function () {
                return os._records(query, 'next').length;
            });
        };
        IDBObjectStore.prototype.delete = function (query) {
            this._writeable();
            var os = this;
            return requestFrom(os, os.transaction, function () {
                var r = query instanceof IDBKeyRange ? os._records(query, 'next')
                    : [{ key: encodeKey(query) }];
                for (var i = 0; i < r.length; i++)
                    backend.deleteRecord(os.transaction.db.name, os.name, r[i].key);
                return undefined;
            });
        };
        IDBObjectStore.prototype.clear = function () {
            this._writeable();
            var os = this;
            return requestFrom(os, os.transaction, function () {
                backend.clear(os.transaction.db.name, os.name);
                return undefined;
            });
        };
        IDBObjectStore.prototype.index = function (name) {
            name = String(name);
            if (!this._meta.indexes[name]) throw ex('NotFoundError', 'Index not found');
            return new IDBIndex(this, this._meta.indexes[name]);
        };
        IDBObjectStore.prototype.createIndex = function (name, keyPath, options) {
            if (this.transaction.mode !== 'versionchange')
                throw ex('InvalidStateError', 'Not in a versionchange transaction');
            name = String(name);
            options = options || {};
            backend.createIndex(this.transaction.db.name, this.name, name,
                                storedKeyPath(keyPath), !!options.unique,
                                !!options.multiEntry);
            this.transaction.db._refresh();
            this._meta = this.transaction.db._stores[this.name];
            this.indexNames = this._meta.indexNames;
            var ix = this.index(name);
            var rows = backend.records(this.transaction.db.name, this.name);
            for (var i = 0; i < rows.length; i++) {
                var entries = this._indexEntries(rows[i].value);
                backend.put(this.transaction.db.name, this.name, rows[i].key,
                            rows[i].value, false, entries, undefined);
            }
            return ix;
        };
        IDBObjectStore.prototype.deleteIndex = function (name) {
            if (this.transaction.mode !== 'versionchange')
                throw ex('InvalidStateError', 'Not in a versionchange transaction');
            backend.deleteIndex(this.transaction.db.name, this.name, String(name));
            this.transaction.db._refresh();
            this._meta = this.transaction.db._stores[this.name];
            this.indexNames = this._meta.indexNames;
        };
        IDBObjectStore.prototype.openCursor = function (query, direction) {
            return cursorRequest(this, this._records(query, direction || 'next'), false, direction || 'next');
        };
        IDBObjectStore.prototype.openKeyCursor = function (query, direction) {
            return cursorRequest(this, this._records(query, direction || 'next'), true, direction || 'next');
        };

        function IDBIndex(store, meta) {
            this.objectStore = store;
            this.name = meta.name;
            this.keyPath = meta.keyPath;
            this.multiEntry = !!meta.multiEntry;
            this.unique = !!meta.unique;
            this._meta = meta;
        }
        IDBIndex.prototype._records = function (query, direction) {
            var range = asRange(query);
            var rows = backend.indexRecords(this.objectStore.transaction.db.name,
                                            this.objectStore.name, this.name);
            var out = [];
            for (var i = 0; i < rows.length; i++)
                if (!range || inRangeEncoded(rows[i].key, range)) out.push(rows[i]);
            return sortedRecords(out, 'key', direction || 'next');
        };
        IDBIndex.prototype.get = function (query) {
            var ix = this;
            return requestFrom(ix, ix.objectStore.transaction, function () {
                var r = ix._records(query, 'next');
                return r.length ? r[0].value : undefined;
            });
        };
        IDBIndex.prototype.getKey = function (query) {
            var ix = this;
            return requestFrom(ix, ix.objectStore.transaction, function () {
                var r = ix._records(query, 'next');
                return r.length ? decodeKey(r[0].primaryKey) : undefined;
            });
        };
        IDBIndex.prototype.getAll = function (query, count) {
            var ix = this;
            return requestFrom(ix, ix.objectStore.transaction, function () {
                var r = ix._records(query, 'next');
                if (count !== undefined) r = r.slice(0, Number(count) >>> 0);
                return r.map(function (x) { return x.value; });
            });
        };
        IDBIndex.prototype.getAllKeys = function (query, count) {
            var ix = this;
            return requestFrom(ix, ix.objectStore.transaction, function () {
                var r = ix._records(query, 'next');
                if (count !== undefined) r = r.slice(0, Number(count) >>> 0);
                return r.map(function (x) { return decodeKey(x.primaryKey); });
            });
        };
        IDBIndex.prototype.getAllRecords = function (options) {
            var ix = this;
            options = options || {};
            return requestFrom(ix, ix.objectStore.transaction, function () {
                var r = ix._records(options.query, options.direction || 'next');
                if (options.count !== undefined) r = r.slice(0, Number(options.count) >>> 0);
                return r.map(function (x) {
                    return new IDBRecord(decodeKey(x.key), decodeKey(x.primaryKey), x.value);
                });
            });
        };
        IDBIndex.prototype.count = function (query) {
            var ix = this;
            return requestFrom(ix, ix.objectStore.transaction, function () {
                return ix._records(query, 'next').length;
            });
        };
        IDBIndex.prototype.openCursor = function (query, direction) {
            return cursorRequest(this, this._records(query, direction || 'next'), false, direction || 'next');
        };
        IDBIndex.prototype.openKeyCursor = function (query, direction) {
            return cursorRequest(this, this._records(query, direction || 'next'), true, direction || 'next');
        };

        function IDBCursor(source, records, keyOnly, direction, request) {
            this.source = source;
            this.direction = direction || 'next';
            this.request = request;
            this._records = records;
            this._keyOnly = !!keyOnly;
            this._pos = 0;
            this._apply();
        }
        IDBCursor.prototype._apply = function () {
            var r = this._records[this._pos];
            if (!r) return false;
            this.key = decodeKey(r.key);
            this.primaryKey = decodeKey(r.primaryKey || r.key);
            if (!this._keyOnly) this.value = r.value;
            else delete this.value;
            return true;
        };
        IDBCursor.prototype._deliver = function () {
            var c = this._apply() ? this : null;
            succeed(this.request, c);
        };
        IDBCursor.prototype._schedule = function () {
            var cur = this;
            var tx = cur.request.transaction;
            cur.request.readyState = 'pending';
            if (tx) tx._pending++;
            task(function () {
                try {
                    if (tx && tx._aborted)
                        fail(cur.request, tx.error || ex('AbortError', 'Transaction aborted'));
                    else
                        cur._deliver();
                } finally {
                    if (tx) {
                        tx._pending--;
                        tx._maybeComplete();
                    }
                }
            });
        };
        IDBCursor.prototype.continue = function (key) {
            var cur = this;
            if (key !== undefined) {
                var target = encodeKey(key);
                while (cur._pos < cur._records.length &&
                       compareEncoded(cur._records[cur._pos].key, target) <= 0)
                    cur._pos++;
            } else {
                cur._pos++;
            }
            cur._schedule();
        };
        IDBCursor.prototype.continuePrimaryKey = function (key, primaryKey) {
            var target = encodeKey(key);
            var primary = encodeKey(primaryKey);
            while (this._pos < this._records.length) {
                this._pos++;
                var r = this._records[this._pos];
                if (!r) break;
                if (compareEncoded(r.key, target) > 0 ||
                    (r.key === target && compareEncoded(r.primaryKey || r.key, primary) > 0))
                    break;
            }
            this._schedule();
        };
        IDBCursor.prototype.advance = function (count) {
            count = Number(count) >>> 0;
            if (!count) throw ex('TypeError', 'advance count must be positive');
            this._pos += count;
            this._schedule();
        };
        IDBCursor.prototype.update = function (value) {
            var store = this.source instanceof IDBIndex ? this.source.objectStore : this.source;
            return store.put(value, this.primaryKey);
        };
        IDBCursor.prototype.delete = function () {
            var store = this.source instanceof IDBIndex ? this.source.objectStore : this.source;
            return store.delete(this.primaryKey);
        };

        function IDBCursorWithValue(source, records, direction, request) {
            IDBCursor.call(this, source, records, false, direction, request);
        }
        IDBCursorWithValue.prototype = Object.create(IDBCursor.prototype);
        IDBCursorWithValue.prototype.constructor = IDBCursorWithValue;

        function cursorRequest(source, records, keyOnly, direction) {
            var tx = source instanceof IDBIndex ? source.objectStore.transaction : source.transaction;
            var req = new IDBRequest();
            req.source = source;
            req.transaction = tx;
            tx._request(req, function () {
                if (!records.length) return null;
                return keyOnly ? new IDBCursor(source, records, true, direction, req)
                               : new IDBCursorWithValue(source, records, direction, req);
            });
            return req;
        }

        function IDBVersionChangeEvent(type, init) {
            var ev = makeEvent(type, init || {});
            ev.oldVersion = init && init.oldVersion || 0;
            ev.newVersion = init && init.newVersion === undefined ? null : init && init.newVersion;
            return ev;
        }

        function IDBFactory() {}
        IDBFactory.prototype.cmp = function (first, second) {
            return cmpCanon(canonKey(first, []), canonKey(second, []));
        };
        IDBFactory.prototype.open = function (name, version) {
            name = String(name);
            if (version !== undefined) {
                version = Number(version);
                if (!isFinite(version) || version <= 0 || Math.floor(version) !== version)
                    throw ex('TypeError', 'Invalid IndexedDB version');
            }
            var req = new IDBOpenDBRequest();
            task(function () {
                try {
                    var info = backend.open(name);
                    var oldVersion = Number(info.version || 0);
                    var wanted = version === undefined ? (oldVersion || 1) : version;
                    if (wanted < oldVersion) throw ex('VersionError', 'Requested version is lower than current version');
                    var db = new IDBDatabase(name, oldVersion || wanted, info);
                    if (wanted > oldVersion) {
                        var tx = new IDBTransaction(db, db.objectStoreNames, 'versionchange', {});
                        db._upgradeTx = tx;
                        req.result = db;
                        req.transaction = tx;
                        req.readyState = 'done';
                        fire(req, 'upgradeneeded', new IDBVersionChangeEvent('upgradeneeded', {
                            oldVersion: oldVersion,
                            newVersion: wanted
                        }));
                        backend.setVersion(name, wanted);
                        db._refresh();
                        db.version = wanted;
                        db._upgradeTx = null;
                        tx._maybeComplete();
                    }
                    succeed(req, db);
                } catch (e) {
                    fail(req, e);
                }
            });
            return req;
        };
        IDBFactory.prototype.deleteDatabase = function (name) {
            name = String(name);
            var req = new IDBOpenDBRequest();
            task(function () {
                try {
                    backend.deleteDatabase(name);
                    succeed(req, undefined);
                } catch (e) {
                    fail(req, e);
                }
            });
            return req;
        };
        IDBFactory.prototype.databases = function () {
            return new Promise(function (resolve, reject) {
                task(function () {
                    try { resolve(backend.databases()); }
                    catch (e) { reject(e); }
                });
            });
        };

        defineCtor('IDBRequest', IDBRequest);
        defineCtor('IDBOpenDBRequest', IDBOpenDBRequest);
        defineCtor('IDBFactory', IDBFactory);
        defineCtor('IDBDatabase', IDBDatabase);
        defineCtor('IDBTransaction', IDBTransaction);
        defineCtor('IDBObjectStore', IDBObjectStore);
        defineCtor('IDBIndex', IDBIndex);
        defineCtor('IDBKeyRange', IDBKeyRange);
        defineCtor('IDBCursor', IDBCursor);
        defineCtor('IDBCursorWithValue', IDBCursorWithValue);
        defineCtor('IDBRecord', IDBRecord);
        defineCtor('IDBVersionChangeEvent', IDBVersionChangeEvent);
        defineCtor('indexedDB', new IDBFactory());
    })();

    if (typeof Symbol !== 'undefined') {
        if (typeof Symbol.dispose === 'undefined') {
            try { Symbol.dispose = Symbol('Symbol.dispose'); } catch (e) {}
        }
        if (typeof Symbol.asyncDispose === 'undefined') {
            try { Symbol.asyncDispose = Symbol('Symbol.asyncDispose'); } catch (e) {}
        }
    }

    if (typeof DOMException !== 'function') {
        var DOM_EXCEPTION_CODES = {
            IndexSizeError:               1,
            HierarchyRequestError:        3,
            WrongDocumentError:           4,
            InvalidCharacterError:        5,
            NoModificationAllowedError:   7,
            NotFoundError:                8,
            NotSupportedError:            9,
            InUseAttributeError:         10,
            InvalidStateError:           11,
            SyntaxError:                 12,
            InvalidModificationError:    13,
            NamespaceError:              14,
            InvalidAccessError:          15,
            SecurityError:               18,
            NetworkError:                19,
            AbortError:                  20,
            URLMismatchError:            21,
            QuotaExceededError:          22,
            TimeoutError:                23,
            InvalidNodeTypeError:        24,
            DataCloneError:              25
        };
        var DomException = function (message, name) {
            if (!(this instanceof DomException)) return new DomException(message, name);
            var err = new Error(String(message == null ? '' : message));
            err.name = String(name == null ? 'Error' : name);
            err.code = DOM_EXCEPTION_CODES[err.name] || 0;
            Object.setPrototypeOf(err, DomException.prototype);
            return err;
        };
        DomException.prototype = Object.create(Error.prototype);
        DomException.prototype.constructor = DomException;
        for (var domExName in DOM_EXCEPTION_CODES) {
            if (Object.prototype.hasOwnProperty.call(DOM_EXCEPTION_CODES, domExName)) {
                try {
                    Object.defineProperty(DomException, domExName + '_CODE', {
                        value: DOM_EXCEPTION_CODES[domExName],
                        writable: false, enumerable: true, configurable: false
                    });
                } catch (e) {}
            }
        }
        defineCtor('DOMException', DomException);
    }

    if (typeof QuotaExceededError !== 'function') {
        var QuotaErr = function (message, options) {
            if (!(this instanceof QuotaErr)) return new QuotaErr(message, options);
            var err = new Error(String(message == null ? '' : message));
            err.name = 'QuotaExceededError';
            err.code = 22;
            err.requested = options && options.requested != null ? options.requested : null;
            err.quota = options && options.quota != null ? options.quota : null;
            Object.setPrototypeOf(err, QuotaErr.prototype);
            return err;
        };
        QuotaErr.prototype = Object.create(
            typeof DOMException === 'function' ? DOMException.prototype : Error.prototype);
        QuotaErr.prototype.constructor = QuotaErr;
        defineCtor('QuotaExceededError', QuotaErr);
    }


    if (typeof Event !== 'undefined' && Event.prototype &&
        typeof Event.prototype.composedPath !== 'function') {
        defineMethod(Event.prototype, 'composedPath', function () {
            var path = [];
            var node = this.target || this.currentTarget;
            while (node) {
                path.push(node);
                node = node.parentNode || null;
            }
            return path;
        });
    }

    var navigator = global.navigator;
    if (navigator && !navigator.locks) {
        try {
            Object.defineProperty(navigator, 'locks', {
                configurable: true, enumerable: true,
                value: {
                    request: function (name, options, callback) {
                        if (typeof options === 'function') {
                            callback = options;
                            options = undefined;
                        }
                        var lock = { name: String(name), mode: (options && options.mode) || 'exclusive' };
                        if (typeof callback !== 'function')
                            return Promise.resolve();
                        try { return Promise.resolve(callback(lock)); }
                        catch (e) { return Promise.reject(e); }
                    },
                    query: function () {
                        return Promise.resolve({ held: [], pending: [] });
                    }
                }
            });
        } catch (e) {}
    }

    if (navigator && !navigator.xr) {
        try {
            Object.defineProperty(navigator, 'xr', {
                configurable: true, enumerable: true,
                value: {
                    isSessionSupported: function () { return Promise.resolve(false); },
                    requestSession: function () {
                        var err = new Error('WebXR sessions are not supported');
                        err.name = 'NotSupportedError';
                        return Promise.reject(err);
                    },
                    addEventListener: function () {},
                    removeEventListener: function () {},
                    dispatchEvent: function () { return false; }
                }
            });
        } catch (e) {}
    }

    if (navigator && typeof navigator.share !== 'function') {
        try {
            Object.defineProperty(navigator, 'share', {
                configurable: true, enumerable: true,
                value: nativeize(function share() {
                    var err = new Error('Web Share API not supported');
                    err.name = 'NotSupportedError';
                    return Promise.reject(err);
                })
            });
            Object.defineProperty(navigator, 'canShare', {
                configurable: true, enumerable: true,
                value: nativeize(function canShare() { return false; })
            });
        } catch (e) {}
    }

    if (navigator) {
        try {
            Object.defineProperty(navigator, 'canShare', {
                configurable: true, enumerable: true,
                value: nativeize(function canShare() { return false; })
            });
        } catch (e) {}
        try {
            Object.defineProperty(navigator, 'vibrate', {
                configurable: true, enumerable: true,
                value: nativeize(function vibrate(pattern) {
                    var list = Array.isArray(pattern) ? pattern : [pattern];
                    for (var i = 0; i < list.length; i++) {
                        var v = Number(list[i]);
                        if (!isFinite(v) || v < 0) return false;
                    }
                    return true;
                })
            });
        } catch (e) {}
        try {
            Object.defineProperty(navigator, 'getAutoplayPolicy', {
                configurable: true, enumerable: true,
                value: nativeize(function getAutoplayPolicy() { return 'allowed'; })
            });
        } catch (e) {}
        if (navigator.mediaDevices) {
            try {
                Object.defineProperty(navigator.mediaDevices, 'getSupportedConstraints', {
                    configurable: true, enumerable: true,
                    value: nativeize(function getSupportedConstraints() {
                        return {
                            width: true, height: true, aspectRatio: true,
                            frameRate: true, facingMode: true, resizeMode: true,
                            sampleRate: true, sampleSize: true, channelCount: true,
                            echoCancellation: true, noiseSuppression: true,
                            autoGainControl: true, deviceId: true, groupId: true
                        };
                    })
                });
            } catch (e) {}

            (function () {
                var md = navigator.mediaDevices;
                var pending = [];

                function makeError(name, msg) {
                    var e;
                    try { e = new DOMException(msg, name); }
                    catch (_) { e = new Error(msg); e.name = name; }
                    return e;
                }
                function rnd() { return Math.random().toString(36).slice(2); }

                function makeTrack(kind, label) {
                    var L = {};
                    var track = {
                        kind: kind,
                        id: 'track-' + kind + '-' + rnd(),
                        label: label || (kind === 'video' ? 'Camera' : 'Microphone'),
                        enabled: true, muted: false, readyState: 'live', contentHint: '',
                        onended: null, onmute: null, onunmute: null,
                        getSettings: function () {
                            return kind === 'video'
                                ? { deviceId: 'default', groupId: 'default',
                                    width: 640, height: 480, frameRate: 30, facingMode: 'user' }
                                : { deviceId: 'default', groupId: 'default',
                                    sampleRate: 48000, channelCount: 1 };
                        },
                        getCapabilities: function () { return {}; },
                        getConstraints: function () { return {}; },
                        applyConstraints: function () { return Promise.resolve(); },
                        clone: function () { return makeTrack(kind, label); },
                        addEventListener: function (t, f) {
                            if (typeof f === 'function') (L[t] = L[t] || []).push(f);
                        },
                        removeEventListener: function (t, f) {
                            var a = L[t]; if (!a) return;
                            var i = a.indexOf(f); if (i >= 0) a.splice(i, 1);
                        },
                        dispatchEvent: function (e) {
                            var a = e && L[e.type];
                            if (a) a.slice().forEach(function (fn) {
                                try { fn.call(track, e); } catch (_) {}
                            });
                            var h = e && track['on' + e.type];
                            if (typeof h === 'function') { try { h.call(track, e); } catch (_) {} }
                            return true;
                        },
                        stop: function () {
                            if (track.readyState === 'ended') return;
                            track.readyState = 'ended';
                            if (kind === 'video' &&
                                typeof globalThis.__nd_camera_release === 'function')
                                globalThis.__nd_camera_release();
                            if (kind === 'audio' &&
                                typeof globalThis.__nd_mic_release === 'function')
                                globalThis.__nd_mic_release();
                            track.dispatchEvent({ type: 'ended' });
                        }
                    };
                    return track;
                }

                function makeStream(tracks) {
                    var L = {};
                    var stream = {
                        id: 'stream-' + rnd(),
                        active: true, _nd_camera: true,
                        onaddtrack: null, onremovetrack: null,
                        getTracks: function () { return tracks.slice(); },
                        getVideoTracks: function () {
                            return tracks.filter(function (t) { return t.kind === 'video'; });
                        },
                        getAudioTracks: function () {
                            return tracks.filter(function (t) { return t.kind === 'audio'; });
                        },
                        getTrackById: function (id) {
                            for (var i = 0; i < tracks.length; i++)
                                if (tracks[i].id === id) return tracks[i];
                            return null;
                        },
                        addTrack: function (t) { if (tracks.indexOf(t) < 0) tracks.push(t); },
                        removeTrack: function (t) {
                            var i = tracks.indexOf(t); if (i >= 0) tracks.splice(i, 1);
                        },
                        clone: function () {
                            return makeStream(tracks.map(function (t) { return t.clone(); }));
                        },
                        addEventListener: function (t, f) {
                            if (typeof f === 'function') (L[t] = L[t] || []).push(f);
                        },
                        removeEventListener: function (t, f) {
                            var a = L[t]; if (!a) return;
                            var i = a.indexOf(f); if (i >= 0) a.splice(i, 1);
                        },
                        dispatchEvent: function (e) {
                            var a = e && L[e.type];
                            if (a) a.slice().forEach(function (fn) {
                                try { fn.call(stream, e); } catch (_) {}
                            });
                            return true;
                        }
                    };
                    return stream;
                }

                function build(wantVideo, wantAudio) {
                    var tracks = [];
                    if (wantVideo)
                        tracks.push(makeTrack('video',
                            typeof globalThis.__nd_camera_label === 'function'
                                ? globalThis.__nd_camera_label() : 'Camera'));
                    if (wantAudio) tracks.push(makeTrack('audio', 'Microphone'));
                    return makeStream(tracks);
                }

                md.getUserMedia = nativeize(function getUserMedia(constraints) {
                    constraints = constraints || {};
                    var wantVideo = !!constraints.video;
                    var wantAudio = !!constraints.audio;
                    return new Promise(function (resolve, reject) {
                        if (!wantVideo && !wantAudio) {
                            reject(new TypeError('getUserMedia: no media requested'));
                            return;
                        }
                        var decision = typeof globalThis.__nd_camera_request === 'function'
                            ? globalThis.__nd_camera_request(wantVideo, wantAudio) : 'denied';
                        if (decision === 'granted')
                            resolve(build(wantVideo, wantAudio));
                        else if (decision === 'denied')
                            reject(makeError('NotAllowedError', 'Permission denied'));
                        else
                            pending.push({ wantVideo: wantVideo, wantAudio: wantAudio,
                                           resolve: resolve, reject: reject });
                    });
                });

                md.enumerateDevices = nativeize(function enumerateDevices() {
                    return new Promise(function (resolve) {
                        var list = typeof globalThis.__nd_camera_enumerate === 'function'
                            ? globalThis.__nd_camera_enumerate() : [];
                        resolve((list || []).map(function (d) {
                            return {
                                deviceId: d.deviceId, groupId: d.groupId,
                                kind: d.kind, label: d.label,
                                toJSON: function () { return this; }
                            };
                        }));
                    });
                });

                globalThis.__nd_camera_resolve_pending = function (allow) {
                    var q = pending; pending = [];
                    q.forEach(function (p) {
                        if (!allow) {
                            p.reject(makeError('NotAllowedError', 'Permission denied'));
                            return;
                        }
                        var d = typeof globalThis.__nd_camera_request === 'function'
                            ? globalThis.__nd_camera_request(p.wantVideo, p.wantAudio)
                            : 'denied';
                        if (d === 'denied')
                            p.reject(makeError('NotAllowedError', 'Permission denied'));
                        else
                            p.resolve(build(p.wantVideo, p.wantAudio));
                    });
                };
            })();
        }
        if (navigator.userAgentData) {
            try {
                Object.defineProperty(navigator.userAgentData, 'toJSON', {
                    configurable: true, enumerable: false,
                    value: function () {
                        return {
                            brands: this.brands || [],
                            mobile: !!this.mobile,
                            platform: String(this.platform || '')
                        };
                    }
                });
            } catch (e) {}
        }
        if (!navigator.credentials) {
            try {
                Object.defineProperty(navigator, 'credentials', {
                    configurable: true, enumerable: true,
                    value: {
                        get: function () { return Promise.resolve(null); },
                        create: function () { return Promise.resolve(null); },
                        store: function (credential) { return Promise.resolve(credential || null); },
                        preventSilentAccess: function () { return Promise.resolve(); }
                    }
                });
            } catch (e) {}
        }
    }

    var storageAccessTarget = typeof document !== 'undefined'
        ? document
        : global.Document && global.Document.prototype;
    if (storageAccessTarget) {
        try {
            Object.defineProperty(storageAccessTarget, 'hasStorageAccess', {
                configurable: true, enumerable: true,
                value: function () { return Promise.resolve(true); }
            });
        } catch (e) {}
        try {
            Object.defineProperty(storageAccessTarget, 'requestStorageAccess', {
                configurable: true, enumerable: true,
                value: function () { return Promise.resolve(); }
            });
        } catch (e) {}
        try {
            Object.defineProperty(storageAccessTarget, 'requestStorageAccessFor', {
                configurable: true, enumerable: true,
                value: function () { return Promise.resolve(); }
            });
        } catch (e) {}
    }

    if (!global.cookieStore) {
        var cookiePairFor = function (name) {
            var key = String(name || '');
            var parts = String(document.cookie || '').split(/;\s*/);
            for (var i = 0; i < parts.length; i++) {
                var eq = parts[i].indexOf('=');
                var n = eq >= 0 ? parts[i].slice(0, eq) : parts[i];
                if (n === key) {
                    return {
                        name: n,
                        value: eq >= 0 ? parts[i].slice(eq + 1) : '',
                        domain: '',
                        path: '/',
                        expires: null,
                        secure: false,
                        sameSite: 'lax'
                    };
                }
            }
            return null;
        };
        try {
            Object.defineProperty(global, 'cookieStore', {
                configurable: true, enumerable: true,
                value: {
                    get: function (name) {
                        if (name && typeof name === 'object') name = name.name;
                        return Promise.resolve(cookiePairFor(name));
                    },
                    getAll: function (query) {
                        var parts = String(document.cookie || '').split(/;\s*/);
                        var out = [];
                        var wanted = query && typeof query === 'object' ? query.name : query;
                        for (var i = 0; i < parts.length; i++) {
                            if (!parts[i]) continue;
                            var eq = parts[i].indexOf('=');
                            var name = eq >= 0 ? parts[i].slice(0, eq) : parts[i];
                            if (wanted && name !== String(wanted)) continue;
                            var item = cookiePairFor(name);
                            if (item) out.push(item);
                        }
                        return Promise.resolve(out);
                    },
                    set: function (name, value) {
                        if (name && typeof name === 'object') {
                            value = name.value;
                            name = name.name;
                        }
                        document.cookie = String(name || '') + '=' + String(value == null ? '' : value);
                        return Promise.resolve();
                    },
                    delete: function (name) {
                        if (name && typeof name === 'object') name = name.name;
                        document.cookie = String(name || '') + '=; Max-Age=0';
                        return Promise.resolve();
                    },
                    addEventListener: function () {},
                    removeEventListener: function () {},
                    dispatchEvent: function () { return true; }
                }
            });
        } catch (e) {}
        try {
            var cookieStoreProto = global.CookieStore && global.CookieStore.prototype;
            if (cookieStoreProto) {
                ['get', 'getAll', 'set', 'delete', 'addEventListener',
                 'removeEventListener', 'dispatchEvent'].forEach(function (name) {
                    if (typeof cookieStoreProto[name] !== 'function') {
                        Object.defineProperty(cookieStoreProto, name, {
                            configurable: true, writable: true,
                            value: global.cookieStore[name]
                        });
                    }
                });
            }
        } catch (e) {}
    }

    if (typeof global.Credential !== 'function') {
        try {
            defineCtor('Credential', function (init) {
                init = init || {};
                this.id = String(init.id || '');
                this.type = String(init.type || '');
            });
        } catch (e) {}
    }

    if (typeof global.PasswordCredential !== 'function') {
        try {
            defineCtor('PasswordCredential', function (init) {
                init = init || {};
                this.id = String(init.id || init.name || '');
                this.name = String(init.name || init.id || '');
                this.type = 'password';
                this.password = String(init.password || '');
            });
        } catch (e) {}
    }

    if (typeof global.FederatedCredential !== 'function') {
        try {
            defineCtor('FederatedCredential', function (init) {
                init = init || {};
                this.id = String(init.id || '');
                this.name = String(init.name || '');
                this.type = 'federated';
                this.provider = String(init.provider || '');
                this.protocol = String(init.protocol || '');
            });
        } catch (e) {}
    }

    if (typeof global.PublicKeyCredential !== 'function') {
        try {
            defineCtor('PublicKeyCredential', function () {
                this.id = '';
                this.rawId = new ArrayBuffer(0);
                this.type = 'public-key';
                this.response = {};
            });
            global.PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable =
                function () { return Promise.resolve(false); };
            global.PublicKeyCredential.isConditionalMediationAvailable =
                function () { return Promise.resolve(false); };
            global.PublicKeyCredential.parseCreationOptionsFromJSON =
                function (options) { return options || {}; };
            global.PublicKeyCredential.parseRequestOptionsFromJSON =
                function (options) { return options || {}; };
        } catch (e) {}
    }

    if (typeof global.IdentityCredential !== 'function') {
        try {
            defineCtor('IdentityCredential', function (init) {
                init = init || {};
                this.token = String(init.token || '');
                this.isAutoSelected = !!init.isAutoSelected;
            });
        } catch (e) {}
    }

    if (typeof global.Notification === 'function') {
        try {
            Object.defineProperty(global.Notification, 'permission', {
                configurable: true, enumerable: true,
                value: 'default'
            });
            Object.defineProperty(global.Notification, 'requestPermission', {
                configurable: true, enumerable: true,
                value: nativeize(function requestPermission(callback) {
                    if (typeof callback === 'function') callback('default');
                    return Promise.resolve('default');
                })
            });
        } catch (e) {}
    }

    if (typeof global.MediaMetadata !== 'function') {
        try {
            defineCtor('MediaMetadata', function (init) {
                init = init || {};
                this.title = String(init.title || '');
                this.artist = String(init.artist || '');
                this.album = String(init.album || '');
                this.artwork = Array.isArray(init.artwork) ? init.artwork.slice() : [];
            });
        } catch (e) {}
    }

    if (navigator && !navigator.mediaSession) {
        try {
            Object.defineProperty(navigator, 'mediaSession', {
                configurable: true, enumerable: true,
                value: {
                    metadata: null,
                    playbackState: 'none',
                    setActionHandler: function (action, handler) {
                        this['_handler_' + String(action)] =
                            typeof handler === 'function' ? handler : null;
                    },
                    setPositionState: function (state) {
                        this._positionState = state || null;
                    }
                }
            });
        } catch (e) {}
    }

    var mediaProto = global.HTMLMediaElement && global.HTMLMediaElement.prototype;
    if (mediaProto) {
        try {
            Object.defineProperty(mediaProto, 'requestPictureInPicture', {
                configurable: true, enumerable: true,
                value: function () {
                    if (typeof document !== 'undefined') {
                        try {
                            Object.defineProperty(document, 'pictureInPictureElement', {
                                configurable: true,
                                value: this
                            });
                        } catch (e) {}
                    }
                    return Promise.resolve(this);
                }
            });
        } catch (e) {}
        try {
            Object.defineProperty(mediaProto, 'disablePictureInPicture', {
                configurable: true, enumerable: true, writable: true,
                value: false
            });
        } catch (e) {}
        try {
            Object.defineProperty(mediaProto, 'webkitSupportsFullscreen', {
                configurable: true, enumerable: true,
                value: false
            });
            Object.defineProperty(mediaProto, 'webkitDisplayingFullscreen', {
                configurable: true, enumerable: true,
                value: false
            });
            Object.defineProperty(mediaProto, 'webkitPresentationMode', {
                configurable: true, enumerable: true,
                value: 'inline'
            });
            Object.defineProperty(mediaProto, 'webkitEnterFullscreen', {
                configurable: true, enumerable: true,
                value: function () {}
            });
            Object.defineProperty(mediaProto, 'webkitExitFullscreen', {
                configurable: true, enumerable: true,
                value: function () {}
            });
            Object.defineProperty(mediaProto, 'webkitSetPresentationMode', {
                configurable: true, enumerable: true,
                value: function () {}
            });
        } catch (e) {}
        if (!('remote' in mediaProto)) {
            try {
                Object.defineProperty(mediaProto, 'remote', {
                    configurable: true, enumerable: true,
                    get: function () {
                        if (!this.__nd_remotePlayback) {
                            Object.defineProperty(this, '__nd_remotePlayback', {
                                configurable: true,
                                value: {
                                    state: 'disconnected',
                                    onconnect: null,
                                    onconnecting: null,
                                    ondisconnect: null,
                                    prompt: function () { return Promise.resolve(); },
                                    watchAvailability: function (callback) {
                                        if (typeof callback === 'function') {
                                            try { callback(false); } catch (e) {}
                                        }
                                        return Promise.resolve(1);
                                    },
                                    cancelWatchAvailability: function () { return Promise.resolve(); },
                                    addEventListener: function () {},
                                    removeEventListener: function () {},
                                    dispatchEvent: function () { return true; }
                                }
                            });
                        }
                        return this.__nd_remotePlayback;
                    }
                });
            } catch (e) {}
        }
        try {
            Object.defineProperty(mediaProto, 'disableRemotePlayback', {
                configurable: true, enumerable: true, writable: true,
                value: false
            });
        } catch (e) {}
    }

    var actualMediaProto = global.Element && global.Element.prototype;
    if (actualMediaProto && actualMediaProto !== mediaProto) {
        try {
            Object.defineProperty(actualMediaProto, 'requestPictureInPicture', {
                configurable: true, enumerable: true,
                value: function () {
                    if (typeof document !== 'undefined') {
                        try {
                            Object.defineProperty(document, 'pictureInPictureElement', {
                                configurable: true,
                                value: this
                            });
                        } catch (e) {}
                    }
                    return Promise.resolve(this);
                }
            });
        } catch (e) {}
        try {
            Object.defineProperty(actualMediaProto, 'disablePictureInPicture', {
                configurable: true, enumerable: true, writable: true,
                value: false
            });
        } catch (e) {}
        try {
            Object.defineProperty(actualMediaProto, 'webkitSupportsFullscreen', {
                configurable: true, enumerable: true,
                value: false
            });
            Object.defineProperty(actualMediaProto, 'webkitDisplayingFullscreen', {
                configurable: true, enumerable: true,
                value: false
            });
            Object.defineProperty(actualMediaProto, 'webkitPresentationMode', {
                configurable: true, enumerable: true,
                value: 'inline'
            });
            Object.defineProperty(actualMediaProto, 'webkitEnterFullscreen', {
                configurable: true, enumerable: true,
                value: function () {}
            });
            Object.defineProperty(actualMediaProto, 'webkitExitFullscreen', {
                configurable: true, enumerable: true,
                value: function () {}
            });
            Object.defineProperty(actualMediaProto, 'webkitSetPresentationMode', {
                configurable: true, enumerable: true,
                value: function () {}
            });
        } catch (e) {}
        if (!('remote' in actualMediaProto)) {
            try {
                Object.defineProperty(actualMediaProto, 'remote', {
                    configurable: true, enumerable: true,
                    get: function () {
                        if (!this.__nd_remotePlayback) {
                            Object.defineProperty(this, '__nd_remotePlayback', {
                                configurable: true,
                                value: {
                                    state: 'disconnected',
                                    onconnect: null,
                                    onconnecting: null,
                                    ondisconnect: null,
                                    prompt: function () { return Promise.resolve(); },
                                    watchAvailability: function (callback) {
                                        if (typeof callback === 'function') {
                                            try { callback(false); } catch (e) {}
                                        }
                                        return Promise.resolve(1);
                                    },
                                    cancelWatchAvailability: function () { return Promise.resolve(); },
                                    addEventListener: function () {},
                                    removeEventListener: function () {},
                                    dispatchEvent: function () { return true; }
                                }
                            });
                        }
                        return this.__nd_remotePlayback;
                    }
                });
            } catch (e) {}
        }
        try {
            Object.defineProperty(actualMediaProto, 'disableRemotePlayback', {
                configurable: true, enumerable: true, writable: true,
                value: false
            });
        } catch (e) {}
    }

    if (typeof document !== 'undefined') {
        try {
            Object.defineProperty(document, 'pictureInPictureEnabled', {
                configurable: true, enumerable: true,
                value: true
            });
            Object.defineProperty(document, 'exitPictureInPicture', {
                configurable: true, enumerable: true,
                value: function () {
                    try {
                        Object.defineProperty(document, 'pictureInPictureElement', {
                            configurable: true,
                            value: null
                        });
                    } catch (e) {}
                    return Promise.resolve();
                }
            });
        } catch (e) {}
    }

    if (typeof global.NavigationHistoryEntry !== 'function') {
        try {
            defineCtor('NavigationHistoryEntry', function () {
                this.id = '0';
                this.key = '0';
                this.index = 0;
                this.url = String(global.location && global.location.href || '');
                this.sameDocument = true;
            });
            global.NavigationHistoryEntry.prototype.getState = function () { return null; };
        } catch (e) {}
    }

    if (typeof global.Navigation !== 'function') {
        try {
            defineCtor('Navigation', function () {});
        } catch (e) {}
    }

    if (!global.navigation) {
        try {
            var makeNavEntry = function () {
                return {
                    id: '0',
                    key: '0',
                    index: 0,
                    url: String(global.location && global.location.href || ''),
                    sameDocument: true,
                    getState: function () { return null; }
                };
            };
            var makeNavResult = function () {
                var done = Promise.resolve(makeNavEntry());
                return { committed: done, finished: done };
            };
            Object.defineProperty(global, 'navigation', {
                configurable: true, enumerable: true,
                value: {
                    currentEntry: makeNavEntry(),
                    transition: null,
                    activation: null,
                    canGoBack: false,
                    canGoForward: false,
                    onnavigate: null,
                    onnavigatesuccess: null,
                    onnavigateerror: null,
                    oncurrententrychange: null,
                    entries: function () { return [this.currentEntry]; },
                    updateCurrentEntry: function (options) {
                        if (options && 'state' in options) this._state = options.state;
                    },
                    navigate: function (url) {
                        if (url != null) this.currentEntry.url = String(url);
                        return makeNavResult();
                    },
                    reload: function () { return makeNavResult(); },
                    traverseTo: function () { return makeNavResult(); },
                    back: function () { return makeNavResult(); },
                    forward: function () { return makeNavResult(); },
                    addEventListener: function () {},
                    removeEventListener: function () {},
                    dispatchEvent: function () { return true; }
                }
            });
        } catch (e) {}
    }

    if (typeof global.trustedTypes === 'undefined') {
        var trustedValue = new WeakMap();
        function makeTrustedCtor(name) {
            var ctor = function () { throw new TypeError('Illegal constructor'); };
            nativeize(ctor, name);
            var proto = {};
            Object.defineProperty(proto, 'toString', {
                configurable: true, writable: true,
                value: nativeize(function toString() {
                    if (!trustedValue.has(this)) throw new TypeError('Illegal invocation');
                    return trustedValue.get(this);
                }, 'toString')
            });
            Object.defineProperty(proto, 'toJSON', {
                configurable: true, writable: true,
                value: nativeize(function toJSON() {
                    if (!trustedValue.has(this)) throw new TypeError('Illegal invocation');
                    return trustedValue.get(this);
                }, 'toJSON')
            });
            Object.defineProperty(proto, Symbol.toStringTag,
                { value: name, configurable: true });
            Object.defineProperty(proto, 'constructor',
                { value: ctor, configurable: true, writable: true });
            Object.defineProperty(ctor, 'prototype', { value: proto });
            replaceCtor(name, ctor);
            return ctor;
        }
        var TrustedHTMLCtor = makeTrustedCtor('TrustedHTML');
        var TrustedScriptCtor = makeTrustedCtor('TrustedScript');
        var TrustedScriptURLCtor = makeTrustedCtor('TrustedScriptURL');
        function trusted(Ctor, value) {
            var result = Object.create(Ctor.prototype);
            trustedValue.set(result, String(value == null ? '' : value));
            return result;
        }
        var policyRules = new WeakMap();
        function TrustedTypePolicy() { throw new TypeError('Illegal constructor'); }
        nativeize(TrustedTypePolicy, 'TrustedTypePolicy');
        ['HTML', 'Script', 'ScriptURL'].forEach(function (kind) {
            var method = 'create' + kind;
            var Ctor = kind === 'HTML' ? TrustedHTMLCtor
                : kind === 'Script' ? TrustedScriptCtor : TrustedScriptURLCtor;
            Object.defineProperty(TrustedTypePolicy.prototype, method, {
                configurable: true, writable: true,
                value: nativeize(function (input) {
                    var record = policyRules.get(this);
                    if (!record) throw new TypeError('Illegal invocation');
                    var rule = record.rules[method];
                    var args = Array.prototype.slice.call(arguments, 1);
                    var value = typeof rule === 'function'
                        ? rule.apply(record.rules, [input].concat(args)) : input;
                    return trusted(Ctor, value);
                }, method)
            });
        });
        Object.defineProperty(TrustedTypePolicy.prototype, 'name', {
            configurable: true, enumerable: true,
            get: nativeize(function name() {
                var record = policyRules.get(this);
                if (!record) throw new TypeError('Illegal invocation');
                return record.name;
            }, 'get name')
        });
        Object.defineProperty(TrustedTypePolicy.prototype, Symbol.toStringTag,
            { value: 'TrustedTypePolicy', configurable: true });
        replaceCtor('TrustedTypePolicy', TrustedTypePolicy);

        var policies = Object.create(null);
        var defaultPolicy = null;
        function TrustedTypePolicyFactory() { throw new TypeError('Illegal constructor'); }
        nativeize(TrustedTypePolicyFactory, 'TrustedTypePolicyFactory');
        var factoryProto = TrustedTypePolicyFactory.prototype;
        defineMethod(factoryProto, 'createPolicy', nativeize(function createPolicy(name, rules) {
            name = String(name);
            if (policies[name]) throw new TypeError('Policy already exists');
            var policy = Object.create(TrustedTypePolicy.prototype);
            policyRules.set(policy, { name: name, rules: rules || {} });
            policies[name] = policy;
            if (name === 'default') defaultPolicy = policy;
            return policy;
        }, 'createPolicy'));
        defineMethod(factoryProto, 'isHTML', nativeize(function isHTML(value) {
            return value instanceof TrustedHTMLCtor;
        }, 'isHTML'));
        defineMethod(factoryProto, 'isScript', nativeize(function isScript(value) {
            return value instanceof TrustedScriptCtor;
        }, 'isScript'));
        defineMethod(factoryProto, 'isScriptURL', nativeize(function isScriptURL(value) {
            return value instanceof TrustedScriptURLCtor;
        }, 'isScriptURL'));
        defineMethod(factoryProto, 'getAttributeType', nativeize(function getAttributeType() {
            return null;
        }, 'getAttributeType'));
        defineMethod(factoryProto, 'getPropertyType', nativeize(function getPropertyType() {
            return null;
        }, 'getPropertyType'));
        Object.defineProperty(factoryProto, 'emptyHTML', {
            configurable: true, enumerable: true,
            get: nativeize(function emptyHTML() { return trusted(TrustedHTMLCtor, ''); }, 'get emptyHTML')
        });
        Object.defineProperty(factoryProto, 'emptyScript', {
            configurable: true, enumerable: true,
            get: nativeize(function emptyScript() { return trusted(TrustedScriptCtor, ''); }, 'get emptyScript')
        });
        Object.defineProperty(factoryProto, 'defaultPolicy', {
            configurable: true, enumerable: true,
            get: nativeize(function defaultPolicyGetter() { return defaultPolicy; }, 'get defaultPolicy')
        });
        Object.defineProperty(factoryProto, Symbol.toStringTag,
            { value: 'TrustedTypePolicyFactory', configurable: true });
        replaceCtor('TrustedTypePolicyFactory', TrustedTypePolicyFactory);
        var factory = Object.create(factoryProto);
        Object.defineProperty(global, 'trustedTypes', {
            value: factory, writable: false, configurable: true
        });
    }

    if (typeof global.Observable !== 'function') {
        function Subscription() {
            this.closed = false;
            this._cleanup = null;
        }
        Subscription.prototype.unsubscribe = function () {
            if (this.closed) return;
            this.closed = true;
            var cleanup = this._cleanup;
            this._cleanup = null;
            if (typeof cleanup === 'function') cleanup();
            else if (cleanup && typeof cleanup.unsubscribe === 'function') cleanup.unsubscribe();
        };
        function Observable(subscriber) {
            if (!(this instanceof Observable)) throw new TypeError('Observable requires new');
            if (typeof subscriber !== 'function') throw new TypeError('subscriber must be a function');
            this._subscriber = subscriber;
        }
        function observableFrom(value) {
            if (value instanceof Observable) return value;
            if (value && typeof Symbol.observable === 'symbol' &&
                typeof value[Symbol.observable] === 'function')
                return value[Symbol.observable]();
            if (value && typeof value.then === 'function') {
                return new Observable(function (observer) {
                    var active = true;
                    value.then(function (result) {
                        if (!active) return;
                        observer.next(result); observer.complete();
                    }, function (error) { if (active) observer.error(error); });
                    return function () { active = false; };
                });
            }
            if (value && typeof value[Symbol.iterator] === 'function') {
                return new Observable(function (observer) {
                    try {
                        for (var item of value) {
                            if (observer.closed) break;
                            observer.next(item);
                        }
                        if (!observer.closed) observer.complete();
                    } catch (error) { observer.error(error); }
                });
            }
            throw new TypeError('Value is not observable');
        }
        var OP = {};
        defineMethod(OP, 'catch', function (handler) {
            var source = this;
            return new Observable(function (observer) {
                var inner;
                var outer = source.subscribe({
                    next: function (value) { observer.next(value); },
                    error: function (error) {
                        try { inner = observableFrom(handler(error)).subscribe(observer); }
                        catch (nextError) { observer.error(nextError); }
                    },
                    complete: function () { observer.complete(); }
                });
                return function () { outer.unsubscribe(); if (inner) inner.unsubscribe(); };
            });
        });
        defineMethod(OP, 'drop', function (count) {
            var source = this; count = Math.max(0, Number(count) || 0);
            return new Observable(function (observer) {
                var seen = 0;
                return source.subscribe({
                    next: function (value) { if (seen++ >= count) observer.next(value); },
                    error: function (error) { observer.error(error); },
                    complete: function () { observer.complete(); }
                });
            });
        });
        defineMethod(OP, 'every', function (predicate) {
            var source = this;
            return new Promise(function (resolve, reject) {
                var index = 0, sub;
                sub = source.subscribe({
                    next: function (value) {
                        try { if (!predicate(value, index++)) { resolve(false); if (sub) sub.unsubscribe(); } }
                        catch (error) { reject(error); if (sub) sub.unsubscribe(); }
                    }, error: reject, complete: function () { resolve(true); }
                });
            });
        });
        defineMethod(OP, 'filter', function (predicate) {
            var source = this;
            return new Observable(function (observer) {
                var index = 0;
                return source.subscribe({
                    next: function (value) {
                        try { if (predicate(value, index++)) observer.next(value); }
                        catch (error) { observer.error(error); }
                    }, error: function (error) { observer.error(error); },
                    complete: function () { observer.complete(); }
                });
            });
        });
        defineMethod(OP, 'finally', function (callback) {
            var source = this;
            return new Observable(function (observer) {
                var sub = source.subscribe(observer);
                return function () { try { sub.unsubscribe(); } finally { callback(); } };
            });
        });
        defineMethod(OP, 'find', function (predicate) {
            var source = this;
            return new Promise(function (resolve, reject) {
                var index = 0, sub;
                sub = source.subscribe({
                    next: function (value) {
                        try { if (predicate(value, index++)) { resolve(value); if (sub) sub.unsubscribe(); } }
                        catch (error) { reject(error); if (sub) sub.unsubscribe(); }
                    }, error: reject, complete: function () { resolve(undefined); }
                });
            });
        });
        defineMethod(OP, 'first', function () {
            var source = this;
            return new Promise(function (resolve, reject) {
                var found = false, sub;
                sub = source.subscribe({
                    next: function (value) { if (!found) { found = true; resolve(value); if (sub) sub.unsubscribe(); } },
                    error: reject,
                    complete: function () { if (!found) reject(new RangeError('Observable is empty')); }
                });
            });
        });
        defineMethod(OP, 'flatMap', function (mapper) {
            var source = this;
            return new Observable(function (observer) {
                var inners = [], outerDone = false, index = 0;
                function finish() { if (outerDone && inners.length === 0) observer.complete(); }
                var outer = source.subscribe({
                    next: function (value) {
                        var inner;
                        try { inner = observableFrom(mapper(value, index++)); }
                        catch (error) { observer.error(error); return; }
                        var sub = inner.subscribe({
                            next: function (item) { observer.next(item); },
                            error: function (error) { observer.error(error); },
                            complete: function () { inners.splice(inners.indexOf(sub), 1); finish(); }
                        });
                        inners.push(sub);
                    }, error: function (error) { observer.error(error); },
                    complete: function () { outerDone = true; finish(); }
                });
                return function () { outer.unsubscribe(); inners.forEach(function (sub) { sub.unsubscribe(); }); };
            });
        });
        defineMethod(OP, 'forEach', function (callback) {
            var source = this;
            return new Promise(function (resolve, reject) {
                var index = 0;
                source.subscribe({
                    next: function (value) { try { callback(value, index++); } catch (error) { reject(error); } },
                    error: reject, complete: resolve
                });
            });
        });
        defineMethod(OP, 'inspect', function (inspector) {
            var source = this; inspector = inspector || {};
            return new Observable(function (observer) {
                if (typeof inspector.subscribe === 'function') inspector.subscribe();
                return source.subscribe({
                    next: function (value) {
                        if (typeof inspector.next === 'function') inspector.next(value);
                        observer.next(value);
                    }, error: function (error) {
                        if (typeof inspector.error === 'function') inspector.error(error);
                        observer.error(error);
                    }, complete: function () {
                        if (typeof inspector.complete === 'function') inspector.complete();
                        observer.complete();
                    }
                });
            });
        });
        defineMethod(OP, 'last', function () {
            var source = this;
            return new Promise(function (resolve, reject) {
                var found = false, last;
                source.subscribe({
                    next: function (value) { found = true; last = value; }, error: reject,
                    complete: function () { found ? resolve(last) : reject(new RangeError('Observable is empty')); }
                });
            });
        });
        defineMethod(OP, 'map', function (mapper) {
            var source = this;
            return new Observable(function (observer) {
                var index = 0;
                return source.subscribe({
                    next: function (value) {
                        try { observer.next(mapper(value, index++)); }
                        catch (error) { observer.error(error); }
                    }, error: function (error) { observer.error(error); },
                    complete: function () { observer.complete(); }
                });
            });
        });
        defineMethod(OP, 'reduce', function (reducer) {
            var source = this, hasInitial = arguments.length > 1, initial = arguments[1];
            return new Promise(function (resolve, reject) {
                var hasValue = hasInitial, accumulator = initial, index = 0;
                source.subscribe({
                    next: function (value) {
                        if (!hasValue) { hasValue = true; accumulator = value; return; }
                        try { accumulator = reducer(accumulator, value, index++); }
                        catch (error) { reject(error); }
                    }, error: reject,
                    complete: function () { hasValue ? resolve(accumulator) : reject(new TypeError('No initial value')); }
                });
            });
        });
        defineMethod(OP, 'some', function (predicate) {
            var source = this;
            return new Promise(function (resolve, reject) {
                var index = 0, sub;
                sub = source.subscribe({
                    next: function (value) {
                        try { if (predicate(value, index++)) { resolve(true); if (sub) sub.unsubscribe(); } }
                        catch (error) { reject(error); if (sub) sub.unsubscribe(); }
                    }, error: reject, complete: function () { resolve(false); }
                });
            });
        });
        defineMethod(OP, 'subscribe', function (observer, options) {
            if (typeof observer === 'function') observer = { next: observer };
            observer = observer || {};
            var subscription = new Subscription();
            var sink = {
                get closed() { return subscription.closed; },
                next: function (value) {
                    if (!subscription.closed && typeof observer.next === 'function') observer.next(value);
                },
                error: function (error) {
                    if (subscription.closed) return;
                    subscription.closed = true;
                    if (typeof observer.error === 'function') observer.error(error);
                    else setTimeout(function () { throw error; }, 0);
                },
                complete: function () {
                    if (subscription.closed) return;
                    subscription.closed = true;
                    if (typeof observer.complete === 'function') observer.complete();
                }
            };
            try { subscription._cleanup = this._subscriber(sink); }
            catch (error) { sink.error(error); }
            var signal = options && options.signal;
            if (signal) {
                if (signal.aborted) subscription.unsubscribe();
                else signal.addEventListener('abort', function () { subscription.unsubscribe(); }, { once: true });
            }
            return subscription;
        });
        defineMethod(OP, 'switchMap', function (mapper) {
            var source = this;
            return new Observable(function (observer) {
                var inner, outerDone = false, index = 0;
                var outer = source.subscribe({
                    next: function (value) {
                        if (inner) inner.unsubscribe();
                        try {
                            inner = observableFrom(mapper(value, index++)).subscribe({
                                next: function (item) { observer.next(item); },
                                error: function (error) { observer.error(error); },
                                complete: function () { inner = null; if (outerDone) observer.complete(); }
                            });
                        } catch (error) { observer.error(error); }
                    }, error: function (error) { observer.error(error); },
                    complete: function () { outerDone = true; if (!inner) observer.complete(); }
                });
                return function () { outer.unsubscribe(); if (inner) inner.unsubscribe(); };
            });
        });
        defineMethod(OP, 'take', function (count) {
            var source = this; count = Math.max(0, Number(count) || 0);
            return new Observable(function (observer) {
                if (count === 0) { observer.complete(); return; }
                var seen = 0, sub;
                sub = source.subscribe({
                    next: function (value) {
                        if (seen++ < count) observer.next(value);
                        if (seen >= count) { observer.complete(); if (sub) sub.unsubscribe(); }
                    }, error: function (error) { observer.error(error); },
                    complete: function () { observer.complete(); }
                });
                return sub;
            });
        });
        defineMethod(OP, 'takeUntil', function (notifier) {
            var source = this;
            return new Observable(function (observer) {
                var sourceSub = source.subscribe(observer);
                var notifierSub = observableFrom(notifier).subscribe({
                    next: function () { sourceSub.unsubscribe(); observer.complete(); },
                    error: function (error) { observer.error(error); }
                });
                return function () { sourceSub.unsubscribe(); notifierSub.unsubscribe(); };
            });
        });
        defineMethod(OP, 'toArray', function () {
            var source = this;
            return new Promise(function (resolve, reject) {
                var values = [];
                source.subscribe({ next: function (value) { values.push(value); }, error: reject,
                    complete: function () { resolve(values); } });
            });
        });
        Object.defineProperty(OP, Symbol.toStringTag,
            { value: 'Observable', configurable: true });
        Object.defineProperty(OP, 'constructor',
            { value: Observable, configurable: true, writable: true });
        Object.defineProperty(Observable, 'prototype', { value: OP });
        Object.defineProperty(Observable, 'from', {
            value: nativeize(observableFrom, 'from'), configurable: true, writable: true
        });
        nativeize(Observable, 'Observable');
        Object.getOwnPropertyNames(OP).forEach(function (name) {
            if (name !== 'constructor' && typeof OP[name] === 'function')
                nativeize(OP[name], name);
        });
        replaceCtor('Observable', Observable);
    }

    if (typeof global.scheduler === 'undefined') {
        defineCtor('scheduler', {
            postTask: function (callback, options) {
                var priority = options && options.priority;
                var delay = (options && options.delay) || 0;
                var signal = options && options.signal;
                return new Promise(function (resolve, reject) {
                    if (signal && signal.aborted) {
                        reject(signal.reason || new Error('AbortError'));
                        return;
                    }
                    var fire = function () {
                        if (signal && signal.aborted) {
                            reject(signal.reason || new Error('AbortError'));
                            return;
                        }
                        try { resolve(callback()); } catch (e) { reject(e); }
                    };
                    if (priority === 'background' || delay > 0) {
                        setTimeout(fire, delay);
                    } else {
                        Promise.resolve().then(fire);
                    }
                    if (signal && typeof signal.addEventListener === 'function') {
                        signal.addEventListener('abort', function () {
                            reject(signal.reason || new Error('AbortError'));
                        });
                    }
                });
            },
            yield: function () {
                if (typeof setTimeout === 'function') {
                    return new Promise(function (resolve) {
                        setTimeout(resolve, 0);
                    });
                }
                return Promise.resolve();
            }
        });
    }

    if (navigator) {
        try {
            if (!navigator.scheduling) {
                Object.defineProperty(navigator, 'scheduling', {
                    configurable: true, enumerable: true,
                    value: {
                        isInputPending: function () { return false; }
                    }
                });
            } else if (typeof navigator.scheduling.isInputPending !== 'function') {
                Object.defineProperty(navigator.scheduling, 'isInputPending', {
                    configurable: true, enumerable: true,
                    value: function () { return false; }
                });
            }
        } catch (e) {}
    }

    if (!global.visualViewport) {
        try {
            var visualViewport = {
                offsetLeft: 0,
                offsetTop: 0,
                scale: 1,
                onresize: null,
                onscroll: null,
                onscrollend: null,
                addEventListener: function () {},
                removeEventListener: function () {},
                dispatchEvent: function () { return true; }
            };
            Object.defineProperty(visualViewport, 'width', {
                configurable: true, enumerable: true,
                get: function () { return Number(global.innerWidth) || 0; }
            });
            Object.defineProperty(visualViewport, 'height', {
                configurable: true, enumerable: true,
                get: function () { return Number(global.innerHeight) || 0; }
            });
            Object.defineProperty(visualViewport, 'pageLeft', {
                configurable: true, enumerable: true,
                get: function () { return Number(global.scrollX || global.pageXOffset) || 0; }
            });
            Object.defineProperty(visualViewport, 'pageTop', {
                configurable: true, enumerable: true,
                get: function () { return Number(global.scrollY || global.pageYOffset) || 0; }
            });
            Object.defineProperty(global, 'visualViewport', {
                configurable: true, enumerable: true,
                value: visualViewport
            });
        } catch (e) {}
    }

    if (typeof global.TaskSignal !== 'function') {
        try {
            var TaskSignal = function (priority) {
                this.aborted = false;
                this.reason = undefined;
                this.onabort = null;
                this.onprioritychange = null;
                this.priority = priority || 'user-visible';
                this._cbs = [];
            };
            if (typeof global.AbortSignal === 'function' && global.AbortSignal.prototype) {
                TaskSignal.prototype = Object.create(global.AbortSignal.prototype);
                TaskSignal.prototype.constructor = TaskSignal;
            }
            TaskSignal.prototype.addEventListener = function (type, cb) {
                if (type === 'abort' && typeof cb === 'function') this._cbs.push(cb);
            };
            TaskSignal.prototype.removeEventListener = function (type, cb) {
                if (type !== 'abort') return;
                var i = this._cbs.indexOf(cb);
                if (i >= 0) this._cbs.splice(i, 1);
            };
            TaskSignal.prototype.dispatchEvent = function (ev) {
                if (ev && ev.type === 'abort') {
                    if (typeof this.onabort === 'function') {
                        try { this.onabort.call(this, ev); } catch (e) {}
                    }
                    var cbs = this._cbs.slice();
                    for (var i = 0; i < cbs.length; i++) {
                        try { cbs[i].call(this, ev); } catch (e) {}
                    }
                }
                return true;
            };
            TaskSignal.prototype.throwIfAborted = function () {
                if (!this.aborted) return;
                throw this.reason || new Error('AbortError');
            };
            defineCtor('TaskSignal', TaskSignal);
        } catch (e) {}
    }

    if (typeof global.TaskController !== 'function') {
        try {
            defineCtor('TaskController', function (options) {
                options = options || {};
                var Signal = typeof global.TaskSignal === 'function' ? global.TaskSignal : global.AbortSignal;
                this.signal = new Signal(options.priority || 'user-visible');
            });
            global.TaskController.prototype.abort = function (reason) {
                var signal = this.signal;
                if (!signal || signal.aborted) return;
                signal.aborted = true;
                signal.reason = reason === undefined ? new Error('AbortError') : reason;
                if (typeof signal.dispatchEvent === 'function')
                    signal.dispatchEvent({ type: 'abort', target: signal });
            };
            global.TaskController.prototype.setPriority = function (priority) {
                if (!this.signal) return;
                this.signal.priority = priority || 'user-visible';
                if (typeof this.signal.onprioritychange === 'function') {
                    try {
                        this.signal.onprioritychange.call(this.signal, {
                            type: 'prioritychange',
                            target: this.signal,
                            previousPriority: undefined
                        });
                    } catch (e) {}
                }
            };
        } catch (e) {}
    }

    if (typeof global.ReportingObserver !== 'function') {
        try {
            var ReportingObserver = function (callback, options) {
                this._callback = typeof callback === 'function' ? callback : null;
                this._options = options || {};
                this._records = [];
                this._observing = false;
            };
            ReportingObserver.prototype.observe = function () {
                this._observing = true;
            };
            ReportingObserver.prototype.disconnect = function () {
                this._observing = false;
                this._records = [];
            };
            ReportingObserver.prototype.takeRecords = function () {
                var records = this._records.slice();
                this._records.length = 0;
                return records;
            };
            ReportingObserver.supportedTypes = ['deprecation', 'intervention', 'crash'];
            defineCtor('ReportingObserver', ReportingObserver);
        } catch (e) {}
    }

    (function () {
        function num(v) {
            v = Number(v);
            return isFinite(v) ? v : 0;
        }
        function rectInit(self, x, y, width, height) {
            self.x = num(x);
            self.y = num(y);
            self.width = num(width);
            self.height = num(height);
        }
        function rectJSON() {
            return {
                x: this.x,
                y: this.y,
                width: this.width,
                height: this.height,
                top: this.top,
                right: this.right,
                bottom: this.bottom,
                left: this.left
            };
        }
        function DOMRectReadOnly(x, y, width, height) {
            rectInit(this, x, y, width, height);
        }
        Object.defineProperty(DOMRectReadOnly.prototype, 'top', {
            configurable: true,
            get: function () { return Math.min(this.y, this.y + this.height); }
        });
        Object.defineProperty(DOMRectReadOnly.prototype, 'right', {
            configurable: true,
            get: function () { return Math.max(this.x, this.x + this.width); }
        });
        Object.defineProperty(DOMRectReadOnly.prototype, 'bottom', {
            configurable: true,
            get: function () { return Math.max(this.y, this.y + this.height); }
        });
        Object.defineProperty(DOMRectReadOnly.prototype, 'left', {
            configurable: true,
            get: function () { return Math.min(this.x, this.x + this.width); }
        });
        DOMRectReadOnly.prototype.toJSON = rectJSON;
        DOMRectReadOnly.fromRect = function (other) {
            other = other || {};
            return new DOMRectReadOnly(other.x, other.y, other.width, other.height);
        };
        function DOMRect(x, y, width, height) {
            if (!(this instanceof DOMRect)) return new DOMRect(x, y, width, height);
            rectInit(this, x, y, width, height);
        }
        DOMRect.prototype = Object.create(DOMRectReadOnly.prototype);
        DOMRect.prototype.constructor = DOMRect;
        DOMRect.fromRect = function (other) {
            other = other || {};
            return new DOMRect(other.x, other.y, other.width, other.height);
        };
        replaceCtor('DOMRectReadOnly', DOMRectReadOnly);
        replaceCtor('DOMRect', DOMRect);
    })();

    if (typeof TextEncoder === 'function' && TextEncoder.prototype &&
        typeof TextEncoder.prototype.encodeInto !== 'function') {
        defineMethod(TextEncoder.prototype, 'encodeInto', function (source, destination) {
            var enc = this.encode(String(source));
            var dest = destination;
            var written = Math.min(enc.length, dest.length);
            for (var i = 0; i < written; i++) dest[i] = enc[i];
            var read = source.length;
            if (written < enc.length) {
                read = 0;
                for (var b = 0; b < written;) {
                    var c = source.charCodeAt(read++);
                    if (c < 0x80) b += 1;
                    else if (c < 0x800) b += 2;
                    else if (c >= 0xd800 && c <= 0xdbff) b += 4;
                    else b += 3;
                }
            }
            return { read: read, written: written };
        });
    }

    if (typeof Headers === 'function' && Headers.prototype &&
        typeof Headers.prototype.getSetCookie !== 'function') {
        defineMethod(Headers.prototype, 'getSetCookie', function () {
            var out = [];
            var m = this._m;
            if (m) {
                var keys = Object.keys(m);
                for (var i = 0; i < keys.length; i++) {
                    if (keys[i].toLowerCase() === 'set-cookie') out.push(m[keys[i]]);
                }
            }
            return out;
        });
    }

    var doc = global.document;
    if (doc && doc.implementation) {
        var liveImpl = doc.implementation;
        var realCreate = liveImpl && liveImpl.createHTMLDocument;
        var realCreateBroken = true;
        try {
            var probe = realCreate && realCreate.call(liveImpl, '');
            if (probe && probe.body) realCreateBroken = false;
        } catch (e) { realCreateBroken = true; }

        if (realCreateBroken) {
            var stubElement = function (tag) {
                var children = [];
                var attrs = {};
                var innerHtml = '';
                var textContent = '';
                var el = {
                    nodeType: 1,
                    tagName: String(tag).toUpperCase(),
                    nodeName: String(tag).toUpperCase(),
                    nodeValue: null,
                    childNodes: children,
                    children: children,
                    parentNode: null,
                    parentElement: null,
                    ownerDocument: null,
                    style: {},
                    href: '',
                    src: '',
                    appendChild: function (n) {
                        if (n) { children.push(n); if (n && typeof n === 'object') { try { n.parentNode = el; n.parentElement = el; } catch (e) {} } }
                        return n;
                    },
                    removeChild: function (n) {
                        var i = children.indexOf(n);
                        if (i >= 0) children.splice(i, 1);
                        if (n && typeof n === 'object') { try { n.parentNode = null; n.parentElement = null; } catch (e) {} }
                        return n;
                    },
                    insertBefore: function (n, ref) {
                        var i = ref ? children.indexOf(ref) : -1;
                        if (i < 0) children.push(n); else children.splice(i, 0, n);
                        if (n && typeof n === 'object') { try { n.parentNode = el; n.parentElement = el; } catch (e) {} }
                        return n;
                    },
                    replaceChild: function (n, ref) {
                        var i = children.indexOf(ref);
                        if (i >= 0) { children.splice(i, 1, n); if (n && typeof n === 'object') { try { n.parentNode = el; n.parentElement = el; } catch (e) {} } }
                        return ref;
                    },
                    contains: function (n) { return children.indexOf(n) >= 0; },
                    cloneNode: function () { var c = stubElement(tag); c.innerHTML = innerHtml; return c; },
                    setAttribute: function (k, v) { attrs[String(k)] = String(v == null ? '' : v); },
                    getAttribute: function (k) { var v = attrs[String(k)]; return v === undefined ? null : v; },
                    hasAttribute: function (k) { return Object.prototype.hasOwnProperty.call(attrs, String(k)); },
                    removeAttribute: function (k) { delete attrs[String(k)]; },
                    hasAttributes: function () { return Object.keys(attrs).length > 0; },
                    addEventListener: function () {},
                    removeEventListener: function () {},
                    dispatchEvent: function () { return true; },
                    getElementsByTagName: function () { return []; },
                    getElementsByClassName: function () { return []; },
                    querySelector: function () { return null; },
                    querySelectorAll: function () { return []; },
                    closest: function () { return null; },
                    matches: function () { return false; },
                    classList: {
                        add: function () {}, remove: function () {},
                        toggle: function () { return false; },
                        contains: function () { return false; },
                        replace: function () {},
                    },
                    attributes: attrs,
                };
                Object.defineProperty(el, 'firstChild', { configurable: true, get: function () { return children[0] || null; } });
                Object.defineProperty(el, 'lastChild', { configurable: true, get: function () { return children[children.length - 1] || null; } });
                Object.defineProperty(el, 'firstElementChild', { configurable: true, get: function () {
                    for (var i = 0; i < children.length; i++) if (children[i] && children[i].nodeType === 1) return children[i];
                    return null;
                } });
                Object.defineProperty(el, 'lastElementChild', { configurable: true, get: function () {
                    for (var i = children.length - 1; i >= 0; i--) if (children[i] && children[i].nodeType === 1) return children[i];
                    return null;
                } });
                Object.defineProperty(el, 'nextSibling', { configurable: true, get: function () {
                    var p = el.parentNode;
                    if (!p || !p.childNodes) return null;
                    var i = p.childNodes.indexOf(el);
                    return (i >= 0 && i + 1 < p.childNodes.length) ? p.childNodes[i + 1] : null;
                } });
                Object.defineProperty(el, 'previousSibling', { configurable: true, get: function () {
                    var p = el.parentNode;
                    if (!p || !p.childNodes) return null;
                    var i = p.childNodes.indexOf(el);
                    return (i > 0) ? p.childNodes[i - 1] : null;
                } });
                Object.defineProperty(el, 'innerHTML', {
                    configurable: true, enumerable: true,
                    get: function () { return innerHtml; },
                    set: function (v) {
                        innerHtml = String(v == null ? '' : v);
                        children.length = 0;
                        var depth = 0;
                        var re = /<(\/?)([\w-]+)([^>]*)>/g, m;
                        while ((m = re.exec(innerHtml)) !== null) {
                            if (m[1] === '/') {
                                if (depth > 0) depth--;
                            } else {
                                var selfClose = m[3].slice(-1) === '/' || /^(?:area|base|br|col|embed|hr|img|input|link|meta|param|source|track|wbr)$/i.test(m[2]);
                                if (depth === 0) {
                                    var child = stubElement(m[2]);
                                    child.parentNode = el; child.parentElement = el;
                                    children.push(child);
                                }
                                if (!selfClose) depth++;
                            }
                        }
                    },
                });
                Object.defineProperty(el, 'textContent', {
                    configurable: true, enumerable: true,
                    get: function () { return textContent; },
                    set: function (v) { textContent = String(v == null ? '' : v); children.length = 0; },
                });
                Object.defineProperty(el, 'outerHTML', {
                    configurable: true, enumerable: true,
                    get: function () { return '<' + tag + '>' + innerHtml + '</' + tag + '>'; },
                    set: function () {},
                });
                return el;
            };

            var stubBody = function () { return stubElement('body'); };

            var stubDocument = function (title) {
                var docStub = {
                    nodeType: 9,
                    nodeName: '#document',
                    title: title == null ? '' : String(title),
                    contentType: 'text/html',
                    compatMode: 'CSS1Compat',
                    location: { href: '' },
                    body: stubBody(),
                    head: stubElement('head'),
                    documentElement: stubElement('html'),
                    createElement: function (tag) { return stubElement(tag); },
                    createTextNode: function (t) { return { nodeType: 3, nodeValue: String(t == null ? '' : t), data: String(t == null ? '' : t) }; },
                    createComment: function (t) { return { nodeType: 8, nodeValue: String(t == null ? '' : t), data: String(t == null ? '' : t) }; },
                    createDocumentFragment: function () {
                        var frag = stubElement('#document-fragment');
                        frag.nodeType = 11; frag.nodeName = '#document-fragment'; frag.tagName = undefined;
                        return frag;
                    },
                    getElementsByTagName: function () { return []; },
                    getElementsByClassName: function () { return []; },
                    getElementById: function () { return null; },
                    querySelector: function () { return null; },
                    querySelectorAll: function () { return []; },
                    addEventListener: function () {},
                    removeEventListener: function () {},
                    dispatchEvent: function () { return true; },
                };
                docStub.implementation = {
                    hasFeature: function () { return true; },
                    createHTMLDocument: stubDocument,
                    createDocument: function () { return stubDocument(''); },
                    createDocumentType: function () { return {}; },
                };
                return docStub;
            };

            try {
                Object.defineProperty(doc, 'implementation', {
                    configurable: true, enumerable: true,
                    get: function () {
                        return {
                            hasFeature: function () { return true; },
                            createHTMLDocument: stubDocument,
                            createDocument: function () { return stubDocument(''); },
                            createDocumentType: function () { return {}; },
                        };
                    },
                });
            } catch (e) {}
        }
    }

    (function () {
        var doc = global.document;
        if (!doc || typeof doc.createElement !== 'function') return;
        if (typeof global.CSSStyleSheet === 'function' &&
            global.CSSStyleSheet.prototype &&
            typeof global.CSSStyleSheet.prototype.replaceSync === 'function')
            return;

        var hostSeq = 0;
        function isIdentChar(c) {
            return c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' ||
                   c >= '0' && c <= '9' || c === '_' || c === '-';
        }
        function rewriteHostTokens(css, id) {
            var marker = '[data-nd-host="' + id + '"]';
            var out = '';
            for (var i = 0; i < css.length;) {
                if (css.substr(i, 10).toLowerCase() === '::slotted(') {
                    var j = i + 10, depth = 1, inner = j;
                    for (; j < css.length && depth; j++) {
                        if (css[j] === '(') depth++;
                        else if (css[j] === ')') { depth--; if (!depth) break; }
                    }
                    out += marker + ' > ' + css.slice(inner, j);
                    i = css[j] === ')' ? j + 1 : j;
                    continue;
                }
                if (css.substr(i, 5).toLowerCase() === ':host') {
                    if (css.substr(i + 5, 9).toLowerCase() === '-context(') {
                        var j = i + 14, depth = 1;
                        for (; j < css.length && depth; j++) {
                            if (css[j] === '(') depth++;
                            else if (css[j] === ')') depth--;
                        }
                        out += marker;
                        i = j;
                        continue;
                    }
                    if (css[i + 5] === '(') {
                        var j = i + 6, depth = 1, inner = j;
                        for (; j < css.length && depth; j++) {
                            if (css[j] === '(') depth++;
                            else if (css[j] === ')') { depth--; if (!depth) break; }
                        }
                        out += marker + css.slice(inner, j);
                        i = css[j] === ')' ? j + 1 : j;
                        continue;
                    }
                    var nc = css[i + 5];
                    if (!nc || !isIdentChar(nc)) { out += marker; i += 5; continue; }
                }
                out += css[i];
                i++;
            }
            return out;
        }
        function scanSegment(css, i, end) {
            var quote = 0, paren = 0, bracket = 0;
            for (; i < end; i++) {
                var c = css[i];
                if (quote) { if (c === '\\' && i + 1 < end) i++; else if (c === quote) quote = 0; }
                else if (c === '"' || c === "'") quote = c;
                else if (c === '/' && css[i + 1] === '*') {
                    i += 2; while (i + 1 < end && !(css[i] === '*' && css[i + 1] === '/')) i++;
                } else if (c === '(') paren++;
                else if (c === ')') { if (paren) paren--; }
                else if (c === '[') bracket++;
                else if (c === ']') { if (bracket) bracket--; }
                else if (!paren && !bracket && (c === '{' || c === ';' || c === '}')) return i;
            }
            return end;
        }
        function skipBlock(css, i, end) {
            var depth = 0, quote = 0;
            for (; i < end; i++) {
                var c = css[i];
                if (quote) { if (c === '\\' && i + 1 < end) i++; else if (c === quote) quote = 0; }
                else if (c === '"' || c === "'") quote = c;
                else if (c === '/' && css[i + 1] === '*') {
                    i += 2; while (i + 1 < end && !(css[i] === '*' && css[i + 1] === '/')) i++;
                } else if (c === '{') depth++;
                else if (c === '}') { depth--; if (depth === 0) return i + 1; }
            }
            return end;
        }
        function splitTopComma(s) {
            var res = [], depth = 0, bracket = 0, quote = 0, start = 0;
            for (var i = 0; i < s.length; i++) {
                var c = s[i];
                if (quote) { if (c === '\\' && i + 1 < s.length) i++; else if (c === quote) quote = 0; }
                else if (c === '"' || c === "'") quote = c;
                else if (c === '(') depth++;
                else if (c === ')') { if (depth) depth--; }
                else if (c === '[') bracket++;
                else if (c === ']') { if (bracket) bracket--; }
                else if (c === ',' && !depth && !bracket) { res.push(s.slice(start, i)); start = i + 1; }
            }
            res.push(s.slice(start));
            return res;
        }
        function scopeSelector(sel, id, marker) {
            sel = sel.replace(/^\s+|\s+$/g, '');
            if (!sel) return '';
            if (sel.indexOf(':host') >= 0 || sel.indexOf('::slotted') >= 0)
                return rewriteHostTokens(sel, id);
            return marker + ' ' + sel;
        }
        function scopeRuleList(css, start, end, id, marker) {
            var out = '', i = start;
            while (i < end) {
                while (i < end && /\s/.test(css[i])) i++;
                if (i >= end) break;
                if (css[i] === '/' && css[i + 1] === '*') {
                    i += 2; while (i + 1 < end && !(css[i] === '*' && css[i + 1] === '/')) i++; i += 2; continue;
                }
                if (css[i] === '}') { i++; continue; }
                if (css[i] === '@') {
                    var seg = scanSegment(css, i, end), term = css[seg], prelude = css.slice(i, seg);
                    if (term === '{') {
                        var be = skipBlock(css, seg, end);
                        if (/^@(media|supports|container|layer|scope)\b/i.test(prelude))
                            out += prelude + '{' + scopeRuleList(css, seg + 1, be - 1, id, marker) + '}';
                        else out += css.slice(i, be);
                        i = be;
                    } else { out += prelude; if (term === ';') { out += ';'; i = seg + 1; } else i = seg; }
                    continue;
                }
                var seg2 = scanSegment(css, i, end);
                if (css[seg2] !== '{') { i = (seg2 < end) ? seg2 + 1 : end; continue; }
                var be2 = skipBlock(css, seg2, end);
                var parts = splitTopComma(css.slice(i, seg2)), scoped = [];
                for (var k = 0; k < parts.length; k++) {
                    var sc = scopeSelector(parts[k], id, marker);
                    if (sc) scoped.push(sc);
                }
                out += scoped.join(', ') + '{' + css.slice(seg2 + 1, be2 > seg2 ? be2 - 1 : seg2) + '}';
                i = be2;
            }
            return out;
        }
        function scopeCss(css, id) {
            if (!id) return css;
            return scopeRuleList(css, 0, css.length, id, '[data-nd-host="' + id + '"]');
        }
        function hostScopeId(host) {
            if (!host || typeof host.getAttribute !== 'function') return null;
            var existing = host.getAttribute('data-nd-host');
            if (existing) return existing;
            var id = 'a' + (++hostSeq);
            try { host.setAttribute('data-nd-host', id); } catch (e) { return null; }
            return id;
        }

        function applyText(sheet) {
            var nodes = sheet.__nodes;
            for (var i = 0; i < nodes.length; i++) {
                try {
                    nodes[i].textContent =
                        scopeCss(sheet.__cssText || '', nodes[i].__ndScopeId);
                } catch (e) {}
            }
        }

        function CSSStyleSheet(options) {
            this.__cssText = '';
            this.__nodes = [];
            this.media = (options && options.media) || '';
            this.disabled = !!(options && options.disabled);
        }
        CSSStyleSheet.prototype.replaceSync = function (text) {
            this.__cssText = String(text == null ? '' : text);
            applyText(this);
        };
        CSSStyleSheet.prototype.replace = function (text) {
            try { this.replaceSync(text); return Promise.resolve(this); }
            catch (e) { return Promise.reject(e); }
        };
        CSSStyleSheet.prototype.insertRule = function (rule, index) {
            this.__cssText += (this.__cssText ? '\n' : '') + String(rule);
            applyText(this);
            return typeof index === 'number' ? index : 0;
        };
        CSSStyleSheet.prototype.deleteRule = function () {};
        Object.defineProperty(CSSStyleSheet.prototype, 'cssRules', {
            configurable: true, get: function () { return []; }
        });
        Object.defineProperty(CSSStyleSheet.prototype, 'rules', {
            configurable: true, get: function () { return []; }
        });
        Object.defineProperty(global, 'CSSStyleSheet', {
            value: CSSStyleSheet, writable: true,
            configurable: true, enumerable: false
        });

        function materialize(target, sheets) {
            var scopeId = (target === doc) ? null : hostScopeId(target.host);
            var container = doc.head || doc.documentElement || doc.body;
            var live = [];
            if (!container || typeof container.appendChild !== 'function')
                return live;
            for (var i = 0; i < sheets.length; i++) {
                var s = sheets[i];
                if (!s || !(s instanceof CSSStyleSheet)) continue;
                var el = doc.createElement('style');
                el.setAttribute('data-adopted', '');
                el.__ndScopeId = scopeId;
                el.textContent = scopeCss(s.__cssText || '', scopeId);
                container.appendChild(el);
                s.__nodes.push(el);
                live.push({ sheet: s, node: el });
            }
            return live;
        }

        function defineAdopted(target) {
            if (!target) return;
            var store = [];
            var live = [];
            try {
                Object.defineProperty(target, 'adoptedStyleSheets', {
                    configurable: true, enumerable: true,
                    get: function () { return store; },
                    set: function (v) {
                        var arr = v ? Array.prototype.slice.call(v) : [];
                        for (var i = 0; i < live.length; i++) {
                            var ent = live[i];
                            var idx = ent.sheet.__nodes.indexOf(ent.node);
                            if (idx >= 0) ent.sheet.__nodes.splice(idx, 1);
                            if (ent.node.parentNode)
                                ent.node.parentNode.removeChild(ent.node);
                        }
                        live = materialize(target, arr);
                        store = arr;
                    }
                });
            } catch (e) {}
        }

        defineAdopted(doc);

        if (typeof global.Element === 'function' &&
            global.Element.prototype &&
            typeof global.Element.prototype.attachShadow === 'function') {
            var origAttach = global.Element.prototype.attachShadow;
            global.Element.prototype.attachShadow = function () {
                var root = origAttach.apply(this, arguments);
                if (root && !('adoptedStyleSheets' in root)) defineAdopted(root);
                return root;
            };
        }
    })();

    /* CSSOM rule model: document.styleSheets, HTMLStyleElement/LinkElement
     * .sheet, and a real CSSRule / CSSStyleRule / CSSGroupingRule tree backed
     * by the owner <style> node. The native bindings exposed an empty
     * styleSheets list and a null .sheet; the rules are parsed from the node's
     * text once, then insertRule/deleteRule/replace mutate the tree in place
     * and rebuild the node's text content, which the engine re-cascades.
     * Each CSSStyleRule's .style is a live native CSSStyleDeclaration. */
    (function () {
        if (typeof document === 'undefined') return;

        if (typeof global.CSSStyleDeclaration === 'function' &&
            global.CSSStyleDeclaration.prototype &&
            !(Symbol.iterator in global.CSSStyleDeclaration.prototype)) {
            try {
                Object.defineProperty(global.CSSStyleDeclaration.prototype,
                                      Symbol.iterator, {
                    configurable: true, writable: true,
                    value: function () {
                        var self = this, i = 0;
                        return {
                            next: function () {
                                if (i < (self.length >>> 0))
                                    return { value: self[i++], done: false };
                                return { value: undefined, done: true };
                            },
                            'return': function () { return { done: true }; }
                        };
                    }
                });
            } catch (e) {}
        }

        function ctorFor(name, parentProto) {
            var ctor = global[name];
            if (typeof ctor !== 'function') {
                ctor = function () {
                    throw new TypeError('Illegal constructor');
                };
                try {
                    Object.defineProperty(global, name, {
                        value: ctor, writable: true,
                        configurable: true, enumerable: false
                    });
                } catch (e) {}
            }
            if (parentProto && ctor.prototype &&
                Object.getPrototypeOf(ctor.prototype) !== parentProto) {
                try { Object.setPrototypeOf(ctor.prototype, parentProto); }
                catch (e) {}
            }
            try {
                Object.defineProperty(ctor.prototype, Symbol.toStringTag, {
                    value: name, configurable: true
                });
            } catch (e) {}
            return ctor;
        }

        function getter(proto, name, fn) {
            try {
                Object.defineProperty(proto, name, {
                    configurable: true, enumerable: true, get: fn
                });
            } catch (e) {}
        }
        function accessor(proto, name, get, set) {
            try {
                Object.defineProperty(proto, name, {
                    configurable: true, enumerable: true, get: get, set: set
                });
            } catch (e) {}
        }
        function method(proto, name, fn) {
            try {
                Object.defineProperty(proto, name, {
                    configurable: true, writable: true,
                    enumerable: true, value: fn
                });
            } catch (e) {}
        }

        var CSSRule = ctorFor('CSSRule', Object.prototype);
        var CSSStyleRule = ctorFor('CSSStyleRule', CSSRule.prototype);
        var CSSGroupingRule = ctorFor('CSSGroupingRule', CSSRule.prototype);
        var CSSConditionRule = ctorFor('CSSConditionRule', CSSGroupingRule.prototype);
        var CSSMediaRule = ctorFor('CSSMediaRule', CSSConditionRule.prototype);
        var CSSSupportsRule = ctorFor('CSSSupportsRule', CSSConditionRule.prototype);

        var CONSTANTS = {
            STYLE_RULE: 1, CHARSET_RULE: 2, IMPORT_RULE: 3, MEDIA_RULE: 4,
            FONT_FACE_RULE: 5, PAGE_RULE: 6, KEYFRAMES_RULE: 7,
            KEYFRAME_RULE: 8, MARGIN_RULE: 9, NAMESPACE_RULE: 10,
            COUNTER_STYLE_RULE: 11, SUPPORTS_RULE: 12,
            FONT_FEATURE_VALUES_RULE: 14
        };
        Object.keys(CONSTANTS).forEach(function (k) {
            var v = CONSTANTS[k];
            try {
                Object.defineProperty(CSSRule.prototype, k, {
                    value: v, enumerable: true, configurable: true
                });
                Object.defineProperty(CSSRule, k, {
                    value: v, enumerable: true, configurable: true
                });
            } catch (e) {}
        });

        getter(CSSRule.prototype, 'type', function () {
            return this.__type | 0;
        });
        getter(CSSRule.prototype, 'parentRule', function () {
            return this.__parentRule || null;
        });
        getter(CSSRule.prototype, 'parentStyleSheet', function () {
            return this.__parentStyleSheet || null;
        });
        accessor(CSSRule.prototype, 'cssText',
            function () {
                return typeof this.__cssText === 'function'
                    ? this.__cssText() : '';
            },
            function () {});

        function notify(owner) {
            var s = owner;
            while (s && typeof s.__notify !== 'function') s = s.__parentStyleSheet;
            if (s && typeof s.__notify === 'function') s.__notify();
        }

        function canonAnB(raw) {
            var s = String(raw);
            if (/^\s*even\s*$/i.test(s)) return '2n';
            if (/^\s*odd\s*$/i.test(s)) return '2n+1';
            var mi = /^\s*([+-]?\d+)\s*$/.exec(s);
            if (mi) return String(parseInt(mi[1], 10));
            var m = /^\s*([+-]?\d*)n\s*(?:([+-])\s*(\d+))?\s*$/i.exec(s);
            if (!m) return null;
            var aStr = m[1];
            var A = (aStr === '' || aStr === '+') ? 1
                  : aStr === '-' ? -1 : parseInt(aStr, 10);
            var B = m[2] ? parseInt(m[2] + m[3], 10) : 0;
            var out = A === 1 ? 'n' : A === -1 ? '-n' : A + 'n';
            if (B > 0) out += '+' + B;
            else if (B < 0) out += '-' + (-B);
            return out;
        }
        var ANB_RE = /:(nth-child|nth-last-child|nth-of-type|nth-last-of-type)\(([^)]*)\)/gi;
        function anbPart(arg) {
            var ofIdx = arg.toLowerCase().indexOf(' of ');
            return ofIdx >= 0 ? arg.slice(0, ofIdx) : arg;
        }
        function canonSelector(sel) {
            if (!sel) return sel;
            return sel.replace(ANB_RE, function (m, fn, arg) {
                var ofIdx = arg.toLowerCase().indexOf(' of ');
                var rest = ofIdx >= 0 ? arg.slice(ofIdx) : '';
                var c = canonAnB(anbPart(arg));
                return c === null ? m
                    : ':' + fn.toLowerCase() + '(' + c + rest + ')';
            });
        }
        function selectorAnBValid(sel) {
            ANB_RE.lastIndex = 0;
            var m;
            while ((m = ANB_RE.exec(sel)))
                if (canonAnB(anbPart(m[2])) === null) return false;
            return true;
        }
        function selectorLooksNested(sel) {
            if (sel.indexOf('&') !== -1 || /^\s*[>+~]/.test(sel)) return true;
            var bare = sel.replace(/"[^"]*"|'[^']*'/g, '');
            return /[|]/.test(bare.replace(/\|\||\|=/g, ''));
        }
        function selectorParses(sel) {
            try { document.querySelectorAll(sel); return true; }
            catch (e) { return false; }
        }
        function preludeSelectorValid(sel) {
            if (!selectorAnBValid(sel)) return false;
            if (selectorLooksNested(sel)) return true;
            return selectorParses(sel);
        }
        accessor(CSSStyleRule.prototype, 'selectorText',
            function () { return canonSelector(this.__selector || ''); },
            function (v) {
                v = String(v);
                try { document.querySelectorAll(v); }
                catch (e) { return; }
                this.__selector = v.replace(/^\s+|\s+$/g, '');
                notify(this);
            });
        accessor(CSSStyleRule.prototype, 'style',
            function () { return this.__style; },
            function (v) {
                try { this.__style.cssText = (v == null) ? '' : String(v); }
                catch (e) {}
                notify(this);
            });

        function declText(rule) {
            var style = rule.__style, out = [];
            try {
                for (var i = 0; i < (style.length >>> 0); i++) {
                    var name = style.item(i);
                    if (!name) continue;
                    var val = style.getPropertyValue(name);
                    var pri = style.getPropertyPriority(name);
                    out.push(name + ': ' + val + (pri ? ' !' + pri : '') + ';');
                }
            } catch (e) { return ''; }
            return out.join(' ');
        }
        method(CSSStyleRule.prototype, '__cssText', function () {
            var d = declText(this);
            return this.__selector + (d ? ' { ' + d + ' }' : ' { }');
        });

        getter(CSSGroupingRule.prototype, 'cssRules', function () {
            return this.__ruleList;
        });
        getter(CSSGroupingRule.prototype, 'rules', function () {
            return this.__ruleList;
        });
        method(CSSGroupingRule.prototype, 'insertRule', function (text, index) {
            return insertInto(this, this.__rules, this.__ruleList,
                              text, index, false);
        });
        method(CSSGroupingRule.prototype, 'deleteRule', function (index) {
            return deleteFrom(this, this.__rules, this.__ruleList, index);
        });
        function splitTopLevel(text, sep) {
            var parts = [], depth = 0, start = 0;
            for (var i = 0; i < text.length; i++) {
                var c = text.charAt(i);
                if (c === '(') depth++;
                else if (c === ')') { if (depth) depth--; }
                else if (c === sep && depth === 0) {
                    parts.push(text.slice(start, i));
                    start = i + 1;
                }
            }
            parts.push(text.slice(start));
            return parts;
        }
        function serializeMediaFeature(f) {
            var inner = f.slice(1, -1).replace(/^\s+|\s+$/g, '');
            var ci = inner.indexOf(':');
            if (ci < 0) return '(' + inner.toLowerCase() + ')';
            var name = inner.slice(0, ci).replace(/^\s+|\s+$/g, '').toLowerCase();
            var val = inner.slice(ci + 1).replace(/^\s+|\s+$/g, '');
            return '(' + name + ': ' + val + ')';
        }
        function serializeMediaQuery(q) {
            q = q.replace(/^\s+|\s+$/g, '');
            if (!q) return '';
            var i = 0, n = q.length, features = [];
            while (i < n && q.charAt(i) !== '(') i++;
            var head = q.slice(0, i).replace(/\s+and\s*$/i, '')
                                    .replace(/^\s+|\s+$/g, '');
            while (i < n) {
                if (q.charAt(i) === '(') {
                    var d = 1, j = i + 1;
                    while (j < n && d > 0) {
                        var ch = q.charAt(j);
                        if (ch === '(') d++;
                        else if (ch === ')') d--;
                        j++;
                    }
                    features.push(serializeMediaFeature(q.slice(i, j)));
                    i = j;
                } else i++;
            }
            var modifier = '', type = '';
            if (head) {
                var toks = head.split(/\s+/);
                var first = toks[0].toLowerCase();
                if (first === 'not' || first === 'only') {
                    modifier = first;
                    type = (toks[1] || '').toLowerCase();
                } else {
                    type = first;
                }
            }
            if (modifier) {
                var s = type ? modifier + ' ' + type : modifier;
                if (features.length) s += ' and ' + features.join(' and ');
                return s;
            }
            if (type && type !== 'all') {
                var s2 = type;
                if (features.length) s2 += ' and ' + features.join(' and ');
                return s2;
            }
            if (type === 'all' && !features.length) return 'all';
            return features.join(' and ');
        }
        function serializeMediaList(text) {
            if (!text) return '';
            return splitTopLevel(text, ',').map(serializeMediaQuery)
                       .filter(function (q) { return q !== ''; })
                       .join(', ');
        }

        method(CSSGroupingRule.prototype, '__header', function () {
            if (this.__at === 'media')
                return '@media ' + serializeMediaList(this.__condition);
            return this.__prelude;
        });
        method(CSSGroupingRule.prototype, '__cssText', function () {
            var inner = this.__rules.map(function (r) {
                return '  ' + r.cssText + '\n';
            }).join('');
            return this.__header() + ' {\n' + inner + '}';
        });

        accessor(CSSConditionRule.prototype, 'conditionText',
            function () {
                return this.__at === 'media'
                    ? serializeMediaList(this.__condition)
                    : (this.__condition || '');
            },
            function () {});
        accessor(CSSMediaRule.prototype, 'media',
            function () { return serializeMediaList(this.__condition); },
            function () {});

        function makeList() {
            var list = [];
            list.item = function (i) {
                i = i >>> 0;
                return (i < this.length) ? this[i] : null;
            };
            try {
                Object.defineProperty(list, Symbol.toStringTag, {
                    value: 'CSSRuleList', configurable: true
                });
            } catch (e) {}
            return list;
        }
        function syncList(list, rules) {
            for (var i = 0; i < rules.length; i++) list[i] = rules[i];
            list.length = rules.length;
        }

        function atKeyword(prelude) {
            var m = /^@([\w-]+)/.exec(prelude);
            return m ? m[1].toLowerCase() : null;
        }
        var GROUPING_AT = {
            media: 'CSSMediaRule', supports: 'CSSSupportsRule',
            container: 'CSSGroupingRule', layer: 'CSSGroupingRule',
            scope: 'CSSGroupingRule', document: 'CSSGroupingRule'
        };

        function parseRuleList(text, sheet, parentRule) {
            var rules = [], i = 0, n = text.length;
            while (i < n) {
                while (i < n && /\s/.test(text.charAt(i))) i++;
                if (i < n && text.charAt(i) === '/' && text.charAt(i + 1) === '*') {
                    i += 2;
                    while (i + 1 < n && !(text.charAt(i) === '*' &&
                                          text.charAt(i + 1) === '/')) i++;
                    i += 2; continue;
                }
                if (i >= n) break;
                if (text.charAt(i) === '}') { i++; continue; }
                var start = i, quote = 0, depth = 0;
                while (i < n) {
                    var c = text.charAt(i);
                    if (quote) {
                        if (c === '\\') i++;
                        else if (c === quote) quote = 0;
                        i++; continue;
                    }
                    if (c === '"' || c === "'") { quote = c; i++; continue; }
                    if (c === '/' && text.charAt(i + 1) === '*') {
                        i += 2;
                        while (i + 1 < n && !(text.charAt(i) === '*' &&
                                              text.charAt(i + 1) === '/')) i++;
                        i += 2; continue;
                    }
                    if (c === '(') { depth++; i++; continue; }
                    if (c === ')') { if (depth) depth--; i++; continue; }
                    if (!depth && (c === '{' || c === ';')) break;
                    i++;
                }
                var prelude = text.slice(start, i).replace(/^\s+|\s+$/g, '');
                if (i < n && text.charAt(i) === ';') {
                    i++;
                    if (prelude) {
                        var sr = makeAtStatement(prelude, sheet, parentRule);
                        if (sr) rules.push(sr);
                    }
                    continue;
                }
                if (i >= n || text.charAt(i) !== '{') {
                    break;
                }
                var bstart = ++i; depth = 1; quote = 0;
                while (i < n && depth > 0) {
                    var c2 = text.charAt(i);
                    if (quote) {
                        if (c2 === '\\') i++;
                        else if (c2 === quote) quote = 0;
                        i++; continue;
                    }
                    if (c2 === '"' || c2 === "'") { quote = c2; i++; continue; }
                    if (c2 === '/' && text.charAt(i + 1) === '*') {
                        i += 2;
                        while (i + 1 < n && !(text.charAt(i) === '*' &&
                                              text.charAt(i + 1) === '/')) i++;
                        i += 2; continue;
                    }
                    if (c2 === '{') depth++;
                    else if (c2 === '}') depth--;
                    i++;
                }
                var block = text.slice(bstart, (depth === 0) ? i - 1 : i);
                var br = makeBlockRule(prelude, block, sheet, parentRule);
                if (br) rules.push(br);
            }
            return rules;
        }

        function serializeDeclBlock(block) {
            var s = String(block), out = [], buf = '', depth = 0, quote = 0;
            for (var i = 0; i < s.length; i++) {
                var c = s.charAt(i);
                if (quote) {
                    buf += c;
                    if (c === quote && s.charAt(i - 1) !== '\\') quote = 0;
                    continue;
                }
                if (c === '"' || c === "'") { quote = c; buf += c; continue; }
                if (c === '(') { depth++; buf += c; continue; }
                if (c === ')') { if (depth) depth--; buf += c; continue; }
                if (c === ';' && depth === 0) {
                    var d = buf.replace(/\s+/g, ' ').replace(/^ | $/g, '');
                    if (d) out.push(d + ';');
                    buf = '';
                    continue;
                }
                buf += c;
            }
            var last = buf.replace(/\s+/g, ' ').replace(/^ | $/g, '');
            if (last) out.push(last + ';');
            return out.join(' ');
        }

        function makeAtStatement(prelude, sheet, parentRule) {
            var kw = atKeyword(prelude), rule = Object.create(CSSRule.prototype);
            rule.__parentStyleSheet = sheet || null;
            rule.__parentRule = parentRule || null;
            rule.__at = kw;
            rule.__type = kw === 'import' ? 3 :
                          kw === 'namespace' ? 10 :
                          kw === 'charset' ? 2 : 0;
            var text = prelude + ';';
            rule.__cssText = function () { return text; };
            return rule;
        }

        function makeBlockRule(prelude, block, sheet, parentRule) {
            var kw = atKeyword(prelude);
            if (kw && GROUPING_AT[kw]) {
                var Ctor = global[GROUPING_AT[kw]] || CSSGroupingRule;
                var g = Object.create(Ctor.prototype);
                g.__parentStyleSheet = sheet || null;
                g.__parentRule = parentRule || null;
                g.__at = kw;
                g.__prelude = prelude;
                g.__type = kw === 'media' ? 4 : kw === 'supports' ? 12 : 0;
                var cond = prelude.replace(/^@[\w-]+\s*/, '')
                                  .replace(/^\s+|\s+$/g, '');
                g.__condition = cond;
                g.__rules = parseRuleList(block, sheet, g);
                g.__ruleList = makeList();
                syncList(g.__ruleList, g.__rules);
                return g;
            }
            if (kw) {
                var ar = Object.create(CSSRule.prototype);
                ar.__parentStyleSheet = sheet || null;
                ar.__parentRule = parentRule || null;
                ar.__at = kw;
                ar.__type = kw === 'font-face' ? 5 : kw === 'page' ? 6 :
                            kw === 'keyframes' || kw === '-webkit-keyframes' ? 7 :
                            kw === 'counter-style' ? 11 : 0;
                var head = prelude.replace(/\s+/g, ' ').replace(/^ | $/g, '');
                var body = serializeDeclBlock(block);
                var raw = head + (body ? ' { ' + body + ' }' : ' { }');
                ar.__cssText = function () { return raw; };
                return ar;
            }
            if (!preludeSelectorValid(prelude)) return null;
            var r = Object.create(CSSStyleRule.prototype);
            r.__parentStyleSheet = sheet || null;
            r.__parentRule = parentRule || null;
            r.__type = 1;
            r.__selector = prelude;
            var holder = document.createElement('span');
            try { holder.style.cssText = block; } catch (e) {}
            r.__holder = holder;
            r.__style = holder.style;
            return r;
        }

        function parseOne(text, sheet, parentRule) {
            var rules = parseRuleList(String(text), sheet, parentRule);
            if (rules.length !== 1) return null;
            return rules[0];
        }

        function domError(name, msg) {
            try { return new DOMException(msg || name, name); }
            catch (e) {
                var err = new Error(msg || name);
                err.name = name;
                return err;
            }
        }

        function insertInto(owner, rules, list, text, index, topLevel) {
            var len = rules.length;
            if (index === undefined) index = 0;
            index = Number(index);
            if (isNaN(index)) index = 0;
            if (index < 0 || index > len)
                throw domError('IndexSizeError',
                    'insertRule index ' + index + ' out of range');
            var rule = parseOne(text, owner.__parentStyleSheet || owner, owner);
            if (!rule)
                throw domError('SyntaxError', 'failed to parse rule');
            if (!topLevel && (rule.__at === 'import' || rule.__at === 'namespace'))
                throw domError('HierarchyRequestError',
                    '@' + rule.__at + ' not allowed here');
            rules.splice(index, 0, rule);
            syncList(list, rules);
            notify(owner);
            return index;
        }
        function deleteFrom(owner, rules, list, index) {
            index = Number(index) || 0;
            if (index < 0 || index >= rules.length)
                throw domError('IndexSizeError',
                    'deleteRule index ' + index + ' out of range');
            rules.splice(index, 1);
            syncList(list, rules);
            notify(owner);
        }

        var SheetProto = (typeof global.CSSStyleSheet === 'function' &&
                          global.CSSStyleSheet.prototype) || Object.prototype;

        function sheetFor(node) {
            if (node.__ndSheet) return node.__ndSheet;
            var sheet = Object.create(SheetProto);
            var rules = null, list = makeList(), lastText = null;

            function ensure() {
                var txt = '';
                try { txt = node.textContent || ''; } catch (e) {}
                if (rules !== null && txt === lastText) return;
                lastText = txt;
                rules = parseRuleList(txt, sheet, null);
                syncList(list, rules);
            }
            function rebuild() {
                rebuildPending = false;
                try {
                    var t = rules.map(function (r) {
                        return r.cssText;
                    }).join('\n');
                    lastText = t;
                    node.textContent = t;
                } catch (e) {}
            }
            var rebuildPending = false;
            function scheduleRebuild() {
                if (rebuildPending) return;
                rebuildPending = true;
                if (typeof Promise === 'function')
                    Promise.resolve().then(rebuild);
                else
                    rebuild();
            }

            Object.defineProperties(sheet, {
                ownerNode: { value: node, enumerable: true },
                ownerRule: { value: null, enumerable: true },
                parentStyleSheet: { value: null, enumerable: true },
                type: { value: 'text/css', enumerable: true },
                href: { value: null, enumerable: true },
                title: {
                    value: (node.getAttribute &&
                            node.getAttribute('title')) || null,
                    enumerable: true
                },
                media: {
                    value: (node.getAttribute &&
                            node.getAttribute('media')) || '',
                    enumerable: true
                },
                disabled: {
                    enumerable: true, configurable: true,
                    get: function () { return false; },
                    set: function () {}
                },
                cssRules: {
                    enumerable: true, configurable: true,
                    get: function () { ensure(); return list; }
                },
                rules: {
                    enumerable: true, configurable: true,
                    get: function () { ensure(); return list; }
                },
                insertRule: {
                    enumerable: true, configurable: true, writable: true,
                    value: function (text, index) {
                        ensure();
                        return insertInto(sheet, rules, list, text, index, true);
                    }
                },
                deleteRule: {
                    enumerable: true, configurable: true, writable: true,
                    value: function (index) {
                        ensure();
                        return deleteFrom(sheet, rules, list, index);
                    }
                },
                replaceSync: {
                    enumerable: true, configurable: true, writable: true,
                    value: function (text) {
                        rules = parseRuleList(
                            String(text == null ? '' : text), sheet, null);
                        syncList(list, rules);
                        rebuild();
                    }
                },
                replace: {
                    enumerable: true, configurable: true, writable: true,
                    value: function (text) {
                        try {
                            this.replaceSync(text);
                            return Promise.resolve(this);
                        } catch (e) { return Promise.reject(e); }
                    }
                },
                __notify: {
                    configurable: true,
                    value: function () { ensure(); scheduleRebuild(); }
                }
            });

            try {
                Object.defineProperty(node, '__ndSheet', {
                    value: sheet, writable: true,
                    configurable: true, enumerable: false
                });
            } catch (e) { node.__ndSheet = sheet; }
            return sheet;
        }

        function defSheet(proto) {
            if (!proto) return;
            try {
                Object.defineProperty(proto, 'sheet', {
                    configurable: true,
                    get: function () {
                        var nm = this.tagName ? this.tagName.toLowerCase() : '';
                        if (nm === 'style') return sheetFor(this);
                        if (nm === 'link') {
                            var rel = (this.getAttribute &&
                                       this.getAttribute('rel') || '').toLowerCase();
                            if (rel && rel.indexOf('stylesheet') < 0) return null;
                            return sheetFor(this);
                        }
                        return null;
                    }
                });
            } catch (e) {}
        }
        if (global.Element && global.Element.prototype)
            defSheet(global.Element.prototype);
        else {
            defSheet(global.HTMLStyleElement && global.HTMLStyleElement.prototype);
            defSheet(global.HTMLLinkElement && global.HTMLLinkElement.prototype);
        }

        try {
            var styleSheetsDef = {
                configurable: true,
                get: function () {
                    var self = this || document;
                    var nodes;
                    try {
                        nodes = self.querySelectorAll(
                            'style, link[rel~="stylesheet"]');
                    } catch (e) {
                        try { nodes = self.getElementsByTagName('style'); }
                        catch (e2) { nodes = []; }
                    }
                    var slist = [];
                    for (var i = 0; i < nodes.length; i++) {
                        var s = sheetFor(nodes[i]);
                        if (s) slist.push(s);
                    }
                    slist.item = function (i) { return this[i] || null; };
                    return slist;
                }
            };
            if (global.Document && global.Document.prototype)
                Object.defineProperty(global.Document.prototype,
                                      'styleSheets', styleSheetsDef);
            Object.defineProperty(document, 'styleSheets', styleSheetsDef);
        } catch (e) {}
    })();

    /* Advertise the spec-required IntersectionObserverEntry/ResizeObserverEntry
     * prototype members so feature-detecting polyfills (e.g. the
     * "intersection-observer" npm package, which checks 'intersectionRatio' in
     * IntersectionObserverEntry.prototype) detect native support and do NOT
     * replace our working native observers with a JS polyfill that breaks in
     * this engine (Reddit's feed loader relies on a working observer). */
    (function () {
        function ensureProto(ctorName, props) {
            var C = global[ctorName];
            if (typeof C !== 'function' || !C.prototype) return;
            var p = C.prototype, k;
            for (k in props) {
                if (!(k in p)) {
                    try {
                        Object.defineProperty(p, k, {
                            value: props[k], writable: true,
                            configurable: true, enumerable: false
                        });
                    } catch (e) {}
                }
            }
        }
        ensureProto('IntersectionObserverEntry', {
            time: 0, rootBounds: null, boundingClientRect: null,
            intersectionRect: null, isIntersecting: false,
            intersectionRatio: 0, target: null
        });
        ensureProto('ResizeObserverEntry', {
            target: null, contentRect: null,
            borderBoxSize: undefined, contentBoxSize: undefined,
            devicePixelContentBoxSize: undefined
        });
    })();

    /* WHATWG Geometry: full 3D DOMMatrix/DOMMatrixReadOnly (including CSS
     * transform-list string parsing) and DOMPoint/DOMPointReadOnly with
     * matrixTransform. Replaces the native 2D-only DOMMatrix binding and the
     * argument-less DOMPoint shim; CSS-3D pages (PolyCSS, cssQuake) project
     * vertices through new DOMPoint(...).matrixTransform(new DOMMatrix(str)). */
    (function () {
        function identity() {
            return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
        }

        function mul(A, B) {
            var out = new Array(16);
            for (var c = 0; c < 4; c++) {
                for (var r = 0; r < 4; r++) {
                    out[c * 4 + r] =
                        A[r]      * B[c * 4]     +
                        A[4 + r]  * B[c * 4 + 1] +
                        A[8 + r]  * B[c * 4 + 2] +
                        A[12 + r] * B[c * 4 + 3];
                }
            }
            return out;
        }

        function parseAngle(tok) {
            var m = /^([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)(deg|rad|grad|turn)?$/.exec(tok);
            if (!m) return null;
            var v = parseFloat(m[1]);
            switch (m[2]) {
            case 'rad':  return v * 180 / Math.PI;
            case 'grad': return v * 0.9;
            case 'turn': return v * 360;
            default:     return v;
            }
        }

        function parseLength(tok) {
            var m = /^([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)(px)?$/.exec(tok);
            return m ? parseFloat(m[1]) : null;
        }

        function parseNumber(tok) {
            var m = /^([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)$/.exec(tok);
            return m ? parseFloat(m[1]) : null;
        }

        function rotationMatrix(x, y, z, deg) {
            var len = Math.sqrt(x * x + y * y + z * z);
            if (len === 0) return { m: identity(), is2D: true };
            x /= len; y /= len; z /= len;
            var rad = deg * Math.PI / 180;
            var s = Math.sin(rad), c = Math.cos(rad), t = 1 - c;
            return {
                m: [
                    t * x * x + c,     t * x * y + s * z, t * x * z - s * y, 0,
                    t * x * y - s * z, t * y * y + c,     t * y * z + s * x, 0,
                    t * x * z + s * y, t * y * z - s * x, t * z * z + c,     0,
                    0, 0, 0, 1
                ],
                is2D: x === 0 && y === 0
            };
        }

        function parseTransformList(str) {
            var s = String(str).trim();
            if (!s || s === 'none') return { m: identity(), is2D: true };
            var m = identity();
            var is2D = true;
            var re = /([a-zA-Z0-9]+)\s*\(([^)]*)\)/g;
            var match, consumed = 0;
            while ((match = re.exec(s)) !== null) {
                var between = s.slice(consumed, match.index);
                if (/\S/.test(between)) return null;
                consumed = re.lastIndex;
                var fn = match[1].toLowerCase();
                var args = match[2].split(',').map(function (a) { return a.trim(); });
                if (args.length === 1 && args[0] === '') args = [];
                var step = transformFunctionMatrix(fn, args);
                if (!step) return null;
                m = mul(m, step.m);
                if (!step.is2D) is2D = false;
            }
            if (consumed === 0 || /\S/.test(s.slice(consumed))) return null;
            return { m: m, is2D: is2D };
        }

        function transformFunctionMatrix(fn, args) {
            var m = identity();
            var v, i;
            switch (fn) {
            case 'matrix':
                if (args.length !== 6) return null;
                for (i = 0; i < 6; i++) if (parseNumber(args[i]) === null) return null;
                m[0] = parseFloat(args[0]); m[1] = parseFloat(args[1]);
                m[4] = parseFloat(args[2]); m[5] = parseFloat(args[3]);
                m[12] = parseFloat(args[4]); m[13] = parseFloat(args[5]);
                return { m: m, is2D: true };
            case 'matrix3d':
                if (args.length !== 16) return null;
                for (i = 0; i < 16; i++) {
                    v = parseNumber(args[i]);
                    if (v === null) return null;
                    m[i] = v;
                }
                return { m: m, is2D: false };
            case 'translate':
                if (args.length < 1 || args.length > 2) return null;
                m[12] = parseLength(args[0]);
                m[13] = args.length === 2 ? parseLength(args[1]) : 0;
                if (m[12] === null || m[13] === null) return null;
                return { m: m, is2D: true };
            case 'translatex':
                if (args.length !== 1 || (m[12] = parseLength(args[0])) === null) return null;
                return { m: m, is2D: true };
            case 'translatey':
                if (args.length !== 1 || (m[13] = parseLength(args[0])) === null) return null;
                return { m: m, is2D: true };
            case 'translatez':
                if (args.length !== 1 || (m[14] = parseLength(args[0])) === null) return null;
                return { m: m, is2D: false };
            case 'translate3d':
                if (args.length !== 3) return null;
                m[12] = parseLength(args[0]);
                m[13] = parseLength(args[1]);
                m[14] = parseLength(args[2]);
                if (m[12] === null || m[13] === null || m[14] === null) return null;
                return { m: m, is2D: false };
            case 'scale':
                if (args.length < 1 || args.length > 2) return null;
                m[0] = parseNumber(args[0]);
                m[5] = args.length === 2 ? parseNumber(args[1]) : m[0];
                if (m[0] === null || m[5] === null) return null;
                return { m: m, is2D: true };
            case 'scalex':
                if (args.length !== 1 || (m[0] = parseNumber(args[0])) === null) return null;
                return { m: m, is2D: true };
            case 'scaley':
                if (args.length !== 1 || (m[5] = parseNumber(args[0])) === null) return null;
                return { m: m, is2D: true };
            case 'scalez':
                if (args.length !== 1 || (m[10] = parseNumber(args[0])) === null) return null;
                return { m: m, is2D: false };
            case 'scale3d':
                if (args.length !== 3) return null;
                m[0] = parseNumber(args[0]);
                m[5] = parseNumber(args[1]);
                m[10] = parseNumber(args[2]);
                if (m[0] === null || m[5] === null || m[10] === null) return null;
                return { m: m, is2D: false };
            case 'rotate':
            case 'rotatez':
                if (args.length !== 1 || (v = parseAngle(args[0])) === null) return null;
                return { m: rotationMatrix(0, 0, 1, v).m, is2D: true };
            case 'rotatex':
                if (args.length !== 1 || (v = parseAngle(args[0])) === null) return null;
                return { m: rotationMatrix(1, 0, 0, v).m, is2D: false };
            case 'rotatey':
                if (args.length !== 1 || (v = parseAngle(args[0])) === null) return null;
                return { m: rotationMatrix(0, 1, 0, v).m, is2D: false };
            case 'rotate3d':
                if (args.length !== 4) return null;
                var ax = parseNumber(args[0]), ay = parseNumber(args[1]),
                    az = parseNumber(args[2]);
                v = parseAngle(args[3]);
                if (ax === null || ay === null || az === null || v === null) return null;
                return rotationMatrix(ax, ay, az, v);
            case 'skew':
                if (args.length < 1 || args.length > 2) return null;
                v = parseAngle(args[0]);
                var sy = args.length === 2 ? parseAngle(args[1]) : 0;
                if (v === null || sy === null) return null;
                m[4] = Math.tan(v * Math.PI / 180);
                m[1] = Math.tan(sy * Math.PI / 180);
                return { m: m, is2D: true };
            case 'skewx':
                if (args.length !== 1 || (v = parseAngle(args[0])) === null) return null;
                m[4] = Math.tan(v * Math.PI / 180);
                return { m: m, is2D: true };
            case 'skewy':
                if (args.length !== 1 || (v = parseAngle(args[0])) === null) return null;
                m[1] = Math.tan(v * Math.PI / 180);
                return { m: m, is2D: true };
            case 'perspective':
                if (args.length !== 1 || (v = parseLength(args[0])) === null) return null;
                if (v > 0) m[11] = -1 / v;
                return { m: m, is2D: false };
            default:
                return null;
            }
        }

        function readAnyMatrix(other) {
            var m = identity();
            var is2D = true;
            if (other && typeof other === 'object') {
                if (other.__nsM3d) {
                    return { m: other.__nsM3d.slice(), is2D: !!other.__nsIs2D };
                }
                var has3d = typeof other.m33 === 'number' && other.is2D === false;
                m[0]  = numOr(other.m11, numOr(other.a, 1));
                m[1]  = numOr(other.m12, numOr(other.b, 0));
                m[4]  = numOr(other.m21, numOr(other.c, 0));
                m[5]  = numOr(other.m22, numOr(other.d, 1));
                m[12] = numOr(other.m41, numOr(other.e, 0));
                m[13] = numOr(other.m42, numOr(other.f, 0));
                if (has3d) {
                    m[2]  = numOr(other.m13, 0); m[3]  = numOr(other.m14, 0);
                    m[6]  = numOr(other.m23, 0); m[7]  = numOr(other.m24, 0);
                    m[8]  = numOr(other.m31, 0); m[9]  = numOr(other.m32, 0);
                    m[10] = numOr(other.m33, 1); m[11] = numOr(other.m34, 0);
                    m[14] = numOr(other.m43, 0); m[15] = numOr(other.m44, 1);
                    is2D = false;
                }
            }
            return { m: m, is2D: is2D };
        }

        function numOr(v, dflt) {
            return typeof v === 'number' && isFinite(v) ? v : dflt;
        }

        function invert(m) {
            var inv = new Array(16);
            inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] +
                     m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
            inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] -
                     m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
            inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] +
                     m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
            inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] -
                      m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
            inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] -
                     m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
            inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] +
                     m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
            inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] -
                     m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
            inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] +
                      m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
            inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] +
                     m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
            inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] -
                     m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
            inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] +
                      m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
            inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] -
                      m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
            inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] -
                     m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
            inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] +
                     m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
            inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] -
                      m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
            inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] +
                      m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
            var det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
            if (det === 0 || !isFinite(det)) return null;
            for (var i = 0; i < 16; i++) inv[i] /= det;
            return inv;
        }

        var FIELDS = [
            'm11', 'm12', 'm13', 'm14', 'm21', 'm22', 'm23', 'm24',
            'm31', 'm32', 'm33', 'm34', 'm41', 'm42', 'm43', 'm44'
        ];
        var ALIASES = { a: 0, b: 1, c: 4, d: 5, e: 12, f: 13 };

        function NSDOMMatrixReadOnly(init) {
            defineMatrix(this, init, true);
        }

        function NSDOMMatrix(init) {
            defineMatrix(this, init, false);
        }

        function defineMatrix(self, init, readonly) {
            var parsed;
            if (init === undefined || init === null) {
                parsed = { m: identity(), is2D: true };
            } else if (typeof init === 'string') {
                parsed = parseTransformList(init);
                if (!parsed) throw new SyntaxError(
                    'Failed to construct DOMMatrix: invalid transform list: ' + init);
            } else if (Array.isArray(init) ||
                       (typeof init.length === 'number' && typeof init !== 'function')) {
                var arr = Array.prototype.slice.call(init);
                if (arr.length === 6) {
                    parsed = { m: identity(), is2D: true };
                    parsed.m[0] = +arr[0]; parsed.m[1] = +arr[1];
                    parsed.m[4] = +arr[2]; parsed.m[5] = +arr[3];
                    parsed.m[12] = +arr[4]; parsed.m[13] = +arr[5];
                } else if (arr.length === 16) {
                    parsed = { m: arr.map(Number), is2D: false };
                } else {
                    throw new TypeError(
                        'Failed to construct DOMMatrix: expected 6 or 16 elements');
                }
            } else {
                parsed = readAnyMatrix(init);
            }
            self.__nsM3d = parsed.m;
            self.__nsIs2D = parsed.is2D;
            self.__nsReadonly = readonly;
        }

        function getter(idx) {
            return function () { return this.__nsM3d[idx]; };
        }

        function setter(idx) {
            return function (v) {
                if (this.__nsReadonly) return;
                this.__nsM3d[idx] = +v;
                if (idx === 2 || idx === 3 || idx === 6 || idx === 7 ||
                    idx === 8 || idx === 9 || idx === 11 || idx === 14 ||
                    (idx === 10 && +v !== 1) || (idx === 15 && +v !== 1))
                    this.__nsIs2D = false;
            };
        }

        function installAccessors(proto) {
            var i, k;
            for (i = 0; i < 16; i++) {
                Object.defineProperty(proto, FIELDS[i], {
                    get: getter(i), set: setter(i),
                    configurable: true, enumerable: true
                });
            }
            for (k in ALIASES) {
                Object.defineProperty(proto, k, {
                    get: getter(ALIASES[k]), set: setter(ALIASES[k]),
                    configurable: true, enumerable: true
                });
            }
            Object.defineProperty(proto, 'is2D', {
                get: function () { return this.__nsIs2D; },
                configurable: true, enumerable: true
            });
            Object.defineProperty(proto, 'isIdentity', {
                get: function () {
                    var id = identity();
                    for (var i = 0; i < 16; i++)
                        if (this.__nsM3d[i] !== id[i]) return false;
                    return true;
                },
                configurable: true, enumerable: true
            });
        }

        installAccessors(NSDOMMatrixReadOnly.prototype);
        NSDOMMatrix.prototype = Object.create(NSDOMMatrixReadOnly.prototype);
        NSDOMMatrix.prototype.constructor = NSDOMMatrix;

        function makeMatrix(m, is2D) {
            var out = new NSDOMMatrix();
            out.__nsM3d = m;
            out.__nsIs2D = is2D;
            return out;
        }

        NSDOMMatrixReadOnly.prototype.multiply = function (other) {
            var o = readAnyMatrix(other);
            return makeMatrix(mul(this.__nsM3d, o.m), this.__nsIs2D && o.is2D);
        };
        NSDOMMatrixReadOnly.prototype.translate = function (tx, ty, tz) {
            tx = +tx || 0; ty = +ty || 0; tz = +tz || 0;
            var t = identity();
            t[12] = tx; t[13] = ty; t[14] = tz;
            return makeMatrix(mul(this.__nsM3d, t), this.__nsIs2D && tz === 0);
        };
        NSDOMMatrixReadOnly.prototype.scale = function (sx, sy, sz, ox, oy, oz) {
            sx = sx === undefined ? 1 : +sx;
            sy = sy === undefined ? sx : +sy;
            sz = sz === undefined ? 1 : +sz;
            ox = +ox || 0; oy = +oy || 0; oz = +oz || 0;
            var r = this.translate(ox, oy, oz);
            var s = identity();
            s[0] = sx; s[5] = sy; s[10] = sz;
            r = makeMatrix(mul(r.__nsM3d, s), r.__nsIs2D && sz === 1);
            return r.translate(-ox, -oy, -oz);
        };
        NSDOMMatrixReadOnly.prototype.scale3d = function (s, ox, oy, oz) {
            return this.scale(s, s, s, ox, oy, oz);
        };
        NSDOMMatrixReadOnly.prototype.rotate = function (rx, ry, rz) {
            if (ry === undefined && rz === undefined) { rz = +rx || 0; rx = 0; ry = 0; }
            else { rx = +rx || 0; ry = +ry || 0; rz = +rz || 0; }
            var m = this.__nsM3d;
            m = mul(m, rotationMatrix(0, 0, 1, rz).m);
            m = mul(m, rotationMatrix(0, 1, 0, ry).m);
            m = mul(m, rotationMatrix(1, 0, 0, rx).m);
            return makeMatrix(m, this.__nsIs2D && rx === 0 && ry === 0);
        };
        NSDOMMatrixReadOnly.prototype.rotateAxisAngle = function (x, y, z, deg) {
            var r = rotationMatrix(+x || 0, +y || 0, +z || 0, +deg || 0);
            return makeMatrix(mul(this.__nsM3d, r.m), this.__nsIs2D && r.is2D);
        };
        NSDOMMatrixReadOnly.prototype.skewX = function (deg) {
            var t = identity();
            t[4] = Math.tan((+deg || 0) * Math.PI / 180);
            return makeMatrix(mul(this.__nsM3d, t), this.__nsIs2D);
        };
        NSDOMMatrixReadOnly.prototype.skewY = function (deg) {
            var t = identity();
            t[1] = Math.tan((+deg || 0) * Math.PI / 180);
            return makeMatrix(mul(this.__nsM3d, t), this.__nsIs2D);
        };
        NSDOMMatrixReadOnly.prototype.inverse = function () {
            var inv = invert(this.__nsM3d);
            if (!inv) {
                var nan = makeMatrix(identity().map(function () { return NaN; }), false);
                return nan;
            }
            return makeMatrix(inv, this.__nsIs2D);
        };
        NSDOMMatrixReadOnly.prototype.flipX = function () {
            var t = identity();
            t[0] = -1;
            return makeMatrix(mul(this.__nsM3d, t), this.__nsIs2D);
        };
        NSDOMMatrixReadOnly.prototype.flipY = function () {
            var t = identity();
            t[5] = -1;
            return makeMatrix(mul(this.__nsM3d, t), this.__nsIs2D);
        };
        NSDOMMatrixReadOnly.prototype.transformPoint = function (p) {
            var x = 0, y = 0, z = 0, w = 1;
            if (p && typeof p === 'object') {
                x = +p.x || 0; y = +p.y || 0; z = +p.z || 0;
                w = p.w === undefined ? 1 : +p.w;
            }
            var m = this.__nsM3d;
            return new NSDOMPoint(
                m[0] * x + m[4] * y + m[8] * z + m[12] * w,
                m[1] * x + m[5] * y + m[9] * z + m[13] * w,
                m[2] * x + m[6] * y + m[10] * z + m[14] * w,
                m[3] * x + m[7] * y + m[11] * z + m[15] * w);
        };
        NSDOMMatrixReadOnly.prototype.toFloat32Array = function () {
            return typeof Float32Array === 'function'
                ? new Float32Array(this.__nsM3d) : this.__nsM3d.slice();
        };
        NSDOMMatrixReadOnly.prototype.toFloat64Array = function () {
            return typeof Float64Array === 'function'
                ? new Float64Array(this.__nsM3d) : this.__nsM3d.slice();
        };
        NSDOMMatrixReadOnly.prototype.toString = function () {
            var m = this.__nsM3d;
            if (this.__nsIs2D) {
                return 'matrix(' + [m[0], m[1], m[4], m[5], m[12], m[13]].join(', ') + ')';
            }
            return 'matrix3d(' + m.join(', ') + ')';
        };
        NSDOMMatrixReadOnly.prototype.toJSON = function () {
            var out = {}, i, k;
            for (i = 0; i < 16; i++) out[FIELDS[i]] = this.__nsM3d[i];
            for (k in ALIASES) out[k] = this.__nsM3d[ALIASES[k]];
            out.is2D = this.__nsIs2D;
            out.isIdentity = this.isIdentity;
            return out;
        };

        function mutSelf(name, base) {
            NSDOMMatrix.prototype[name] = function () {
                var r = base.apply(this, arguments);
                this.__nsM3d = r.__nsM3d;
                this.__nsIs2D = r.__nsIs2D;
                return this;
            };
        }
        mutSelf('multiplySelf',        NSDOMMatrixReadOnly.prototype.multiply);
        mutSelf('translateSelf',       NSDOMMatrixReadOnly.prototype.translate);
        mutSelf('scaleSelf',           NSDOMMatrixReadOnly.prototype.scale);
        mutSelf('scale3dSelf',         NSDOMMatrixReadOnly.prototype.scale3d);
        mutSelf('rotateSelf',          NSDOMMatrixReadOnly.prototype.rotate);
        mutSelf('rotateAxisAngleSelf', NSDOMMatrixReadOnly.prototype.rotateAxisAngle);
        mutSelf('skewXSelf',           NSDOMMatrixReadOnly.prototype.skewX);
        mutSelf('skewYSelf',           NSDOMMatrixReadOnly.prototype.skewY);
        mutSelf('invertSelf',          NSDOMMatrixReadOnly.prototype.inverse);
        NSDOMMatrix.prototype.preMultiplySelf = function (other) {
            var o = readAnyMatrix(other);
            this.__nsIs2D = this.__nsIs2D && o.is2D;
            this.__nsM3d = mul(o.m, this.__nsM3d);
            return this;
        };
        NSDOMMatrix.prototype.setMatrixValue = function (str) {
            var parsed = parseTransformList(str);
            if (!parsed) throw new SyntaxError(
                'Failed to set matrix value: invalid transform list: ' + str);
            this.__nsM3d = parsed.m;
            this.__nsIs2D = parsed.is2D;
            return this;
        };

        function fromMatrixImpl(Ctor) {
            return function (other) { return new Ctor(other); };
        }
        function fromArrayImpl(Ctor) {
            return function (arr) {
                return new Ctor(Array.prototype.slice.call(arr));
            };
        }
        NSDOMMatrix.fromMatrix = fromMatrixImpl(NSDOMMatrix);
        NSDOMMatrix.fromFloat32Array = fromArrayImpl(NSDOMMatrix);
        NSDOMMatrix.fromFloat64Array = fromArrayImpl(NSDOMMatrix);
        NSDOMMatrixReadOnly.fromMatrix = fromMatrixImpl(NSDOMMatrixReadOnly);
        NSDOMMatrixReadOnly.fromFloat32Array = fromArrayImpl(NSDOMMatrixReadOnly);
        NSDOMMatrixReadOnly.fromFloat64Array = fromArrayImpl(NSDOMMatrixReadOnly);

        function NSDOMPointReadOnly(x, y, z, w) {
            this.x = x === undefined ? 0 : +x;
            this.y = y === undefined ? 0 : +y;
            this.z = z === undefined ? 0 : +z;
            this.w = w === undefined ? 1 : +w;
        }
        NSDOMPointReadOnly.prototype.matrixTransform = function (m) {
            var mat = (m && m.__nsM3d) ? m : new NSDOMMatrixReadOnly(m);
            return mat.transformPoint(this);
        };
        NSDOMPointReadOnly.prototype.toJSON = function () {
            return { x: this.x, y: this.y, z: this.z, w: this.w };
        };
        NSDOMPointReadOnly.fromPoint = function (p) {
            p = p || {};
            return new NSDOMPointReadOnly(p.x, p.y, p.z, p.w);
        };

        function NSDOMPoint(x, y, z, w) {
            NSDOMPointReadOnly.call(this, x, y, z, w);
        }
        NSDOMPoint.prototype = Object.create(NSDOMPointReadOnly.prototype);
        NSDOMPoint.prototype.constructor = NSDOMPoint;
        NSDOMPoint.fromPoint = function (p) {
            p = p || {};
            return new NSDOMPoint(p.x, p.y, p.z, p.w);
        };

        global.DOMMatrix = NSDOMMatrix;
        global.DOMMatrixReadOnly = NSDOMMatrixReadOnly;
        global.WebKitCSSMatrix = NSDOMMatrix;
        global.DOMPoint = NSDOMPoint;
        global.DOMPointReadOnly = NSDOMPointReadOnly;
    })();

    (function () {
        var doc = global.document;
        if (!doc || typeof doc.createElement !== 'function') return;

        function domEx(name) {
            try { return new DOMException(name, name); }
            catch (e) {
                var err = new Error(name);
                err.name = name;
                return err;
            }
        }

        function rangeEx(code) {
            var err = new Error('RangeException ' + code);
            err.name = 'RangeException';
            err.code = code;
            err.BAD_BOUNDARYPOINTS_ERR = 1;
            err.INVALID_NODE_TYPE_ERR = 2;
            return err;
        }

        function isCharData(n) {
            var t = n.nodeType;
            return t === 3 || t === 4 || t === 7 || t === 8;
        }

        function isTextNode(n) {
            return n.nodeType === 3 || n.nodeType === 4;
        }

        function nodeLength(n) {
            if (n.nodeType === 10 || n.nodeType === 2) return 0;
            if (isCharData(n)) return n.data ? n.data.length : 0;
            return n.childNodes ? n.childNodes.length : 0;
        }

        function indexOfNode(n) {
            var i = 0;
            while ((n = n.previousSibling)) i++;
            return i;
        }

        function rootOf(n) {
            while (n.parentNode) n = n.parentNode;
            return n;
        }

        function ownerDoc(n) {
            if (n.nodeType === 9) return n;
            return n.ownerDocument || doc;
        }

        function isInclusiveAncestor(a, b) {
            while (b) {
                if (b === a) return true;
                b = b.parentNode;
            }
            return false;
        }

        function pathTo(n) {
            var p = [];
            while (n) { p.unshift(n); n = n.parentNode; }
            return p;
        }

        function bpCompare(nodeA, offsetA, nodeB, offsetB) {
            if (nodeA === nodeB)
                return offsetA < offsetB ? -1 : offsetA > offsetB ? 1 : 0;
            var pa = pathTo(nodeA), pb = pathTo(nodeB);
            var i = 0;
            while (i < pa.length && i < pb.length && pa[i] === pb[i]) i++;
            if (i === pa.length)
                return indexOfNode(pb[i]) < offsetA ? 1 : -1;
            if (i === pb.length)
                return indexOfNode(pa[i]) < offsetB ? -1 : 1;
            return indexOfNode(pa[i]) < indexOfNode(pb[i]) ? -1 : 1;
        }

        function replaceData(n, off, count, s) {
            if (typeof n.replaceData === 'function') {
                n.replaceData(off, count, s);
                return;
            }
            var d = n.data || '';
            n.data = d.substring(0, off) + s + d.substring(off + count);
        }

        function checkBoundary(node, offset) {
            if (!node || typeof node.nodeType !== 'number')
                throw new TypeError('parameter 1 is not of type Node');
            if (node.nodeType === 10) throw domEx('InvalidNodeTypeError');
            offset = Number(offset) >>> 0;
            if (offset > nodeLength(node)) throw domEx('IndexSizeError');
            return offset;
        }

        var liveRanges = [];
        var canTrack = typeof WeakRef === 'function';

        function trackRange(r) {
            if (canTrack) liveRanges.push(new WeakRef(r));
            return r;
        }

        function forEachLiveRange(cb) {
            if (!canTrack) return;
            var kept = 0;
            for (var i = 0; i < liveRanges.length; i++) {
                var r = liveRanges[i].deref();
                if (r === undefined) continue;
                liveRanges[kept++] = liveRanges[i];
                cb(r);
            }
            liveRanges.length = kept;
        }

        function rangeReplaceData(node, offset, count, newLength) {
            var delta = newLength - count;
            forEachLiveRange(function (r) {
                var o;
                if (r._sc === node) {
                    o = r._so;
                    if (o > offset && o <= offset + count) r._so = offset;
                    else if (o > offset + count) r._so = o + delta;
                }
                if (r._ec === node) {
                    o = r._eo;
                    if (o > offset && o <= offset + count) r._eo = offset;
                    else if (o > offset + count) r._eo = o + delta;
                }
            });
        }

        function NdRange(ownerDoc) {
            var d = ownerDoc || doc;
            this._sc = d; this._so = 0;
            this._ec = d; this._eo = 0;
            trackRange(this);
        }

        function mkRange(sc, so, ec, eo) {
            var r = new NdRange();
            r._sc = sc; r._so = so; r._ec = ec; r._eo = eo;
            return r;
        }

        function containedIn(n, r) {
            return rootOf(n) === rootOf(r._sc) &&
                   bpCompare(n, 0, r._sc, r._so) > 0 &&
                   bpCompare(n, nodeLength(n), r._ec, r._eo) < 0;
        }

        function partiallyContainedIn(n, r) {
            var a = isInclusiveAncestor(n, r._sc);
            var b = isInclusiveAncestor(n, r._ec);
            return (a && !b) || (b && !a);
        }

        Object.defineProperties(NdRange.prototype, {
            startContainer: { get: function () { return this._sc; }, configurable: true },
            startOffset:    { get: function () { return this._so; }, configurable: true },
            endContainer:   { get: function () { return this._ec; }, configurable: true },
            endOffset:      { get: function () { return this._eo; }, configurable: true },
            collapsed:      { get: function () {
                return this._sc === this._ec && this._so === this._eo;
            }, configurable: true },
            commonAncestorContainer: { get: function () {
                for (var a = this._sc; a; a = a.parentNode)
                    if (isInclusiveAncestor(a, this._ec)) return a;
                return null;
            }, configurable: true }
        });

        NdRange.prototype.setStart = function (node, offset) {
            offset = checkBoundary(node, offset);
            this._sc = node; this._so = offset;
            if (rootOf(node) !== rootOf(this._ec) ||
                bpCompare(node, offset, this._ec, this._eo) > 0) {
                this._ec = node; this._eo = offset;
            }
        };

        NdRange.prototype.setEnd = function (node, offset) {
            offset = checkBoundary(node, offset);
            this._ec = node; this._eo = offset;
            if (rootOf(node) !== rootOf(this._sc) ||
                bpCompare(node, offset, this._sc, this._so) < 0) {
                this._sc = node; this._so = offset;
            }
        };

        NdRange.prototype.setStartBefore = function (node) {
            if (!node || typeof node.nodeType !== 'number')
                throw new TypeError('parameter 1 is not of type Node');
            var parent = node.parentNode;
            if (!parent) throw domEx('InvalidNodeTypeError');
            this.setStart(parent, indexOfNode(node));
        };

        NdRange.prototype.setStartAfter = function (node) {
            if (!node || typeof node.nodeType !== 'number')
                throw new TypeError('parameter 1 is not of type Node');
            var parent = node.parentNode;
            if (!parent) throw domEx('InvalidNodeTypeError');
            this.setStart(parent, indexOfNode(node) + 1);
        };

        NdRange.prototype.setEndBefore = function (node) {
            if (!node || typeof node.nodeType !== 'number')
                throw new TypeError('parameter 1 is not of type Node');
            var parent = node.parentNode;
            if (!parent) throw domEx('InvalidNodeTypeError');
            this.setEnd(parent, indexOfNode(node));
        };

        NdRange.prototype.setEndAfter = function (node) {
            if (!node || typeof node.nodeType !== 'number')
                throw new TypeError('parameter 1 is not of type Node');
            var parent = node.parentNode;
            if (!parent) throw domEx('InvalidNodeTypeError');
            this.setEnd(parent, indexOfNode(node) + 1);
        };

        NdRange.prototype.collapse = function (toStart) {
            if (toStart) { this._ec = this._sc; this._eo = this._so; }
            else { this._sc = this._ec; this._so = this._eo; }
        };

        NdRange.prototype.selectNode = function (node) {
            if (!node || typeof node.nodeType !== 'number')
                throw new TypeError('parameter 1 is not of type Node');
            var parent = node.parentNode;
            if (!parent) throw domEx('InvalidNodeTypeError');
            var i = indexOfNode(node);
            this._sc = parent; this._so = i;
            this._ec = parent; this._eo = i + 1;
        };

        NdRange.prototype.selectNodeContents = function (node) {
            if (!node || typeof node.nodeType !== 'number')
                throw new TypeError('parameter 1 is not of type Node');
            if (node.nodeType === 10) throw domEx('InvalidNodeTypeError');
            this._sc = node; this._so = 0;
            this._ec = node; this._eo = nodeLength(node);
        };

        NdRange.prototype.cloneRange = function () {
            return mkRange(this._sc, this._so, this._ec, this._eo);
        };

        NdRange.prototype.detach = function () {};

        NdRange.prototype.comparePoint = function (node, offset) {
            if (!node || typeof node.nodeType !== 'number')
                throw new TypeError('parameter 1 is not of type Node');
            if (rootOf(node) !== rootOf(this._sc))
                throw domEx('WrongDocumentError');
            if (node.nodeType === 10) throw domEx('InvalidNodeTypeError');
            offset = Number(offset) >>> 0;
            if (offset > nodeLength(node)) throw domEx('IndexSizeError');
            if (bpCompare(node, offset, this._sc, this._so) < 0) return -1;
            if (bpCompare(node, offset, this._ec, this._eo) > 0) return 1;
            return 0;
        };

        NdRange.prototype.isPointInRange = function (node, offset) {
            if (!node || typeof node.nodeType !== 'number')
                throw new TypeError('parameter 1 is not of type Node');
            if (rootOf(node) !== rootOf(this._sc)) return false;
            if (node.nodeType === 10) throw domEx('InvalidNodeTypeError');
            offset = Number(offset) >>> 0;
            if (offset > nodeLength(node)) throw domEx('IndexSizeError');
            return bpCompare(node, offset, this._sc, this._so) >= 0 &&
                   bpCompare(node, offset, this._ec, this._eo) <= 0;
        };

        NdRange.prototype.intersectsNode = function (node) {
            if (!node || typeof node.nodeType !== 'number')
                throw new TypeError('parameter 1 is not of type Node');
            if (rootOf(node) !== rootOf(this._sc)) return false;
            var parent = node.parentNode;
            if (!parent) return true;
            var offset = indexOfNode(node);
            return bpCompare(parent, offset, this._ec, this._eo) < 0 &&
                   bpCompare(parent, offset + 1, this._sc, this._so) > 0;
        };

        NdRange.prototype.compareBoundaryPoints = function (how, sourceRange) {
            how = (how >>> 0) & 0xffff;
            if (!sourceRange || !(sourceRange instanceof NdRange))
                throw new TypeError('parameter 2 is not of type Range');
            if (how > 3) throw domEx('NotSupportedError');
            if (rootOf(this._sc) !== rootOf(sourceRange._sc))
                throw domEx('WrongDocumentError');
            var tn, to, on, oo;
            if (how === 0) { tn = this._sc; to = this._so; on = sourceRange._sc; oo = sourceRange._so; }
            else if (how === 1) { tn = this._ec; to = this._eo; on = sourceRange._sc; oo = sourceRange._so; }
            else if (how === 2) { tn = this._ec; to = this._eo; on = sourceRange._ec; oo = sourceRange._eo; }
            else { tn = this._sc; to = this._so; on = sourceRange._ec; oo = sourceRange._eo; }
            return bpCompare(tn, to, on, oo);
        };

        NdRange.prototype.toString = function () {
            var sc = this._sc, so = this._so, ec = this._ec, eo = this._eo;
            if (sc === ec && isTextNode(sc))
                return (sc.data || '').substring(so, eo);
            if (sc === ec && sc.nodeType === 2) {
                var av = '', akids = sc.childNodes || [];
                for (var ai = so; ai < eo && ai < akids.length; ai++)
                    if (isTextNode(akids[ai])) av += akids[ai].data || '';
                return av;
            }
            var s = '';
            if (isTextNode(sc)) s += (sc.data || '').substring(so);
            var self = this;
            (function walk(n) {
                for (var c = n.firstChild; c; c = c.nextSibling) {
                    if (isTextNode(c) && containedIn(c, self)) s += c.data || '';
                    walk(c);
                }
            })(this.commonAncestorContainer || rootOf(sc));
            if (isTextNode(ec) && ec !== sc) s += (ec.data || '').substring(0, eo);
            return s;
        };

        function collectContainedRoots(r) {
            var out = [];
            (function walk(n) {
                for (var c = n.firstChild; c; c = c.nextSibling) {
                    if (containedIn(c, r)) { out.push(c); continue; }
                    walk(c);
                }
            })(r.commonAncestorContainer || rootOf(r._sc));
            return out;
        }

        function cloneOrExtract(r, extract) {
            var sc = r._sc, so = r._so, ec = r._ec, eo = r._eo;
            var frag = ownerDoc(sc).createDocumentFragment();
            if (r.collapsed) return frag;
            if (sc === ec && sc.nodeType === 2) {
                var akids = sc.childNodes || [];
                for (var ai = so; ai < eo && ai < akids.length; ai++) {
                    if (extract) frag.appendChild(akids[ai]);
                    else frag.appendChild(akids[ai].cloneNode(true));
                }
                if (extract) {
                    sc.value = '';
                    r._sc = sc; r._so = 0; r._ec = sc; r._eo = 0;
                }
                return frag;
            }
            if (sc === ec && isCharData(sc)) {
                var c0 = sc.cloneNode(false);
                c0.data = (sc.data || '').substring(so, eo);
                frag.appendChild(c0);
                if (extract) replaceData(sc, so, eo - so, '');
                return frag;
            }
            var commonAncestor = r.commonAncestorContainer;
            var firstPartial = null, lastPartial = null;
            if (!isInclusiveAncestor(sc, ec))
                for (var f = commonAncestor.firstChild; f; f = f.nextSibling)
                    if (partiallyContainedIn(f, r)) { firstPartial = f; break; }
            if (!isInclusiveAncestor(ec, sc))
                for (var l = commonAncestor.lastChild; l; l = l.previousSibling)
                    if (partiallyContainedIn(l, r)) { lastPartial = l; break; }
            var contained = [];
            for (var ch = commonAncestor.firstChild; ch; ch = ch.nextSibling)
                if (containedIn(ch, r)) {
                    if (ch.nodeType === 10) throw domEx('HierarchyRequestError');
                    contained.push(ch);
                }
            var newNode = null, newOffset = 0;
            if (extract) {
                if (isInclusiveAncestor(sc, ec)) {
                    newNode = sc; newOffset = so;
                } else {
                    var ref = sc;
                    while (ref.parentNode &&
                           !isInclusiveAncestor(ref.parentNode, ec))
                        ref = ref.parentNode;
                    newNode = ref.parentNode;
                    newOffset = indexOfNode(ref) + 1;
                }
            }
            if (firstPartial && isCharData(firstPartial)) {
                var c1 = sc.cloneNode(false);
                c1.data = (sc.data || '').substring(so);
                frag.appendChild(c1);
                if (extract) replaceData(sc, so, nodeLength(sc) - so, '');
            } else if (firstPartial) {
                var c2 = firstPartial.cloneNode(false);
                frag.appendChild(c2);
                var sub1 = mkRange(sc, so, firstPartial, nodeLength(firstPartial));
                c2.appendChild(cloneOrExtract(sub1, extract));
            }
            for (var i = 0; i < contained.length; i++) {
                if (extract) frag.appendChild(contained[i]);
                else frag.appendChild(contained[i].cloneNode(true));
            }
            if (lastPartial && isCharData(lastPartial)) {
                var c3 = ec.cloneNode(false);
                c3.data = (ec.data || '').substring(0, eo);
                frag.appendChild(c3);
                if (extract) replaceData(ec, 0, eo, '');
            } else if (lastPartial) {
                var c4 = lastPartial.cloneNode(false);
                frag.appendChild(c4);
                var sub2 = mkRange(lastPartial, 0, ec, eo);
                c4.appendChild(cloneOrExtract(sub2, extract));
            }
            if (extract) {
                r._sc = newNode; r._so = newOffset;
                r._ec = newNode; r._eo = newOffset;
            }
            return frag;
        }

        NdRange.prototype.cloneContents = function () {
            return cloneOrExtract(this, false);
        };

        NdRange.prototype.extractContents = function () {
            return cloneOrExtract(this, true);
        };

        NdRange.prototype.deleteContents = function () {
            if (this.collapsed) return;
            var sc = this._sc, so = this._so, ec = this._ec, eo = this._eo;
            if (sc === ec && isCharData(sc)) {
                replaceData(sc, so, eo - so, '');
                return;
            }
            var toRemove = collectContainedRoots(this);
            var newNode, newOffset;
            if (isInclusiveAncestor(sc, ec)) {
                newNode = sc; newOffset = so;
            } else {
                var ref = sc;
                while (ref.parentNode && !isInclusiveAncestor(ref.parentNode, ec))
                    ref = ref.parentNode;
                newNode = ref.parentNode;
                newOffset = indexOfNode(ref) + 1;
            }
            if (isCharData(sc)) replaceData(sc, so, nodeLength(sc) - so, '');
            for (var i = 0; i < toRemove.length; i++)
                if (toRemove[i].parentNode)
                    toRemove[i].parentNode.removeChild(toRemove[i]);
            if (isCharData(ec)) replaceData(ec, 0, eo, '');
            this._sc = newNode; this._so = newOffset;
            this._ec = newNode; this._eo = newOffset;
        };

        function countElementChildren(p) {
            var n = 0;
            for (var c = p.firstChild; c; c = c.nextSibling)
                if (c.nodeType === 1) n++;
            return n;
        }
        function hasChildOfType(p, t) {
            for (var c = p.firstChild; c; c = c.nextSibling)
                if (c.nodeType === t) return true;
            return false;
        }
        function doctypeFollowing(child) {
            for (var c = child.nextSibling; c; c = c.nextSibling)
                if (c.nodeType === 10) return true;
            return false;
        }
        function elementPreceding(child) {
            for (var c = child.previousSibling; c; c = c.previousSibling)
                if (c.nodeType === 1) return true;
            return false;
        }

        function ensurePreInsertion(node, parent, child) {
            var pt = parent.nodeType;
            if (pt !== 1 && pt !== 9 && pt !== 11)
                throw domEx('HierarchyRequestError');
            if (isInclusiveAncestor(node, parent))
                throw domEx('HierarchyRequestError');
            if (child && child.parentNode !== parent)
                throw domEx('NotFoundError');
            var nt = node.nodeType;
            if (nt !== 1 && nt !== 3 && nt !== 4 && nt !== 7 && nt !== 8 &&
                nt !== 10 && nt !== 11)
                throw domEx('HierarchyRequestError');
            if ((nt === 3 || nt === 4) && pt === 9)
                throw domEx('HierarchyRequestError');
            if (nt === 10 && pt !== 9)
                throw domEx('HierarchyRequestError');
            if (pt !== 9) return;
            if (nt === 11) {
                var elems = countElementChildren(node);
                if (elems > 1 || hasChildOfType(node, 3))
                    throw domEx('HierarchyRequestError');
                if (elems === 1 && (countElementChildren(parent) > 0 ||
                    (child && child.nodeType === 10) ||
                    (child && doctypeFollowing(child))))
                    throw domEx('HierarchyRequestError');
            } else if (nt === 1) {
                if (countElementChildren(parent) > 0 ||
                    (child && child.nodeType === 10) ||
                    (child && doctypeFollowing(child)))
                    throw domEx('HierarchyRequestError');
            } else if (nt === 10) {
                if (hasChildOfType(parent, 10) ||
                    (child && elementPreceding(child)) ||
                    (!child && countElementChildren(parent) > 0))
                    throw domEx('HierarchyRequestError');
            }
        }

        NdRange.prototype.insertNode = function (node) {
            if (!node || typeof node.nodeType !== 'number')
                throw new TypeError('parameter 1 is not of type Node');
            var sc = this._sc, so = this._so;
            var sct = sc.nodeType;
            if (sct === 7 || sct === 8 ||
                (isTextNode(sc) && !sc.parentNode) || node === sc)
                throw domEx('HierarchyRequestError');
            var referenceNode;
            if (isTextNode(sc)) referenceNode = sc;
            else referenceNode = (sct !== 2 && sc.childNodes &&
                                  sc.childNodes[so]) || null;
            var parent = referenceNode ? referenceNode.parentNode : sc;
            ensurePreInsertion(node, parent, referenceNode);
            if (isTextNode(sc)) referenceNode = sc.splitText(so);
            if (node === referenceNode) referenceNode = referenceNode.nextSibling;
            if (node.parentNode) node.parentNode.removeChild(node);
            var newOffset = referenceNode ? indexOfNode(referenceNode)
                                          : nodeLength(parent);
            newOffset += node.nodeType === 11 ? nodeLength(node) : 1;
            var wasCollapsed = this.collapsed;
            parent.insertBefore(node, referenceNode);
            if (wasCollapsed) { this._ec = parent; this._eo = newOffset; }
        };

        NdRange.prototype.surroundContents = function (newParent) {
            if (!newParent || typeof newParent.nodeType !== 'number')
                throw new TypeError('parameter 1 is not of type Node');
            for (var a = this._sc; a && !isInclusiveAncestor(a, this._ec);
                 a = a.parentNode)
                if (!isTextNode(a)) throw domEx('InvalidStateError');
            for (var b = this._ec; b && !isInclusiveAncestor(b, this._sc);
                 b = b.parentNode)
                if (!isTextNode(b)) throw domEx('InvalidStateError');
            var nt = newParent.nodeType;
            if (nt === 9 || nt === 10 || nt === 11)
                throw domEx('InvalidNodeTypeError');
            var fragment = this.extractContents();
            while (newParent.firstChild)
                newParent.removeChild(newParent.firstChild);
            this.insertNode(newParent);
            newParent.appendChild(fragment);
            this.selectNode(newParent);
        };

        NdRange.prototype.createContextualFragment = function (html) {
            var node = this._sc;
            var el = node.nodeType === 1 ? node
                   : isCharData(node) ? node.parentNode : null;
            var tag = el && el.nodeType === 1 ? el.localName : 'body';
            if (tag === 'html') tag = 'body';
            var scratch;
            try { scratch = doc.createElement(tag); }
            catch (e) { scratch = doc.createElement('body'); }
            scratch.innerHTML = String(html);
            var frag = ownerDoc(node).createDocumentFragment();
            while (scratch.firstChild) frag.appendChild(scratch.firstChild);
            return frag;
        };

        function nativeProxy(self) {
            if (typeof global.__ndNativeRange !== 'function') return null;
            var r = global.__ndNativeRange();
            r.startContainer = self._sc;
            r.startOffset = self._so;
            r.endContainer = self._ec;
            r.endOffset = self._eo;
            r.collapsed = self.collapsed;
            return r;
        }

        NdRange.prototype.getBoundingClientRect = function () {
            var r = nativeProxy(this);
            return r ? r.getBoundingClientRect() : null;
        };

        NdRange.prototype.getClientRects = function () {
            var r = nativeProxy(this);
            return r ? r.getClientRects() : [];
        };

        var rangeConstants = {
            START_TO_START: 0, START_TO_END: 1,
            END_TO_END: 2, END_TO_START: 3
        };
        for (var rk in rangeConstants) {
            NdRange[rk] = rangeConstants[rk];
            NdRange.prototype[rk] = rangeConstants[rk];
        }

        (function wrapCharacterDataMutations() {
            var tn;
            try { tn = doc.createTextNode('x'); } catch (e) { return; }

            function ownerProtoWith(obj, prop) {
                var p = obj;
                while (p) {
                    if (Object.prototype.hasOwnProperty.call(p, prop)) return p;
                    p = Object.getPrototypeOf(p);
                }
                return null;
            }
            function clamp(v, lo, hi) {
                v = Math.trunc(Number(v));
                if (!isFinite(v)) v = 0;
                if (v < lo) v = lo;
                if (v > hi) v = hi;
                return v;
            }
            function clampU(v, hi) {
                v = Number(v);
                if (!isFinite(v)) v = 0;
                v = v >>> 0;
                return v > hi ? hi : v;
            }
            function curLen(node) {
                return node.data ? node.data.length : 0;
            }
            function strLen(v) {
                return (v === null || v === undefined ? '' : String(v)).length;
            }

            function wrapMethod(name, plan) {
                var proto = ownerProtoWith(tn, name);
                if (!proto) return;
                var orig = proto[name];
                if (typeof orig !== 'function') return;
                var fn = function () {
                    var len = curLen(this);
                    var ret = orig.apply(this, arguments);
                    var p = plan(len, arguments);
                    if (p) rangeReplaceData(this, p[0], p[1], p[2]);
                    return ret;
                };
                try { Object.defineProperty(fn, 'length', { value: orig.length, configurable: true }); } catch (e) {}
                try { Object.defineProperty(fn, 'name', { value: name, configurable: true }); } catch (e) {}
                Object.defineProperty(proto, name, {
                    value: fn, writable: true, configurable: true, enumerable: false
                });
            }

            wrapMethod('replaceData', function (len, args) {
                var off = clampU(args[0], len);
                return [off, clampU(args[1], len - off), strLen(args[2])];
            });
            wrapMethod('insertData', function (len, args) {
                return [clampU(args[0], len), 0, strLen(args[1])];
            });
            wrapMethod('deleteData', function (len, args) {
                var off = clampU(args[0], len);
                return [off, clampU(args[1], len - off), 0];
            });

            function wrapAccessor(name) {
                var proto = ownerProtoWith(tn, name);
                if (!proto) return;
                var d = Object.getOwnPropertyDescriptor(proto, name);
                if (!d || typeof d.set !== 'function') return;
                var origSet = d.set, origGet = d.get;
                Object.defineProperty(proto, name, {
                    get: origGet,
                    set: function (v) {
                        if (!isCharData(this)) { origSet.call(this, v); return; }
                        var len = curLen(this);
                        origSet.call(this, v);
                        rangeReplaceData(this, 0, len, strLen(v));
                    },
                    configurable: true, enumerable: d.enumerable === true
                });
            }

            wrapAccessor('data');
            wrapAccessor('nodeValue');
            wrapAccessor('textContent');

            var splitProto = ownerProtoWith(tn, 'splitText');
            if (splitProto && typeof splitProto.splitText === 'function') {
                var origSplit = splitProto.splitText;
                Object.defineProperty(splitProto, 'splitText', {
                    value: function (rawOffset) {
                        var len = curLen(this);
                        var offset = clamp(rawOffset, 0, len);
                        var count = len - offset;
                        var parent = this.parentNode;
                        var index = parent ? indexOfNode(this) : 0;
                        var node = this;
                        var newNode = origSplit.apply(this, arguments);
                        if (parent && newNode) {
                            forEachLiveRange(function (r) {
                                if (r._sc === node && r._so > offset) {
                                    r._sc = newNode; r._so -= offset;
                                } else if (r._sc === parent && r._so === index + 1) {
                                    r._so += 1;
                                }
                                if (r._ec === node && r._eo > offset) {
                                    r._ec = newNode; r._eo -= offset;
                                } else if (r._ec === parent && r._eo === index + 1) {
                                    r._eo += 1;
                                }
                            });
                        }
                        rangeReplaceData(node, offset, count, 0);
                        return newNode;
                    },
                    writable: true, configurable: true, enumerable: false
                });
            }
        })();

        (function wrapNodeMutations() {
            var el;
            try { el = doc.createElement('span'); } catch (e) { return; }

            function nodeProtosWith(name) {
                var out = [];
                function scan(start) {
                    var p = start;
                    while (p) {
                        if (Object.prototype.hasOwnProperty.call(p, name)) {
                            if (out.indexOf(p) < 0) out.push(p);
                            return;
                        }
                        p = Object.getPrototypeOf(p);
                    }
                }
                scan(el);
                scan(doc);
                if (typeof global.Document === 'function' && global.Document.prototype)
                    scan(global.Document.prototype);
                return out;
            }
            function preState(node) {
                var parent = node && node.parentNode;
                return parent ? { node: node, parent: parent, index: indexOfNode(node) } : null;
            }
            function applyRemove(st) {
                if (!st) return;
                forEachLiveRange(function (r) {
                    if (isInclusiveAncestor(st.node, r._sc)) { r._sc = st.parent; r._so = st.index; }
                    else if (r._sc === st.parent && r._so > st.index) r._so -= 1;
                    if (isInclusiveAncestor(st.node, r._ec)) { r._ec = st.parent; r._eo = st.index; }
                    else if (r._ec === st.parent && r._eo > st.index) r._eo -= 1;
                });
            }
            function applyInsert(node) {
                var parent = node && node.parentNode;
                if (!parent) return;
                if (node.nextSibling === null) return;
                var index = indexOfNode(node);
                forEachLiveRange(function (r) {
                    if (r._sc === parent && r._so > index) r._so += 1;
                    if (r._ec === parent && r._eo > index) r._eo += 1;
                });
            }
            function wrap(name, handler) {
                var protos = nodeProtosWith(name);
                for (var i = 0; i < protos.length; i++) {
                    (function (proto) {
                        var orig = proto[name];
                        if (typeof orig !== 'function') return;
                        var fn = function () { return handler.call(this, orig, arguments); };
                        try { Object.defineProperty(fn, 'length', { value: orig.length, configurable: true }); } catch (e) {}
                        try { Object.defineProperty(fn, 'name', { value: name, configurable: true }); } catch (e) {}
                        Object.defineProperty(proto, name, {
                            value: fn, writable: true, configurable: true, enumerable: false
                        });
                    })(protos[i]);
                }
            }

            wrap('appendChild', function (orig, args) {
                var st = preState(args[0]);
                var ret = orig.apply(this, args);
                applyRemove(st);
                applyInsert(args[0]);
                return ret;
            });
            wrap('insertBefore', function (orig, args) {
                var st = preState(args[0]);
                var ret = orig.apply(this, args);
                applyRemove(st);
                applyInsert(args[0]);
                return ret;
            });
            wrap('removeChild', function (orig, args) {
                var st = preState(args[0]);
                var ret = orig.apply(this, args);
                applyRemove(st);
                return ret;
            });
            wrap('replaceChild', function (orig, args) {
                var newChild = args[0], oldChild = args[1];
                var stOld = preState(oldChild);
                var stNew = (newChild !== oldChild) ? preState(newChild) : null;
                var ret = orig.apply(this, args);
                applyRemove(stOld);
                applyRemove(stNew);
                applyInsert(newChild);
                return ret;
            });
        })();

        nativeize(NdRange, 'Range');
        try { Object.defineProperty(NdRange, 'length', { value: 0 }); } catch (e) {}
        global.Range = NdRange;
        global.__ndCreateRange = function (ownerDoc) {
            return new NdRange(ownerDoc && ownerDoc.nodeType === 9 ? ownerDoc : doc);
        };

        var nativeGetSelection = global.getSelection;
        function selNative() {
            if (typeof nativeGetSelection !== 'function') return null;
            try { return nativeGetSelection.call(global); } catch (e) { return null; }
        }

        function NdSelection() {
            this._range = null;
            this._direction = 'none';
        }
        Object.defineProperties(NdSelection.prototype, {
            rangeCount: { get: function () {
                if (this._range) return 1;
                var n = selNative(); return n ? (n.rangeCount | 0) : 0;
            }, configurable: true },
            isCollapsed: { get: function () {
                if (this._range) return this._range.collapsed;
                var n = selNative(); return n ? !!n.isCollapsed : true;
            }, configurable: true },
            type: { get: function () {
                if (this._range) return this._range.collapsed ? 'Caret' : 'Range';
                var n = selNative(); return n ? String(n.type) : 'None';
            }, configurable: true },
            anchorNode: { get: function () {
                if (!this._range) { var n = selNative(); return n ? n.anchorNode : null; }
                return this._direction === 'backward'
                    ? this._range.endContainer : this._range.startContainer;
            }, configurable: true },
            anchorOffset: { get: function () {
                if (!this._range) { var n = selNative(); return n ? (n.anchorOffset | 0) : 0; }
                return this._direction === 'backward'
                    ? this._range.endOffset : this._range.startOffset;
            }, configurable: true },
            focusNode: { get: function () {
                if (!this._range) { var n = selNative(); return n ? n.focusNode : null; }
                return this._direction === 'backward'
                    ? this._range.startContainer : this._range.endContainer;
            }, configurable: true },
            focusOffset: { get: function () {
                if (!this._range) { var n = selNative(); return n ? (n.focusOffset | 0) : 0; }
                return this._direction === 'backward'
                    ? this._range.startOffset : this._range.endOffset;
            }, configurable: true }
        });
        NdSelection.prototype.getRangeAt = function (i) {
            if (this._range) { if ((i | 0) !== 0) throw domEx('IndexSizeError'); return this._range; }
            var n = selNative();
            if (n && (i | 0) < (n.rangeCount | 0)) return n.getRangeAt(i);
            throw domEx('IndexSizeError');
        };
        NdSelection.prototype.removeAllRanges = function () {
            this._range = null; this._direction = 'none';
        };
        NdSelection.prototype.empty = NdSelection.prototype.removeAllRanges;
        NdSelection.prototype.addRange = function (range) {
            if (this._range || !range) return;
            this._range = range instanceof NdRange ? range
                : mkRange(range.startContainer, range.startOffset,
                          range.endContainer, range.endOffset);
            this._direction = 'forward';
        };
        NdSelection.prototype.removeRange = function (range) {
            if (this._range === range) this.removeAllRanges();
        };
        NdSelection.prototype.collapse = function (node, offset) {
            if (node == null) { this.removeAllRanges(); return; }
            offset = checkBoundary(node, offset);
            this._range = mkRange(node, offset, node, offset);
            this._direction = 'forward';
        };
        NdSelection.prototype.setPosition = NdSelection.prototype.collapse;
        NdSelection.prototype.collapseToStart = function () {
            if (!this._range) throw domEx('InvalidStateError');
            var sc = this._range.startContainer, so = this._range.startOffset;
            this._range = mkRange(sc, so, sc, so); this._direction = 'forward';
        };
        NdSelection.prototype.collapseToEnd = function () {
            if (!this._range) throw domEx('InvalidStateError');
            var ec = this._range.endContainer, eo = this._range.endOffset;
            this._range = mkRange(ec, eo, ec, eo); this._direction = 'forward';
        };
        NdSelection.prototype.extend = function (node, offset) {
            if (!this._range) throw domEx('InvalidStateError');
            offset = checkBoundary(node, offset);
            var an = this.anchorNode, ao = this.anchorOffset;
            if (rootOf(node) !== rootOf(an)) throw domEx('WrongDocumentError');
            if (bpCompare(an, ao, node, offset) <= 0) {
                this._range = mkRange(an, ao, node, offset); this._direction = 'forward';
            } else {
                this._range = mkRange(node, offset, an, ao); this._direction = 'backward';
            }
        };
        NdSelection.prototype.setBaseAndExtent = function (an, ao, fn, fo) {
            ao = checkBoundary(an, ao);
            fo = checkBoundary(fn, fo);
            if (rootOf(an) !== rootOf(fn)) throw domEx('WrongDocumentError');
            if (bpCompare(an, ao, fn, fo) <= 0) {
                this._range = mkRange(an, ao, fn, fo); this._direction = 'forward';
            } else {
                this._range = mkRange(fn, fo, an, ao); this._direction = 'backward';
            }
        };
        NdSelection.prototype.selectAllChildren = function (node) {
            if (!node) return;
            this.setBaseAndExtent(node, 0, node, node.childNodes.length);
        };
        NdSelection.prototype.containsNode = function (node, allowPartial) {
            if (!this._range || !node) return false;
            return allowPartial ? this._range.intersectsNode(node)
                                : containedIn(node, this._range);
        };
        NdSelection.prototype.deleteFromDocument = function () {
            if (this._range) this._range.deleteContents();
        };
        NdSelection.prototype.modify = function () {};
        NdSelection.prototype.toString = function () {
            if (this._range) return this._range.toString();
            var n = selNative(); return n ? String(n) : '';
        };

        global.Selection = NdSelection;
        var theSelection = new NdSelection();
        function getSelectionImpl() { return theSelection; }
        global.getSelection = getSelectionImpl;
        if (doc) { try { doc.getSelection = getSelectionImpl; } catch (e) {} }
    })();

    /* HTMLImageElement.decode(): the native binding always resolved. Per the
     * HTML spec the promise rejects with an "EncodingError" when the image
     * has no usable source, fails to load, or its document is not fully
     * active, and resolves once a usable source has been decoded. The
     * active-document check is deferred one microtask so a synchronous adopt
     * into an inactive document after the call is observed. */
    (function () {
        if (typeof global.HTMLImageElement !== 'function' ||
            !global.HTMLImageElement.prototype) return;

        try {
            if (typeof global.Image === 'function' &&
                global.Image.prototype !== global.HTMLImageElement.prototype)
                global.Image.prototype = global.HTMLImageElement.prototype;
        } catch (e) {}

        function encodingError() {
            try { return new DOMException('The source image cannot be decoded.',
                                          'EncodingError'); }
            catch (e) {
                var err = new Error('The source image cannot be decoded.');
                err.name = 'EncodingError';
                return err;
            }
        }

        var decodeProto = global.HTMLImageElement.prototype;
        try {
            if (typeof document !== 'undefined' && document.createElement) {
                var p = Object.getPrototypeOf(document.createElement('img'));
                while (p && !Object.prototype.hasOwnProperty.call(p, 'decode'))
                    p = Object.getPrototypeOf(p);
                if (p) decodeProto = p;
            }
        } catch (e) {}

        Object.defineProperty(decodeProto, 'decode', {
            configurable: true, writable: true, enumerable: false,
            value: function () {
                var img = this;
                return new Promise(function (resolve, reject) {
                    function fail() { reject(encodingError()); }
                    Promise.resolve().then(function () {
                        var doc = img.ownerDocument;
                        if (!doc || doc.defaultView == null) { fail(); return; }
                        var src = (img.getAttribute && img.getAttribute('src')) || '';
                        var srcset = (img.getAttribute && img.getAttribute('srcset')) || '';
                        if (src === '' && srcset === '') { fail(); return; }
                        if (img.complete && img.naturalWidth > 0) { resolve(); return; }
                        var url = img.currentSrc || src;
                        if (!url) { fail(); return; }
                        var probe = new Image();
                        probe.onload = function () { resolve(); };
                        probe.onerror = fail;
                        probe.src = url;
                    });
                });
            }
        });
    })();
    /* Text tracks: addTextTrack() was a no-op and textTracks returned a fresh
     * empty array. Provide a working TextTrack / TextTrackList / TextTrackCue
     * model: addTextTrack, a media element's live textTracks list with an async
     * 'addtrack' TrackEvent, a <track> element's .track and readyState, mode
     * validation, cue add/remove, and WebVTT loading of a <track src> (fetch +
     * a minimal cue parser, firing load/error). Cue timing/rendering is not
     * implemented. */
    (function () {
        if (typeof document === 'undefined') return;

        function eventTarget(obj) {
            var listeners = {};
            obj.addEventListener = function (type, cb) {
                if (!cb) return;
                (listeners[type] || (listeners[type] = [])).push(cb);
            };
            obj.removeEventListener = function (type, cb) {
                var a = listeners[type];
                if (a) { var i = a.indexOf(cb); if (i >= 0) a.splice(i, 1); }
            };
            obj.dispatchEvent = function (ev) {
                try {
                    if (ev && ev.target == null)
                        Object.defineProperty(ev, 'target',
                            { value: obj, configurable: true });
                } catch (e) {}
                var prev;
                try { prev = global.event; global.event = ev; } catch (e) {}
                var on = obj['on' + ev.type];
                if (typeof on === 'function') { try { on.call(obj, ev); } catch (e) {} }
                var a = listeners[ev.type];
                if (a) a.slice().forEach(function (cb) {
                    try { cb.call(obj, ev); } catch (e) {}
                });
                try { global.event = prev; } catch (e) {}
                return true;
            };
            return obj;
        }

        function adoptInterface(obj, name, arrayBase) {
            var iface = global[name];
            if (typeof iface !== 'function' || !iface.prototype) return obj;
            try {
                if (arrayBase &&
                    Object.getPrototypeOf(iface.prototype) !== Array.prototype)
                    Object.setPrototypeOf(iface.prototype, Array.prototype);
                Object.setPrototypeOf(obj, iface.prototype);
            } catch (e) {}
            return obj;
        }

        function makeCueList() {
            var list = [];
            list.getCueById = function (id) {
                for (var i = 0; i < this.length; i++)
                    if (this[i].id === id) return this[i];
                return null;
            };
            return adoptInterface(list, 'TextTrackCueList', true);
        }

        var MODES = { disabled: 1, hidden: 1, showing: 1 };

        function TextTrack(el, kind, label, language, id, mode) {
            var self = eventTarget({});
            var _mode = MODES[mode] ? mode : 'disabled';
            var cues = makeCueList();
            var active = makeCueList();
            self.oncuechange = null;
            self.__el = el || null;
            self.__cues = cues;
            Object.defineProperties(self, {
                kind:     { enumerable: true, value: kind || '' },
                label:    { enumerable: true, value: label || '' },
                language: { enumerable: true, value: language || '' },
                id:       { enumerable: true, value: id || '' },
                inBandMetadataTrackDispatchType: { enumerable: true, value: '' },
                mode: {
                    enumerable: true,
                    get: function () { return _mode; },
                    set: function (v) {
                        var s = (typeof v === 'string') ? v : String(v);
                        if (MODES[s]) {
                            _mode = s;
                            if (s !== 'disabled') {
                                if (typeof queueMicrotask === 'function')
                                    queueMicrotask(function () { maybeLoad(self); });
                                else maybeLoad(self);
                            }
                        }
                    }
                },
                cues:       { enumerable: true, get: function () {
                    return _mode === 'disabled' ? null : cues; } },
                activeCues: { enumerable: true, get: function () {
                    return _mode === 'disabled' ? null : active; } },
                addCue: { value: function (cue) {
                    if (!cue) return;
                    try { cue.track = self; } catch (e) {}
                    if (cues.indexOf(cue) >= 0) return;
                    var st = +cue.startTime, et = +cue.endTime;
                    var i = 0;
                    while (i < cues.length) {
                        var cst = +cues[i].startTime, cet = +cues[i].endTime;
                        if (cst > st || (cst === st && cet < et)) break;
                        i++;
                    }
                    cues.splice(i, 0, cue);
                } },
                removeCue: { value: function (cue) {
                    var i = cues.indexOf(cue);
                    if (i < 0) throw new DOMException('Cue not found', 'NotFoundError');
                    cues.splice(i, 1);
                    try { cue.track = null; } catch (e) {}
                } }
            });
            try {
                Object.defineProperty(self, Symbol.toStringTag,
                    { value: 'TextTrack', configurable: true });
            } catch (e) {}
            return adoptInterface(self, 'TextTrack', false);
        }

        function parseTimestamp(s) {
            var m = /^(?:(\d+):)?(\d{2}):(\d{2})\.(\d{3})$/.exec(s.trim());
            if (!m) return NaN;
            return (m[1] ? +m[1] * 3600 : 0) + (+m[2]) * 60 + (+m[3]) + (+m[4]) / 1000;
        }

        function parseVtt(text, track) {
            var lines = String(text).replace(/\r\n|\r/g, '\n').split('\n');
            if (!/^﻿?WEBVTT/.test(lines[0] || '')) return false;
            var i = 1;
            while (i < lines.length) {
                while (i < lines.length && lines[i].trim() === '') i++;
                if (i >= lines.length) break;
                var id = '';
                if (lines[i].indexOf('-->') < 0) { id = lines[i]; i++; }
                if (i >= lines.length || lines[i].indexOf('-->') < 0) {
                    while (i < lines.length && lines[i].trim() !== '') i++;
                    continue;
                }
                var tm = lines[i].split('-->');
                var start = parseTimestamp(tm[0]);
                var end = parseTimestamp((tm[1] || '').trim().split(/\s+/)[0] || '');
                i++;
                var textLines = [];
                while (i < lines.length && lines[i].trim() !== '') {
                    textLines.push(lines[i]); i++;
                }
                if (!isNaN(start) && !isNaN(end) && typeof global.VTTCue === 'function') {
                    try {
                        var cue = new global.VTTCue(start, end, textLines.join('\n'));
                        cue.id = id;
                        track.addCue(cue);
                    } catch (e) {}
                }
            }
            return true;
        }

        var RS = { NONE: 0, LOADING: 1, LOADED: 2, ERROR: 3 };

        function maybeLoad(track) {
            var el = track.__el;
            if (!el || track.__loading) return;
            var parent = el.parentNode;
            if (!parent || !isMedia(parent)) return;
            var src = el.getAttribute ? (el.getAttribute('src') || '') : '';
            if (track.__loadedSrc === src && (src || track.__triedEmpty)) return;
            if (!src) {
                track.__triedEmpty = true;
                track.__loadedSrc = src;
                el.__trackRS = RS.ERROR;
                var fail = function () {
                    try { el.dispatchEvent(new Event('error')); } catch (e) {}
                };
                if (typeof queueMicrotask === 'function') queueMicrotask(fail);
                else setTimeout(fail, 0);
                return;
            }
            track.__loading = true;
            track.__loadedSrc = src;
            el.__trackRS = RS.LOADING;
            var resolved = src;
            try { resolved = new URL(src, document.baseURI).href; } catch (e) {}
            (typeof fetch === 'function'
                ? fetch(resolved).then(function (r) {
                    if (!r.ok) throw new Error('http ' + r.status);
                    return r.text();
                  })
                : Promise.reject(new Error('no fetch'))
            ).then(function (text) {
                track.__loading = false;
                var ok = parseVtt(text, track);
                el.__trackRS = ok ? RS.LOADED : RS.ERROR;
                try { el.dispatchEvent(new Event(ok ? 'load' : 'error')); } catch (e) {}
            }).catch(function () {
                track.__loading = false;
                el.__trackRS = RS.ERROR;
                try { el.dispatchEvent(new Event('error')); } catch (e) {}
            });
        }

        function trackListFor(el) {
            if (el.__ndTT) return el.__ndTT;
            var list = eventTarget([]);
            list.onaddtrack = null;
            list.onremovetrack = null;
            list.onchange = null;
            list.getTrackById = function (id) {
                for (var i = 0; i < this.length; i++)
                    if (this[i].id === id) return this[i];
                return null;
            };
            try {
                Object.defineProperty(list, Symbol.toStringTag,
                    { value: 'TextTrackList', configurable: true });
            } catch (e) {}
            adoptInterface(list, 'TextTrackList', true);
            try {
                Object.defineProperty(el, '__ndTT',
                    { value: list, configurable: true });
            } catch (e) { el.__ndTT = list; }
            return list;
        }

        function addTrack(list, track) {
            if (list.indexOf(track) >= 0) return;
            list.push(track);
            var fire = function () {
                var ev;
                try { ev = new TrackEvent('addtrack'); }
                catch (e) { ev = { type: 'addtrack' }; }
                try { ev.track = track; } catch (e) {}
                list.dispatchEvent(ev);
            };
            if (typeof queueMicrotask === 'function') queueMicrotask(fire);
            else setTimeout(fire, 0);
        }

        var proto = Object.getPrototypeOf(document.createElement('video'));
        var nativeReadyState = Object.getOwnPropertyDescriptor(proto, 'readyState');

        function isMedia(el) {
            var nm = el && el.tagName ? el.tagName.toLowerCase() : '';
            return nm === 'video' || nm === 'audio';
        }
        function isTrack(el) {
            return el && el.tagName && el.tagName.toLowerCase() === 'track';
        }

        Object.defineProperty(proto, 'textTracks', {
            configurable: true, enumerable: true,
            get: function () {
                var list = trackListFor(this);
                if (isMedia(this) && this.children) {
                    for (var i = 0; i < this.children.length; i++)
                        if (isTrack(this.children[i]))
                            considerTrack(this.children[i]);
                }
                return list;
            }
        });

        Object.defineProperty(proto, 'addTextTrack', {
            configurable: true, writable: true, enumerable: true,
            value: function (kind, label, language) {
                var k = (kind == null) ? '' : String(kind);
                var valid = { subtitles: 1, captions: 1, descriptions: 1,
                              chapters: 1, metadata: 1 };
                if (!valid[k])
                    throw new TypeError("Failed to execute 'addTextTrack': " +
                        "The provided value '" + k + "' is not a valid 'TextTrackKind'.");
                var track = TextTrack(null, k, label, language, '', 'hidden');
                addTrack(trackListFor(this), track);
                return track;
            }
        });

        Object.defineProperty(proto, 'track', {
            configurable: true, enumerable: true,
            get: function () {
                if (!isTrack(this)) return null;
                if (!this.__ndTrack) {
                    var kind = (this.getAttribute('kind') || 'subtitles').toLowerCase();
                    var valid = { subtitles: 1, captions: 1, descriptions: 1,
                                  chapters: 1, metadata: 1 };
                    if (!valid[kind]) kind = 'metadata';
                    var t = TextTrack(this, kind, this.getAttribute('label') || '',
                                      this.getAttribute('srclang') || '',
                                      this.id || '', 'disabled');
                    try { Object.defineProperty(this, '__ndTrack',
                        { value: t, configurable: true }); }
                    catch (e) { this.__ndTrack = t; }
                }
                var parent = this.parentNode;
                if (parent && isMedia(parent)) {
                    addTrack(trackListFor(parent), this.__ndTrack);
                    var self = this;
                    if (typeof queueMicrotask === 'function')
                        queueMicrotask(function () { maybeLoad(self.__ndTrack); });
                }
                return this.__ndTrack;
            }
        });

        Object.defineProperty(proto, 'readyState', {
            configurable: true, enumerable: true,
            get: function () {
                if (isTrack(this)) return this.__trackRS || 0;
                return nativeReadyState && nativeReadyState.get
                    ? nativeReadyState.get.call(this) : 0;
            }
        });

        if (typeof global.HTMLTrackElement === 'function') {
            ['NONE', 'LOADING', 'LOADED', 'ERROR'].forEach(function (k) {
                try {
                    Object.defineProperty(global.HTMLTrackElement, k,
                        { value: RS[k], enumerable: true });
                    Object.defineProperty(global.HTMLTrackElement.prototype, k,
                        { value: RS[k], enumerable: true, configurable: true });
                } catch (e) {}
            });
        }

        if (typeof global.VTTCue === 'function' && global.VTTCue.prototype) {
            try {
                var vp = global.VTTCue.prototype;
                if (!Object.getOwnPropertyDescriptor(vp, 'id')) {
                    Object.defineProperty(vp, 'id', {
                        configurable: true, enumerable: true,
                        get: function () {
                            return this.__nd_id === undefined ? '' : this.__nd_id;
                        },
                        set: function (v) { this.__nd_id = String(v); }
                    });
                }
            } catch (e) {}
        }

        /* The spec's track processing model runs on connection, not on JS
         * access, so a <track> appended to a media element (without anyone
         * touching .track) must still load. A document-wide observer would tax
         * every page, so it is started lazily only once a track/audio/video
         * element is created. */
        var observing = false;
        function considerTrack(el) {
            if (!isTrack(el)) return;
            var parent = el.parentNode;
            if (!parent || !isMedia(parent)) return;
            var track = el.track;
            var isDefault = el.default === true ||
                (el.hasAttribute && el.hasAttribute('default'));
            if (track.mode === 'disabled' && isDefault)
                track.mode = 'hidden';
            if (track.mode !== 'disabled') {
                if (typeof queueMicrotask === 'function')
                    queueMicrotask(function () { maybeLoad(track); });
                else maybeLoad(track);
            }
        }
        function scanForTracks(node) {
            if (!node || node.nodeType !== 1) return;
            considerTrack(node);
            if (node.querySelectorAll) {
                var ts = node.querySelectorAll('track');
                for (var i = 0; i < ts.length; i++) considerTrack(ts[i]);
            }
        }
        function removeTrackNode(parent, node) {
            if (!isTrack(node) || !node.__ndTrack || !parent || !parent.__ndTT)
                return;
            var list = parent.__ndTT;
            var idx = list.indexOf(node.__ndTrack);
            if (idx < 0) return;
            var track = node.__ndTrack;
            list.splice(idx, 1);
            var fire = function () {
                var ev;
                try { ev = new TrackEvent('removetrack'); }
                catch (e) { ev = { type: 'removetrack' }; }
                try { ev.track = track; } catch (e) {}
                list.dispatchEvent(ev);
            };
            if (typeof queueMicrotask === 'function') queueMicrotask(fire);
            else setTimeout(fire, 0);
        }
        function onMutations(muts) {
            for (var i = 0; i < muts.length; i++) {
                var m = muts[i];
                for (var j = 0; j < m.addedNodes.length; j++)
                    scanForTracks(m.addedNodes[j]);
                for (var k = 0; k < m.removedNodes.length; k++)
                    removeTrackNode(m.target, m.removedNodes[k]);
            }
        }
        function ensureObserver() {
            if (observing || typeof MutationObserver !== 'function' ||
                !document.documentElement) return;
            observing = true;
            new MutationObserver(onMutations).observe(document.documentElement,
                       { childList: true, subtree: true });
            scanForTracks(document.documentElement);
        }
        function observeMedia(el) {
            if (typeof MutationObserver !== 'function') return;
            try {
                new MutationObserver(onMutations).observe(el,
                    { childList: true, subtree: true });
            } catch (e) {}
        }
        if (typeof document.createElement === 'function') {
            var origCreate = document.createElement;
            var wrapCreate = function createElement(name) {
                var el = origCreate.apply(this, arguments);
                var n = ('' + name).toLowerCase();
                if (n === 'track' || n === 'video' || n === 'audio')
                    ensureObserver();
                if (n === 'video' || n === 'audio')
                    observeMedia(el);
                return el;
            };
            try {
                Object.defineProperty(wrapCreate, 'length',
                    { value: 1, configurable: true });
            } catch (e) {}
            nativeize(wrapCreate, 'createElement');
            document.createElement = wrapCreate;
        }
    })();

    (function processingInstructionAttributes() {
        var PI = global.ProcessingInstruction;
        if (typeof PI !== 'function' || !PI.prototype) return;
        var proto = PI.prototype;

        function domEx(name, msg) {
            try { return new global.DOMException(msg || name, name); }
            catch (e) {
                var er = new Error(msg || name);
                er.name = name;
                return er;
            }
        }
        function isWs(ch) {
            return ch === ' ' || ch === '\t' || ch === '\n' ||
                   ch === '\r' || ch === '\f';
        }
        function validName(name) {
            if (typeof name !== 'string' || !name.length) return false;
            for (var i = 0; i < name.length; i++) {
                var ch = name[i];
                if (isWs(ch) || ch === '=' || ch === '>' || ch === '/' ||
                    ch === '"' || ch === "'")
                    return false;
            }
            return true;
        }
        function unescapeValue(v) {
            if (v.indexOf('&') < 0) return v;
            return v.replace(/&quot;/g, '"').replace(/&nbsp;/g, '\u00a0')
                    .replace(/&lt;/g, '<').replace(/&gt;/g, '>')
                    .replace(/&amp;/g, '&');
        }
        function escapeValue(v) {
            return String(v).replace(/&/g, '&amp;').replace(/"/g, '&quot;')
                            .replace(/</g, '&lt;').replace(/>/g, '&gt;')
                            .replace(/\u00a0/g, '&nbsp;');
        }
        function parseAttrs(data) {
            var s = data === null || data === undefined ? '' : String(data);
            var attrs = [];
            var i = 0, n = s.length;
            while (i < n) {
                while (i < n && isWs(s[i])) i++;
                if (i >= n) break;
                var nameStart = i;
                while (i < n && s[i] !== '=' && !isWs(s[i])) i++;
                var name = s.slice(nameStart, i);
                if (!validName(name) || i >= n || s[i] !== '=') return null;
                i++;
                if (i >= n || s[i] !== '"') return null;
                i++;
                var vs = i;
                while (i < n && s[i] !== '"') i++;
                if (i >= n) return null;
                attrs.push([name, unescapeValue(s.slice(vs, i))]);
                i++;
                if (i < n && !isWs(s[i])) return null;
            }
            return attrs;
        }
        function serializeAttrs(attrs) {
            var parts = [];
            for (var i = 0; i < attrs.length; i++)
                parts.push(attrs[i][0] + '="' +
                           escapeValue(attrs[i][1]) + '"');
            return parts.join(' ');
        }
        function findAttr(attrs, name) {
            for (var i = 0; i < attrs.length; i++)
                if (attrs[i][0] === name) return i;
            return -1;
        }
        function def(name, fn) {
            Object.defineProperty(proto, name, {
                configurable: true, writable: true, value: fn
            });
        }
        if (!Object.getOwnPropertyDescriptor(proto, 'target')) {
            Object.defineProperty(proto, 'target', {
                configurable: true,
                get: function () { return this.nodeName; }
            });
        }
        def('hasAttributes', function () {
            var a = parseAttrs(this.data);
            return !!a && a.length > 0;
        });
        def('getAttributeNames', function () {
            var a = parseAttrs(this.data);
            if (!a) return [];
            var out = [];
            for (var i = 0; i < a.length; i++) out.push(a[i][0]);
            return out;
        });
        def('getAttribute', function (name) {
            var a = parseAttrs(this.data);
            if (!a) return null;
            var i = findAttr(a, String(name));
            return i < 0 ? null : a[i][1];
        });
        def('hasAttribute', function (name) {
            var a = parseAttrs(this.data);
            return !!a && findAttr(a, String(name)) >= 0;
        });
        def('setAttribute', function (name, value) {
            name = String(name);
            if (!validName(name))
                throw domEx('InvalidCharacterError',
                            'invalid attribute name');
            var a = parseAttrs(this.data) || [];
            var i = findAttr(a, name);
            if (i < 0) a.push([name, String(value)]);
            else a[i] = [name, String(value)];
            this.data = serializeAttrs(a);
        });
        def('removeAttribute', function (name) {
            var a = parseAttrs(this.data);
            if (!a) return;
            var i = findAttr(a, String(name));
            if (i < 0) return;
            a.splice(i, 1);
            this.data = serializeAttrs(a);
        });
        def('toggleAttribute', function (name, force) {
            name = String(name);
            if (!validName(name))
                throw domEx('InvalidCharacterError',
                            'invalid attribute name');
            var a = parseAttrs(this.data) || [];
            var i = findAttr(a, name);
            if (i >= 0) {
                if (force === true) return true;
                a.splice(i, 1);
                this.data = serializeAttrs(a);
                return false;
            }
            if (force === false) return false;
            a.push([name, '']);
            this.data = serializeAttrs(a);
            return true;
        });
    })();

    (function () {
        var nav = global.navigator;
        if (!nav || typeof global.Navigator !== 'function') return;
        var Np = global.Navigator.prototype;
        if (!Np || Object.getPrototypeOf(nav) !== Np) return;
        Object.getOwnPropertyNames(nav).forEach(function (k) {
            var d = Object.getOwnPropertyDescriptor(nav, k);
            if (!d || !d.configurable) return;
            try {
                delete nav[k];
                if (!Object.prototype.hasOwnProperty.call(Np, k))
                    Object.defineProperty(Np, k, d);
            } catch (e) {}
        });
    })();

})(typeof globalThis !== 'undefined' ? globalThis : this);
