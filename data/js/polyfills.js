(function (global) {
    'use strict';

    function defineCtor(name, ctor) {
        try {
            Object.defineProperty(global, name, {
                value: ctor, writable: true, configurable: true, enumerable: false
            });
        } catch (e) { global[name] = ctor; }
    }

    function defineMethod(proto, name, fn) {
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

    function blobPartBytes(part) {
        if (part == null) return new Uint8Array(0);
        if (part instanceof Uint8Array) return part;
        if (part instanceof ArrayBuffer) return new Uint8Array(part);
        if (ArrayBuffer.isView && ArrayBuffer.isView(part))
            return new Uint8Array(part.buffer, part.byteOffset, part.byteLength);
        if (part instanceof Blob) return part._b || new Uint8Array(0);
        return new TextEncoder().encode(String(part));
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
    Blob.prototype.text = function () {
        var b = this._b;
        return Promise.resolve(new TextDecoder().decode(b));
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
    XMLSerializer.prototype.serializeToString = function (node) {
        if (!node) return '';
        if (typeof node.outerHTML === 'string') return node.outerHTML;
        if (typeof node.innerHTML === 'string') return node.innerHTML;
        if (typeof node.nodeValue === 'string') return node.nodeValue;
        return String(node);
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
        self.locked = false;
        var controller = {
            enqueue: function (chunk) {
                if (!self._closed && !self._cancelled) self._buf.push(chunk);
            },
            close: function () { self._closed = true; },
            error: function (e) { self._error = e; self._closed = true; },
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
        return Promise.resolve({ value: undefined, done: true });
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
    ReadableStream.prototype.pipeTo = function () { return Promise.resolve(); };
    ReadableStream.prototype.pipeThrough = function (transform) {
        return transform && transform.readable ? transform.readable : new ReadableStream();
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
    defineCtor('ReadableStream', ReadableStream);

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
    defineCtor('WritableStream', WritableStream);

    function TransformStream(transformer, writableStrategy, readableStrategy) {
        if (!(this instanceof TransformStream))
            return new TransformStream(transformer, writableStrategy, readableStrategy);
        this.readable = new ReadableStream();
        this.writable = new WritableStream();
    }
    defineCtor('TransformStream', TransformStream);

    function TextEncoderStream() {
        if (!(this instanceof TextEncoderStream)) return new TextEncoderStream();
        TransformStream.call(this);
        Object.defineProperty(this, 'encoding', { value: 'utf-8', configurable: true });
    }
    TextEncoderStream.prototype = Object.create(TransformStream.prototype);
    TextEncoderStream.prototype.constructor = TextEncoderStream;
    defineCtor('TextEncoderStream', TextEncoderStream);

    function TextDecoderStream(label, options) {
        if (!(this instanceof TextDecoderStream)) return new TextDecoderStream(label, options);
        TransformStream.call(this);
        Object.defineProperty(this, 'encoding', {
            value: label ? String(label).toLowerCase() : 'utf-8',
            configurable: true
        });
    }
    TextDecoderStream.prototype = Object.create(TransformStream.prototype);
    TextDecoderStream.prototype.constructor = TextDecoderStream;
    defineCtor('TextDecoderStream', TextDecoderStream);

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
})(typeof globalThis !== 'undefined' ? globalThis : this);
