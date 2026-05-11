/* Nordstjernen — JavaScript engine binding (QuickJS). */

#include "js.h"

#include <string.h>

#include <quickjs.h>

struct nd_js {
    JSRuntime    *rt;
    JSContext    *ctx;
    nd_js_log_cb  log_cb;
    gpointer      log_user_data;
};

static nd_js *g_active_js;

static void
nd_js_emit(nd_js *js, const char *prefix, JSContext *ctx, int argc, JSValueConst *argv)
{
    if (!js || !js->log_cb) return;
    GString *out = g_string_new(prefix);
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (s) {
            if (i > 0 || (prefix && *prefix)) g_string_append_c(out, ' ');
            g_string_append(out, s);
            JS_FreeCString(ctx, s);
        }
    }
    js->log_cb(out->str, js->log_user_data);
    g_string_free(out, TRUE);
}

static JSValue
nd_js_console_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    nd_js_emit(g_active_js, "", ctx, argc, argv);
    return JS_UNDEFINED;
}

static JSValue
nd_js_console_warn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    nd_js_emit(g_active_js, "[warn]", ctx, argc, argv);
    return JS_UNDEFINED;
}

static JSValue
nd_js_console_error(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    nd_js_emit(g_active_js, "[error]", ctx, argc, argv);
    return JS_UNDEFINED;
}

static JSValue
nd_js_alert(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    nd_js_emit(g_active_js, "[alert]", ctx, argc, argv);
    return JS_UNDEFINED;
}

nd_js *
nd_js_new(nd_js_log_cb log_cb, gpointer log_user_data)
{
    nd_js *js = g_new0(nd_js, 1);
    js->rt = JS_NewRuntime();
    if (!js->rt) { g_free(js); return NULL; }
    js->ctx = JS_NewContext(js->rt);
    if (!js->ctx) { JS_FreeRuntime(js->rt); g_free(js); return NULL; }
    js->log_cb = log_cb;
    js->log_user_data = log_user_data;

    JSValue global = JS_GetGlobalObject(js->ctx);
    JSValue console = JS_NewObject(js->ctx);
    JS_SetPropertyStr(js->ctx, console, "log",
                      JS_NewCFunction(js->ctx, nd_js_console_log, "log", 1));
    JS_SetPropertyStr(js->ctx, console, "warn",
                      JS_NewCFunction(js->ctx, nd_js_console_warn, "warn", 1));
    JS_SetPropertyStr(js->ctx, console, "error",
                      JS_NewCFunction(js->ctx, nd_js_console_error, "error", 1));
    JS_SetPropertyStr(js->ctx, console, "info",
                      JS_NewCFunction(js->ctx, nd_js_console_log, "info", 1));
    JS_SetPropertyStr(js->ctx, console, "debug",
                      JS_NewCFunction(js->ctx, nd_js_console_log, "debug", 1));
    JS_SetPropertyStr(js->ctx, global, "console", console);

    JS_SetPropertyStr(js->ctx, global, "alert",
                      JS_NewCFunction(js->ctx, nd_js_alert, "alert", 1));

    JSValue navigator = JS_NewObject(js->ctx);
    JS_SetPropertyStr(js->ctx, navigator, "userAgent",
                      JS_NewString(js->ctx, "Nordstjernen/0.0.1"));
    JS_SetPropertyStr(js->ctx, navigator, "appName",
                      JS_NewString(js->ctx, "Nordstjernen"));
    JS_SetPropertyStr(js->ctx, global, "navigator", navigator);

    JS_SetPropertyStr(js->ctx, global, "window", JS_DupValue(js->ctx, global));

    JS_FreeValue(js->ctx, global);
    return js;
}

static void
nd_js_install_document(nd_js *js, const nd_node *doc, const char *base_url)
{
    JSContext *ctx = js->ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    char *title_str = NULL;
    if (doc) {
        nd_node *t = nd_node_find_first_element(doc, "title");
        if (t) title_str = nd_node_collect_text(t);
    }
    if (!title_str) title_str = g_strdup("");

    JSValue document = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, document, "title",  JS_NewString(ctx, title_str));
    JS_SetPropertyStr(ctx, document, "URL",    JS_NewString(ctx, base_url ? base_url : ""));
    JS_SetPropertyStr(ctx, document, "domain", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, global, "document", document);

    JSValue location = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, location, "href", JS_NewString(ctx, base_url ? base_url : ""));
    JS_SetPropertyStr(ctx, global, "location", location);

    JS_FreeValue(ctx, global);
    g_free(title_str);
}

void
nd_js_free(nd_js *js)
{
    if (!js) return;
    JS_FreeContext(js->ctx);
    JS_FreeRuntime(js->rt);
    g_free(js);
}

static void
nd_js_eval(nd_js *js, const char *src, gsize len, const char *origin)
{
    g_active_js = js;
    JSValue v = JS_Eval(js->ctx, src, len, origin, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue ex = JS_GetException(js->ctx);
        const char *msg = JS_ToCString(js->ctx, ex);
        if (msg && js->log_cb) {
            char *line = g_strdup_printf("JS error: %s", msg);
            js->log_cb(line, js->log_user_data);
            g_free(line);
        }
        if (msg) JS_FreeCString(js->ctx, msg);
        JS_FreeValue(js->ctx, ex);
    }
    JS_FreeValue(js->ctx, v);
    g_active_js = NULL;
}

static void
nd_js_walk_scripts(nd_js *js, const nd_node *n, const char *origin)
{
    if (!n) return;
    if (n->kind == ND_NODE_ELEMENT && n->name && strcmp(n->name, "script") == 0) {
        const char *type = nd_element_get_attr(n, "type");
        if (!type || !*type || g_ascii_strcasecmp(type, "text/javascript") == 0 ||
            g_ascii_strcasecmp(type, "application/javascript") == 0 ||
            g_ascii_strcasecmp(type, "module") == 0) {
            for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
                if (c->kind == ND_NODE_TEXT && c->text)
                    nd_js_eval(js, c->text, strlen(c->text), origin);
            }
        }
        return;
    }
    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        nd_js_walk_scripts(js, c, origin);
}

void
nd_js_run_scripts_in_doc(nd_js *js, const nd_node *doc, const char *base_url)
{
    if (!js || !doc) return;
    nd_js_install_document(js, doc, base_url);
    nd_js_walk_scripts(js, doc, base_url && *base_url ? base_url : "inline");
}
