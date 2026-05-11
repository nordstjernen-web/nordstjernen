/* Nordstjernen — JavaScript engine binding (QuickJS). */

#include "js.h"

#include <string.h>

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <quickjs.h>

#include "css.h"
#include "html.h"
#include "net.h"

struct nd_js {
    JSRuntime    *rt;
    JSContext    *ctx;
    nd_js_log_cb  log_cb;
    gpointer      log_user_data;
    nd_js_mutated_cb mut_cb;
    gpointer      mut_user_data;
    nd_js_navigate_cb nav_cb;
    gpointer      nav_user_data;
    char         *current_url;
    const nd_node *current_doc;
    gboolean      mutated;
    GHashTable   *timers;
    int           next_timer_id;
    GPtrArray    *orphan_nodes;
    GPtrArray    *listeners;
    GHashTable   *local_storage;
    GHashTable   *session_storage;
    char         *local_storage_origin;
    char         *local_storage_path;
    gboolean      local_storage_dirty;
    gboolean      local_storage_disabled;
};

typedef struct nd_listener {
    const nd_node *target;
    char          *type;
    JSValue        cb;
} nd_listener;

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
nd_drain_microtasks(nd_js *js)
{
    if (!js) return;
    JSContext *ctx_out = NULL;
    int r;
    int safety = 1000;
    while (safety-- > 0 && (r = JS_ExecutePendingJob(js->rt, &ctx_out)) > 0)
        ;
    if (r < 0 && js->log_cb)
        js->log_cb("[error] microtask threw", js->log_user_data);
}

static void nd_storage_flush(nd_js *js);

static void
nd_drain_mutations(nd_js *js)
{
    nd_drain_microtasks(js);
    if (js->mutated && js->mut_cb)
        js->mut_cb(js->mut_user_data);
    js->mutated = FALSE;
    nd_storage_flush(js);
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
static JSClassID nd_style_class_id;
static JSClassID nd_token_list_class_id;
static JSClassID nd_storage_class_id;

static void
nd_style_finalizer(JSRuntime *rt, JSValue val) { (void)rt; (void)val; }

static char *
camel_to_kebab(const char *s)
{
    GString *out = g_string_new(NULL);
    for (const char *p = s; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            if (p != s) g_string_append_c(out, '-');
            g_string_append_c(out, (char)(*p - 'A' + 'a'));
        } else {
            g_string_append_c(out, *p);
        }
    }
    return g_string_free(out, FALSE);
}

static int
nd_style_get_own_property(JSContext *ctx, JSPropertyDescriptor *desc,
                          JSValueConst obj, JSAtom prop)
{
    nd_node *n = JS_GetOpaque(obj, nd_style_class_id);
    if (!n) return 0;
    const char *name = JS_AtomToCString(ctx, prop);
    if (!name) return 0;
    if (strcmp(name, "cssText") == 0 || strcmp(name, "constructor") == 0) {
        JS_FreeCString(ctx, name);
        return 0;
    }
    char *css = camel_to_kebab(name);
    JS_FreeCString(ctx, name);
    const char *style = nd_element_get_attr(n, "style");
    char *val = nd_inline_style_get(style, css);
    g_free(css);
    if (!val) return 0;
    if (desc) {
        desc->flags  = JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE | JS_PROP_WRITABLE;
        desc->value  = JS_NewString(ctx, val);
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    }
    g_free(val);
    return 1;
}

static int
nd_style_set_property(JSContext *ctx, JSValueConst obj, JSAtom prop,
                      JSValueConst val, JSValueConst receiver, int flags)
{
    (void)receiver; (void)flags;
    nd_node *n = JS_GetOpaque(obj, nd_style_class_id);
    if (!n) return FALSE;
    const char *name = JS_AtomToCString(ctx, prop);
    if (!name) return FALSE;
    if (strcmp(name, "cssText") == 0) {
        JS_FreeCString(ctx, name);
        const char *s = JS_ToCString(ctx, val);
        if (s) {
            nd_element_set_attr(n, "style", s);
            JS_FreeCString(ctx, s);
            if (g_active_js) g_active_js->mutated = TRUE;
        }
        return TRUE;
    }
    char *css = camel_to_kebab(name);
    JS_FreeCString(ctx, name);
    const char *vstr = JS_ToCString(ctx, val);
    const char *old = nd_element_get_attr(n, "style");
    char *new_style = nd_inline_style_set(old, css, vstr ? vstr : "");
    nd_element_set_attr(n, "style", new_style);
    g_free(new_style);
    g_free(css);
    if (vstr) JS_FreeCString(ctx, vstr);
    if (g_active_js) g_active_js->mutated = TRUE;
    return TRUE;
}

static JSClassExoticMethods nd_style_exotic = {
    .get_own_property = nd_style_get_own_property,
    .set_property     = nd_style_set_property,
};

static JSClassDef nd_style_class = {
    .class_name = "CSSStyleDeclaration",
    .finalizer  = nd_style_finalizer,
    .exotic     = &nd_style_exotic,
};

static void
nd_token_list_finalizer(JSRuntime *rt, JSValue val) { (void)rt; (void)val; }

static JSClassDef nd_token_list_class = {
    .class_name = "DOMTokenList",
    .finalizer  = nd_token_list_finalizer,
};

