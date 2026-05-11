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

static JSValue
nd_js_console_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_active_js || !g_active_js->log_cb) return JS_UNDEFINED;
    GString *out = g_string_new(NULL);
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (s) {
            if (i > 0) g_string_append_c(out, ' ');
            g_string_append(out, s);
            JS_FreeCString(ctx, s);
        }
    }
    g_active_js->log_cb(out->str, g_active_js->log_user_data);
    g_string_free(out, TRUE);
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
    JS_SetPropertyStr(js->ctx, global, "console", console);
    JS_FreeValue(js->ctx, global);
    return js;
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
    nd_js_walk_scripts(js, doc, base_url && *base_url ? base_url : "inline");
}

gboolean
nd_js_available(void) { return TRUE; }
