/* Nordstjernen — experimental V8 JavaScript engine backend implementing the
 * js.h engine contract (pure-JS execution; DOM bindings are minimal stubs).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include <glib.h>
#include <cairo.h>

#include "js.h"
#include "config.h"
#include "css.h"
#include "dom.h"
#include "html.h"
#include "layout.h"
#include "net.h"
#include "webcrypto.h"

#include <glib/gstdio.h>

#include <openssl/rand.h>

#include <libplatform/libplatform.h>
#include <v8.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

extern "C" gboolean ns_ext_should_block(const char *url, const char *initiator);
extern "C" char *ns_webgl_take_pending_origin(void);
extern "C" void ns_webgl_set_decision(const char *origin, int allow);

namespace {

std::unique_ptr<v8::Platform> g_v8_platform;

void ns_v8_global_init(void)
{
    static gsize once = 0;
    if (g_once_init_enter(&once)) {
        g_v8_platform = v8::platform::NewDefaultPlatform();
        v8::V8::InitializePlatform(g_v8_platform.get());
        v8::V8::Initialize();
        g_once_init_leave(&once, 1);
    }
}

struct ns_v8_listener {
    std::string type;
    v8::Global<v8::Function> fn;
};

struct ns_v8_timer;
struct ns_v8_wrap;

}

struct ns_v8_request;

struct ns_js_drag_session {
    int unused;
};

struct ns_js {
    v8::Isolate *isolate;
    v8::ArrayBuffer::Allocator *allocator;
    v8::Global<v8::Context> context;
    v8::Global<v8::Object> document;
    v8::Global<v8::FunctionTemplate> node_tmpl;
    std::vector<ns_v8_wrap *> wraps;

    ns_js_log_cb      log_cb;      gpointer log_user_data;
    ns_js_mutated_cb  mut_cb;      gpointer mut_user_data;
    ns_js_navigate_cb nav_cb;      gpointer nav_user_data;
    ns_js_download_cb download_cb; gpointer download_user_data;
    ns_js_audio_cb    audio_cb;    gpointer audio_user_data;
    ns_js_media_seek_cb media_seek_cb; gpointer media_seek_user_data;
    ns_js_media_play_cb media_play_cb; gpointer media_play_user_data;
    ns_js_media_muted_cb media_muted_cb; gpointer media_muted_user_data;
    ns_js_mse_cb mse_cb; gpointer mse_user_data;
    ns_js_mse_buffered_cb mse_buffered_cb; gpointer mse_buffered_user_data;
    ns_js_mse_remove_cb mse_remove_cb; gpointer mse_remove_user_data;
    ns_js_media_volume_cb media_volume_cb; gpointer media_volume_user_data;
    ns_js_form_submit_cb form_submit_cb; gpointer form_submit_user_data;
    ns_js_layout_flush_cb layout_flush_cb; gpointer layout_flush_user_data;

    char *current_url;
    char *partition;
    char *early_inject_src;
    ns_node *current_doc;
    const ns_node *focused;
    const struct ns_box *layout_root;
    GHashTable *styles;
    gboolean in_layout_flush;
    double scroll_x, scroll_y;
    std::map<const ns_node *, struct ns_v8_canvas *> canvases;
    GHashTable *local_storage;
    char *local_storage_path;
    gboolean local_storage_dirty;
    guint local_storage_flush_source;

    GPtrArray *csp_headers;
    std::vector<struct ns_v8_worker *> workers;
    std::map<std::string, v8::Global<v8::Module>> modules;
    std::map<int, std::string> module_urls;
    std::map<int, ns_v8_timer *> timers;
    std::vector<v8::Global<v8::Function>> raf_queue;
    std::vector<ns_v8_listener> listeners;
    std::vector<struct ns_v8_request *> pending_requests;

    gint64 origin_us;
    int next_timer_id;
    int pump_depth;
    int ready_state;
    gboolean mutated;
};

struct ns_v8_request {
    ns_js *js;
    GCancellable *cancellable;
    v8::Global<v8::Promise::Resolver> resolver;
    char *url;
};

struct ns_v8_worker {
    ns_js *owner;
    GThread *thread;
    GAsyncQueue *inbox;
    GAsyncQueue *outbox;
    gint terminate;
    guint pending_idle;
    char *url;
    char *source;
    v8::Global<v8::Object> js_obj;
};

struct ns_v8_canvas {
    ns_js *js;
    const ns_node *node;
    cairo_surface_t *surf;
    cairo_t *cr;
    double fill[4];
    double stroke[4];
    std::string fill_style;
    std::string stroke_style;
    double global_alpha;
    double font_size;
    v8::Global<v8::Object> ctx_obj;
};

namespace {

struct ns_v8_timer {
    ns_js *js;
    int id;
    gboolean repeat;
    guint source_id;
    v8::Global<v8::Function> fn;
};

struct ns_v8_scope {
    v8::Isolate::Scope iso_scope;
    v8::HandleScope hs;
    v8::Local<v8::Context> ctx;
    v8::Context::Scope ctx_scope;
    explicit ns_v8_scope(ns_js *js)
        : iso_scope(js->isolate),
          hs(js->isolate),
          ctx(js->context.Get(js->isolate)),
          ctx_scope(ctx) {}
};

struct ns_v8_pump_guard {
    ns_js *js;
    explicit ns_v8_pump_guard(ns_js *j) : js(j) { js->pump_depth++; }
    ~ns_v8_pump_guard() { js->pump_depth--; }
};

ns_js *ns_v8_js_of(v8::Isolate *iso)
{
    return static_cast<ns_js *>(iso->GetData(0));
}

v8::Local<v8::String> ns_v8_str(v8::Isolate *iso, const char *s)
{
    return v8::String::NewFromUtf8(iso, s ? s : "",
                                   v8::NewStringType::kNormal)
        .ToLocalChecked();
}

std::string ns_v8_utf8(v8::Isolate *iso, v8::Local<v8::Value> v)
{
    if (v.IsEmpty()) return "";
    v8::String::Utf8Value u(iso, v);
    return *u ? std::string(*u, u.length()) : "";
}

void ns_v8_log(ns_js *js, const char *line)
{
    if (js->log_cb) js->log_cb(line, js->log_user_data);
}

void ns_v8_settle(ns_js *js)
{
    js->isolate->PerformMicrotaskCheckpoint();
    while (v8::platform::PumpMessageLoop(g_v8_platform.get(), js->isolate)) {}
}

void ns_v8_report_try_catch(ns_js *js, v8::TryCatch &tc, const char *origin)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    std::string msg = ns_v8_utf8(iso, tc.Exception());
    std::string stack;
    v8::Local<v8::Value> st;
    if (tc.StackTrace(ctx).ToLocal(&st)) stack = ns_v8_utf8(iso, st);
    char *line = g_strdup_printf("[error] %s: %s%s%s",
                                 origin ? origin : "script",
                                 msg.empty() ? "(no message)" : msg.c_str(),
                                 stack.empty() ? "" : "\n",
                                 stack.c_str());
    ns_v8_log(js, line);
    g_free(line);
}

gboolean ns_v8_eval(ns_js *js, const char *src, gssize len, const char *origin,
                    v8::Local<v8::Value> *result_out)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::TryCatch tc(iso);
    v8::Local<v8::String> code;
    if (!v8::String::NewFromUtf8(iso, src ? src : "",
                                 v8::NewStringType::kNormal,
                                 len < 0 ? -1 : (int)len)
             .ToLocal(&code)) {
        ns_v8_log(js, "[error] script source is not valid UTF-8");
        return FALSE;
    }
    v8::ScriptOrigin so(ns_v8_str(iso, origin ? origin : "inline"));
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(ctx, code, &so).ToLocal(&script)) {
        ns_v8_report_try_catch(js, tc, origin);
        return FALSE;
    }
    v8::Local<v8::Value> result;
    if (!script->Run(ctx).ToLocal(&result)) {
        ns_v8_report_try_catch(js, tc, origin);
        ns_v8_settle(js);
        return FALSE;
    }
    if (result_out) *result_out = result;
    ns_v8_settle(js);
    return TRUE;
}

void ns_v8_call_function(ns_js *js, v8::Local<v8::Function> fn, int argc,
                         v8::Local<v8::Value> *argv, const char *origin)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::TryCatch tc(iso);
    v8::Local<v8::Value> r;
    if (!fn->Call(ctx, ctx->Global(), argc, argv).ToLocal(&r))
        ns_v8_report_try_catch(js, tc, origin);
    ns_v8_settle(js);
}

void ns_v8_event_prevent_default(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    if (info.This()->IsObject())
        info.This()
            ->Set(ctx, ns_v8_str(iso, "defaultPrevented"),
                  v8::Boolean::New(iso, true))
            .Check();
}

void ns_v8_event_stop_propagation(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    if (info.This()->IsObject())
        info.This()
            ->Set(ctx, ns_v8_str(iso, "cancelBubble"),
                  v8::Boolean::New(iso, true))
            .Check();
}

v8::Local<v8::Object> ns_v8_make_event(ns_js *js, const char *type)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Object> ev = v8::Object::New(iso);
    ev->Set(ctx, ns_v8_str(iso, "type"), ns_v8_str(iso, type)).Check();
    ev->Set(ctx, ns_v8_str(iso, "bubbles"), v8::Boolean::New(iso, true))
        .Check();
    ev->Set(ctx, ns_v8_str(iso, "cancelable"), v8::Boolean::New(iso, true))
        .Check();
    ev->Set(ctx, ns_v8_str(iso, "defaultPrevented"),
            v8::Boolean::New(iso, false)).Check();
    ev->Set(ctx, ns_v8_str(iso, "cancelBubble"),
            v8::Boolean::New(iso, false)).Check();
    ev->Set(ctx, ns_v8_str(iso, "timeStamp"),
            v8::Number::New(iso,
                (double)(g_get_monotonic_time() - js->origin_us) / 1000.0))
        .Check();
    if (!js->document.IsEmpty())
        ev->Set(ctx, ns_v8_str(iso, "target"), js->document.Get(iso)).Check();
    ev->Set(ctx, ns_v8_str(iso, "preventDefault"),
            v8::Function::New(ctx, ns_v8_event_prevent_default)
                .ToLocalChecked()).Check();
    ev->Set(ctx, ns_v8_str(iso, "stopPropagation"),
            v8::Function::New(ctx, ns_v8_event_stop_propagation)
                .ToLocalChecked()).Check();
    ev->Set(ctx, ns_v8_str(iso, "stopImmediatePropagation"),
            v8::Function::New(ctx, ns_v8_event_stop_propagation)
                .ToLocalChecked()).Check();
    return ev;
}

void ns_v8_fire(ns_js *js, const char *type, v8::Local<v8::Object> event)
{
    std::vector<v8::Global<v8::Function>> to_run;
    for (auto &l : js->listeners)
        if (l.type == type)
            to_run.emplace_back(js->isolate, l.fn.Get(js->isolate));
    if (strcmp(type, "load") == 0 || strcmp(type, "DOMContentLoaded") == 0 ||
        strcmp(type, "error") == 0) {
        v8::Local<v8::Context> ctx = js->isolate->GetCurrentContext();
        char *prop = g_strdup_printf("on%s", type);
        char *lower = g_ascii_strdown(prop, -1);
        v8::Local<v8::Value> h;
        if (ctx->Global()
                ->Get(ctx, ns_v8_str(js->isolate, lower))
                .ToLocal(&h) &&
            h->IsFunction())
            to_run.emplace_back(js->isolate, h.As<v8::Function>());
        g_free(lower);
        g_free(prop);
    }
    for (auto &g : to_run) {
        v8::Local<v8::Value> arg = event;
        v8::Local<v8::Function> fn = g.Get(js->isolate);
        ns_v8_call_function(js, fn, 1, &arg, type);
    }
}

void ns_v8_fire_simple(ns_js *js, const char *type)
{
    ns_v8_fire(js, type, ns_v8_make_event(js, type));
}

struct ns_v8_wrap {
    ns_js *js;
    ns_node *node;
    gboolean owned;
    v8::Global<v8::Object> handle;
    std::vector<ns_v8_listener> listeners;
};

void ns_v8_node_invalidated(ns_node *n)
{
    ns_v8_wrap *w = static_cast<ns_v8_wrap *>(n->js_wrapper);
    n->js_wrapper = NULL;
    n->js_invalidate = NULL;
    if (w) w->node = NULL;
}

v8::Local<v8::Value> ns_v8_wrap_node(ns_js *js, ns_node *n)
{
    v8::Isolate *iso = js->isolate;
    if (!n) return v8::Null(iso);
    if (n->js_wrapper) {
        ns_v8_wrap *w = static_cast<ns_v8_wrap *>(n->js_wrapper);
        return w->handle.Get(iso);
    }
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Function> ctor;
    if (!js->node_tmpl.Get(iso)->GetFunction(ctx).ToLocal(&ctor))
        return v8::Null(iso);
    v8::Local<v8::Object> obj;
    if (!ctor->NewInstance(ctx).ToLocal(&obj)) return v8::Null(iso);
    ns_v8_wrap *w = new ns_v8_wrap();
    w->js = js;
    w->node = n;
    w->owned = FALSE;
    w->handle.Reset(iso, obj);
    obj->SetAlignedPointerInInternalField(0, w);
    n->js_wrapper = w;
    n->js_invalidate = ns_v8_node_invalidated;
    js->wraps.push_back(w);
    return obj;
}

ns_v8_wrap *ns_v8_wrap_of(v8::Local<v8::Value> v)
{
    if (v.IsEmpty() || !v->IsObject()) return nullptr;
    v8::Local<v8::Object> o = v.As<v8::Object>();
    if (o->InternalFieldCount() < 1) return nullptr;
    return static_cast<ns_v8_wrap *>(o->GetAlignedPointerFromInternalField(0));
}

ns_node *ns_v8_node_of(v8::Local<v8::Value> v)
{
    ns_v8_wrap *w = ns_v8_wrap_of(v);
    return w ? w->node : nullptr;
}

ns_node *ns_v8_self(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    return ns_v8_node_of(info.This());
}

void ns_v8_mutated(ns_js *js)
{
    js->mutated = TRUE;
    if (js->mut_cb) js->mut_cb(js->mut_user_data);
}

gboolean ns_v8_node_in_doc(ns_js *js, const ns_node *n)
{
    return js->current_doc && n && ns_node_root(n) == js->current_doc;
}

void ns_v8_detach(ns_js *js, ns_node *n)
{
    if (ns_v8_node_in_doc(js, n) && n->parent) {
        ns_doc_id_index_subtree_removed(js->current_doc, n);
        ns_doc_class_index_subtree_removed(js->current_doc, n);
        ns_doc_tag_index_subtree_removed(js->current_doc, n);
    }
    ns_node_remove(n);
}

void ns_v8_note_inserted(ns_js *js, ns_node *n)
{
    if (ns_v8_node_in_doc(js, n)) {
        ns_doc_id_index_subtree_added(js->current_doc, n);
        ns_doc_class_index_subtree_added(js->current_doc, n);
        ns_doc_tag_index_subtree_added(js->current_doc, n);
    }
    ns_v8_mutated(js);
}

void ns_v8_wrap_set_owned(ns_js *js, ns_node *n, gboolean owned)
{
    if (!n) return;
    if (!n->js_wrapper && owned) ns_v8_wrap_node(js, n);
    if (n->js_wrapper)
        static_cast<ns_v8_wrap *>(n->js_wrapper)->owned = owned;
}

void ns_v8_set_attr_indexed(ns_js *js, ns_node *el, const char *name,
                            const char *value)
{
    gboolean in_doc = ns_v8_node_in_doc(js, el);
    if (in_doc && g_ascii_strcasecmp(name, "id") == 0) {
        const char *old = ns_element_get_attr(el, "id");
        if (old) ns_doc_id_index_unregister(js->current_doc, old, el);
        ns_element_set_attr(el, name, value);
        if (value) ns_doc_id_index_register(js->current_doc, value, el);
    } else if (in_doc && g_ascii_strcasecmp(name, "class") == 0) {
        const char *old = ns_element_get_attr(el, "class");
        if (old) ns_doc_class_index_unregister(js->current_doc, old, el);
        ns_element_set_attr(el, name, value);
        if (value) ns_doc_class_index_register(js->current_doc, value, el);
    } else {
        ns_element_set_attr(el, name, value);
    }
    ns_v8_mutated(js);
}

void ns_v8_remove_attr_indexed(ns_js *js, ns_node *el, const char *name)
{
    gboolean in_doc = ns_v8_node_in_doc(js, el);
    if (in_doc && g_ascii_strcasecmp(name, "id") == 0) {
        const char *old = ns_element_get_attr(el, "id");
        if (old) ns_doc_id_index_unregister(js->current_doc, old, el);
    } else if (in_doc && g_ascii_strcasecmp(name, "class") == 0) {
        const char *old = ns_element_get_attr(el, "class");
        if (old) ns_doc_class_index_unregister(js->current_doc, old, el);
    }
    ns_element_remove_attr(el, name);
    ns_v8_mutated(js);
}

gboolean ns_v8_matches_any(GPtrArray *sels, const ns_node *el)
{
    if (!sels || !el || el->kind != NS_NODE_ELEMENT) return FALSE;
    for (guint i = 0; i < sels->len; i++) {
        ns_css_selector *sel =
            static_cast<ns_css_selector *>(g_ptr_array_index(sels, i));
        if (ns_css_selector_matches(sel, el)) return TRUE;
    }
    return FALSE;
}

ns_node *ns_v8_query_walk_first(ns_node *n, GPtrArray *sels)
{
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT && ns_v8_matches_any(sels, c))
            return c;
        ns_node *m = ns_v8_query_walk_first(c, sels);
        if (m) return m;
    }
    return NULL;
}

void ns_v8_query_walk_all(ns_node *n, GPtrArray *sels,
                          std::vector<ns_node *> &out)
{
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT && ns_v8_matches_any(sels, c))
            out.push_back(c);
        ns_v8_query_walk_all(c, sels, out);
    }
}

void ns_v8_query(ns_js *js, ns_node *root, const char *selector,
                 gboolean all, std::vector<ns_node *> &out)
{
    if (!root || !selector) return;
    gboolean valid = FALSE;
    GPtrArray *sels = ns_css_parse_selector_list_checked(selector, &valid);
    if (!sels) return;
    if (!valid) {
        g_ptr_array_free(sels, TRUE);
        return;
    }
    const ns_node *prev_scope = ns_css_set_match_scope(root);
    const ns_node *prev_focus = ns_css_set_focus_node(js->focused);
    if (all) {
        ns_v8_query_walk_all(root, sels, out);
    } else {
        ns_node *m = ns_v8_query_walk_first(root, sels);
        if (m) out.push_back(m);
    }
    ns_css_set_focus_node(prev_focus);
    ns_css_set_match_scope(prev_scope);
    g_ptr_array_free(sels, TRUE);
}

void ns_v8_collect_text(const ns_node *n, GString *out)
{
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_TEXT && c->text) g_string_append(out, c->text);
        else if (c->kind == NS_NODE_ELEMENT) ns_v8_collect_text(c, out);
    }
}

void ns_v8_clear_children(ns_js *js, ns_node *n)
{
    while (n->first_child) {
        ns_node *c = n->first_child;
        ns_v8_detach(js, c);
        if (c->js_wrapper &&
            static_cast<ns_v8_wrap *>(c->js_wrapper)->owned) continue;
        ns_node_free(c);
    }
}

void ns_v8_append_moved(ns_js *js, ns_node *parent, ns_node *child)
{
    if (child->flags & NS_NODE_FRAGMENT) {
        ns_node *c = child->first_child;
        while (c) {
            ns_node *next = c->next_sibling;
            ns_node_remove(c);
            ns_node_append_child(parent, c);
            ns_v8_note_inserted(js, c);
            c = next;
        }
        return;
    }
    ns_v8_detach(js, child);
    ns_node_append_child(parent, child);
    ns_v8_wrap_set_owned(js, child, FALSE);
    ns_v8_note_inserted(js, child);
}

gboolean ns_v8_dom_dispatch_obj(ns_js *js, ns_node *target,
                                const char *type,
                                v8::Local<v8::Object> ev,
                                gboolean *prevented_out)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    gboolean ran = FALSE;
    ev->Set(ctx, ns_v8_str(iso, "target"), ns_v8_wrap_node(js, target))
        .Check();
    for (ns_node *n = target; n; n = n->parent) {
        std::vector<v8::Global<v8::Function>> fns;
        if (n->js_wrapper) {
            ns_v8_wrap *w = static_cast<ns_v8_wrap *>(n->js_wrapper);
            for (auto &l : w->listeners)
                if (l.type == type)
                    fns.emplace_back(iso, l.fn.Get(iso));
        }
        char *attr_name = g_strdup_printf("on%s", type);
        const char *attr_src = n->kind == NS_NODE_ELEMENT
                                   ? ns_element_get_attr(n, attr_name)
                                   : NULL;
        g_free(attr_name);
        if (attr_src && *attr_src) {
            std::string wrapped =
                std::string("(function(event){") + attr_src + "\n})";
            v8::TryCatch tc(iso);
            v8::Local<v8::Script> script;
            v8::Local<v8::Value> fnv;
            if (v8::Script::Compile(ctx, ns_v8_str(iso, wrapped.c_str()))
                    .ToLocal(&script) &&
                script->Run(ctx).ToLocal(&fnv) && fnv->IsFunction())
                fns.emplace_back(iso, fnv.As<v8::Function>());
        }
        if (!fns.empty()) {
            v8::Local<v8::Value> cur = ns_v8_wrap_node(js, n);
            ev->Set(ctx, ns_v8_str(iso, "currentTarget"), cur).Check();
            for (auto &g : fns) {
                v8::Local<v8::Function> fn = g.Get(iso);
                v8::TryCatch tc(iso);
                v8::Local<v8::Value> arg = ev;
                v8::Local<v8::Value> r;
                if (!fn->Call(ctx, cur, 1, &arg).ToLocal(&r))
                    ns_v8_report_try_catch(js, tc, type);
                ran = TRUE;
            }
            ns_v8_settle(js);
        }
        v8::Local<v8::Value> stop;
        if (ev->Get(ctx, ns_v8_str(iso, "cancelBubble")).ToLocal(&stop) &&
            stop->BooleanValue(iso))
            break;
    }
    for (auto &l : js->listeners) {
        if (l.type != type) continue;
        v8::Local<v8::Value> arg = ev;
        ns_v8_call_function(js, l.fn.Get(iso), 1, &arg, type);
        ran = TRUE;
    }
    if (prevented_out) {
        v8::Local<v8::Value> p;
        *prevented_out =
            ev->Get(ctx, ns_v8_str(iso, "defaultPrevented")).ToLocal(&p) &&
            p->BooleanValue(iso);
    }
    return ran;
}

gboolean ns_v8_dom_dispatch(ns_js *js, ns_node *target, const char *type,
                            gboolean *prevented_out)
{
    return ns_v8_dom_dispatch_obj(js, target, type,
                                  ns_v8_make_event(js, type), prevented_out);
}

ns_js *ns_v8_js_here(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    return ns_v8_js_of(info.GetIsolate());
}

void ns_v8_el_get_attribute(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_node *n = ns_v8_self(info);
    v8::Isolate *iso = info.GetIsolate();
    if (!n || n->kind != NS_NODE_ELEMENT || info.Length() < 1) {
        info.GetReturnValue().SetNull();
        return;
    }
    char *name = g_ascii_strdown(ns_v8_utf8(iso, info[0]).c_str(), -1);
    const char *v = ns_element_get_attr(n, name);
    g_free(name);
    if (v) info.GetReturnValue().Set(ns_v8_str(iso, v));
    else info.GetReturnValue().SetNull();
}

void ns_v8_el_set_attribute(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    v8::Isolate *iso = info.GetIsolate();
    if (!js || !n || n->kind != NS_NODE_ELEMENT || info.Length() < 2) return;
    char *name = g_ascii_strdown(ns_v8_utf8(iso, info[0]).c_str(), -1);
    std::string value = ns_v8_utf8(iso, info[1]);
    ns_v8_set_attr_indexed(js, n, name, value.c_str());
    g_free(name);
}

void ns_v8_el_remove_attribute(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    if (!js || !n || n->kind != NS_NODE_ELEMENT || info.Length() < 1) return;
    char *name =
        g_ascii_strdown(ns_v8_utf8(info.GetIsolate(), info[0]).c_str(), -1);
    ns_v8_remove_attr_indexed(js, n, name);
    g_free(name);
}

void ns_v8_el_has_attribute(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_node *n = ns_v8_self(info);
    if (!n || n->kind != NS_NODE_ELEMENT || info.Length() < 1) {
        info.GetReturnValue().Set(false);
        return;
    }
    char *name =
        g_ascii_strdown(ns_v8_utf8(info.GetIsolate(), info[0]).c_str(), -1);
    info.GetReturnValue().Set(ns_element_get_attr(n, name) != NULL);
    g_free(name);
}

void ns_v8_el_get_attribute_names(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Array> arr = v8::Array::New(iso);
    ns_node *n = ns_v8_self(info);
    if (n && n->kind == NS_NODE_ELEMENT) {
        guint i = 0;
        for (ns_attr *a = n->attrs; a; a = a->next)
            if (a->name)
                arr->Set(ctx, i++, ns_v8_str(iso, a->name)).Check();
    }
    info.GetReturnValue().Set(arr);
}

void ns_v8_el_append_child(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    ns_node *child = info.Length() >= 1 ? ns_v8_node_of(info[0]) : NULL;
    if (!js || !n || !child || child == n) return;
    for (ns_node *p = n; p; p = p->parent)
        if (p == child) return;
    ns_v8_append_moved(js, n, child);
    info.GetReturnValue().Set(info[0]);
}

void ns_v8_insert_node_before(ns_js *js, ns_node *parent, ns_node *child,
                              ns_node *ref)
{
    if (!ref || ref->parent != parent) {
        ns_v8_append_moved(js, parent, child);
        return;
    }
    if (child->flags & NS_NODE_FRAGMENT) {
        ns_node *c = child->first_child;
        while (c) {
            ns_node *next = c->next_sibling;
            ns_node_remove(c);
            ns_v8_insert_node_before(js, parent, c, ref);
            c = next;
        }
        return;
    }
    ns_v8_detach(js, child);
    child->parent = parent;
    child->next_sibling = ref;
    child->prev_sibling = ref->prev_sibling;
    if (ref->prev_sibling) ref->prev_sibling->next_sibling = child;
    else parent->first_child = child;
    ref->prev_sibling = child;
    ns_v8_wrap_set_owned(js, child, FALSE);
    ns_v8_note_inserted(js, child);
}

void ns_v8_el_insert_before(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    ns_node *child = info.Length() >= 1 ? ns_v8_node_of(info[0]) : NULL;
    ns_node *ref = info.Length() >= 2 ? ns_v8_node_of(info[1]) : NULL;
    if (!js || !n || !child || child == n) return;
    for (ns_node *p = n; p; p = p->parent)
        if (p == child) return;
    ns_v8_insert_node_before(js, n, child, ref);
    info.GetReturnValue().Set(info[0]);
}

void ns_v8_el_remove_child(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    ns_node *child = info.Length() >= 1 ? ns_v8_node_of(info[0]) : NULL;
    if (!js || !n || !child || child->parent != n) return;
    ns_v8_detach(js, child);
    ns_v8_wrap_set_owned(js, child, TRUE);
    ns_v8_mutated(js);
    info.GetReturnValue().Set(info[0]);
}

void ns_v8_el_replace_child(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    ns_node *newc = info.Length() >= 1 ? ns_v8_node_of(info[0]) : NULL;
    ns_node *oldc = info.Length() >= 2 ? ns_v8_node_of(info[1]) : NULL;
    if (!js || !n || !newc || !oldc || oldc->parent != n) return;
    ns_v8_insert_node_before(js, n, newc, oldc);
    ns_v8_detach(js, oldc);
    ns_v8_wrap_set_owned(js, oldc, TRUE);
    ns_v8_mutated(js);
    info.GetReturnValue().Set(info[1]);
}

void ns_v8_el_remove_self(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    if (!js || !n || !n->parent) return;
    ns_v8_detach(js, n);
    ns_v8_wrap_set_owned(js, n, TRUE);
    ns_v8_mutated(js);
}

void ns_v8_el_append_any(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    v8::Isolate *iso = info.GetIsolate();
    if (!js || !n) return;
    for (int i = 0; i < info.Length(); i++) {
        ns_node *child = ns_v8_node_of(info[i]);
        if (child) {
            ns_v8_append_moved(js, n, child);
        } else {
            std::string s = ns_v8_utf8(iso, info[i]);
            ns_node *t = ns_node_new_text(g_strdup(s.c_str()));
            ns_node_append_child(n, t);
            ns_v8_note_inserted(js, t);
        }
    }
}

void ns_v8_el_clone_node(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    if (!js || !n) {
        info.GetReturnValue().SetNull();
        return;
    }
    gboolean deep = info.Length() >= 1 &&
                    info[0]->BooleanValue(info.GetIsolate());
    ns_node *clone = ns_node_clone(n, deep);
    if (!clone) {
        info.GetReturnValue().SetNull();
        return;
    }
    v8::Local<v8::Value> w = ns_v8_wrap_node(js, clone);
    ns_v8_wrap_set_owned(js, clone, TRUE);
    info.GetReturnValue().Set(w);
}

void ns_v8_el_contains(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_node *n = ns_v8_self(info);
    ns_node *other = info.Length() >= 1 ? ns_v8_node_of(info[0]) : NULL;
    gboolean found = FALSE;
    for (ns_node *p = other; p; p = p->parent)
        if (p == n) {
            found = TRUE;
            break;
        }
    info.GetReturnValue().Set(found != FALSE);
}

void ns_v8_return_node_vector(const v8::FunctionCallbackInfo<v8::Value> &info,
                              ns_js *js, std::vector<ns_node *> &nodes,
                              gboolean first_only)
{
    v8::Isolate *iso = info.GetIsolate();
    if (first_only) {
        if (nodes.empty()) info.GetReturnValue().SetNull();
        else info.GetReturnValue().Set(ns_v8_wrap_node(js, nodes[0]));
        return;
    }
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Array> arr = v8::Array::New(iso, (int)nodes.size());
    for (guint i = 0; i < nodes.size(); i++)
        arr->Set(ctx, i, ns_v8_wrap_node(js, nodes[i])).Check();
    info.GetReturnValue().Set(arr);
}

void ns_v8_el_query(const v8::FunctionCallbackInfo<v8::Value> &info,
                    gboolean all)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    v8::Isolate *iso = info.GetIsolate();
    std::vector<ns_node *> nodes;
    if (js && n && info.Length() >= 1)
        ns_v8_query(js, n, ns_v8_utf8(iso, info[0]).c_str(), all, nodes);
    ns_v8_return_node_vector(info, js, nodes, !all);
}

void ns_v8_el_query_selector(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_el_query(info, FALSE);
}

void ns_v8_el_query_selector_all(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_el_query(info, TRUE);
}

void ns_v8_el_matches(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    v8::Isolate *iso = info.GetIsolate();
    gboolean matched = FALSE;
    if (js && n && info.Length() >= 1) {
        gboolean valid = FALSE;
        GPtrArray *sels = ns_css_parse_selector_list_checked(
            ns_v8_utf8(iso, info[0]).c_str(), &valid);
        if (sels) {
            if (valid) {
                const ns_node *pf = ns_css_set_focus_node(js->focused);
                matched = ns_v8_matches_any(sels, n);
                ns_css_set_focus_node(pf);
            }
            g_ptr_array_free(sels, TRUE);
        }
    }
    info.GetReturnValue().Set(matched != FALSE);
}

void ns_v8_el_closest(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    v8::Isolate *iso = info.GetIsolate();
    info.GetReturnValue().SetNull();
    if (!js || !n || info.Length() < 1) return;
    gboolean valid = FALSE;
    GPtrArray *sels = ns_css_parse_selector_list_checked(
        ns_v8_utf8(iso, info[0]).c_str(), &valid);
    if (!sels) return;
    if (valid) {
        const ns_node *pf = ns_css_set_focus_node(js->focused);
        for (ns_node *p = n; p; p = p->parent) {
            if (p->kind == NS_NODE_ELEMENT && ns_v8_matches_any(sels, p)) {
                info.GetReturnValue().Set(ns_v8_wrap_node(js, p));
                break;
            }
        }
        ns_css_set_focus_node(pf);
    }
    g_ptr_array_free(sels, TRUE);
}

void ns_v8_collect_by_tag(ns_node *n, const char *tag,
                          std::vector<ns_node *> &out)
{
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT &&
            (strcmp(tag, "*") == 0 ||
             (c->name && g_ascii_strcasecmp(c->name, tag) == 0)))
            out.push_back(c);
        ns_v8_collect_by_tag(c, tag, out);
    }
}

void ns_v8_collect_by_class(ns_node *n, const char *cls, gsize len,
                            std::vector<ns_node *> &out)
{
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT && ns_node_has_class(c, cls, len))
            out.push_back(c);
        ns_v8_collect_by_class(c, cls, len, out);
    }
}

void ns_v8_el_get_by_tag(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    std::vector<ns_node *> nodes;
    if (js && n && info.Length() >= 1) {
        std::string tag = ns_v8_utf8(info.GetIsolate(), info[0]);
        ns_v8_collect_by_tag(n, tag.c_str(), nodes);
    }
    ns_v8_return_node_vector(info, js, nodes, FALSE);
}

void ns_v8_el_get_by_class(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    std::vector<ns_node *> nodes;
    if (js && n && info.Length() >= 1) {
        std::string cls = ns_v8_utf8(info.GetIsolate(), info[0]);
        if (!cls.empty())
            ns_v8_collect_by_class(n, cls.c_str(), cls.size(), nodes);
    }
    ns_v8_return_node_vector(info, js, nodes, FALSE);
}

void ns_v8_el_add_listener(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_v8_wrap *w = ns_v8_wrap_of(info.This());
    if (!w || info.Length() < 2 || !info[1]->IsFunction()) return;
    ns_v8_listener l;
    l.type = ns_v8_utf8(iso, info[0]);
    l.fn.Reset(iso, info[1].As<v8::Function>());
    w->listeners.push_back(std::move(l));
}

void ns_v8_el_remove_listener(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_v8_wrap *w = ns_v8_wrap_of(info.This());
    if (!w || info.Length() < 2 || !info[1]->IsFunction()) return;
    std::string type = ns_v8_utf8(iso, info[0]);
    v8::Local<v8::Function> fn = info[1].As<v8::Function>();
    for (auto it = w->listeners.begin(); it != w->listeners.end(); ++it) {
        if (it->type == type && it->fn.Get(iso) == fn) {
            w->listeners.erase(it);
            return;
        }
    }
}

void ns_v8_el_dispatch_event(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    v8::Isolate *iso = info.GetIsolate();
    info.GetReturnValue().Set(true);
    if (!js || !n || info.Length() < 1 || !info[0]->IsObject()) return;
    v8::Local<v8::Object> ev = info[0].As<v8::Object>();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Value> tv;
    if (!ev->Get(ctx, ns_v8_str(iso, "type")).ToLocal(&tv)) return;
    std::string type = ns_v8_utf8(iso, tv);
    gboolean prevented = FALSE;
    ns_v8_dom_dispatch_obj(js, n, type.c_str(), ev, &prevented);
    info.GetReturnValue().Set(!prevented);
}

void ns_v8_el_click(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    if (!js || !n) return;
    ns_v8_dom_dispatch(js, n, "click", NULL);
}

void ns_v8_el_focus(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    if (js && n) js->focused = n;
}

void ns_v8_el_blur(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    if (js && n && js->focused == n) js->focused = NULL;
}

void ns_v8_el_node_type(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_node *n = ns_v8_self(info);
    int t = 1;
    if (n) {
        switch (n->kind) {
        case NS_NODE_ELEMENT: t = 1; break;
        case NS_NODE_TEXT: t = 3; break;
        case NS_NODE_COMMENT: t = 8; break;
        case NS_NODE_DOCUMENT: t = 9; break;
        default: t = 1; break;
        }
        if (n->flags & NS_NODE_FRAGMENT) t = 11;
    }
    info.GetReturnValue().Set(t);
}

void ns_v8_el_node_name(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_node *n = ns_v8_self(info);
    if (!n) {
        info.GetReturnValue().Set(ns_v8_str(iso, ""));
        return;
    }
    if (n->kind == NS_NODE_TEXT) {
        info.GetReturnValue().Set(ns_v8_str(iso, "#text"));
        return;
    }
    if (n->kind == NS_NODE_COMMENT) {
        info.GetReturnValue().Set(ns_v8_str(iso, "#comment"));
        return;
    }
    char *upper = g_ascii_strup(n->name ? n->name : "", -1);
    info.GetReturnValue().Set(ns_v8_str(iso, upper));
    g_free(upper);
}

void ns_v8_el_rel_get(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    if (!js || !n) {
        info.GetReturnValue().SetNull();
        return;
    }
    int code = (int)(intptr_t)info.Data().As<v8::External>()->Value();
    ns_node *r = NULL;
    switch (code) {
    case 0: r = n->parent; break;
    case 1: r = n->first_child; break;
    case 2: r = n->last_child; break;
    case 3: r = n->next_sibling; break;
    case 4: r = n->prev_sibling; break;
    case 5:
        for (r = n->first_child; r && r->kind != NS_NODE_ELEMENT;
             r = r->next_sibling) {}
        break;
    case 6:
        for (r = n->last_child; r && r->kind != NS_NODE_ELEMENT;
             r = r->prev_sibling) {}
        break;
    case 7:
        for (r = n->next_sibling; r && r->kind != NS_NODE_ELEMENT;
             r = r->next_sibling) {}
        break;
    case 8:
        for (r = n->prev_sibling; r && r->kind != NS_NODE_ELEMENT;
             r = r->prev_sibling) {}
        break;
    case 9:
        r = n->parent && n->parent->kind == NS_NODE_ELEMENT ? n->parent
                                                            : NULL;
        break;
    }
    if (r) info.GetReturnValue().Set(ns_v8_wrap_node(js, r));
    else info.GetReturnValue().SetNull();
}

void ns_v8_el_children_get(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    gboolean elements_only =
        (intptr_t)info.Data().As<v8::External>()->Value() != 0;
    std::vector<ns_node *> nodes;
    if (n)
        for (ns_node *c = n->first_child; c; c = c->next_sibling)
            if (!elements_only || c->kind == NS_NODE_ELEMENT)
                nodes.push_back(c);
    ns_v8_return_node_vector(info, js, nodes, FALSE);
}

void ns_v8_el_child_count(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_node *n = ns_v8_self(info);
    int count = 0;
    if (n)
        for (ns_node *c = n->first_child; c; c = c->next_sibling)
            if (c->kind == NS_NODE_ELEMENT) count++;
    info.GetReturnValue().Set(count);
}

void ns_v8_el_text_get(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_node *n = ns_v8_self(info);
    if (!n) {
        info.GetReturnValue().Set(ns_v8_str(iso, ""));
        return;
    }
    if (n->kind == NS_NODE_TEXT || n->kind == NS_NODE_COMMENT) {
        info.GetReturnValue().Set(ns_v8_str(iso, n->text ? n->text : ""));
        return;
    }
    GString *text = g_string_new(NULL);
    ns_v8_collect_text(n, text);
    info.GetReturnValue().Set(ns_v8_str(iso, text->str));
    g_string_free(text, TRUE);
}

void ns_v8_el_text_set(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    v8::Isolate *iso = info.GetIsolate();
    if (!js || !n || info.Length() < 1) return;
    std::string s = ns_v8_utf8(iso, info[0]);
    if (n->kind == NS_NODE_TEXT || n->kind == NS_NODE_COMMENT) {
        ns_node_replace_text_owned(n, g_strdup(s.c_str()));
        ns_v8_mutated(js);
        return;
    }
    ns_v8_clear_children(js, n);
    if (!s.empty()) {
        ns_node *t = ns_node_new_text(g_strdup(s.c_str()));
        ns_node_append_child(n, t);
    }
    ns_v8_mutated(js);
}

void ns_v8_el_inner_html_get(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_node *n = ns_v8_self(info);
    if (!n) {
        info.GetReturnValue().Set(ns_v8_str(iso, ""));
        return;
    }
    char *html = ns_node_inner_html(n);
    info.GetReturnValue().Set(ns_v8_str(iso, html ? html : ""));
    g_free(html);
}

void ns_v8_el_inner_html_set(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    v8::Isolate *iso = info.GetIsolate();
    if (!js || !n || n->kind != NS_NODE_ELEMENT || info.Length() < 1) return;
    std::string s = ns_v8_utf8(iso, info[0]);
    ns_node *target = n;
    const char *context_tag = n->name ? n->name : "div";
    if (ns_node_is_element_named(n, "template")) {
        target = ns_template_content_get(n);
        context_tag = "template";
        if (!target) return;
    }
    ns_node *fragment = ns_html_parse_fragment_in(context_tag, s.c_str(),
                                                  (gssize)s.size());
    if (!fragment) return;
    ns_v8_clear_children(js, target);
    ns_node *c = fragment->first_child;
    while (c) {
        ns_node *next = c->next_sibling;
        ns_node_own_strings_deep(c);
        ns_node_remove(c);
        ns_node_append_child(target, c);
        ns_v8_note_inserted(js, c);
        c = next;
    }
    ns_node_free(fragment);
    ns_v8_mutated(js);
}

void ns_v8_el_outer_html_get(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_node *n = ns_v8_self(info);
    if (!n) {
        info.GetReturnValue().Set(ns_v8_str(iso, ""));
        return;
    }
    char *html = ns_node_outer_html(n);
    info.GetReturnValue().Set(ns_v8_str(iso, html ? html : ""));
    g_free(html);
}

void ns_v8_el_owner_document(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    if (js && !js->document.IsEmpty())
        info.GetReturnValue().Set(js->document.Get(info.GetIsolate()));
    else
        info.GetReturnValue().SetNull();
}

void ns_v8_collect_options(ns_node *select, std::vector<ns_node *> &out)
{
    for (ns_node *c = select->first_child; c; c = c->next_sibling) {
        if (ns_node_is_element_named(c, "option")) {
            out.push_back(c);
        } else if (ns_node_is_element_named(c, "optgroup")) {
            for (ns_node *cc = c->first_child; cc; cc = cc->next_sibling)
                if (ns_node_is_element_named(cc, "option"))
                    out.push_back(cc);
        }
    }
}

ns_node *ns_v8_option_select_of(ns_node *option)
{
    ns_node *select = option->parent;
    if (select && ns_node_is_element_named(select, "optgroup"))
        select = select->parent;
    if (!select || !ns_node_is_element_named(select, "select")) return NULL;
    return select;
}

void ns_v8_select_set_selected(ns_js *js, ns_node *option)
{
    ns_node *select = ns_v8_option_select_of(option);
    if (!select) return;
    std::vector<ns_node *> opts;
    ns_v8_collect_options(select, opts);
    for (ns_node *o : opts) ns_element_remove_attr(o, "selected");
    ns_element_set_attr(option, "selected", "");
    ns_element_remove_attr(select, "data-nd-noselect");
    ns_css_mark_restyle_dirty(select);
    ns_v8_mutated(js);
}

gboolean ns_v8_select_choose_scoped(ns_js *js, ns_node *option)
{
    ns_node *select = ns_v8_option_select_of(option);
    if (!select || ns_element_effectively_disabled(option)) return FALSE;
    ns_v8_select_set_selected(js, option);
    ns_v8_dom_dispatch(js, select, "input", NULL);
    ns_v8_dom_dispatch(js, select, "change", NULL);
    return TRUE;
}

int ns_v8_select_current_index(ns_node *select, std::vector<ns_node *> &opts)
{
    const ns_node *cur = ns_select_chosen_option(select);
    for (guint i = 0; i < opts.size(); i++)
        if (opts[i] == cur) return (int)i;
    return -1;
}

void ns_v8_clear_radio_group(ns_js *js, ns_node *el)
{
    const char *group = ns_element_get_attr(el, "name");
    if (!group || !js->current_doc) return;
    std::vector<ns_node *> inputs;
    ns_v8_collect_by_tag(js->current_doc, "input", inputs);
    for (ns_node *r : inputs) {
        if (r == el) continue;
        const char *rt = ns_element_get_attr(r, "type");
        const char *rn = ns_element_get_attr(r, "name");
        if (rt && rn && g_ascii_strcasecmp(rt, "radio") == 0 &&
            strcmp(rn, group) == 0)
            ns_element_set_attr(r, "data-nd-checked", "0");
    }
}

void ns_v8_el_value_get(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_node *n = ns_v8_self(info);
    if (!n || n->kind != NS_NODE_ELEMENT || !n->name) {
        info.GetReturnValue().Set(ns_v8_str(iso, ""));
        return;
    }
    if (strcmp(n->name, "select") == 0) {
        const ns_node *opt = ns_element_get_attr(n, "multiple")
                                 ? ns_select_first_selected_option(n)
                                 : ns_select_chosen_option(n);
        char *v = ns_option_value_dup(opt);
        info.GetReturnValue().Set(ns_v8_str(iso, v ? v : ""));
        g_free(v);
        return;
    }
    if (strcmp(n->name, "option") == 0) {
        char *v = ns_option_value_dup(n);
        info.GetReturnValue().Set(ns_v8_str(iso, v ? v : ""));
        g_free(v);
        return;
    }
    if (strcmp(n->name, "textarea") == 0 || strcmp(n->name, "output") == 0) {
        char *t = ns_node_collect_text(n);
        info.GetReturnValue().Set(ns_v8_str(iso, t ? t : ""));
        g_free(t);
        return;
    }
    if (strcmp(n->name, "input") == 0) {
        const char *v = ns_input_used_value(n);
        info.GetReturnValue().Set(ns_v8_str(iso, v ? v : ""));
        return;
    }
    const char *v = ns_element_get_attr(n, "value");
    info.GetReturnValue().Set(ns_v8_str(iso, v ? v : ""));
}

void ns_v8_el_value_set(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    v8::Isolate *iso = info.GetIsolate();
    if (!js || !n || n->kind != NS_NODE_ELEMENT || !n->name ||
        info.Length() < 1)
        return;
    std::string v = ns_v8_utf8(iso, info[0]);
    if (strcmp(n->name, "select") == 0) {
        std::vector<ns_node *> opts;
        ns_v8_collect_options(n, opts);
        for (ns_node *o : opts) {
            char *ov = ns_option_value_dup(o);
            gboolean hit = ov && v == ov;
            g_free(ov);
            if (hit) {
                ns_v8_select_set_selected(js, o);
                return;
            }
        }
        ns_element_set_attr(n, "data-nd-noselect", "1");
        ns_v8_mutated(js);
        return;
    }
    ns_node_set_editable_value(n, v.c_str());
    ns_css_mark_restyle_dirty(n->parent ? n->parent : n);
    ns_v8_mutated(js);
}

void ns_v8_el_checked_get(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_node *n = ns_v8_self(info);
    info.GetReturnValue().Set(n && ns_input_is_checked(n));
}

void ns_v8_el_checked_set(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    if (!js || !n || n->kind != NS_NODE_ELEMENT || info.Length() < 1) return;
    if (info[0]->BooleanValue(info.GetIsolate())) {
        const char *type = ns_element_get_attr(n, "type");
        if (type && g_ascii_strcasecmp(type, "radio") == 0)
            ns_v8_clear_radio_group(js, n);
        ns_element_set_attr(n, "data-nd-checked", "1");
    } else {
        ns_element_set_attr(n, "data-nd-checked", "0");
    }
    ns_css_mark_restyle_dirty(n->parent ? n->parent : n);
    ns_v8_mutated(js);
}

void ns_v8_flush_layout(ns_js *js)
{
    if (js->in_layout_flush || !js->layout_flush_cb) return;
    js->in_layout_flush = TRUE;
    js->layout_flush_cb(js->layout_flush_user_data);
    js->in_layout_flush = FALSE;
}

const ns_box *ns_v8_box_find(const ns_box *b, const ns_node *target)
{
    if (!b) return NULL;
    if (b->dom == target) return b;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling) {
        const ns_box *m = ns_v8_box_find(c, target);
        if (m) return m;
    }
    return NULL;
}

const ns_box *ns_v8_box_of(ns_js *js, const ns_node *n)
{
    if (!n) return NULL;
    ns_v8_flush_layout(js);
    if (!js->layout_root) return NULL;
    return ns_v8_box_find(js->layout_root, n);
}

void ns_v8_border_box(const ns_box *b, double *x, double *y, double *w,
                      double *h)
{
    *x = b->x - b->border.left;
    *y = b->y - b->border.top;
    *w = b->content_width + b->padding.left + b->padding.right +
         b->border.left + b->border.right;
    *h = b->content_height + b->padding.top + b->padding.bottom +
         b->border.top + b->border.bottom;
}

void ns_v8_el_metric_get(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    int code = (int)(intptr_t)info.Data().As<v8::External>()->Value();
    double out = 0;
    const ns_box *b = js ? ns_v8_box_of(js, n) : NULL;
    if (b) {
        double x, y, w, h;
        ns_v8_border_box(b, &x, &y, &w, &h);
        switch (code) {
        case 0: out = w; break;
        case 1: out = h; break;
        case 2: out = w - b->border.left - b->border.right; break;
        case 3: out = h - b->border.top - b->border.bottom; break;
        case 4: out = y; break;
        case 5: out = x; break;
        case 6: out = w - b->border.left - b->border.right; break;
        case 7: out = h - b->border.top - b->border.bottom; break;
        }
    }
    info.GetReturnValue().Set(out);
}

void ns_v8_el_bounding_rect_real(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    double x = 0, y = 0, w = 0, h = 0;
    const ns_box *b = js ? ns_v8_box_of(js, n) : NULL;
    if (b) {
        ns_v8_border_box(b, &x, &y, &w, &h);
        x -= js->scroll_x;
        y -= js->scroll_y;
    }
    v8::Local<v8::Object> rect = v8::Object::New(iso);
    struct {
        const char *name;
        double value;
    } fields[] = {{"x", x},      {"y", y},          {"width", w},
                  {"height", h}, {"top", y},        {"left", x},
                  {"right", x + w}, {"bottom", y + h}};
    for (auto &f : fields)
        rect->Set(ctx, ns_v8_str(iso, f.name),
                  v8::Number::New(iso, f.value)).Check();
    info.GetReturnValue().Set(rect);
}

void ns_v8_computed_style_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_js *js = ns_v8_js_here(info);
    info.GetReturnValue().Set(ns_v8_str(iso, ""));
    if (!js || !js->styles || info.Length() < 2) return;
    ns_node *n = ns_v8_node_of(info[0]);
    if (!n) return;
    std::string prop = ns_v8_utf8(iso, info[1]);
    int id = ns_css_prop_id(prop.c_str());
    if (id < 0 || id >= NS_CSS_PROP_COUNT) return;
    ns_v8_flush_layout(js);
    const ns_style *style =
        static_cast<const ns_style *>(g_hash_table_lookup(js->styles, n));
    if (!style || !style->values[id]) return;
    char *v = ns_css_value_serialize(style->values[id]);
    if (v) info.GetReturnValue().Set(ns_v8_str(iso, v));
    g_free(v);
}

v8::Local<v8::Object> ns_v8_raw_response(ns_js *js, ns_response *resp)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Object> raw = v8::Object::New(iso);
    raw->Set(ctx, ns_v8_str(iso, "status"),
             v8::Integer::New(iso, (int)resp->status)).Check();
    raw->Set(ctx, ns_v8_str(iso, "url"),
             ns_v8_str(iso, resp->final_url ? resp->final_url : "")).Check();
    raw->Set(ctx, ns_v8_str(iso, "redirected"),
             v8::Boolean::New(iso, resp->redirect_count > 0)).Check();
    raw->Set(ctx, ns_v8_str(iso, "headersRaw"),
             ns_v8_str(iso, resp->raw_headers ? resp->raw_headers : ""))
        .Check();
    raw->Set(ctx, ns_v8_str(iso, "contentType"),
             ns_v8_str(iso, resp->content_type ? resp->content_type : ""))
        .Check();
    const char *data =
        resp->body ? (const char *)resp->body->data : "";
    guint len = resp->body ? resp->body->len : 0;
    v8::Local<v8::String> text;
    if (!v8::String::NewFromUtf8(iso, data, v8::NewStringType::kNormal,
                                 (int)len)
             .ToLocal(&text))
        text = v8::String::NewFromOneByte(iso, (const uint8_t *)data,
                                          v8::NewStringType::kNormal,
                                          (int)len)
                   .ToLocalChecked();
    raw->Set(ctx, ns_v8_str(iso, "bodyText"), text).Check();
    v8::Local<v8::ArrayBuffer> buf = v8::ArrayBuffer::New(iso, len);
    if (len) memcpy(buf->GetBackingStore()->Data(), data, len);
    raw->Set(ctx, ns_v8_str(iso, "bodyBuffer"), buf).Check();
    return raw;
}

gboolean ns_v8_fetch_allowed(ns_js *js, const char *url, ns_response *resp)
{
    if (!js->partition || !*js->partition) return TRUE;
    char *origin = ns_url_origin_from(url);
    gboolean same = origin && strcmp(origin, js->partition) == 0;
    g_free(origin);
    if (same) return TRUE;
    if (resp && resp->cors_allow_origin) {
        if (strcmp(resp->cors_allow_origin, "*") == 0) return TRUE;
        if (strcmp(resp->cors_allow_origin, js->partition) == 0) return TRUE;
    }
    return FALSE;
}

void ns_v8_fetch_done(GObject *, GAsyncResult *res, gpointer user_data)
{
    ns_v8_request *r = static_cast<ns_v8_request *>(user_data);
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(res, &err);
    ns_js *js = r->js;
    if (!js) {
        if (resp) ns_response_free(resp);
        if (err) g_error_free(err);
        g_free(r->url);
        g_object_unref(r->cancellable);
        delete r;
        return;
    }
    js->pending_requests.erase(std::find(js->pending_requests.begin(),
                                         js->pending_requests.end(), r));
    {
        ns_v8_scope scope(js);
        ns_v8_pump_guard guard(js);
        v8::Isolate *iso = js->isolate;
        v8::Local<v8::Promise::Resolver> resolver = r->resolver.Get(iso);
        if (!resp || resp->error || err) {
            const char *msg = resp && resp->error ? resp->error
                              : err ? err->message
                                    : "network error";
            char *line = g_strdup_printf("fetch failed: %s: %s", r->url, msg);
            v8::Local<v8::Value> ex =
                v8::Exception::TypeError(ns_v8_str(iso, line));
            g_free(line);
            resolver->Reject(scope.ctx, ex).Check();
        } else if (!ns_v8_fetch_allowed(js, r->url, resp)) {
            resolver
                ->Reject(scope.ctx,
                         v8::Exception::TypeError(ns_v8_str(iso,
                             "fetch blocked by CORS policy")))
                .Check();
        } else {
            resolver->Resolve(scope.ctx, ns_v8_raw_response(js, resp))
                .Check();
        }
        ns_v8_settle(js);
    }
    if (resp) ns_response_free(resp);
    if (err) g_error_free(err);
    r->resolver.Reset();
    g_free(r->url);
    g_object_unref(r->cancellable);
    delete r;
}

void ns_v8_fetch_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    ns_js *js = ns_v8_js_here(info);
    if (!js || info.Length() < 1) return;
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(ctx).ToLocal(&resolver)) return;
    info.GetReturnValue().Set(resolver->GetPromise());
    std::string url_in = ns_v8_utf8(iso, info[0]);
    char *url = ns_url_resolve(js->current_url, url_in.c_str());
    if (!url) {
        resolver
            ->Reject(ctx, v8::Exception::TypeError(
                              ns_v8_str(iso, "invalid URL")))
            .Check();
        return;
    }
    std::string method = info.Length() > 1 && info[1]->IsString()
                             ? ns_v8_utf8(iso, info[1])
                             : "GET";
    std::string body;
    gboolean has_body = FALSE;
    if (info.Length() > 2 && info[2]->IsString()) {
        body = ns_v8_utf8(iso, info[2]);
        has_body = TRUE;
    }
    std::string content_type;
    std::vector<std::string> extra;
    if (info.Length() > 3 && info[3]->IsArray()) {
        v8::Local<v8::Array> arr = info[3].As<v8::Array>();
        for (uint32_t i = 0; i + 1 < arr->Length(); i += 2) {
            v8::Local<v8::Value> k, v;
            if (!arr->Get(ctx, i).ToLocal(&k) ||
                !arr->Get(ctx, i + 1).ToLocal(&v))
                continue;
            std::string key = ns_v8_utf8(iso, k);
            std::string value = ns_v8_utf8(iso, v);
            if (g_ascii_strcasecmp(key.c_str(), "content-type") == 0)
                content_type = value;
            else
                extra.push_back(key + ": " + value);
        }
    }
    std::vector<const char *> extra_ptrs;
    for (auto &h : extra) extra_ptrs.push_back(h.c_str());
    extra_ptrs.push_back(nullptr);

    ns_v8_request *r = new ns_v8_request();
    r->js = js;
    r->cancellable = g_cancellable_new();
    r->resolver.Reset(iso, resolver);
    r->url = url;
    js->pending_requests.push_back(r);
    ns_net_request_async(url, js->current_url, method.c_str(),
                         has_body ? body.data() : NULL,
                         has_body ? body.size() : 0,
                         content_type.empty() ? NULL : content_type.c_str(),
                         extra_ptrs.data(), r->cancellable, ns_v8_fetch_done,
                         r);
}

void ns_v8_fetch_sync_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_js *js = ns_v8_js_here(info);
    info.GetReturnValue().SetNull();
    if (!js || info.Length() < 1) return;
    std::string url_in = ns_v8_utf8(iso, info[0]);
    char *url = ns_url_resolve(js->current_url, url_in.c_str());
    if (!url) return;
    std::string method = info.Length() > 1 && info[1]->IsString()
                             ? ns_v8_utf8(iso, info[1])
                             : "GET";
    std::string body;
    gboolean has_body = FALSE;
    if (info.Length() > 2 && info[2]->IsString()) {
        body = ns_v8_utf8(iso, info[2]);
        has_body = TRUE;
    }
    GError *err = NULL;
    ns_response *resp = ns_net_request_blocking(
        url, js->current_url, method.c_str(),
        has_body ? body.data() : NULL, has_body ? body.size() : 0, NULL,
        NULL, NULL, &err);
    if (resp && !resp->error && ns_v8_fetch_allowed(js, url, resp))
        info.GetReturnValue().Set(ns_v8_raw_response(js, resp));
    if (resp) ns_response_free(resp);
    if (err) g_error_free(err);
    g_free(url);
}

ns_v8_canvas *ns_v8_canvas_of(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    if (info.Data().IsEmpty() || !info.Data()->IsExternal()) return nullptr;
    return static_cast<ns_v8_canvas *>(
        info.Data().As<v8::External>()->Value());
}

double ns_v8_arg_num(const v8::FunctionCallbackInfo<v8::Value> &info, int i)
{
    if (i >= info.Length() || !info[i]->IsNumber()) return 0;
    double v = info[i].As<v8::Number>()->Value();
    return v == v ? v : 0;
}

void ns_v8_canvas_touch(ns_v8_canvas *c)
{
    cairo_surface_flush(c->surf);
    ns_v8_mutated(c->js);
}

void ns_v8_canvas_set_source(ns_v8_canvas *c, const double rgba[4])
{
    cairo_set_source_rgba(c->cr, rgba[0], rgba[1], rgba[2],
                          rgba[3] * c->global_alpha);
}

void ns_v8_canvas_fill_rect(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (!c) return;
    ns_v8_canvas_set_source(c, c->fill);
    cairo_rectangle(c->cr, ns_v8_arg_num(info, 0), ns_v8_arg_num(info, 1),
                    ns_v8_arg_num(info, 2), ns_v8_arg_num(info, 3));
    cairo_fill(c->cr);
    ns_v8_canvas_touch(c);
}

void ns_v8_canvas_stroke_rect(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (!c) return;
    ns_v8_canvas_set_source(c, c->stroke);
    cairo_rectangle(c->cr, ns_v8_arg_num(info, 0), ns_v8_arg_num(info, 1),
                    ns_v8_arg_num(info, 2), ns_v8_arg_num(info, 3));
    cairo_stroke(c->cr);
    ns_v8_canvas_touch(c);
}

void ns_v8_canvas_clear_rect(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (!c) return;
    cairo_save(c->cr);
    cairo_set_operator(c->cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(c->cr, ns_v8_arg_num(info, 0), ns_v8_arg_num(info, 1),
                    ns_v8_arg_num(info, 2), ns_v8_arg_num(info, 3));
    cairo_fill(c->cr);
    cairo_restore(c->cr);
    ns_v8_canvas_touch(c);
}

void ns_v8_canvas_begin_path(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (c) cairo_new_path(c->cr);
}

void ns_v8_canvas_close_path(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (c) cairo_close_path(c->cr);
}

void ns_v8_canvas_move_to(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (c)
        cairo_move_to(c->cr, ns_v8_arg_num(info, 0),
                      ns_v8_arg_num(info, 1));
}

void ns_v8_canvas_line_to(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (c)
        cairo_line_to(c->cr, ns_v8_arg_num(info, 0),
                      ns_v8_arg_num(info, 1));
}

void ns_v8_canvas_bezier(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (c)
        cairo_curve_to(c->cr, ns_v8_arg_num(info, 0), ns_v8_arg_num(info, 1),
                       ns_v8_arg_num(info, 2), ns_v8_arg_num(info, 3),
                       ns_v8_arg_num(info, 4), ns_v8_arg_num(info, 5));
}

void ns_v8_canvas_quadratic(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (!c) return;
    double x0, y0;
    if (!cairo_has_current_point(c->cr)) cairo_move_to(c->cr, 0, 0);
    cairo_get_current_point(c->cr, &x0, &y0);
    double cx = ns_v8_arg_num(info, 0), cy = ns_v8_arg_num(info, 1);
    double x = ns_v8_arg_num(info, 2), y = ns_v8_arg_num(info, 3);
    cairo_curve_to(c->cr, x0 + 2.0 / 3.0 * (cx - x0),
                   y0 + 2.0 / 3.0 * (cy - y0), x + 2.0 / 3.0 * (cx - x),
                   y + 2.0 / 3.0 * (cy - y), x, y);
}

void ns_v8_canvas_arc(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (!c) return;
    double x = ns_v8_arg_num(info, 0), y = ns_v8_arg_num(info, 1);
    double r = ns_v8_arg_num(info, 2);
    double a0 = ns_v8_arg_num(info, 3), a1 = ns_v8_arg_num(info, 4);
    gboolean ccw = info.Length() > 5 &&
                   info[5]->BooleanValue(info.GetIsolate());
    if (ccw) cairo_arc_negative(c->cr, x, y, r, a0, a1);
    else cairo_arc(c->cr, x, y, r, a0, a1);
}

void ns_v8_canvas_rect(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (c)
        cairo_rectangle(c->cr, ns_v8_arg_num(info, 0),
                        ns_v8_arg_num(info, 1), ns_v8_arg_num(info, 2),
                        ns_v8_arg_num(info, 3));
}

void ns_v8_canvas_fill(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (!c) return;
    ns_v8_canvas_set_source(c, c->fill);
    cairo_fill_preserve(c->cr);
    ns_v8_canvas_touch(c);
}

void ns_v8_canvas_stroke(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (!c) return;
    ns_v8_canvas_set_source(c, c->stroke);
    cairo_stroke_preserve(c->cr);
    ns_v8_canvas_touch(c);
}

void ns_v8_canvas_save(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (c) cairo_save(c->cr);
}

void ns_v8_canvas_restore(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (c) cairo_restore(c->cr);
}

void ns_v8_canvas_translate(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (c)
        cairo_translate(c->cr, ns_v8_arg_num(info, 0),
                        ns_v8_arg_num(info, 1));
}

void ns_v8_canvas_scale(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (c)
        cairo_scale(c->cr, ns_v8_arg_num(info, 0), ns_v8_arg_num(info, 1));
}

void ns_v8_canvas_rotate(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (c) cairo_rotate(c->cr, ns_v8_arg_num(info, 0));
}

void ns_v8_canvas_set_transform(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (!c) return;
    cairo_matrix_t m;
    cairo_matrix_init(&m, ns_v8_arg_num(info, 0), ns_v8_arg_num(info, 1),
                      ns_v8_arg_num(info, 2), ns_v8_arg_num(info, 3),
                      ns_v8_arg_num(info, 4), ns_v8_arg_num(info, 5));
    cairo_set_matrix(c->cr, &m);
}

void ns_v8_canvas_reset_transform(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (c) cairo_identity_matrix(c->cr);
}

void ns_v8_canvas_fill_text(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (!c || info.Length() < 3) return;
    std::string text = ns_v8_utf8(info.GetIsolate(), info[0]);
    ns_v8_canvas_set_source(c, c->fill);
    cairo_select_font_face(c->cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(c->cr, c->font_size);
    cairo_move_to(c->cr, ns_v8_arg_num(info, 1), ns_v8_arg_num(info, 2));
    cairo_show_text(c->cr, text.c_str());
    ns_v8_canvas_touch(c);
}

void ns_v8_canvas_measure_text(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    v8::Local<v8::Object> out = v8::Object::New(iso);
    double width = 0;
    if (c && info.Length() >= 1) {
        std::string text = ns_v8_utf8(iso, info[0]);
        cairo_select_font_face(c->cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(c->cr, c->font_size);
        cairo_text_extents_t ext;
        cairo_text_extents(c->cr, text.c_str(), &ext);
        width = ext.x_advance;
    }
    out->Set(ctx, ns_v8_str(iso, "width"), v8::Number::New(iso, width))
        .Check();
    info.GetReturnValue().Set(out);
}

void ns_v8_canvas_get_image_data(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (!c) return;
    int sx = (int)ns_v8_arg_num(info, 0), sy = (int)ns_v8_arg_num(info, 1);
    int w = (int)ns_v8_arg_num(info, 2), h = (int)ns_v8_arg_num(info, 3);
    if (w <= 0 || h <= 0) return;
    cairo_surface_flush(c->surf);
    int surf_w = cairo_image_surface_get_width(c->surf);
    int surf_h = cairo_image_surface_get_height(c->surf);
    int stride = cairo_image_surface_get_stride(c->surf);
    const guint8 *pixels = cairo_image_surface_get_data(c->surf);
    v8::Local<v8::ArrayBuffer> buf =
        v8::ArrayBuffer::New(iso, (size_t)w * h * 4);
    guint8 *out = static_cast<guint8 *>(buf->GetBackingStore()->Data());
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            guint8 *dst = out + ((size_t)y * w + x) * 4;
            int px = sx + x, py = sy + y;
            if (px < 0 || py < 0 || px >= surf_w || py >= surf_h) {
                dst[0] = dst[1] = dst[2] = dst[3] = 0;
                continue;
            }
            guint32 v;
            memcpy(&v, pixels + (size_t)py * stride + (size_t)px * 4, 4);
            guint8 a = (guint8)(v >> 24);
            dst[0] = (guint8)(v >> 16);
            dst[1] = (guint8)(v >> 8);
            dst[2] = (guint8)v;
            dst[3] = a;
            if (a && a != 255) {
                dst[0] = (guint8)MIN(255, dst[0] * 255 / a);
                dst[1] = (guint8)MIN(255, dst[1] * 255 / a);
                dst[2] = (guint8)MIN(255, dst[2] * 255 / a);
            }
        }
    }
    v8::Local<v8::Object> data = v8::Object::New(iso);
    data->Set(ctx, ns_v8_str(iso, "width"), v8::Integer::New(iso, w))
        .Check();
    data->Set(ctx, ns_v8_str(iso, "height"), v8::Integer::New(iso, h))
        .Check();
    data->Set(ctx, ns_v8_str(iso, "data"),
              v8::Uint8ClampedArray::New(buf, 0, (size_t)w * h * 4))
        .Check();
    info.GetReturnValue().Set(data);
}

void ns_v8_canvas_style_get(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (!c) return;
    gboolean is_fill = info.Length() == 0 || TRUE;
    (void)is_fill;
    info.GetReturnValue().Set(
        ns_v8_str(info.GetIsolate(), c->fill_style.c_str()));
}

void ns_v8_canvas_set_style(ns_v8_canvas *c, const std::string &s,
                            gboolean is_fill)
{
    guint8 r, g, b, a;
    if (!ns_css_parse_color(s.c_str(), &r, &g, &b, &a)) return;
    double *rgba = is_fill ? c->fill : c->stroke;
    rgba[0] = r / 255.0;
    rgba[1] = g / 255.0;
    rgba[2] = b / 255.0;
    rgba[3] = a / 255.0;
    if (is_fill) c->fill_style = s;
    else c->stroke_style = s;
}

void ns_v8_canvas_fill_style_set(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (!c || info.Length() < 1 || !info[0]->IsString()) return;
    ns_v8_canvas_set_style(c, ns_v8_utf8(info.GetIsolate(), info[0]), TRUE);
}

void ns_v8_canvas_stroke_style_set(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (!c || info.Length() < 1 || !info[0]->IsString()) return;
    ns_v8_canvas_set_style(c, ns_v8_utf8(info.GetIsolate(), info[0]),
                           FALSE);
}

void ns_v8_canvas_line_width_set(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    double w = ns_v8_arg_num(info, 0);
    if (c && w > 0) cairo_set_line_width(c->cr, w);
}

void ns_v8_canvas_global_alpha_set(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    double a = ns_v8_arg_num(info, 0);
    if (c && a >= 0 && a <= 1) c->global_alpha = a;
}

void ns_v8_canvas_font_set(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_canvas *c = ns_v8_canvas_of(info);
    if (!c || info.Length() < 1) return;
    std::string f = ns_v8_utf8(info.GetIsolate(), info[0]);
    double size = 0;
    if (sscanf(f.c_str(), "%lf", &size) == 1 && size > 0)
        c->font_size = size;
    const char *px = strstr(f.c_str(), "px");
    if (px) {
        const char *p = f.c_str();
        while (p < px && !g_ascii_isdigit(*p)) p++;
        double s = g_ascii_strtod(p, NULL);
        if (s > 0) c->font_size = s;
    }
}

void ns_v8_canvas_noop(const v8::FunctionCallbackInfo<v8::Value> &)
{
}

void ns_v8_canvas_bind(ns_js *js, v8::Local<v8::Object> obj,
                       const char *name, v8::FunctionCallback cb,
                       ns_v8_canvas *c)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Function> fn =
        v8::Function::New(ctx, cb, v8::External::New(iso, c))
            .ToLocalChecked();
    obj->Set(ctx, ns_v8_str(iso, name), fn).Check();
}

ns_v8_canvas *ns_v8_canvas_state(ns_js *js, ns_node *n)
{
    auto it = js->canvases.find(n);
    if (it != js->canvases.end()) return it->second;
    int w = 300, h = 150;
    const char *ws = ns_element_get_attr(n, "width");
    const char *hs = ns_element_get_attr(n, "height");
    if (ws && atoi(ws) > 0) w = atoi(ws);
    if (hs && atoi(hs) > 0) h = atoi(hs);
    w = CLAMP(w, 1, 8192);
    h = CLAMP(h, 1, 8192);
    ns_v8_canvas *c = new ns_v8_canvas();
    c->js = js;
    c->node = n;
    c->surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    c->cr = cairo_create(c->surf);
    c->fill[0] = c->fill[1] = c->fill[2] = 0;
    c->fill[3] = 1;
    c->stroke[0] = c->stroke[1] = c->stroke[2] = 0;
    c->stroke[3] = 1;
    c->fill_style = "#000000";
    c->stroke_style = "#000000";
    c->global_alpha = 1;
    c->font_size = 10;
    js->canvases[n] = c;
    return c;
}

ns_node *ns_v8_find_shadow_child(ns_node *host)
{
    for (ns_node *c = host->first_child; c; c = c->next_sibling)
        if (c->kind == NS_NODE_ELEMENT &&
            ns_element_get_attr(c, NS_SHADOW_ATTR))
            return c;
    return NULL;
}

v8::Local<v8::Value> ns_v8_shadow_wrap(ns_js *js, ns_node *root,
                                       ns_node *host)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Value> wv = ns_v8_wrap_node(js, root);
    if (wv->IsObject()) {
        v8::Local<v8::Object> w = wv.As<v8::Object>();
        const char *mode = ns_element_get_attr(root, NS_SHADOW_ATTR);
        w->Set(ctx, ns_v8_str(iso, "host"), ns_v8_wrap_node(js, host))
            .Check();
        w->Set(ctx, ns_v8_str(iso, "mode"),
               ns_v8_str(iso, mode ? mode : "open")).Check();
    }
    return wv;
}

void ns_v8_el_attach_shadow(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    ns_js *js = ns_v8_js_here(info);
    ns_node *host = ns_v8_self(info);
    if (!js || !host || host->kind != NS_NODE_ELEMENT) {
        iso->ThrowException(v8::Exception::TypeError(
            ns_v8_str(iso, "attachShadow requires an Element host")));
        return;
    }
    std::string mode = "open";
    if (info.Length() >= 1 && info[0]->IsObject()) {
        v8::Local<v8::Value> mv;
        if (info[0].As<v8::Object>()
                ->Get(ctx, ns_v8_str(iso, "mode"))
                .ToLocal(&mv))
            mode = ns_v8_utf8(iso, mv);
    }
    if (mode != "open" && mode != "closed") {
        iso->ThrowException(v8::Exception::TypeError(
            ns_v8_str(iso, "attachShadow: mode must be 'open' or 'closed'")));
        return;
    }
    ns_node *existing = ns_v8_find_shadow_child(host);
    if (existing) {
        const char *emode = ns_element_get_attr(existing, NS_SHADOW_ATTR);
        gboolean declarative =
            ns_element_get_attr(existing, "data-nd-shadow-declarative") !=
            NULL;
        if (!declarative || !emode || mode != emode) {
            iso->ThrowException(v8::Exception::Error(ns_v8_str(iso,
                "attachShadow: the element already hosts a shadow root")));
            return;
        }
        ns_v8_clear_children(js, existing);
        ns_element_remove_attr(existing, "data-nd-shadow-declarative");
        ns_v8_mutated(js);
        info.GetReturnValue().Set(ns_v8_shadow_wrap(js, existing, host));
        return;
    }
    ns_node *root = ns_node_new_element(g_strdup("div"));
    ns_element_set_attr(root, NS_SHADOW_ATTR, mode.c_str());
    ns_node_append_child(host, root);
    ns_v8_note_inserted(js, root);
    info.GetReturnValue().Set(ns_v8_shadow_wrap(js, root, host));
}

void ns_v8_el_shadow_root_get(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    info.GetReturnValue().SetNull();
    if (!js || !n || n->kind != NS_NODE_ELEMENT) return;
    ns_node *root = ns_v8_find_shadow_child(n);
    if (!root) return;
    const char *mode = ns_element_get_attr(root, NS_SHADOW_ATTR);
    if (mode && strcmp(mode, "open") == 0)
        info.GetReturnValue().Set(ns_v8_shadow_wrap(js, root, n));
}

void ns_v8_el_get_context(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    info.GetReturnValue().SetNull();
    if (!js || !n || !ns_node_is_element_named(n, "canvas")) return;
    std::string kind = info.Length() >= 1 ? ns_v8_utf8(iso, info[0]) : "2d";
    if (kind != "2d") return;
    ns_v8_canvas *c = ns_v8_canvas_state(js, n);
    if (!c->ctx_obj.IsEmpty()) {
        info.GetReturnValue().Set(c->ctx_obj.Get(iso));
        return;
    }
    v8::Local<v8::Object> obj = v8::Object::New(iso);
    struct {
        const char *name;
        v8::FunctionCallback cb;
    } fns[] = {
        {"fillRect", ns_v8_canvas_fill_rect},
        {"strokeRect", ns_v8_canvas_stroke_rect},
        {"clearRect", ns_v8_canvas_clear_rect},
        {"beginPath", ns_v8_canvas_begin_path},
        {"closePath", ns_v8_canvas_close_path},
        {"moveTo", ns_v8_canvas_move_to},
        {"lineTo", ns_v8_canvas_line_to},
        {"bezierCurveTo", ns_v8_canvas_bezier},
        {"quadraticCurveTo", ns_v8_canvas_quadratic},
        {"arc", ns_v8_canvas_arc},
        {"rect", ns_v8_canvas_rect},
        {"fill", ns_v8_canvas_fill},
        {"stroke", ns_v8_canvas_stroke},
        {"save", ns_v8_canvas_save},
        {"restore", ns_v8_canvas_restore},
        {"translate", ns_v8_canvas_translate},
        {"scale", ns_v8_canvas_scale},
        {"rotate", ns_v8_canvas_rotate},
        {"setTransform", ns_v8_canvas_set_transform},
        {"resetTransform", ns_v8_canvas_reset_transform},
        {"fillText", ns_v8_canvas_fill_text},
        {"strokeText", ns_v8_canvas_fill_text},
        {"measureText", ns_v8_canvas_measure_text},
        {"getImageData", ns_v8_canvas_get_image_data},
        {"clip", ns_v8_canvas_noop},
        {"drawImage", ns_v8_canvas_noop},
        {"putImageData", ns_v8_canvas_noop},
        {"createLinearGradient", ns_v8_canvas_noop},
        {"createRadialGradient", ns_v8_canvas_noop},
        {"createPattern", ns_v8_canvas_noop},
        {"setLineDash", ns_v8_canvas_noop},
    };
    for (auto &f : fns) ns_v8_canvas_bind(js, obj, f.name, f.cb, c);
    struct {
        const char *name;
        v8::FunctionCallback setter;
    } props[] = {
        {"fillStyle", ns_v8_canvas_fill_style_set},
        {"strokeStyle", ns_v8_canvas_stroke_style_set},
        {"lineWidth", ns_v8_canvas_line_width_set},
        {"globalAlpha", ns_v8_canvas_global_alpha_set},
        {"font", ns_v8_canvas_font_set},
    };
    for (auto &p : props) {
        v8::Local<v8::Function> getter =
            v8::Function::New(ctx, ns_v8_canvas_style_get,
                              v8::External::New(iso, c))
                .ToLocalChecked();
        v8::Local<v8::Function> setter =
            v8::Function::New(ctx, p.setter, v8::External::New(iso, c))
                .ToLocalChecked();
        obj->SetAccessorProperty(ns_v8_str(iso, p.name), getter, setter);
    }
    obj->Set(ctx, ns_v8_str(iso, "canvas"), info.This()).Check();
    c->ctx_obj.Reset(iso, obj);
    info.GetReturnValue().Set(obj);
}

void ns_v8_el_dim_get(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    gboolean is_width =
        (intptr_t)info.Data().As<v8::External>()->Value() != 0;
    int fallback = is_width ? 300 : 150;
    if (!n) {
        info.GetReturnValue().Set(0);
        return;
    }
    if (js && ns_node_is_element_named(n, "canvas")) {
        auto it = js->canvases.find(n);
        if (it != js->canvases.end()) {
            info.GetReturnValue().Set(
                is_width ? cairo_image_surface_get_width(it->second->surf)
                         : cairo_image_surface_get_height(it->second->surf));
            return;
        }
    }
    const char *attr = ns_element_get_attr(n, is_width ? "width" : "height");
    info.GetReturnValue().Set(attr && atoi(attr) > 0 ? atoi(attr)
                              : ns_node_is_element_named(n, "canvas")
                                  ? fallback
                                  : 0);
}

void ns_v8_el_dim_set(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    if (!js || !n || info.Length() < 1) return;
    gboolean is_width =
        (intptr_t)info.Data().As<v8::External>()->Value() != 0;
    int v = (int)ns_v8_arg_num(info, 0);
    char buf[32];
    g_snprintf(buf, sizeof buf, "%d", v);
    ns_v8_set_attr_indexed(js, n, is_width ? "width" : "height", buf);
    auto it = js->canvases.find(n);
    if (it != js->canvases.end()) {
        ns_v8_canvas *c = it->second;
        int w = is_width ? CLAMP(v, 1, 8192)
                         : cairo_image_surface_get_width(c->surf);
        int h = is_width ? cairo_image_surface_get_height(c->surf)
                         : CLAMP(v, 1, 8192);
        cairo_destroy(c->cr);
        cairo_surface_destroy(c->surf);
        c->surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        c->cr = cairo_create(c->surf);
        ns_v8_mutated(js);
    }
}

void ns_v8_el_to_data_url(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_js *js = ns_v8_js_here(info);
    ns_node *n = ns_v8_self(info);
    info.GetReturnValue().Set(ns_v8_str(iso, "data:,"));
    if (!js || !n) return;
    auto it = js->canvases.find(n);
    if (it == js->canvases.end()) return;
    cairo_surface_flush(it->second->surf);
    GByteArray *png = g_byte_array_new();
    cairo_surface_write_to_png_stream(
        it->second->surf,
        [](void *closure, const unsigned char *data,
           unsigned int length) -> cairo_status_t {
            g_byte_array_append(static_cast<GByteArray *>(closure), data,
                                length);
            return CAIRO_STATUS_SUCCESS;
        },
        png);
    char *b64 = g_base64_encode(png->data, png->len);
    char *url = g_strdup_printf("data:image/png;base64,%s", b64);
    info.GetReturnValue().Set(ns_v8_str(iso, url));
    g_free(url);
    g_free(b64);
    g_byte_array_free(png, TRUE);
}

gboolean ns_v8_bytes_of(v8::Local<v8::Value> v, const guint8 **data,
                        gsize *len)
{
    if (v.IsEmpty()) return FALSE;
    if (v->IsArrayBuffer()) {
        v8::Local<v8::ArrayBuffer> ab = v.As<v8::ArrayBuffer>();
        *data = static_cast<const guint8 *>(ab->GetBackingStore()->Data());
        *len = ab->ByteLength();
        return TRUE;
    }
    if (v->IsArrayBufferView()) {
        v8::Local<v8::ArrayBufferView> view = v.As<v8::ArrayBufferView>();
        *data = static_cast<const guint8 *>(
                    view->Buffer()->GetBackingStore()->Data()) +
                view->ByteOffset();
        *len = view->ByteLength();
        return TRUE;
    }
    return FALSE;
}

void ns_v8_crypto_get_random_values(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    if (info.Length() < 1 || !info[0]->IsArrayBufferView()) return;
    v8::Local<v8::ArrayBufferView> view =
        info[0].As<v8::ArrayBufferView>();
    guint8 *data = static_cast<guint8 *>(
                       view->Buffer()->GetBackingStore()->Data()) +
                   view->ByteOffset();
    gsize len = view->ByteLength();
    if (len > 0 && len <= 65536) RAND_bytes(data, (int)len);
    info.GetReturnValue().Set(info[0]);
}

void ns_v8_crypto_random_uuid(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    guint8 b[16];
    RAND_bytes(b, sizeof b);
    b[6] = (guint8)((b[6] & 0x0f) | 0x40);
    b[8] = (guint8)((b[8] & 0x3f) | 0x80);
    char *uuid = g_strdup_printf(
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
        b[11], b[12], b[13], b[14], b[15]);
    info.GetReturnValue().Set(ns_v8_str(info.GetIsolate(), uuid));
    g_free(uuid);
}

void ns_v8_crypto_digest(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(ctx).ToLocal(&resolver)) return;
    info.GetReturnValue().Set(resolver->GetPromise());
    std::string algo;
    if (info.Length() >= 1) {
        if (info[0]->IsString()) {
            algo = ns_v8_utf8(iso, info[0]);
        } else if (info[0]->IsObject()) {
            v8::Local<v8::Value> nv;
            if (info[0].As<v8::Object>()
                    ->Get(ctx, ns_v8_str(iso, "name"))
                    .ToLocal(&nv))
                algo = ns_v8_utf8(iso, nv);
        }
    }
    const guint8 *data = NULL;
    gsize len = 0;
    if (algo.empty() || info.Length() < 2 ||
        !ns_v8_bytes_of(info[1], &data, &len)) {
        resolver
            ->Reject(ctx, v8::Exception::TypeError(ns_v8_str(iso,
                         "digest: expected algorithm and BufferSource")))
            .Check();
        return;
    }
    gsize out_len = 0;
    guint8 *out = ns_crypto_digest(algo.c_str(), data, len, &out_len);
    if (!out) {
        resolver
            ->Reject(ctx, v8::Exception::Error(ns_v8_str(iso,
                         "digest: unsupported algorithm")))
            .Check();
        return;
    }
    v8::Local<v8::ArrayBuffer> buf = v8::ArrayBuffer::New(iso, out_len);
    memcpy(buf->GetBackingStore()->Data(), out, out_len);
    g_free(out);
    resolver->Resolve(ctx, buf).Check();
}

void ns_v8_bind_fn(ns_js *js, v8::Local<v8::Object> obj, const char *name,
                   v8::FunctionCallback cb);

struct ns_v8_worker_timer {
    int id;
    gint64 due_us;
    v8::Global<v8::Function> fn;
};

struct ns_v8_worker_rt {
    ns_v8_worker *w;
    v8::Isolate *iso;
    std::vector<ns_v8_worker_timer> timers;
    int next_timer_id;
};

void ns_v8_worker_emit(ns_v8_worker *w, const char *prefix,
                       const char *payload)
{
    g_async_queue_push(w->outbox, g_strdup_printf("%s%s", prefix, payload));
}

gboolean ns_v8_worker_deliver_idle(gpointer data);

void ns_v8_worker_schedule_deliver(ns_v8_worker *w)
{
    if (g_atomic_int_get((gint *)&w->pending_idle)) return;
    g_atomic_int_set((gint *)&w->pending_idle, 1);
    g_idle_add(ns_v8_worker_deliver_idle, w);
}

void ns_v8_worker_post_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_v8_worker *w = static_cast<ns_v8_worker *>(
        info.Data().As<v8::External>()->Value());
    if (info.Length() < 1) return;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::String> json;
    if (!v8::JSON::Stringify(ctx, info[0]).ToLocal(&json)) return;
    ns_v8_worker_emit(w, "m", ns_v8_utf8(iso, json).c_str());
    ns_v8_worker_schedule_deliver(w);
}

void ns_v8_worker_log_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_v8_worker *w = static_cast<ns_v8_worker *>(
        info.Data().As<v8::External>()->Value());
    GString *line = g_string_new("[worker] ");
    for (int i = 0; i < info.Length(); i++) {
        if (i) g_string_append_c(line, ' ');
        g_string_append(line, ns_v8_utf8(iso, info[i]).c_str());
    }
    ns_v8_worker_emit(w, "l", line->str);
    g_string_free(line, TRUE);
    ns_v8_worker_schedule_deliver(w);
}

void ns_v8_worker_close_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_worker *w = static_cast<ns_v8_worker *>(
        info.Data().As<v8::External>()->Value());
    g_atomic_int_set(&w->terminate, 1);
}

void ns_v8_worker_import_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_v8_worker *w = static_cast<ns_v8_worker *>(
        info.Data().As<v8::External>()->Value());
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    for (int i = 0; i < info.Length(); i++) {
        std::string spec = ns_v8_utf8(iso, info[i]);
        char *url = ns_url_resolve(w->url, spec.c_str());
        if (!url) continue;
        ns_response *resp = ns_net_fetch_blocking(url, NULL, NULL);
        if (resp && resp->status < 400 && resp->body && resp->body->len) {
            v8::TryCatch tc(iso);
            v8::Local<v8::String> code;
            v8::Local<v8::Script> script;
            v8::Local<v8::Value> r;
            if (v8::String::NewFromUtf8(iso, (const char *)resp->body->data,
                                        v8::NewStringType::kNormal,
                                        (int)resp->body->len)
                    .ToLocal(&code) &&
                v8::Script::Compile(ctx, code).ToLocal(&script))
                (void)!script->Run(ctx).ToLocal(&r);
            if (tc.HasCaught())
                ns_v8_worker_emit(w, "l", "[worker] importScripts error");
        }
        if (resp) ns_response_free(resp);
        g_free(url);
    }
}

void ns_v8_worker_set_timeout_cb(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_v8_worker_rt *rt = static_cast<ns_v8_worker_rt *>(
        info.Data().As<v8::External>()->Value());
    if (info.Length() < 1 || !info[0]->IsFunction()) return;
    double ms = info.Length() > 1 && info[1]->IsNumber()
                    ? info[1].As<v8::Number>()->Value()
                    : 0;
    if (ms < 0 || ms != ms) ms = 0;
    ns_v8_worker_timer t;
    t.id = rt->next_timer_id++;
    t.due_us = g_get_monotonic_time() + (gint64)(ms * 1000);
    t.fn.Reset(iso, info[0].As<v8::Function>());
    rt->timers.push_back(std::move(t));
    info.GetReturnValue().Set(rt->timers.back().id);
}

void ns_v8_worker_clear_timeout_cb(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_worker_rt *rt = static_cast<ns_v8_worker_rt *>(
        info.Data().As<v8::External>()->Value());
    if (info.Length() < 1 || !info[0]->IsNumber()) return;
    int id = (int)info[0].As<v8::Number>()->Value();
    for (auto it = rt->timers.begin(); it != rt->timers.end(); ++it)
        if (it->id == id) {
            rt->timers.erase(it);
            return;
        }
}

void ns_v8_worker_dispatch_message(ns_v8_worker_rt *rt, const char *json)
{
    v8::Isolate *iso = rt->iso;
    v8::HandleScope hs(iso);
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Value> handler;
    if (!ctx->Global()
             ->Get(ctx, v8::String::NewFromUtf8Literal(iso, "onmessage"))
             .ToLocal(&handler) ||
        !handler->IsFunction())
        return;
    v8::Local<v8::Value> data;
    v8::Local<v8::String> json_str;
    if (!v8::String::NewFromUtf8(iso, json).ToLocal(&json_str) ||
        !v8::JSON::Parse(ctx, json_str).ToLocal(&data))
        return;
    v8::Local<v8::Object> ev = v8::Object::New(iso);
    ev->Set(ctx, v8::String::NewFromUtf8Literal(iso, "data"), data).Check();
    ev->Set(ctx, v8::String::NewFromUtf8Literal(iso, "type"),
            v8::String::NewFromUtf8Literal(iso, "message")).Check();
    v8::TryCatch tc(iso);
    v8::Local<v8::Value> arg = ev;
    v8::Local<v8::Value> r;
    if (!handler.As<v8::Function>()
             ->Call(ctx, ctx->Global(), 1, &arg)
             .ToLocal(&r)) {
        std::string msg = ns_v8_utf8(iso, tc.Exception());
        char *line = g_strdup_printf("[worker] onmessage error: %s",
                                     msg.c_str());
        ns_v8_worker_emit(rt->w, "l", line);
        g_free(line);
        ns_v8_worker_schedule_deliver(rt->w);
    }
    iso->PerformMicrotaskCheckpoint();
}

gpointer ns_v8_worker_thread(gpointer data)
{
    ns_v8_worker *w = static_cast<ns_v8_worker *>(data);
    ns_v8_worker_rt rt;
    rt.w = w;
    rt.next_timer_id = 1;
    v8::ArrayBuffer::Allocator *allocator =
        v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = allocator;
    v8::Isolate *iso = v8::Isolate::New(params);
    rt.iso = iso;
    {
        v8::Isolate::Scope iso_scope(iso);
        v8::HandleScope hs(iso);
        v8::Local<v8::Context> ctx = v8::Context::New(iso);
        v8::Context::Scope ctx_scope(ctx);
        v8::Local<v8::Object> global = ctx->Global();
        v8::Local<v8::External> wext = v8::External::New(iso, w);
        v8::Local<v8::External> rtext = v8::External::New(iso, &rt);
        struct {
            const char *name;
            v8::FunctionCallback cb;
            v8::Local<v8::External> data;
        } fns[] = {
            {"postMessage", ns_v8_worker_post_cb, wext},
            {"close", ns_v8_worker_close_cb, wext},
            {"importScripts", ns_v8_worker_import_cb, wext},
            {"setTimeout", ns_v8_worker_set_timeout_cb, rtext},
            {"clearTimeout", ns_v8_worker_clear_timeout_cb, rtext},
        };
        for (auto &f : fns) {
            v8::Local<v8::Function> fn =
                v8::Function::New(ctx, f.cb, f.data).ToLocalChecked();
            global
                ->Set(ctx,
                      v8::String::NewFromUtf8(iso, f.name).ToLocalChecked(),
                      fn)
                .Check();
        }
        global->Set(ctx, v8::String::NewFromUtf8Literal(iso, "self"),
                    global).Check();
        v8::Local<v8::Object> console = v8::Object::New(iso);
        const char *lvls[] = {"log", "info", "warn", "error", "debug"};
        for (auto *lvl : lvls) {
            v8::Local<v8::Function> fn =
                v8::Function::New(ctx, ns_v8_worker_log_cb, wext)
                    .ToLocalChecked();
            console
                ->Set(ctx,
                      v8::String::NewFromUtf8(iso, lvl).ToLocalChecked(),
                      fn)
                .Check();
        }
        global->Set(ctx, v8::String::NewFromUtf8Literal(iso, "console"),
                    console).Check();
        {
            v8::TryCatch tc(iso);
            v8::Local<v8::String> code;
            v8::Local<v8::Script> script;
            v8::Local<v8::Value> r;
            if (!v8::String::NewFromUtf8(iso, w->source ? w->source : "")
                     .ToLocal(&code) ||
                !v8::Script::Compile(ctx, code).ToLocal(&script) ||
                !script->Run(ctx).ToLocal(&r)) {
                std::string msg = tc.HasCaught()
                                      ? ns_v8_utf8(iso, tc.Exception())
                                      : "script failed";
                char *line = g_strdup_printf("[worker] %s: %s", w->url,
                                             msg.c_str());
                ns_v8_worker_emit(w, "e", line);
                g_free(line);
                ns_v8_worker_schedule_deliver(w);
            }
            iso->PerformMicrotaskCheckpoint();
        }
        while (!g_atomic_int_get(&w->terminate)) {
            char *msg = static_cast<char *>(
                g_async_queue_timeout_pop(w->inbox, 20000));
            if (msg) {
                if (strcmp(msg, "\x04") != 0)
                    ns_v8_worker_dispatch_message(&rt, msg);
                g_free(msg);
            }
            gint64 now = g_get_monotonic_time();
            for (size_t i = 0; i < rt.timers.size();) {
                if (rt.timers[i].due_us <= now) {
                    v8::HandleScope ths(iso);
                    v8::Local<v8::Function> fn = rt.timers[i].fn.Get(iso);
                    rt.timers.erase(rt.timers.begin() + (long)i);
                    v8::TryCatch tc(iso);
                    v8::Local<v8::Value> r;
                    v8::Local<v8::Context> cur =
                        iso->GetCurrentContext();
                    if (!fn->Call(cur, cur->Global(), 0, nullptr)
                             .ToLocal(&r))
                        tc.Reset();
                    iso->PerformMicrotaskCheckpoint();
                } else {
                    i++;
                }
            }
            while (v8::platform::PumpMessageLoop(g_v8_platform.get(),
                                                 iso)) {}
        }
        for (auto &t : rt.timers) t.fn.Reset();
        rt.timers.clear();
    }
    iso->Dispose();
    delete allocator;
    return NULL;
}

gboolean ns_v8_worker_deliver_idle(gpointer data)
{
    ns_v8_worker *w = static_cast<ns_v8_worker *>(data);
    g_atomic_int_set((gint *)&w->pending_idle, 0);
    ns_js *js = w->owner;
    char *msg;
    while ((msg = static_cast<char *>(g_async_queue_try_pop(w->outbox)))) {
        if (!js) {
            g_free(msg);
            continue;
        }
        char kind = msg[0];
        const char *payload = msg + 1;
        if (kind == 'l' || kind == 'e') {
            ns_v8_log(js, payload);
        } else if (kind == 'm' && !js->pump_depth) {
            ns_v8_scope scope(js);
            ns_v8_pump_guard guard(js);
            v8::Isolate *iso = js->isolate;
            v8::Local<v8::Object> obj = w->js_obj.Get(iso);
            v8::Local<v8::Value> handler;
            if (obj->Get(scope.ctx, ns_v8_str(iso, "onmessage"))
                    .ToLocal(&handler) &&
                handler->IsFunction()) {
                v8::Local<v8::Value> parsed;
                if (v8::JSON::Parse(scope.ctx, ns_v8_str(iso, payload))
                        .ToLocal(&parsed)) {
                    v8::Local<v8::Object> ev = v8::Object::New(iso);
                    ev->Set(scope.ctx, ns_v8_str(iso, "data"), parsed)
                        .Check();
                    ev->Set(scope.ctx, ns_v8_str(iso, "type"),
                            ns_v8_str(iso, "message")).Check();
                    v8::Local<v8::Value> arg = ev;
                    ns_v8_call_function(js, handler.As<v8::Function>(), 1,
                                        &arg, "worker-message");
                }
            }
        }
        g_free(msg);
    }
    return G_SOURCE_REMOVE;
}

void ns_v8_worker_free(ns_v8_worker *w)
{
    g_atomic_int_set(&w->terminate, 1);
    g_async_queue_push(w->inbox, g_strdup("\x04"));
    if (w->thread) g_thread_join(w->thread);
    while (g_idle_remove_by_data(w)) {}
    char *m;
    while ((m = static_cast<char *>(g_async_queue_try_pop(w->inbox))))
        g_free(m);
    while ((m = static_cast<char *>(g_async_queue_try_pop(w->outbox))))
        g_free(m);
    g_async_queue_unref(w->inbox);
    g_async_queue_unref(w->outbox);
    w->js_obj.Reset();
    g_free(w->url);
    g_free(w->source);
    delete w;
}

void ns_v8_worker_obj_post(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_v8_worker *w = static_cast<ns_v8_worker *>(
        info.Data().As<v8::External>()->Value());
    if (info.Length() < 1) return;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::String> json;
    if (!v8::JSON::Stringify(ctx, info[0]).ToLocal(&json)) return;
    g_async_queue_push(w->inbox,
                       g_strdup(ns_v8_utf8(iso, json).c_str()));
}

void ns_v8_worker_obj_terminate(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_worker *w = static_cast<ns_v8_worker *>(
        info.Data().As<v8::External>()->Value());
    g_atomic_int_set(&w->terminate, 1);
    g_async_queue_push(w->inbox, g_strdup("\x04"));
}

void ns_v8_worker_ctor_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    ns_js *js = ns_v8_js_of(iso);
    if (!js || !info.IsConstructCall() || info.Length() < 1) {
        iso->ThrowException(v8::Exception::TypeError(
            ns_v8_str(iso, "Worker requires a script URL")));
        return;
    }
    std::string spec = ns_v8_utf8(iso, info[0]);
    char *url = ns_url_resolve(js->current_url, spec.c_str());
    if (!url) {
        iso->ThrowException(v8::Exception::TypeError(
            ns_v8_str(iso, "Worker: cannot resolve script URL")));
        return;
    }
    ns_response *resp = ns_net_fetch_blocking(url, NULL, NULL);
    if (!resp || resp->status >= 400 || !resp->body || !resp->body->len) {
        char *line = g_strdup_printf("Worker: failed to fetch %s", url);
        iso->ThrowException(
            v8::Exception::Error(ns_v8_str(iso, line)));
        g_free(line);
        g_free(url);
        if (resp) ns_response_free(resp);
        return;
    }
    ns_v8_worker *w = new ns_v8_worker();
    w->owner = js;
    w->inbox = g_async_queue_new();
    w->outbox = g_async_queue_new();
    w->terminate = 0;
    w->pending_idle = 0;
    w->url = url;
    w->source = g_strndup((const char *)resp->body->data, resp->body->len);
    ns_response_free(resp);
    v8::Local<v8::Object> obj = info.This();
    v8::Local<v8::External> wext = v8::External::New(iso, w);
    obj->Set(ctx, ns_v8_str(iso, "postMessage"),
             v8::Function::New(ctx, ns_v8_worker_obj_post, wext)
                 .ToLocalChecked()).Check();
    obj->Set(ctx, ns_v8_str(iso, "terminate"),
             v8::Function::New(ctx, ns_v8_worker_obj_terminate, wext)
                 .ToLocalChecked()).Check();
    w->js_obj.Reset(iso, obj);
    js->workers.push_back(w);
    w->thread = g_thread_new("ns-v8-worker", ns_v8_worker_thread, w);
}

char *ns_v8_storage_path_for_origin(const char *origin)
{
    if (!origin || !*origin || strcmp(origin, "null") == 0) return NULL;
    const ns_config *cfg = ns_config_get();
    if (cfg && cfg->private_mode) return NULL;
    g_autofree char *hash =
        g_compute_checksum_for_string(G_CHECKSUM_SHA256, origin, -1);
    g_autofree char *dir = g_build_filename(g_get_user_data_dir(),
                                            NS_APP_DIR_NAME, "localstorage",
                                            NULL);
    g_mkdir_with_parents(dir, 0700);
    g_chmod(dir, 0700);
    g_autofree char *file = g_strdup_printf("%s.ini", hash);
    return g_build_filename(dir, file, NULL);
}

void ns_v8_storage_flush(ns_js *js)
{
    if (js->local_storage_flush_source) {
        g_source_remove(js->local_storage_flush_source);
        js->local_storage_flush_source = 0;
    }
    if (!js->local_storage_dirty || !js->local_storage_path ||
        !js->local_storage)
        return;
    GKeyFile *kf = g_key_file_new();
    if (js->partition)
        g_key_file_set_string(kf, "meta", "origin", js->partition);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, js->local_storage);
    while (g_hash_table_iter_next(&it, &k, &v))
        g_key_file_set_string(kf, "storage", (const char *)k,
                              (const char *)v);
    gsize len = 0;
    char *data = g_key_file_to_data(kf, &len, NULL);
    if (data) {
        g_file_set_contents(js->local_storage_path, data, (gssize)len,
                            NULL);
        g_chmod(js->local_storage_path, 0600);
        g_free(data);
    }
    g_key_file_free(kf);
    js->local_storage_dirty = FALSE;
}

gboolean ns_v8_storage_flush_timer(gpointer data)
{
    ns_js *js = static_cast<ns_js *>(data);
    js->local_storage_flush_source = 0;
    ns_v8_storage_flush(js);
    return G_SOURCE_REMOVE;
}

void ns_v8_storage_mark_dirty(ns_js *js)
{
    js->local_storage_dirty = TRUE;
    if (!js->local_storage_flush_source)
        js->local_storage_flush_source =
            g_timeout_add(300, ns_v8_storage_flush_timer, js);
}

void ns_v8_storage_open(ns_js *js)
{
    if (js->local_storage) {
        ns_v8_storage_flush(js);
        g_hash_table_destroy(js->local_storage);
    }
    g_free(js->local_storage_path);
    js->local_storage =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    js->local_storage_path = ns_v8_storage_path_for_origin(js->partition);
    js->local_storage_dirty = FALSE;
    if (!js->local_storage_path) return;
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, js->local_storage_path,
                                  G_KEY_FILE_NONE, NULL)) {
        gsize n = 0;
        char **keys = g_key_file_get_keys(kf, "storage", &n, NULL);
        for (gsize i = 0; keys && i < n; i++) {
            char *value =
                g_key_file_get_string(kf, "storage", keys[i], NULL);
            if (value)
                g_hash_table_insert(js->local_storage, g_strdup(keys[i]),
                                    value);
        }
        g_strfreev(keys);
    }
    g_key_file_free(kf);
}

void ns_v8_storage_get_all_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    ns_js *js = ns_v8_js_here(info);
    v8::Local<v8::Object> out = v8::Object::New(iso);
    if (js && js->local_storage) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, js->local_storage);
        while (g_hash_table_iter_next(&it, &k, &v))
            out->Set(ctx, ns_v8_str(iso, (const char *)k),
                     ns_v8_str(iso, (const char *)v)).Check();
    }
    info.GetReturnValue().Set(out);
}

void ns_v8_storage_set_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    if (!js || !js->local_storage || info.Length() < 2) return;
    v8::Isolate *iso = info.GetIsolate();
    std::string k = ns_v8_utf8(iso, info[0]);
    std::string v = ns_v8_utf8(iso, info[1]);
    g_hash_table_insert(js->local_storage, g_strdup(k.c_str()),
                        g_strdup(v.c_str()));
    ns_v8_storage_mark_dirty(js);
}

void ns_v8_storage_remove_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    if (!js || !js->local_storage || info.Length() < 1) return;
    std::string k = ns_v8_utf8(info.GetIsolate(), info[0]);
    if (g_hash_table_remove(js->local_storage, k.c_str()))
        ns_v8_storage_mark_dirty(js);
}

void ns_v8_storage_clear_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    if (!js || !js->local_storage) return;
    g_hash_table_remove_all(js->local_storage);
    ns_v8_storage_mark_dirty(js);
}

void ns_v8_resolve_url_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    info.GetReturnValue().SetNull();
    if (info.Length() < 1) return;
    std::string input = ns_v8_utf8(iso, info[0]);
    std::string base;
    if (info.Length() > 1 && info[1]->IsString())
        base = ns_v8_utf8(iso, info[1]);
    char *abs = ns_url_resolve(base.empty() ? input.c_str() : base.c_str(),
                               input.c_str());
    if (abs && *abs) info.GetReturnValue().Set(ns_v8_str(iso, abs));
    g_free(abs);
}

void ns_v8_url_origin_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    info.GetReturnValue().Set(ns_v8_str(iso, "null"));
    if (info.Length() < 1) return;
    std::string url = ns_v8_utf8(iso, info[0]);
    char *origin = ns_url_origin_from(url.c_str());
    if (origin && *origin)
        info.GetReturnValue().Set(ns_v8_str(iso, origin));
    g_free(origin);
}

void ns_v8_install_crypto(ns_js *js)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Object> global = ctx->Global();
    v8::Local<v8::Object> crypto = v8::Object::New(iso);
    ns_v8_bind_fn(js, crypto, "getRandomValues",
                  ns_v8_crypto_get_random_values);
    ns_v8_bind_fn(js, crypto, "randomUUID", ns_v8_crypto_random_uuid);
    v8::Local<v8::Object> subtle = v8::Object::New(iso);
    ns_v8_bind_fn(js, subtle, "digest", ns_v8_crypto_digest);
    crypto->Set(ctx, ns_v8_str(iso, "subtle"), subtle).Check();
    global->Set(ctx, ns_v8_str(iso, "crypto"), crypto).Check();
}

void ns_v8_element_ctor_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    if (!info.IsConstructCall() || !info.This()->IsObject()) return;
    v8::Local<v8::Object> self = info.This();
    if (self->InternalFieldCount() < 1) return;
    self->SetAlignedPointerInInternalField(0, nullptr);
    ns_js *js = ns_v8_js_of(iso);
    if (!js) return;
    v8::Local<v8::Value> nt = info.NewTarget();
    if (nt.IsEmpty() || !nt->IsObject()) return;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Value> tagv;
    if (!nt.As<v8::Object>()
             ->Get(ctx, ns_v8_str(iso, "__ceTagName"))
             .ToLocal(&tagv) ||
        !tagv->IsString())
        return;
    std::string tag = ns_v8_utf8(iso, tagv);
    if (tag.empty()) return;
    ns_node *el = ns_node_new_element(g_strdup(tag.c_str()));
    ns_v8_wrap *w = new ns_v8_wrap();
    w->js = js;
    w->node = el;
    w->owned = TRUE;
    w->handle.Reset(iso, self);
    self->SetAlignedPointerInInternalField(0, w);
    el->js_wrapper = w;
    el->js_invalidate = ns_v8_node_invalidated;
    js->wraps.push_back(w);
}

void ns_v8_make_node_template(ns_js *js)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::FunctionTemplate> ft =
        v8::FunctionTemplate::New(iso, ns_v8_element_ctor_cb);
    ft->SetClassName(ns_v8_str(iso, "Element"));
    ft->InstanceTemplate()->SetInternalFieldCount(1);
    v8::Local<v8::ObjectTemplate> proto = ft->PrototypeTemplate();

    struct {
        const char *name;
        v8::FunctionCallback cb;
    } methods[] = {
        {"getAttribute", ns_v8_el_get_attribute},
        {"setAttribute", ns_v8_el_set_attribute},
        {"removeAttribute", ns_v8_el_remove_attribute},
        {"hasAttribute", ns_v8_el_has_attribute},
        {"getAttributeNames", ns_v8_el_get_attribute_names},
        {"appendChild", ns_v8_el_append_child},
        {"insertBefore", ns_v8_el_insert_before},
        {"removeChild", ns_v8_el_remove_child},
        {"replaceChild", ns_v8_el_replace_child},
        {"remove", ns_v8_el_remove_self},
        {"append", ns_v8_el_append_any},
        {"cloneNode", ns_v8_el_clone_node},
        {"contains", ns_v8_el_contains},
        {"querySelector", ns_v8_el_query_selector},
        {"querySelectorAll", ns_v8_el_query_selector_all},
        {"matches", ns_v8_el_matches},
        {"closest", ns_v8_el_closest},
        {"getElementsByTagName", ns_v8_el_get_by_tag},
        {"getElementsByClassName", ns_v8_el_get_by_class},
        {"addEventListener", ns_v8_el_add_listener},
        {"removeEventListener", ns_v8_el_remove_listener},
        {"dispatchEvent", ns_v8_el_dispatch_event},
        {"click", ns_v8_el_click},
        {"focus", ns_v8_el_focus},
        {"blur", ns_v8_el_blur},
        {"getBoundingClientRect", ns_v8_el_bounding_rect_real},
        {"getContext", ns_v8_el_get_context},
        {"toDataURL", ns_v8_el_to_data_url},
        {"attachShadow", ns_v8_el_attach_shadow},
    };
    for (auto &m : methods)
        proto->Set(iso, m.name, v8::FunctionTemplate::New(iso, m.cb));

    struct {
        const char *name;
        int code;
    } rels[] = {
        {"parentNode", 0},         {"firstChild", 1},
        {"lastChild", 2},          {"nextSibling", 3},
        {"previousSibling", 4},    {"firstElementChild", 5},
        {"lastElementChild", 6},   {"nextElementSibling", 7},
        {"previousElementSibling", 8}, {"parentElement", 9},
    };
    for (auto &r : rels)
        proto->SetAccessorProperty(
            ns_v8_str(iso, r.name),
            v8::FunctionTemplate::New(iso, ns_v8_el_rel_get,
                v8::External::New(iso, (void *)(intptr_t)r.code)));

    proto->SetAccessorProperty(ns_v8_str(iso, "nodeType"),
        v8::FunctionTemplate::New(iso, ns_v8_el_node_type));
    proto->SetAccessorProperty(ns_v8_str(iso, "nodeName"),
        v8::FunctionTemplate::New(iso, ns_v8_el_node_name));
    proto->SetAccessorProperty(ns_v8_str(iso, "tagName"),
        v8::FunctionTemplate::New(iso, ns_v8_el_node_name));
    proto->SetAccessorProperty(ns_v8_str(iso, "children"),
        v8::FunctionTemplate::New(iso, ns_v8_el_children_get,
            v8::External::New(iso, (void *)(intptr_t)1)));
    proto->SetAccessorProperty(ns_v8_str(iso, "childNodes"),
        v8::FunctionTemplate::New(iso, ns_v8_el_children_get,
            v8::External::New(iso, (void *)(intptr_t)0)));
    proto->SetAccessorProperty(ns_v8_str(iso, "childElementCount"),
        v8::FunctionTemplate::New(iso, ns_v8_el_child_count));
    proto->SetAccessorProperty(ns_v8_str(iso, "textContent"),
        v8::FunctionTemplate::New(iso, ns_v8_el_text_get),
        v8::FunctionTemplate::New(iso, ns_v8_el_text_set));
    proto->SetAccessorProperty(ns_v8_str(iso, "nodeValue"),
        v8::FunctionTemplate::New(iso, ns_v8_el_text_get),
        v8::FunctionTemplate::New(iso, ns_v8_el_text_set));
    proto->SetAccessorProperty(ns_v8_str(iso, "data"),
        v8::FunctionTemplate::New(iso, ns_v8_el_text_get),
        v8::FunctionTemplate::New(iso, ns_v8_el_text_set));
    proto->SetAccessorProperty(ns_v8_str(iso, "innerHTML"),
        v8::FunctionTemplate::New(iso, ns_v8_el_inner_html_get),
        v8::FunctionTemplate::New(iso, ns_v8_el_inner_html_set));
    proto->SetAccessorProperty(ns_v8_str(iso, "outerHTML"),
        v8::FunctionTemplate::New(iso, ns_v8_el_outer_html_get));
    proto->SetAccessorProperty(ns_v8_str(iso, "ownerDocument"),
        v8::FunctionTemplate::New(iso, ns_v8_el_owner_document));
    proto->SetAccessorProperty(ns_v8_str(iso, "shadowRoot"),
        v8::FunctionTemplate::New(iso, ns_v8_el_shadow_root_get));
    proto->SetAccessorProperty(ns_v8_str(iso, "value"),
        v8::FunctionTemplate::New(iso, ns_v8_el_value_get),
        v8::FunctionTemplate::New(iso, ns_v8_el_value_set));
    proto->SetAccessorProperty(ns_v8_str(iso, "checked"),
        v8::FunctionTemplate::New(iso, ns_v8_el_checked_get),
        v8::FunctionTemplate::New(iso, ns_v8_el_checked_set));

    proto->SetAccessorProperty(ns_v8_str(iso, "width"),
        v8::FunctionTemplate::New(iso, ns_v8_el_dim_get,
            v8::External::New(iso, (void *)(intptr_t)1)),
        v8::FunctionTemplate::New(iso, ns_v8_el_dim_set,
            v8::External::New(iso, (void *)(intptr_t)1)));
    proto->SetAccessorProperty(ns_v8_str(iso, "height"),
        v8::FunctionTemplate::New(iso, ns_v8_el_dim_get,
            v8::External::New(iso, (void *)(intptr_t)0)),
        v8::FunctionTemplate::New(iso, ns_v8_el_dim_set,
            v8::External::New(iso, (void *)(intptr_t)0)));

    struct {
        const char *name;
        int code;
    } metrics[] = {
        {"offsetWidth", 0},  {"offsetHeight", 1}, {"clientWidth", 2},
        {"clientHeight", 3}, {"offsetTop", 4},    {"offsetLeft", 5},
        {"scrollWidth", 6},  {"scrollHeight", 7},
    };
    for (auto &m : metrics)
        proto->SetAccessorProperty(
            ns_v8_str(iso, m.name),
            v8::FunctionTemplate::New(iso, ns_v8_el_metric_get,
                v8::External::New(iso, (void *)(intptr_t)m.code)));

    js->node_tmpl.Reset(iso, ft);
}

void ns_v8_doc_get_element_by_id(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    info.GetReturnValue().SetNull();
    if (!js || !js->current_doc || info.Length() < 1) return;
    std::string id = ns_v8_utf8(info.GetIsolate(), info[0]);
    ns_node *found = ns_node_find_by_id(js->current_doc, id.c_str());
    if (found) info.GetReturnValue().Set(ns_v8_wrap_node(js, found));
}

void ns_v8_doc_query(const v8::FunctionCallbackInfo<v8::Value> &info,
                     gboolean all)
{
    ns_js *js = ns_v8_js_here(info);
    std::vector<ns_node *> nodes;
    if (js && js->current_doc && info.Length() >= 1)
        ns_v8_query(js, js->current_doc,
                    ns_v8_utf8(info.GetIsolate(), info[0]).c_str(), all,
                    nodes);
    ns_v8_return_node_vector(info, js, nodes, !all);
}

void ns_v8_doc_query_selector(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_doc_query(info, FALSE);
}

void ns_v8_doc_query_selector_all(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_doc_query(info, TRUE);
}

void ns_v8_doc_get_by_tag(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    std::vector<ns_node *> nodes;
    if (js && js->current_doc && info.Length() >= 1) {
        std::string tag = ns_v8_utf8(info.GetIsolate(), info[0]);
        ns_v8_collect_by_tag(js->current_doc, tag.c_str(), nodes);
    }
    ns_v8_return_node_vector(info, js, nodes, FALSE);
}

void ns_v8_doc_get_by_class(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    std::vector<ns_node *> nodes;
    if (js && js->current_doc && info.Length() >= 1) {
        std::string cls = ns_v8_utf8(info.GetIsolate(), info[0]);
        if (!cls.empty())
            ns_v8_collect_by_class(js->current_doc, cls.c_str(), cls.size(),
                                   nodes);
    }
    ns_v8_return_node_vector(info, js, nodes, FALSE);
}

void ns_v8_doc_create_element(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    info.GetReturnValue().SetNull();
    if (!js || info.Length() < 1) return;
    char *tag =
        g_ascii_strdown(ns_v8_utf8(info.GetIsolate(), info[0]).c_str(), -1);
    if (!*tag) {
        g_free(tag);
        return;
    }
    ns_node *el = ns_node_new_element(tag);
    v8::Local<v8::Value> w = ns_v8_wrap_node(js, el);
    ns_v8_wrap_set_owned(js, el, TRUE);
    v8::Local<v8::Context> ctx = info.GetIsolate()->GetCurrentContext();
    v8::Local<v8::Value> up;
    if (ctx->Global()
            ->Get(ctx, ns_v8_str(info.GetIsolate(), "__nsUpgradeCreated"))
            .ToLocal(&up) &&
        up->IsFunction()) {
        v8::TryCatch tc(info.GetIsolate());
        v8::Local<v8::Value> arg = w;
        v8::Local<v8::Value> r;
        if (!up.As<v8::Function>()
                 ->Call(ctx, ctx->Global(), 1, &arg)
                 .ToLocal(&r))
            tc.Reset();
    }
    info.GetReturnValue().Set(w);
}

void ns_v8_doc_create_text(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    info.GetReturnValue().SetNull();
    if (!js) return;
    std::string s = info.Length() >= 1
                        ? ns_v8_utf8(info.GetIsolate(), info[0])
                        : std::string();
    ns_node *t = ns_node_new_text(g_strdup(s.c_str()));
    v8::Local<v8::Value> w = ns_v8_wrap_node(js, t);
    ns_v8_wrap_set_owned(js, t, TRUE);
    info.GetReturnValue().Set(w);
}

void ns_v8_doc_create_comment(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    info.GetReturnValue().SetNull();
    if (!js) return;
    std::string s = info.Length() >= 1
                        ? ns_v8_utf8(info.GetIsolate(), info[0])
                        : std::string();
    ns_node *c = ns_node_new_comment(g_strdup(s.c_str()));
    v8::Local<v8::Value> w = ns_v8_wrap_node(js, c);
    ns_v8_wrap_set_owned(js, c, TRUE);
    info.GetReturnValue().Set(w);
}

void ns_v8_doc_create_fragment(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    info.GetReturnValue().SetNull();
    if (!js) return;
    ns_node *f = ns_node_new_element(g_strdup("#document-fragment"));
    f->flags |= NS_NODE_FRAGMENT;
    v8::Local<v8::Value> w = ns_v8_wrap_node(js, f);
    ns_v8_wrap_set_owned(js, f, TRUE);
    info.GetReturnValue().Set(w);
}

void ns_v8_doc_root_get(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    info.GetReturnValue().SetNull();
    if (!js || !js->current_doc) return;
    const char *tag =
        (const char *)info.Data().As<v8::External>()->Value();
    ns_node *el = ns_node_find_first_element(js->current_doc, tag);
    if (el) info.GetReturnValue().Set(ns_v8_wrap_node(js, el));
}

void ns_v8_doc_active_element(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    if (js && js->focused)
        info.GetReturnValue().Set(
            ns_v8_wrap_node(js, (ns_node *)js->focused));
    else
        info.GetReturnValue().SetNull();
}

void ns_v8_document_title_set(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_here(info);
    if (!js || !js->current_doc || info.Length() < 1) return;
    std::string s = ns_v8_utf8(info.GetIsolate(), info[0]);
    ns_node *title = ns_node_find_first_element(js->current_doc, "title");
    if (!title) {
        ns_node *head = ns_node_find_first_element(js->current_doc, "head");
        if (!head) return;
        title = ns_node_new_element(g_strdup("title"));
        ns_node_append_child(head, title);
    }
    ns_v8_clear_children(js, title);
    ns_node_append_child(title, ns_node_new_text(g_strdup(s.c_str())));
    ns_v8_mutated(js);
}

const char ns_v8_dom_bootstrap_src[] =
    "(function () {\n"
    "  'use strict';\n"
    "  var E = globalThis.Element;\n"
    "  if (!E) return;\n"
    "  var p = E.prototype;\n"
    "  function reflect(name, attr) {\n"
    "    Object.defineProperty(p, name, {\n"
    "      configurable: true,\n"
    "      get: function () { return this.getAttribute(attr) || ''; },\n"
    "      set: function (v) { this.setAttribute(attr, String(v)); }\n"
    "    });\n"
    "  }\n"
    "  function reflectBool(name, attr) {\n"
    "    Object.defineProperty(p, name, {\n"
    "      configurable: true,\n"
    "      get: function () { return this.hasAttribute(attr); },\n"
    "      set: function (v) {\n"
    "        if (v) this.setAttribute(attr, '');\n"
    "        else this.removeAttribute(attr);\n"
    "      }\n"
    "    });\n"
    "  }\n"
    "  reflect('id', 'id');\n"
    "  reflect('className', 'class');\n"
    "  reflect('name', 'name');\n"
    "  reflect('type', 'type');\n"
    "  reflect('href', 'href');\n"
    "  reflect('src', 'src');\n"
    "  reflect('alt', 'alt');\n"
    "  reflect('title', 'title');\n"
    "  reflect('placeholder', 'placeholder');\n"
    "  reflectBool('disabled', 'disabled');\n"
    "  reflectBool('hidden', 'hidden');\n"
    "  reflectBool('required', 'required');\n"
    "  reflectBool('selected', 'selected');\n"
    "  Object.defineProperty(p, 'innerText', {\n"
    "    configurable: true,\n"
    "    get: function () { return this.textContent; },\n"
    "    set: function (v) { this.textContent = v; }\n"
    "  });\n"
    "  ['scrollTop', 'scrollLeft'].forEach(function (k) {\n"
    "    Object.defineProperty(p, k, {\n"
    "      configurable: true,\n"
    "      get: function () { return 0; },\n"
    "      set: function () {}\n"
    "    });\n"
    "  });\n"
    "  globalThis.getComputedStyle = function (el) {\n"
    "    var api = {\n"
    "      getPropertyValue: function (prop) {\n"
    "        return __nsComputedStyle(el, String(prop).toLowerCase());\n"
    "      },\n"
    "      setProperty: function () {},\n"
    "      removeProperty: function () { return ''; }\n"
    "    };\n"
    "    return new Proxy(api, {\n"
    "      get: function (t, prop) {\n"
    "        if (prop in t) return t[prop];\n"
    "        if (typeof prop !== 'string') return undefined;\n"
    "        return t.getPropertyValue(camel2kebab(prop));\n"
    "      }\n"
    "    });\n"
    "  };\n"
    "  globalThis.history = {\n"
    "    length: 1,\n"
    "    state: null,\n"
    "    scrollRestoration: 'auto',\n"
    "    pushState: function (s) { this.state = s; this.length++; },\n"
    "    replaceState: function (s) { this.state = s; },\n"
    "    go: function () {}, back: function () {}, forward: function () {}\n"
    "  };\n"
    "  globalThis.scrollTo = globalThis.scroll = function () {};\n"
    "  globalThis.scrollBy = function () {};\n"
    "  function camel2kebab(s) {\n"
    "    return String(s).replace(/[A-Z]/g, function (c) {\n"
    "      return '-' + c.toLowerCase();\n"
    "    });\n"
    "  }\n"
    "  function parseCss(t) {\n"
    "    var m = new Map();\n"
    "    if (t) t.split(';').forEach(function (d) {\n"
    "      var i = d.indexOf(':');\n"
    "      if (i > 0) {\n"
    "        var k = d.slice(0, i).trim().toLowerCase();\n"
    "        var v = d.slice(i + 1).trim();\n"
    "        if (k) m.set(k, v);\n"
    "      }\n"
    "    });\n"
    "    return m;\n"
    "  }\n"
    "  function serialCss(m) {\n"
    "    var out = [];\n"
    "    m.forEach(function (v, k) { out.push(k + ': ' + v); });\n"
    "    return out.join('; ');\n"
    "  }\n"
    "  function makeStyle(el) {\n"
    "    var api = {\n"
    "      getPropertyValue: function (prop) {\n"
    "        return parseCss(el.getAttribute('style'))\n"
    "          .get(String(prop).toLowerCase()) || '';\n"
    "      },\n"
    "      setProperty: function (prop, v) {\n"
    "        var m = parseCss(el.getAttribute('style'));\n"
    "        m.set(String(prop).toLowerCase(), String(v));\n"
    "        el.setAttribute('style', serialCss(m));\n"
    "      },\n"
    "      removeProperty: function (prop) {\n"
    "        var m = parseCss(el.getAttribute('style'));\n"
    "        var old = m.get(String(prop).toLowerCase()) || '';\n"
    "        m.delete(String(prop).toLowerCase());\n"
    "        el.setAttribute('style', serialCss(m));\n"
    "        return old;\n"
    "      }\n"
    "    };\n"
    "    return new Proxy(api, {\n"
    "      get: function (t, prop) {\n"
    "        if (prop in t) return t[prop];\n"
    "        if (prop === 'cssText') return el.getAttribute('style') || '';\n"
    "        if (typeof prop !== 'string') return undefined;\n"
    "        return t.getPropertyValue(camel2kebab(prop));\n"
    "      },\n"
    "      set: function (t, prop, v) {\n"
    "        if (prop === 'cssText') {\n"
    "          el.setAttribute('style', String(v));\n"
    "          return true;\n"
    "        }\n"
    "        if (typeof prop === 'string')\n"
    "          t.setProperty(camel2kebab(prop), v);\n"
    "        return true;\n"
    "      }\n"
    "    });\n"
    "  }\n"
    "  function makeClassList(el) {\n"
    "    function get() {\n"
    "      return (el.getAttribute('class') || '').split(/\\s+/)\n"
    "        .filter(Boolean);\n"
    "    }\n"
    "    function put(a) { el.setAttribute('class', a.join(' ')); }\n"
    "    return {\n"
    "      add: function () {\n"
    "        var a = get();\n"
    "        for (var i = 0; i < arguments.length; i++) {\n"
    "          var c = String(arguments[i]);\n"
    "          if (a.indexOf(c) < 0) a.push(c);\n"
    "        }\n"
    "        put(a);\n"
    "      },\n"
    "      remove: function () {\n"
    "        var drop = Array.prototype.map.call(arguments, String);\n"
    "        put(get().filter(function (c) {\n"
    "          return drop.indexOf(c) < 0;\n"
    "        }));\n"
    "      },\n"
    "      toggle: function (c, force) {\n"
    "        c = String(c);\n"
    "        var a = get();\n"
    "        var has = a.indexOf(c) >= 0;\n"
    "        var want = force === undefined ? !has : !!force;\n"
    "        if (want && !has) a.push(c);\n"
    "        if (!want && has) a.splice(a.indexOf(c), 1);\n"
    "        put(a);\n"
    "        return want;\n"
    "      },\n"
    "      contains: function (c) { return get().indexOf(String(c)) >= 0; },\n"
    "      item: function (i) { return get()[i] || null; },\n"
    "      get length() { return get().length; },\n"
    "      toString: function () { return el.getAttribute('class') || ''; }\n"
    "    };\n"
    "  }\n"
    "  Object.defineProperty(p, 'style', {\n"
    "    configurable: true,\n"
    "    get: function () {\n"
    "      if (!this.__style) this.__style = makeStyle(this);\n"
    "      return this.__style;\n"
    "    },\n"
    "    set: function (v) { this.setAttribute('style', String(v)); }\n"
    "  });\n"
    "  Object.defineProperty(p, 'classList', {\n"
    "    configurable: true,\n"
    "    get: function () {\n"
    "      if (!this.__classList) this.__classList = makeClassList(this);\n"
    "      return this.__classList;\n"
    "    }\n"
    "  });\n"
    "  ['click', 'input', 'change', 'submit', 'keydown', 'keyup',\n"
    "   'keypress', 'mousedown', 'mouseup', 'mouseover', 'mouseout',\n"
    "   'mousemove', 'focus', 'blur', 'load', 'error'].forEach(function (t) {\n"
    "    Object.defineProperty(p, 'on' + t, {\n"
    "      configurable: true,\n"
    "      get: function () {\n"
    "        return (this.__handlers && this.__handlers[t]) || null;\n"
    "      },\n"
    "      set: function (f) {\n"
    "        var h = this.__handlers || (this.__handlers = {});\n"
    "        if (h[t]) this.removeEventListener(t, h[t]);\n"
    "        h[t] = typeof f === 'function' ? f : null;\n"
    "        if (h[t]) this.addEventListener(t, h[t]);\n"
    "      }\n"
    "    });\n"
    "  });\n"
    "  globalThis.Node = E;\n"
    "  globalThis.HTMLElement = E;\n"
    "  globalThis.EventTarget = globalThis.EventTarget || E;\n"
    "  E.ELEMENT_NODE = 1; E.TEXT_NODE = 3; E.COMMENT_NODE = 8;\n"
    "  E.DOCUMENT_NODE = 9; E.DOCUMENT_FRAGMENT_NODE = 11;\n"
    "  globalThis.Event = function Event(type, opts) {\n"
    "    this.type = String(type);\n"
    "    this.bubbles = !!(opts && opts.bubbles);\n"
    "    this.cancelable = !!(opts && opts.cancelable);\n"
    "    this.defaultPrevented = false;\n"
    "    this.cancelBubble = false;\n"
    "    this.target = null;\n"
    "    this.currentTarget = null;\n"
    "    this.timeStamp = performance.now();\n"
    "  };\n"
    "  Event.prototype.preventDefault = function () {\n"
    "    this.defaultPrevented = true;\n"
    "  };\n"
    "  Event.prototype.stopPropagation = function () {\n"
    "    this.cancelBubble = true;\n"
    "  };\n"
    "  Event.prototype.stopImmediatePropagation =\n"
    "    Event.prototype.stopPropagation;\n"
    "  globalThis.CustomEvent = function CustomEvent(type, opts) {\n"
    "    Event.call(this, type, opts);\n"
    "    this.detail = opts && opts.detail !== undefined ? opts.detail\n"
    "                                                    : null;\n"
    "  };\n"
    "  CustomEvent.prototype = Object.create(Event.prototype);\n"
    "  function NoopObserver() {}\n"
    "  NoopObserver.prototype.observe = function () {};\n"
    "  NoopObserver.prototype.unobserve = function () {};\n"
    "  NoopObserver.prototype.disconnect = function () {};\n"
    "  NoopObserver.prototype.takeRecords = function () { return []; };\n"
    "  globalThis.MutationObserver = NoopObserver;\n"
    "  globalThis.IntersectionObserver = NoopObserver;\n"
    "  globalThis.ResizeObserver = NoopObserver;\n"
    "  globalThis.Image = function (w, h) {\n"
    "    var img = document.createElement('img');\n"
    "    if (w !== undefined) img.setAttribute('width', w);\n"
    "    if (h !== undefined) img.setAttribute('height', h);\n"
    "    return img;\n"
    "  };\n"
    "  globalThis.Option = function (text, value) {\n"
    "    var o = document.createElement('option');\n"
    "    if (text !== undefined) o.textContent = String(text);\n"
    "    if (value !== undefined) o.setAttribute('value', value);\n"
    "    return o;\n"
    "  };\n"
    "  var ceRegistry = new Map();\n"
    "  var ceWaiters = new Map();\n"
    "  function ceIsConnected(el) {\n"
    "    for (var n = el; n; n = n.parentNode)\n"
    "      if (n.nodeType === 9 ||\n"
    "          (n.tagName === 'HTML' && !n.parentNode))\n"
    "        return true;\n"
    "    return false;\n"
    "  }\n"
    "  function ceUpgrade(el, entry) {\n"
    "    if (el.__ceUpgraded) return;\n"
    "    el.__ceUpgraded = true;\n"
    "    Object.setPrototypeOf(el, entry.ctor.prototype);\n"
    "    try {\n"
    "      if (entry.observed && entry.observed.length &&\n"
    "          typeof el.attributeChangedCallback === 'function')\n"
    "        entry.observed.forEach(function (a) {\n"
    "          var v = el.getAttribute(a);\n"
    "          if (v !== null) el.attributeChangedCallback(a, null, v);\n"
    "        });\n"
    "      if (ceIsConnected(el) &&\n"
    "          typeof el.connectedCallback === 'function')\n"
    "        el.connectedCallback();\n"
    "    } catch (e) { console.error('custom element upgrade: ' + e); }\n"
    "  }\n"
    "  function ceMaybeUpgrade(el) {\n"
    "    if (!el || !el.tagName) return el;\n"
    "    var entry = ceRegistry.get(el.tagName.toLowerCase());\n"
    "    if (entry) ceUpgrade(el, entry);\n"
    "    return el;\n"
    "  }\n"
    "  globalThis.customElements = {\n"
    "    define: function (name, ctor, opts) {\n"
    "      name = String(name).toLowerCase();\n"
    "      var observed = [];\n"
    "      try {\n"
    "        if (ctor.observedAttributes)\n"
    "          observed = Array.from(ctor.observedAttributes).map(String);\n"
    "      } catch (e) {}\n"
    "      var entry = { ctor: ctor, observed: observed,\n"
    "                    extends_tag: opts && opts.extends };\n"
    "      try { ctor.__ceTagName = name; } catch (e) {}\n"
    "      ceRegistry.set(name, entry);\n"
    "      var existing = document.getElementsByTagName(name);\n"
    "      for (var i = 0; i < existing.length; i++)\n"
    "        ceUpgrade(existing[i], entry);\n"
    "      var w = ceWaiters.get(name);\n"
    "      if (w) { w.forEach(function (r) { r(ctor); });\n"
    "               ceWaiters.delete(name); }\n"
    "    },\n"
    "    get: function (name) {\n"
    "      var e = ceRegistry.get(String(name).toLowerCase());\n"
    "      return e ? e.ctor : undefined;\n"
    "    },\n"
    "    whenDefined: function (name) {\n"
    "      name = String(name).toLowerCase();\n"
    "      var e = ceRegistry.get(name);\n"
    "      if (e) return Promise.resolve(e.ctor);\n"
    "      return new Promise(function (resolve) {\n"
    "        var w = ceWaiters.get(name) || [];\n"
    "        w.push(resolve);\n"
    "        ceWaiters.set(name, w);\n"
    "      });\n"
    "    },\n"
    "    upgrade: function (root) { ceMaybeUpgrade(root); }\n"
    "  };\n"
    "  var origSetAttribute = p.setAttribute;\n"
    "  p.setAttribute = function (name, value) {\n"
    "    var old = this.getAttribute(name);\n"
    "    origSetAttribute.call(this, name, value);\n"
    "    if (this.__ceUpgraded &&\n"
    "        typeof this.attributeChangedCallback === 'function') {\n"
    "      var entry = ceRegistry.get(this.tagName.toLowerCase());\n"
    "      if (entry && entry.observed.indexOf(String(name)) >= 0)\n"
    "        this.attributeChangedCallback(String(name), old,\n"
    "                                      this.getAttribute(name));\n"
    "    }\n"
    "  };\n"
    "  var origAppendChild = p.appendChild;\n"
    "  p.appendChild = function (child) {\n"
    "    var r = origAppendChild.call(this, child);\n"
    "    if (child && child.tagName) {\n"
    "      ceMaybeUpgrade(child);\n"
    "      if (child.__ceUpgraded && ceIsConnected(child) &&\n"
    "          typeof child.connectedCallback === 'function' &&\n"
    "          !child.__ceConnectedFired) {\n"
    "        child.__ceConnectedFired = true;\n"
    "        try { child.connectedCallback(); }\n"
    "        catch (e) { console.error('connectedCallback: ' + e); }\n"
    "      }\n"
    "    }\n"
    "    return r;\n"
    "  };\n"
    "  var origRemoveChild = p.removeChild;\n"
    "  p.removeChild = function (child) {\n"
    "    var r = origRemoveChild.call(this, child);\n"
    "    if (child && child.__ceUpgraded &&\n"
    "        typeof child.disconnectedCallback === 'function') {\n"
    "      child.__ceConnectedFired = false;\n"
    "      try { child.disconnectedCallback(); }\n"
    "      catch (e) { console.error('disconnectedCallback: ' + e); }\n"
    "    }\n"
    "    return r;\n"
    "  };\n"
    "  globalThis.__nsUpgradeCreated = ceMaybeUpgrade;\n"
    "})();\n";

const char ns_v8_net_bootstrap_src[] =
    "(function () {\n"
    "  'use strict';\n"
    "  function parseRawHeaders(raw) {\n"
    "    var m = {};\n"
    "    String(raw || '').split(/\\r?\\n/).forEach(function (line) {\n"
    "      var i = line.indexOf(':');\n"
    "      if (i > 0)\n"
    "        m[line.slice(0, i).trim().toLowerCase()] =\n"
    "          line.slice(i + 1).trim();\n"
    "    });\n"
    "    return m;\n"
    "  }\n"
    "  function Headers(init) {\n"
    "    this.__m = {};\n"
    "    var self = this;\n"
    "    if (init) {\n"
    "      if (Array.isArray(init))\n"
    "        init.forEach(function (p) { self.set(p[0], p[1]); });\n"
    "      else if (init instanceof Headers)\n"
    "        init.forEach(function (v, k) { self.set(k, v); });\n"
    "      else\n"
    "        Object.keys(init).forEach(function (k) {\n"
    "          self.set(k, init[k]);\n"
    "        });\n"
    "    }\n"
    "  }\n"
    "  Headers.prototype.set = function (k, v) {\n"
    "    this.__m[String(k).toLowerCase()] = String(v);\n"
    "  };\n"
    "  Headers.prototype.append = Headers.prototype.set;\n"
    "  Headers.prototype.get = function (k) {\n"
    "    var v = this.__m[String(k).toLowerCase()];\n"
    "    return v === undefined ? null : v;\n"
    "  };\n"
    "  Headers.prototype.has = function (k) {\n"
    "    return this.get(k) !== null;\n"
    "  };\n"
    "  Headers.prototype.delete = function (k) {\n"
    "    delete this.__m[String(k).toLowerCase()];\n"
    "  };\n"
    "  Headers.prototype.forEach = function (cb) {\n"
    "    var m = this.__m;\n"
    "    Object.keys(m).forEach(function (k) { cb(m[k], k); });\n"
    "  };\n"
    "  globalThis.Headers = Headers;\n"
    "  function makeResponse(raw) {\n"
    "    var headers = new Headers(parseRawHeaders(raw.headersRaw));\n"
    "    if (!headers.has('content-type') && raw.contentType)\n"
    "      headers.set('content-type', raw.contentType);\n"
    "    return {\n"
    "      ok: raw.status >= 200 && raw.status < 300,\n"
    "      status: raw.status,\n"
    "      statusText: '',\n"
    "      url: raw.url,\n"
    "      redirected: raw.redirected,\n"
    "      headers: headers,\n"
    "      bodyUsed: false,\n"
    "      text: function () { return Promise.resolve(raw.bodyText); },\n"
    "      json: function () {\n"
    "        return Promise.resolve().then(function () {\n"
    "          return JSON.parse(raw.bodyText);\n"
    "        });\n"
    "      },\n"
    "      arrayBuffer: function () {\n"
    "        return Promise.resolve(raw.bodyBuffer);\n"
    "      },\n"
    "      blob: function () { return Promise.resolve(raw.bodyBuffer); },\n"
    "      clone: function () { return makeResponse(raw); }\n"
    "    };\n"
    "  }\n"
    "  function headerPairs(h) {\n"
    "    var out = [];\n"
    "    if (!h) return out;\n"
    "    if (Array.isArray(h))\n"
    "      h.forEach(function (p) { out.push(String(p[0]), String(p[1])); });\n"
    "    else if (typeof h.forEach === 'function')\n"
    "      h.forEach(function (v, k) { out.push(String(k), String(v)); });\n"
    "    else\n"
    "      Object.keys(h).forEach(function (k) {\n"
    "        out.push(k, String(h[k]));\n"
    "      });\n"
    "    return out;\n"
    "  }\n"
    "  globalThis.fetch = function (input, init) {\n"
    "    init = init || {};\n"
    "    var url = typeof input === 'string' ? input\n"
    "              : String(input && input.url !== undefined ? input.url\n"
    "                                                        : input);\n"
    "    var method = String(init.method ||\n"
    "                        (input && input.method) || 'GET').toUpperCase();\n"
    "    var body = init.body != null ? String(init.body) : null;\n"
    "    var headers = headerPairs(init.headers ||\n"
    "                              (input && input.headers));\n"
    "    return __nsFetch(url, method, body, headers).then(makeResponse);\n"
    "  };\n"
    "  globalThis.Request = function (url, init) {\n"
    "    init = init || {};\n"
    "    this.url = String(url);\n"
    "    this.method = String(init.method || 'GET').toUpperCase();\n"
    "    this.headers = new Headers(init.headers);\n"
    "  };\n"
    "  globalThis.AbortController = function () {\n"
    "    this.signal = { aborted: false, reason: undefined,\n"
    "                    addEventListener: function () {},\n"
    "                    removeEventListener: function () {},\n"
    "                    throwIfAborted: function () {} };\n"
    "  };\n"
    "  AbortController.prototype.abort = function (reason) {\n"
    "    this.signal.aborted = true;\n"
    "    this.signal.reason = reason;\n"
    "  };\n"
    "  globalThis.AbortSignal = function () {};\n"
    "  AbortSignal.timeout = function () {\n"
    "    return new AbortController().signal;\n"
    "  };\n"
    "  AbortSignal.abort = function () {\n"
    "    var s = new AbortController().signal;\n"
    "    s.aborted = true;\n"
    "    return s;\n"
    "  };\n"
    "  function XMLHttpRequest() {\n"
    "    this.readyState = 0;\n"
    "    this.status = 0;\n"
    "    this.responseText = '';\n"
    "    this.response = '';\n"
    "    this.responseType = '';\n"
    "    this.responseURL = '';\n"
    "    this.timeout = 0;\n"
    "    this.withCredentials = false;\n"
    "    this.upload = { addEventListener: function () {},\n"
    "                    removeEventListener: function () {} };\n"
    "    this.__h = [];\n"
    "    this.__listeners = {};\n"
    "  }\n"
    "  XMLHttpRequest.prototype.open = function (m, u, async_flag) {\n"
    "    this.__m = String(m).toUpperCase();\n"
    "    this.__u = String(u);\n"
    "    this.__async = async_flag !== false;\n"
    "    this.readyState = 1;\n"
    "    this.__fire('readystatechange');\n"
    "  };\n"
    "  XMLHttpRequest.prototype.setRequestHeader = function (k, v) {\n"
    "    this.__h.push(String(k), String(v));\n"
    "  };\n"
    "  XMLHttpRequest.prototype.getAllResponseHeaders = function () {\n"
    "    return this.__raw || '';\n"
    "  };\n"
    "  XMLHttpRequest.prototype.getResponseHeader = function (k) {\n"
    "    var m = parseRawHeaders(this.__raw);\n"
    "    var v = m[String(k).toLowerCase()];\n"
    "    return v === undefined ? null : v;\n"
    "  };\n"
    "  XMLHttpRequest.prototype.addEventListener = function (t, f) {\n"
    "    (this.__listeners[t] = this.__listeners[t] || []).push(f);\n"
    "  };\n"
    "  XMLHttpRequest.prototype.removeEventListener = function (t, f) {\n"
    "    var l = this.__listeners[t];\n"
    "    if (l) {\n"
    "      var i = l.indexOf(f);\n"
    "      if (i >= 0) l.splice(i, 1);\n"
    "    }\n"
    "  };\n"
    "  XMLHttpRequest.prototype.__fire = function (t) {\n"
    "    var ev = { type: t, target: this };\n"
    "    var on = this['on' + t];\n"
    "    if (typeof on === 'function') {\n"
    "      try { on.call(this, ev); }\n"
    "      catch (e) { console.error('xhr on' + t + ': ' + e); }\n"
    "    }\n"
    "    var self = this;\n"
    "    (this.__listeners[t] || []).slice().forEach(function (f) {\n"
    "      try { f.call(self, ev); }\n"
    "      catch (e) { console.error('xhr ' + t + ' listener: ' + e); }\n"
    "    });\n"
    "  };\n"
    "  XMLHttpRequest.prototype.send = function (body) {\n"
    "    var self = this;\n"
    "    function done(raw) {\n"
    "      self.status = raw.status;\n"
    "      self.__raw = raw.headersRaw;\n"
    "      self.responseURL = raw.url;\n"
    "      self.responseText = raw.bodyText;\n"
    "      if (self.responseType === 'arraybuffer')\n"
    "        self.response = raw.bodyBuffer;\n"
    "      else if (self.responseType === 'json') {\n"
    "        try { self.response = JSON.parse(raw.bodyText); }\n"
    "        catch (e) { self.response = null; }\n"
    "      } else {\n"
    "        self.response = raw.bodyText;\n"
    "      }\n"
    "      self.readyState = 4;\n"
    "      self.__fire('readystatechange');\n"
    "      self.__fire('load');\n"
    "      self.__fire('loadend');\n"
    "    }\n"
    "    function fail() {\n"
    "      self.status = 0;\n"
    "      self.readyState = 4;\n"
    "      self.__fire('readystatechange');\n"
    "      self.__fire('error');\n"
    "      self.__fire('loadend');\n"
    "    }\n"
    "    var b = body != null ? String(body) : null;\n"
    "    if (this.__async) {\n"
    "      __nsFetch(this.__u, this.__m, b, this.__h).then(done, fail);\n"
    "    } else {\n"
    "      var raw = __nsFetchSync(this.__u, this.__m, b);\n"
    "      if (raw) done(raw);\n"
    "      else fail();\n"
    "    }\n"
    "  };\n"
    "  XMLHttpRequest.prototype.abort = function () {};\n"
    "  XMLHttpRequest.prototype.overrideMimeType = function () {};\n"
    "  globalThis.XMLHttpRequest = XMLHttpRequest;\n"
    "  globalThis.TextEncoder = function () { this.encoding = 'utf-8'; };\n"
    "  TextEncoder.prototype.encode = function (s) {\n"
    "    var bin = unescape(encodeURIComponent(String(s)));\n"
    "    var out = new Uint8Array(bin.length);\n"
    "    for (var i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);\n"
    "    return out;\n"
    "  };\n"
    "  globalThis.TextDecoder = function () { this.encoding = 'utf-8'; };\n"
    "  TextDecoder.prototype.decode = function (buf) {\n"
    "    if (buf == null) return '';\n"
    "    var bytes = buf instanceof Uint8Array ? buf : new Uint8Array(buf);\n"
    "    var bin = '';\n"
    "    for (var i = 0; i < bytes.length; i++)\n"
    "      bin += String.fromCharCode(bytes[i]);\n"
    "    try { return decodeURIComponent(escape(bin)); }\n"
    "    catch (e) { return bin; }\n"
    "  };\n"
    "  function USP(init) {\n"
    "    this.__p = [];\n"
    "    if (init == null) return;\n"
    "    if (typeof init === 'string') {\n"
    "      init.replace(/^\\?/, '').split('&').forEach(function (pair) {\n"
    "        if (!pair) return;\n"
    "        var i = pair.indexOf('=');\n"
    "        var k = i < 0 ? pair : pair.slice(0, i);\n"
    "        var v = i < 0 ? '' : pair.slice(i + 1);\n"
    "        try { k = decodeURIComponent(k.replace(/\\+/g, ' ')); }\n"
    "        catch (e) {}\n"
    "        try { v = decodeURIComponent(v.replace(/\\+/g, ' ')); }\n"
    "        catch (e) {}\n"
    "        this.__p.push([k, v]);\n"
    "      }, this);\n"
    "    } else if (Array.isArray(init)) {\n"
    "      init.forEach(function (pair) {\n"
    "        this.__p.push([String(pair[0]), String(pair[1])]);\n"
    "      }, this);\n"
    "    } else if (init instanceof USP) {\n"
    "      this.__p = init.__p.map(function (pair) {\n"
    "        return [pair[0], pair[1]];\n"
    "      });\n"
    "    } else if (typeof init === 'object') {\n"
    "      Object.keys(init).forEach(function (k) {\n"
    "        this.__p.push([k, String(init[k])]);\n"
    "      }, this);\n"
    "    }\n"
    "  }\n"
    "  USP.prototype.get = function (k) {\n"
    "    k = String(k);\n"
    "    for (var i = 0; i < this.__p.length; i++)\n"
    "      if (this.__p[i][0] === k) return this.__p[i][1];\n"
    "    return null;\n"
    "  };\n"
    "  USP.prototype.getAll = function (k) {\n"
    "    k = String(k);\n"
    "    return this.__p.filter(function (pair) { return pair[0] === k; })\n"
    "      .map(function (pair) { return pair[1]; });\n"
    "  };\n"
    "  USP.prototype.has = function (k) { return this.get(k) !== null; };\n"
    "  USP.prototype.set = function (k, v) {\n"
    "    this.delete(k);\n"
    "    this.__p.push([String(k), String(v)]);\n"
    "  };\n"
    "  USP.prototype.append = function (k, v) {\n"
    "    this.__p.push([String(k), String(v)]);\n"
    "  };\n"
    "  USP.prototype.delete = function (k) {\n"
    "    k = String(k);\n"
    "    this.__p = this.__p.filter(function (pair) {\n"
    "      return pair[0] !== k;\n"
    "    });\n"
    "  };\n"
    "  USP.prototype.forEach = function (cb, self) {\n"
    "    this.__p.slice().forEach(function (pair) {\n"
    "      cb.call(self, pair[1], pair[0], this);\n"
    "    }, this);\n"
    "  };\n"
    "  USP.prototype.keys = function () {\n"
    "    return this.__p.map(function (pair) { return pair[0]; })[\n"
    "      Symbol.iterator]();\n"
    "  };\n"
    "  USP.prototype.values = function () {\n"
    "    return this.__p.map(function (pair) { return pair[1]; })[\n"
    "      Symbol.iterator]();\n"
    "  };\n"
    "  USP.prototype.entries = function () {\n"
    "    return this.__p.slice()[Symbol.iterator]();\n"
    "  };\n"
    "  USP.prototype[Symbol.iterator] = USP.prototype.entries;\n"
    "  USP.prototype.toString = function () {\n"
    "    return this.__p.map(function (pair) {\n"
    "      return encodeURIComponent(pair[0]) + '=' +\n"
    "             encodeURIComponent(pair[1]);\n"
    "    }).join('&');\n"
    "  };\n"
    "  Object.defineProperty(USP.prototype, 'size', {\n"
    "    get: function () { return this.__p.length; }\n"
    "  });\n"
    "  globalThis.URLSearchParams = USP;\n"
    "  function URL(input, base) {\n"
    "    var href = __nsResolveUrl(String(input),\n"
    "                              base !== undefined ? String(base)\n"
    "                                                 : undefined);\n"
    "    if (!href) throw new TypeError('Invalid URL: ' + input);\n"
    "    this.href = href;\n"
    "    this.origin = __nsUrlOrigin(href);\n"
    "    var m = href.match(/^([^:]+:)(?:\\/\\/([^\\/?#]*))?([^?#]*)"
    "(\\?[^#]*)?(#.*)?$/) || [];\n"
    "    this.protocol = m[1] || '';\n"
    "    this.host = m[2] || '';\n"
    "    var hi = this.host.indexOf(':');\n"
    "    this.hostname = hi < 0 ? this.host : this.host.slice(0, hi);\n"
    "    this.port = hi < 0 ? '' : this.host.slice(hi + 1);\n"
    "    this.pathname = m[3] || (m[2] !== undefined ? '/' : '');\n"
    "    this.search = m[4] || '';\n"
    "    this.hash = m[5] || '';\n"
    "    this.username = '';\n"
    "    this.password = '';\n"
    "    this.searchParams = new USP(this.search);\n"
    "  }\n"
    "  URL.prototype.toString = function () { return this.href; };\n"
    "  URL.prototype.toJSON = function () { return this.href; };\n"
    "  URL.canParse = function (input, base) {\n"
    "    try { new URL(input, base); return true; }\n"
    "    catch (e) { return false; }\n"
    "  };\n"
    "  globalThis.URL = URL;\n"
    "  globalThis.__nsInitStorage = function () {\n"
    "    var m = new Map();\n"
    "    var all = __nsStorageGetAll();\n"
    "    Object.keys(all).forEach(function (k) { m.set(k, all[k]); });\n"
    "    globalThis.localStorage = {\n"
    "      get length() { return m.size; },\n"
    "      key: function (i) {\n"
    "        var keys = Array.from(m.keys());\n"
    "        return i >= 0 && i < keys.length ? keys[i] : null;\n"
    "      },\n"
    "      getItem: function (k) {\n"
    "        k = String(k);\n"
    "        return m.has(k) ? m.get(k) : null;\n"
    "      },\n"
    "      setItem: function (k, v) {\n"
    "        k = String(k); v = String(v);\n"
    "        m.set(k, v);\n"
    "        __nsStorageSet(k, v);\n"
    "      },\n"
    "      removeItem: function (k) {\n"
    "        k = String(k);\n"
    "        m.delete(k);\n"
    "        __nsStorageRemove(k);\n"
    "      },\n"
    "      clear: function () { m.clear(); __nsStorageClear(); }\n"
    "    };\n"
    "  };\n"
    "})();\n";

void ns_v8_console_emit(const v8::FunctionCallbackInfo<v8::Value> &info,
                        const char *prefix)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_js *js = ns_v8_js_of(iso);
    if (!js) return;
    GString *line = g_string_new(prefix);
    if (line->len) g_string_append_c(line, ' ');
    for (int i = 0; i < info.Length(); i++) {
        if (i) g_string_append_c(line, ' ');
        v8::Local<v8::Value> v = info[i];
        if (v->IsObject() && !v->IsFunction()) {
            v8::Local<v8::String> json;
            v8::Local<v8::Context> ctx = iso->GetCurrentContext();
            if (v8::JSON::Stringify(ctx, v).ToLocal(&json) &&
                json->Length() > 0 && json->Length() < 4096) {
                g_string_append(line, ns_v8_utf8(iso, json).c_str());
                continue;
            }
        }
        g_string_append(line, ns_v8_utf8(iso, v).c_str());
    }
    ns_v8_log(js, line->str);
    g_string_free(line, TRUE);
}

void ns_v8_console_log(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_console_emit(info, "");
}

void ns_v8_console_warn(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_console_emit(info, "[warn]");
}

void ns_v8_console_error(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_console_emit(info, "[error]");
}

void ns_v8_noop_cb(const v8::FunctionCallbackInfo<v8::Value> &)
{
}

gboolean ns_v8_timer_fire(gpointer data)
{
    ns_v8_timer *t = static_cast<ns_v8_timer *>(data);
    ns_js *js = t->js;
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    v8::Local<v8::Function> fn = t->fn.Get(js->isolate);
    ns_v8_call_function(js, fn, 0, nullptr, "timer");
    if (t->repeat) return G_SOURCE_CONTINUE;
    js->timers.erase(t->id);
    return G_SOURCE_REMOVE;
}

void ns_v8_timer_destroy(gpointer data)
{
    ns_v8_timer *t = static_cast<ns_v8_timer *>(data);
    t->js->timers.erase(t->id);
    delete t;
}

void ns_v8_set_timer(const v8::FunctionCallbackInfo<v8::Value> &info,
                     gboolean repeat)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_js *js = ns_v8_js_of(iso);
    if (!js || info.Length() < 1) return;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Function> fn;
    if (info[0]->IsFunction()) {
        fn = info[0].As<v8::Function>();
    } else {
        std::string src = ns_v8_utf8(iso, info[0]);
        std::string wrapped = "(function(){" + src + "})";
        v8::Local<v8::Script> script;
        v8::TryCatch tc(iso);
        if (!v8::Script::Compile(ctx, ns_v8_str(iso, wrapped.c_str()))
                 .ToLocal(&script))
            return;
        v8::Local<v8::Value> v;
        if (!script->Run(ctx).ToLocal(&v) || !v->IsFunction()) return;
        fn = v.As<v8::Function>();
    }
    double ms = 0;
    if (info.Length() > 1 && info[1]->IsNumber())
        ms = info[1].As<v8::Number>()->Value();
    if (ms < 0 || !(ms == ms)) ms = 0;
    ns_v8_timer *t = new ns_v8_timer();
    t->js = js;
    t->id = js->next_timer_id++;
    t->repeat = repeat;
    t->fn.Reset(iso, fn);
    t->source_id = g_timeout_add_full(G_PRIORITY_DEFAULT, (guint)ms,
                                      ns_v8_timer_fire, t,
                                      ns_v8_timer_destroy);
    js->timers[t->id] = t;
    info.GetReturnValue().Set(t->id);
}

void ns_v8_set_timeout(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_set_timer(info, FALSE);
}

void ns_v8_set_interval(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_set_timer(info, TRUE);
}

void ns_v8_clear_timer(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_of(info.GetIsolate());
    if (!js || info.Length() < 1 || !info[0]->IsNumber()) return;
    int id = (int)info[0].As<v8::Number>()->Value();
    auto it = js->timers.find(id);
    if (it != js->timers.end()) g_source_remove(it->second->source_id);
}

void ns_v8_queue_microtask(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    if (info.Length() < 1 || !info[0]->IsFunction()) return;
    iso->EnqueueMicrotask(info[0].As<v8::Function>());
}

void ns_v8_request_animation_frame(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_js *js = ns_v8_js_of(iso);
    if (!js || info.Length() < 1 || !info[0]->IsFunction()) return;
    js->raf_queue.emplace_back(iso, info[0].As<v8::Function>());
    info.GetReturnValue().Set((int)js->raf_queue.size());
}

void ns_v8_cancel_animation_frame(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_of(info.GetIsolate());
    if (!js || info.Length() < 1 || !info[0]->IsNumber()) return;
    size_t id = (size_t)info[0].As<v8::Number>()->Value();
    if (id >= 1 && id <= js->raf_queue.size())
        js->raf_queue[id - 1].Reset();
}

void ns_v8_add_event_listener(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_js *js = ns_v8_js_of(iso);
    if (!js || info.Length() < 2 || !info[1]->IsFunction()) return;
    ns_v8_listener l;
    l.type = ns_v8_utf8(iso, info[0]);
    l.fn.Reset(iso, info[1].As<v8::Function>());
    js->listeners.push_back(std::move(l));
}

void ns_v8_remove_event_listener(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_js *js = ns_v8_js_of(iso);
    if (!js || info.Length() < 2 || !info[1]->IsFunction()) return;
    std::string type = ns_v8_utf8(iso, info[0]);
    v8::Local<v8::Function> fn = info[1].As<v8::Function>();
    for (auto it = js->listeners.begin(); it != js->listeners.end(); ++it) {
        if (it->type == type && it->fn.Get(iso) == fn) {
            js->listeners.erase(it);
            return;
        }
    }
}

void ns_v8_dispatch_event_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    info.GetReturnValue().Set(true);
}

void ns_v8_alert(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_js *js = ns_v8_js_of(iso);
    if (!js) return;
    std::string msg =
        info.Length() > 0 ? ns_v8_utf8(iso, info[0]) : std::string();
    char *line = g_strdup_printf("[alert] %s", msg.c_str());
    ns_v8_log(js, line);
    g_free(line);
}

void ns_v8_confirm(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    info.GetReturnValue().Set(false);
}

void ns_v8_prompt(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    info.GetReturnValue().SetNull();
}

void ns_v8_btoa(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    if (info.Length() < 1) return;
    std::string s = ns_v8_utf8(iso, info[0]);
    char *b64 = g_base64_encode((const guchar *)s.data(), s.size());
    info.GetReturnValue().Set(ns_v8_str(iso, b64));
    g_free(b64);
}

void ns_v8_atob(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    if (info.Length() < 1) return;
    std::string s = ns_v8_utf8(iso, info[0]);
    gsize out_len = 0;
    guchar *raw = g_base64_decode(s.c_str(), &out_len);
    if (!raw) return;
    v8::Local<v8::String> out;
    if (v8::String::NewFromOneByte(iso, (const uint8_t *)raw,
                                   v8::NewStringType::kNormal, (int)out_len)
            .ToLocal(&out))
        info.GetReturnValue().Set(out);
    g_free(raw);
}

void ns_v8_performance_now(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_of(info.GetIsolate());
    if (!js) return;
    info.GetReturnValue().Set(
        (double)(g_get_monotonic_time() - js->origin_us) / 1000.0);
}

std::string ns_v8_url_part(const char *url, char part)
{
    if (!url) return "";
    const char *scheme_end = strstr(url, "://");
    if (part == 'p') {
        const char *colon = strchr(url, ':');
        return colon ? std::string(url, colon - url + 1) : "";
    }
    const char *rest = scheme_end ? scheme_end + 3 : url;
    const char *path = strchr(rest, '/');
    const char *query = strchr(rest, '?');
    const char *frag = strchr(rest, '#');
    if (part == 'h') {
        const char *end = path ? path : (query ? query : (frag ? frag : rest + strlen(rest)));
        return std::string(rest, end - rest);
    }
    if (part == 'a') {
        if (!path || (frag && frag < path)) return "/";
        const char *end = query && query > path ? query
                          : (frag && frag > path ? frag : path + strlen(path));
        return std::string(path, end - path);
    }
    if (part == 'q') {
        if (!query || (frag && frag < query)) return "";
        const char *end = frag ? frag : query + strlen(query);
        return std::string(query, end - query);
    }
    if (part == 'f') return frag ? std::string(frag) : "";
    return "";
}

void ns_v8_location_get(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_js *js = ns_v8_js_of(iso);
    if (!js) return;
    char part = (char)(intptr_t)info.Data().As<v8::External>()->Value();
    const char *url = js->current_url ? js->current_url : "about:blank";
    std::string out;
    if (part == 'H') {
        out = url;
    } else if (part == 'o') {
        char *origin = ns_url_origin_from(url);
        out = origin ? origin : "null";
        g_free(origin);
    } else if (part == 'n') {
        char *host = ns_url_host_from(url);
        out = host ? host : "";
        g_free(host);
    } else {
        out = ns_v8_url_part(url, part);
    }
    info.GetReturnValue().Set(ns_v8_str(iso, out.c_str()));
}

void ns_v8_navigate_to(ns_js *js, const char *target, gboolean reload)
{
    if (!js->nav_cb || !target) return;
    char *abs = ns_url_resolve(js->current_url, target);
    js->nav_cb(abs ? abs : target, reload, js->nav_user_data);
    g_free(abs);
}

void ns_v8_location_set_href(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_js *js = ns_v8_js_of(iso);
    if (!js || info.Length() < 1) return;
    std::string url = ns_v8_utf8(iso, info[0]);
    ns_v8_navigate_to(js, url.c_str(), FALSE);
}

void ns_v8_location_assign(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_v8_location_set_href(info);
}

void ns_v8_location_reload(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_of(info.GetIsolate());
    if (!js) return;
    ns_v8_navigate_to(js, js->current_url ? js->current_url : "about:blank",
                      TRUE);
}

void ns_v8_document_title_get(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_js *js = ns_v8_js_of(iso);
    if (!js || !js->current_doc) {
        info.GetReturnValue().Set(ns_v8_str(iso, ""));
        return;
    }
    ns_node *title = ns_node_find_first_element(js->current_doc, "title");
    GString *text = g_string_new(NULL);
    if (title)
        for (ns_node *c = title->first_child; c; c = c->next_sibling)
            if (c->kind == NS_NODE_TEXT && c->text)
                g_string_append(text, c->text);
    info.GetReturnValue().Set(ns_v8_str(iso, text->str));
    g_string_free(text, TRUE);
}

void ns_v8_document_ready_state_get(
    const v8::FunctionCallbackInfo<v8::Value> &info)
{
    ns_js *js = ns_v8_js_of(info.GetIsolate());
    const char *state = "loading";
    if (js && js->ready_state == 1) state = "interactive";
    if (js && js->ready_state >= 2) state = "complete";
    info.GetReturnValue().Set(ns_v8_str(info.GetIsolate(), state));
}

void ns_v8_document_cookie_get(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    ns_js *js = ns_v8_js_of(iso);
    if (!js || !js->current_url) {
        info.GetReturnValue().Set(ns_v8_str(iso, ""));
        return;
    }
    char *jar = ns_net_cookies_for_js(js->current_url);
    info.GetReturnValue().Set(ns_v8_str(iso, jar ? jar : ""));
    g_free(jar);
}

void ns_v8_empty_array_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    info.GetReturnValue().Set(v8::Array::New(info.GetIsolate()));
}

void ns_v8_promise_reject(v8::PromiseRejectMessage message)
{
    if (message.GetEvent() != v8::kPromiseRejectWithNoHandler) return;
    v8::Isolate *iso = v8::Isolate::GetCurrent();
    if (!iso) return;
    ns_js *js = ns_v8_js_of(iso);
    if (!js) return;
    v8::HandleScope hs(iso);
    std::string msg = ns_v8_utf8(iso, message.GetValue());
    char *line = g_strdup_printf("[unhandled rejection] %s", msg.c_str());
    ns_v8_log(js, line);
    g_free(line);
}

void ns_v8_bind_fn(ns_js *js, v8::Local<v8::Object> obj, const char *name,
                   v8::FunctionCallback cb)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Function> fn = v8::Function::New(ctx, cb).ToLocalChecked();
    obj->Set(ctx, ns_v8_str(iso, name), fn).Check();
}

const char ns_v8_bootstrap_src[] =
    "(function () {\n"
    "  'use strict';\n"
    "  function makeStorage() {\n"
    "    var m = new Map();\n"
    "    return {\n"
    "      get length() { return m.size; },\n"
    "      key: function (i) { var k = Array.from(m.keys()); return i >= 0 && i < k.length ? k[i] : null; },\n"
    "      getItem: function (k) { k = String(k); return m.has(k) ? m.get(k) : null; },\n"
    "      setItem: function (k, v) { m.set(String(k), String(v)); },\n"
    "      removeItem: function (k) { m.delete(String(k)); },\n"
    "      clear: function () { m.clear(); }\n"
    "    };\n"
    "  }\n"
    "  globalThis.localStorage = makeStorage();\n"
    "  globalThis.sessionStorage = makeStorage();\n"
    "  globalThis.matchMedia = function (q) {\n"
    "    return { matches: false, media: String(q), onchange: null,\n"
    "             addListener: function () {}, removeListener: function () {},\n"
    "             addEventListener: function () {}, removeEventListener: function () {},\n"
    "             dispatchEvent: function () { return false; } };\n"
    "  };\n"
    "  globalThis.getComputedStyle = function () {\n"
    "    return { getPropertyValue: function () { return ''; } };\n"
    "  };\n"
    "  globalThis.requestIdleCallback = function (cb) {\n"
    "    return setTimeout(function () {\n"
    "      cb({ didTimeout: false, timeRemaining: function () { return 50; } });\n"
    "    }, 1);\n"
    "  };\n"
    "  globalThis.cancelIdleCallback = function (id) { clearTimeout(id); };\n"
    "})();\n";

void ns_v8_install_base(ns_js *js)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Object> global = ctx->Global();

    global->Set(ctx, ns_v8_str(iso, "window"), global).Check();
    global->Set(ctx, ns_v8_str(iso, "self"), global).Check();
    global->Set(ctx, ns_v8_str(iso, "top"), global).Check();
    global->Set(ctx, ns_v8_str(iso, "parent"), global).Check();
    global->Set(ctx, ns_v8_str(iso, "frames"), global).Check();

    v8::Local<v8::Object> console = v8::Object::New(iso);
    ns_v8_bind_fn(js, console, "log", ns_v8_console_log);
    ns_v8_bind_fn(js, console, "info", ns_v8_console_log);
    ns_v8_bind_fn(js, console, "debug", ns_v8_console_log);
    ns_v8_bind_fn(js, console, "trace", ns_v8_console_log);
    ns_v8_bind_fn(js, console, "warn", ns_v8_console_warn);
    ns_v8_bind_fn(js, console, "error", ns_v8_console_error);
    ns_v8_bind_fn(js, console, "group", ns_v8_noop_cb);
    ns_v8_bind_fn(js, console, "groupEnd", ns_v8_noop_cb);
    ns_v8_bind_fn(js, console, "count", ns_v8_noop_cb);
    ns_v8_bind_fn(js, console, "time", ns_v8_noop_cb);
    ns_v8_bind_fn(js, console, "timeEnd", ns_v8_noop_cb);
    ns_v8_bind_fn(js, console, "table", ns_v8_console_log);
    ns_v8_bind_fn(js, console, "assert", ns_v8_noop_cb);
    global->Set(ctx, ns_v8_str(iso, "console"), console).Check();

    ns_v8_bind_fn(js, global, "setTimeout", ns_v8_set_timeout);
    ns_v8_bind_fn(js, global, "setInterval", ns_v8_set_interval);
    ns_v8_bind_fn(js, global, "clearTimeout", ns_v8_clear_timer);
    ns_v8_bind_fn(js, global, "clearInterval", ns_v8_clear_timer);
    ns_v8_bind_fn(js, global, "queueMicrotask", ns_v8_queue_microtask);
    ns_v8_bind_fn(js, global, "requestAnimationFrame",
                  ns_v8_request_animation_frame);
    ns_v8_bind_fn(js, global, "cancelAnimationFrame",
                  ns_v8_cancel_animation_frame);
    ns_v8_bind_fn(js, global, "addEventListener", ns_v8_add_event_listener);
    ns_v8_bind_fn(js, global, "removeEventListener",
                  ns_v8_remove_event_listener);
    ns_v8_bind_fn(js, global, "dispatchEvent", ns_v8_dispatch_event_cb);
    ns_v8_bind_fn(js, global, "alert", ns_v8_alert);
    ns_v8_bind_fn(js, global, "confirm", ns_v8_confirm);
    ns_v8_bind_fn(js, global, "prompt", ns_v8_prompt);
    ns_v8_bind_fn(js, global, "btoa", ns_v8_btoa);
    ns_v8_bind_fn(js, global, "atob", ns_v8_atob);

    global->Set(ctx, ns_v8_str(iso, "innerWidth"),
                v8::Number::New(iso, 1280)).Check();
    global->Set(ctx, ns_v8_str(iso, "innerHeight"),
                v8::Number::New(iso, 720)).Check();
    global->Set(ctx, ns_v8_str(iso, "devicePixelRatio"),
                v8::Number::New(iso, 1)).Check();

    v8::Local<v8::Object> navigator = v8::Object::New(iso);
    navigator->Set(ctx, ns_v8_str(iso, "userAgent"),
                   ns_v8_str(iso, "Mozilla/5.0 (X11; Linux x86_64) "
                                  "Nordstjernen")).Check();
    navigator->Set(ctx, ns_v8_str(iso, "appName"),
                   ns_v8_str(iso, "Nordstjernen")).Check();
    navigator->Set(ctx, ns_v8_str(iso, "platform"),
                   ns_v8_str(iso, "Linux x86_64")).Check();
    navigator->Set(ctx, ns_v8_str(iso, "cookieEnabled"),
                   v8::Boolean::New(iso, true)).Check();
    navigator->Set(ctx, ns_v8_str(iso, "onLine"),
                   v8::Boolean::New(iso, true)).Check();
    char **langs = ns_net_navigator_languages();
    v8::Local<v8::Array> lang_arr = v8::Array::New(iso);
    for (guint i = 0; langs && langs[i]; i++)
        lang_arr->Set(ctx, i, ns_v8_str(iso, langs[i])).Check();
    navigator->Set(ctx, ns_v8_str(iso, "languages"), lang_arr).Check();
    navigator
        ->Set(ctx, ns_v8_str(iso, "language"),
              ns_v8_str(iso, langs && langs[0] ? langs[0] : "en"))
        .Check();
    g_strfreev(langs);
    global->Set(ctx, ns_v8_str(iso, "navigator"), navigator).Check();

    v8::Local<v8::Object> performance = v8::Object::New(iso);
    ns_v8_bind_fn(js, performance, "now", ns_v8_performance_now);
    ns_v8_bind_fn(js, performance, "mark", ns_v8_noop_cb);
    ns_v8_bind_fn(js, performance, "measure", ns_v8_noop_cb);
    ns_v8_bind_fn(js, performance, "getEntriesByType", ns_v8_empty_array_cb);
    ns_v8_bind_fn(js, performance, "getEntriesByName", ns_v8_empty_array_cb);
    performance->Set(ctx, ns_v8_str(iso, "timeOrigin"),
                     v8::Number::New(iso, (double)js->origin_us / 1000.0))
        .Check();
    global->Set(ctx, ns_v8_str(iso, "performance"), performance).Check();

    v8::Local<v8::Object> location = v8::Object::New(iso);
    struct {
        const char *name;
        char part;
    } parts[] = {
        {"href", 'H'},     {"protocol", 'p'}, {"host", 'h'},
        {"hostname", 'n'}, {"pathname", 'a'}, {"search", 'q'},
        {"hash", 'f'},     {"origin", 'o'},
    };
    for (auto &p : parts) {
        v8::Local<v8::Function> getter =
            v8::Function::New(ctx, ns_v8_location_get,
                              v8::External::New(iso, (void *)(intptr_t)p.part))
                .ToLocalChecked();
        v8::Local<v8::Function> setter;
        if (p.part == 'H')
            setter = v8::Function::New(ctx, ns_v8_location_set_href)
                         .ToLocalChecked();
        location->SetAccessorProperty(ns_v8_str(iso, p.name), getter, setter);
    }
    ns_v8_bind_fn(js, location, "assign", ns_v8_location_assign);
    ns_v8_bind_fn(js, location, "replace", ns_v8_location_assign);
    ns_v8_bind_fn(js, location, "reload", ns_v8_location_reload);
    v8::Local<v8::Function> href_getter =
        v8::Function::New(ctx, ns_v8_location_get,
                          v8::External::New(iso, (void *)(intptr_t)'H'))
            .ToLocalChecked();
    location->SetAccessorProperty(ns_v8_str(iso, "toString"), href_getter,
                                  v8::Local<v8::Function>());
    global->Set(ctx, ns_v8_str(iso, "location"), location).Check();

    ns_v8_eval(js, ns_v8_bootstrap_src, -1, "v8-bootstrap", nullptr);

    v8::Local<v8::Function> element_fn;
    if (js->node_tmpl.Get(iso)->GetFunction(ctx).ToLocal(&element_fn)) {
        global->Set(ctx, ns_v8_str(iso, "Element"), element_fn).Check();
        ns_v8_bind_fn(js, global, "__nsComputedStyle",
                      ns_v8_computed_style_cb);
        ns_v8_bind_fn(js, global, "__nsFetch", ns_v8_fetch_cb);
        ns_v8_bind_fn(js, global, "__nsFetchSync", ns_v8_fetch_sync_cb);
        ns_v8_bind_fn(js, global, "__nsResolveUrl", ns_v8_resolve_url_cb);
        ns_v8_bind_fn(js, global, "__nsUrlOrigin", ns_v8_url_origin_cb);
        ns_v8_bind_fn(js, global, "__nsStorageGetAll",
                      ns_v8_storage_get_all_cb);
        ns_v8_bind_fn(js, global, "__nsStorageSet", ns_v8_storage_set_cb);
        ns_v8_bind_fn(js, global, "__nsStorageRemove",
                      ns_v8_storage_remove_cb);
        ns_v8_bind_fn(js, global, "__nsStorageClear",
                      ns_v8_storage_clear_cb);
        global->Set(ctx, ns_v8_str(iso, "Worker"),
                    v8::Function::New(ctx, ns_v8_worker_ctor_cb)
                        .ToLocalChecked()).Check();
        ns_v8_install_crypto(js);
        ns_v8_eval(js, ns_v8_dom_bootstrap_src, -1, "v8-dom-bootstrap",
                   nullptr);
        ns_v8_eval(js, ns_v8_net_bootstrap_src, -1, "v8-net-bootstrap",
                   nullptr);
    }
}

void ns_v8_install_document(ns_js *js, const char *base_url)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Object> global = ctx->Global();

    v8::Local<v8::Object> document = v8::Object::New(iso);
    document->Set(ctx, ns_v8_str(iso, "URL"), ns_v8_str(iso, base_url))
        .Check();
    document->Set(ctx, ns_v8_str(iso, "documentURI"),
                  ns_v8_str(iso, base_url)).Check();
    document->Set(ctx, ns_v8_str(iso, "baseURI"), ns_v8_str(iso, base_url))
        .Check();
    document->Set(ctx, ns_v8_str(iso, "defaultView"), global).Check();
    document->Set(ctx, ns_v8_str(iso, "characterSet"), ns_v8_str(iso, "UTF-8"))
        .Check();
    document->Set(ctx, ns_v8_str(iso, "compatMode"),
                  ns_v8_str(iso, "CSS1Compat")).Check();
    document->SetAccessorProperty(
        ns_v8_str(iso, "title"),
        v8::Function::New(ctx, ns_v8_document_title_get).ToLocalChecked(),
        v8::Function::New(ctx, ns_v8_document_title_set).ToLocalChecked());
    document->SetAccessorProperty(
        ns_v8_str(iso, "readyState"),
        v8::Function::New(ctx, ns_v8_document_ready_state_get)
            .ToLocalChecked(),
        v8::Local<v8::Function>());
    document->SetAccessorProperty(
        ns_v8_str(iso, "cookie"),
        v8::Function::New(ctx, ns_v8_document_cookie_get).ToLocalChecked(),
        v8::Function::New(ctx, ns_v8_noop_cb).ToLocalChecked());
    ns_v8_bind_fn(js, document, "addEventListener",
                  ns_v8_add_event_listener);
    ns_v8_bind_fn(js, document, "removeEventListener",
                  ns_v8_remove_event_listener);
    ns_v8_bind_fn(js, document, "dispatchEvent", ns_v8_dispatch_event_cb);
    ns_v8_bind_fn(js, document, "getElementById",
                  ns_v8_doc_get_element_by_id);
    ns_v8_bind_fn(js, document, "querySelector", ns_v8_doc_query_selector);
    ns_v8_bind_fn(js, document, "querySelectorAll",
                  ns_v8_doc_query_selector_all);
    ns_v8_bind_fn(js, document, "getElementsByTagName", ns_v8_doc_get_by_tag);
    ns_v8_bind_fn(js, document, "getElementsByClassName",
                  ns_v8_doc_get_by_class);
    ns_v8_bind_fn(js, document, "createElement", ns_v8_doc_create_element);
    ns_v8_bind_fn(js, document, "createTextNode", ns_v8_doc_create_text);
    ns_v8_bind_fn(js, document, "createComment", ns_v8_doc_create_comment);
    ns_v8_bind_fn(js, document, "createDocumentFragment",
                  ns_v8_doc_create_fragment);
    struct {
        const char *name;
        const char *tag;
    } roots[] = {{"documentElement", "html"}, {"body", "body"},
                 {"head", "head"}};
    for (auto &r : roots)
        document->SetAccessorProperty(
            ns_v8_str(iso, r.name),
            v8::Function::New(ctx, ns_v8_doc_root_get,
                v8::External::New(iso, (void *)r.tag)).ToLocalChecked());
    document->SetAccessorProperty(
        ns_v8_str(iso, "activeElement"),
        v8::Function::New(ctx, ns_v8_doc_active_element).ToLocalChecked());
    global->Set(ctx, ns_v8_str(iso, "document"), document).Check();
    js->document.Reset(iso, document);
}

struct ns_v8_script_task {
    ns_node *node;
    int phase;
};

gboolean ns_v8_script_type_runs(const char *type)
{
    if (!type || !*type) return TRUE;
    char *t = g_ascii_strdown(type, -1);
    g_strstrip(t);
    gboolean runs = strcmp(t, "text/javascript") == 0 ||
                    strcmp(t, "application/javascript") == 0 ||
                    strcmp(t, "text/ecmascript") == 0 ||
                    strcmp(t, "application/ecmascript") == 0;
    g_free(t);
    return runs;
}

void ns_v8_collect_scripts(ns_node *n, std::vector<ns_v8_script_task> &out)
{
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT && c->name &&
            strcmp(c->name, "script") == 0) {
            const char *type = ns_element_get_attr(c, "type");
            if (type && g_ascii_strcasecmp(type, "module") == 0) {
                out.push_back({c, 3});
            } else if (ns_v8_script_type_runs(type)) {
                int phase = 0;
                if (ns_element_get_attr(c, "async")) phase = 2;
                else if (ns_element_get_attr(c, "defer") &&
                         ns_element_get_attr(c, "src")) phase = 1;
                out.push_back({c, phase});
            }
        }
        ns_v8_collect_scripts(c, out);
    }
}

void ns_v8_run_one_script(ns_js *js, ns_node *script, const char *base_url)
{
    const char *src_attr = ns_element_get_attr(script, "src");
    if (src_attr && *src_attr) {
        char *url = ns_url_resolve(base_url, src_attr);
        if (!url) return;
        ns_response *resp = ns_net_fetch_blocking(url, NULL, NULL);
        if (resp && resp->status >= 200 && resp->status < 400 && resp->body &&
            resp->body->len) {
            ns_v8_eval(js, (const char *)resp->body->data,
                       (gssize)resp->body->len, url, nullptr);
        } else {
            char *line = g_strdup_printf(
                "[error] script fetch failed: %s (status %ld)", url,
                resp ? resp->status : 0L);
            ns_v8_log(js, line);
            g_free(line);
        }
        if (resp) ns_response_free(resp);
        g_free(url);
        return;
    }
    GString *text = g_string_new(NULL);
    for (ns_node *c = script->first_child; c; c = c->next_sibling)
        if ((c->kind == NS_NODE_TEXT || c->kind == NS_NODE_COMMENT) && c->text)
            g_string_append(text, c->text);
    if (text->len) {
        char *origin = g_strdup_printf("%s (inline)", base_url);
        ns_v8_eval(js, text->str, (gssize)text->len, origin, nullptr);
        g_free(origin);
    }
    g_string_free(text, TRUE);
}

void ns_v8_run_phase(ns_js *js, std::vector<ns_v8_script_task> &tasks,
                     int phase, const char *base_url)
{
    for (auto &t : tasks)
        if (t.phase == phase) ns_v8_run_one_script(js, t.node, base_url);
}

v8::MaybeLocal<v8::Module> ns_v8_module_compile(ns_js *js, const char *src,
                                                gssize len, const char *url)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::String> code;
    if (!v8::String::NewFromUtf8(iso, src ? src : "",
                                 v8::NewStringType::kNormal,
                                 len < 0 ? -1 : (int)len)
             .ToLocal(&code))
        return v8::MaybeLocal<v8::Module>();
    v8::ScriptOrigin origin(ns_v8_str(iso, url), 0, 0, false, -1,
                            v8::Local<v8::Value>(), false, false, true);
    v8::ScriptCompiler::Source source(code, origin);
    v8::Local<v8::Module> mod;
    if (!v8::ScriptCompiler::CompileModule(iso, &source).ToLocal(&mod))
        return v8::MaybeLocal<v8::Module>();
    js->module_urls[mod->GetIdentityHash()] = url;
    return mod;
}

v8::MaybeLocal<v8::Module> ns_v8_module_load(ns_js *js, const char *url)
{
    auto it = js->modules.find(url);
    if (it != js->modules.end())
        return it->second.Get(js->isolate);
    ns_response *resp = ns_net_fetch_blocking(url, NULL, NULL);
    if (!resp || resp->status >= 400 || !resp->body || !resp->body->len) {
        char *line = g_strdup_printf("[error] module fetch failed: %s"
                                     " (status %ld)",
                                     url, resp ? resp->status : 0L);
        ns_v8_log(js, line);
        g_free(line);
        if (resp) ns_response_free(resp);
        return v8::MaybeLocal<v8::Module>();
    }
    v8::MaybeLocal<v8::Module> maybe =
        ns_v8_module_compile(js, (const char *)resp->body->data,
                             (gssize)resp->body->len, url);
    ns_response_free(resp);
    v8::Local<v8::Module> mod;
    if (maybe.ToLocal(&mod))
        js->modules[url].Reset(js->isolate, mod);
    return maybe;
}

v8::MaybeLocal<v8::Module> ns_v8_module_resolve(
    v8::Local<v8::Context>, v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray>, v8::Local<v8::Module> referrer)
{
    v8::Isolate *iso = v8::Isolate::GetCurrent();
    ns_js *js = ns_v8_js_of(iso);
    if (!js) return v8::MaybeLocal<v8::Module>();
    std::string spec = ns_v8_utf8(iso, specifier);
    std::string ref_url = js->current_url ? js->current_url : "";
    auto it = js->module_urls.find(referrer->GetIdentityHash());
    if (it != js->module_urls.end()) ref_url = it->second;
    char *abs = ns_url_resolve(ref_url.c_str(), spec.c_str());
    if (!abs) {
        iso->ThrowException(v8::Exception::TypeError(
            ns_v8_str(iso, "cannot resolve module specifier")));
        return v8::MaybeLocal<v8::Module>();
    }
    v8::MaybeLocal<v8::Module> m = ns_v8_module_load(js, abs);
    if (m.IsEmpty()) {
        char *line = g_strdup_printf("module load failed: %s", abs);
        iso->ThrowException(
            v8::Exception::TypeError(ns_v8_str(iso, line)));
        g_free(line);
    }
    g_free(abs);
    return m;
}

gboolean ns_v8_module_run(ns_js *js, v8::Local<v8::Module> mod,
                          const char *origin)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::TryCatch tc(iso);
    bool ok = false;
    if (mod->InstantiateModule(ctx, ns_v8_module_resolve).To(&ok) && ok) {
        v8::Local<v8::Value> result;
        if (mod->Evaluate(ctx).ToLocal(&result)) {
            ns_v8_settle(js);
            if (result->IsPromise()) {
                v8::Local<v8::Promise> p = result.As<v8::Promise>();
                if (p->State() == v8::Promise::kRejected) {
                    std::string msg = ns_v8_utf8(iso, p->Result());
                    char *line = g_strdup_printf("[error] %s: %s", origin,
                                                 msg.c_str());
                    ns_v8_log(js, line);
                    g_free(line);
                }
            }
            return TRUE;
        }
    }
    if (tc.HasCaught()) ns_v8_report_try_catch(js, tc, origin);
    ns_v8_settle(js);
    return FALSE;
}

void ns_v8_run_module_task(ns_js *js, ns_node *script, const char *base_url,
                           int index)
{
    const char *src_attr = ns_element_get_attr(script, "src");
    v8::Local<v8::Module> mod;
    char *origin = NULL;
    if (src_attr && *src_attr) {
        char *url = ns_url_resolve(base_url, src_attr);
        if (!url) return;
        if (!ns_v8_module_load(js, url).ToLocal(&mod)) {
            g_free(url);
            return;
        }
        origin = url;
    } else {
        GString *text = g_string_new(NULL);
        for (ns_node *c = script->first_child; c; c = c->next_sibling)
            if ((c->kind == NS_NODE_TEXT || c->kind == NS_NODE_COMMENT) &&
                c->text)
                g_string_append(text, c->text);
        if (!text->len) {
            g_string_free(text, TRUE);
            return;
        }
        origin = g_strdup_printf("%s#module-%d", base_url, index);
        gboolean compiled =
            ns_v8_module_compile(js, text->str, (gssize)text->len, origin)
                .ToLocal(&mod);
        g_string_free(text, TRUE);
        if (!compiled) {
            g_free(origin);
            return;
        }
    }
    ns_v8_module_run(js, mod, origin);
    g_free(origin);
}

v8::MaybeLocal<v8::Promise> ns_v8_dynamic_import(
    v8::Local<v8::Context> context, v8::Local<v8::Data>,
    v8::Local<v8::Value> resource_name, v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray>)
{
    v8::Isolate *iso = v8::Isolate::GetCurrent();
    ns_js *js = ns_v8_js_of(iso);
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver))
        return v8::MaybeLocal<v8::Promise>();
    if (!js) {
        resolver
            ->Reject(context, v8::Exception::TypeError(
                                  ns_v8_str(iso, "no engine")))
            .Check();
        return resolver->GetPromise();
    }
    std::string spec = ns_v8_utf8(iso, specifier);
    std::string ref = ns_v8_utf8(iso, resource_name);
    if (ref.empty() && js->current_url) ref = js->current_url;
    char *abs = ns_url_resolve(ref.c_str(), spec.c_str());
    v8::Local<v8::Module> mod;
    if (!abs || !ns_v8_module_load(js, abs).ToLocal(&mod)) {
        char *line = g_strdup_printf("dynamic import failed: %s",
                                     spec.c_str());
        resolver
            ->Reject(context,
                     v8::Exception::TypeError(ns_v8_str(iso, line)))
            .Check();
        g_free(line);
        g_free(abs);
        return resolver->GetPromise();
    }
    if (ns_v8_module_run(js, mod, abs) &&
        mod->GetStatus() == v8::Module::kEvaluated)
        resolver->Resolve(context, mod->GetModuleNamespace()).Check();
    else
        resolver
            ->Reject(context, v8::Exception::TypeError(ns_v8_str(iso,
                         "module evaluation failed")))
            .Check();
    g_free(abs);
    return resolver->GetPromise();
}

void ns_v8_import_meta(v8::Local<v8::Context> context,
                       v8::Local<v8::Module> mod, v8::Local<v8::Object> meta)
{
    v8::Isolate *iso = v8::Isolate::GetCurrent();
    ns_js *js = ns_v8_js_of(iso);
    if (!js) return;
    std::string url = js->current_url ? js->current_url : "";
    auto it = js->module_urls.find(mod->GetIdentityHash());
    if (it != js->module_urls.end()) url = it->second;
    meta->Set(context, ns_v8_str(iso, "url"), ns_v8_str(iso, url.c_str()))
        .Check();
}

}

const char *
ns_js_engine_version(void)
{
    static char *version;
    if (!version)
        version = g_strdup_printf("V8 %s (experimental backend)",
                                  v8::V8::GetVersion());
    return version;
}

ns_js *
ns_js_new(ns_js_log_cb log_cb, gpointer log_user_data,
          ns_js_mutated_cb mut_cb, gpointer mut_user_data,
          ns_js_navigate_cb nav_cb, gpointer nav_user_data,
          const ns_js_navigation_timing *navigation_timing)
{
    ns_v8_global_init();
    ns_js *js = new ns_js();
    js->log_cb = log_cb;
    js->log_user_data = log_user_data;
    js->mut_cb = mut_cb;
    js->mut_user_data = mut_user_data;
    js->nav_cb = nav_cb;
    js->nav_user_data = nav_user_data;
    js->current_url = NULL;
    js->partition = NULL;
    js->early_inject_src = NULL;
    js->current_doc = NULL;
    js->focused = NULL;
    js->csp_headers = g_ptr_array_new_with_free_func(g_free);
    js->origin_us = navigation_timing && navigation_timing->origin_us
                        ? navigation_timing->origin_us
                        : g_get_monotonic_time();
    js->next_timer_id = 1;
    js->pump_depth = 0;
    js->ready_state = 0;
    js->mutated = FALSE;

    js->allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = js->allocator;
    js->isolate = v8::Isolate::New(params);
    js->isolate->SetData(0, js);
    js->isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
    js->isolate->SetPromiseRejectCallback(ns_v8_promise_reject);
    js->isolate->SetHostImportModuleDynamicallyCallback(ns_v8_dynamic_import);
    js->isolate->SetHostInitializeImportMetaObjectCallback(
        ns_v8_import_meta);

    v8::Isolate::Scope iso_scope(js->isolate);
    v8::HandleScope hs(js->isolate);
    v8::Local<v8::Context> ctx = v8::Context::New(js->isolate);
    js->context.Reset(js->isolate, ctx);
    v8::Context::Scope ctx_scope(ctx);
    ns_v8_make_node_template(js);
    ns_v8_install_base(js);
    return js;
}

void
ns_js_free(ns_js *js)
{
    if (!js) return;
    while (!js->timers.empty())
        g_source_remove(js->timers.begin()->second->source_id);
    {
        v8::Isolate::Scope iso_scope(js->isolate);
        for (size_t i = 0; i < js->wraps.size(); i++) {
            ns_v8_wrap *w = js->wraps[i];
            if (w->owned && w->node && !w->node->parent)
                ns_node_free(w->node);
        }
        for (ns_v8_wrap *w : js->wraps) {
            if (w->node) {
                w->node->js_wrapper = NULL;
                w->node->js_invalidate = NULL;
            }
            w->handle.Reset();
            w->listeners.clear();
            delete w;
        }
        for (ns_v8_request *r : js->pending_requests) {
            r->js = NULL;
            r->resolver.Reset();
            g_cancellable_cancel(r->cancellable);
        }
        js->pending_requests.clear();
        for (ns_v8_worker *w : js->workers) {
            w->owner = NULL;
            ns_v8_worker_free(w);
        }
        js->workers.clear();
        for (auto &entry : js->canvases) {
            ns_v8_canvas *c = entry.second;
            c->ctx_obj.Reset();
            cairo_destroy(c->cr);
            cairo_surface_destroy(c->surf);
            delete c;
        }
        js->canvases.clear();
        js->wraps.clear();
        for (auto &m : js->modules) m.second.Reset();
        js->modules.clear();
        js->module_urls.clear();
        js->raf_queue.clear();
        js->listeners.clear();
        js->node_tmpl.Reset();
        js->document.Reset();
        js->context.Reset();
    }
    ns_v8_storage_flush(js);
    if (js->local_storage) g_hash_table_destroy(js->local_storage);
    g_free(js->local_storage_path);
    js->isolate->Dispose();
    delete js->allocator;
    g_ptr_array_free(js->csp_headers, TRUE);
    g_free(js->current_url);
    g_free(js->partition);
    g_free(js->early_inject_src);
    delete js;
}

void
ns_js_run_scripts_in_doc(ns_js *js, ns_node *doc, const char *base_url_borrowed)
{
    if (!js || !doc) return;
    if (js->pump_depth) return;
    g_autofree char *base_url =
        g_strdup(base_url_borrowed && *base_url_borrowed ? base_url_borrowed
                                                         : "about:blank");
    js->current_doc = doc;
    g_free(js->current_url);
    js->current_url = g_strdup(base_url);
    g_free(js->partition);
    js->partition = ns_url_origin_from(base_url);
    js->ready_state = 0;

    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);

    ns_v8_install_document(js, base_url);
    ns_v8_storage_open(js);
    ns_v8_eval(js, "__nsInitStorage && __nsInitStorage();", -1,
               "v8-storage-init", nullptr);

    const char *early = g_getenv("NS_EARLY_JS_FILE");
    if (early && *early) {
        char *early_src = NULL;
        if (g_file_get_contents(early, &early_src, NULL, NULL)) {
            ns_v8_eval(js, early_src, -1, "early-inject", nullptr);
            g_free(early_src);
        }
    }
    if (js->early_inject_src && *js->early_inject_src)
        ns_v8_eval(js, js->early_inject_src, -1, "early-inject", nullptr);

    std::vector<ns_v8_script_task> tasks;
    ns_v8_collect_scripts(doc, tasks);

    ns_v8_run_phase(js, tasks, 0, base_url);
    ns_v8_run_phase(js, tasks, 1, base_url);
    int module_index = 0;
    for (auto &t : tasks)
        if (t.phase == 3)
            ns_v8_run_module_task(js, t.node, base_url, module_index++);
    js->ready_state = 1;
    ns_v8_fire_simple(js, "readystatechange");
    ns_v8_fire_simple(js, "DOMContentLoaded");
    ns_v8_run_phase(js, tasks, 2, base_url);
    js->ready_state = 2;
    ns_v8_fire_simple(js, "readystatechange");
    ns_v8_fire_simple(js, "load");
    ns_v8_fire_simple(js, "pageshow");
    ns_v8_settle(js);
}

char *
ns_js_eval_source(ns_js *js, const char *src, const char *origin)
{
    if (!js || !src) return NULL;
    if (js->pump_depth) return NULL;
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    v8::TryCatch tc(js->isolate);
    v8::Local<v8::Value> result;
    v8::Local<v8::String> code;
    if (!v8::String::NewFromUtf8(js->isolate, src,
                                 v8::NewStringType::kNormal)
             .ToLocal(&code))
        return g_strdup("error: source is not valid UTF-8");
    v8::ScriptOrigin so(ns_v8_str(js->isolate, origin ? origin : "console"));
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(scope.ctx, code, &so).ToLocal(&script) ||
        !script->Run(scope.ctx).ToLocal(&result)) {
        std::string msg = ns_v8_utf8(js->isolate, tc.Exception());
        return g_strdup_printf("error: %s",
                               msg.empty() ? "(no message)" : msg.c_str());
    }
    std::string out = ns_v8_utf8(js->isolate, result);
    ns_v8_settle(js);
    return g_strdup(out.empty() && result->IsUndefined() ? "undefined"
                                                         : out.c_str());
}

gboolean
ns_js_in_pump(const ns_js *js)
{
    return js && js->pump_depth > 0;
}

gboolean
ns_js_consume_mutated(ns_js *js)
{
    if (!js) return FALSE;
    gboolean m = js->mutated;
    js->mutated = FALSE;
    return m;
}

gboolean
ns_js_run_animation_frame(ns_js *js)
{
    if (!js || js->raf_queue.empty()) return FALSE;
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    std::vector<v8::Global<v8::Function>> queue;
    queue.swap(js->raf_queue);
    double ts = (double)(g_get_monotonic_time() - js->origin_us) / 1000.0;
    gboolean ran = FALSE;
    for (auto &g : queue) {
        if (g.IsEmpty()) continue;
        v8::Local<v8::Value> arg = v8::Number::New(js->isolate, ts);
        ns_v8_call_function(js, g.Get(js->isolate), 1, &arg,
                            "animation-frame");
        ran = TRUE;
    }
    return ran;
}

gboolean
ns_js_has_pending_animation_frame(const ns_js *js)
{
    return js && !js->raf_queue.empty();
}

gboolean
ns_js_has_pending_work(const ns_js *js)
{
    return js && (!js->timers.empty() || !js->pending_requests.empty());
}

void
ns_js_dump_stats(ns_js *js, GString *out)
{
    if (!js || !out) return;
    v8::HeapStatistics heap;
    js->isolate->GetHeapStatistics(&heap);
    g_string_append_printf(out,
                           "js engine: v8 %s (experimental backend)\n"
                           "heap: %zu KiB used / %zu KiB total\n"
                           "timers: %zu  listeners: %zu  raf queue: %zu\n",
                           v8::V8::GetVersion(),
                           heap.used_heap_size() / 1024,
                           heap.total_heap_size() / 1024,
                           js->timers.size(), js->listeners.size(),
                           js->raf_queue.size());
}

void
ns_js_dispatch_hashchange(ns_js *js, const char *old_url, const char *new_url)
{
    if (!js) return;
    g_free(js->current_url);
    js->current_url = g_strdup(new_url);
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    v8::Local<v8::Object> ev = ns_v8_make_event(js, "hashchange");
    ev->Set(scope.ctx, ns_v8_str(js->isolate, "oldURL"),
            ns_v8_str(js->isolate, old_url)).Check();
    ev->Set(scope.ctx, ns_v8_str(js->isolate, "newURL"),
            ns_v8_str(js->isolate, new_url)).Check();
    ns_v8_fire(js, "hashchange", ev);
}

void
ns_js_set_early_inject_src(ns_js *js, const char *src)
{
    if (!js) return;
    g_free(js->early_inject_src);
    js->early_inject_src = g_strdup(src);
}

void
ns_js_add_csp_header(ns_js *js, const char *header_value)
{
    if (!js || !header_value) return;
    g_ptr_array_add(js->csp_headers, g_strdup(header_value));
}

gboolean
ns_js_csp_form_action_allowed(const ns_js *js, const char *action_url)
{
    (void)js;
    (void)action_url;
    return TRUE;
}

const char *
ns_js_current_url(const ns_js *js)
{
    return js && js->current_url ? js->current_url : "";
}

const char *
ns_js_storage_partition(const ns_js *js)
{
    return js && js->partition ? js->partition : "";
}

void
ns_js_set_form_submit_cb(ns_js *js, ns_js_form_submit_cb cb, gpointer user_data)
{
    if (!js) return;
    js->form_submit_cb = cb;
    js->form_submit_user_data = user_data;
}

void
ns_js_set_download_cb(ns_js *js, ns_js_download_cb cb, gpointer user_data)
{
    if (!js) return;
    js->download_cb = cb;
    js->download_user_data = user_data;
}

void
ns_js_set_audio_cb(ns_js *js, ns_js_audio_cb cb, gpointer user_data)
{
    if (!js) return;
    js->audio_cb = cb;
    js->audio_user_data = user_data;
}

void
ns_js_set_media_seek_cb(ns_js *js, ns_js_media_seek_cb cb, gpointer user_data)
{
    if (!js) return;
    js->media_seek_cb = cb;
    js->media_seek_user_data = user_data;
}

void
ns_js_set_media_play_cb(ns_js *js, ns_js_media_play_cb cb, gpointer user_data)
{
    if (!js) return;
    js->media_play_cb = cb;
    js->media_play_user_data = user_data;
}

void
ns_js_set_media_muted_cb(ns_js *js, ns_js_media_muted_cb cb,
                         gpointer user_data)
{
    if (!js) return;
    js->media_muted_cb = cb;
    js->media_muted_user_data = user_data;
}

void
ns_js_set_mse_cb(ns_js *js, ns_js_mse_cb cb, gpointer user_data)
{
    if (!js) return;
    js->mse_cb = cb;
    js->mse_user_data = user_data;
}

void
ns_js_set_mse_buffered_cb(ns_js *js, ns_js_mse_buffered_cb cb,
                          gpointer user_data)
{
    if (!js) return;
    js->mse_buffered_cb = cb;
    js->mse_buffered_user_data = user_data;
}

void
ns_js_set_mse_remove_cb(ns_js *js, ns_js_mse_remove_cb cb, gpointer user_data)
{
    if (!js) return;
    js->mse_remove_cb = cb;
    js->mse_remove_user_data = user_data;
}

void
ns_js_set_media_volume_cb(ns_js *js, ns_js_media_volume_cb cb,
                          gpointer user_data)
{
    if (!js) return;
    js->media_volume_cb = cb;
    js->media_volume_user_data = user_data;
}

void
ns_js_set_layout_flush_cb(ns_js *js, ns_js_layout_flush_cb cb,
                          gpointer user_data)
{
    if (!js) return;
    js->layout_flush_cb = cb;
    js->layout_flush_user_data = user_data;
}

void
ns_js_video_event(ns_js *js, const void *node, const char *kind, double value)
{
    (void)js;
    (void)node;
    (void)kind;
    (void)value;
}

gboolean
ns_js_dispatch_event(ns_js *js, const ns_node *target, const char *type,
                     gboolean *default_prevented)
{
    if (default_prevented) *default_prevented = FALSE;
    if (!js || !type || js->pump_depth) return FALSE;
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    if (!target || target->kind == NS_NODE_DOCUMENT) {
        ns_v8_fire_simple(js, type);
        return TRUE;
    }
    return ns_v8_dom_dispatch(js, (ns_node *)target, type,
                              default_prevented);
}

gboolean
ns_js_click_activate(ns_js *js, const ns_node *node)
{
    if (!js || !node || node->kind != NS_NODE_ELEMENT || js->pump_depth)
        return FALSE;
    if (!ns_node_is_element_named(node, "input")) return FALSE;
    const char *type = ns_element_get_attr(node, "type");
    if (!type) return FALSE;
    ns_node *el = (ns_node *)node;
    if (g_ascii_strcasecmp(type, "checkbox") == 0) {
        ns_element_set_attr(el, "data-nd-checked",
                            ns_input_is_checked(el) ? "0" : "1");
    } else if (g_ascii_strcasecmp(type, "radio") == 0) {
        ns_v8_clear_radio_group(js, el);
        ns_element_set_attr(el, "data-nd-checked", "1");
    } else {
        return FALSE;
    }
    ns_css_mark_restyle_dirty(el->parent ? el->parent : el);
    ns_v8_mutated(js);
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    ns_v8_dom_dispatch(js, el, "input", NULL);
    ns_v8_dom_dispatch(js, el, "change", NULL);
    return TRUE;
}

gboolean
ns_js_node_has_click_handler(ns_js *js, const ns_node *target)
{
    if (!js || !target) return FALSE;
    for (const ns_node *n = target; n; n = n->parent) {
        if (n->js_wrapper) {
            ns_v8_wrap *w = static_cast<ns_v8_wrap *>(n->js_wrapper);
            for (auto &l : w->listeners)
                if (l.type == "click") return TRUE;
        }
        if (n->kind == NS_NODE_ELEMENT &&
            ns_element_get_attr(n, "onclick"))
            return TRUE;
    }
    return FALSE;
}

gboolean
ns_js_select_choose_option(ns_js *js, ns_node *option)
{
    if (!js || !option || js->pump_depth) return FALSE;
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    return ns_v8_select_choose_scoped(js, option);
}

gboolean
ns_js_select_toggle_option(ns_js *js, ns_node *option)
{
    if (!js || !option || js->pump_depth) return FALSE;
    ns_node *select = ns_v8_option_select_of(option);
    if (!select || ns_element_effectively_disabled(option)) return FALSE;
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    if (ns_element_get_attr(option, "selected"))
        ns_element_remove_attr(option, "selected");
    else
        ns_element_set_attr(option, "selected", "");
    ns_css_mark_restyle_dirty(select);
    ns_v8_mutated(js);
    ns_v8_dom_dispatch(js, select, "input", NULL);
    ns_v8_dom_dispatch(js, select, "change", NULL);
    return TRUE;
}

gboolean
ns_js_select_step(ns_js *js, ns_node *select, int dir)
{
    if (!js || !select || js->pump_depth ||
        !ns_node_is_element_named(select, "select"))
        return FALSE;
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    std::vector<ns_node *> opts;
    ns_v8_collect_options(select, opts);
    if (opts.empty()) return FALSE;
    int idx = ns_v8_select_current_index(select, opts);
    if (idx < 0) idx = dir > 0 ? -1 : (int)opts.size();
    for (guint step = 0; step < opts.size(); step++) {
        idx += dir > 0 ? 1 : -1;
        if (idx < 0 || idx >= (int)opts.size()) break;
        if (!ns_element_effectively_disabled(opts[idx]))
            return ns_v8_select_choose_scoped(js, opts[idx]);
    }
    return FALSE;
}

gboolean
ns_js_select_edge(ns_js *js, ns_node *select, gboolean last)
{
    if (!js || !select || js->pump_depth ||
        !ns_node_is_element_named(select, "select"))
        return FALSE;
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    std::vector<ns_node *> opts;
    ns_v8_collect_options(select, opts);
    if (last) {
        for (int i = (int)opts.size() - 1; i >= 0; i--)
            if (!ns_element_effectively_disabled(opts[i]))
                return ns_v8_select_choose_scoped(js, opts[i]);
    } else {
        for (ns_node *o : opts)
            if (!ns_element_effectively_disabled(o))
                return ns_v8_select_choose_scoped(js, o);
    }
    return FALSE;
}

gboolean
ns_js_select_typeahead(ns_js *js, ns_node *select, const char *key)
{
    if (!js || !select || !key || !*key || js->pump_depth ||
        !ns_node_is_element_named(select, "select"))
        return FALSE;
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    std::vector<ns_node *> opts;
    ns_v8_collect_options(select, opts);
    if (opts.empty()) return FALSE;
    int cur = ns_v8_select_current_index(select, opts);
    gsize key_len = strlen(key);
    for (guint step = 1; step <= opts.size(); step++) {
        ns_node *o = opts[(guint)(cur + (int)step) % opts.size()];
        if (ns_element_effectively_disabled(o)) continue;
        char *text = ns_node_collect_text(o);
        gboolean hit = text &&
                       g_ascii_strncasecmp(g_strstrip(text), key, key_len) == 0;
        g_free(text);
        if (hit) return ns_v8_select_choose_scoped(js, o);
    }
    return FALSE;
}

void
ns_js_activate_element(ns_js *js, const ns_node *el)
{
    if (!js || !el || js->pump_depth) return;
    {
        ns_v8_scope scope(js);
        ns_v8_pump_guard guard(js);
        ns_v8_dom_dispatch(js, (ns_node *)el, "click", NULL);
    }
    ns_js_click_activate(js, el);
}

gboolean
ns_js_dispatch_submit_event(ns_js *js, const ns_node *form,
                            const ns_node *submitter,
                            gboolean *default_prevented)
{
    (void)submitter;
    if (default_prevented) *default_prevented = FALSE;
    if (!js || !form || js->pump_depth) return FALSE;
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    return ns_v8_dom_dispatch(js, (ns_node *)form, "submit",
                              default_prevented);
}

static void
ns_v8_form_reset_walk(ns_node *n)
{
    for (ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT && c->name) {
            if (strcmp(c->name, "input") == 0) {
                ns_element_remove_attr(c, "data-nd-checked");
                ns_element_remove_attr(c, "data-nd-value");
                ns_element_remove_attr(c, "data-nd-vdirty");
            } else if (strcmp(c->name, "textarea") == 0) {
                ns_element_remove_attr(c, "data-nd-value");
            } else if (strcmp(c->name, "select") == 0) {
                ns_element_remove_attr(c, "data-nd-noselect");
            }
        }
        ns_v8_form_reset_walk(c);
    }
}

void
ns_js_form_reset(ns_js *js, ns_node *form)
{
    if (!js || !form) return;
    ns_v8_form_reset_walk(form);
    ns_css_mark_restyle_dirty(form);
    ns_v8_mutated(js);
    if (js->pump_depth) return;
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    ns_v8_dom_dispatch(js, form, "reset", NULL);
}

gboolean
ns_js_activate_summary(ns_js *js, const ns_node *el)
{
    if (!js || !el || js->pump_depth ||
        !ns_node_is_element_named(el, "summary"))
        return FALSE;
    ns_node *details = el->parent;
    while (details && !ns_node_is_element_named(details, "details"))
        details = details->parent;
    if (!details) return FALSE;
    gboolean open = ns_element_get_attr(details, "open") != NULL;
    if (open) ns_element_remove_attr(details, "open");
    else ns_element_set_attr(details, "open", "");
    ns_css_mark_restyle_dirty(details);
    ns_v8_mutated(js);
    ns_js_details_toggle_open(js, details, !open);
    return TRUE;
}

void
ns_js_dialog_close(ns_js *js, ns_node *dialog, const char *return_value)
{
    (void)return_value;
    if (!js || !dialog || js->pump_depth) return;
    gboolean was_open = ns_element_get_attr(dialog, "open") != NULL;
    ns_element_remove_attr(dialog, "open");
    ns_css_mark_restyle_dirty(dialog->parent ? dialog->parent : dialog);
    ns_v8_mutated(js);
    if (was_open) {
        ns_v8_scope scope(js);
        ns_v8_pump_guard guard(js);
        ns_v8_dom_dispatch(js, dialog, "close", NULL);
    }
}

void
ns_js_set_focus(ns_js *js, const ns_node *el)
{
    if (js) js->focused = el;
}

void
ns_js_set_focused_node(ns_js *js, const ns_node *el)
{
    if (js) js->focused = el;
}

const ns_node *
ns_js_focused_node(const ns_js *js)
{
    return js ? js->focused : NULL;
}

static void
ns_v8_collect_focusable(const ns_node *n, std::vector<const ns_node *> &out)
{
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT && ns_node_is_focusable(c))
            out.push_back(c);
        ns_v8_collect_focusable(c, out);
    }
}

const ns_node *
ns_js_sequential_focus_target(ns_js *js, gboolean backward)
{
    if (!js || !js->current_doc) return NULL;
    std::vector<const ns_node *> focusable;
    ns_v8_collect_focusable(js->current_doc, focusable);
    if (focusable.empty()) return NULL;
    int cur = -1;
    for (guint i = 0; i < focusable.size(); i++)
        if (focusable[i] == js->focused) cur = (int)i;
    int count = (int)focusable.size();
    int next;
    if (cur < 0) next = backward ? count - 1 : 0;
    else next = ((cur + (backward ? -1 : 1)) % count + count) % count;
    return focusable[(guint)next];
}

gboolean
ns_node_is_focusable(const ns_node *el)
{
    if (!el || el->kind != NS_NODE_ELEMENT || !el->name) return FALSE;
    if (ns_element_effectively_disabled(el)) return FALSE;
    if (ns_element_effectively_inert(el)) return FALSE;
    if (ns_element_get_attr(el, "hidden")) return FALSE;
    if (ns_element_get_attr(el, "tabindex")) return TRUE;
    const char *n = el->name;
    if (strcmp(n, "a") == 0 || strcmp(n, "area") == 0)
        return ns_element_get_attr(el, "href") != NULL;
    if (strcmp(n, "button") == 0 || strcmp(n, "select") == 0 ||
        strcmp(n, "textarea") == 0 || strcmp(n, "iframe") == 0 ||
        strcmp(n, "summary") == 0)
        return TRUE;
    if (strcmp(n, "input") == 0) {
        const char *t = ns_element_get_attr(el, "type");
        return !(t && g_ascii_strcasecmp(t, "hidden") == 0);
    }
    const char *ce = ns_element_get_attr(el, "contenteditable");
    if (ce && g_ascii_strcasecmp(ce, "false") != 0) return TRUE;
    return FALSE;
}

void
ns_js_refresh_top_layer(ns_js *js)
{
    (void)js;
}

void
ns_js_details_toggle_open(ns_js *js, ns_node *details, gboolean open)
{
    if (!js || !details || js->pump_depth) return;
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    v8::Isolate *iso = js->isolate;
    const char *old_state = open ? "closed" : "open";
    const char *new_state = open ? "open" : "closed";
    const char *types[] = {"beforetoggle", "toggle"};
    for (const char *type : types) {
        v8::Local<v8::Object> ev = ns_v8_make_event(js, type);
        ev->Set(scope.ctx, ns_v8_str(iso, "oldState"),
                ns_v8_str(iso, old_state)).Check();
        ev->Set(scope.ctx, ns_v8_str(iso, "newState"),
                ns_v8_str(iso, new_state)).Check();
        ns_v8_dom_dispatch_obj(js, details, type, ev, NULL);
    }
}

void
ns_js_dispatch_anim_events(ns_js *js, ns_anim *anim)
{
    (void)js;
    (void)anim;
}

void
ns_js_set_style_table(ns_js *js, GHashTable *styles)
{
    if (js) js->styles = styles;
}

void
ns_js_sync_window_metrics(ns_js *js)
{
    if (!js) return;
    ns_v8_scope scope(js);
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Object> global = scope.ctx->Global();
    double vw = ns_css_viewport_w();
    double vh = ns_css_viewport_h();
    if (vw <= 0) vw = 1000;
    if (vh <= 0) vh = 800;
    global->Set(scope.ctx, ns_v8_str(iso, "innerWidth"),
                v8::Number::New(iso, vw)).Check();
    global->Set(scope.ctx, ns_v8_str(iso, "innerHeight"),
                v8::Number::New(iso, vh)).Check();
    global->Set(scope.ctx, ns_v8_str(iso, "outerWidth"),
                v8::Number::New(iso, vw)).Check();
    global->Set(scope.ctx, ns_v8_str(iso, "outerHeight"),
                v8::Number::New(iso, vh)).Check();
    v8::Local<v8::Object> screen = v8::Object::New(iso);
    screen->Set(scope.ctx, ns_v8_str(iso, "width"),
                v8::Number::New(iso, vw)).Check();
    screen->Set(scope.ctx, ns_v8_str(iso, "height"),
                v8::Number::New(iso, vh)).Check();
    screen->Set(scope.ctx, ns_v8_str(iso, "availWidth"),
                v8::Number::New(iso, vw)).Check();
    screen->Set(scope.ctx, ns_v8_str(iso, "availHeight"),
                v8::Number::New(iso, vh)).Check();
    global->Set(scope.ctx, ns_v8_str(iso, "screen"), screen).Check();
}

void
ns_js_dispatch_resize(ns_js *js)
{
    if (!js) return;
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    ns_v8_fire_simple(js, "resize");
}

void
ns_js_note_viewport_scroll(ns_js *js, double x, double y)
{
    if (!js) return;
    js->scroll_x = x;
    js->scroll_y = y;
    ns_v8_scope scope(js);
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Object> global = scope.ctx->Global();
    const char *xs[] = {"scrollX", "pageXOffset"};
    const char *ys[] = {"scrollY", "pageYOffset"};
    for (auto *k : xs)
        global->Set(scope.ctx, ns_v8_str(iso, k), v8::Number::New(iso, x))
            .Check();
    for (auto *k : ys)
        global->Set(scope.ctx, ns_v8_str(iso, k), v8::Number::New(iso, y))
            .Check();
}

void
ns_js_set_layout_root(ns_js *js, const struct ns_box *root)
{
    if (js) js->layout_root = root;
}

void
ns_js_fire_media_load_events(ns_js *js, const struct ns_box *layout)
{
    (void)js;
    (void)layout;
}

void
ns_js_fire_page_transition(ns_js *js, const char *type, gboolean persisted)
{
    if (!js) return;
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    v8::Local<v8::Object> ev = ns_v8_make_event(js, type);
    ev->Set(scope.ctx, ns_v8_str(js->isolate, "persisted"),
            v8::Boolean::New(js->isolate, persisted)).Check();
    ns_v8_fire(js, type, ev);
}

cairo_surface_t *
ns_js_canvas_surface(ns_js *js, const ns_node *n)
{
    if (!js || !n) return NULL;
    auto it = js->canvases.find(n);
    if (it == js->canvases.end()) return NULL;
    cairo_surface_flush(it->second->surf);
    return it->second->surf;
}

void
ns_js_request_repaint(ns_js *js)
{
    (void)js;
}

void
ns_js_set_image_cache(ns_js *js, struct ns_image_cache *cache)
{
    (void)js;
    (void)cache;
}

const struct ns_image *
ns_js_image_for_node(ns_js *js, const ns_node *el)
{
    (void)js;
    (void)el;
    return NULL;
}

gboolean
ns_js_dispatch_key_event(ns_js *js, const ns_node *target, const char *type,
                         const char *key, const char *code, int key_code,
                         gboolean shift, gboolean ctrl, gboolean alt,
                         gboolean meta, gboolean *default_prevented)
{
    return ns_js_dispatch_key_event_full(js, target, type, key, code, key_code,
                                         0, shift, ctrl, alt, meta,
                                         default_prevented);
}

gboolean
ns_js_dispatch_key_event_full(ns_js *js, const ns_node *target,
                              const char *type, const char *key,
                              const char *code, int key_code, int char_code,
                              gboolean shift, gboolean ctrl, gboolean alt,
                              gboolean meta, gboolean *default_prevented)
{
    if (default_prevented) *default_prevented = FALSE;
    if (!js || !type || js->pump_depth) return FALSE;
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Object> ev = ns_v8_make_event(js, type);
    ev->Set(scope.ctx, ns_v8_str(iso, "key"), ns_v8_str(iso, key)).Check();
    ev->Set(scope.ctx, ns_v8_str(iso, "code"), ns_v8_str(iso, code)).Check();
    ev->Set(scope.ctx, ns_v8_str(iso, "keyCode"),
            v8::Integer::New(iso, key_code)).Check();
    ev->Set(scope.ctx, ns_v8_str(iso, "which"),
            v8::Integer::New(iso, key_code)).Check();
    ev->Set(scope.ctx, ns_v8_str(iso, "charCode"),
            v8::Integer::New(iso, char_code)).Check();
    ev->Set(scope.ctx, ns_v8_str(iso, "shiftKey"),
            v8::Boolean::New(iso, shift)).Check();
    ev->Set(scope.ctx, ns_v8_str(iso, "ctrlKey"),
            v8::Boolean::New(iso, ctrl)).Check();
    ev->Set(scope.ctx, ns_v8_str(iso, "altKey"),
            v8::Boolean::New(iso, alt)).Check();
    ev->Set(scope.ctx, ns_v8_str(iso, "metaKey"),
            v8::Boolean::New(iso, meta)).Check();
    ns_node *node = target && target->kind != NS_NODE_DOCUMENT
                        ? (ns_node *)target
                        : NULL;
    if (!node) {
        ns_v8_fire(js, type, ev);
        return TRUE;
    }
    return ns_v8_dom_dispatch_obj(js, node, type, ev, default_prevented);
}

gboolean
ns_js_dispatch_mouse_event(ns_js *js, const ns_node *target, const char *type,
                           double client_x, double client_y, double page_x,
                           double page_y, int button, int buttons,
                           gboolean shift, gboolean ctrl, gboolean alt,
                           gboolean meta, const ns_node *related,
                           gboolean *default_prevented)
{
    if (default_prevented) *default_prevented = FALSE;
    if (!js || !type || js->pump_depth) return FALSE;
    ns_v8_scope scope(js);
    ns_v8_pump_guard guard(js);
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Object> ev = ns_v8_make_event(js, type);
    struct {
        const char *name;
        double value;
    } nums[] = {{"clientX", client_x}, {"clientY", client_y},
                {"pageX", page_x},     {"pageY", page_y},
                {"screenX", client_x}, {"screenY", client_y},
                {"offsetX", client_x}, {"offsetY", client_y}};
    for (auto &f : nums)
        ev->Set(scope.ctx, ns_v8_str(iso, f.name),
                v8::Number::New(iso, f.value)).Check();
    ev->Set(scope.ctx, ns_v8_str(iso, "button"),
            v8::Integer::New(iso, button)).Check();
    ev->Set(scope.ctx, ns_v8_str(iso, "buttons"),
            v8::Integer::New(iso, buttons)).Check();
    ev->Set(scope.ctx, ns_v8_str(iso, "which"),
            v8::Integer::New(iso, button + 1)).Check();
    ev->Set(scope.ctx, ns_v8_str(iso, "shiftKey"),
            v8::Boolean::New(iso, shift)).Check();
    ev->Set(scope.ctx, ns_v8_str(iso, "ctrlKey"),
            v8::Boolean::New(iso, ctrl)).Check();
    ev->Set(scope.ctx, ns_v8_str(iso, "altKey"),
            v8::Boolean::New(iso, alt)).Check();
    ev->Set(scope.ctx, ns_v8_str(iso, "metaKey"),
            v8::Boolean::New(iso, meta)).Check();
    ev->Set(scope.ctx, ns_v8_str(iso, "relatedTarget"),
            related ? ns_v8_wrap_node(js, (ns_node *)related)
                    : v8::Local<v8::Value>(v8::Null(iso))).Check();
    ns_node *node = target && target->kind != NS_NODE_DOCUMENT
                        ? (ns_node *)target
                        : NULL;
    if (!node) {
        ns_v8_fire(js, type, ev);
        return TRUE;
    }
    return ns_v8_dom_dispatch_obj(js, node, type, ev, default_prevented);
}

ns_js_drag_session *
ns_js_drag_session_new(ns_js *js)
{
    (void)js;
    return g_new0(ns_js_drag_session, 1);
}

void
ns_js_drag_session_free(ns_js_drag_session *session)
{
    g_free(session);
}

void
ns_js_drag_session_set_data(ns_js_drag_session *session, const char *type,
                            const char *data)
{
    (void)session;
    (void)type;
    (void)data;
}

void
ns_js_drag_session_add_file(ns_js_drag_session *session, const char *path)
{
    (void)session;
    (void)path;
}

gboolean
ns_js_dispatch_drag_event(ns_js *js, ns_js_drag_session *session,
                          const ns_node *target, const char *type,
                          double client_x, double client_y, double page_x,
                          double page_y, int button, int buttons,
                          gboolean shift, gboolean ctrl, gboolean alt,
                          gboolean meta, const ns_node *related,
                          gboolean *default_prevented)
{
    (void)js;
    (void)session;
    (void)target;
    (void)type;
    (void)client_x;
    (void)client_y;
    (void)page_x;
    (void)page_y;
    (void)button;
    (void)buttons;
    (void)shift;
    (void)ctrl;
    (void)alt;
    (void)meta;
    (void)related;
    if (default_prevented) *default_prevented = FALSE;
    return FALSE;
}

gboolean
ns_ext_should_block(const char *url, const char *initiator)
{
    (void)url;
    (void)initiator;
    return FALSE;
}

char *
ns_webgl_take_pending_origin(void)
{
    return NULL;
}

void
ns_webgl_set_decision(const char *origin, int allow)
{
    (void)origin;
    (void)allow;
}