static gboolean
class_attr_contains(const char *cls, const char *token, gsize tlen,
                    const char **out_start, gsize *out_len)
{
    if (!cls) return FALSE;
    const char *p = cls;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if ((gsize)(p - tok) == tlen && strncmp(tok, token, tlen) == 0) {
            if (out_start) *out_start = tok;
            if (out_len)   *out_len = (gsize)(p - tok);
            return TRUE;
        }
    }
    return FALSE;
}

static char *
class_attr_add(const char *cls, const char *token)
{
    gsize tlen = strlen(token);
    if (class_attr_contains(cls, token, tlen, NULL, NULL))
        return cls ? g_strdup(cls) : g_strdup("");
    if (!cls || !*cls) return g_strdup(token);
    return g_strdup_printf("%s %s", cls, token);
}

static char *
class_attr_remove(const char *cls, const char *token)
{
    if (!cls) return g_strdup("");
    gsize tlen = strlen(token);
    GString *out = g_string_new(NULL);
    const char *p = cls;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if ((gsize)(p - tok) == tlen && strncmp(tok, token, tlen) == 0)
            continue;
        if (out->len > 0) g_string_append_c(out, ' ');
        g_string_append_len(out, tok, (gsize)(p - tok));
    }
    return g_string_free(out, FALSE);
}

static JSValue
nd_tlist_contains(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    nd_node *n = JS_GetOpaque(this_val, nd_token_list_class_id);
    if (!n || argc < 1) return JS_FALSE;
    const char *t = JS_ToCString(ctx, argv[0]);
    if (!t) return JS_FALSE;
    const char *cls = nd_element_get_attr(n, "class");
    gboolean has = class_attr_contains(cls, t, strlen(t), NULL, NULL);
    JS_FreeCString(ctx, t);
    return has ? JS_TRUE : JS_FALSE;
}

static JSValue
nd_tlist_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    nd_node *n = JS_GetOpaque(this_val, nd_token_list_class_id);
    if (!n) return JS_UNDEFINED;
    for (int i = 0; i < argc; i++) {
        const char *t = JS_ToCString(ctx, argv[i]);
        if (!t) continue;
        char *next = class_attr_add(nd_element_get_attr(n, "class"), t);
        nd_element_set_attr(n, "class", next);
        g_free(next);
        JS_FreeCString(ctx, t);
    }
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_tlist_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    nd_node *n = JS_GetOpaque(this_val, nd_token_list_class_id);
    if (!n) return JS_UNDEFINED;
    for (int i = 0; i < argc; i++) {
        const char *t = JS_ToCString(ctx, argv[i]);
        if (!t) continue;
        char *next = class_attr_remove(nd_element_get_attr(n, "class"), t);
        nd_element_set_attr(n, "class", next);
        g_free(next);
        JS_FreeCString(ctx, t);
    }
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_tlist_toggle(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    nd_node *n = JS_GetOpaque(this_val, nd_token_list_class_id);
    if (!n || argc < 1) return JS_FALSE;
    const char *t = JS_ToCString(ctx, argv[0]);
    if (!t) return JS_FALSE;
    const char *cls = nd_element_get_attr(n, "class");
    gboolean has = class_attr_contains(cls, t, strlen(t), NULL, NULL);
    char *next = has ? class_attr_remove(cls, t) : class_attr_add(cls, t);
    nd_element_set_attr(n, "class", next);
    g_free(next);
    JS_FreeCString(ctx, t);
    if (g_active_js) g_active_js->mutated = TRUE;
    return has ? JS_FALSE : JS_TRUE;
}

static char *
nd_origin_of(const char *url)
{
    if (!url || !*url) return NULL;
    if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://"))
        return NULL;
    const char *scheme_end = strstr(url, "://");
    const char *authority = scheme_end + 3;
    const char *p = authority;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    return g_strndup(url, (gsize)(p - url));
}

static char *
nd_storage_path_for_origin(const char *origin)
{
    if (!origin || !*origin) return NULL;
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, origin, -1);
    char *dir = g_build_filename(g_get_user_data_dir(), "nordstjernen",
                                 "localstorage", NULL);
    g_mkdir_with_parents(dir, 0700);
    g_chmod(dir, 0700);
    char *file = g_strdup_printf("%s.ini", hash);
    char *full = g_build_filename(dir, file, NULL);
    g_free(hash); g_free(dir); g_free(file);
    return full;
}

static void
nd_storage_flush(nd_js *js)
{
    if (!js || !js->local_storage_dirty || !js->local_storage_path) return;
    if (js->local_storage_disabled) { js->local_storage_dirty = FALSE; return; }
    GKeyFile *kf = g_key_file_new();
    if (js->local_storage_origin)
        g_key_file_set_string(kf, "meta", "origin", js->local_storage_origin);
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, js->local_storage);
    while (g_hash_table_iter_next(&it, &k, &v))
        g_key_file_set_string(kf, "storage", (const char *)k, (const char *)v);
    gsize len = 0;
    char *data = g_key_file_to_data(kf, &len, NULL);
    if (data) {
        g_file_set_contents(js->local_storage_path, data, (gssize)len, NULL);
        g_chmod(js->local_storage_path, 0600);
        g_free(data);
    }
    g_key_file_free(kf);
    js->local_storage_dirty = FALSE;
}

