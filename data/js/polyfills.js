(function (global) {
    'use strict';

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

    function Headers(init) {
        if (!(this instanceof Headers)) return new Headers(init);
        this._m = Object.create(null);
        if (init == null) return;
        var self = this;
        if (init instanceof Headers) {
            init.forEach(function (v, k) { self.append(k, v); });
            return;
        }
        if (typeof init.length === 'number') {
            for (var i = 0; i < init.length; i++) {
                var p = init[i];
                if (p && p.length >= 2) self.append(p[0], p[1]);
            }
            return;
        }
        var keys = Object.keys(init);
        for (var j = 0; j < keys.length; j++) self.append(keys[j], init[keys[j]]);
    }
    Headers.prototype.append = function (k, v) {
        var key = normHeader(k);
        var val = String(v);
        if (this._m[key] != null) this._m[key] += ', ' + val;
        else this._m[key] = val;
    };
    Headers.prototype.set = function (k, v) { this._m[normHeader(k)] = String(v); };
    Headers.prototype.get = function (k) {
        var v = this._m[normHeader(k)];
        return v == null ? null : v;
    };
    Headers.prototype.has = function (k) { return this._m[normHeader(k)] != null; };
    Headers.prototype.delete = function (k) { delete this._m[normHeader(k)]; };
    Headers.prototype.forEach = function (fn, thisArg) {
        var keys = Object.keys(this._m);
        for (var i = 0; i < keys.length; i++)
            fn.call(thisArg, this._m[keys[i]], keys[i], this);
    };
    Headers.prototype.keys = function () {
        return Object.keys(this._m)[Symbol.iterator]();
    };
    Headers.prototype.values = function () {
        var self = this;
        return Object.keys(this._m).map(function (k) { return self._m[k]; })[Symbol.iterator]();
    };
    Headers.prototype.entries = function () {
        var self = this;
        return Object.keys(this._m).map(function (k) { return [k, self._m[k]]; })[Symbol.iterator]();
    };
    if (typeof Symbol !== 'undefined' && Symbol.iterator) {
        Headers.prototype[Symbol.iterator] = Headers.prototype.entries;
    }
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
        NF.SHOW_PROCESSING_INSTRUCTION = 0x40;
        NF.SHOW_COMMENT              = 0x80;
        NF.SHOW_DOCUMENT             = 0x100;
        NF.SHOW_DOCUMENT_TYPE        = 0x200;
        NF.SHOW_DOCUMENT_FRAGMENT    = 0x400;
        NF.FILTER_ACCEPT             = 1;
        NF.FILTER_REJECT             = 2;
        NF.FILTER_SKIP               = 3;
        defineCtor('NodeFilter', NF);
    }

    function makeTreeWalker(root, whatToShow, filterArg) {
        var what = (whatToShow == null) ? 0xFFFFFFFF : (whatToShow >>> 0);
        var filterFn = null;
        if (typeof filterArg === 'function') filterFn = filterArg;
        else if (filterArg && typeof filterArg.acceptNode === 'function')
            filterFn = function (n) { return filterArg.acceptNode(n); };

        function matchesWhat(n) {
            if (!n || typeof n.nodeType !== 'number') return false;
            var bit = 1 << (n.nodeType - 1);
            return (what & bit) !== 0;
        }
        function decide(n) {
            if (!matchesWhat(n)) return 3; /* SKIP */
            if (!filterFn) return 1;
            var r = filterFn(n);
            return r === 2 ? 2 : (r === 3 ? 3 : (r === 1 ? 1 : 3));
        }
        function nextDescendantOrSibling(n, stopAt) {
            if (!n) return null;
            if (n.firstChild) return n.firstChild;
            while (n && n !== stopAt) {
                if (n.nextSibling) return n.nextSibling;
                n = n.parentNode;
            }
            return null;
        }
        function previousDescendantOrSibling(n, stopAt) {
            if (!n || n === stopAt) return null;
            if (n.previousSibling) {
                var p = n.previousSibling;
                while (p.lastChild) p = p.lastChild;
                return p;
            }
            return n.parentNode === stopAt ? null : n.parentNode;
        }

        var walker = {
            root: root,
            whatToShow: what,
            filter: filterArg || null,
            currentNode: root,
            firstChild: function () {
                var n = this.currentNode && this.currentNode.firstChild;
                while (n) {
                    var d = decide(n);
                    if (d === 1) { this.currentNode = n; return n; }
                    if (d === 2) {
                        var sib = n.nextSibling;
                        while (!sib && n.parentNode && n.parentNode !== this.currentNode) {
                            n = n.parentNode; sib = n.nextSibling;
                        }
                        n = sib;
                    } else {
                        n = n.firstChild || (function step(x, stop) {
                            while (x && x !== stop) {
                                if (x.nextSibling) return x.nextSibling;
                                x = x.parentNode;
                            }
                            return null;
                        })(n, this.currentNode);
                    }
                }
                return null;
            },
            lastChild: function () {
                var n = this.currentNode && this.currentNode.lastChild;
                while (n) {
                    var d = decide(n);
                    if (d === 1) { this.currentNode = n; return n; }
                    n = n.previousSibling;
                }
                return null;
            },
            parentNode: function () {
                var n = this.currentNode && this.currentNode.parentNode;
                while (n && n !== this.root) {
                    if (decide(n) === 1) { this.currentNode = n; return n; }
                    n = n.parentNode;
                }
                return null;
            },
            nextSibling: function () {
                var n = this.currentNode && this.currentNode.nextSibling;
                while (n) {
                    if (decide(n) === 1) { this.currentNode = n; return n; }
                    n = n.nextSibling;
                }
                return null;
            },
            previousSibling: function () {
                var n = this.currentNode && this.currentNode.previousSibling;
                while (n) {
                    if (decide(n) === 1) { this.currentNode = n; return n; }
                    n = n.previousSibling;
                }
                return null;
            },
            nextNode: function () {
                var n = this.currentNode;
                while (true) {
                    n = nextDescendantOrSibling(n, this.root);
                    if (!n) return null;
                    if (decide(n) === 1) { this.currentNode = n; return n; }
                }
            },
            previousNode: function () {
                var n = this.currentNode;
                while (true) {
                    n = previousDescendantOrSibling(n, this.root);
                    if (!n) return null;
                    if (decide(n) === 1) { this.currentNode = n; return n; }
                }
            }
        };
        return walker;
    }

    if (global.document) {
        global.document.createTreeWalker = function (root, whatToShow, filter) {
            return makeTreeWalker(root, whatToShow, filter);
        };
        var origNI = global.document.createNodeIterator;
        global.document.createNodeIterator = function (root, whatToShow, filter) {
            var w = makeTreeWalker(root, whatToShow, filter);
            return {
                root: w.root, whatToShow: w.whatToShow, filter: w.filter,
                referenceNode: w.currentNode,
                pointerBeforeReferenceNode: true,
                nextNode: function () { return w.nextNode(); },
                previousNode: function () { return w.previousNode(); },
                detach: function () {}
            };
        };
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
                    return {
                        configurable: true, enumerable: false,
                        get: function () {
                            return this[Symbol.for('nd.on.' + propName)] || null;
                        },
                        set: function (v) {
                            this[Symbol.for('nd.on.' + propName)] = v;
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
                    Object.defineProperty(elementProto, name, {
                        configurable: true, get: getter
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
        this._underlying = underlying || {};
        this.locked = false;
    }
    WritableStream.prototype.getWriter = function () {
        var self = this;
        self.locked = true;
        return {
            write: function () { return Promise.resolve(); },
            close: function () { return Promise.resolve(); },
            abort: function () { return Promise.resolve(); },
            releaseLock: function () { self.locked = false; },
            ready: Promise.resolve(),
            closed: Promise.resolve(),
            desiredSize: 1
        };
    };
    WritableStream.prototype.abort = function () { return Promise.resolve(); };
    WritableStream.prototype.close = function () { return Promise.resolve(); };
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

    if (typeof global.Intl !== 'object' || global.Intl === null) {
        var Intl = {};
        var DTF_MONTHS = ['January', 'February', 'March', 'April', 'May', 'June',
            'July', 'August', 'September', 'October', 'November', 'December'];
        var DTF_DAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday',
            'Friday', 'Saturday'];
        function dtf2(n) { return (n < 10 ? '0' : '') + n; }
        function DateTimeFormat(locales, options) {
            if (!(this instanceof DateTimeFormat)) return new DateTimeFormat(locales, options);
            if (Array.isArray(locales)) locales = locales[0];
            this._locale = (typeof locales === 'string' ? locales : 'en-US');
            var o = Object.assign({}, options || {});
            if (o.dateStyle) {
                if (o.dateStyle === 'full') { o.weekday = 'long'; o.year = 'numeric'; o.month = 'long'; o.day = 'numeric'; }
                else if (o.dateStyle === 'long') { o.year = 'numeric'; o.month = 'long'; o.day = 'numeric'; }
                else if (o.dateStyle === 'medium') { o.year = 'numeric'; o.month = 'short'; o.day = 'numeric'; }
                else { o.year = '2-digit'; o.month = 'numeric'; o.day = 'numeric'; }
            }
            if (o.timeStyle) {
                o.hour = 'numeric'; o.minute = '2-digit';
                if (o.timeStyle === 'medium' || o.timeStyle === 'long' || o.timeStyle === 'full') o.second = '2-digit';
            }
            if (!o.weekday && !o.year && !o.month && !o.day &&
                !o.hour && !o.minute && !o.second) {
                o.year = 'numeric'; o.month = 'numeric'; o.day = 'numeric';
            }
            this._options = o;
        }
        DateTimeFormat.prototype._parts = function (d) {
            var date = (d instanceof Date) ? d : new Date(d == null ? Date.now() : d);
            if (isNaN(date.getTime())) return [{ type: 'literal', value: 'Invalid Date' }];
            var o = this._options, utc = o.timeZone === 'UTC';
            function g(name) { return utc ? date['getUTC' + name]() : date['get' + name](); }
            var Y = g('FullYear'), Mo = g('Month'), D = g('Date'), Wd = g('Day'),
                H = g('Hours'), Mi = g('Minutes'), S = g('Seconds');
            var dateParts = [], timeParts = [];
            if (o.weekday)
                dateParts.push({ type: 'weekday', value: o.weekday === 'narrow' ? DTF_DAYS[Wd].charAt(0)
                    : o.weekday === 'short' ? DTF_DAYS[Wd].slice(0, 3) : DTF_DAYS[Wd] });
            if (o.month)
                dateParts.push({ type: 'month', value:
                    o.month === 'long' ? DTF_MONTHS[Mo]
                    : o.month === 'short' ? DTF_MONTHS[Mo].slice(0, 3)
                    : o.month === 'narrow' ? DTF_MONTHS[Mo].charAt(0)
                    : o.month === '2-digit' ? dtf2(Mo + 1) : String(Mo + 1) });
            if (o.day)
                dateParts.push({ type: 'day', value: o.day === '2-digit' ? dtf2(D) : String(D) });
            if (o.year)
                dateParts.push({ type: 'year', value: o.year === '2-digit' ? dtf2(Y % 100) : String(Y) });
            var hour12 = o.hour12 !== undefined ? o.hour12
                : (o.hourCycle ? (o.hourCycle === 'h11' || o.hourCycle === 'h12') : true);
            if (o.hour || o.minute || o.second) {
                var hh = H, ap = null;
                if (hour12) { ap = H < 12 ? 'AM' : 'PM'; hh = H % 12; if (hh === 0) hh = 12; }
                if (o.hour) timeParts.push({ type: 'hour', value: o.hour === '2-digit' ? dtf2(hh) : String(hh) });
                if (o.minute) timeParts.push({ type: 'minute', value: o.minute === '2-digit' ? dtf2(Mi) : String(Mi) });
                if (o.second) timeParts.push({ type: 'second', value: o.second === '2-digit' ? dtf2(S) : String(S) });
                if (ap) timeParts.push({ type: 'dayPeriod', value: ap });
            }
            var loc = String(this._locale).toLowerCase();
            var numericDate = (o.month === 'numeric' || o.month === '2-digit') &&
                              (o.day === 'numeric' || o.day === '2-digit') && !o.weekday;
            var out = [];
            if (numericDate) {
                var sep = /^(de|nb|nn|no|da|fi)/.test(loc) ? '.' :
                          /^(fr|es|it|nl|pt|ru|pl)/.test(loc) ? '/' : '/';
                var order = /^(de|nb|nn|no|da|fi|fr|es|it|nl|pt|ru|pl|sv|en-gb)/.test(loc)
                    ? ['day', 'month', 'year'] : ['month', 'day', 'year'];
                var byType = {};
                for (var i = 0; i < dateParts.length; i++) byType[dateParts[i].type] = dateParts[i];
                for (var j = 0; j < order.length; j++) {
                    if (byType[order[j]]) {
                        if (out.length) out.push({ type: 'literal', value: sep });
                        out.push(byType[order[j]]);
                    }
                }
            } else {
                for (var k = 0; k < dateParts.length; k++) {
                    if (out.length) {
                        var prev = out[out.length - 1].type;
                        var lit = (prev === 'weekday') ? ', '
                                : (dateParts[k].type === 'year') ? ', ' : ' ';
                        out.push({ type: 'literal', value: lit });
                    }
                    out.push(dateParts[k]);
                }
            }
            if (timeParts.length) {
                if (out.length) out.push({ type: 'literal', value: ', ' });
                for (var t = 0; t < timeParts.length; t++) {
                    if (t > 0) {
                        var sepc = timeParts[t].type === 'dayPeriod' ? ' ' : ':';
                        out.push({ type: 'literal', value: sepc });
                    }
                    out.push(timeParts[t]);
                }
            }
            return out;
        };
        DateTimeFormat.prototype.format = function (d) {
            var parts = this._parts(d), s = '';
            for (var i = 0; i < parts.length; i++) s += parts[i].value;
            return s;
        };
        DateTimeFormat.prototype.formatToParts = function (d) {
            return this._parts(d);
        };
        DateTimeFormat.prototype.formatRange = function (a, b) {
            return this.format(a) + ' – ' + this.format(b);
        };
        DateTimeFormat.prototype.formatRangeToParts = function (a, b) {
            return [
                { type: 'literal', value: this.format(a) },
                { type: 'literal', value: ' – ' },
                { type: 'literal', value: this.format(b) }
            ];
        };
        DateTimeFormat.prototype.resolvedOptions = function () {
            return Object.assign({ locale: this._locale, calendar: 'gregory',
                numberingSystem: 'latn', timeZone: 'UTC' }, this._options);
        };
        DateTimeFormat.supportedLocalesOf = function (l) {
            return Array.isArray(l) ? l : (l ? [l] : []);
        };
        Intl.DateTimeFormat = DateTimeFormat;
        Date.prototype.toLocaleDateString = function (locales, options) {
            var o = options ? Object.assign({}, options) : { year: 'numeric', month: 'numeric', day: 'numeric' };
            delete o.hour; delete o.minute; delete o.second; delete o.timeStyle;
            return new DateTimeFormat(locales, o).format(this);
        };
        Date.prototype.toLocaleTimeString = function (locales, options) {
            var o = options ? Object.assign({}, options) : { hour: 'numeric', minute: '2-digit', second: '2-digit' };
            delete o.year; delete o.month; delete o.day; delete o.weekday; delete o.dateStyle;
            return new DateTimeFormat(locales, o).format(this);
        };
        Date.prototype.toLocaleString = function (locales, options) {
            var o = options ? Object.assign({}, options)
                : { year: 'numeric', month: 'numeric', day: 'numeric',
                    hour: 'numeric', minute: '2-digit', second: '2-digit' };
            return new DateTimeFormat(locales, o).format(this);
        };

        function nfSeparators(locale) {
            var l = String(locale || 'en').toLowerCase();
            var commaDecimal = /^(de|fr|es|it|nl|pt|ru|pl|tr|sv|nb|nn|no|da|fi|cs|el|hu|ro|uk|id|vi|ca|hr|sk|sl|bg|lt|lv|et|is|af|sr|gl|eu)/.test(l);
            if (/^(fr|ru|pl|uk|fi|sv|cs|hu|sk|nb|nn|no)/.test(l))
                return { group: ' ', decimal: commaDecimal ? ',' : '.' };
            return commaDecimal ? { group: '.', decimal: ',' } : { group: ',', decimal: '.' };
        }
        function nfGroup(intStr, sep) {
            var out = '', c = 0;
            for (var i = intStr.length - 1; i >= 0; i--) {
                out = intStr.charAt(i) + out;
                if (++c % 3 === 0 && i > 0) out = sep + out;
            }
            return out;
        }
        function nfCurrencySymbol(code) {
            var m = { USD: '$', CAD: 'CA$', AUD: 'A$', EUR: '€', GBP: '£',
                      JPY: '¥', CNY: 'CN¥', INR: '₹', KRW: '₩',
                      RUB: '₽', BRL: 'R$', NZD: 'NZ$', ZAR: 'R', MXN: 'MX$' };
            return m[code] || (code + ' ');
        }
        function NumberFormat(locales, options) {
            if (!(this instanceof NumberFormat)) return new NumberFormat(locales, options);
            this._options = options || {};
            if (Array.isArray(locales)) locales = locales[0];
            this._locale = (typeof locales === 'string' ? locales : 'en-US');
        }
        NumberFormat.prototype.format = function (n) {
            var num = Number(n);
            if (n == null || isNaN(num)) return 'NaN';
            if (!isFinite(num)) return (num < 0 ? '-' : '') + '∞';
            var opts = this._options;
            var sep = nfSeparators(this._locale);
            var neg = num < 0 || (num === 0 && 1 / num < 0);
            var abs = Math.abs(num);
            if (opts.style === 'percent') abs *= 100;
            var suffix = '';
            if (opts.notation === 'compact' && abs >= 1000) {
                var units = [[1e12, 'T'], [1e9, 'B'], [1e6, 'M'], [1e3, 'K']];
                for (var u = 0; u < units.length; u++) {
                    if (abs >= units[u][0]) { abs = abs / units[u][0]; suffix = units[u][1]; break; }
                }
            }
            var isCompact = opts.notation === 'compact';
            var minF = (typeof opts.minimumFractionDigits === 'number') ? opts.minimumFractionDigits
                     : (opts.style === 'currency' ? 2 : 0);
            var maxF = (typeof opts.maximumFractionDigits === 'number') ? opts.maximumFractionDigits
                     : (opts.style === 'currency' ? 2
                       : (opts.style === 'percent' ? 0
                         : (isCompact ? Math.max(minF, 1) : 3)));
            if (maxF < minF) maxF = minF;
            var fixed = abs.toFixed(maxF);
            if (maxF > minF && fixed.indexOf('.') >= 0) {
                fixed = fixed.replace(/0+$/, '');
                var dot = fixed.indexOf('.');
                var fracLen = dot < 0 ? 0 : fixed.length - dot - 1;
                if (fracLen < minF) {
                    if (dot < 0) fixed += '.';
                    for (var k = fracLen; k < minF; k++) fixed += '0';
                }
                if (fixed.charAt(fixed.length - 1) === '.') fixed = fixed.slice(0, -1);
            }
            var parts = fixed.split('.');
            var useGroup = opts.useGrouping !== false && !isCompact;
            var intOut = useGroup ? nfGroup(parts[0], sep.group) : parts[0];
            var out = intOut + (parts[1] !== undefined ? (sep.decimal + parts[1]) : '');
            if (isCompact) out += suffix;
            if (opts.style === 'percent') out += '%';
            if (opts.style === 'currency') out = nfCurrencySymbol(opts.currency || 'USD') + out;
            var sign = neg ? '-' : (opts.signDisplay === 'always' ? '+' : '');
            return sign + out;
        };
        NumberFormat.prototype.formatToParts = function (n) {
            var s = this.format(n), parts = [];
            if (s.charAt(0) === '-' || s.charAt(0) === '+') {
                parts.push({ type: 'minusSign', value: s.charAt(0) });
                s = s.slice(1);
            }
            parts.push({ type: 'integer', value: s });
            return parts;
        };
        NumberFormat.prototype.formatRange = function (a, b) {
            return this.format(a) + ' – ' + this.format(b);
        };
        NumberFormat.prototype.resolvedOptions = function () {
            return Object.assign({ locale: this._locale, numberingSystem: 'latn',
                                   useGrouping: this._options.useGrouping !== false }, this._options);
        };
        NumberFormat.supportedLocalesOf = function (l) {
            return Array.isArray(l) ? l : (l ? [l] : []);
        };
        Intl.NumberFormat = NumberFormat;
        if (typeof Number.prototype.toLocaleString !== 'function' ||
            (12345).toLocaleString('en-US') === '12345') {
            Number.prototype.toLocaleString = function (locales, options) {
                return new NumberFormat(locales, options).format(this);
            };
        }

        function Collator(locales, options) {
            if (!(this instanceof Collator)) return new Collator(locales, options);
            this._options = options || {};
            this._locale = (typeof locales === 'string' ? locales : 'en-US');
        }
        Collator.prototype.compare = function (a, b) {
            a = String(a); b = String(b);
            return a < b ? -1 : a > b ? 1 : 0;
        };
        Collator.prototype.resolvedOptions = function () {
            return Object.assign({ locale: this._locale }, this._options);
        };
        Collator.supportedLocalesOf = function (l) {
            return Array.isArray(l) ? l : (l ? [l] : []);
        };
        Intl.Collator = Collator;

        function PluralRules(locales, options) {
            if (!(this instanceof PluralRules)) return new PluralRules(locales, options);
            this._options = options || {};
            this._locale = (typeof locales === 'string' ? locales : 'en-US');
        }
        PluralRules.prototype.select = function (n) {
            return Number(n) === 1 ? 'one' : 'other';
        };
        PluralRules.prototype.selectRange = function () { return 'other'; };
        PluralRules.prototype.resolvedOptions = function () {
            return Object.assign({ locale: this._locale, pluralCategories: ['one', 'other'] }, this._options);
        };
        PluralRules.supportedLocalesOf = function (l) {
            return Array.isArray(l) ? l : (l ? [l] : []);
        };
        Intl.PluralRules = PluralRules;

        function ListFormat(locales, options) {
            if (!(this instanceof ListFormat)) return new ListFormat(locales, options);
            this._options = options || {};
        }
        ListFormat.prototype.format = function (list) {
            return Array.from(list || []).join(', ');
        };
        ListFormat.prototype.formatToParts = function (list) {
            var arr = Array.from(list || []);
            var out = [];
            for (var i = 0; i < arr.length; i++) {
                if (i > 0) out.push({ type: 'literal', value: ', ' });
                out.push({ type: 'element', value: String(arr[i]) });
            }
            return out;
        };
        ListFormat.supportedLocalesOf = function (l) {
            return Array.isArray(l) ? l : (l ? [l] : []);
        };
        Intl.ListFormat = ListFormat;

        function RelativeTimeFormat(locales, options) {
            if (!(this instanceof RelativeTimeFormat)) return new RelativeTimeFormat(locales, options);
            this._options = options || {};
        }
        RelativeTimeFormat.prototype.format = function (value, unit) {
            var v = Number(value);
            var u = String(unit || '');
            if (v === 0) return 'now';
            return (v > 0 ? 'in ' : '') + Math.abs(v) + ' ' + u +
                   (Math.abs(v) !== 1 ? 's' : '') + (v < 0 ? ' ago' : '');
        };
        RelativeTimeFormat.prototype.formatToParts = function (v, u) {
            return [{ type: 'literal', value: this.format(v, u) }];
        };
        RelativeTimeFormat.supportedLocalesOf = function (l) {
            return Array.isArray(l) ? l : (l ? [l] : []);
        };
        Intl.RelativeTimeFormat = RelativeTimeFormat;

        function Segmenter(locales, options) {
            if (!(this instanceof Segmenter)) return new Segmenter(locales, options);
            this._granularity = (options && options.granularity) || 'grapheme';
        }
        Segmenter.prototype.segment = function (str) {
            var s = String(str || '');
            var gran = this._granularity;
            var parts = [];
            if (gran === 'word') {
                var re = /\S+|\s+/g, m;
                while ((m = re.exec(s))) {
                    parts.push({ segment: m[0], index: m.index, isWordLike: /\S/.test(m[0]) });
                }
            } else if (gran === 'sentence') {
                var sentRe = /[^.!?]+[.!?]?\s*/g, sm;
                while ((sm = sentRe.exec(s))) {
                    if (sm[0]) parts.push({ segment: sm[0], index: sm.index });
                }
            } else {
                for (var i = 0; i < s.length; i++)
                    parts.push({ segment: s.charAt(i), index: i, isWordLike: /\w/.test(s.charAt(i)) });
            }
            parts[Symbol.iterator] = function () {
                var idx = 0;
                return { next: function () {
                    return idx < parts.length
                        ? { value: parts[idx++], done: false }
                        : { value: undefined, done: true };
                } };
            };
            return parts;
        };
        Intl.Segmenter = Segmenter;

        function Locale(tag, options) {
            if (!(this instanceof Locale)) return new Locale(tag, options);
            this.baseName = String(tag || 'en');
            if (options) Object.assign(this, options);
        }
        Locale.prototype.toString = function () { return this.baseName; };
        Locale.prototype.maximize = function () { return this; };
        Locale.prototype.minimize = function () { return this; };
        Intl.Locale = Locale;

        Intl.getCanonicalLocales = function (locales) {
            if (locales == null) return [];
            return Array.isArray(locales) ? locales.map(String) : [String(locales)];
        };
        Intl.supportedValuesOf = function () { return []; };

        defineCtor('Intl', Intl);
    }

    function CompressionStream() {
        if (!(this instanceof CompressionStream)) return new CompressionStream();
        TransformStream.call(this);
    }
    CompressionStream.prototype = Object.create(TransformStream.prototype);
    CompressionStream.prototype.constructor = CompressionStream;
    defineCtor('CompressionStream', CompressionStream);

    function DecompressionStream() {
        if (!(this instanceof DecompressionStream)) return new DecompressionStream();
        TransformStream.call(this);
    }
    DecompressionStream.prototype = Object.create(TransformStream.prototype);
    DecompressionStream.prototype.constructor = DecompressionStream;
    defineCtor('DecompressionStream', DecompressionStream);

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

    if (typeof global.setImmediate !== 'function') {
        var immediateCounter = 0;
        var immediateMap = {};
        defineCtor('setImmediate', function (fn) {
            var id = ++immediateCounter;
            var args = Array.prototype.slice.call(arguments, 1);
            immediateMap[id] = setTimeout(function () {
                delete immediateMap[id];
                fn.apply(null, args);
            }, 0);
            return id;
        });
        defineCtor('clearImmediate', function (id) {
            var tid = immediateMap[id];
            if (tid !== undefined) {
                clearTimeout(tid);
                delete immediateMap[id];
            }
        });
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

    if (navigator && typeof navigator.share !== 'function') {
        try {
            Object.defineProperty(navigator, 'share', {
                configurable: true, enumerable: true,
                value: function () {
                    var err = new Error('Web Share API not supported');
                    err.name = 'NotSupportedError';
                    return Promise.reject(err);
                }
            });
            Object.defineProperty(navigator, 'canShare', {
                configurable: true, enumerable: true,
                value: function () { return false; }
            });
        } catch (e) {}
    }

    if (navigator) {
        try {
            Object.defineProperty(navigator, 'canShare', {
                configurable: true, enumerable: true,
                value: function () { return false; }
            });
        } catch (e) {}
        try {
            Object.defineProperty(navigator, 'vibrate', {
                configurable: true, enumerable: true,
                value: function () { return false; }
            });
        } catch (e) {}
        try {
            Object.defineProperty(navigator, 'getAutoplayPolicy', {
                configurable: true, enumerable: true,
                value: function () { return 'allowed'; }
            });
        } catch (e) {}
        if (navigator.mediaDevices) {
            try {
                Object.defineProperty(navigator.mediaDevices, 'getSupportedConstraints', {
                    configurable: true, enumerable: true,
                    value: function () {
                        return {
                            width: true, height: true, aspectRatio: true,
                            frameRate: true, facingMode: true, resizeMode: true,
                            sampleRate: true, sampleSize: true, channelCount: true,
                            echoCancellation: true, noiseSuppression: true,
                            autoGainControl: true, deviceId: true, groupId: true
                        };
                    }
                });
            } catch (e) {}
        }
        if (navigator.permissions) {
            var makePermissionStatus = function (name, state) {
                return {
                    name: name || '',
                    state: state || 'prompt',
                    onchange: null,
                    addEventListener: function () {},
                    removeEventListener: function () {},
                    dispatchEvent: function () { return true; }
                };
            };
            try {
                Object.defineProperty(navigator.permissions, 'query', {
                    configurable: true, enumerable: true,
                    value: function (desc) {
                        var name = desc && desc.name ? String(desc.name) : '';
                        var state = name === 'notifications' ? 'denied' : 'prompt';
                        return Promise.resolve(makePermissionStatus(name, state));
                    }
                });
                Object.defineProperty(navigator.permissions, 'request', {
                    configurable: true, enumerable: true,
                    value: function (desc) {
                        var name = desc && desc.name ? String(desc.name) : '';
                        return Promise.resolve(makePermissionStatus(name, 'prompt'));
                    }
                });
            } catch (e) {}
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
                value: 'denied'
            });
            Object.defineProperty(global.Notification, 'requestPermission', {
                configurable: true, enumerable: true,
                value: function (callback) {
                    if (typeof callback === 'function') callback('denied');
                    return Promise.resolve('denied');
                }
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
        defineCtor('trustedTypes', {
            createPolicy: function (name, rules) {
                rules = rules || {};
                var policy = { name: String(name) };
                ['HTML', 'Script', 'ScriptURL'].forEach(function (kind) {
                    var method = 'create' + kind;
                    policy[method] = function (input) {
                        var fn = rules && rules[method];
                        if (typeof fn === 'function') return fn.call(rules, input);
                        return String(input == null ? '' : input);
                    };
                });
                return policy;
            },
            isHTML: function () { return false; },
            isScript: function () { return false; },
            isScriptURL: function () { return false; },
            emptyHTML: '',
            emptyScript: '',
            defaultPolicy: null
        });
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

    /* document.styleSheets + HTMLStyleElement.sheet with a working insertRule.
     * The native bindings exposed an empty styleSheets list and a null .sheet,
     * so CSS-in-JS libraries that create a <style>, read .sheet and call
     * insertRule() had no way to inject rules. Back each sheet by its owner
     * <style> node: insert/delete/replace rebuild the node's text content,
     * which the engine re-cascades. */
    (function () {
        if (typeof document === 'undefined') return;

        function splitRules(css) {
            var rules = [], depth = 0, start = 0, inStr = 0;
            for (var i = 0; i < css.length; i++) {
                var ch = css.charAt(i);
                if (inStr) { if (ch === inStr && css.charAt(i - 1) !== '\\') inStr = 0; continue; }
                if (ch === '"' || ch === "'") { inStr = ch; continue; }
                if (ch === '{') depth++;
                else if (ch === '}') {
                    depth--;
                    if (depth === 0) {
                        var r = css.slice(start, i + 1).replace(/^\s+|\s+$/g, '');
                        if (r) rules.push(r);
                        start = i + 1;
                    }
                }
            }
            var tail = css.slice(start).replace(/^\s+|\s+$/g, '');
            if (tail) rules.push(tail);
            return rules;
        }
        function makeRule(text) {
            var b = text.indexOf('{');
            return { cssText: text, selectorText: b >= 0 ? text.slice(0, b).replace(/^\s+|\s+$/g, '') : text,
                     type: 1, parentStyleSheet: null, parentRule: null };
        }
        function sheetFor(node) {
            if (node.__ndSheet) return node.__ndSheet;
            var rules = null;
            function ensure() {
                if (rules) return;
                var txt = ''; try { txt = node.textContent || ''; } catch (e) {}
                rules = splitRules(txt).map(makeRule);
            }
            function rebuild() {
                try { node.textContent = rules.map(function (r) { return r.cssText; }).join('\n'); } catch (e) {}
            }
            var sheet = {
                ownerNode: node, type: 'text/css', title: null, parentStyleSheet: null, href: null,
                get disabled() { return false; }, set disabled(v) {},
                get cssRules() { ensure(); var l = rules.slice(); l.item = function (i) { return l[i] || null; }; return l; },
                get rules() { return this.cssRules; },
                insertRule: function (rule, index) {
                    ensure();
                    if (typeof index !== 'number' || index < 0 || index > rules.length) index = rules.length;
                    rules.splice(index, 0, makeRule(String(rule)));
                    rebuild();
                    return index;
                },
                deleteRule: function (index) { ensure(); if (index >= 0 && index < rules.length) { rules.splice(index, 1); rebuild(); } },
                replaceSync: function (text) { rules = splitRules(String(text == null ? '' : text)).map(makeRule); rebuild(); },
                replace: function (text) { try { this.replaceSync(text); return Promise.resolve(this); } catch (e) { return Promise.reject(e); } }
            };
            try { Object.defineProperty(node, '__ndSheet', { value: sheet, writable: true, configurable: true, enumerable: false }); }
            catch (e) { node.__ndSheet = sheet; }
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
                            var rel = (this.getAttribute && this.getAttribute('rel') || '').toLowerCase();
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
            Object.defineProperty(document, 'styleSheets', {
                configurable: true,
                get: function () {
                    var nodes;
                    try { nodes = document.querySelectorAll('style, link[rel~="stylesheet"]'); }
                    catch (e) { try { nodes = document.getElementsByTagName('style'); } catch (e2) { nodes = []; } }
                    var list = [];
                    for (var i = 0; i < nodes.length; i++) {
                        var s = sheetFor(nodes[i]);
                        if (s) list.push(s);
                    }
                    list.item = function (i) { return list[i] || null; };
                    return list;
                }
            });
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
})(typeof globalThis !== 'undefined' ? globalThis : this);
