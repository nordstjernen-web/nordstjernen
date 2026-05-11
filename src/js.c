/* Nordstjernen — JavaScript engine binding (QuickJS). */

#include "js.h"

#include <string.h>

#include <quickjs.h>

#include "css.h"

struct nd_js {
    JSRuntime    *rt;
    JSContext    *ctx;
    nd_js_log_cb  log_cb;
    gpointer      log_user_data;
    const nd_node *current_doc;
};

static nd_js *g_active_js;
static JSClassID nd_element_class_id;

static void
nd_element_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt; (void)val;
}

static JSClassDef nd_element_class = {
    .class_name = "Element",
    .finalizer  = nd_element_finalizer,
};

static JSValue
nd_make_element(JSContext *ctx, const nd_node *node)
{
    if (!node) return JS_NULL;
    JSValue obj = JS_NewObjectClass(ctx, nd_element_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, (void *)node);
    return obj;
}

static const nd_node *
nd_unwrap_element(JSValueConst val)
{
    return JS_GetOpaque(val, nd_element_class_id);
}

static JSValue
nd_element_get_tagName(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n || !n->name) return JS_NULL;
    char *up = g_ascii_strup(n->name, -1);
    JSValue v = JS_NewString(ctx, up);
    g_free(up);
    return v;
}

static JSValue
nd_element_get_textContent(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_NULL;
    char *t = nd_node_collect_text(n);
    JSValue v = JS_NewString(ctx, t ? t : "");
    g_free(t);
    return v;
}

static JSValue
nd_element_get_id(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_NULL;
    const char *v = nd_element_get_attr(n, "id");
    return JS_NewString(ctx, v ? v : "");
}

static JSValue
nd_element_get_className(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_NULL;
    const char *v = nd_element_get_attr(n, "class");
    return JS_NewString(ctx, v ? v : "");
}

static JSValue
nd_element_getAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n || argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;
    const char *val = nd_element_get_attr(n, name);
    JS_FreeCString(ctx, name);
    return val ? JS_NewString(ctx, val) : JS_NULL;
}

static JSValue
nd_element_hasAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n || argc < 1) return JS_FALSE;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_FALSE;
    const char *val = nd_element_get_attr(n, name);
    JS_FreeCString(ctx, name);
    return val ? JS_TRUE : JS_FALSE;
}

static const nd_node *
next_element_sibling(const nd_node *n)
{
    for (const nd_node *s = n ? n->next_sibling : NULL; s; s = s->next_sibling)
        if (s->kind == ND_NODE_ELEMENT) return s;
    return NULL;
}

static const nd_node *
prev_element_sibling(const nd_node *n)
{
    for (const nd_node *s = n ? n->prev_sibling : NULL; s; s = s->prev_sibling)
        if (s->kind == ND_NODE_ELEMENT) return s;
    return NULL;
}

static const nd_node *
first_element_child(const nd_node *n)
{
    for (const nd_node *c = n ? n->first_child : NULL; c; c = c->next_sibling)
        if (c->kind == ND_NODE_ELEMENT) return c;
    return NULL;
}

static JSValue
nd_element_get_parentElement(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n || !n->parent || n->parent->kind != ND_NODE_ELEMENT) return JS_NULL;
    return nd_make_element(ctx, n->parent);
}

static JSValue
nd_element_get_firstElementChild(JSContext *ctx, JSValueConst this_val)
{
    return nd_make_element(ctx, first_element_child(nd_unwrap_element(this_val)));
}

static JSValue
nd_element_get_nextElementSibling(JSContext *ctx, JSValueConst this_val)
{
    return nd_make_element(ctx, next_element_sibling(nd_unwrap_element(this_val)));
}

static JSValue
nd_element_get_previousElementSibling(JSContext *ctx, JSValueConst this_val)
{
    return nd_make_element(ctx, prev_element_sibling(nd_unwrap_element(this_val)));
}