static void
nd_storage_load_for(nd_js *js, const char *new_url)
{
    if (!js) return;
    if (js->local_storage_disabled) {
        g_hash_table_remove_all(js->local_storage);
        return;
    }
    char *new_origin = nd_origin_of(new_url);
    if (js->local_storage_origin && new_origin &&
        strcmp(js->local_storage_origin, new_origin) == 0) {
        g_free(new_origin);
        return;
    }
    nd_storage_flush(js);
    g_hash_table_remove_all(js->local_storage);
    g_free(js->local_storage_origin);
    g_free(js->local_storage_path);
    js->local_storage_origin = new_origin;
    js->local_storage_path = nd_storage_path_for_origin(new_origin);
    if (!js->local_storage_path) return;
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, js->local_storage_path, G_KEY_FILE_NONE, NULL)) {
        gsize n = 0;
        char **keys = g_key_file_get_keys(kf, "storage", &n, NULL);
        if (keys) {
            for (gsize i = 0; i < n; i++) {
                char *v = g_key_file_get_string(kf, "storage", keys[i], NULL);
                if (v) g_hash_table_replace(js->local_storage,
                                            g_strdup(keys[i]), v);
            }
            g_strfreev(keys);
        }
    }
    g_key_file_free(kf);
}

static void
nd_storage_finalizer(JSRuntime *rt, JSValue val) { (void)rt; (void)val; }

static JSClassDef nd_storage_class = {
    .class_name = "Storage",
    .finalizer  = nd_storage_finalizer,
};

static JSValue
nd_storage_getItem(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    GHashTable *store = JS_GetOpaque(this_val, nd_storage_class_id);
    if (!store || argc < 1) return JS_NULL;
    const char *k = JS_ToCString(ctx, argv[0]);
    if (!k) return JS_NULL;
    const char *v = g_hash_table_lookup(store, k);
    JS_FreeCString(ctx, k);
    return v ? JS_NewString(ctx, v) : JS_NULL;
}

static void
nd_storage_maybe_dirty(GHashTable *store)
{
    if (g_active_js && store == g_active_js->local_storage)
        g_active_js->local_storage_dirty = TRUE;
}

static JSValue
nd_storage_setItem(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    GHashTable *store = JS_GetOpaque(this_val, nd_storage_class_id);
    if (!store || argc < 2) return JS_UNDEFINED;
    const char *k = JS_ToCString(ctx, argv[0]);
    const char *v = JS_ToCString(ctx, argv[1]);
    if (k && v) {
        g_hash_table_replace(store, g_strdup(k), g_strdup(v));
        nd_storage_maybe_dirty(store);
    }
    if (k) JS_FreeCString(ctx, k);
    if (v) JS_FreeCString(ctx, v);
    return JS_UNDEFINED;
}

static JSValue
nd_storage_removeItem(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    GHashTable *store = JS_GetOpaque(this_val, nd_storage_class_id);
    if (!store || argc < 1) return JS_UNDEFINED;
    const char *k = JS_ToCString(ctx, argv[0]);
    if (k && g_hash_table_remove(store, k))
        nd_storage_maybe_dirty(store);
    if (k) JS_FreeCString(ctx, k);
    return JS_UNDEFINED;
}

static JSValue
nd_storage_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    GHashTable *store = JS_GetOpaque(this_val, nd_storage_class_id);
    if (store && g_hash_table_size(store) > 0) {
        g_hash_table_remove_all(store);
        nd_storage_maybe_dirty(store);
    }
    return JS_UNDEFINED;
}

static JSValue
nd_storage_key(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    GHashTable *store = JS_GetOpaque(this_val, nd_storage_class_id);
    if (!store || argc < 1) return JS_NULL;
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    if (idx < 0) return JS_NULL;
    GList *keys = g_hash_table_get_keys(store);
    keys = g_list_sort(keys, (GCompareFunc)strcmp);
    GList *node = g_list_nth(keys, (guint)idx);
    JSValue ret = node ? JS_NewString(ctx, node->data) : JS_NULL;
    g_list_free(keys);
    return ret;
}

static JSValue
nd_storage_get_length(JSContext *ctx, JSValueConst this_val)
{
    GHashTable *store = JS_GetOpaque(this_val, nd_storage_class_id);
    return JS_NewInt32(ctx, store ? (int)g_hash_table_size(store) : 0);
}

static const JSCFunctionListEntry nd_storage_proto_funcs[] = {
    JS_CFUNC_DEF("getItem",    1, nd_storage_getItem),
    JS_CFUNC_DEF("setItem",    2, nd_storage_setItem),
    JS_CFUNC_DEF("removeItem", 1, nd_storage_removeItem),
    JS_CFUNC_DEF("clear",      0, nd_storage_clear),
    JS_CFUNC_DEF("key",        1, nd_storage_key),
    JS_CGETSET_DEF("length", nd_storage_get_length, NULL),
};

static const JSCFunctionListEntry nd_tlist_proto_funcs[] = {
    JS_CFUNC_DEF("contains", 1, nd_tlist_contains),
    JS_CFUNC_DEF("add",      1, nd_tlist_add),
    JS_CFUNC_DEF("remove",   1, nd_tlist_remove),
    JS_CFUNC_DEF("toggle",   1, nd_tlist_toggle),
};

static JSValue
nd_style_get_cssText(JSContext *ctx, JSValueConst this_val)
{
    nd_node *n = JS_GetOpaque(this_val, nd_style_class_id);
    if (!n) return JS_NewString(ctx, "");
    const char *s = nd_element_get_attr(n, "style");
    return JS_NewString(ctx, s ? s : "");
}

