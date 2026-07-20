/* Nordstjernen — experimental V8 JavaScript engine backend implementing the
 * js.h engine contract (pure-JS execution; DOM bindings are minimal stubs).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include <glib.h>
#include <cairo.h>

#include "js.h"
#include "dom.h"
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

}

struct ns_js_drag_session {
    int unused;
};

struct ns_js {
    v8::Isolate *isolate;
    v8::ArrayBuffer::Allocator *allocator;
    v8::Global<v8::Context> context;
    v8::Global<v8::Object> document;

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

v8::Local<v8::Object> ns_v8_make_event(ns_js *js, const char *type)
{
    v8::Isolate *iso = js->isolate;
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Object> ev = v8::Object::New(iso);
    ev->Set(ctx, ns_v8_str(iso, "type"), ns_v8_str(iso, type)).Check();
    ev->Set(ctx, ns_v8_str(iso, "bubbles"), v8::Boolean::New(iso, false))
        .Check();
    ev->Set(ctx, ns_v8_str(iso, "cancelable"), v8::Boolean::New(iso, false))
        .Check();
    ev->Set(ctx, ns_v8_str(iso, "timeStamp"),
            v8::Number::New(iso,
                (double)(g_get_monotonic_time() - js->origin_us) / 1000.0))
        .Check();
    if (!js->document.IsEmpty())
        ev->Set(ctx, ns_v8_str(iso, "target"), js->document.Get(iso)).Check();
    v8::Local<v8::Function> noop =
        v8::Function::New(ctx,
            [](const v8::FunctionCallbackInfo<v8::Value> &) {})
            .ToLocalChecked();
    ev->Set(ctx, ns_v8_str(iso, "preventDefault"), noop).Check();
    ev->Set(ctx, ns_v8_str(iso, "stopPropagation"), noop).Check();
    ev->Set(ctx, ns_v8_str(iso, "stopImmediatePropagation"), noop).Check();
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

void ns_v8_null_cb(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    info.GetReturnValue().SetNull();
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
        v8::Function::New(ctx, ns_v8_noop_cb).ToLocalChecked());
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
    ns_v8_bind_fn(js, document, "getElementById", ns_v8_null_cb);
    ns_v8_bind_fn(js, document, "querySelector", ns_v8_null_cb);
    ns_v8_bind_fn(js, document, "querySelectorAll", ns_v8_empty_array_cb);
    ns_v8_bind_fn(js, document, "getElementsByTagName",
                  ns_v8_empty_array_cb);
    ns_v8_bind_fn(js, document, "getElementsByClassName",
                  ns_v8_empty_array_cb);
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
        js->raf_queue.clear();
        js->listeners.clear();
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
    (void)js;
    (void)target;
    (void)type;
    if (default_prevented) *default_prevented = FALSE;
    return FALSE;
}

gboolean
ns_js_click_activate(ns_js *js, const ns_node *node)
{
    (void)js;
    (void)node;
    return FALSE;
}

gboolean
ns_js_node_has_click_handler(ns_js *js, const ns_node *target)
{
    (void)js;
    (void)target;
    return FALSE;
}

gboolean
ns_js_select_choose_option(ns_js *js, ns_node *option)
{
    (void)js;
    (void)option;
    return FALSE;
}

gboolean
ns_js_select_toggle_option(ns_js *js, ns_node *option)
{
    (void)js;
    (void)option;
    return FALSE;
}

gboolean
ns_js_select_step(ns_js *js, ns_node *select, int dir)
{
    (void)js;
    (void)select;
    (void)dir;
    return FALSE;
}

gboolean
ns_js_select_edge(ns_js *js, ns_node *select, gboolean last)
{
    (void)js;
    (void)select;
    (void)last;
    return FALSE;
}

gboolean
ns_js_select_typeahead(ns_js *js, ns_node *select, const char *key)
{
    (void)js;
    (void)select;
    (void)key;
    return FALSE;
}

void
ns_js_activate_element(ns_js *js, const ns_node *el)
{
    (void)js;
    (void)el;
}

gboolean
ns_js_dispatch_submit_event(ns_js *js, const ns_node *form,
                            const ns_node *submitter,
                            gboolean *default_prevented)
{
    (void)js;
    (void)form;
    (void)submitter;
    if (default_prevented) *default_prevented = FALSE;
    return FALSE;
}

void
ns_js_form_reset(ns_js *js, ns_node *form)
{
    (void)js;
    (void)form;
}

gboolean
ns_js_activate_summary(ns_js *js, const ns_node *el)
{
    (void)js;
    (void)el;
    return FALSE;
}

void
ns_js_dialog_close(ns_js *js, ns_node *dialog, const char *return_value)
{
    (void)js;
    (void)dialog;
    (void)return_value;
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

const ns_node *
ns_js_sequential_focus_target(ns_js *js, gboolean backward)
{
    (void)js;
    (void)backward;
    return NULL;
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
    (void)js;
    (void)details;
    (void)open;
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
    (void)js;
    (void)target;
    (void)type;
    (void)key;
    (void)code;
    (void)key_code;
    (void)char_code;
    (void)shift;
    (void)ctrl;
    (void)alt;
    (void)meta;
    if (default_prevented) *default_prevented = FALSE;
    return FALSE;
}

gboolean
ns_js_dispatch_mouse_event(ns_js *js, const ns_node *target, const char *type,
                           double client_x, double client_y, double page_x,
                           double page_y, int button, int buttons,
                           gboolean shift, gboolean ctrl, gboolean alt,
                           gboolean meta, const ns_node *related,
                           gboolean *default_prevented)
{
    (void)js;
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
