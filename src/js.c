/* Nordstjernen — JavaScript engine binding (QuickJS). */

#include "js.h"

#include <string.h>

#include <quickjs.h>

#include "css.h"
#include "html.h"

struct nd_js {
    JSRuntime    *rt;
    JSContext    *ctx;
    nd_js_log_cb  log_cb;
    gpointer      log_user_data;
    nd_js_mutated_cb mut_cb;
    gpointer      mut_user_data;
    const nd_node *current_doc;
    gboolean      mutated;
    GHashTable   *timers;
    int           next_timer_id;
    GPtrArray    *orphan_nodes;
};

typedef struct nd_timer {
    nd_js  *js;
    JSValue cb;
    int     id;
    guint   glib_source;
    gboolean is_interval;
} nd_timer;

static nd_js *g_active_js;

static void
nd_timer_free(gpointer data)
{
    nd_timer *t = data;
    if (!t) return;
    if (t->glib_source) g_source_remove(t->glib_source);
    JS_FreeValue(t->js->ctx, t->cb);
    g_free(t);
}

static void
nd_drain_mutations(nd_js *js)
{
    if (js->mutated && js->mut_cb)
        js->mut_cb(js->mut_user_data);
    js->mutated = FALSE;
}

static gboolean
nd_timer_fire(gpointer data)
{
    nd_timer *t = data;
    nd_js *js = t->js;
    g_active_js = js;
    JSValue ret = JS_Call(js->ctx, t->cb, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(ret)) {
        JSValue ex = JS_GetException(js->ctx);
        const char *msg = JS_ToCString(js->ctx, ex);
        if (msg && js->log_cb) {
            char *line = g_strdup_printf("JS error in timer: %s", msg);
            js->log_cb(line, js->log_user_data);
            g_free(line);
        }
        if (msg) JS_FreeCString(js->ctx, msg);
        JS_FreeValue(js->ctx, ex);
    }
    JS_FreeValue(js->ctx, ret);
    nd_drain_mutations(js);
    g_active_js = NULL;
    if (!t->is_interval) {
        t->glib_source = 0;
        g_hash_table_remove(js->timers, GINT_TO_POINTER(t->id));
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static JSValue
nd_js_setTimeout(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                 int is_interval)
{
    (void)this_val;
    if (!g_active_js || argc < 1) return JS_NewInt32(ctx, 0);
    if (!JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    int32_t ms = 0;
    if (argc >= 2) JS_ToInt32(ctx, &ms, argv[1]);
    if (ms < 0) ms = 0;

    nd_js *js = g_active_js;
    nd_timer *t = g_new0(nd_timer, 1);
    t->js = js;
    t->cb = JS_DupValue(ctx, argv[0]);
    t->is_interval = is_interval;
    t->id = ++js->next_timer_id;
    t->glib_source = g_timeout_add((guint)ms, nd_timer_fire, t);
    g_hash_table_insert(js->timers, GINT_TO_POINTER(t->id), t);
    return JS_NewInt32(ctx, t->id);
}

static JSValue
nd_js_setTimeout_wrap(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return nd_js_setTimeout(ctx, this_val, argc, argv, 0);
}

static JSValue
nd_js_setInterval_wrap(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return nd_js_setTimeout(ctx, this_val, argc, argv, 1);
}

static JSValue
nd_js_clearTimer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_active_js || argc < 1) return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    nd_timer *t = g_hash_table_lookup(g_active_js->timers, GINT_TO_POINTER(id));
    if (t) {
        if (t->glib_source) { g_source_remove(t->glib_source); t->glib_source = 0; }
        g_hash_table_remove(g_active_js->timers, GINT_TO_POINTER(id));
    }
    return JS_UNDEFINED;
}

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
nd_element_get_innerHTML(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_NewString(ctx, "");
    char *html = nd_node_inner_html(n);
    JSValue v = JS_NewString(ctx, html ? html : "");
    g_free(html);
    return v;
}

static void
nd_element_clear_children(nd_node *n)
{
    nd_node *c = n->first_child;
    while (c) {
        nd_node *next = c->next_sibling;
        nd_node_remove(c);
        nd_node_free(c);
        c = next;
    }
    n->first_child = NULL;
    n->last_child  = NULL;
}

static JSValue
nd_element_set_textContent(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    nd_node *n = (nd_node *)nd_unwrap_element(this_val);
    if (!n || n->kind != ND_NODE_ELEMENT) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (!s) return JS_UNDEFINED;
    nd_element_clear_children(n);
    if (*s)
        nd_node_append_child(n, nd_node_new_text(g_strdup(s)));
    JS_FreeCString(ctx, s);
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_element_set_innerHTML(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    nd_node *n = (nd_node *)nd_unwrap_element(this_val);
    if (!n || n->kind != ND_NODE_ELEMENT) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (!s) return JS_UNDEFINED;
    nd_element_clear_children(n);
    nd_node *fragment = nd_html_parse(s, -1);
    JS_FreeCString(ctx, s);
    if (fragment) {
        nd_node *c = fragment->first_child;
        while (c) {
            nd_node *next = c->next_sibling;
            nd_node_remove(c);
            nd_node_append_child(n, c);
            c = next;
        }
        nd_node_free(fragment);
    }
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_element_get_outerHTML(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_NewString(ctx, "");
    char *html = nd_node_outer_html(n);
    JSValue v = JS_NewString(ctx, html ? html : "");
    g_free(html);
    return v;
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

static JSValue
nd_element_appendChild(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    nd_node *parent = (nd_node *)nd_unwrap_element(this_val);
    if (!parent || argc < 1) return JS_NULL;
    nd_node *child = (nd_node *)nd_unwrap_element(argv[0]);
    if (!child) return JS_NULL;
    if (g_active_js)
        g_ptr_array_remove_fast(g_active_js->orphan_nodes, child);
    nd_node_append_child(parent, child);
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue
nd_element_removeChild(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    nd_node *parent = (nd_node *)nd_unwrap_element(this_val);
    if (!parent || argc < 1) return JS_NULL;
    nd_node *child = (nd_node *)nd_unwrap_element(argv[0]);
    if (!child || child->parent != parent) return JS_NULL;
    nd_node_remove(child);
    if (g_active_js) {
        g_ptr_array_add(g_active_js->orphan_nodes, child);
        g_active_js->mutated = TRUE;
    }
    return JS_DupValue(ctx, argv[0]);
}

static JSValue
nd_element_setAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    nd_node *n = (nd_node *)nd_unwrap_element(this_val);
    if (!n || argc < 2) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *val  = JS_ToCString(ctx, argv[1]);
    if (name && val) {
        nd_element_set_attr(n, name, val);
        if (g_active_js) g_active_js->mutated = TRUE;
    }
    if (name) JS_FreeCString(ctx, name);
    if (val)  JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

static JSValue
nd_element_removeAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    nd_node *n = (nd_node *)nd_unwrap_element(this_val);
    if (!n || argc < 1 || n->kind != ND_NODE_ELEMENT) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_UNDEFINED;
    nd_attr **prev = &n->attrs;
    for (nd_attr *a = n->attrs; a; prev = &a->next, a = a->next) {
        if (strcmp(a->name, name) == 0) {
            *prev = a->next;
            g_free(a->name);
            g_free(a->value);
            g_free(a);
            if (g_active_js) g_active_js->mutated = TRUE;
            break;
        }
    }
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
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
    JS_CGETSET_DEF("textContent",            nd_element_get_textContent,            nd_element_set_textContent),
    JS_CGETSET_DEF("id",                     nd_element_get_id,                     NULL),
    JS_CGETSET_DEF("className",              nd_element_get_className,              NULL),
    JS_CGETSET_DEF("innerHTML",              nd_element_get_innerHTML,              nd_element_set_innerHTML),
    JS_CGETSET_DEF("outerHTML",              nd_element_get_outerHTML,              NULL),
    JS_CGETSET_DEF("parentElement",          nd_element_get_parentElement,          NULL),
    JS_CGETSET_DEF("parentNode",             nd_element_get_parentElement,          NULL),
    JS_CGETSET_DEF("firstElementChild",      nd_element_get_firstElementChild,      NULL),
    JS_CGETSET_DEF("nextElementSibling",     nd_element_get_nextElementSibling,     NULL),
    JS_CGETSET_DEF("previousElementSibling", nd_element_get_previousElementSibling, NULL),
    JS_CGETSET_DEF("children",               nd_element_get_children,               NULL),
    JS_CFUNC_DEF("getAttribute",            1, nd_element_getAttribute),
    JS_CFUNC_DEF("hasAttribute",            1, nd_element_hasAttribute),
    JS_CFUNC_DEF("setAttribute",            2, nd_element_setAttribute),
    JS_CFUNC_DEF("removeAttribute",         1, nd_element_removeAttribute),
    JS_CFUNC_DEF("appendChild",             1, nd_element_appendChild),
    JS_CFUNC_DEF("removeChild",             1, nd_element_removeChild),    JS_CFUNC_DEF("getElementsByTagName",    1, nd_element_getElementsByTagName),
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
nd_js_new(nd_js_log_cb log_cb, gpointer log_user_data,
          nd_js_mutated_cb mut_cb, gpointer mut_user_data)
{
    nd_js *js = g_new0(nd_js, 1);
    js->rt = JS_NewRuntime();
    if (!js->rt) { g_free(js); return NULL; }
    js->ctx = JS_NewContext(js->rt);
    if (!js->ctx) { JS_FreeRuntime(js->rt); g_free(js); return NULL; }
    js->log_cb = log_cb;
    js->log_user_data = log_user_data;
    js->mut_cb = mut_cb;
    js->mut_user_data = mut_user_data;
    js->timers = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                       NULL, nd_timer_free);
    js->orphan_nodes = g_ptr_array_new();

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
    JS_SetPropertyStr(js->ctx, global, "setTimeout",
                      JS_NewCFunction(js->ctx, nd_js_setTimeout_wrap, "setTimeout", 2));
    JS_SetPropertyStr(js->ctx, global, "setInterval",
                      JS_NewCFunction(js->ctx, nd_js_setInterval_wrap, "setInterval", 2));
    JS_SetPropertyStr(js->ctx, global, "clearTimeout",
                      JS_NewCFunction(js->ctx, nd_js_clearTimer, "clearTimeout", 1));
    JS_SetPropertyStr(js->ctx, global, "clearInterval",
                      JS_NewCFunction(js->ctx, nd_js_clearTimer, "clearInterval", 1));

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
nd_document_createElement(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_active_js || argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;
    char *lower = g_ascii_strdown(name, -1);
    JS_FreeCString(ctx, name);
    nd_node *el = nd_node_new_element(lower);
    g_ptr_array_add(g_active_js->orphan_nodes, el);
    return nd_make_element(ctx, el);
}

static JSValue
nd_document_createTextNode(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_active_js || argc < 1) return JS_NULL;
    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_NULL;
    char *dup = g_strdup(text);
    JS_FreeCString(ctx, text);
    nd_node *n = nd_node_new_text(dup);
    g_ptr_array_add(g_active_js->orphan_nodes, n);
    return nd_make_element(ctx, n);
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
    JS_CFUNC_DEF("createElement",           1, nd_document_createElement),
    JS_CFUNC_DEF("createTextNode",          1, nd_document_createTextNode),
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
    if (js->timers) g_hash_table_destroy(js->timers);
    if (js->orphan_nodes) {
        for (guint i = 0; i < js->orphan_nodes->len; i++)
            nd_node_free(g_ptr_array_index(js->orphan_nodes, i));
        g_ptr_array_free(js->orphan_nodes, TRUE);
    }
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

gboolean
nd_js_consume_mutated(nd_js *js)
{
    if (!js) return FALSE;
    gboolean m = js->mutated;
    js->mutated = FALSE;
    return m;
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