static JSValue
nd_style_set_cssText(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    nd_node *n = JS_GetOpaque(this_val, nd_style_class_id);
    if (!n) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (s) {
        nd_element_set_attr(n, "style", s);
        JS_FreeCString(ctx, s);
        if (g_active_js) g_active_js->mutated = TRUE;
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry nd_style_proto_funcs[] = {
    JS_CGETSET_DEF("cssText", nd_style_get_cssText, nd_style_set_cssText),
};

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
nd_element_get_style(JSContext *ctx, JSValueConst this_val)
{
    nd_node *n = (nd_node *)nd_unwrap_element(this_val);
    if (!n) return JS_NULL;
    JSValue obj = JS_NewObjectClass(ctx, nd_style_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, n);
    return obj;
}

static JSValue
nd_element_get_classList(JSContext *ctx, JSValueConst this_val)
{
    nd_node *n = (nd_node *)nd_unwrap_element(this_val);
    if (!n) return JS_NULL;
    JSValue obj = JS_NewObjectClass(ctx, nd_token_list_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, n);
    return obj;
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
nd_element_addEventListener(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n || argc < 2 || !g_active_js) return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    if (!JS_IsFunction(ctx, argv[1])) { JS_FreeCString(ctx, type); return JS_UNDEFINED; }
    nd_listener *l = g_new0(nd_listener, 1);
    l->target = n;
    l->type   = g_strdup(type);
    l->cb     = JS_DupValue(ctx, argv[1]);
    g_ptr_array_add(g_active_js->listeners, l);
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

static JSValue
nd_element_removeEventListener(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n || argc < 2 || !g_active_js) return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    for (guint i = 0; i < g_active_js->listeners->len; i++) {
        nd_listener *l = g_ptr_array_index(g_active_js->listeners, i);
        if (l->target == n && strcmp(l->type, type) == 0 &&
            JS_VALUE_GET_PTR(l->cb) == JS_VALUE_GET_PTR(argv[1])) {
            JS_FreeValue(ctx, l->cb);
            g_free(l->type);
            g_free(l);
            g_ptr_array_remove_index_fast(g_active_js->listeners, i);
            break;
        }
    }
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

typedef struct nd_js_fetch_state {
    JSContext *ctx;
    nd_js     *js;
    JSValue    resolve;
    JSValue    reject;
} nd_js_fetch_state;

static void
nd_on_js_fetch_done(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    nd_js_fetch_state *st = user_data;
    GError *err = NULL;
    nd_response *resp = nd_net_fetch_finish(result, &err);
    g_active_js = st->js;
    if (!resp || resp->error) {
        const char *msg = resp ? resp->error :
                                (err ? err->message : "fetch failed");
        JSValue m = JS_NewString(st->ctx, msg ? msg : "fetch failed");
        JS_Call(st->ctx, st->reject, JS_UNDEFINED, 1, &m);
        JS_FreeValue(st->ctx, m);
        if (resp) nd_response_free(resp);
        if (err) g_error_free(err);
    } else {
        JSValue r = JS_NewObject(st->ctx);
        JS_SetPropertyStr(st->ctx, r, "ok",
            JS_NewBool(st->ctx, resp->status >= 200 && resp->status < 300));
        JS_SetPropertyStr(st->ctx, r, "status", JS_NewInt32(st->ctx, (int)resp->status));
        JS_SetPropertyStr(st->ctx, r, "statusText", JS_NewString(st->ctx, ""));
        JS_SetPropertyStr(st->ctx, r, "url",
            JS_NewString(st->ctx, resp->final_url ? resp->final_url : ""));
        char *body_text = NULL;
        if (resp->body && resp->body->len > 0)
            body_text = g_strndup((const char *)resp->body->data, resp->body->len);
        JS_SetPropertyStr(st->ctx, r, "body",
            JS_NewString(st->ctx, body_text ? body_text : ""));
        char *script = g_strdup_printf(
            "(function(r){"
            " r.text = function(){return Promise.resolve(r.body);};"
            " r.json = function(){return Promise.resolve(JSON.parse(r.body));};"
            " return r;"
            "})");
        JSValue helper = JS_Eval(st->ctx, script, strlen(script), "fetch", JS_EVAL_TYPE_GLOBAL);
        g_free(script);
        if (!JS_IsException(helper)) {
            JSValue called = JS_Call(st->ctx, helper, JS_UNDEFINED, 1, &r);
            JS_FreeValue(st->ctx, called);
        }
        JS_FreeValue(st->ctx, helper);
        JS_Call(st->ctx, st->resolve, JS_UNDEFINED, 1, &r);
        JS_FreeValue(st->ctx, r);
        g_free(body_text);
        nd_response_free(resp);
    }
    JS_FreeValue(st->ctx, st->resolve);
    JS_FreeValue(st->ctx, st->reject);
    nd_drain_mutations(st->js);
    g_active_js = NULL;
    g_free(st);
}

static JSValue
nd_js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_active_js || argc < 1)
        return JS_ThrowTypeError(ctx, "fetch requires a URL");
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) {
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        return promise;
    }
    nd_js_fetch_state *st = g_new0(nd_js_fetch_state, 1);
    st->ctx = ctx;
    st->js = g_active_js;
    st->resolve = resolving[0];
    st->reject  = resolving[1];
    nd_net_fetch_async(url, NULL, nd_on_js_fetch_done, st);
    JS_FreeCString(ctx, url);
    return promise;
}

static JSValue
nd_event_noop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static JSValue nd_document_addEventListener(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv);
static JSValue nd_document_removeEventListener(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv);

static JSValue
nd_window_get_property_value_stub(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewString(ctx, "");
}

