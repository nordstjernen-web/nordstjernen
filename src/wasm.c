/* Nordstjernen — WebAssembly JS API implemented over the vendored WAMR interpreter.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "wasm.h"
#include "js_classid.h"

#if !defined(NS_HAVE_WAMR)

void ns_wasm_install(JSContext *ctx, JSValueConst global)
{
    (void)ctx;
    (void)global;
}

#else

#include <glib.h>
#include <string.h>

#include <wasm_export.h>

#include "ns_wamr.h"

#define NS_WASM_STACK_SIZE (1u << 20)
#define NS_WASM_RECLAIM_INTERVAL 64
#define NS_WASM_MAX_PARAMS 64
#define NS_WASM_MAX_RESULTS 16
#define NS_WASM_PAGE_SIZE 65536u
#define NS_WASM_MEMORY_MAX_BYTES (512u * 1024u * 1024u)

static JSClassID ns_wasm_module_class_id;
static JSClassID ns_wasm_instance_class_id;
static JSClassID ns_wasm_memory_class_id;
static JSClassID ns_wasm_table_class_id;
static JSClassID ns_wasm_global_class_id;
static JSClassID ns_wasm_func_class_id;

static GMutex ns_wasm_link_mutex;

typedef struct {
    JSContext *ctx;
    JSValue fn;
    guint8 param_kinds[NS_WASM_MAX_PARAMS];
    guint param_count;
    guint8 result_kind;
    guint index;
    gboolean has_result;
} ns_wasm_binding;

typedef struct {
    GPtrArray *bindings;
    GPtrArray *symbol_blocks;
    GPtrArray *strings;
    JSValue provided_memory;
    JSValue provided_table;
} ns_wasm_linkage;

typedef struct {
    guint8 *load_bytes;
    size_t len;
    wasm_module_t module;
    ns_wasm_linkage linkage;
    gboolean imports_linked;
} ns_wasm_module;

typedef struct {
    JSContext *ctx;
    wasm_module_inst_t inst;
    wasm_exec_env_t exec_env;
    JSValue module_obj;
    JSValue exports;
    JSValue memory_obj;
    JSValue pending_exc;
    ns_wasm_linkage linkage;
    gboolean has_pending;
    guint call_depth;
    guint calls_since_reclaim;
} ns_wasm_instance;

typedef struct {
    JSContext *ctx;
    JSValue instance;
    JSValue buffer;
    guint8 *base;
    size_t size;
    guint8 *staging;
    size_t staging_size;
    uint32_t max_pages;
} ns_wasm_memory;

typedef struct {
    JSContext *ctx;
    JSValue instance;
    char *name;
    wasm_valkind_t elem_kind;
} ns_wasm_table;

typedef struct {
    JSContext *ctx;
    JSValue instance;
    char *name;
    JSValue ref_value;
    wasm_val_t val;
    wasm_valkind_t kind;
    gboolean standalone;
    gboolean is_mutable;
} ns_wasm_global;

typedef struct {
    JSContext *ctx;
    JSValue instance;
    wasm_function_inst_t func;
} ns_wasm_func;

typedef struct {
    JSContext *ctx;
    JSValue v;
} ns_wasm_ref;

static GPrivate ns_wasm_thread_env_done = G_PRIVATE_INIT(NULL);
static GPrivate ns_wasm_instantiating = G_PRIVATE_INIT(NULL);

static void ns_wasm_on_memory_grown(wasm_module_inst_t inst, void *user_data);

static void
ns_wasm_linkage_init(ns_wasm_linkage *linkage)
{
    linkage->bindings = g_ptr_array_new();
    linkage->symbol_blocks = g_ptr_array_new_with_free_func(g_free);
    linkage->strings = g_ptr_array_new_with_free_func(g_free);
    linkage->provided_memory = JS_UNDEFINED;
    linkage->provided_table = JS_UNDEFINED;
}

static void
ns_wasm_linkage_clear(JSRuntime *rt, ns_wasm_linkage *linkage)
{
    if (!linkage)
        return;
    if (linkage->bindings) {
        for (guint i = 0; i < linkage->bindings->len; i++) {
            ns_wasm_binding *b = g_ptr_array_index(linkage->bindings, i);
            JS_FreeValueRT(rt, b->fn);
            g_free(b);
        }
        g_ptr_array_free(linkage->bindings, TRUE);
        linkage->bindings = NULL;
    }
    if (linkage->symbol_blocks) {
        g_ptr_array_free(linkage->symbol_blocks, TRUE);
        linkage->symbol_blocks = NULL;
    }
    if (linkage->strings) {
        g_ptr_array_free(linkage->strings, TRUE);
        linkage->strings = NULL;
    }
    JS_FreeValueRT(rt, linkage->provided_memory);
    linkage->provided_memory = JS_UNDEFINED;
    JS_FreeValueRT(rt, linkage->provided_table);
    linkage->provided_table = JS_UNDEFINED;
}

static void
ns_wasm_linkage_gc_mark(JSRuntime *rt, const ns_wasm_linkage *linkage,
                        JS_MarkFunc *mark_func)
{
    if (!linkage || !linkage->bindings)
        return;
    for (guint i = 0; i < linkage->bindings->len; i++) {
        ns_wasm_binding *b = g_ptr_array_index(linkage->bindings, i);
        JS_MarkValue(rt, b->fn, mark_func);
    }
    JS_MarkValue(rt, linkage->provided_memory, mark_func);
    JS_MarkValue(rt, linkage->provided_table, mark_func);
}

static gpointer
ns_wasm_runtime_init_once(gpointer data)
{
    RuntimeInitArgs args;
    memset(&args, 0, sizeof(args));
    args.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&args))
        return NULL;
    wasm_runtime_set_log_level(g_getenv("ND_WASM_LOG")
                                   ? WASM_LOG_LEVEL_VERBOSE
                                   : WASM_LOG_LEVEL_ERROR);
    wasm_runtime_set_enlarge_mem_success_callback(ns_wasm_on_memory_grown,
                                                  NULL);
    (void)data;
    return GINT_TO_POINTER(1);
}

static gboolean
ns_wasm_runtime_ready(void)
{
    static GOnce once = G_ONCE_INIT;
    g_once(&once, ns_wasm_runtime_init_once, NULL);
    if (!once.retval)
        return FALSE;
    if (!g_private_get(&ns_wasm_thread_env_done)) {
        wasm_runtime_init_thread_env();
        g_private_set(&ns_wasm_thread_env_done, GINT_TO_POINTER(1));
    }
    return TRUE;
}

static guint8 *
ns_wasm_memdup_bytes(const guint8 *bytes, size_t len)
{
    if (len == 0)
        return g_malloc0(1);
    return g_memdup2(bytes, len);
}

static JSValue
ns_wasm_throw_named(JSContext *ctx, const char *class_name, const char *msg)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ns = JS_GetPropertyStr(ctx, global, "WebAssembly");
    JSValue ctor = JS_IsObject(ns) ? JS_GetPropertyStr(ctx, ns, class_name)
                                   : JS_UNDEFINED;
    JS_FreeValue(ctx, ns);
    JS_FreeValue(ctx, global);
    if (JS_IsFunction(ctx, ctor)) {
        JSValue arg = JS_NewString(ctx, msg ? msg : "wasm error");
        JSValue err = JS_CallConstructor(ctx, ctor, 1, &arg);
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, ctor);
        if (!JS_IsException(err))
            return JS_Throw(ctx, err);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, ctor);
    return JS_ThrowTypeError(ctx, "%s: %s", class_name,
                             msg ? msg : "wasm error");
}

static guint8 *
ns_wasm_copy_buffer_source(JSContext *ctx, JSValueConst v, size_t *out_len)
{
    *out_len = 0;
    size_t off = 0, blen = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &off, &blen, &bpe);
    if (!JS_IsException(buf)) {
        size_t total = 0;
        uint8_t *base = JS_GetArrayBuffer(ctx, &total, buf);
        guint8 *out = NULL;
        if (base && off <= total && blen <= total - off) {
            if (blen > G_MAXUINT32) {
                JS_ThrowRangeError(ctx, "wasm module too large");
                JS_FreeValue(ctx, buf);
                return NULL;
            }
            out = ns_wasm_memdup_bytes(base + off, blen);
            *out_len = blen;
        }
        JS_FreeValue(ctx, buf);
        return out;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    size_t total = 0;
    uint8_t *base = JS_GetArrayBuffer(ctx, &total, v);
    if (base) {
        if (total > G_MAXUINT32) {
            JS_ThrowRangeError(ctx, "wasm module too large");
            return NULL;
        }
        *out_len = total;
        return ns_wasm_memdup_bytes(base, total);
    }
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    return NULL;
}

static JSValue
ns_wasm_buffer_source_error(JSContext *ctx, const char *msg)
{
    if (JS_HasException(ctx))
        return JS_EXCEPTION;
    return JS_ThrowTypeError(ctx, "%s", msg);
}

static int
ns_wasm_to_u32_index(JSContext *ctx, JSValueConst v, uint32_t *out,
                     const char *what)
{
    uint64_t idx = 0;
    if (JS_ToIndex(ctx, &idx, v))
        return -1;
    if (idx > G_MAXUINT32) {
        JS_ThrowRangeError(ctx, "%s is too large", what);
        return -1;
    }
    *out = (uint32_t)idx;
    return 0;
}

static ns_wasm_ref *
ns_wasm_ref_box(JSContext *ctx, JSValueConst v)
{
    ns_wasm_ref *r = g_new0(ns_wasm_ref, 1);
    r->ctx = ctx;
    r->v = JS_DupValue(ctx, v);
    return r;
}

static void
ns_wasm_ref_cleanup(void *p)
{
    ns_wasm_ref *r = p;
    JS_FreeValue(r->ctx, r->v);
    g_free(r);
}

static gboolean
ns_wasm_ref_register(wasm_module_inst_t inst, ns_wasm_ref *r, uint32_t *out_idx)
{
    uint32_t idx = 0;
    if (!wasm_externref_obj2ref(inst, r, &idx)) {
        ns_wasm_ref_cleanup(r);
        return FALSE;
    }
    wasm_externref_set_cleanup(inst, r, ns_wasm_ref_cleanup);
    if (out_idx)
        *out_idx = idx;
    return TRUE;
}

static JSValue
ns_wasm_ref_to_js(JSContext *ctx, void *obj)
{
    ns_wasm_ref *r = obj;
    if (!r)
        return JS_NULL;
    return JS_DupValue(ctx, r->v);
}

static ns_wasm_instance *
ns_wasm_instance_opaque(JSValueConst v)
{
    return JS_GetOpaque(v, ns_wasm_instance_class_id);
}

static int
ns_wasm_js_to_val(JSContext *ctx, ns_wasm_instance *wi, JSValueConst v,
                  wasm_valkind_t kind, wasm_val_t *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    switch (kind) {
    case WASM_I32: {
        int32_t i = 0;
        if (JS_ToInt32(ctx, &i, v))
            return -1;
        out->of.i32 = i;
        return 0;
    }
    case WASM_I64: {
        int64_t i = 0;
        if (JS_ToBigInt64(ctx, &i, v))
            return -1;
        out->of.i64 = i;
        return 0;
    }
    case WASM_F32: {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        out->of.f32 = (float)d;
        return 0;
    }
    case WASM_F64: {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        out->of.f64 = d;
        return 0;
    }
    case WASM_EXTERNREF: {
        ns_wasm_ref *r = ns_wasm_ref_box(ctx, v);
        if (!ns_wasm_ref_register(wi->inst, r, NULL)) {
            JS_ThrowInternalError(ctx, "wasm externref registration failed");
            return -1;
        }
        out->of.foreign = (uintptr_t)r;
        return 0;
    }
    default:
        JS_ThrowTypeError(ctx, "unsupported wasm value kind %d", (int)kind);
        return -1;
    }
}

static JSValue
ns_wasm_val_to_js(JSContext *ctx, const wasm_val_t *val)
{
    switch (val->kind) {
    case WASM_I32:
        return JS_NewInt32(ctx, val->of.i32);
    case WASM_I64:
        return JS_NewBigInt64(ctx, val->of.i64);
    case WASM_F32:
        return JS_NewFloat64(ctx, val->of.f32);
    case WASM_F64:
        return JS_NewFloat64(ctx, val->of.f64);
    case WASM_EXTERNREF:
        return ns_wasm_ref_to_js(ctx, (void *)val->of.foreign);
    default:
        return JS_UNDEFINED;
    }
}

static JSValue
ns_wasm_call_function(JSContext *ctx, ns_wasm_instance *wi,
                      wasm_function_inst_t func, int argc, JSValueConst *argv)
{
    if (!wi || !wi->inst || !wi->exec_env)
        return JS_ThrowTypeError(ctx, "wasm instance is gone");

    guint n_params = wasm_func_get_param_count(func, wi->inst);
    guint n_results = wasm_func_get_result_count(func, wi->inst);
    wasm_valkind_t param_kinds[NS_WASM_MAX_PARAMS];
    wasm_valkind_t result_kinds[NS_WASM_MAX_RESULTS];
    if (n_params > G_N_ELEMENTS(param_kinds) ||
        n_results > G_N_ELEMENTS(result_kinds))
        return JS_ThrowTypeError(ctx, "wasm function arity too large");
    wasm_func_get_param_types(func, wi->inst, param_kinds);
    wasm_func_get_result_types(func, wi->inst, result_kinds);

    wasm_val_t args[NS_WASM_MAX_PARAMS], results[NS_WASM_MAX_RESULTS];
    memset(results, 0, sizeof(results));
    for (guint i = 0; i < n_params; i++) {
        JSValueConst src = (int)i < argc ? argv[i] : JS_UNDEFINED;
        if (ns_wasm_js_to_val(ctx, wi, src, param_kinds[i], &args[i])) {
            if (wi->call_depth == 0)
                ns_wamr_externref_reclaim(wi->inst);
            return JS_EXCEPTION;
        }
    }

    if (wi->call_depth == 0) {
        wasm_runtime_clear_exception(wi->inst);
        if (wi->has_pending) {
            JS_FreeValue(ctx, wi->pending_exc);
            wi->pending_exc = JS_UNDEFINED;
            wi->has_pending = FALSE;
        }
    }

    wi->call_depth++;
    gboolean ok = wasm_runtime_call_wasm_a(wi->exec_env, func, n_results,
                                           results, n_params, args);
    wi->call_depth--;

    if (wi->call_depth == 0 &&
        ++wi->calls_since_reclaim >= NS_WASM_RECLAIM_INTERVAL) {
        wi->calls_since_reclaim = 0;
        ns_wamr_externref_reclaim(wi->inst);
    }

    if (!ok) {
        if (wi->call_depth > 0)
            return JS_EXCEPTION;
        if (wi->has_pending) {
            JSValue exc = wi->pending_exc;
            wi->pending_exc = JS_UNDEFINED;
            wi->has_pending = FALSE;
            wasm_runtime_clear_exception(wi->inst);
            return JS_Throw(ctx, exc);
        }
        const char *msg = wasm_runtime_get_exception(wi->inst);
        JSValue ret = ns_wasm_throw_named(ctx, "RuntimeError",
                                          msg ? msg : "wasm trap");
        wasm_runtime_clear_exception(wi->inst);
        return ret;
    }

    if (n_results == 0)
        return JS_UNDEFINED;
    if (n_results == 1)
        return ns_wasm_val_to_js(ctx, &results[0]);
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (guint i = 0; i < n_results; i++) {
        JSValue v = ns_wasm_val_to_js(ctx, &results[i]);
        if (JS_IsException(v) || JS_SetPropertyUint32(ctx, arr, i, v) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static void
ns_wasm_func_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wasm_func *f = JS_GetOpaque(val, ns_wasm_func_class_id);
    if (!f)
        return;
    JS_FreeValueRT(rt, f->instance);
    g_free(f);
}

static void
ns_wasm_func_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ns_wasm_func *f = JS_GetOpaque(val, ns_wasm_func_class_id);
    if (f)
        JS_MarkValue(rt, f->instance, mark_func);
}

static JSValue
ns_wasm_func_call(JSContext *ctx, JSValueConst func_obj, JSValueConst this_val,
                  int argc, JSValueConst *argv, int flags)
{
    ns_wasm_func *f = JS_GetOpaque(func_obj, ns_wasm_func_class_id);
    if (!f)
        return JS_ThrowTypeError(ctx, "not a wasm function");
    (void)this_val;
    (void)flags;
    return ns_wasm_call_function(ctx, ns_wasm_instance_opaque(f->instance),
                                 f->func, argc, argv);
}

static const JSClassDef ns_wasm_func_class = {
    "WasmFunction",
    .finalizer = ns_wasm_func_finalizer,
    .gc_mark = ns_wasm_func_gc_mark,
    .call = ns_wasm_func_call,
};

static JSValue
ns_wasm_make_func(JSContext *ctx, JSValueConst instance,
                  wasm_function_inst_t func)
{
    JSValue obj = JS_NewObjectClass(ctx, ns_wasm_func_class_id);
    if (JS_IsException(obj))
        return obj;
    ns_wasm_func *f = g_new0(ns_wasm_func, 1);
    f->ctx = ctx;
    f->instance = JS_DupValue(ctx, instance);
    f->func = func;
    JS_SetOpaque(obj, f);
    ns_wasm_instance *wi = ns_wasm_instance_opaque(instance);
    if (wi && wi->inst) {
        guint n_params = wasm_func_get_param_count(func, wi->inst);
        JS_DefinePropertyValueStr(ctx, obj, "length",
                                  JS_NewInt32(ctx, (int)n_params),
                                  JS_PROP_CONFIGURABLE);
    }
    return obj;
}

static void
ns_wasm_memory_detach_buffer(ns_wasm_memory *m)
{
    if (!m || JS_IsUndefined(m->buffer))
        return;
    JS_DetachArrayBuffer(m->ctx, m->buffer);
    JS_FreeValue(m->ctx, m->buffer);
    m->buffer = JS_UNDEFINED;
    m->base = NULL;
    m->size = 0;
}

static void
ns_wasm_memory_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wasm_memory *m = JS_GetOpaque(val, ns_wasm_memory_class_id);
    if (!m)
        return;
    ns_wasm_memory_detach_buffer(m);
    JS_FreeValueRT(rt, m->instance);
    g_free(m->staging);
    g_free(m);
}

static void
ns_wasm_memory_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ns_wasm_memory *m = JS_GetOpaque(val, ns_wasm_memory_class_id);
    if (!m)
        return;
    JS_MarkValue(rt, m->instance, mark_func);
    JS_MarkValue(rt, m->buffer, mark_func);
}

static const JSClassDef ns_wasm_memory_class = {
    "Memory",
    .finalizer = ns_wasm_memory_finalizer,
    .gc_mark = ns_wasm_memory_gc_mark,
};

static JSValue
ns_wasm_memory_buffer_get(JSContext *ctx, JSValueConst this_val)
{
    ns_wasm_memory *m = JS_GetOpaque2(ctx, this_val, ns_wasm_memory_class_id);
    if (!m)
        return JS_EXCEPTION;
    guint8 *base;
    size_t size;
    if (m->staging) {
        base = m->staging;
        size = m->staging_size;
    } else {
        ns_wasm_instance *wi = ns_wasm_instance_opaque(m->instance);
        if (!wi || !wi->inst)
            return JS_ThrowTypeError(ctx, "wasm instance is gone");
        wasm_memory_inst_t mem = wasm_runtime_get_memory(wi->inst, 0);
        if (!mem)
            return JS_ThrowTypeError(ctx, "wasm memory is gone");
        base = wasm_memory_get_base_address(mem);
        size = (size_t)wasm_memory_get_cur_page_count(mem) *
               wasm_memory_get_bytes_per_page(mem);
    }
    if (!JS_IsUndefined(m->buffer) && m->base == base && m->size == size)
        return JS_DupValue(ctx, m->buffer);
    ns_wasm_memory_detach_buffer(m);
    JSValue buffer = JS_NewArrayBuffer(ctx, base, size, 0, NULL, NULL, FALSE);
    if (JS_IsException(buffer)) {
        m->buffer = JS_UNDEFINED;
        m->base = NULL;
        m->size = 0;
        return buffer;
    }
    m->buffer = buffer;
    m->base = base;
    m->size = size;
    return JS_DupValue(ctx, m->buffer);
}

static JSValue
ns_wasm_memory_grow(JSContext *ctx, JSValueConst this_val, int argc,
                    JSValueConst *argv)
{
    ns_wasm_memory *m = JS_GetOpaque2(ctx, this_val, ns_wasm_memory_class_id);
    if (!m)
        return JS_EXCEPTION;
    uint32_t delta = 0;
    if (ns_wasm_to_u32_index(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                             &delta, "wasm memory.grow delta"))
        return JS_EXCEPTION;
    if (m->staging) {
        uint32_t old_pages = (uint32_t)(m->staging_size / NS_WASM_PAGE_SIZE);
        if (delta) {
            guint64 new_pages = (guint64)old_pages + delta;
            if (m->max_pages && new_pages > m->max_pages)
                return JS_ThrowRangeError(ctx, "wasm memory.grow failed");
            guint64 new_size = new_pages * NS_WASM_PAGE_SIZE;
            if (new_size > NS_WASM_MEMORY_MAX_BYTES)
                return JS_ThrowRangeError(ctx, "wasm memory.grow failed");
            m->staging = g_realloc(m->staging, new_size);
            memset(m->staging + m->staging_size, 0,
                   new_size - m->staging_size);
            m->staging_size = (size_t)new_size;
            if (!JS_IsUndefined(m->buffer) &&
                JS_RepointArrayBuffer(ctx, m->buffer, m->staging,
                                      m->staging_size) == 0) {
                m->base = m->staging;
                m->size = m->staging_size;
            } else {
                ns_wasm_memory_detach_buffer(m);
            }
        }
        return JS_NewUint32(ctx, old_pages);
    }
    ns_wasm_instance *wi = ns_wasm_instance_opaque(m->instance);
    if (!wi || !wi->inst)
        return JS_ThrowTypeError(ctx, "wasm instance is gone");
    wasm_memory_inst_t mem = wasm_runtime_get_memory(wi->inst, 0);
    if (!mem)
        return JS_ThrowTypeError(ctx, "wasm memory is gone");
    uint32_t old_pages = (uint32_t)wasm_memory_get_cur_page_count(mem);
    if (delta) {
        if (!wasm_memory_enlarge(mem, delta))
            return JS_ThrowRangeError(ctx, "wasm memory.grow failed");
        ns_wasm_memory_detach_buffer(m);
    }
    return JS_NewUint32(ctx, old_pages);
}

static const JSCFunctionListEntry ns_wasm_memory_proto_funcs[] = {
    JS_CGETSET_DEF("buffer", ns_wasm_memory_buffer_get, NULL),
    JS_CFUNC_DEF("grow", 1, ns_wasm_memory_grow),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WebAssembly.Memory",
                       JS_PROP_CONFIGURABLE),
};

static void
ns_wasm_on_memory_grown(wasm_module_inst_t inst, void *user_data)
{
    (void)user_data;
    ns_wasm_instance *wi = wasm_runtime_get_custom_data(inst);
    if (!wi || JS_IsUndefined(wi->memory_obj))
        return;
    ns_wasm_memory *m = JS_GetOpaque(wi->memory_obj, ns_wasm_memory_class_id);
    ns_wasm_memory_detach_buffer(m);
}

static void
ns_wasm_table_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wasm_table *t = JS_GetOpaque(val, ns_wasm_table_class_id);
    if (!t)
        return;
    JS_FreeValueRT(rt, t->instance);
    g_free(t->name);
    g_free(t);
}

static void
ns_wasm_table_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ns_wasm_table *t = JS_GetOpaque(val, ns_wasm_table_class_id);
    if (t)
        JS_MarkValue(rt, t->instance, mark_func);
}

static const JSClassDef ns_wasm_table_class = {
    "Table",
    .finalizer = ns_wasm_table_finalizer,
    .gc_mark = ns_wasm_table_gc_mark,
};

static void
ns_wasm_instance_detach_memory(ns_wasm_instance *wi)
{
    if (!wi || JS_IsUndefined(wi->memory_obj))
        return;
    ns_wasm_memory *m = JS_GetOpaque(wi->memory_obj,
                                     ns_wasm_memory_class_id);
    ns_wasm_memory_detach_buffer(m);
}

static ns_wasm_table *
ns_wasm_table_opaque(JSContext *ctx, JSValueConst this_val,
                     ns_wasm_instance **out_wi)
{
    ns_wasm_table *t = JS_GetOpaque2(ctx, this_val, ns_wasm_table_class_id);
    if (!t)
        return NULL;
    ns_wasm_instance *wi = ns_wasm_instance_opaque(t->instance);
    if (!wi || !wi->inst) {
        JS_ThrowTypeError(ctx, "wasm instance is gone");
        return NULL;
    }
    if (!t->name) {
        JS_ThrowTypeError(ctx, "wasm table is gone");
        return NULL;
    }
    *out_wi = wi;
    return t;
}

static JSValue
ns_wasm_table_length_get(JSContext *ctx, JSValueConst this_val)
{
    ns_wasm_instance *wi = NULL;
    ns_wasm_table *t = ns_wasm_table_opaque(ctx, this_val, &wi);
    if (!t)
        return JS_EXCEPTION;
    uint32_t size = 0;
    if (!ns_wamr_table_size(wi->inst, t->name, &size, NULL))
        return JS_ThrowTypeError(ctx, "wasm table is gone");
    return JS_NewUint32(ctx, size);
}

static JSValue
ns_wasm_table_get(JSContext *ctx, JSValueConst this_val, int argc,
                  JSValueConst *argv)
{
    ns_wasm_instance *wi = NULL;
    ns_wasm_table *t = ns_wasm_table_opaque(ctx, this_val, &wi);
    if (!t)
        return JS_EXCEPTION;
    uint32_t idx = 0;
    if (ns_wasm_to_u32_index(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                             &idx, "wasm table index"))
        return JS_EXCEPTION;
    if (t->elem_kind == WASM_FUNCREF) {
        wasm_table_inst_t tbl;
        if (!wasm_runtime_get_export_table_inst(wi->inst, t->name, &tbl))
            return JS_ThrowTypeError(ctx, "wasm table is gone");
        if (idx >= tbl.cur_size)
            return JS_ThrowRangeError(ctx, "table index out of bounds");
        wasm_function_inst_t func =
            wasm_table_get_func_inst(wi->inst, &tbl, idx);
        if (!func)
            return JS_NULL;
        return ns_wasm_make_func(ctx, t->instance, func);
    }
    uint32_t ref = NS_WAMR_NULL_REF;
    if (!ns_wamr_table_get_ref(wi->inst, t->name, idx, &ref))
        return JS_ThrowRangeError(ctx, "table index out of bounds");
    if (ref == NS_WAMR_NULL_REF)
        return JS_NULL;
    void *obj = NULL;
    if (!wasm_externref_ref2obj(ref, &obj))
        return JS_NULL;
    return ns_wasm_ref_to_js(ctx, obj);
}

static JSValue
ns_wasm_table_set(JSContext *ctx, JSValueConst this_val, int argc,
                  JSValueConst *argv)
{
    ns_wasm_instance *wi = NULL;
    ns_wasm_table *t = ns_wasm_table_opaque(ctx, this_val, &wi);
    if (!t)
        return JS_EXCEPTION;
    uint32_t idx = 0;
    if (ns_wasm_to_u32_index(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                             &idx, "wasm table index"))
        return JS_EXCEPTION;
    JSValueConst v = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (t->elem_kind == WASM_FUNCREF) {
        if (!JS_IsNull(v))
            return JS_ThrowTypeError(ctx,
                                     "setting funcref table entries from JS "
                                     "is not supported");
        if (!ns_wamr_table_set_ref(wi->inst, t->name, idx, NS_WAMR_NULL_REF))
            return JS_ThrowRangeError(ctx, "table index out of bounds");
        return JS_UNDEFINED;
    }
    uint32_t ref = NS_WAMR_NULL_REF;
    if (!JS_IsNull(v)) {
        uint32_t size = 0;
        if (!ns_wamr_table_size(wi->inst, t->name, &size, NULL))
            return JS_ThrowTypeError(ctx, "wasm table is gone");
        if (idx >= size)
            return JS_ThrowRangeError(ctx, "table index out of bounds");
        ns_wasm_ref *r = ns_wasm_ref_box(ctx, v);
        if (!ns_wasm_ref_register(wi->inst, r, &ref))
            return JS_ThrowInternalError(ctx,
                                         "wasm externref registration failed");
    }
    if (!ns_wamr_table_set_ref(wi->inst, t->name, idx, ref))
        return JS_ThrowRangeError(ctx, "table index out of bounds");
    return JS_UNDEFINED;
}

static JSValue
ns_wasm_table_grow(JSContext *ctx, JSValueConst this_val, int argc,
                   JSValueConst *argv)
{
    ns_wasm_instance *wi = NULL;
    ns_wasm_table *t = ns_wasm_table_opaque(ctx, this_val, &wi);
    if (!t)
        return JS_EXCEPTION;
    uint32_t delta = 0;
    if (ns_wasm_to_u32_index(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                             &delta, "wasm table.grow delta"))
        return JS_EXCEPTION;
    uint32_t ref = NS_WAMR_NULL_REF;
    gboolean fill_externref = FALSE;
    if (argc > 1) {
        if (t->elem_kind == WASM_FUNCREF && !JS_IsNull(argv[1]))
            return JS_ThrowTypeError(ctx,
                                     "growing funcref table entries from JS "
                                     "is not supported");
        if (t->elem_kind == WASM_EXTERNREF && !JS_IsNull(argv[1])) {
            ns_wasm_ref *r = ns_wasm_ref_box(ctx, argv[1]);
            if (!ns_wasm_ref_register(wi->inst, r, &ref))
                return JS_ThrowInternalError(
                    ctx, "wasm externref registration failed");
            fill_externref = TRUE;
        }
    }
    uint32_t old_size = 0;
    if (!ns_wamr_table_grow(wi->inst, t->name, delta, &old_size)) {
        if (fill_externref)
            ns_wamr_externref_reclaim(wi->inst);
        return JS_ThrowRangeError(ctx, "wasm table.grow failed");
    }
    if (fill_externref) {
        for (uint32_t i = 0; i < delta; i++) {
            if (!ns_wamr_table_set_ref(wi->inst, t->name, old_size + i, ref)) {
                ns_wamr_externref_reclaim(wi->inst);
                return JS_ThrowRangeError(ctx, "table index out of bounds");
            }
        }
    }
    return JS_NewUint32(ctx, old_size);
}

static const JSCFunctionListEntry ns_wasm_table_proto_funcs[] = {
    JS_CGETSET_DEF("length", ns_wasm_table_length_get, NULL),
    JS_CFUNC_DEF("get", 1, ns_wasm_table_get),
    JS_CFUNC_DEF("set", 2, ns_wasm_table_set),
    JS_CFUNC_DEF("grow", 1, ns_wasm_table_grow),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WebAssembly.Table",
                       JS_PROP_CONFIGURABLE),
};

static void
ns_wasm_global_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wasm_global *g = JS_GetOpaque(val, ns_wasm_global_class_id);
    if (!g)
        return;
    JS_FreeValueRT(rt, g->instance);
    JS_FreeValueRT(rt, g->ref_value);
    g_free(g->name);
    g_free(g);
}

static void
ns_wasm_global_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ns_wasm_global *g = JS_GetOpaque(val, ns_wasm_global_class_id);
    if (!g)
        return;
    JS_MarkValue(rt, g->instance, mark_func);
    JS_MarkValue(rt, g->ref_value, mark_func);
}

static const JSClassDef ns_wasm_global_class = {
    "Global",
    .finalizer = ns_wasm_global_finalizer,
    .gc_mark = ns_wasm_global_gc_mark,
};

static gboolean
ns_wasm_global_resolve(JSContext *ctx, JSValueConst this_val,
                       wasm_global_inst_t *out)
{
    ns_wasm_global *g = JS_GetOpaque2(ctx, this_val, ns_wasm_global_class_id);
    if (!g) {
        JS_ThrowTypeError(ctx, "not a WebAssembly.Global");
        return FALSE;
    }
    ns_wasm_instance *wi = ns_wasm_instance_opaque(g->instance);
    if (!wi || !wi->inst) {
        JS_ThrowTypeError(ctx, "wasm instance is gone");
        return FALSE;
    }
    if (!wasm_runtime_get_export_global_inst(wi->inst, g->name, out) ||
        !out->global_data) {
        JS_ThrowTypeError(ctx, "wasm global is gone");
        return FALSE;
    }
    return TRUE;
}

static int
ns_wasm_global_convert(JSContext *ctx, wasm_valkind_t kind, JSValueConst v,
                       wasm_val_t *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (JS_IsUndefined(v))
        return 0;
    switch (kind) {
    case WASM_I32:
        return JS_ToInt32(ctx, &out->of.i32, v) ? -1 : 0;
    case WASM_I64:
        return JS_ToBigInt64(ctx, &out->of.i64, v) ? -1 : 0;
    case WASM_F32: {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        out->of.f32 = (float)d;
        return 0;
    }
    case WASM_F64:
        return JS_ToFloat64(ctx, &out->of.f64, v) ? -1 : 0;
    default:
        JS_ThrowTypeError(ctx, "unsupported wasm global type");
        return -1;
    }
}

static JSValue
ns_wasm_global_standalone_get(JSContext *ctx, const ns_wasm_global *g)
{
    if (g->kind == WASM_EXTERNREF || g->kind == WASM_FUNCREF)
        return JS_DupValue(ctx, g->ref_value);
    return ns_wasm_val_to_js(ctx, &g->val);
}

static JSValue
ns_wasm_global_value_get(JSContext *ctx, JSValueConst this_val)
{
    ns_wasm_global *sg = JS_GetOpaque(this_val, ns_wasm_global_class_id);
    if (sg && sg->standalone)
        return ns_wasm_global_standalone_get(ctx, sg);
    wasm_global_inst_t gi;
    if (!ns_wasm_global_resolve(ctx, this_val, &gi))
        return JS_EXCEPTION;
    switch (gi.kind) {
    case WASM_I32:
        return JS_NewInt32(ctx, *(int32_t *)gi.global_data);
    case WASM_I64:
        return JS_NewBigInt64(ctx, *(int64_t *)gi.global_data);
    case WASM_F32:
        return JS_NewFloat64(ctx, *(float *)gi.global_data);
    case WASM_F64:
        return JS_NewFloat64(ctx, *(double *)gi.global_data);
    default:
        return JS_ThrowTypeError(ctx, "unsupported wasm global type");
    }
}

static JSValue
ns_wasm_global_value_set(JSContext *ctx, JSValueConst this_val,
                         JSValueConst val)
{
    ns_wasm_global *sg = JS_GetOpaque(this_val, ns_wasm_global_class_id);
    if (sg && sg->standalone) {
        if (!sg->is_mutable)
            return JS_ThrowTypeError(ctx, "WebAssembly.Global is immutable");
        if (sg->kind == WASM_EXTERNREF || sg->kind == WASM_FUNCREF) {
            JS_FreeValue(ctx, sg->ref_value);
            sg->ref_value = JS_DupValue(ctx, val);
            return JS_UNDEFINED;
        }
        wasm_val_t next;
        if (ns_wasm_global_convert(ctx, sg->kind, val, &next))
            return JS_EXCEPTION;
        sg->val = next;
        return JS_UNDEFINED;
    }
    wasm_global_inst_t gi;
    if (!ns_wasm_global_resolve(ctx, this_val, &gi))
        return JS_EXCEPTION;
    if (!gi.is_mutable)
        return JS_ThrowTypeError(ctx, "WebAssembly.Global is immutable");
    switch (gi.kind) {
    case WASM_I32: {
        int32_t i = 0;
        if (JS_ToInt32(ctx, &i, val))
            return JS_EXCEPTION;
        *(int32_t *)gi.global_data = i;
        break;
    }
    case WASM_I64: {
        int64_t i = 0;
        if (JS_ToBigInt64(ctx, &i, val))
            return JS_EXCEPTION;
        *(int64_t *)gi.global_data = i;
        break;
    }
    case WASM_F32: {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, val))
            return JS_EXCEPTION;
        *(float *)gi.global_data = (float)d;
        break;
    }
    case WASM_F64: {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, val))
            return JS_EXCEPTION;
        *(double *)gi.global_data = d;
        break;
    }
    default:
        return JS_ThrowTypeError(ctx, "unsupported wasm global type");
    }
    return JS_UNDEFINED;
}

static JSValue
ns_wasm_global_value_of(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    return ns_wasm_global_value_get(ctx, this_val);
}

static const JSCFunctionListEntry ns_wasm_global_proto_funcs[] = {
    JS_CGETSET_DEF("value", ns_wasm_global_value_get,
                   ns_wasm_global_value_set),
    JS_CFUNC_DEF("valueOf", 0, ns_wasm_global_value_of),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WebAssembly.Global",
                       JS_PROP_CONFIGURABLE),
};

static void
ns_wasm_module_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wasm_module *m = JS_GetOpaque(val, ns_wasm_module_class_id);
    if (!m)
        return;
    if (m->module)
        wasm_runtime_unload(m->module);
    ns_wasm_linkage_clear(rt, &m->linkage);
    g_free(m->load_bytes);
    g_free(m);
}

static void
ns_wasm_module_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    (void)rt;
    (void)val;
    (void)mark_func;
}

static const JSClassDef ns_wasm_module_class = {
    "Module",
    .finalizer = ns_wasm_module_finalizer,
    .gc_mark = ns_wasm_module_gc_mark,
};

static void
ns_wasm_instance_finalizer(JSRuntime *rt, JSValue val)
{
    ns_wasm_instance *wi = JS_GetOpaque(val, ns_wasm_instance_class_id);
    if (!wi)
        return;
    ns_wasm_instance_detach_memory(wi);
    if (wi->exec_env)
        wasm_runtime_destroy_exec_env(wi->exec_env);
    if (wi->inst) {
        wasm_runtime_set_custom_data(wi->inst, NULL);
        wasm_runtime_deinstantiate(wi->inst);
    }
    ns_wasm_linkage_clear(rt, &wi->linkage);
    JS_FreeValueRT(rt, wi->module_obj);
    JS_FreeValueRT(rt, wi->exports);
    JS_FreeValueRT(rt, wi->memory_obj);
    JS_FreeValueRT(rt, wi->pending_exc);
    g_free(wi);
}

static void
ns_wasm_instance_gc_mark(JSRuntime *rt, JSValueConst val,
                         JS_MarkFunc *mark_func)
{
    ns_wasm_instance *wi = JS_GetOpaque(val, ns_wasm_instance_class_id);
    if (!wi)
        return;
    JS_MarkValue(rt, wi->module_obj, mark_func);
    JS_MarkValue(rt, wi->exports, mark_func);
    JS_MarkValue(rt, wi->memory_obj, mark_func);
    JS_MarkValue(rt, wi->pending_exc, mark_func);
    ns_wasm_linkage_gc_mark(rt, &wi->linkage, mark_func);
}

static const JSClassDef ns_wasm_instance_class = {
    "Instance",
    .finalizer = ns_wasm_instance_finalizer,
    .gc_mark = ns_wasm_instance_gc_mark,
};

static void
ns_wasm_native_fail(JSContext *ctx, ns_wasm_instance *wi,
                    wasm_module_inst_t inst, const char *msg)
{
    if (!JS_HasException(ctx))
        JS_ThrowInternalError(ctx, "%s", msg ? msg : "wasm import failed");
    JSValue exc = JS_GetException(ctx);
    if (wi && !wi->has_pending) {
        wi->pending_exc = exc;
        wi->has_pending = TRUE;
    } else {
        JS_FreeValue(ctx, exc);
    }
    if (inst)
        wasm_runtime_set_exception(inst, msg ? msg : "wasm import failed");
}

static void
ns_wasm_native_dispatch(wasm_exec_env_t exec_env, uint64_t *args)
{
    ns_wasm_binding *desc = wasm_runtime_get_function_attachment(exec_env);
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    ns_wasm_instance *wi = inst ? wasm_runtime_get_custom_data(inst) : NULL;
    if (!wi)
        wi = g_private_get(&ns_wasm_instantiating);
    ns_wasm_binding *b = NULL;
    if (wi && desc && wi->linkage.bindings &&
        desc->index < wi->linkage.bindings->len)
        b = g_ptr_array_index(wi->linkage.bindings, desc->index);
    if (!b || !b->ctx) {
        if (inst)
            wasm_runtime_set_exception(inst, "wasm import binding is gone");
        return;
    }
    JSContext *ctx = b->ctx;

    JSValue argv[NS_WASM_MAX_PARAMS];
    guint argc = b->param_count;
    for (guint i = 0; i < argc; i++) {
        guint64 *slot = &args[i];
        switch (b->param_kinds[i]) {
        case WASM_I32:
            argv[i] = JS_NewInt32(ctx, *(int32_t *)slot);
            break;
        case WASM_I64:
            argv[i] = JS_NewBigInt64(ctx, *(int64_t *)slot);
            break;
        case WASM_F32:
            argv[i] = JS_NewFloat64(ctx, *(float *)slot);
            break;
        case WASM_F64:
            argv[i] = JS_NewFloat64(ctx, *(double *)slot);
            break;
        case WASM_EXTERNREF:
            argv[i] = ns_wasm_ref_to_js(ctx, *(void **)slot);
            break;
        default:
            argv[i] = JS_UNDEFINED;
            break;
        }
        if (JS_IsException(argv[i])) {
            for (guint j = 0; j < i; j++)
                JS_FreeValue(ctx, argv[j]);
            ns_wasm_native_fail(ctx, wi, inst,
                                "wasm import argument conversion failed");
            return;
        }
    }

    JSValue ret = JS_Call(ctx, b->fn, JS_UNDEFINED, (int)argc, argv);
    for (guint i = 0; i < argc; i++)
        JS_FreeValue(ctx, argv[i]);

    if (JS_IsException(ret)) {
        if (wi && !wi->has_pending) {
            wi->pending_exc = JS_GetException(ctx);
            wi->has_pending = TRUE;
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        if (inst)
            wasm_runtime_set_exception(inst, "uncaught JavaScript exception");
        return;
    }

    if (b->has_result) {
        switch (b->result_kind) {
        case WASM_I32: {
            int32_t i = 0;
            if (JS_ToInt32(ctx, &i, ret)) {
                ns_wasm_native_fail(ctx, wi, inst,
                                    "wasm import result conversion failed");
                JS_FreeValue(ctx, ret);
                return;
            }
            *(int32_t *)args = i;
            break;
        }
        case WASM_I64: {
            int64_t i = 0;
            if (JS_ToBigInt64(ctx, &i, ret)) {
                ns_wasm_native_fail(ctx, wi, inst,
                                    "wasm import result conversion failed");
                JS_FreeValue(ctx, ret);
                return;
            }
            *(int64_t *)args = i;
            break;
        }
        case WASM_F32:
        case WASM_F64: {
            double d = 0;
            if (JS_ToFloat64(ctx, &d, ret)) {
                ns_wasm_native_fail(ctx, wi, inst,
                                    "wasm import result conversion failed");
                JS_FreeValue(ctx, ret);
                return;
            }
            if (b->result_kind == WASM_F32)
                *(float *)args = (float)d;
            else
                *(double *)args = d;
            break;
        }
        case WASM_EXTERNREF: {
            ns_wasm_ref *r = ns_wasm_ref_box(ctx, ret);
            if (ns_wasm_ref_register(inst, r, NULL))
                *(void **)args = r;
            else {
                ns_wasm_native_fail(ctx, wi, inst,
                                    "wasm externref registration failed");
                JS_FreeValue(ctx, ret);
                return;
            }
            break;
        }
        default:
            break;
        }
    }
    JS_FreeValue(ctx, ret);
}

static char
ns_wasm_signature_char(wasm_valkind_t kind)
{
    switch (kind) {
    case WASM_I32:
        return 'i';
    case WASM_I64:
        return 'I';
    case WASM_F32:
        return 'f';
    case WASM_F64:
        return 'F';
    case WASM_EXTERNREF:
        return 'r';
    case WASM_FUNCREF:
        return 'i';
    default:
        return 'i';
    }
}

static gboolean
ns_wasm_kind_supported(wasm_valkind_t kind)
{
    return kind == WASM_I32 || kind == WASM_I64 || kind == WASM_F32 ||
           kind == WASM_F64 || kind == WASM_EXTERNREF;
}

static gboolean
ns_wasm_check_import_func_type(JSContext *ctx, wasm_func_type_t type)
{
    guint param_count = wasm_func_type_get_param_count(type);
    guint n_results = wasm_func_type_get_result_count(type);
    if (param_count > NS_WASM_MAX_PARAMS || n_results > 1) {
        ns_wasm_throw_named(ctx, "LinkError",
                            "unsupported wasm import function arity");
        return FALSE;
    }
    for (guint p = 0; p < param_count; p++) {
        wasm_valkind_t kind = wasm_func_type_get_param_valkind(type, p);
        if (!ns_wasm_kind_supported(kind)) {
            ns_wasm_throw_named(ctx, "LinkError",
                                "unsupported wasm import parameter type");
            return FALSE;
        }
    }
    if (n_results > 0 &&
        !ns_wasm_kind_supported(wasm_func_type_get_result_valkind(type, 0))) {
        ns_wasm_throw_named(ctx, "LinkError",
                            "unsupported wasm import result type");
        return FALSE;
    }
    return TRUE;
}

static gboolean
ns_wasm_global_current_value(JSValueConst v, wasm_val_t *out)
{
    ns_wasm_global *g = JS_GetOpaque(v, ns_wasm_global_class_id);
    if (!g)
        return FALSE;
    if (g->standalone) {
        if (g->kind == WASM_EXTERNREF || g->kind == WASM_FUNCREF)
            return FALSE;
        *out = g->val;
        return TRUE;
    }
    ns_wasm_instance *wi = ns_wasm_instance_opaque(g->instance);
    if (!wi || !wi->inst)
        return FALSE;
    wasm_global_inst_t gi;
    if (!wasm_runtime_get_export_global_inst(wi->inst, g->name, &gi) ||
        !gi.global_data)
        return FALSE;
    memset(out, 0, sizeof(*out));
    out->kind = gi.kind;
    switch (gi.kind) {
    case WASM_I32:
        out->of.i32 = *(int32_t *)gi.global_data;
        return TRUE;
    case WASM_I64:
        out->of.i64 = *(int64_t *)gi.global_data;
        return TRUE;
    case WASM_F32:
        out->of.f32 = *(float *)gi.global_data;
        return TRUE;
    case WASM_F64:
        out->of.f64 = *(double *)gi.global_data;
        return TRUE;
    default:
        return FALSE;
    }
}

static gboolean
ns_wasm_bind_imports(JSContext *ctx, wasm_module_t module,
                     ns_wasm_linkage *linkage, JSValueConst import_object)
{
    int32_t n = wasm_runtime_get_import_count(module);
    for (int32_t i = 0; i < n; i++) {
        wasm_import_t imp;
        wasm_runtime_get_import_type(module, i, &imp);
        if (!JS_IsObject(import_object)) {
            ns_wasm_throw_named(ctx, "LinkError", "import object required");
            return FALSE;
        }
        if (imp.kind != WASM_IMPORT_EXPORT_KIND_FUNC) {
            JSValue ns = JS_GetPropertyStr(ctx, import_object,
                                           imp.module_name);
            JSValue v = JS_IsObject(ns) ? JS_GetPropertyStr(ctx, ns, imp.name)
                                        : JS_UNDEFINED;
            JS_FreeValue(ctx, ns);
            switch (imp.kind) {
            case WASM_IMPORT_EXPORT_KIND_MEMORY:
                if (JS_GetOpaque(v, ns_wasm_memory_class_id) &&
                    JS_IsUndefined(linkage->provided_memory))
                    linkage->provided_memory = JS_DupValue(ctx, v);
                break;
            case WASM_IMPORT_EXPORT_KIND_TABLE:
                if (JS_GetOpaque(v, ns_wasm_table_class_id) &&
                    JS_IsUndefined(linkage->provided_table))
                    linkage->provided_table = JS_DupValue(ctx, v);
                break;
            case WASM_IMPORT_EXPORT_KIND_GLOBAL: {
                wasm_val_t gval;
                if (ns_wasm_global_current_value(v, &gval)) {
                    if (gval.kind ==
                        wasm_global_type_get_valkind(imp.u.global_type))
                        wasm_runtime_link_import_global(module, i, &gval);
                    break;
                }
                if (JS_IsNumber(v) || JS_IsBigInt(v)) {
                    wasm_val_t val;
                    memset(&val, 0, sizeof(val));
                    val.kind =
                        wasm_global_type_get_valkind(imp.u.global_type);
                    gboolean ok = TRUE;
                    if (val.kind == WASM_I64 && JS_IsBigInt(v)) {
                        ok = !JS_ToBigInt64(ctx, &val.of.i64, v);
                    } else {
                        double d = 0;
                        ok = !JS_ToFloat64(ctx, &d, v);
                        switch (val.kind) {
                        case WASM_I32:
                            val.of.i32 = (int32_t)(int64_t)d;
                            break;
                        case WASM_I64:
                            val.of.i64 = (int64_t)d;
                            break;
                        case WASM_F32:
                            val.of.f32 = (float)d;
                            break;
                        case WASM_F64:
                            val.of.f64 = d;
                            break;
                        default:
                            ok = FALSE;
                            break;
                        }
                    }
                    if (ok)
                        wasm_runtime_link_import_global(module, i, &val);
                    else if (JS_HasException(ctx)) {
                        JS_FreeValue(ctx, v);
                        return FALSE;
                    }
                }
                break;
            }
            default:
                break;
            }
            JS_FreeValue(ctx, v);
            continue;
        }
        JSValue ns = JS_GetPropertyStr(ctx, import_object, imp.module_name);
        JSValue fn = JS_IsObject(ns) ? JS_GetPropertyStr(ctx, ns, imp.name)
                                     : JS_UNDEFINED;
        JS_FreeValue(ctx, ns);
        if (!JS_IsFunction(ctx, fn)) {
            JS_FreeValue(ctx, fn);
            char *msg = g_strdup_printf("import %s.%s is not a function",
                                        imp.module_name, imp.name);
            ns_wasm_throw_named(ctx, "LinkError", msg);
            g_free(msg);
            return FALSE;
        }

        if (!ns_wasm_check_import_func_type(ctx, imp.u.func_type)) {
            JS_FreeValue(ctx, fn);
            return FALSE;
        }

        ns_wasm_binding *b = g_new0(ns_wasm_binding, 1);
        b->ctx = ctx;
        b->fn = fn;
        b->index = linkage->bindings->len;
        b->param_count = wasm_func_type_get_param_count(imp.u.func_type);
        for (guint p = 0; p < b->param_count; p++)
            b->param_kinds[p] =
                wasm_func_type_get_param_valkind(imp.u.func_type, p);
        b->has_result = wasm_func_type_get_result_count(imp.u.func_type) > 0;
        if (b->has_result)
            b->result_kind =
                wasm_func_type_get_result_valkind(imp.u.func_type, 0);
        g_ptr_array_add(linkage->bindings, b);
    }
    return TRUE;
}

static gboolean
ns_wasm_link_import_symbols(JSContext *ctx, ns_wasm_module *mod)
{
    if (mod->imports_linked)
        return TRUE;

    int32_t n = wasm_runtime_get_import_count(mod->module);
    ns_wasm_linkage symbols;
    ns_wasm_linkage_init(&symbols);
    GHashTable *groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                               (GDestroyNotify)g_array_unref);

    for (int32_t i = 0; i < n; i++) {
        wasm_import_t imp;
        wasm_runtime_get_import_type(mod->module, i, &imp);
        if (imp.kind != WASM_IMPORT_EXPORT_KIND_FUNC)
            continue;
        if (!ns_wasm_check_import_func_type(ctx, imp.u.func_type)) {
            g_hash_table_destroy(groups);
            ns_wasm_linkage_clear(JS_GetRuntime(ctx), &symbols);
            return FALSE;
        }

        ns_wasm_binding *b = g_new0(ns_wasm_binding, 1);
        b->fn = JS_UNDEFINED;
        b->index = symbols.bindings->len;
        b->param_count = wasm_func_type_get_param_count(imp.u.func_type);
        for (guint p = 0; p < b->param_count; p++)
            b->param_kinds[p] =
                wasm_func_type_get_param_valkind(imp.u.func_type, p);
        b->has_result = wasm_func_type_get_result_count(imp.u.func_type) > 0;
        if (b->has_result)
            b->result_kind =
                wasm_func_type_get_result_valkind(imp.u.func_type, 0);
        g_ptr_array_add(symbols.bindings, b);

        GString *sig = g_string_new("(");
        for (guint p = 0; p < b->param_count; p++)
            g_string_append_c(sig, ns_wasm_signature_char(b->param_kinds[p]));
        g_string_append_c(sig, ')');
        if (b->has_result)
            g_string_append_c(sig, ns_wasm_signature_char(b->result_kind));
        char *sig_str = g_string_free(sig, FALSE);
        g_ptr_array_add(symbols.strings, sig_str);
        char *name_str = g_strdup(imp.name);
        g_ptr_array_add(symbols.strings, name_str);

        GArray *group = g_hash_table_lookup(groups, imp.module_name);
        if (!group) {
            group = g_array_new(FALSE, TRUE, sizeof(NativeSymbol));
            g_hash_table_insert(groups, g_strdup(imp.module_name), group);
        }
        NativeSymbol sym = {
            .symbol = name_str,
            .func_ptr = (void *)ns_wasm_native_dispatch,
            .signature = sig_str,
            .attachment = b,
        };
        g_array_append_val(group, sym);
    }

    GHashTableIter it;
    gpointer key, value;
    GPtrArray *reg_names = g_ptr_array_new();
    GPtrArray *reg_blocks = g_ptr_array_new();
    g_hash_table_iter_init(&it, groups);

    g_mutex_lock(&ns_wasm_link_mutex);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        GArray *group = value;
        NativeSymbol *syms =
            g_memdup2(group->data, group->len * sizeof(NativeSymbol));
        g_ptr_array_add(symbols.symbol_blocks, syms);
        char *reg_name = g_strdup(key);
        g_ptr_array_add(symbols.strings, reg_name);
        wasm_runtime_register_natives_raw(reg_name, syms, group->len);
        g_ptr_array_add(reg_names, reg_name);
        g_ptr_array_add(reg_blocks, syms);
    }

    gboolean ok = wasm_runtime_resolve_symbols(mod->module);

    for (guint i = 0; i < reg_names->len; i++)
        wasm_runtime_unregister_natives(g_ptr_array_index(reg_names, i),
                                        g_ptr_array_index(reg_blocks, i));
    g_mutex_unlock(&ns_wasm_link_mutex);

    g_ptr_array_free(reg_names, TRUE);
    g_ptr_array_free(reg_blocks, TRUE);
    g_hash_table_destroy(groups);

    if (!ok) {
        GString *msg = g_string_new("unresolved wasm imports:");
        for (int32_t i = 0; i < n; i++) {
            wasm_import_t imp;
            wasm_runtime_get_import_type(mod->module, i, &imp);
            if (imp.kind == WASM_IMPORT_EXPORT_KIND_FUNC && !imp.linked)
                g_string_append_printf(msg, " %s.%s", imp.module_name,
                                       imp.name);
        }
        ns_wasm_throw_named(ctx, "LinkError", msg->str);
        g_string_free(msg, TRUE);
        ns_wasm_linkage_clear(JS_GetRuntime(ctx), &symbols);
        return FALSE;
    }
    ns_wasm_linkage_clear(JS_GetRuntime(ctx), &mod->linkage);
    mod->linkage = symbols;
    mod->imports_linked = TRUE;
    return TRUE;
}

static JSValue
ns_wasm_build_exports(JSContext *ctx, JSValueConst instance_obj,
                      ns_wasm_instance *wi, ns_wasm_module *mod)
{
    JSValue exports = JS_NewObject(ctx);
    if (JS_IsException(exports))
        return exports;
    int32_t n = wasm_runtime_get_export_count(mod->module);
    for (int32_t i = 0; i < n; i++) {
        wasm_export_t exp;
        wasm_runtime_get_export_type(mod->module, i, &exp);
        switch (exp.kind) {
        case WASM_IMPORT_EXPORT_KIND_FUNC: {
            wasm_function_inst_t func =
                wasm_runtime_lookup_function(wi->inst, exp.name);
            if (func) {
                JSValue fn = ns_wasm_make_func(ctx, instance_obj, func);
                if (JS_IsException(fn)) {
                    JS_FreeValue(ctx, exports);
                    return fn;
                }
                JS_DefinePropertyValueStr(ctx, fn, "name",
                                          JS_NewString(ctx, exp.name),
                                          JS_PROP_CONFIGURABLE);
                if (JS_SetPropertyStr(ctx, exports, exp.name, fn) < 0) {
                    JS_FreeValue(ctx, exports);
                    return JS_EXCEPTION;
                }
            }
            break;
        }
        case WASM_IMPORT_EXPORT_KIND_MEMORY: {
            JSValue mem_obj;
            if (!JS_IsUndefined(wi->memory_obj)) {
                mem_obj = JS_DupValue(ctx, wi->memory_obj);
            } else {
                mem_obj = JS_NewObjectClass(ctx, ns_wasm_memory_class_id);
                if (JS_IsException(mem_obj)) {
                    JS_FreeValue(ctx, exports);
                    return mem_obj;
                }
                ns_wasm_memory *m = g_new0(ns_wasm_memory, 1);
                m->ctx = ctx;
                m->instance = JS_DupValue(ctx, instance_obj);
                m->buffer = JS_UNDEFINED;
                JS_SetOpaque(mem_obj, m);
                wi->memory_obj = JS_DupValue(ctx, mem_obj);
            }
            if (JS_SetPropertyStr(ctx, exports, exp.name, mem_obj) < 0) {
                JS_FreeValue(ctx, exports);
                return JS_EXCEPTION;
            }
            break;
        }
        case WASM_IMPORT_EXPORT_KIND_TABLE: {
            JSValue tbl_obj = JS_NewObjectClass(ctx, ns_wasm_table_class_id);
            if (JS_IsException(tbl_obj)) {
                JS_FreeValue(ctx, exports);
                return tbl_obj;
            }
            ns_wasm_table *t = g_new0(ns_wasm_table, 1);
            t->ctx = ctx;
            t->instance = JS_DupValue(ctx, instance_obj);
            t->name = g_strdup(exp.name);
            t->elem_kind = wasm_table_type_get_elem_kind(exp.u.table_type);
            JS_SetOpaque(tbl_obj, t);
            if (JS_SetPropertyStr(ctx, exports, exp.name, tbl_obj) < 0) {
                JS_FreeValue(ctx, exports);
                return JS_EXCEPTION;
            }
            break;
        }
        case WASM_IMPORT_EXPORT_KIND_GLOBAL: {
            wasm_global_inst_t gi;
            if (!wasm_runtime_get_export_global_inst(wi->inst, exp.name, &gi))
                break;
            JSValue g_obj = JS_NewObjectClass(ctx, ns_wasm_global_class_id);
            if (JS_IsException(g_obj)) {
                JS_FreeValue(ctx, exports);
                return g_obj;
            }
            ns_wasm_global *g = g_new0(ns_wasm_global, 1);
            g->ctx = ctx;
            g->instance = JS_DupValue(ctx, instance_obj);
            g->name = g_strdup(exp.name);
            g->ref_value = JS_UNDEFINED;
            JS_SetOpaque(g_obj, g);
            if (JS_SetPropertyStr(ctx, exports, exp.name, g_obj) < 0) {
                JS_FreeValue(ctx, exports);
                return JS_EXCEPTION;
            }
            break;
        }
        default:
            break;
        }
    }
    return exports;
}

static JSValue
ns_wasm_module_from_bytes(JSContext *ctx, const guint8 *bytes, size_t len)
{
    if (!ns_wasm_runtime_ready())
        return JS_ThrowInternalError(ctx, "wasm runtime init failed");
    if (len > G_MAXUINT32)
        return ns_wasm_throw_named(ctx, "CompileError",
                                   "wasm module too large");

    ns_wasm_module *mod = g_new0(ns_wasm_module, 1);
    ns_wasm_linkage_init(&mod->linkage);
    mod->load_bytes = ns_wasm_memdup_bytes(bytes, len);
    mod->len = len;

    char error_buf[192] = "";
    LoadArgs load_args;
    memset(&load_args, 0, sizeof(load_args));
    load_args.name = (char *)"";
    load_args.no_resolve = true;
    mod->module = wasm_runtime_load_ex(mod->load_bytes, (uint32_t)len,
                                       &load_args, error_buf,
                                       sizeof(error_buf));
    JSValue obj = mod->module
                      ? JS_NewObjectClass(ctx, ns_wasm_module_class_id)
                      : JS_EXCEPTION;
    if (JS_IsException(obj)) {
        if (mod->module)
            wasm_runtime_unload(mod->module);
        ns_wasm_linkage_clear(JS_GetRuntime(ctx), &mod->linkage);
        g_free(mod->load_bytes);
        g_free(mod);
        if (!JS_HasException(ctx))
            return ns_wasm_throw_named(ctx, "CompileError", error_buf);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, mod);
    return obj;
}

static int
ns_wasm_segment_range_cmp(gconstpointer a, gconstpointer b)
{
    const guint64 *ra = a, *rb = b;
    if (ra[0] < rb[0]) return -1;
    if (ra[0] > rb[0]) return 1;
    return 0;
}

static gboolean
ns_wasm_copy_staging_around_segments(ns_wasm_instance *wi, guint8 *base,
                                     size_t size, const guint8 *staging,
                                     size_t staging_size)
{
    wasm_module_t module = wasm_runtime_get_module(wi->inst);
    int32_t n = wasm_runtime_get_data_segment_count(module);
    if (n < 0)
        return FALSE;

    guint64 copy_len = MIN(size, staging_size);
    GArray *ranges = g_array_new(FALSE, FALSE, sizeof(guint64) * 2);
    for (int32_t i = 0; i < n; i++) {
        guint64 off = 0, range[2];
        uint32_t len = 0;
        if (!wasm_runtime_get_data_segment_range(module, i, &off, &len)) {
            g_array_free(ranges, TRUE);
            return FALSE;
        }
        if (len == 0 || off >= copy_len)
            continue;
        range[0] = off;
        range[1] = MIN(off + len, copy_len);
        g_array_append_val(ranges, range);
    }
    g_array_sort(ranges, ns_wasm_segment_range_cmp);

    guint64 pos = 0;
    for (guint i = 0; i < ranges->len; i++) {
        const guint64 *range =
            &g_array_index(ranges, guint64, (gsize)i * 2);
        if (range[0] > pos)
            memcpy(base + pos, staging + pos, range[0] - pos);
        if (range[1] > pos)
            pos = range[1];
    }
    if (copy_len > pos)
        memcpy(base + pos, staging + pos, copy_len - pos);
    g_array_free(ranges, TRUE);
    return TRUE;
}

static void
ns_wasm_adopt_provided_imports(JSContext *ctx, JSValueConst instance_obj,
                               ns_wasm_instance *wi)
{
    if (!JS_IsUndefined(wi->linkage.provided_memory)) {
        ns_wasm_memory *m = JS_GetOpaque(wi->linkage.provided_memory,
                                         ns_wasm_memory_class_id);
        wasm_memory_inst_t mem =
            m && m->staging ? wasm_runtime_get_memory(wi->inst, 0) : NULL;
        if (mem) {
            guint8 *base = wasm_memory_get_base_address(mem);
            size_t size = (size_t)wasm_memory_get_cur_page_count(mem) *
                          wasm_memory_get_bytes_per_page(mem);
            ns_wasm_copy_staging_around_segments(wi, base, size, m->staging,
                                                 m->staging_size);
            if (!JS_IsUndefined(m->buffer) &&
                JS_RepointArrayBuffer(ctx, m->buffer, base, size) == 0) {
                m->base = base;
                m->size = size;
            } else {
                ns_wasm_memory_detach_buffer(m);
            }
            g_free(m->staging);
            m->staging = NULL;
            m->staging_size = 0;
            JS_FreeValue(ctx, m->instance);
            m->instance = JS_DupValue(ctx, instance_obj);
            if (JS_IsUndefined(wi->memory_obj))
                wi->memory_obj =
                    JS_DupValue(ctx, wi->linkage.provided_memory);
        }
    }
    if (!JS_IsUndefined(wi->linkage.provided_table)) {
        ns_wasm_table *t = JS_GetOpaque(wi->linkage.provided_table,
                                        ns_wasm_table_class_id);
        if (t && JS_IsUndefined(t->instance))
            t->instance = JS_DupValue(ctx, instance_obj);
    }
}

static JSValue
ns_wasm_instance_create(JSContext *ctx, JSValueConst module_obj,
                        JSValueConst import_object)
{
    ns_wasm_module *mod =
        JS_GetOpaque2(ctx, module_obj, ns_wasm_module_class_id);
    if (!mod || !mod->module)
        return JS_EXCEPTION;

    JSRuntime *rt = JS_GetRuntime(ctx);
    ns_wasm_linkage linkage;
    ns_wasm_linkage_init(&linkage);

    char error_buf[192] = "";
    wasm_module_t inst_module = mod->module;
    int32_t import_count = wasm_runtime_get_import_count(mod->module);
    if (import_count > 0) {
        if (!ns_wasm_bind_imports(ctx, inst_module, &linkage,
                                  import_object)) {
            ns_wasm_linkage_clear(rt, &linkage);
            return JS_EXCEPTION;
        }
        if (!ns_wasm_link_import_symbols(ctx, mod)) {
            ns_wasm_linkage_clear(rt, &linkage);
            return JS_EXCEPTION;
        }
    }

    if (import_count == 0 &&
        !ns_wasm_bind_imports(ctx, inst_module, &linkage, import_object)) {
        ns_wasm_linkage_clear(rt, &linkage);
        return JS_EXCEPTION;
    }

    ns_wasm_instance *wi = g_new0(ns_wasm_instance, 1);
    wi->ctx = ctx;
    wi->module_obj = JS_UNDEFINED;
    wi->exports = JS_UNDEFINED;
    wi->memory_obj = JS_UNDEFINED;
    wi->pending_exc = JS_UNDEFINED;
    wi->linkage = linkage;

    g_private_set(&ns_wasm_instantiating, wi);
    wasm_module_inst_t inst =
        wasm_runtime_instantiate(inst_module, NS_WASM_STACK_SIZE, 0, error_buf,
                                 sizeof(error_buf));
    g_private_set(&ns_wasm_instantiating, NULL);
    if (!inst) {
        JSValue exc = JS_UNDEFINED;
        if (wi->has_pending) {
            exc = wi->pending_exc;
            wi->pending_exc = JS_UNDEFINED;
            wi->has_pending = FALSE;
        }
        ns_wasm_linkage_clear(rt, &wi->linkage);
        JS_FreeValueRT(rt, wi->pending_exc);
        g_free(wi);
        if (!JS_IsUndefined(exc))
            return JS_Throw(ctx, exc);
        return ns_wasm_throw_named(ctx, "LinkError", error_buf);
    }

    wasm_exec_env_t exec_env =
        wasm_runtime_create_exec_env(inst, NS_WASM_STACK_SIZE);
    if (!exec_env) {
        wasm_runtime_deinstantiate(inst);
        ns_wasm_linkage_clear(rt, &wi->linkage);
        JS_FreeValueRT(rt, wi->pending_exc);
        g_free(wi);
        return JS_ThrowInternalError(ctx, "wasm exec env creation failed");
    }

    JSValue obj = JS_NewObjectClass(ctx, ns_wasm_instance_class_id);
    if (JS_IsException(obj)) {
        wasm_runtime_destroy_exec_env(exec_env);
        wasm_runtime_deinstantiate(inst);
        ns_wasm_linkage_clear(rt, &wi->linkage);
        JS_FreeValueRT(rt, wi->pending_exc);
        g_free(wi);
        return obj;
    }

    wi->inst = inst;
    wi->exec_env = exec_env;
    wi->module_obj = JS_DupValue(ctx, module_obj);
    JS_SetOpaque(obj, wi);
    wasm_runtime_set_custom_data(inst, wi);

    ns_wasm_adopt_provided_imports(ctx, obj, wi);

    wi->exports = ns_wasm_build_exports(ctx, obj, wi, mod);
    if (JS_IsException(wi->exports)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, obj, "exports",
                                  JS_DupValue(ctx, wi->exports),
                                  JS_PROP_ENUMERABLE) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue
ns_wasm_module_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                    JSValueConst *argv)
{
    (void)new_target;
    size_t len = 0;
    guint8 *bytes =
        argc > 0 ? ns_wasm_copy_buffer_source(ctx, argv[0], &len) : NULL;
    if (!bytes)
        return ns_wasm_buffer_source_error(ctx, "WebAssembly.Module requires a "
                                                "BufferSource");
    JSValue obj = ns_wasm_module_from_bytes(ctx, bytes, len);
    g_free(bytes);
    return obj;
}

static JSValue
ns_wasm_instance_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                      JSValueConst *argv)
{
    (void)new_target;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "WebAssembly.Instance requires a "
                                      "Module");
    return ns_wasm_instance_create(ctx, argv[0],
                                   argc > 1 ? argv[1] : JS_UNDEFINED);
}

static JSValue
ns_wasm_global_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                    JSValueConst *argv)
{
    (void)new_target;
    JSValueConst desc = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (!JS_IsObject(desc))
        return JS_ThrowTypeError(ctx, "wasm global descriptor required");
    JSValue tv = JS_GetPropertyStr(ctx, desc, "value");
    const char *type = JS_ToCString(ctx, tv);
    JS_FreeValue(ctx, tv);
    if (!type)
        return JS_EXCEPTION;
    wasm_valkind_t kind;
    gboolean is_ref = FALSE;
    if (strcmp(type, "i32") == 0) {
        kind = WASM_I32;
    } else if (strcmp(type, "i64") == 0) {
        kind = WASM_I64;
    } else if (strcmp(type, "f32") == 0) {
        kind = WASM_F32;
    } else if (strcmp(type, "f64") == 0) {
        kind = WASM_F64;
    } else if (strcmp(type, "externref") == 0) {
        kind = WASM_EXTERNREF;
        is_ref = TRUE;
    } else if (strcmp(type, "anyfunc") == 0 || strcmp(type, "funcref") == 0) {
        kind = WASM_FUNCREF;
        is_ref = TRUE;
    } else {
        JS_FreeCString(ctx, type);
        return JS_ThrowTypeError(ctx, "unsupported wasm global type");
    }
    JS_FreeCString(ctx, type);
    JSValue mv = JS_GetPropertyStr(ctx, desc, "mutable");
    int is_mutable = JS_ToBool(ctx, mv);
    JS_FreeValue(ctx, mv);
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    wasm_val_t val;
    memset(&val, 0, sizeof(val));
    val.kind = kind;
    if (!is_ref && ns_wasm_global_convert(ctx, kind, init, &val))
        return JS_EXCEPTION;
    if (kind == WASM_FUNCREF && !JS_IsUndefined(init) && !JS_IsNull(init) &&
        !JS_GetOpaque(init, ns_wasm_func_class_id))
        return JS_ThrowTypeError(ctx, "funcref global requires an exported "
                                      "wasm function or null");
    JSValue obj = JS_NewObjectClass(ctx, ns_wasm_global_class_id);
    if (JS_IsException(obj))
        return obj;
    ns_wasm_global *g = g_new0(ns_wasm_global, 1);
    g->ctx = ctx;
    g->instance = JS_UNDEFINED;
    if (kind == WASM_FUNCREF && JS_IsUndefined(init))
        g->ref_value = JS_NULL;
    else
        g->ref_value = is_ref ? JS_DupValue(ctx, init) : JS_UNDEFINED;
    g->val = val;
    g->kind = kind;
    g->standalone = TRUE;
    g->is_mutable = is_mutable != 0;
    JS_SetOpaque(obj, g);
    return obj;
}

static JSValue
ns_wasm_memory_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                    JSValueConst *argv)
{
    (void)new_target;
    JSValueConst desc = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (!JS_IsObject(desc))
        return JS_ThrowTypeError(ctx, "wasm memory descriptor required");
    JSValue iv = JS_GetPropertyStr(ctx, desc, "initial");
    if (JS_IsUndefined(iv)) {
        JS_FreeValue(ctx, iv);
        iv = JS_GetPropertyStr(ctx, desc, "minimum");
    }
    uint32_t initial = 0;
    int rc = ns_wasm_to_u32_index(ctx, iv, &initial, "wasm memory initial");
    JS_FreeValue(ctx, iv);
    if (rc)
        return JS_EXCEPTION;
    JSValue maxv = JS_GetPropertyStr(ctx, desc, "maximum");
    uint32_t maximum = 0;
    if (!JS_IsUndefined(maxv))
        rc = ns_wasm_to_u32_index(ctx, maxv, &maximum, "wasm memory maximum");
    JS_FreeValue(ctx, maxv);
    if (rc)
        return JS_EXCEPTION;
    if (maximum && maximum < initial)
        return JS_ThrowRangeError(ctx, "wasm memory maximum is less than initial");
    JSValue sv = JS_GetPropertyStr(ctx, desc, "shared");
    int shared = JS_ToBool(ctx, sv);
    JS_FreeValue(ctx, sv);
    if (shared)
        return JS_ThrowTypeError(ctx, "shared wasm memory is not supported");
    guint64 bytes = (guint64)initial * NS_WASM_PAGE_SIZE;
    if (bytes > NS_WASM_MEMORY_MAX_BYTES)
        return JS_ThrowRangeError(ctx, "wasm memory initial is too large");
    JSValue obj = JS_NewObjectClass(ctx, ns_wasm_memory_class_id);
    if (JS_IsException(obj))
        return obj;
    ns_wasm_memory *m = g_new0(ns_wasm_memory, 1);
    m->ctx = ctx;
    m->instance = JS_UNDEFINED;
    m->buffer = JS_UNDEFINED;
    m->staging = g_malloc0(bytes ? (size_t)bytes : 1);
    m->staging_size = (size_t)bytes;
    m->max_pages = maximum;
    JS_SetOpaque(obj, m);
    return obj;
}

static JSValue
ns_wasm_table_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                   JSValueConst *argv)
{
    (void)new_target;
    JSValueConst desc = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (!JS_IsObject(desc))
        return JS_ThrowTypeError(ctx, "wasm table descriptor required");
    JSValue ev = JS_GetPropertyStr(ctx, desc, "element");
    const char *elem = JS_ToCString(ctx, ev);
    wasm_valkind_t elem_kind =
        elem && strcmp(elem, "externref") == 0 ? WASM_EXTERNREF : WASM_FUNCREF;
    if (elem)
        JS_FreeCString(ctx, elem);
    JS_FreeValue(ctx, ev);
    JSValue obj = JS_NewObjectClass(ctx, ns_wasm_table_class_id);
    if (JS_IsException(obj))
        return obj;
    ns_wasm_table *t = g_new0(ns_wasm_table, 1);
    t->ctx = ctx;
    t->instance = JS_UNDEFINED;
    t->elem_kind = elem_kind;
    JS_SetOpaque(obj, t);
    return obj;
}

static JSValue
ns_wasm_resolved_promise(JSContext *ctx, JSValue value)
{
    JSValue resolvers[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolvers);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, value);
        return promise;
    }
    if (JS_IsException(value)) {
        JSValue exc = JS_GetException(ctx);
        JSValue r = JS_Call(ctx, resolvers[1], JS_UNDEFINED, 1,
                            (JSValueConst *)&exc);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, exc);
    } else {
        JSValue r = JS_Call(ctx, resolvers[0], JS_UNDEFINED, 1,
                            (JSValueConst *)&value);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, value);
    }
    JS_FreeValue(ctx, resolvers[0]);
    JS_FreeValue(ctx, resolvers[1]);
    return promise;
}

static JSValue
ns_wasm_compile(JSContext *ctx, JSValueConst this_val, int argc,
                JSValueConst *argv)
{
    (void)this_val;
    size_t len = 0;
    guint8 *bytes =
        argc > 0 ? ns_wasm_copy_buffer_source(ctx, argv[0], &len) : NULL;
    if (!bytes)
        return ns_wasm_resolved_promise(
            ctx, ns_wasm_buffer_source_error(ctx,
                                             "WebAssembly.compile requires a "
                                             "BufferSource"));
    JSValue module = ns_wasm_module_from_bytes(ctx, bytes, len);
    g_free(bytes);
    return ns_wasm_resolved_promise(ctx, module);
}

static JSValue
ns_wasm_instantiate(JSContext *ctx, JSValueConst this_val, int argc,
                    JSValueConst *argv)
{
    (void)this_val;
    JSValueConst source = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst imports = argc > 1 ? argv[1] : JS_UNDEFINED;

    if (JS_GetOpaque(source, ns_wasm_module_class_id))
        return ns_wasm_resolved_promise(
            ctx, ns_wasm_instance_create(ctx, source, imports));

    size_t len = 0;
    guint8 *bytes = ns_wasm_copy_buffer_source(ctx, source, &len);
    if (!bytes)
        return ns_wasm_resolved_promise(
            ctx, ns_wasm_buffer_source_error(
                     ctx, "WebAssembly.instantiate requires a BufferSource or "
                          "Module"));
    JSValue module = ns_wasm_module_from_bytes(ctx, bytes, len);
    g_free(bytes);
    if (JS_IsException(module))
        return ns_wasm_resolved_promise(ctx, module);

    JSValue instance = ns_wasm_instance_create(ctx, module, imports);
    if (JS_IsException(instance)) {
        JS_FreeValue(ctx, module);
        return ns_wasm_resolved_promise(ctx, instance);
    }
    JSValue pair = JS_NewObject(ctx);
    if (JS_IsException(pair)) {
        JS_FreeValue(ctx, module);
        JS_FreeValue(ctx, instance);
        return ns_wasm_resolved_promise(ctx, pair);
    }
    if (JS_SetPropertyStr(ctx, pair, "module", module) < 0) {
        JS_FreeValue(ctx, pair);
        JS_FreeValue(ctx, instance);
        return ns_wasm_resolved_promise(ctx, JS_EXCEPTION);
    }
    if (JS_SetPropertyStr(ctx, pair, "instance", instance) < 0) {
        JS_FreeValue(ctx, pair);
        return ns_wasm_resolved_promise(ctx, JS_EXCEPTION);
    }
    return ns_wasm_resolved_promise(ctx, pair);
}

static JSValue
ns_wasm_validate(JSContext *ctx, JSValueConst this_val, int argc,
                 JSValueConst *argv)
{
    (void)this_val;
    if (!ns_wasm_runtime_ready())
        return JS_NewBool(ctx, FALSE);
    size_t len = 0;
    guint8 *bytes =
        argc > 0 ? ns_wasm_copy_buffer_source(ctx, argv[0], &len) : NULL;
    if (!bytes)
        return ns_wasm_buffer_source_error(ctx,
                                           "WebAssembly.validate requires a "
                                           "BufferSource");
    char error_buf[128];
    LoadArgs load_args;
    memset(&load_args, 0, sizeof(load_args));
    load_args.name = (char *)"";
    load_args.no_resolve = true;
    wasm_module_t module = wasm_runtime_load_ex(bytes, (uint32_t)len,
                                                &load_args, error_buf,
                                                sizeof(error_buf));
    gboolean ok = module != NULL;
    if (module)
        wasm_runtime_unload(module);
    g_free(bytes);
    return JS_NewBool(ctx, ok);
}

static const char *
ns_wasm_kind_name(uint8_t kind)
{
    switch (kind) {
    case WASM_IMPORT_EXPORT_KIND_FUNC:
        return "function";
    case WASM_IMPORT_EXPORT_KIND_TABLE:
        return "table";
    case WASM_IMPORT_EXPORT_KIND_MEMORY:
        return "memory";
    case WASM_IMPORT_EXPORT_KIND_GLOBAL:
        return "global";
    default:
        return "";
    }
}

static JSValue
ns_wasm_module_exports_static(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    (void)this_val;
    ns_wasm_module *mod =
        argc > 0 ? JS_GetOpaque2(ctx, argv[0], ns_wasm_module_class_id) : NULL;
    if (!mod || !mod->module)
        return JS_ThrowTypeError(ctx,
                                 "WebAssembly.Module.exports requires a Module");
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    int32_t n = wasm_runtime_get_export_count(mod->module);
    for (int32_t i = 0; i < n; i++) {
        wasm_export_t exp;
        wasm_runtime_get_export_type(mod->module, i, &exp);
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, exp.name));
        JS_SetPropertyStr(ctx, o, "kind",
                          JS_NewString(ctx, ns_wasm_kind_name(exp.kind)));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
    }
    return arr;
}

static JSValue
ns_wasm_module_imports_static(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    (void)this_val;
    ns_wasm_module *mod =
        argc > 0 ? JS_GetOpaque2(ctx, argv[0], ns_wasm_module_class_id) : NULL;
    if (!mod || !mod->module)
        return JS_ThrowTypeError(ctx,
                                 "WebAssembly.Module.imports requires a Module");
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    int32_t n = wasm_runtime_get_import_count(mod->module);
    for (int32_t i = 0; i < n; i++) {
        wasm_import_t imp;
        wasm_runtime_get_import_type(mod->module, i, &imp);
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "module",
                          JS_NewString(ctx, imp.module_name));
        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, imp.name));
        JS_SetPropertyStr(ctx, o, "kind",
                          JS_NewString(ctx, ns_wasm_kind_name(imp.kind)));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
    }
    return arr;
}

static const char ns_wasm_bootstrap_js[] =
    "(() => {"
    "  const W = WebAssembly;"
    "  W.CompileError = class CompileError extends Error {};"
    "  W.CompileError.prototype.name = 'CompileError';"
    "  W.LinkError = class LinkError extends Error {};"
    "  W.LinkError.prototype.name = 'LinkError';"
    "  W.RuntimeError = class RuntimeError extends Error {};"
    "  W.RuntimeError.prototype.name = 'RuntimeError';"
    "  W.instantiateStreaming = async (src, imports) =>"
    "    W.instantiate(await (await src).arrayBuffer(), imports);"
    "  W.compileStreaming = async (src) =>"
    "    W.compile(await (await src).arrayBuffer());"
    "})();";

static void
ns_wasm_register_class(JSRuntime *rt, JSClassID *class_id,
                       const JSClassDef *def)
{
    ns_new_class_id(class_id);
    if (!JS_IsRegisteredClass(rt, *class_id))
        JS_NewClass(rt, *class_id, def);
}

void
ns_wasm_install(JSContext *ctx, JSValueConst global)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    ns_wamr_set_host_calls_ctors(true);
    ns_wasm_register_class(rt, &ns_wasm_module_class_id, &ns_wasm_module_class);
    ns_wasm_register_class(rt, &ns_wasm_instance_class_id,
                           &ns_wasm_instance_class);
    ns_wasm_register_class(rt, &ns_wasm_memory_class_id, &ns_wasm_memory_class);
    ns_wasm_register_class(rt, &ns_wasm_table_class_id, &ns_wasm_table_class);
    ns_wasm_register_class(rt, &ns_wasm_global_class_id, &ns_wasm_global_class);
    ns_wasm_register_class(rt, &ns_wasm_func_class_id, &ns_wasm_func_class);

    JSValue memory_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, memory_proto, ns_wasm_memory_proto_funcs,
                               G_N_ELEMENTS(ns_wasm_memory_proto_funcs));
    JS_SetClassProto(ctx, ns_wasm_memory_class_id, memory_proto);

    JSValue table_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, table_proto, ns_wasm_table_proto_funcs,
                               G_N_ELEMENTS(ns_wasm_table_proto_funcs));
    JS_SetClassProto(ctx, ns_wasm_table_class_id, table_proto);

    JSValue global_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, global_proto, ns_wasm_global_proto_funcs,
                               G_N_ELEMENTS(ns_wasm_global_proto_funcs));
    JS_SetClassProto(ctx, ns_wasm_global_class_id, global_proto);

    JSValue module_proto = JS_NewObject(ctx);
    JS_SetClassProto(ctx, ns_wasm_module_class_id,
                     JS_DupValue(ctx, module_proto));
    JSValue instance_proto = JS_NewObject(ctx);
    JS_SetClassProto(ctx, ns_wasm_instance_class_id,
                     JS_DupValue(ctx, instance_proto));

    JSValue function_ctor = JS_GetPropertyStr(ctx, global, "Function");
    JSValue function_proto = JS_GetPropertyStr(ctx, function_ctor,
                                               "prototype");
    JS_FreeValue(ctx, function_ctor);
    JS_SetClassProto(ctx, ns_wasm_func_class_id, function_proto);

    JSValue ns = JS_NewObject(ctx);
    JSValue module_ctor = JS_NewCFunction2(ctx, ns_wasm_module_ctor, "Module",
                                           1, JS_CFUNC_constructor, 0);
    JSValue instance_ctor = JS_NewCFunction2(ctx, ns_wasm_instance_ctor,
                                             "Instance", 2,
                                             JS_CFUNC_constructor, 0);
    JSValue memory_ctor = JS_NewCFunction2(ctx, ns_wasm_memory_ctor,
                                           "Memory", 1, JS_CFUNC_constructor,
                                           0);
    JSValue table_ctor = JS_NewCFunction2(ctx, ns_wasm_table_ctor,
                                          "Table", 1, JS_CFUNC_constructor, 0);
    JSValue global_ctor = JS_NewCFunction2(ctx, ns_wasm_global_ctor,
                                           "Global", 1, JS_CFUNC_constructor,
                                           0);
    JS_SetPropertyStr(ctx, memory_ctor, "prototype",
                      JS_DupValue(ctx, memory_proto));
    JS_SetPropertyStr(ctx, table_ctor, "prototype",
                      JS_DupValue(ctx, table_proto));
    JS_SetPropertyStr(ctx, global_ctor, "prototype",
                      JS_DupValue(ctx, global_proto));
    JS_SetPropertyStr(ctx, module_ctor, "exports",
                      JS_NewCFunction(ctx, ns_wasm_module_exports_static,
                                      "exports", 1));
    JS_SetPropertyStr(ctx, module_ctor, "imports",
                      JS_NewCFunction(ctx, ns_wasm_module_imports_static,
                                      "imports", 1));
    JS_SetPropertyStr(ctx, module_ctor, "prototype", module_proto);
    JS_SetPropertyStr(ctx, instance_ctor, "prototype", instance_proto);
    JS_SetPropertyStr(ctx, ns, "Module", module_ctor);
    JS_SetPropertyStr(ctx, ns, "Instance", instance_ctor);
    JS_SetPropertyStr(ctx, ns, "Memory", memory_ctor);
    JS_SetPropertyStr(ctx, ns, "Table", table_ctor);
    JS_SetPropertyStr(ctx, ns, "Global", global_ctor);
    JSValue compile_fn = JS_NewCFunction(ctx, ns_wasm_compile, "compile", 1);
    JS_SetPropertyStr(ctx, ns, "compile", compile_fn);
    JSValue inst_fn = JS_NewCFunction(ctx, ns_wasm_instantiate, "instantiate",
                                      2);
    JS_SetPropertyStr(ctx, ns, "instantiate", inst_fn);
    JSValue validate_fn = JS_NewCFunction(ctx, ns_wasm_validate, "validate",
                                          1);
    JS_SetPropertyStr(ctx, ns, "validate", validate_fn);
    JS_SetPropertyStr(ctx, global, "WebAssembly", ns);

    JSValue boot = JS_Eval(ctx, ns_wasm_bootstrap_js,
                           sizeof(ns_wasm_bootstrap_js) - 1,
                           "<wasm-bootstrap>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(boot))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, boot);
}

#endif /* NS_HAVE_WAMR */
