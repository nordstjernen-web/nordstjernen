/* Nordstjernen — Performance API: performance.*, PerformanceObserver (QuickJS).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "js_internal.h"

#include <math.h>
#include <string.h>

#define NS_PERF_ENTRY_CAP   256

typedef struct ns_perf_entry {
    char  *name;
    char  *type;
    char  *initiator_type;
    double start_time;
    double duration;
    gint64 transfer_size;
} ns_perf_entry;

void
ns_perf_entry_free(gpointer p)
{
    ns_perf_entry *e = p;
    if (!e) return;
    g_free(e->name);
    g_free(e->type);
    g_free(e->initiator_type);
    g_free(e);
}

static ns_perf_entry *
ns_perf_entry_clone(const ns_perf_entry *e)
{
    if (!e) return NULL;
    ns_perf_entry *copy = g_new0(ns_perf_entry, 1);
    copy->name       = g_strdup(e->name ? e->name : "");
    copy->type       = g_strdup(e->type ? e->type : "");
    copy->initiator_type = g_strdup(e->initiator_type ? e->initiator_type : "");
    copy->start_time = e->start_time;
    copy->duration   = e->duration;
    copy->transfer_size = e->transfer_size;
    return copy;
}

#define NS_TIMER_RESOLUTION_US 100

double
ns_perf_clamp_ms(gint64 delta_us)
{
    if (delta_us < 0) delta_us = 0;
    delta_us = (delta_us / NS_TIMER_RESOLUTION_US) * NS_TIMER_RESOLUTION_US;
    return (double)delta_us / 1000.0;
}

double
ns_perf_relative_ms(gint64 now_us, gint64 origin_us)
{
    now_us = (now_us / NS_TIMER_RESOLUTION_US) * NS_TIMER_RESOLUTION_US;
    origin_us = (origin_us / NS_TIMER_RESOLUTION_US) * NS_TIMER_RESOLUTION_US;
    if (now_us < origin_us) return 0;
    return (double)(now_us - origin_us) / 1000.0;
}

double
ns_perf_now_ms(const ns_js *js)
{
    gint64 origin = js ? js->time_origin_us : 0;
    return ns_perf_relative_ms(g_get_monotonic_time(), origin);
}

JSValue
ns_window_performance_now(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, ns_perf_now_ms(js_from_ctx(ctx)));
}

static JSValue
ns_perf_entry_to_js(JSContext *ctx, const ns_perf_entry *e)
{
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, e->name ? e->name : ""));
    JS_SetPropertyStr(ctx, o, "entryType",
                      JS_NewString(ctx, e->type ? e->type : ""));
    JS_SetPropertyStr(ctx, o, "startTime", JS_NewFloat64(ctx, e->start_time));
    JS_SetPropertyStr(ctx, o, "duration",  JS_NewFloat64(ctx, e->duration));
    if (e->type && strcmp(e->type, "resource") == 0) {
        double end = e->start_time + e->duration;
        JS_SetPropertyStr(ctx, o, "initiatorType",
                          JS_NewString(ctx, e->initiator_type
                                            ? e->initiator_type : "other"));
        JS_SetPropertyStr(ctx, o, "nextHopProtocol", JS_NewString(ctx, "h2"));
        JS_SetPropertyStr(ctx, o, "workerStart", JS_NewFloat64(ctx, 0));
        JS_SetPropertyStr(ctx, o, "redirectStart", JS_NewFloat64(ctx, 0));
        JS_SetPropertyStr(ctx, o, "redirectEnd", JS_NewFloat64(ctx, 0));
        JS_SetPropertyStr(ctx, o, "fetchStart",
                          JS_NewFloat64(ctx, e->start_time));
        JS_SetPropertyStr(ctx, o, "domainLookupStart",
                          JS_NewFloat64(ctx, e->start_time));
        JS_SetPropertyStr(ctx, o, "domainLookupEnd",
                          JS_NewFloat64(ctx, e->start_time));
        JS_SetPropertyStr(ctx, o, "connectStart",
                          JS_NewFloat64(ctx, e->start_time));
        JS_SetPropertyStr(ctx, o, "connectEnd",
                          JS_NewFloat64(ctx, e->start_time));
        JS_SetPropertyStr(ctx, o, "secureConnectionStart",
                          JS_NewFloat64(ctx, e->start_time));
        JS_SetPropertyStr(ctx, o, "requestStart",
                          JS_NewFloat64(ctx, e->start_time));
        JS_SetPropertyStr(ctx, o, "responseStart", JS_NewFloat64(ctx, end));
        JS_SetPropertyStr(ctx, o, "responseEnd", JS_NewFloat64(ctx, end));
        JS_SetPropertyStr(ctx, o, "transferSize",
                          JS_NewInt64(ctx, e->transfer_size));
        JS_SetPropertyStr(ctx, o, "encodedBodySize",
                          JS_NewInt64(ctx, e->transfer_size));
        JS_SetPropertyStr(ctx, o, "decodedBodySize",
                          JS_NewInt64(ctx, e->transfer_size));
        JS_SetPropertyStr(ctx, o, "serverTiming", JS_NewArray(ctx));
        JS_SetPropertyStr(ctx, o, "responseStatus", JS_NewInt32(ctx, 200));
    }
    return o;
}

static JSValue
ns_perf_build_navigation_entry(JSContext *ctx, ns_js *js)
{
    JSValue o = JS_NewObject(ctx);
    const char *url = js && js->current_url ? js->current_url : "";
    const ns_js_navigation_timing *t = js ? &js->navigation_timing : NULL;
    double complete = t ? t->load_event_end_ms : 0;
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, url));
    JS_SetPropertyStr(ctx, o, "entryType", JS_NewString(ctx, "navigation"));
    JS_SetPropertyStr(ctx, o, "startTime", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, o, "duration", JS_NewFloat64(ctx, complete));
    JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, "navigate"));
    JS_SetPropertyStr(ctx, o, "initiatorType", JS_NewString(ctx, "navigation"));
    JS_SetPropertyStr(ctx, o, "nextHopProtocol", JS_NewString(ctx, "h2"));
    JS_SetPropertyStr(ctx, o, "redirectCount", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, o, "workerStart", JS_NewFloat64(ctx, 0));
    const struct { const char *k; double v; } f[] = {
        {"unloadEventStart",0},{"unloadEventEnd",0},{"redirectStart",0},
        {"redirectEnd",0},{"fetchStart",0},
        {"domainLookupStart",t ? t->domain_lookup_start_ms : 0},
        {"domainLookupEnd",t ? t->domain_lookup_end_ms : 0},
        {"connectStart",t ? t->connect_start_ms : 0},
        {"connectEnd",t ? t->connect_end_ms : 0},
        {"secureConnectionStart",t ? t->secure_connection_start_ms : 0},
        {"requestStart",t ? t->request_start_ms : 0},
        {"responseStart",t ? t->response_start_ms : 0},
        {"responseEnd",t ? t->response_end_ms : 0},
        {"domLoading",t ? t->dom_loading_ms : 0},
        {"domInteractive",t ? t->dom_interactive_ms : 0},
        {"domContentLoadedEventStart",
         t ? t->dom_content_loaded_event_start_ms : 0},
        {"domContentLoadedEventEnd",
         t ? t->dom_content_loaded_event_end_ms : 0},
        {"domComplete",t ? t->dom_complete_ms : 0},
        {"loadEventStart",t ? t->load_event_start_ms : 0},
        {"loadEventEnd",complete},
    };
    for (gsize i = 0; i < G_N_ELEMENTS(f); i++)
        JS_SetPropertyStr(ctx, o, f[i].k, JS_NewFloat64(ctx, f[i].v));
    JS_SetPropertyStr(ctx, o, "transferSize", JS_NewInt64(ctx, 0));
    JS_SetPropertyStr(ctx, o, "encodedBodySize", JS_NewInt64(ctx, 0));
    JS_SetPropertyStr(ctx, o, "decodedBodySize", JS_NewInt64(ctx, 0));
    JS_SetPropertyStr(ctx, o, "serverTiming", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, o, "responseStatus", JS_NewInt32(ctx, 200));
    return o;
}

static JSValue
ns_perf_build_paint_entry(JSContext *ctx, const char *name, double start)
{
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, o, "entryType", JS_NewString(ctx, "paint"));
    JS_SetPropertyStr(ctx, o, "startTime", JS_NewFloat64(ctx, start));
    JS_SetPropertyStr(ctx, o, "duration", JS_NewFloat64(ctx, 0));
    return o;
}

static void ns_perf_observer_queue(ns_js *js, const ns_perf_entry *entry);

void
ns_perf_add_resource(ns_js *js, const char *url, const char *initiator,
                     double start_ms, double duration_ms, gint64 size)
{
    if (!js || !js->perf_entries || !url) return;
    if (g_str_has_prefix(url, "data:") || g_str_has_prefix(url, "blob:") ||
        g_str_has_prefix(url, "about:"))
        return;
    if (js->perf_entries->len >= NS_PERF_ENTRY_CAP)
        g_ptr_array_remove_index(js->perf_entries, 0);
    ns_perf_entry *e = g_new0(ns_perf_entry, 1);
    e->name = g_strdup(url);
    e->type = g_strdup("resource");
    e->initiator_type = g_strdup(initiator ? initiator : "other");
    e->start_time = start_ms;
    e->duration = duration_ms >= 0 ? duration_ms : 0;
    e->transfer_size = size > 0 ? size : 0;
    g_ptr_array_add(js->perf_entries, e);
    ns_perf_observer_queue(js, e);
}

static JSClassID ns_perf_observer_class_id;

static JSValue
ns_perf_records_to_array(JSContext *ctx, GPtrArray *records)
{
    JSValue arr = JS_NewArray(ctx);
    if (!records) return arr;
    for (guint i = 0; i < records->len; i++) {
        const ns_perf_entry *e = g_ptr_array_index(records, i);
        if (e) JS_SetPropertyUint32(ctx, arr, i, ns_perf_entry_to_js(ctx, e));
    }
    return arr;
}

static JSValue
ns_perf_entry_list_getEntries(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue entries = JS_GetPropertyStr(ctx, this_val, "_entries");
    JSValue out = JS_NewArray(ctx);
    if (!JS_IsObject(entries)) {
        JS_FreeValue(ctx, entries);
        return out;
    }
    uint32_t len = ns_js_array_length(ctx, entries);
    for (uint32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, entries, i);
        if (!JS_IsException(v)) JS_SetPropertyUint32(ctx, out, i, v);
    }
    JS_FreeValue(ctx, entries);
    return out;
}

static gboolean
ns_perf_js_entry_matches(JSContext *ctx, JSValueConst entry,
                         const char *name, const char *type)
{
    if (name) {
        JSValue v = JS_GetPropertyStr(ctx, entry, "name");
        const char *s = JS_ToCString(ctx, v);
        gboolean ok = s && strcmp(s, name) == 0;
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, v);
        if (!ok) return FALSE;
    }
    if (type) {
        JSValue v = JS_GetPropertyStr(ctx, entry, "entryType");
        const char *s = JS_ToCString(ctx, v);
        gboolean ok = s && strcmp(s, type) == 0;
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, v);
        if (!ok) return FALSE;
    }
    return TRUE;
}

static JSValue
ns_perf_entry_list_getEntriesByName(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    const char *name = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    const char *type = argc > 1 && JS_IsString(argv[1])
                         ? JS_ToCString(ctx, argv[1]) : NULL;
    JSValue entries = JS_GetPropertyStr(ctx, this_val, "_entries");
    JSValue out = JS_NewArray(ctx);
    if (!name || !JS_IsObject(entries)) {
        if (name) JS_FreeCString(ctx, name);
        if (type) JS_FreeCString(ctx, type);
        JS_FreeValue(ctx, entries);
        return out;
    }
    uint32_t len = ns_js_array_length(ctx, entries);
    uint32_t oi = 0;
    for (uint32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, entries, i);
        if (JS_IsException(v)) continue;
        if (ns_perf_js_entry_matches(ctx, v, name, type))
            JS_SetPropertyUint32(ctx, out, oi++, v);
        else
            JS_FreeValue(ctx, v);
    }
    JS_FreeCString(ctx, name);
    if (type) JS_FreeCString(ctx, type);
    JS_FreeValue(ctx, entries);
    return out;
}

static JSValue
ns_perf_entry_list_getEntriesByType(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    const char *type = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    JSValue entries = JS_GetPropertyStr(ctx, this_val, "_entries");
    JSValue out = JS_NewArray(ctx);
    if (!type || !JS_IsObject(entries)) {
        if (type) JS_FreeCString(ctx, type);
        JS_FreeValue(ctx, entries);
        return out;
    }
    uint32_t len = ns_js_array_length(ctx, entries);
    uint32_t oi = 0;
    for (uint32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, entries, i);
        if (JS_IsException(v)) continue;
        if (ns_perf_js_entry_matches(ctx, v, NULL, type))
            JS_SetPropertyUint32(ctx, out, oi++, v);
        else
            JS_FreeValue(ctx, v);
    }
    JS_FreeCString(ctx, type);
    JS_FreeValue(ctx, entries);
    return out;
}

static JSValue
ns_perf_entry_list_from_array(JSContext *ctx, JSValueConst entries)
{
    JSValue list = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, list, "_entries", JS_DupValue(ctx, entries));
    ns_bind_fn(ctx, list, "getEntries",       ns_perf_entry_list_getEntries,       0);
    ns_bind_fn(ctx, list, "getEntriesByName", ns_perf_entry_list_getEntriesByName, 2);
    ns_bind_fn(ctx, list, "getEntriesByType", ns_perf_entry_list_getEntriesByType, 1);
    return list;
}

static void
ns_perf_observer_free(ns_js *js, ns_perf_observer *o)
{
    if (!o) return;
    if (js && js->ctx) JS_FreeValue(js->ctx, o->cb);
    if (o->entry_types) g_ptr_array_free(o->entry_types, TRUE);
    if (o->records) g_ptr_array_free(o->records, TRUE);
    g_free(o);
}

static void
ns_perf_observer_finalizer(JSRuntime *rt, JSValue val)
{
    ns_perf_observer *o = JS_GetOpaque(val, ns_perf_observer_class_id);
    if (!o) return;
    ns_js *js = JS_GetRuntimeOpaque(rt);
    if (js && js->perf_observers)
        g_ptr_array_remove_fast(js->perf_observers, o);
    ns_perf_observer_free(js, o);
}

static JSClassDef ns_perf_observer_class = {
    "PerformanceObserver",
    .finalizer = ns_perf_observer_finalizer,
};

static ns_perf_observer *
ns_unwrap_perf_observer(JSValueConst v)
{
    return JS_GetOpaque(v, ns_perf_observer_class_id);
}

static gboolean
ns_perf_observer_wants(const ns_perf_observer *o, const char *type)
{
    if (!o || !o->entry_types || !type) return FALSE;
    for (guint i = 0; i < o->entry_types->len; i++) {
        const char *want = g_ptr_array_index(o->entry_types, i);
        if (want && strcmp(want, type) == 0) return TRUE;
    }
    return FALSE;
}

static void
ns_perf_observer_add_type(ns_perf_observer *o, const char *type)
{
    if (!o || !o->entry_types || !type || !*type) return;
    if (ns_perf_observer_wants(o, type)) return;
    g_ptr_array_add(o->entry_types, g_strdup(type));
}

static void
ns_perf_schedule_drain(ns_js *js);

static void
ns_perf_observer_queue(ns_js *js, const ns_perf_entry *entry)
{
    if (!js || !js->ctx || !js->perf_observers || !entry || !entry->type) return;
    gboolean queued = FALSE;
    for (guint i = 0; i < js->perf_observers->len; i++) {
        ns_perf_observer *o = g_ptr_array_index(js->perf_observers, i);
        if (!o || o->disconnected || !JS_IsFunction(js->ctx, o->cb)) continue;
        if (!ns_perf_observer_wants(o, entry->type)) continue;
        if (!o->records)
            o->records = g_ptr_array_new_with_free_func(ns_perf_entry_free);
        if (o->records->len >= NS_PERF_ENTRY_CAP)
            g_ptr_array_remove_index(o->records, 0);
        g_ptr_array_add(o->records, ns_perf_entry_clone(entry));
        if (!o->pinned) {
            JS_DupValue(js->ctx, o->wrapper);
            o->pinned = TRUE;
        }
        queued = TRUE;
    }
    if (queued) ns_perf_schedule_drain(js);
}

static JSValue
ns_perf_drain_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_js *js = js_from_ctx(ctx);
    if (!js || !js->perf_observers) return JS_UNDEFINED;
    js->perf_drain_scheduled = FALSE;
    for (guint i = 0; i < js->perf_observers->len; i++) {
        ns_perf_observer *o = g_ptr_array_index(js->perf_observers, i);
        if (!o || o->disconnected || !o->records || o->records->len == 0)
            continue;
        if (!JS_IsFunction(ctx, o->cb)) continue;
        GPtrArray *records = o->records;
        o->records = g_ptr_array_new_with_free_func(ns_perf_entry_free);
        JSValue arr = ns_perf_records_to_array(ctx, records);
        JSValue list = ns_perf_entry_list_from_array(ctx, arr);
        JSValueConst call_args[2] = { list, JS_DupValue(ctx, o->wrapper) };
        JSValue ret = JS_Call(ctx, o->cb, o->wrapper, 2, call_args);
        if (JS_IsException(ret)) {
            JSValue ex = JS_GetException(ctx);
            if (js->log_cb) {
                const char *msg = JS_ToCString(ctx, ex);
                if (msg) {
                    char *line = g_strdup_printf("JS error in PerformanceObserver: %s", msg);
                    js->log_cb(line, js->log_user_data);
                    g_free(line);
                    JS_FreeCString(ctx, msg);
                }
            }
            JS_FreeValue(ctx, ex);
        }
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, (JSValue)call_args[1]);
        JS_FreeValue(ctx, list);
        JS_FreeValue(ctx, arr);
        g_ptr_array_free(records, TRUE);
    }
    return JS_UNDEFINED;
}

static void
ns_perf_schedule_drain(ns_js *js)
{
    if (!js || !js->ctx || js->perf_drain_scheduled) return;
    js->perf_drain_scheduled = TRUE;
    JS_EnqueueJob(js->ctx, ns_perf_drain_job, 0, NULL);
}

static void
ns_perf_observer_collect_types(JSContext *ctx, ns_perf_observer *o,
                               JSValueConst options)
{
    JSValue type = JS_GetPropertyStr(ctx, options, "type");
    if (JS_IsString(type)) {
        const char *s = JS_ToCString(ctx, type);
        if (s) {
            ns_perf_observer_add_type(o, s);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, type);

    JSValue entry_types = JS_GetPropertyStr(ctx, options, "entryTypes");
    if (JS_IsObject(entry_types)) {
        uint32_t len = ns_js_array_length(ctx, entry_types);
        for (uint32_t i = 0; i < len; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, entry_types, i);
            const char *s = JS_ToCString(ctx, v);
            if (s) {
                ns_perf_observer_add_type(o, s);
                JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, v);
        }
    }
    JS_FreeValue(ctx, entry_types);
}

JSValue
ns_perf_observer_observe(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    ns_perf_observer *o = ns_unwrap_perf_observer(this_val);
    ns_js *js = js_from_ctx(ctx);
    if (!o || argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "PerformanceObserver.observe: options required");
    if (!o->entry_types)
        o->entry_types = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_set_size(o->entry_types, 0);
    ns_perf_observer_collect_types(ctx, o, argv[0]);
    if (o->entry_types->len == 0)
        return JS_ThrowTypeError(ctx, "PerformanceObserver.observe: type or entryTypes required");
    o->disconnected = FALSE;
    if (!o->pinned) {
        JS_DupValue(ctx, o->wrapper);
        o->pinned = TRUE;
    }
    if (js && js->perf_entries && ns_js_get_bool_prop(ctx, argv[0], "buffered", NULL)) {
        for (guint i = 0; i < js->perf_entries->len; i++) {
            const ns_perf_entry *e = g_ptr_array_index(js->perf_entries, i);
            if (!e || !ns_perf_observer_wants(o, e->type)) continue;
            if (!o->records)
                o->records = g_ptr_array_new_with_free_func(ns_perf_entry_free);
            if (o->records->len >= NS_PERF_ENTRY_CAP)
                g_ptr_array_remove_index(o->records, 0);
            g_ptr_array_add(o->records, ns_perf_entry_clone(e));
        }
        if (o->records && o->records->len > 0)
            ns_perf_schedule_drain(js);
    }
    return JS_UNDEFINED;
}

JSValue
ns_perf_observer_disconnect(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_perf_observer *o = ns_unwrap_perf_observer(this_val);
    if (!o) return JS_UNDEFINED;
    o->disconnected = TRUE;
    if (o->records) g_ptr_array_set_size(o->records, 0);
    if (o->entry_types) g_ptr_array_set_size(o->entry_types, 0);
    if (o->pinned) {
        o->pinned = FALSE;
        JS_FreeValue(ctx, o->wrapper);
    }
    return JS_UNDEFINED;
}

JSValue
ns_perf_observer_takeRecords(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_perf_observer *o = ns_unwrap_perf_observer(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!o || !o->records) return arr;
    GPtrArray *records = o->records;
    o->records = g_ptr_array_new_with_free_func(ns_perf_entry_free);
    JS_FreeValue(ctx, arr);
    arr = ns_perf_records_to_array(ctx, records);
    g_ptr_array_free(records, TRUE);
    return arr;
}

JSValue
ns_perf_observer_ctor(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    ns_js *js = js_from_ctx(ctx);
    if (!ns_perf_observer_class_id)
        JS_NewClassID(JS_GetRuntime(ctx), &ns_perf_observer_class_id);
    JS_NewClass(JS_GetRuntime(ctx), ns_perf_observer_class_id, &ns_perf_observer_class);
    JSValue proto = JS_GetPropertyStr(ctx, this_val, "prototype");
    JSValue obj;
    if (JS_IsObject(proto)) {
        obj = JS_NewObjectProtoClass(ctx, proto, ns_perf_observer_class_id);
        JSAtom obs_atom = JS_NewAtom(ctx, "observe");
        if (JS_HasProperty(ctx, proto, obs_atom) <= 0) {
            ns_bind_fn(ctx, proto, "observe",     ns_perf_observer_observe,     1);
            ns_bind_fn(ctx, proto, "disconnect",  ns_perf_observer_disconnect,  0);
            ns_bind_fn(ctx, proto, "takeRecords", ns_perf_observer_takeRecords, 0);
        }
        JS_FreeAtom(ctx, obs_atom);
    } else {
        obj = JS_NewObjectClass(ctx, ns_perf_observer_class_id);
        ns_bind_fn(ctx, obj, "observe",     ns_perf_observer_observe,     1);
        ns_bind_fn(ctx, obj, "disconnect",  ns_perf_observer_disconnect,  0);
        ns_bind_fn(ctx, obj, "takeRecords", ns_perf_observer_takeRecords, 0);
    }
    JS_FreeValue(ctx, proto);
    ns_perf_observer *o = g_new0(ns_perf_observer, 1);
    o->entry_types = g_ptr_array_new_with_free_func(g_free);
    o->records = g_ptr_array_new_with_free_func(ns_perf_entry_free);
    o->cb = (argc >= 1 && JS_IsFunction(ctx, argv[0]))
        ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    o->wrapper = obj;
    JS_SetOpaque(obj, o);
    ns_bind_fn_if_not_callable(ctx, obj, "observe",
                               ns_perf_observer_observe, 1);
    ns_bind_fn_if_not_callable(ctx, obj, "disconnect",
                               ns_perf_observer_disconnect, 0);
    ns_bind_fn_if_not_callable(ctx, obj, "takeRecords",
                               ns_perf_observer_takeRecords, 0);
    if (js) {
        if (!js->perf_observers)
            js->perf_observers = g_ptr_array_new();
        g_ptr_array_add(js->perf_observers, o);
    }
    return obj;
}

JSValue
ns_perf_supported_entry_types(JSContext *ctx)
{
    static const char *types[] = { "mark", "measure", "navigation", "resource", "paint" };
    JSValue arr = JS_NewArray(ctx);
    for (guint i = 0; i < G_N_ELEMENTS(types); i++)
        JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, types[i]));
    return arr;
}

static void
ns_perf_push(ns_js *js, const char *type, const char *name,
             double start_time, double duration)
{
    if (!js || !js->perf_entries) return;
    if (js->perf_entries->len >= NS_PERF_ENTRY_CAP)
        g_ptr_array_remove_index(js->perf_entries, 0);
    ns_perf_entry *e = g_new0(ns_perf_entry, 1);
    e->name       = g_strdup(name ? name : "");
    e->type       = g_strdup(type);
    e->start_time = start_time;
    e->duration   = duration;
    g_ptr_array_add(js->perf_entries, e);
    ns_perf_observer_queue(js, e);
}

JSValue
ns_window_performance_mark(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    ns_js *js = js_from_ctx(ctx);
    const char *name = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    double t = ns_perf_now_ms(js);
    ns_perf_push(js, "mark", name, t, 0.0);
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "name",
                      JS_NewString(ctx, name ? name : ""));
    JS_SetPropertyStr(ctx, r, "entryType", JS_NewString(ctx, "mark"));
    JS_SetPropertyStr(ctx, r, "startTime", JS_NewFloat64(ctx, t));
    JS_SetPropertyStr(ctx, r, "duration",  JS_NewFloat64(ctx, 0.0));
    if (name) JS_FreeCString(ctx, name);
    return r;
}

static gboolean
ns_perf_lookup_mark(const ns_js *js, const char *name, double *out_time)
{
    if (!js || !js->perf_entries || !name) return FALSE;
    for (guint i = js->perf_entries->len; i > 0; i--) {
        const ns_perf_entry *e = g_ptr_array_index(js->perf_entries, i - 1);
        if (e && e->type && !strcmp(e->type, "mark") &&
            e->name && !strcmp(e->name, name)) {
            if (out_time) *out_time = e->start_time;
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
ns_perf_resolve_time(JSContext *ctx, JSValueConst v, const ns_js *js,
                     double fallback, double *out)
{
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        *out = fallback;
        return TRUE;
    }
    if (JS_IsNumber(v)) {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, v) < 0) return FALSE;
        *out = d;
        return TRUE;
    }
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (!s) return FALSE;
        gboolean ok = ns_perf_lookup_mark(js, s, out);
        JS_FreeCString(ctx, s);
        if (!ok) *out = fallback;
        return TRUE;
    }
    *out = fallback;
    return TRUE;
}

JSValue
ns_window_performance_measure(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    ns_js *js = js_from_ctx(ctx);
    const char *name = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    double end_time = ns_perf_now_ms(js);
    double start_time = 0.0;
    double resolved_end = end_time;
    JSValue start_v = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue end_v   = argc > 2 ? argv[2] : JS_UNDEFINED;
    ns_perf_resolve_time(ctx, start_v, js, 0.0, &start_time);
    ns_perf_resolve_time(ctx, end_v,   js, end_time, &resolved_end);
    double duration = resolved_end - start_time;
    if (duration < 0) duration = 0;
    ns_perf_push(js, "measure", name, start_time, duration);
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "name",
                      JS_NewString(ctx, name ? name : ""));
    JS_SetPropertyStr(ctx, r, "entryType", JS_NewString(ctx, "measure"));
    JS_SetPropertyStr(ctx, r, "startTime", JS_NewFloat64(ctx, start_time));
    JS_SetPropertyStr(ctx, r, "duration",  JS_NewFloat64(ctx, duration));
    if (name) JS_FreeCString(ctx, name);
    return r;
}

static void
ns_perf_clear(ns_js *js, const char *type, const char *name)
{
    if (!js || !js->perf_entries) return;
    for (guint i = js->perf_entries->len; i > 0; i--) {
        const ns_perf_entry *e = g_ptr_array_index(js->perf_entries, i - 1);
        if (!e) continue;
        if (type && (!e->type || strcmp(e->type, type))) continue;
        if (name && (!e->name || strcmp(e->name, name))) continue;
        g_ptr_array_remove_index(js->perf_entries, i - 1);
    }
}

JSValue
ns_window_performance_clearMarks(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    ns_js *js = js_from_ctx(ctx);
    const char *name = argc > 0 && JS_IsString(argv[0])
                         ? JS_ToCString(ctx, argv[0]) : NULL;
    ns_perf_clear(js, "mark", name);
    if (name) JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

JSValue
ns_window_performance_clearMeasures(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    ns_js *js = js_from_ctx(ctx);
    const char *name = argc > 0 && JS_IsString(argv[0])
                         ? JS_ToCString(ctx, argv[0]) : NULL;
    ns_perf_clear(js, "measure", name);
    if (name) JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

JSValue
ns_window_performance_getEntries(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    ns_js *js = js_from_ctx(ctx);
    JSValue arr = JS_NewArray(ctx);
    if (!js) return arr;
    uint32_t out = 0;
    JS_SetPropertyUint32(ctx, arr, out++,
                         ns_perf_build_navigation_entry(ctx, js));
    JS_SetPropertyUint32(ctx, arr, out++,
                         ns_perf_build_paint_entry(ctx, "first-paint", 60));
    JS_SetPropertyUint32(ctx, arr, out++,
        ns_perf_build_paint_entry(ctx, "first-contentful-paint", 65));
    if (js->perf_entries)
        for (guint i = 0; i < js->perf_entries->len; i++) {
            const ns_perf_entry *e = g_ptr_array_index(js->perf_entries, i);
            if (e) JS_SetPropertyUint32(ctx, arr, out++,
                                        ns_perf_entry_to_js(ctx, e));
        }
    return arr;
}

JSValue
ns_window_performance_getEntriesByName(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)this_val;
    ns_js *js = js_from_ctx(ctx);
    JSValue arr = JS_NewArray(ctx);
    if (!js || !js->perf_entries || argc < 1) return arr;
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *type = argc > 1 && JS_IsString(argv[1])
                         ? JS_ToCString(ctx, argv[1]) : NULL;
    uint32_t out = 0;
    for (guint i = 0; i < js->perf_entries->len; i++) {
        const ns_perf_entry *e = g_ptr_array_index(js->perf_entries, i);
        if (!e) continue;
        if (name && (!e->name || strcmp(e->name, name))) continue;
        if (type && (!e->type || strcmp(e->type, type))) continue;
        JS_SetPropertyUint32(ctx, arr, out++, ns_perf_entry_to_js(ctx, e));
    }
    if (name) JS_FreeCString(ctx, name);
    if (type) JS_FreeCString(ctx, type);
    return arr;
}

JSValue
ns_window_performance_getEntriesByType(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)this_val;
    ns_js *js = js_from_ctx(ctx);
    JSValue arr = JS_NewArray(ctx);
    if (!js || argc < 1) return arr;
    const char *type = JS_ToCString(ctx, argv[0]);
    uint32_t out = 0;
    if (type && strcmp(type, "navigation") == 0) {
        JS_SetPropertyUint32(ctx, arr, out++,
                             ns_perf_build_navigation_entry(ctx, js));
    } else if (type && strcmp(type, "paint") == 0) {
        JS_SetPropertyUint32(ctx, arr, out++,
                             ns_perf_build_paint_entry(ctx, "first-paint", 60));
        JS_SetPropertyUint32(ctx, arr, out++,
            ns_perf_build_paint_entry(ctx, "first-contentful-paint", 65));
    }
    if (js->perf_entries)
        for (guint i = 0; i < js->perf_entries->len; i++) {
            const ns_perf_entry *e = g_ptr_array_index(js->perf_entries, i);
            if (!e) continue;
            if (type && (!e->type || strcmp(e->type, type))) continue;
            JS_SetPropertyUint32(ctx, arr, out++, ns_perf_entry_to_js(ctx, e));
        }
    if (type) JS_FreeCString(ctx, type);
    return arr;
}

JSValue
ns_window_performance_memory_get(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    ns_js *js = js_from_ctx(ctx);
    JSValue mem = JS_NewObject(ctx);
    int64_t used = 0, total = 0, limit = 128LL * 1024 * 1024;
    if (js && js->rt) {
        JSMemoryUsage u;
        memset(&u, 0, sizeof(u));
        JS_ComputeMemoryUsage(js->rt, &u);
        used = u.memory_used_size > 0 ? u.memory_used_size : 0;
        total = u.malloc_size > 0 ? u.malloc_size : used;
        if (u.malloc_limit > 0) limit = u.malloc_limit;
        if (total < used) total = used;
    }
    JS_SetPropertyStr(ctx, mem, "jsHeapSizeLimit", JS_NewFloat64(ctx, (double)limit));
    JS_SetPropertyStr(ctx, mem, "totalJSHeapSize", JS_NewFloat64(ctx, (double)total));
    JS_SetPropertyStr(ctx, mem, "usedJSHeapSize",  JS_NewFloat64(ctx, (double)used));
    return mem;
}