static JSValue
nd_window_matchMedia(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    (void)this_val;
    const char *q = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    JSValue mql = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, mql, "matches", JS_FALSE);
    JS_SetPropertyStr(ctx, mql, "media", JS_NewString(ctx, q ? q : ""));
    JS_SetPropertyStr(ctx, mql, "addListener",
        JS_NewCFunction(ctx, nd_event_noop, "addListener", 1));
    JS_SetPropertyStr(ctx, mql, "removeListener",
        JS_NewCFunction(ctx, nd_event_noop, "removeListener", 1));
    JS_SetPropertyStr(ctx, mql, "addEventListener",
        JS_NewCFunction(ctx, nd_event_noop, "addEventListener", 2));
    JS_SetPropertyStr(ctx, mql, "removeEventListener",
        JS_NewCFunction(ctx, nd_event_noop, "removeEventListener", 2));
    if (q) JS_FreeCString(ctx, q);
    return mql;
}

static JSValue
nd_window_getComputedStyle(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue cs = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, cs, "getPropertyValue",
        JS_NewCFunction(ctx, nd_window_get_property_value_stub, "getPropertyValue", 1));
    return cs;
}

static JSValue
nd_window_requestAnimationFrame(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    JSValueConst args[2] = { argv[0], JS_NewInt32(ctx, 16) };
    JSValue ret = nd_js_setTimeout_wrap(ctx, this_val, 2, args);
    JS_FreeValue(ctx, args[1]);
    return ret;
}

static JSValue
nd_event_prevent_default(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, this_val, "defaultPrevented", JS_TRUE);
    return JS_UNDEFINED;
}

static JSValue
nd_event_stop_propagation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, this_val, "_propagation_stopped", JS_TRUE);
    return JS_UNDEFINED;
}

static JSValue
nd_make_event(JSContext *ctx, const char *type, const nd_node *target)
{
    JSValue event = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, type));
    JS_SetPropertyStr(ctx, event, "target", nd_make_element(ctx, target));
    JS_SetPropertyStr(ctx, event, "defaultPrevented", JS_FALSE);
    JS_SetPropertyStr(ctx, event, "bubbles", JS_TRUE);
    JS_SetPropertyStr(ctx, event, "cancelable", JS_TRUE);
    JS_SetPropertyStr(ctx, event, "preventDefault",
        JS_NewCFunction(ctx, nd_event_prevent_default, "preventDefault", 0));
    JS_SetPropertyStr(ctx, event, "stopPropagation",
        JS_NewCFunction(ctx, nd_event_stop_propagation, "stopPropagation", 0));
    JS_SetPropertyStr(ctx, event, "stopImmediatePropagation",
        JS_NewCFunction(ctx, nd_event_noop, "stopImmediatePropagation", 0));
    return event;
}

static gboolean
nd_js_dispatch_built_event(nd_js *js, const nd_node *target, const char *type,
                           JSValue event, gboolean *default_prevented)
{
    gboolean fired = FALSE;
    g_active_js = js;
    gboolean stopped = FALSE;
    for (const nd_node *cur = target; cur && !stopped; cur = cur->parent) {
        for (guint i = 0; i < js->listeners->len; i++) {
            nd_listener *l = g_ptr_array_index(js->listeners, i);
            if (l->target != cur || strcmp(l->type, type) != 0) continue;
            JS_SetPropertyStr(js->ctx, event, "currentTarget", nd_make_element(js->ctx, cur));
            JSValue ret = JS_Call(js->ctx, l->cb, JS_UNDEFINED, 1, &event);
            JSValue stop_prop = JS_GetPropertyStr(js->ctx, event, "_propagation_stopped");
            if (JS_ToBool(js->ctx, stop_prop)) stopped = TRUE;
            JS_FreeValue(js->ctx, stop_prop);
            if (JS_IsException(ret)) {
                JSValue ex = JS_GetException(js->ctx);
                const char *m = JS_ToCString(js->ctx, ex);
                if (m && js->log_cb) {
                    char *line = g_strdup_printf("JS error in %s handler: %s", type, m);
                    js->log_cb(line, js->log_user_data);
                    g_free(line);
                }
                if (m) JS_FreeCString(js->ctx, m);
                JS_FreeValue(js->ctx, ex);
            }
            JS_FreeValue(js->ctx, ret);
            fired = TRUE;
        }
    }
    if (default_prevented) {
        JSValue dp = JS_GetPropertyStr(js->ctx, event, "defaultPrevented");
        *default_prevented = JS_ToBool(js->ctx, dp) ? TRUE : FALSE;
        JS_FreeValue(js->ctx, dp);
    }
    JS_FreeValue(js->ctx, event);
    nd_drain_mutations(js);
    g_active_js = NULL;
    return fired;
}

gboolean
nd_js_dispatch_event(nd_js *js, const nd_node *target, const char *type,
                     gboolean *default_prevented)
{
    if (default_prevented) *default_prevented = FALSE;
    if (!js || !target || !type) return FALSE;
    JSValue event = nd_make_event(js->ctx, type, target);
    return nd_js_dispatch_built_event(js, target, type, event, default_prevented);
}