static JSValue
nd_element_get_children(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!n) return arr;
    uint32_t i = 0;
    for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind != ND_NODE_ELEMENT) continue;
        JS_SetPropertyUint32(ctx, arr, i++, nd_make_element(ctx, c));
    }
    return arr;
}

static void
nd_collect_by_tag(const nd_node *n, const char *tag, JSContext *ctx,
                  JSValue arr, uint32_t *idx)
{
    if (!n) return;
    if (n->kind == ND_NODE_ELEMENT && n->name &&
        (strcmp(tag, "*") == 0 || g_ascii_strcasecmp(n->name, tag) == 0))
        JS_SetPropertyUint32(ctx, arr, (*idx)++, nd_make_element(ctx, n));
    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        nd_collect_by_tag(c, tag, ctx, arr, idx);
}

static gboolean
element_has_class(const nd_node *n, const char *want)
{
    const char *cls = nd_element_get_attr(n, "class");
    if (!cls) return FALSE;
    gsize wl = strlen(want);
    const char *p = cls;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if ((gsize)(p - tok) == wl && strncmp(tok, want, wl) == 0) return TRUE;
    }
    return FALSE;
}

static void
nd_collect_by_class(const nd_node *n, const char *cls, JSContext *ctx,
                    JSValue arr, uint32_t *idx)
{
    if (!n) return;
    if (n->kind == ND_NODE_ELEMENT && element_has_class(n, cls))
        JS_SetPropertyUint32(ctx, arr, (*idx)++, nd_make_element(ctx, n));
    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        nd_collect_by_class(c, cls, ctx, arr, idx);
}

static JSValue
nd_element_getElementsByTagName(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const nd_node *root = nd_unwrap_element(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!root || argc < 1) return arr;
    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return arr;
    uint32_t i = 0;
    for (const nd_node *c = root->first_child; c; c = c->next_sibling)
        nd_collect_by_tag(c, tag, ctx, arr, &i);
    JS_FreeCString(ctx, tag);
    return arr;
}

static gboolean
nd_matches_any_selector(GPtrArray *sels, const nd_node *el)
{
    for (guint i = 0; i < sels->len; i++) {
        if (nd_css_selector_matches(g_ptr_array_index(sels, i), el))
            return TRUE;
    }
    return FALSE;
}

static const nd_node *
nd_walk_first_match(const nd_node *root, GPtrArray *sels)
{
    if (!root) return NULL;
    if (root->kind == ND_NODE_ELEMENT && nd_matches_any_selector(sels, root))
        return root;
    for (const nd_node *c = root->first_child; c; c = c->next_sibling) {
        const nd_node *m = nd_walk_first_match(c, sels);
        if (m) return m;
    }
    return NULL;
}

static void
nd_walk_all_matches(const nd_node *root, GPtrArray *sels, JSContext *ctx,
                    JSValue arr, uint32_t *idx)
{
    if (!root) return;
    if (root->kind == ND_NODE_ELEMENT && nd_matches_any_selector(sels, root))
        JS_SetPropertyUint32(ctx, arr, (*idx)++, nd_make_element(ctx, root));
    for (const nd_node *c = root->first_child; c; c = c->next_sibling)
        nd_walk_all_matches(c, sels, ctx, arr, idx);
}

