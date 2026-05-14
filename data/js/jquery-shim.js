(function (global) {
    'use strict';

    var doc = global.document;
    var slice = Array.prototype.slice;
    var toString = Object.prototype.toString;
    var hasOwn = Object.prototype.hasOwnProperty;
    var rspace = /\s+/;
    var rnotwhite = /\S+/g;
    var rhtml = /^\s*<(\w+)[\s\S]*>/;
    var rsingleTag = /^<([a-zA-Z][\w-]*)\s*\/?>(?:<\/\1>|)$/;
    var rquickIs = /^(?:#([\w-]+)|\.([\w-]+)|([a-zA-Z][\w-]*))$/;
    var dataStore = new WeakMap();
    var eventStore = new WeakMap();
    var readyQueue = [];
    var readyFired = false;

    function isArrayLike(o) {
        if (o == null || o === global) return false;
        if (Array.isArray(o)) return true;
        var l = o.length;
        return typeof l === 'number' && l >= 0 &&
               (l === 0 || (l - 1) in o);
    }

    function isFunction(o) { return typeof o === 'function'; }
    function isPlainObject(o) {
        if (!o || toString.call(o) !== '[object Object]') return false;
        var proto = Object.getPrototypeOf(o);
        return proto === null || proto === Object.prototype;
    }
    function isWindow(o) { return o != null && o === o.window; }
    function isNumeric(o) {
        var t = typeof o;
        return (t === 'number' || (t === 'string' && o.trim() !== '')) &&
               !isNaN(parseFloat(o)) && isFinite(o);
    }
    function isElement(o) { return o && o.nodeType === 1; }
    function isDocument(o) { return o && o.nodeType === 9; }

    function jQuery(sel, ctx) { return new jQuery.fn.init(sel, ctx); }
    var $ = jQuery;

    jQuery.fn = jQuery.prototype = {
        jquery: '3.x-nordstjernen-shim',
        constructor: jQuery,
        length: 0,
        toArray: function () { return slice.call(this); },
        get: function (i) {
            if (i == null) return this.toArray();
            return i < 0 ? this[this.length + i] : this[i];
        },
        eq: function (i) {
            var n = this.length;
            var j = +i + (i < 0 ? n : 0);
            return $(j >= 0 && j < n ? [this[j]] : []);
        },
        first: function () { return this.eq(0); },
        last: function () { return this.eq(-1); },
        slice: function () {
            return $(slice.apply(this, arguments));
        },
        each: function (fn) {
            for (var i = 0; i < this.length; i++) {
                if (fn.call(this[i], i, this[i]) === false) break;
            }
            return this;
        },
        map: function (fn) {
            var out = [];
            for (var i = 0; i < this.length; i++) {
                var v = fn.call(this[i], i, this[i]);
                if (v != null) {
                    if (isArrayLike(v)) {
                        for (var j = 0; j < v.length; j++) out.push(v[j]);
                    } else out.push(v);
                }
            }
            return $(out);
        },
        push: Array.prototype.push,
        sort: Array.prototype.sort,
        splice: Array.prototype.splice,
        end: function () { return this.prevObject || $(); },
        pushStack: function (arr) {
            var ret = $.merge($(), arr);
            ret.prevObject = this;
            return ret;
        }
    };

    jQuery.fn.init = function (sel, ctx) {
        if (!sel) return this;
        if (typeof sel === 'string') {
            sel = sel.trim();
            if (sel[0] === '<' && sel[sel.length - 1] === '>' && sel.length >= 3) {
                var nodes = $.parseHTML(sel, ctx && ctx.nodeType ? ctx : doc);
                if (isPlainObject(ctx)) {
                    var $set = $(nodes);
                    for (var k in ctx) {
                        if (isFunction($set[k])) $set[k](ctx[k]);
                        else $set.attr(k, ctx[k]);
                    }
                    return $set.length ? mergeIntoThis(this, $set.toArray()) : this;
                }
                return mergeIntoThis(this, nodes);
            }
            var root = (ctx && ctx.nodeType ? ctx :
                        (ctx instanceof jQuery ? ctx[0] : doc));
            if (!root) return this;
            var quick = rquickIs.exec(sel);
            if (quick) {
                if (quick[1]) {
                    var el = doc.getElementById(quick[1]);
                    return el ? mergeIntoThis(this, [el]) : this;
                }
                if (quick[2]) {
                    return mergeIntoThis(this, root.getElementsByClassName(quick[2]));
                }
                if (quick[3]) {
                    return mergeIntoThis(this, root.getElementsByTagName(quick[3]));
                }
            }
            try {
                return mergeIntoThis(this, root.querySelectorAll(sel));
            } catch (e) {
                return this;
            }
        }
        if (isFunction(sel)) {
            return $.ready(sel);
        }
        if (sel.nodeType || isWindow(sel)) {
            this[0] = sel; this.length = 1; return this;
        }
        if (sel instanceof jQuery) {
            return mergeIntoThis(this, sel.toArray());
        }
        if (isArrayLike(sel)) {
            return mergeIntoThis(this, sel);
        }
        this[0] = sel; this.length = 1;
        return this;
    };
    jQuery.fn.init.prototype = jQuery.fn;

    function mergeIntoThis(target, src) {
        var n = src.length || 0;
        for (var i = 0; i < n; i++) target[i] = src[i];
        target.length = n;
        return target;
    }

    $.extend = $.fn.extend = function () {
        var deep = false, i = 0, tgt = arguments[0] || {};
        if (typeof tgt === 'boolean') { deep = tgt; tgt = arguments[++i] || {}; }
        if (typeof tgt !== 'object' && !isFunction(tgt)) tgt = {};
        if (i === arguments.length - 1) { tgt = this; i--; }
        for (++i; i < arguments.length; i++) {
            var opt = arguments[i];
            if (opt == null) continue;
            for (var k in opt) {
                if (!hasOwn.call(opt, k)) continue;
                var src = tgt[k], v = opt[k];
                if (tgt === v) continue;
                if (deep && v && (isPlainObject(v) || Array.isArray(v))) {
                    var clone = Array.isArray(v) ? (Array.isArray(src) ? src : [])
                                                 : (isPlainObject(src) ? src : {});
                    tgt[k] = $.extend(deep, clone, v);
                } else if (v !== undefined) {
                    tgt[k] = v;
                }
            }
        }
        return tgt;
    };

    $.extend({
        noop: function () {},
        now: Date.now,
        guid: 1,
        expando: 'jQuery' + Math.random().toString(36).slice(2),
        isArray: Array.isArray,
        isFunction: isFunction,
        isPlainObject: isPlainObject,
        isWindow: isWindow,
        isNumeric: isNumeric,
        isEmptyObject: function (o) { for (var k in o) return false; return true; },
        type: function (o) {
            if (o == null) return o + '';
            return typeof o === 'object' || typeof o === 'function'
                ? (toString.call(o).slice(8, -1).toLowerCase())
                : typeof o;
        },
        trim: function (s) { return s == null ? '' : String(s).trim(); },
        each: function (obj, fn) {
            if (isArrayLike(obj)) {
                for (var i = 0; i < obj.length; i++)
                    if (fn.call(obj[i], i, obj[i]) === false) break;
            } else {
                for (var k in obj)
                    if (hasOwn.call(obj, k))
                        if (fn.call(obj[k], k, obj[k]) === false) break;
            }
            return obj;
        },
        map: function (obj, fn) {
            var out = [];
            if (isArrayLike(obj)) {
                for (var i = 0; i < obj.length; i++) {
                    var v = fn(obj[i], i);
                    if (v != null) out.push(v);
                }
            } else {
                for (var k in obj) if (hasOwn.call(obj, k)) {
                    var w = fn(obj[k], k);
                    if (w != null) out.push(w);
                }
            }
            return [].concat.apply([], out);
        },
        grep: function (arr, fn, inv) {
            var out = [], n = arr.length;
            inv = !!inv;
            for (var i = 0; i < n; i++) {
                if (!fn(arr[i], i) === inv) out.push(arr[i]);
            }
            return out;
        },
        merge: function (a, b) {
            var n = +b.length, i = 0, l = a.length;
            for (; i < n; i++) a[l++] = b[i];
            a.length = l;
            return a;
        },
        makeArray: function (obj, results) {
            var ret = results || [];
            if (obj == null) return ret;
            if (isArrayLike(obj)) $.merge(ret, typeof obj === 'string' ? [obj] : obj);
            else ret.push(obj);
            return ret;
        },
        inArray: function (v, arr, i) {
            return arr == null ? -1 : Array.prototype.indexOf.call(arr, v, i || 0);
        },
        contains: function (a, b) {
            return a !== b && a.contains ? a.contains(b) : false;
        },
        parseJSON: function (s) { return JSON.parse(s); },
        parseXML: function (s) {
            if (!s || typeof s !== 'string') return null;
            try { return new global.DOMParser().parseFromString(s, 'text/xml'); }
            catch (e) { return null; }
        },
        parseHTML: function (s, ctx, keepScripts) {
            if (typeof s !== 'string') return [];
            if (typeof ctx === 'boolean') { keepScripts = ctx; ctx = doc; }
            ctx = ctx || doc;
            var single = rsingleTag.exec(s);
            if (single) return [ctx.createElement(single[1])];
            var tpl = ctx.createElement('template');
            tpl.innerHTML = s;
            var src = tpl.content || tpl;
            var out = [];
            for (var i = 0; i < src.childNodes.length; i++) {
                var n = src.childNodes[i];
                if (!keepScripts && n.nodeName === 'SCRIPT') continue;
                out.push(n);
            }
            return out;
        },
        globalEval: function (code) {
            try { (0, eval)(code); } catch (e) {}
        },
        proxy: function (fn, ctx) {
            var args = slice.call(arguments, 2);
            return function () {
                return fn.apply(ctx, args.concat(slice.call(arguments)));
            };
        },
        noConflict: function () { return $; },
        camelCase: function (s) {
            return s.replace(/^-ms-/, 'ms-')
                    .replace(/-([a-z])/g, function (_, c) { return c.toUpperCase(); });
        },
        error: function (msg) { throw new Error(msg); },
        ready: function (fn) {
            if (readyFired) { try { fn($); } catch (e) {} return $; }
            readyQueue.push(fn);
            return $;
        }
    });

    function fireReady() {
        if (readyFired) return;
        readyFired = true;
        while (readyQueue.length) {
            try { readyQueue.shift().call(doc, $); } catch (e) {}
        }
    }
    if (doc) {
        if (doc.readyState === 'complete' || doc.readyState === 'interactive') {
            global.setTimeout(fireReady, 0);
        } else {
            doc.addEventListener('DOMContentLoaded', fireReady);
            global.addEventListener('load', fireReady);
        }
    }

    function dataFor(el) {
        var d = dataStore.get(el);
        if (!d) { d = {}; dataStore.set(el, d); }
        return d;
    }

    function parseDataAttr(v) {
        if (v === 'true') return true;
        if (v === 'false') return false;
        if (v === 'null') return null;
        if (v === '' + +v) return +v;
        if (/^(?:\{[\s\S]*\}|\[[\s\S]*\])$/.test(v)) {
            try { return JSON.parse(v); } catch (e) {}
        }
        return v;
    }

    $.data = function (el, key, val) {
        if (!el) return undefined;
        var store = dataFor(el);
        if (arguments.length === 3) { store[key] = val; return val; }
        if (key !== undefined) {
            if (key in store) return store[key];
            if (el.nodeType === 1) {
                var attr = el.getAttribute('data-' + key.replace(/[A-Z]/g, function (m) {
                    return '-' + m.toLowerCase();
                }));
                if (attr != null) return parseDataAttr(attr);
            }
            return undefined;
        }
        return store;
    };
    $.removeData = function (el, key) {
        if (!el) return;
        if (key === undefined) { dataStore['delete'](el); return; }
        var store = dataStore.get(el);
        if (store) delete store[key];
    };
    $.hasData = function (el) { return dataStore.has(el); };

    function evalString(v) { return parseDataAttr(v); }

    function eachEl(set, fn) {
        for (var i = 0; i < set.length; i++) {
            if (set[i] && set[i].nodeType === 1) fn(set[i], i);
        }
        return set;
    }

    $.fn.extend({
        find: function (sel) {
            var out = [];
            for (var i = 0; i < this.length; i++) {
                if (!this[i] || !this[i].querySelectorAll) continue;
                var r = this[i].querySelectorAll(sel);
                for (var j = 0; j < r.length; j++) out.push(r[j]);
            }
            return this.pushStack(out);
        },
        filter: function (sel) {
            return this.pushStack(filterSet(this, sel, false));
        },
        not: function (sel) {
            return this.pushStack(filterSet(this, sel, true));
        },
        is: function (sel) {
            var arr = filterSet(this, sel, false);
            return arr.length > 0;
        },
        has: function (target) {
            var $t = $(target), out = [];
            this.each(function (_, el) {
                for (var i = 0; i < $t.length; i++) {
                    if (el.contains($t[i])) { out.push(el); break; }
                }
            });
            return this.pushStack(out);
        },
        add: function (sel, ctx) {
            return this.pushStack($.merge($.merge([], this), $(sel, ctx)));
        },
        addBack: function (sel) {
            return this.add(sel == null ? this.prevObject : this.prevObject.filter(sel));
        },
        index: function (target) {
            if (target == null) {
                return this[0] && this[0].parentNode
                    ? this.first().prevAll().length : -1;
            }
            if (typeof target === 'string') {
                return Array.prototype.indexOf.call($(target), this[0]);
            }
            return Array.prototype.indexOf.call(this, target.jquery ? target[0] : target);
        },
        each: jQuery.fn.each,
        contents: function () {
            var out = [];
            this.each(function (_, el) {
                if (el && el.childNodes)
                    for (var i = 0; i < el.childNodes.length; i++)
                        out.push(el.childNodes[i]);
            });
            return this.pushStack(out);
        },
        parent: function (sel) {
            return this.pushStack(uniq(mapEl(this, function (el) { return el.parentElement; }, sel)));
        },
        parents: function (sel) {
            return this.pushStack(uniq(traverse(this, 'parentElement', sel)));
        },
        parentsUntil: function (until, sel) {
            return this.pushStack(uniq(traverseUntil(this, 'parentElement', until, sel)));
        },
        closest: function (sel) {
            var out = [];
            this.each(function (_, el) {
                var c = el.closest ? el.closest(sel) : null;
                if (c) out.push(c);
            });
            return this.pushStack(uniq(out));
        },
        children: function (sel) {
            var out = [];
            this.each(function (_, el) {
                if (el && el.children)
                    for (var i = 0; i < el.children.length; i++) out.push(el.children[i]);
            });
            return this.pushStack(filterArr(out, sel));
        },
        siblings: function (sel) {
            var out = [];
            this.each(function (_, el) {
                if (!el || !el.parentNode) return;
                var sibs = el.parentNode.children;
                for (var i = 0; i < sibs.length; i++)
                    if (sibs[i] !== el) out.push(sibs[i]);
            });
            return this.pushStack(filterArr(uniq(out), sel));
        },
        next: function (sel) {
            return this.pushStack(filterArr(mapEl(this, function (e) { return e.nextElementSibling; }), sel));
        },
        prev: function (sel) {
            return this.pushStack(filterArr(mapEl(this, function (e) { return e.previousElementSibling; }), sel));
        },
        nextAll: function (sel) {
            return this.pushStack(traverse(this, 'nextElementSibling', sel));
        },
        prevAll: function (sel) {
            return this.pushStack(traverse(this, 'previousElementSibling', sel));
        },
        nextUntil: function (until, sel) {
            return this.pushStack(traverseUntil(this, 'nextElementSibling', until, sel));
        },
        prevUntil: function (until, sel) {
            return this.pushStack(traverseUntil(this, 'previousElementSibling', until, sel));
        }
    });

    function filterSet(set, sel, invert) {
        var arr = [];
        if (isFunction(sel)) {
            for (var i = 0; i < set.length; i++) {
                var keep = !!sel.call(set[i], i, set[i]);
                if (keep !== invert) arr.push(set[i]);
            }
            return arr;
        }
        if (sel && sel.nodeType) {
            for (var j = 0; j < set.length; j++) {
                if ((set[j] === sel) !== invert) arr.push(set[j]);
            }
            return arr;
        }
        if (typeof sel === 'string') {
            for (var k = 0; k < set.length; k++) {
                var el = set[k];
                var m = el && el.matches ? el.matches(sel) : false;
                if (m !== invert) arr.push(el);
            }
            return arr;
        }
        if (isArrayLike(sel)) {
            var list = slice.call(sel);
            for (var n = 0; n < set.length; n++) {
                var has = list.indexOf(set[n]) !== -1;
                if (has !== invert) arr.push(set[n]);
            }
        }
        return arr;
    }

    function filterArr(arr, sel) {
        if (!sel) return arr;
        var out = [];
        for (var i = 0; i < arr.length; i++) {
            var el = arr[i];
            if (el && el.matches && el.matches(sel)) out.push(el);
        }
        return out;
    }

    function uniq(arr) {
        var out = [], seen = new Set();
        for (var i = 0; i < arr.length; i++) {
            if (arr[i] && !seen.has(arr[i])) { seen.add(arr[i]); out.push(arr[i]); }
        }
        return out;
    }

    function mapEl(set, step, sel) {
        var out = [];
        for (var i = 0; i < set.length; i++) {
            var v = step(set[i]);
            if (v) out.push(v);
        }
        return sel ? filterArr(out, sel) : out;
    }

    function traverse(set, prop, sel) {
        var out = [];
        for (var i = 0; i < set.length; i++) {
            for (var n = set[i] && set[i][prop]; n; n = n[prop]) {
                if (n.nodeType === 1) out.push(n);
            }
        }
        return sel ? filterArr(out, sel) : out;
    }

    function traverseUntil(set, prop, until, sel) {
        var matches = until ? function (n) {
            return typeof until === 'string'
                ? (n.matches && n.matches(until))
                : (n === until || (until.jquery && Array.prototype.indexOf.call(until, n) !== -1));
        } : function () { return false; };
        var out = [];
        for (var i = 0; i < set.length; i++) {
            for (var n = set[i] && set[i][prop]; n; n = n[prop]) {
                if (n.nodeType !== 1) continue;
                if (matches(n)) break;
                out.push(n);
            }
        }
        return sel ? filterArr(out, sel) : out;
    }

    $.fn.extend({
        attr: function (name, val) {
            if (typeof name === 'object') {
                for (var k in name) this.attr(k, name[k]);
                return this;
            }
            if (val === undefined) {
                return this[0] && this[0].getAttribute ? this[0].getAttribute(name) : undefined;
            }
            return eachEl(this, function (el) {
                if (val === null) el.removeAttribute(name);
                else el.setAttribute(name, '' + (isFunction(val) ? val.call(el, 0, el.getAttribute(name)) : val));
            });
        },
        removeAttr: function (name) {
            var parts = String(name).match(rnotwhite) || [];
            return eachEl(this, function (el) {
                for (var i = 0; i < parts.length; i++) el.removeAttribute(parts[i]);
            });
        },
        prop: function (name, val) {
            if (typeof name === 'object') {
                for (var k in name) this.prop(k, name[k]);
                return this;
            }
            if (val === undefined) return this[0] ? this[0][name] : undefined;
            return eachEl(this, function (el) {
                el[name] = isFunction(val) ? val.call(el, 0, el[name]) : val;
            });
        },
        removeProp: function (name) {
            return eachEl(this, function (el) { try { delete el[name]; } catch (e) {} });
        },
        hasClass: function (cls) {
            var classes = String(cls).match(rnotwhite) || [];
            for (var i = 0; i < this.length; i++) {
                var el = this[i];
                if (!el || !el.classList) continue;
                var ok = true;
                for (var j = 0; j < classes.length; j++) {
                    if (!el.classList.contains(classes[j])) { ok = false; break; }
                }
                if (ok && classes.length) return true;
            }
            return false;
        },
        addClass: function (cls) {
            return eachEl(this, function (el, i) {
                var v = isFunction(cls) ? cls.call(el, i, el.className) : cls;
                var parts = String(v || '').match(rnotwhite) || [];
                for (var j = 0; j < parts.length; j++) el.classList.add(parts[j]);
            });
        },
        removeClass: function (cls) {
            return eachEl(this, function (el, i) {
                if (cls === undefined) { el.className = ''; return; }
                var v = isFunction(cls) ? cls.call(el, i, el.className) : cls;
                var parts = String(v || '').match(rnotwhite) || [];
                for (var j = 0; j < parts.length; j++) el.classList.remove(parts[j]);
            });
        },
        toggleClass: function (cls, force) {
            return eachEl(this, function (el, i) {
                var v = isFunction(cls) ? cls.call(el, i, el.className, force) : cls;
                var parts = String(v || '').match(rnotwhite) || [];
                for (var j = 0; j < parts.length; j++) {
                    if (force === undefined) el.classList.toggle(parts[j]);
                    else if (force) el.classList.add(parts[j]);
                    else el.classList.remove(parts[j]);
                }
            });
        },
        val: function (v) {
            if (!arguments.length) {
                var el = this[0];
                if (!el) return undefined;
                if (el.nodeName === 'SELECT' && el.multiple) {
                    var arr = [];
                    if (el.options) for (var i = 0; i < el.options.length; i++)
                        if (el.options[i].selected) arr.push(el.options[i].value);
                    return arr;
                }
                return el.value != null ? el.value : '';
            }
            return eachEl(this, function (el, i) {
                var nv = isFunction(v) ? v.call(el, i, el.value) : v;
                if (nv == null) nv = '';
                el.value = Array.isArray(nv) ? nv.join(',') : nv;
            });
        },
        text: function (t) {
            if (t === undefined) {
                var out = '';
                for (var i = 0; i < this.length; i++) {
                    if (this[i]) out += this[i].textContent || '';
                }
                return out;
            }
            return eachEl(this, function (el, i) {
                el.textContent = isFunction(t) ? t.call(el, i, el.textContent) : t;
            });
        },
        html: function (h) {
            if (h === undefined) return this[0] ? this[0].innerHTML : undefined;
            return eachEl(this, function (el, i) {
                el.innerHTML = isFunction(h) ? h.call(el, i, el.innerHTML) : h;
            });
        },
        data: function (k, v) {
            if (k === undefined) {
                if (!this[0] || this[0].nodeType !== 1) return undefined;
                var store = dataFor(this[0]);
                var attrs = this[0].attributes;
                if (attrs) for (var i = 0; i < attrs.length; i++) {
                    var a = attrs[i];
                    if (a.name.indexOf('data-') === 0) {
                        var key = a.name.slice(5).replace(/-([a-z])/g, function (_, c) { return c.toUpperCase(); });
                        if (!(key in store)) store[key] = parseDataAttr(a.value);
                    }
                }
                return store;
            }
            if (typeof k === 'object') {
                return eachEl(this, function (el) {
                    var s = dataFor(el);
                    for (var key in k) s[key] = k[key];
                });
            }
            if (v === undefined) return this[0] ? $.data(this[0], k) : undefined;
            return eachEl(this, function (el) { $.data(el, k, v); });
        },
        removeData: function (k) {
            return eachEl(this, function (el) { $.removeData(el, k); });
        }
    });

    function setCss(el, name, val) {
        if (!el || !el.style) return;
        name = $.camelCase(name);
        if (typeof val === 'number' && !cssNumber[name]) val = val + 'px';
        el.style[name] = val == null ? '' : val;
    }
    function getCss(el, name) {
        if (!el) return undefined;
        var cs = global.getComputedStyle ? global.getComputedStyle(el) : null;
        var camel = $.camelCase(name);
        if (cs && cs.getPropertyValue) {
            var v = cs.getPropertyValue(name) || cs[camel];
            if (v != null && v !== '') return v;
        }
        return el.style ? el.style[camel] : undefined;
    }
    var cssNumber = {
        animationIterationCount: true, columnCount: true, fillOpacity: true,
        flexGrow: true, flexShrink: true, fontWeight: true, gridArea: true,
        gridColumn: true, gridColumnEnd: true, gridColumnStart: true,
        gridRow: true, gridRowEnd: true, gridRowStart: true, lineHeight: true,
        opacity: true, order: true, orphans: true, widows: true, zIndex: true,
        zoom: true
    };

    $.fn.extend({
        css: function (name, val) {
            if (typeof name === 'object') {
                for (var k in name) this.css(k, name[k]);
                return this;
            }
            if (val === undefined) {
                if (Array.isArray(name)) {
                    var out = {};
                    for (var i = 0; i < name.length; i++) out[name[i]] = getCss(this[0], name[i]);
                    return out;
                }
                return getCss(this[0], name);
            }
            return eachEl(this, function (el, i) {
                setCss(el, name, isFunction(val) ? val.call(el, i, getCss(el, name)) : val);
            });
        },
        width: function (v) { return dim(this, 'Width', v, 'content'); },
        height: function (v) { return dim(this, 'Height', v, 'content'); },
        innerWidth: function () { return dim(this, 'Width', undefined, 'padding'); },
        innerHeight: function () { return dim(this, 'Height', undefined, 'padding'); },
        outerWidth: function (m) { return dim(this, 'Width', undefined, m ? 'margin' : 'border'); },
        outerHeight: function (m) { return dim(this, 'Height', undefined, m ? 'margin' : 'border'); },
        offset: function () {
            var el = this[0];
            if (!el || !el.getBoundingClientRect) return { top: 0, left: 0 };
            var r = el.getBoundingClientRect();
            return {
                top: r.top + (global.pageYOffset || 0),
                left: r.left + (global.pageXOffset || 0)
            };
        },
        position: function () {
            var el = this[0];
            if (!el || !el.getBoundingClientRect) return { top: 0, left: 0 };
            var r = el.getBoundingClientRect();
            return { top: r.top, left: r.left };
        },
        scrollTop: function (v) {
            if (v === undefined) {
                var el = this[0];
                if (!el) return undefined;
                return isWindow(el) ? (el.pageYOffset || 0) : (el.scrollTop || 0);
            }
            return eachEl(this, function (el) { if (!isWindow(el)) el.scrollTop = v; });
        },
        scrollLeft: function (v) {
            if (v === undefined) {
                var el = this[0];
                if (!el) return undefined;
                return isWindow(el) ? (el.pageXOffset || 0) : (el.scrollLeft || 0);
            }
            return eachEl(this, function (el) { if (!isWindow(el)) el.scrollLeft = v; });
        },
        offsetParent: function () {
            return this.map(function () {
                var op = this.offsetParent;
                while (op && op.style && op.style.position === 'static') op = op.offsetParent;
                return op || doc.documentElement;
            });
        }
    });

    function dim(set, name, val, box) {
        var el = set[0];
        if (val === undefined) {
            if (!el) return undefined;
            if (isWindow(el)) return el['inner' + name] || 0;
            if (isDocument(el)) {
                var de = el.documentElement;
                return Math.max(
                    el.body ? el.body['scroll' + name] : 0,
                    de['scroll' + name] || 0,
                    de['offset' + name] || 0,
                    de['client' + name] || 0
                );
            }
            var key = (box === 'content' ? 'client' :
                       box === 'padding' ? 'client' :
                       'offset') + name;
            return el[key] || 0;
        }
        return eachEl(set, function (e, i) {
            var nv = isFunction(val) ? val.call(e, i, e['client' + name]) : val;
            if (typeof nv === 'number') nv = nv + 'px';
            e.style[name === 'Width' ? 'width' : 'height'] = nv;
        });
    }

    function buildFragment(nodes, ownerDoc) {
        var frag = ownerDoc.createDocumentFragment();
        for (var i = 0; i < nodes.length; i++) {
            var item = nodes[i];
            if (item == null) continue;
            if (typeof item === 'string') {
                var parsed = $.parseHTML(item, ownerDoc, true);
                for (var j = 0; j < parsed.length; j++) frag.appendChild(parsed[j]);
            } else if (item.nodeType) {
                frag.appendChild(item);
            } else if (item.jquery || isArrayLike(item)) {
                for (var k = 0; k < item.length; k++) frag.appendChild(item[k]);
            }
        }
        return frag;
    }

    function cloneContents(contents) {
        var out = [];
        for (var i = 0; i < contents.length; i++) {
            var c = contents[i];
            if (c == null) continue;
            if (c && c.nodeType && c.cloneNode) out.push(c.cloneNode(true));
            else if (c && c.jquery) {
                for (var j = 0; j < c.length; j++)
                    if (c[j] && c[j].cloneNode) out.push(c[j].cloneNode(true));
            }
            else out.push(c);
        }
        return out;
    }

    function domInsert(set, contents, place) {
        var owner = doc;
        for (var i = 0; i < set.length; i++) {
            var target = set[i];
            if (!target || !target.nodeType) continue;
            var feed = (i === set.length - 1) ? contents : cloneContents(contents);
            var frag = buildFragment(feed, owner);
            place(target, frag);
        }
        return set;
    }

    $.fn.extend({
        append: function () {
            return domInsert(this, slice.call(arguments), function (t, f) { t.appendChild(f); });
        },
        prepend: function () {
            return domInsert(this, slice.call(arguments), function (t, f) {
                t.insertBefore(f, t.firstChild);
            });
        },
        before: function () {
            return domInsert(this, slice.call(arguments), function (t, f) {
                if (t.parentNode) t.parentNode.insertBefore(f, t);
            });
        },
        after: function () {
            return domInsert(this, slice.call(arguments), function (t, f) {
                if (t.parentNode) t.parentNode.insertBefore(f, t.nextSibling);
            });
        },
        appendTo: function (sel) { $(sel).append(this); return this; },
        prependTo: function (sel) { $(sel).prepend(this); return this; },
        insertBefore: function (sel) { $(sel).before(this); return this; },
        insertAfter: function (sel) { $(sel).after(this); return this; },
        remove: function (sel) {
            var set = sel ? filterArr(this.toArray(), sel) : this;
            for (var i = 0; i < set.length; i++) {
                var el = set[i];
                if (el && el.parentNode) el.parentNode.removeChild(el);
            }
            return this;
        },
        detach: function (sel) { return this.remove(sel); },
        empty: function () {
            return eachEl(this, function (el) {
                while (el.firstChild) el.removeChild(el.firstChild);
            });
        },
        replaceWith: function (val) {
            return this.each(function (i, el) {
                if (!el || !el.parentNode) return;
                var v = isFunction(val) ? val.call(el, i, el.outerHTML) : val;
                var frag = buildFragment([v], el.ownerDocument || doc);
                el.parentNode.replaceChild(frag, el);
            });
        },
        replaceAll: function (sel) { $(sel).replaceWith(this); return this; },
        clone: function (withData) {
            var out = [];
            this.each(function (_, el) {
                if (el && el.cloneNode) out.push(el.cloneNode(true));
            });
            void withData;
            return this.pushStack(out);
        },
        wrap: function (html) {
            return this.each(function (i, el) {
                if (!el || !el.parentNode) return;
                var w = (isFunction(html) ? html.call(el, i) : html);
                var wrapper = $(w).clone()[0];
                if (!wrapper) return;
                el.parentNode.insertBefore(wrapper, el);
                var deepest = wrapper;
                while (deepest.firstElementChild) deepest = deepest.firstElementChild;
                deepest.appendChild(el);
            });
        },
        wrapAll: function (html) {
            if (!this.length) return this;
            var w = $(isFunction(html) ? html.call(this[0]) : html).clone()[0];
            if (!w) return this;
            var first = this[0];
            if (first.parentNode) first.parentNode.insertBefore(w, first);
            var deepest = w;
            while (deepest.firstElementChild) deepest = deepest.firstElementChild;
            this.each(function (_, el) { deepest.appendChild(el); });
            return this;
        },
        wrapInner: function (html) {
            return this.each(function (i, el) {
                var contents = $(el).contents();
                if (contents.length) contents.wrapAll(isFunction(html) ? html.call(el, i) : html);
                else el.appendChild($($.parseHTML(typeof html === 'string' ? html : html.outerHTML)[0]));
            });
        },
        unwrap: function (sel) {
            this.parent(sel).not('body').each(function (_, p) {
                while (p.firstChild) p.parentNode.insertBefore(p.firstChild, p);
                p.parentNode.removeChild(p);
            });
            return this;
        }
    });

    function getEvents(el) {
        var s = eventStore.get(el);
        if (!s) { s = {}; eventStore.set(el, s); }
        return s;
    }

    function addOne(el, type, selector, handler, one) {
        if (!el || !el.addEventListener) return;
        var events = getEvents(el);
        var list = events[type] || (events[type] = []);
        var listener = function (evt) {
            var ctx = el, t = evt.target;
            if (selector) {
                ctx = null;
                while (t && t !== el) {
                    if (t.matches && t.matches(selector)) { ctx = t; break; }
                    t = t.parentNode;
                }
                if (!ctx) return;
            }
            evt.delegateTarget = el;
            if (one) {
                el.removeEventListener(type, listener);
                var idx = list.indexOf(entry);
                if (idx !== -1) list.splice(idx, 1);
            }
            var r = handler.call(ctx, evt);
            if (r === false) {
                if (evt.preventDefault) evt.preventDefault();
                if (evt.stopPropagation) evt.stopPropagation();
            }
            return r;
        };
        var entry = { type: type, selector: selector || '', handler: handler, listener: listener };
        list.push(entry);
        el.addEventListener(type, listener);
    }

    function removeOne(el, type, selector, handler) {
        var events = eventStore.get(el);
        if (!events) return;
        function purge(t) {
            var list = events[t];
            if (!list) return;
            for (var i = list.length - 1; i >= 0; i--) {
                var e = list[i];
                if (selector && e.selector !== selector) continue;
                if (handler && e.handler !== handler) continue;
                el.removeEventListener(t, e.listener);
                list.splice(i, 1);
            }
            if (!list.length) delete events[t];
        }
        if (type) {
            var types = type.match(rnotwhite) || [];
            for (var i = 0; i < types.length; i++) purge(types[i]);
        } else {
            for (var t in events) purge(t);
        }
    }

    function dispatch(el, type, extra) {
        if (!el) return;
        var evt;
        try {
            evt = new global.CustomEvent(type, { bubbles: true, cancelable: true, detail: extra && extra[0] });
        } catch (e) {
            evt = doc.createEvent ? doc.createEvent('Event') : { type: type };
            if (evt.initEvent) evt.initEvent(type, true, true);
        }
        if (extra) evt._data = extra;
        if (el.dispatchEvent) el.dispatchEvent(evt);
        else if (typeof el['on' + type] === 'function') el['on' + type](evt);
    }

    $.fn.extend({
        on: function (types, selector, data, fn) {
            if (typeof types === 'object') {
                for (var t in types) this.on(t, selector, data, types[t]);
                return this;
            }
            if (fn == null) {
                if (data == null) {
                    if (typeof selector === 'function') { fn = selector; selector = undefined; }
                } else if (typeof selector === 'function') {
                    fn = selector; data = undefined; selector = undefined;
                } else {
                    fn = data; data = undefined;
                }
            }
            if (!fn) return this;
            void data;
            var parts = String(types).match(rnotwhite) || [];
            return eachEl(this, function (el) {
                for (var i = 0; i < parts.length; i++) addOne(el, parts[i], selector, fn, false);
            });
        },
        one: function (types, selector, data, fn) {
            if (fn == null) {
                if (typeof selector === 'function') { fn = selector; selector = undefined; }
                else if (typeof data === 'function') { fn = data; data = undefined; }
            }
            if (!fn) return this;
            var parts = String(types).match(rnotwhite) || [];
            return eachEl(this, function (el) {
                for (var i = 0; i < parts.length; i++) addOne(el, parts[i], selector, fn, true);
            });
        },
        off: function (types, selector, fn) {
            if (typeof types === 'object') {
                for (var t in types) this.off(t, selector, types[t]);
                return this;
            }
            if (typeof selector === 'function') { fn = selector; selector = undefined; }
            return eachEl(this, function (el) { removeOne(el, types, selector, fn); });
        },
        trigger: function (type, extra) {
            return this.each(function (_, el) { dispatch(el, type, extra ? [].concat(extra) : null); });
        },
        triggerHandler: function (type, extra) {
            if (!this[0]) return undefined;
            dispatch(this[0], type, extra ? [].concat(extra) : null);
            return undefined;
        }
    });

    var shortcuts = ['blur','focus','focusin','focusout','resize','scroll',
        'click','dblclick','mousedown','mouseup','mousemove','mouseover',
        'mouseout','mouseenter','mouseleave','change','select','submit',
        'keydown','keypress','keyup','contextmenu','load','unload','error'];
    shortcuts.forEach(function (name) {
        $.fn[name] = function (data, fn) {
            if (arguments.length > 0) return this.on(name, null, data, fn);
            return this.trigger(name);
        };
    });
    $.fn.hover = function (over, out) {
        return this.on('mouseenter', over).on('mouseleave', out || over);
    };

    $.fn.extend({
        show: function () {
            return eachEl(this, function (el) {
                if (el.style.display === 'none') el.style.display = '';
            });
        },
        hide: function () {
            return eachEl(this, function (el) { el.style.display = 'none'; });
        },
        toggle: function (state) {
            if (typeof state === 'boolean') return state ? this.show() : this.hide();
            return eachEl(this, function (el) {
                var cs = global.getComputedStyle ? global.getComputedStyle(el).display : el.style.display;
                if (cs === 'none' || el.style.display === 'none') el.style.display = '';
                else el.style.display = 'none';
            });
        },
        fadeIn: function (_, cb) { this.show(); if (isFunction(cb)) cb.call(this); return this; },
        fadeOut: function (_, cb) { this.hide(); if (isFunction(cb)) cb.call(this); return this; },
        fadeToggle: function (_, cb) { this.toggle(); if (isFunction(cb)) cb.call(this); return this; },
        slideUp: function (_, cb) { this.hide(); if (isFunction(cb)) cb.call(this); return this; },
        slideDown: function (_, cb) { this.show(); if (isFunction(cb)) cb.call(this); return this; },
        slideToggle: function (_, cb) { this.toggle(); if (isFunction(cb)) cb.call(this); return this; },
        animate: function (props, _dur, _easing, cb) {
            var c = isFunction(cb) ? cb : (isFunction(_easing) ? _easing : isFunction(_dur) ? _dur : null);
            this.each(function (_, el) {
                for (var k in props) {
                    var v = props[k];
                    if (typeof v === 'number') v = v + 'px';
                    el.style[$.camelCase(k)] = v;
                }
            });
            if (c) c.call(this);
            return this;
        },
        stop: function () { return this; },
        finish: function () { return this; },
        delay: function () { return this; },
        queue: function () { return this; },
        dequeue: function () { return this; }
    });

    function ajaxSettings(opts) {
        return $.extend({
            url: '',
            type: 'GET',
            method: 'GET',
            async: true,
            cache: true,
            dataType: 'text',
            contentType: 'application/x-www-form-urlencoded; charset=UTF-8',
            headers: {},
            timeout: 0,
            data: null,
            processData: true
        }, opts || {});
    }

    function param(obj, prefix) {
        var pairs = [];
        if (obj == null) return '';
        if (typeof obj === 'string') return obj;
        if (Array.isArray(obj)) {
            obj.forEach(function (item) {
                pairs.push(encodeURIComponent(item.name) + '=' +
                    encodeURIComponent(item.value == null ? '' : item.value));
            });
            return pairs.join('&').replace(/%20/g, '+');
        }
        for (var k in obj) if (hasOwn.call(obj, k)) {
            var key = prefix ? prefix + '[' + k + ']' : k;
            var v = obj[k];
            if (v && typeof v === 'object') pairs.push(param(v, key));
            else pairs.push(encodeURIComponent(key) + '=' + encodeURIComponent(v == null ? '' : v));
        }
        return pairs.join('&').replace(/%20/g, '+');
    }
    $.param = param;

    $.ajax = function (urlOrOpts, opts) {
        if (typeof urlOrOpts === 'string') {
            opts = $.extend({}, opts || {}, { url: urlOrOpts });
        } else {
            opts = urlOrOpts || {};
        }
        var s = ajaxSettings(opts);
        s.type = (s.method || s.type || 'GET').toUpperCase();
        var url = s.url;
        var body = null;
        if (s.data && s.processData && typeof s.data !== 'string' &&
            !(s.data instanceof global.FormData || (global.Blob && s.data instanceof global.Blob))) {
            s.data = param(s.data);
        }
        if (s.type === 'GET' || s.type === 'HEAD') {
            if (s.data) url += (url.indexOf('?') === -1 ? '?' : '&') + s.data;
        } else {
            body = s.data;
        }

        var jqXHR = {
            readyState: 0,
            status: 0,
            statusText: '',
            responseText: '',
            responseJSON: undefined,
            getAllResponseHeaders: function () { return ''; },
            getResponseHeader: function () { return null; },
            setRequestHeader: function (k, v) { s.headers[k] = v; return jqXHR; },
            abort: function () { jqXHR._abort && jqXHR._abort(); },
            done: function (fn) { donelist.push(fn); return jqXHR; },
            fail: function (fn) { faillist.push(fn); return jqXHR; },
            always: function (fn) { donelist.push(fn); faillist.push(fn); return jqXHR; },
            then: function (a, b) {
                return new Promise(function (res, rej) {
                    jqXHR.done(function (d) { try { res(a ? a(d) : d); } catch (e) { rej(e); } })
                         .fail(function (err) { try { rej(b ? b(err) : err); } catch (e) { rej(e); } });
                });
            }
        };
        var donelist = [], faillist = [];

        var headers = $.extend({}, s.headers);
        if (s.contentType && (s.type === 'POST' || s.type === 'PUT' || s.type === 'PATCH') &&
            !headers['Content-Type'] && !headers['content-type']) {
            headers['Content-Type'] = s.contentType;
        }
        if (!('Accept' in headers) && s.dataType) {
            var acc = { json: 'application/json', xml: 'application/xml',
                        html: 'text/html', script: 'text/javascript', text: '*/*' };
            headers.Accept = acc[s.dataType] || '*/*';
        }

        var ctrl = null;
        if (global.AbortController) ctrl = new global.AbortController();
        jqXHR._abort = function () { try { ctrl && ctrl.abort(); } catch (e) {} };

        global.fetch(url, {
            method: s.type,
            headers: headers,
            body: body,
            credentials: s.xhrFields && s.xhrFields.withCredentials ? 'include' : 'same-origin',
            signal: ctrl ? ctrl.signal : undefined
        }).then(function (resp) {
            jqXHR.status = resp.status;
            jqXHR.statusText = resp.statusText;
            return resp.text().then(function (text) {
                jqXHR.responseText = text;
                var parsed = text;
                if (s.dataType === 'json' || /application\/json/.test(resp.headers && resp.headers.get && resp.headers.get('content-type') || '')) {
                    try { parsed = text ? JSON.parse(text) : null; jqXHR.responseJSON = parsed; } catch (e) {}
                } else if (s.dataType === 'xml') {
                    parsed = $.parseXML(text);
                } else if (s.dataType === 'html') {
                    parsed = text;
                }
                jqXHR.readyState = 4;
                if (resp.ok) {
                    donelist.forEach(function (fn) { try { fn(parsed, resp.statusText, jqXHR); } catch (e) {} });
                    if (isFunction(s.success)) try { s.success(parsed, resp.statusText, jqXHR); } catch (e) {}
                } else {
                    faillist.forEach(function (fn) { try { fn(jqXHR, resp.statusText, text); } catch (e) {} });
                    if (isFunction(s.error)) try { s.error(jqXHR, resp.statusText, text); } catch (e) {}
                }
                if (isFunction(s.complete)) try { s.complete(jqXHR, resp.statusText); } catch (e) {}
            });
        }).catch(function (err) {
            jqXHR.readyState = 4;
            jqXHR.statusText = (err && err.message) || 'error';
            faillist.forEach(function (fn) { try { fn(jqXHR, 'error', err); } catch (e) {} });
            if (isFunction(s.error)) try { s.error(jqXHR, 'error', err); } catch (e) {}
            if (isFunction(s.complete)) try { s.complete(jqXHR, 'error'); } catch (e) {}
        });

        return jqXHR;
    };

    $.get = function (url, data, success, type) {
        if (isFunction(data)) { type = type || success; success = data; data = undefined; }
        return $.ajax({ url: url, type: 'GET', data: data, success: success, dataType: type });
    };
    $.post = function (url, data, success, type) {
        if (isFunction(data)) { type = type || success; success = data; data = undefined; }
        return $.ajax({ url: url, type: 'POST', data: data, success: success, dataType: type });
    };
    $.getJSON = function (url, data, success) {
        return $.get(url, data, success, 'json');
    };
    $.getScript = function (url, success) {
        return $.ajax({ url: url, dataType: 'script', success: function (code) {
            $.globalEval(code);
            if (isFunction(success)) success(code);
        }});
    };

    $.fn.load = function (url, params, callback) {
        if (!this.length) return this;
        var off = url.indexOf(' ');
        var sel = '';
        if (off >= 0) { sel = url.slice(off + 1); url = url.slice(0, off); }
        if (isFunction(params)) { callback = params; params = undefined; }
        var self = this;
        $.ajax({
            url: url, type: params ? 'POST' : 'GET', data: params, dataType: 'html'
        }).done(function (html) {
            self.each(function (_, el) {
                if (sel) {
                    var tmp = doc.createElement('div');
                    tmp.innerHTML = html;
                    var matches = tmp.querySelectorAll(sel);
                    el.innerHTML = '';
                    for (var i = 0; i < matches.length; i++) el.appendChild(matches[i]);
                } else el.innerHTML = html;
            });
            if (isFunction(callback)) callback.call(self, html, 'success');
        }).fail(function (xhr, status, err) {
            if (isFunction(callback)) callback.call(self, xhr.responseText, status, err);
        });
        return this;
    };

    $.fn.serializeArray = function () {
        var out = [];
        this.each(function (_, form) {
            if (!form.elements) return;
            for (var i = 0; i < form.elements.length; i++) {
                var el = form.elements[i];
                if (!el.name || el.disabled) continue;
                if (el.type === 'submit' || el.type === 'button' || el.type === 'reset' || el.type === 'file') continue;
                if ((el.type === 'checkbox' || el.type === 'radio') && !el.checked) continue;
                if (el.nodeName === 'SELECT' && el.multiple) {
                    for (var j = 0; j < el.options.length; j++)
                        if (el.options[j].selected)
                            out.push({ name: el.name, value: el.options[j].value });
                } else {
                    out.push({ name: el.name, value: el.value });
                }
            }
        });
        return out;
    };
    $.fn.serialize = function () { return param(this.serializeArray()); };

    function Deferred(beforeStart) {
        var state = 'pending', value, queued = { done: [], fail: [], progress: [] };
        function fire(list, v) { while (list.length) try { list.shift()(v); } catch (e) {} }
        var d = {
            state: function () { return state; },
            done: function (fn) {
                if (state === 'resolved') try { fn(value); } catch (e) {}
                else if (state === 'pending') queued.done.push(fn);
                return d.promise();
            },
            fail: function (fn) {
                if (state === 'rejected') try { fn(value); } catch (e) {}
                else if (state === 'pending') queued.fail.push(fn);
                return d.promise();
            },
            always: function (fn) { return d.done(fn).fail(fn); },
            then: function (onDone, onFail) {
                var next = Deferred();
                d.done(function (v) {
                    try { var r = isFunction(onDone) ? onDone(v) : v;
                        if (r && isFunction(r.then)) r.then(next.resolve, next.reject);
                        else next.resolve(r);
                    } catch (e) { next.reject(e); }
                });
                d.fail(function (v) {
                    try { var r = isFunction(onFail) ? onFail(v) : null;
                        if (isFunction(onFail)) {
                            if (r && isFunction(r.then)) r.then(next.resolve, next.reject);
                            else next.resolve(r);
                        } else next.reject(v);
                    } catch (e) { next.reject(e); }
                });
                return next.promise();
            },
            promise: function (obj) {
                var p = { state: d.state, done: d.done, fail: d.fail, always: d.always, then: d.then, promise: d.promise };
                if (obj) for (var k in p) obj[k] = p[k];
                return obj || p;
            },
            resolve: function (v) { if (state !== 'pending') return d; state = 'resolved'; value = v; fire(queued.done, v); return d; },
            reject:  function (v) { if (state !== 'pending') return d; state = 'rejected'; value = v; fire(queued.fail, v); return d; },
            notify:  function (v) { fire(queued.progress.slice(), v); return d; }
        };
        d.resolveWith = function (ctx, args) { return d.resolve.apply(ctx, args || []); };
        d.rejectWith  = function (ctx, args) { return d.reject.apply(ctx, args || []); };
        if (beforeStart) beforeStart.call(d, d);
        return d;
    }
    $.Deferred = Deferred;

    $.when = function () {
        var args = slice.call(arguments);
        if (!args.length) return Deferred().resolve().promise();
        var d = Deferred();
        var pending = args.length;
        var results = new Array(pending);
        args.forEach(function (a, i) {
            if (a && isFunction(a.then)) {
                a.then(function (v) { results[i] = v; if (!--pending) d.resolve.apply(d, results); },
                       function (e) { d.reject(e); });
            } else {
                results[i] = a;
                if (!--pending) d.resolve.apply(d, results);
            }
        });
        return d.promise();
    };

    global.jQuery = global.$ = $;
    if (doc) {
        try {
            Object.defineProperty(doc, 'jQuery', { value: $, configurable: true });
        } catch (e) {}
    }

})(typeof globalThis !== 'undefined' ? globalThis : this);