gboolean
nd_js_dispatch_key_event(nd_js *js, const nd_node *target, const char *type,
                         const char *key, const char *code, int key_code,
                         gboolean shift, gboolean ctrl, gboolean alt, gboolean meta,
                         gboolean *default_prevented)
{
    if (default_prevented) *default_prevented = FALSE;
    if (!js || !target || !type) return FALSE;
    JSValue event = nd_make_event(js->ctx, type, target);
    JS_SetPropertyStr(js->ctx, event, "key",     JS_NewString(js->ctx, key  ? key  : ""));
    JS_SetPropertyStr(js->ctx, event, "code",    JS_NewString(js->ctx, code ? code : ""));
    JS_SetPropertyStr(js->ctx, event, "keyCode", JS_NewInt32 (js->ctx, key_code));
    JS_SetPropertyStr(js->ctx, event, "which",   JS_NewInt32 (js->ctx, key_code));
    JS_SetPropertyStr(js->ctx, event, "shiftKey", shift ? JS_TRUE : JS_FALSE);
    JS_SetPropertyStr(js->ctx, event, "ctrlKey",  ctrl  ? JS_TRUE : JS_FALSE);
    JS_SetPropertyStr(js->ctx, event, "altKey",   alt   ? JS_TRUE : JS_FALSE);
    JS_SetPropertyStr(js->ctx, event, "metaKey",  meta  ? JS_TRUE : JS_FALSE);
    JS_SetPropertyStr(js->ctx, event, "repeat",   JS_FALSE);
    return nd_js_dispatch_built_event(js, target, type, event, default_prevented);
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
    JS_CGETSET_DEF("style",                  nd_element_get_style,                  NULL),
    JS_CGETSET_DEF("classList",              nd_element_get_classList,              NULL),
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
    JS_CFUNC_DEF("removeChild",             1, nd_element_removeChild),
    JS_CFUNC_DEF("addEventListener",        2, nd_element_addEventListener),
    JS_CFUNC_DEF("removeEventListener",     2, nd_element_removeEventListener),    JS_CFUNC_DEF("getElementsByTagName",    1, nd_element_getElementsByTagName),
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
          nd_js_mutated_cb mut_cb, gpointer mut_user_data,
          nd_js_navigate_cb nav_cb, gpointer nav_user_data)
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
    js->nav_cb = nav_cb;
    js->nav_user_data = nav_user_data;
    js->timers = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                       NULL, nd_timer_free);
    js->orphan_nodes = g_ptr_array_new();
    js->listeners    = g_ptr_array_new();
    js->local_storage   = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    js->session_storage = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    js->local_storage_disabled = g_getenv("ND_NO_LOCAL_STORAGE") != NULL;

    if (!nd_element_class_id)
        JS_NewClassID(js->rt, &nd_element_class_id);
    JS_NewClass(js->rt, nd_element_class_id, &nd_element_class);
    JSValue element_proto = JS_NewObject(js->ctx);
    JS_SetPropertyFunctionList(js->ctx, element_proto, nd_element_proto_funcs,
                               G_N_ELEMENTS(nd_element_proto_funcs));
    JS_SetClassProto(js->ctx, nd_element_class_id, element_proto);

    if (!nd_style_class_id)
        JS_NewClassID(js->rt, &nd_style_class_id);
    JS_NewClass(js->rt, nd_style_class_id, &nd_style_class);
    JSValue style_proto = JS_NewObject(js->ctx);
    JS_SetPropertyFunctionList(js->ctx, style_proto, nd_style_proto_funcs,
                               G_N_ELEMENTS(nd_style_proto_funcs));
    JS_SetClassProto(js->ctx, nd_style_class_id, style_proto);

    if (!nd_token_list_class_id)
        JS_NewClassID(js->rt, &nd_token_list_class_id);
    JS_NewClass(js->rt, nd_token_list_class_id, &nd_token_list_class);
    JSValue tlist_proto = JS_NewObject(js->ctx);
    JS_SetPropertyFunctionList(js->ctx, tlist_proto, nd_tlist_proto_funcs,
                               G_N_ELEMENTS(nd_tlist_proto_funcs));
    JS_SetClassProto(js->ctx, nd_token_list_class_id, tlist_proto);

    if (!nd_storage_class_id)
        JS_NewClassID(js->rt, &nd_storage_class_id);
    JS_NewClass(js->rt, nd_storage_class_id, &nd_storage_class);
    JSValue storage_proto = JS_NewObject(js->ctx);
    JS_SetPropertyFunctionList(js->ctx, storage_proto, nd_storage_proto_funcs,
                               G_N_ELEMENTS(nd_storage_proto_funcs));
    JS_SetClassProto(js->ctx, nd_storage_class_id, storage_proto);

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
    JS_SetPropertyStr(js->ctx, global, "fetch",
                      JS_NewCFunction(js->ctx, nd_js_fetch, "fetch", 1));

    JSValue navigator = JS_NewObject(js->ctx);
    JS_SetPropertyStr(js->ctx, navigator, "userAgent",
                      JS_NewString(js->ctx, "Nordstjernen/0.0.1"));
    JS_SetPropertyStr(js->ctx, navigator, "appName",
                      JS_NewString(js->ctx, "Nordstjernen"));
    JS_SetPropertyStr(js->ctx, global, "navigator", navigator);

    JS_SetPropertyStr(js->ctx, global, "addEventListener",
        JS_NewCFunction(js->ctx, nd_document_addEventListener, "addEventListener", 2));
    JS_SetPropertyStr(js->ctx, global, "removeEventListener",
        JS_NewCFunction(js->ctx, nd_document_removeEventListener, "removeEventListener", 2));
    JS_SetPropertyStr(js->ctx, global, "scrollY", JS_NewInt32(js->ctx, 0));
    JS_SetPropertyStr(js->ctx, global, "scrollX", JS_NewInt32(js->ctx, 0));
    JS_SetPropertyStr(js->ctx, global, "pageYOffset", JS_NewInt32(js->ctx, 0));
    JS_SetPropertyStr(js->ctx, global, "pageXOffset", JS_NewInt32(js->ctx, 0));
    JS_SetPropertyStr(js->ctx, global, "innerWidth",  JS_NewInt32(js->ctx, 1000));
    JS_SetPropertyStr(js->ctx, global, "innerHeight", JS_NewInt32(js->ctx, 800));
    JS_SetPropertyStr(js->ctx, global, "outerWidth",  JS_NewInt32(js->ctx, 1000));
    JS_SetPropertyStr(js->ctx, global, "outerHeight", JS_NewInt32(js->ctx, 800));
    JS_SetPropertyStr(js->ctx, global, "devicePixelRatio", JS_NewFloat64(js->ctx, 1.0));
    JS_SetPropertyStr(js->ctx, global, "scrollTo",
        JS_NewCFunction(js->ctx, nd_event_noop, "scrollTo", 2));
    JS_SetPropertyStr(js->ctx, global, "scrollBy",
        JS_NewCFunction(js->ctx, nd_event_noop, "scrollBy", 2));
    JS_SetPropertyStr(js->ctx, global, "scroll",
        JS_NewCFunction(js->ctx, nd_event_noop, "scroll", 2));
    JS_SetPropertyStr(js->ctx, global, "matchMedia",
        JS_NewCFunction(js->ctx, nd_window_matchMedia, "matchMedia", 1));
    JS_SetPropertyStr(js->ctx, global, "getComputedStyle",
        JS_NewCFunction(js->ctx, nd_window_getComputedStyle, "getComputedStyle", 1));
    JS_SetPropertyStr(js->ctx, global, "requestAnimationFrame",
        JS_NewCFunction(js->ctx, nd_window_requestAnimationFrame, "requestAnimationFrame", 1));
    JS_SetPropertyStr(js->ctx, global, "cancelAnimationFrame",
        JS_NewCFunction(js->ctx, nd_event_noop, "cancelAnimationFrame", 1));

    JS_SetPropertyStr(js->ctx, global, "window", JS_DupValue(js->ctx, global));

    JSValue local_obj = JS_NewObjectClass(js->ctx, nd_storage_class_id);
    JS_SetOpaque(local_obj, js->local_storage);
    JS_SetPropertyStr(js->ctx, global, "localStorage", local_obj);

    JSValue session_obj = JS_NewObjectClass(js->ctx, nd_storage_class_id);
    JS_SetOpaque(session_obj, js->session_storage);
    JS_SetPropertyStr(js->ctx, global, "sessionStorage", session_obj);

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