static JSValue
nd_query_selector_impl(JSContext *ctx, const nd_node *root,
                       int argc, JSValueConst *argv, gboolean want_all,
                       gboolean include_self)
{
    if (!root || argc < 1) return want_all ? JS_NewArray(ctx) : JS_NULL;
    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return want_all ? JS_NewArray(ctx) : JS_NULL;
    GPtrArray *sels = nd_css_parse_selector_list(sel);
    JS_FreeCString(ctx, sel);
    if (sels->len == 0) {
        g_ptr_array_free(sels, TRUE);
        return want_all ? JS_NewArray(ctx) : JS_NULL;
    }
    JSValue ret;
    if (want_all) {
        ret = JS_NewArray(ctx);
        uint32_t i = 0;
        if (include_self && root->kind == ND_NODE_ELEMENT &&
            nd_matches_any_selector(sels, root))
            JS_SetPropertyUint32(ctx, ret, i++, nd_make_element(ctx, root));
        for (const nd_node *c = root->first_child; c; c = c->next_sibling)
            nd_walk_all_matches(c, sels, ctx, ret, &i);
    } else {
        const nd_node *m = NULL;
        if (include_self && root->kind == ND_NODE_ELEMENT &&
            nd_matches_any_selector(sels, root))
            m = root;
        if (!m)
            for (const nd_node *c = root->first_child; !m && c; c = c->next_sibling)
                m = nd_walk_first_match(c, sels);
        ret = nd_make_element(ctx, m);
    }
    g_ptr_array_free(sels, TRUE);
    return ret;
}

static JSValue
nd_element_querySelector(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    return nd_query_selector_impl(ctx, nd_unwrap_element(this_val), argc, argv,
                                  FALSE, FALSE);
}

static JSValue
nd_element_querySelectorAll(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    return nd_query_selector_impl(ctx, nd_unwrap_element(this_val), argc, argv,
                                  TRUE, FALSE);
}

static JSValue
nd_element_getElementsByClassName(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const nd_node *root = nd_unwrap_element(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!root || argc < 1) return arr;
    const char *cls = JS_ToCString(ctx, argv[0]);
    if (!cls) return arr;
    uint32_t i = 0;
    for (const nd_node *c = root->first_child; c; c = c->next_sibling)
        nd_collect_by_class(c, cls, ctx, arr, &i);
    JS_FreeCString(ctx, cls);
    return arr;
}

static const JSCFunctionListEntry nd_element_proto_funcs[] = {
    JS_CGETSET_DEF("tagName",                nd_element_get_tagName,                NULL),
    JS_CGETSET_DEF("textContent",            nd_element_get_textContent,            NULL),
    JS_CGETSET_DEF("id",                     nd_element_get_id,                     NULL),
    JS_CGETSET_DEF("className",              nd_element_get_className,              NULL),
    JS_CGETSET_DEF("parentElement",          nd_element_get_parentElement,          NULL),
    JS_CGETSET_DEF("parentNode",             nd_element_get_parentElement,          NULL),
    JS_CGETSET_DEF("firstElementChild",      nd_element_get_firstElementChild,      NULL),
    JS_CGETSET_DEF("nextElementSibling",     nd_element_get_nextElementSibling,     NULL),
    JS_CGETSET_DEF("previousElementSibling", nd_element_get_previousElementSibling, NULL),
    JS_CGETSET_DEF("children",               nd_element_get_children,               NULL),
    JS_CFUNC_DEF("getAttribute",            1, nd_element_getAttribute),
    JS_CFUNC_DEF("hasAttribute",            1, nd_element_hasAttribute),    JS_CFUNC_DEF("getElementsByTagName",    1, nd_element_getElementsByTagName),
    JS_CFUNC_DEF("getElementsByClassName",  1, nd_element_getElementsByClassName),
    JS_CFUNC_DEF("querySelector",           1, nd_element_querySelector),
    JS_CFUNC_DEF("querySelectorAll",        1, nd_element_querySelectorAll),
};

static JSValue
nd_document_getElementById(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_active_js || !g_active_js->current_doc || argc < 1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_NULL;
    nd_node *found = nd_node_find_by_id(g_active_js->current_doc, id);
    JS_FreeCString(ctx, id);
    return nd_make_element(ctx, found);
}

static JSValue
nd_document_get_documentElement(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!g_active_js || !g_active_js->current_doc) return JS_NULL;
    nd_node *root = nd_node_find_first_element(g_active_js->current_doc, "html");
    return nd_make_element(ctx, root);
}

