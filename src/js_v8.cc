/* Nordstjernen — experimental V8 JavaScript engine backend implementing the
 * js.h engine contract (pure-JS execution; DOM bindings are minimal stubs).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include <glib.h>
#include <cairo.h>

#include "js.h"
#include "css.h"
#include "dom.h"
#include "html.h"
#include "net.h"

#include <libplatform/libplatform.h>
#include <v8.h>

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

    GPtrArray *csp_headers;
    std::map<int, ns_v8_timer *> timers;
    std::vector<v8::Global<v8::Function>> raf_queue;
    std::vector<ns_v8_listener> listeners;

    gint64 origin_us;
    int next_timer_id;
    int pump_depth;
    int ready_state;
    gboolean mutated;
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

void ns_v8_el_bounding_rect(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Isolate *iso = info.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Object> rect = v8::Object::New(iso);
    const char *keys[] = {"x",     "y",      "width", "height",
                          "top",   "left",   "right", "bottom"};
    for (auto *k : keys)
        rect->Set(ctx, ns_v8_str(iso, k), v8::Number::New(iso, 0)).Check();
    info.GetReturnValue().Set(rect);
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

void ns_v8_make_node_template(ns_js *js)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::FunctionTemplate> ft = v8::FunctionTemplate::New(iso);
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
        {"getBoundingClientRect", ns_v8_el_bounding_rect},
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
    proto->SetAccessorProperty(ns_v8_str(iso, "value"),
        v8::FunctionTemplate::New(iso, ns_v8_el_value_get),
        v8::FunctionTemplate::New(iso, ns_v8_el_value_set));
    proto->SetAccessorProperty(ns_v8_str(iso, "checked"),
        v8::FunctionTemplate::New(iso, ns_v8_el_checked_get),
        v8::FunctionTemplate::New(iso, ns_v8_el_checked_set));

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
    "  ['offsetWidth', 'offsetHeight', 'clientWidth', 'clientHeight',\n"
    "   'offsetTop', 'offsetLeft', 'scrollTop', 'scrollLeft',\n"
    "   'scrollWidth', 'scrollHeight'].forEach(function (k) {\n"
    "    Object.defineProperty(p, k, {\n"
    "      configurable: true,\n"
    "      get: function () { return 0; },\n"
    "      set: function () {}\n"
    "    });\n"
    "  });\n"
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
        ns_v8_eval(js, ns_v8_dom_bootstrap_src, -1, "v8-dom-bootstrap",
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
        js->wraps.clear();
        js->raf_queue.clear();
        js->listeners.clear();
        js->node_tmpl.Reset();
        js->document.Reset();
        js->context.Reset();
    }
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
    int modules = 0;
    for (auto &t : tasks)
        if (t.phase == 3) modules++;
    if (modules) {
        char *line = g_strdup_printf(
            "js_v8: %d module script(s) skipped (not yet supported)", modules);
        ns_v8_log(js, line);
        g_free(line);
    }

    ns_v8_run_phase(js, tasks, 0, base_url);
    ns_v8_run_phase(js, tasks, 1, base_url);
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
    return js && !js->timers.empty();
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
    (void)js;
    (void)styles;
}

void
ns_js_sync_window_metrics(ns_js *js)
{
    (void)js;
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
    (void)js;
    (void)x;
    (void)y;
}

void
ns_js_set_layout_root(ns_js *js, const struct ns_box *root)
{
    (void)js;
    (void)root;
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
    (void)js;
    (void)n;
    return NULL;
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