static JSValue
nd_document_addEventListener(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_active_js || !g_active_js->current_doc || argc < 2) return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    if (!JS_IsFunction(ctx, argv[1])) { JS_FreeCString(ctx, type); return JS_UNDEFINED; }
    nd_listener *l = g_new0(nd_listener, 1);
    l->target = g_active_js->current_doc;
    l->type   = g_strdup(type);
    l->cb     = JS_DupValue(ctx, argv[1]);
    g_ptr_array_add(g_active_js->listeners, l);
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

static JSValue
nd_document_removeEventListener(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_active_js || !g_active_js->current_doc || argc < 2) return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    for (guint i = 0; i < g_active_js->listeners->len; i++) {
        nd_listener *l = g_ptr_array_index(g_active_js->listeners, i);
        if (l->target == g_active_js->current_doc && strcmp(l->type, type) == 0 &&
            JS_VALUE_GET_TAG(l->cb) == JS_VALUE_GET_TAG(argv[1]) &&
            JS_VALUE_GET_PTR(l->cb) == JS_VALUE_GET_PTR(argv[1])) {
            JS_FreeValue(ctx, l->cb);
            g_free(l->type);
            g_free(l);
            g_ptr_array_remove_index_fast(g_active_js->listeners, i);
            break;
        }
    }
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
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
    JS_CFUNC_DEF("addEventListener",    2, nd_document_addEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, nd_document_removeEventListener),
};

static JSValue
nd_location_get_href(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!g_active_js) return JS_NewString(ctx, "");
    return JS_NewString(ctx, g_active_js->current_url ? g_active_js->current_url : "");
}