static JSValue
nd_document_get_body(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!g_active_js || !g_active_js->current_doc) return JS_NULL;
    nd_node *body = nd_node_find_first_element(g_active_js->current_doc, "body");
    return nd_make_element(ctx, body);
}

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

    if (!nd_element_class_id)
        JS_NewClassID(js->rt, &nd_element_class_id);
    JS_NewClass(js->rt, nd_element_class_id, &nd_element_class);
    JSValue element_proto = JS_NewObject(js->ctx);
    JS_SetPropertyFunctionList(js->ctx, element_proto, nd_element_proto_funcs,
                               G_N_ELEMENTS(nd_element_proto_funcs));
    JS_SetClassProto(js->ctx, nd_element_class_id, element_proto);

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

static JSValue
nd_document_getElementsByTagName(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue arr = JS_NewArray(ctx);
    if (!g_active_js || !g_active_js->current_doc || argc < 1) return arr;
    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return arr;
    uint32_t i = 0;
    nd_collect_by_tag(g_active_js->current_doc, tag, ctx, arr, &i);
    JS_FreeCString(ctx, tag);
    return arr;
}

static JSValue
nd_document_getElementsByClassName(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue arr = JS_NewArray(ctx);
    if (!g_active_js || !g_active_js->current_doc || argc < 1) return arr;
    const char *cls = JS_ToCString(ctx, argv[0]);
    if (!cls) return arr;
    uint32_t i = 0;
    nd_collect_by_class(g_active_js->current_doc, cls, ctx, arr, &i);
    JS_FreeCString(ctx, cls);
    return arr;
}

static JSValue
nd_document_querySelector(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_active_js) return JS_NULL;
    return nd_query_selector_impl(ctx, g_active_js->current_doc, argc, argv,
                                  FALSE, TRUE);
}

static JSValue
nd_document_querySelectorAll(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_active_js) return JS_NewArray(ctx);
    return nd_query_selector_impl(ctx, g_active_js->current_doc, argc, argv,
                                  TRUE, TRUE);
}

static const JSCFunctionListEntry nd_document_funcs[] = {
    JS_CFUNC_DEF("getElementById",          1, nd_document_getElementById),
    JS_CFUNC_DEF("getElementsByTagName",    1, nd_document_getElementsByTagName),
    JS_CFUNC_DEF("getElementsByClassName",  1, nd_document_getElementsByClassName),
    JS_CFUNC_DEF("querySelector",           1, nd_document_querySelector),
    JS_CFUNC_DEF("querySelectorAll",        1, nd_document_querySelectorAll),
    JS_CGETSET_DEF("documentElement", nd_document_get_documentElement, NULL),
    JS_CGETSET_DEF("body",            nd_document_get_body,            NULL),
};

static void
nd_js_install_document(nd_js *js, const nd_node *doc, const char *base_url)
{
    js->current_doc = doc;

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
    JS_SetPropertyFunctionList(ctx, document, nd_document_funcs,
                               G_N_ELEMENTS(nd_document_funcs));
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

char *
nd_js_eval_source(nd_js *js, const char *src, const char *origin)
{
    if (!js || !src) return NULL;
    g_active_js = js;
    JSValue v = JS_Eval(js->ctx, src, strlen(src), origin ? origin : "console", JS_EVAL_TYPE_GLOBAL);
    char *out = NULL;
    if (JS_IsException(v)) {
        JSValue ex = JS_GetException(js->ctx);
        const char *msg = JS_ToCString(js->ctx, ex);
        out = g_strdup_printf("error: %s", msg ? msg : "(no message)");
        if (msg) JS_FreeCString(js->ctx, msg);
        JS_FreeValue(js->ctx, ex);
    } else {
        const char *s = JS_ToCString(js->ctx, v);
        out = g_strdup(s ? s : "undefined");
        if (s) JS_FreeCString(js->ctx, s);
    }
    JS_FreeValue(js->ctx, v);
    g_active_js = NULL;
    return out;
}