static JSValue
nd_location_set_href(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    (void)this_val;
    if (!g_active_js || !g_active_js->nav_cb) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (!s) return JS_UNDEFINED;
    g_active_js->nav_cb(s, FALSE, g_active_js->nav_user_data);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue
nd_location_assign(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_active_js || !g_active_js->nav_cb || argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_UNDEFINED;
    g_active_js->nav_cb(s, FALSE, g_active_js->nav_user_data);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue
nd_location_reload(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (g_active_js && g_active_js->nav_cb)
        g_active_js->nav_cb(g_active_js->current_url, TRUE, g_active_js->nav_user_data);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry nd_location_funcs[] = {
    JS_CGETSET_DEF("href", nd_location_get_href, nd_location_set_href),
    JS_CFUNC_DEF("assign", 1, nd_location_assign),
    JS_CFUNC_DEF("reload", 0, nd_location_reload),
};

static void
nd_js_install_document(nd_js *js, const nd_node *doc, const char *base_url)
{
    js->current_doc = doc;
    g_free(js->current_url);
    js->current_url = g_strdup(base_url ? base_url : "");

    nd_storage_load_for(js, js->current_url);

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
    JS_SetPropertyStr(ctx, document, "URL",    JS_NewString(ctx, js->current_url));
    JS_SetPropertyStr(ctx, document, "domain", JS_NewString(ctx, ""));
    JS_SetPropertyFunctionList(ctx, document, nd_document_funcs,
                               G_N_ELEMENTS(nd_document_funcs));
    JS_SetPropertyStr(ctx, global, "document", document);

    JSValue location = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, location, nd_location_funcs,
                               G_N_ELEMENTS(nd_location_funcs));
    JS_SetPropertyStr(ctx, global, "location", location);

    JS_FreeValue(ctx, global);
    g_free(title_str);
}

void
nd_js_free(nd_js *js)
{
    if (!js) return;
    nd_storage_flush(js);
    g_free(js->local_storage_origin);
    g_free(js->local_storage_path);
    g_free(js->current_url);
    if (js->timers) g_hash_table_destroy(js->timers);
    if (js->listeners) {
        for (guint i = 0; i < js->listeners->len; i++) {
            nd_listener *l = g_ptr_array_index(js->listeners, i);
            JS_FreeValue(js->ctx, l->cb);
            g_free(l->type);
            g_free(l);
        }
        g_ptr_array_free(js->listeners, TRUE);
    }
    if (js->orphan_nodes) {
        for (guint i = 0; i < js->orphan_nodes->len; i++)
            nd_node_free(g_ptr_array_index(js->orphan_nodes, i));
        g_ptr_array_free(js->orphan_nodes, TRUE);
    }
    if (js->local_storage)   g_hash_table_destroy(js->local_storage);
    if (js->session_storage) g_hash_table_destroy(js->session_storage);
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
    nd_drain_microtasks(js);
    g_active_js = NULL;
}

static char *
nd_js_resolve_url(const char *base, const char *href)
{
    if (!href || !*href) return NULL;
    if (g_str_has_prefix(href, "http://") || g_str_has_prefix(href, "https://"))
        return g_strdup(href);
    if (g_str_has_prefix(href, "//"))
        return g_strconcat("https:", href, NULL);
    if (!base || !*base) return NULL;
    const char *scheme_end = strstr(base, "://");
    if (!scheme_end) return NULL;
    const char *host_start = scheme_end + 3;
    const char *host_end = strchr(host_start, '/');
    gsize host_len = host_end ? (gsize)(host_end - base) : strlen(base);
    if (href[0] == '/') {
        char *root = g_strndup(base, host_len);
        char *r = g_strconcat(root, href, NULL);
        g_free(root);
        return r;
    }
    const char *q = strrchr(base, '/');
    if (q && q > scheme_end + 2) {
        gsize prefix_len = (gsize)(q - base) + 1;
        char *prefix = g_strndup(base, prefix_len);
        char *r = g_strconcat(prefix, href, NULL);
        g_free(prefix);
        return r;
    }
    return g_strconcat(base, "/", href, NULL);
}

#define ND_MAX_SCRIPT_BYTES (8u * 1024u * 1024u)

static void
nd_js_walk_scripts(nd_js *js, const nd_node *n, const char *origin)
{
    if (!n) return;
    if (n->kind == ND_NODE_ELEMENT && n->name && strcmp(n->name, "script") == 0) {
        const char *type = nd_element_get_attr(n, "type");
        gboolean ok_type = !type || !*type ||
                           g_ascii_strcasecmp(type, "text/javascript") == 0 ||
                           g_ascii_strcasecmp(type, "application/javascript") == 0 ||
                           g_ascii_strcasecmp(type, "module") == 0;
        if (!ok_type) return;
        const char *src = nd_element_get_attr(n, "src");
        if (src && *src) {
            char *abs = nd_js_resolve_url(origin, src);
            if (!abs) return;
            if (g_str_has_prefix(origin, "https://") && g_str_has_prefix(abs, "http://")) {
                if (js->log_cb) {
                    char *line = g_strdup_printf(
                        "mixed-content blocked: script %s on https page", abs);
                    js->log_cb(line, js->log_user_data);
                    g_free(line);
                }
                g_free(abs);
                return;
            }
            GError *err = NULL;
            nd_response *resp = nd_net_fetch_blocking(abs, NULL, &err);
            if (resp && resp->status == 200 && resp->body && resp->body->len > 0 &&
                resp->body->len <= ND_MAX_SCRIPT_BYTES) {
                nd_js_eval(js, (const char *)resp->body->data, resp->body->len, abs);
            } else if (js->log_cb) {
                const char *why = err ? err->message :
                    (resp && resp->error ? resp->error :
                     (resp ? "non-200 status" : "fetch failed"));
                char *line = g_strdup_printf("script %s: %s", abs, why);
                js->log_cb(line, js->log_user_data);
                g_free(line);
            }
            if (resp) nd_response_free(resp);
            if (err) g_error_free(err);
            g_free(abs);
            return;
        }
        for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
            if (c->kind == ND_NODE_TEXT && c->text)
                nd_js_eval(js, c->text, strlen(c->text), origin);
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
    nd_js_dispatch_event(js, doc, "DOMContentLoaded", NULL);
    nd_js_dispatch_event(js, doc, "load", NULL);
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
    nd_drain_microtasks(js);
    g_active_js = NULL;
    return out;
}
