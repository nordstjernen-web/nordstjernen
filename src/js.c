/* Nordstjernen — JavaScript engine binding (QuickJS). */

#include "js.h"

#include <string.h>

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <quickjs.h>

#include "config.h"
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
    nd_js_scroll_to_cb scroll_to_cb;
    gpointer      scroll_to_user_data;
    nd_js_form_submit_cb form_submit_cb;
    gpointer      form_submit_user_data;
    char         *current_url;
    nd_node       *current_doc;
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
    char         *cookie_value;
    char         *referrer;
    int           ready_state;
    gint64        eval_deadline_us;
};

static gint64
nd_js_eval_budget_us(void)
{
    const nd_config *c = nd_config_get();
    int ms = c ? c->js_eval_budget_ms : 5000;
    if (ms <= 0) ms = 5000;
    return (gint64)ms * 1000LL;
}

static int
nd_js_interrupt_cb(JSRuntime *rt, void *opaque)
{
    (void)rt;
    nd_js *js = opaque;
    if (!js || js->eval_deadline_us == 0) return 0;
    return g_get_monotonic_time() > js->eval_deadline_us ? 1 : 0;
}

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
nd_tlist_replace(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    nd_node *n = JS_GetOpaque(this_val, nd_token_list_class_id);
    if (!n || argc < 2) return JS_FALSE;
    const char *old_token = JS_ToCString(ctx, argv[0]);
    const char *new_token = JS_ToCString(ctx, argv[1]);
    JSValue result = JS_FALSE;
    if (old_token && new_token) {
        const char *cls = nd_element_get_attr(n, "class");
        if (class_attr_contains(cls, old_token, strlen(old_token), NULL, NULL)) {
            char *step1 = class_attr_remove(cls, old_token);
            char *step2 = class_attr_add(step1, new_token);
            nd_element_set_attr(n, "class", step2);
            g_free(step1); g_free(step2);
            if (g_active_js) g_active_js->mutated = TRUE;
            result = JS_TRUE;
        }
    }
    if (old_token) JS_FreeCString(ctx, old_token);
    if (new_token) JS_FreeCString(ctx, new_token);
    return result;
}

static JSValue
nd_tlist_get_length(JSContext *ctx, JSValueConst this_val)
{
    nd_node *n = JS_GetOpaque(this_val, nd_token_list_class_id);
    if (!n) return JS_NewInt32(ctx, 0);
    const char *cls = nd_element_get_attr(n, "class");
    if (!cls) return JS_NewInt32(ctx, 0);
    int count = 0;
    gboolean in_token = FALSE;
    for (const char *p = cls; *p; p++) {
        gboolean ws = (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r');
        if (!ws && !in_token) { count++; in_token = TRUE; }
        if (ws) in_token = FALSE;
    }
    return JS_NewInt32(ctx, count);
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

#define nd_origin_of nd_url_origin_from

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
        GError *err = NULL;
        if (!g_file_set_contents(js->local_storage_path, data, (gssize)len, &err)) {
            g_warning("local storage: failed to write %s: %s",
                      js->local_storage_path, err->message);
            g_clear_error(&err);
        }
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

static void nd_storage_maybe_dirty(GHashTable *store);

static int
nd_storage_get_own(JSContext *ctx, JSPropertyDescriptor *desc,
                   JSValueConst obj, JSAtom prop)
{
    GHashTable *store = JS_GetOpaque(obj, nd_storage_class_id);
    if (!store) return 0;
    const char *name = JS_AtomToCString(ctx, prop);
    if (!name) return 0;
    if (strcmp(name, "length") == 0 || strcmp(name, "constructor") == 0 ||
        strcmp(name, "getItem") == 0 || strcmp(name, "setItem") == 0 ||
        strcmp(name, "removeItem") == 0 || strcmp(name, "clear") == 0 ||
        strcmp(name, "key") == 0) {
        JS_FreeCString(ctx, name);
        return 0;
    }
    const char *val = g_hash_table_lookup(store, name);
    JS_FreeCString(ctx, name);
    if (!val) return 0;
    if (desc) {
        desc->flags  = JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE | JS_PROP_WRITABLE;
        desc->value  = JS_NewString(ctx, val);
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    }
    return 1;
}

static int
nd_storage_set_prop(JSContext *ctx, JSValueConst obj, JSAtom prop,
                    JSValueConst val, JSValueConst receiver, int flags)
{
    (void)receiver; (void)flags;
    GHashTable *store = JS_GetOpaque(obj, nd_storage_class_id);
    if (!store) return FALSE;
    const char *name = JS_AtomToCString(ctx, prop);
    if (!name) return FALSE;
    const char *vstr = JS_ToCString(ctx, val);
    if (vstr) {
        g_hash_table_replace(store, g_strdup(name), g_strdup(vstr));
        nd_storage_maybe_dirty(store);
        JS_FreeCString(ctx, vstr);
    }
    JS_FreeCString(ctx, name);
    return TRUE;
}

static int
nd_storage_delete(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    GHashTable *store = JS_GetOpaque(obj, nd_storage_class_id);
    if (!store) return FALSE;
    const char *name = JS_AtomToCString(ctx, prop);
    if (!name) return FALSE;
    gboolean removed = g_hash_table_remove(store, name);
    if (removed) nd_storage_maybe_dirty(store);
    JS_FreeCString(ctx, name);
    return TRUE;
}

static JSClassExoticMethods nd_storage_exotic = {
    .get_own_property = nd_storage_get_own,
    .set_property     = nd_storage_set_prop,
    .delete_property  = nd_storage_delete,
};

static JSClassDef nd_storage_class = {
    .class_name = "Storage",
    .finalizer  = nd_storage_finalizer,
    .exotic     = &nd_storage_exotic,
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
    JS_CFUNC_DEF("replace",  2, nd_tlist_replace),
    JS_CGETSET_DEF("length", nd_tlist_get_length, NULL),
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

static JSValue
nd_style_getPropertyValue(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    nd_node *n = JS_GetOpaque(this_val, nd_style_class_id);
    if (!n || argc < 1) return JS_NewString(ctx, "");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NewString(ctx, "");
    const char *style = nd_element_get_attr(n, "style");
    char *val = nd_inline_style_get(style, name);
    JS_FreeCString(ctx, name);
    JSValue ret = JS_NewString(ctx, val ? val : "");
    g_free(val);
    return ret;
}

static JSValue
nd_style_setProperty(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    nd_node *n = JS_GetOpaque(this_val, nd_style_class_id);
    if (!n || argc < 2) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (name) {
        const char *old = nd_element_get_attr(n, "style");
        char *new_style = nd_inline_style_set(old, name, value ? value : "");
        nd_element_set_attr(n, "style", new_style);
        g_free(new_style);
        if (g_active_js) g_active_js->mutated = TRUE;
    }
    if (name) JS_FreeCString(ctx, name);
    if (value) JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

static JSValue
nd_style_removeProperty(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    nd_node *n = JS_GetOpaque(this_val, nd_style_class_id);
    if (!n || argc < 1) return JS_NewString(ctx, "");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NewString(ctx, "");
    const char *style = nd_element_get_attr(n, "style");
    char *old_val = nd_inline_style_get(style, name);
    char *new_style = nd_inline_style_set(style, name, "");
    nd_element_set_attr(n, "style", new_style);
    g_free(new_style);
    JS_FreeCString(ctx, name);
    if (g_active_js) g_active_js->mutated = TRUE;
    JSValue ret = JS_NewString(ctx, old_val ? old_val : "");
    g_free(old_val);
    return ret;
}

static JSValue
nd_style_get_zero(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewInt32(ctx, 0);
}

static JSValue
nd_style_get_null(JSContext *ctx, JSValueConst this_val)
{
    (void)ctx; (void)this_val;
    return JS_NULL;
}

static JSValue
nd_style_get_empty(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewString(ctx, "");
}

static JSValue
nd_style_item(JSContext *ctx, JSValueConst this_val,
              int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewString(ctx, "");
}

static JSValue
nd_style_getPropertyPriority(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewString(ctx, "");
}

static const JSCFunctionListEntry nd_style_proto_funcs[] = {
    JS_CGETSET_DEF("cssText", nd_style_get_cssText, nd_style_set_cssText),
    JS_CGETSET_DEF("length",      nd_style_get_zero,  NULL),
    JS_CGETSET_DEF("parentRule",  nd_style_get_null,  NULL),
    JS_CGETSET_DEF("cssFloat",    nd_style_get_empty, NULL),
    JS_CFUNC_DEF("getPropertyValue",   1, nd_style_getPropertyValue),
    JS_CFUNC_DEF("setProperty",        2, nd_style_setProperty),
    JS_CFUNC_DEF("removeProperty",     1, nd_style_removeProperty),
    JS_CFUNC_DEF("item",               1, nd_style_item),
    JS_CFUNC_DEF("getPropertyPriority", 1, nd_style_getPropertyPriority),
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

static nd_node *
nd_unwrap_element_mut(JSValueConst val)
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
nd_element_get_localName(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n || !n->name) return JS_NULL;
    return JS_NewString(ctx, n->name);
}

static JSValue
nd_element_attr_getter(JSContext *ctx, JSValueConst this_val, int magic)
{
    static const char *names[] = {
        "title", "name", "alt", "src", "href", "type", "placeholder", "lang", "dir",
        "action", "method", "enctype", "target", "rel", "accept", "accept-charset",
        "autocomplete", "list", "min", "max", "step", "pattern", "spellcheck",
        "crossorigin", "referrerpolicy", "decoding", "loading", "fetchpriority",
        "sizes", "srcset", "usemap", "inputmode", "size", "cols", "rows",
        "maxlength", "minlength", "coords", "shape",
        "formaction", "formmethod", "formenctype", "formtarget",
        "integrity", "kind", "label", "hreflang", "charset", "content",
        "http-equiv", "contenteditable", "slot", "is", "role",
        "aria-label", "aria-hidden", "aria-disabled", "aria-pressed",
        "aria-expanded", "aria-controls", "aria-describedby",
        "aria-labelledby", "aria-live", "aria-busy", "aria-checked",
        "aria-current", "aria-selected", "aria-readonly", "aria-required",
        "aria-valuenow", "aria-valuemin", "aria-valuemax",
    };
    if (magic < 0 || magic >= (int)G_N_ELEMENTS(names))
        return JS_NewString(ctx, "");
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_NewString(ctx, "");
    const char *v = nd_element_get_attr(n, names[magic]);
    return JS_NewString(ctx, v ? v : "");
}

static JSValue
nd_element_attr_setter(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    static const char *names[] = {
        "title", "name", "alt", "src", "href", "type", "placeholder", "lang", "dir",
        "action", "method", "enctype", "target", "rel", "accept", "accept-charset",
        "autocomplete", "list", "min", "max", "step", "pattern", "spellcheck",
        "crossorigin", "referrerpolicy", "decoding", "loading", "fetchpriority",
        "sizes", "srcset", "usemap", "inputmode", "size", "cols", "rows",
        "maxlength", "minlength", "coords", "shape",
        "formaction", "formmethod", "formenctype", "formtarget",
        "integrity", "kind", "label", "hreflang", "charset", "content",
        "http-equiv", "contenteditable", "slot", "is", "role",
        "aria-label", "aria-hidden", "aria-disabled", "aria-pressed",
        "aria-expanded", "aria-controls", "aria-describedby",
        "aria-labelledby", "aria-live", "aria-busy", "aria-checked",
        "aria-current", "aria-selected", "aria-readonly", "aria-required",
        "aria-valuenow", "aria-valuemin", "aria-valuemax",
    };
    if (magic < 0 || magic >= (int)G_N_ELEMENTS(names))
        return JS_UNDEFINED;
    nd_node *n = nd_unwrap_element_mut(this_val);
    if (!n) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (s) {
        nd_element_set_attr(n, names[magic], s);
        if (g_active_js) g_active_js->mutated = TRUE;
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

static JSValue
nd_element_get_tabIndex(JSContext *ctx, JSValueConst this_val)
{
    (void)ctx;
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_NewInt32(ctx, -1);
    const char *v = nd_element_get_attr(n, "tabindex");
    if (!v) {
        if (n->name &&
            (g_ascii_strcasecmp(n->name, "a") == 0 ||
             g_ascii_strcasecmp(n->name, "area") == 0 ||
             g_ascii_strcasecmp(n->name, "button") == 0 ||
             g_ascii_strcasecmp(n->name, "input") == 0 ||
             g_ascii_strcasecmp(n->name, "select") == 0 ||
             g_ascii_strcasecmp(n->name, "textarea") == 0))
            return JS_NewInt32(ctx, 0);
        return JS_NewInt32(ctx, -1);
    }
    return JS_NewInt32(ctx, atoi(v));
}

static JSValue
nd_element_set_tabIndex(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    nd_node *n = nd_unwrap_element_mut(this_val);
    if (!n) return JS_UNDEFINED;
    int32_t iv = 0;
    if (JS_ToInt32(ctx, &iv, val) == 0) {
        char buf[16];
        g_snprintf(buf, sizeof buf, "%d", iv);
        nd_element_set_attr(n, "tabindex", buf);
        if (g_active_js) g_active_js->mutated = TRUE;
    }
    return JS_UNDEFINED;
}

static JSValue
nd_element_get_htmlFor(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_NewString(ctx, "");
    const char *v = nd_element_get_attr(n, "for");
    return JS_NewString(ctx, v ? v : "");
}

static JSValue
nd_element_set_htmlFor(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    nd_node *n = nd_unwrap_element_mut(this_val);
    if (!n) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (s) {
        nd_element_set_attr(n, "for", s);
        if (g_active_js) g_active_js->mutated = TRUE;
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

static const char *kBoolAttrs[] = {
    "open", "selected", "multiple", "readonly", "autofocus",
    "controls", "loop", "muted", "autoplay", "defer", "async",
    "novalidate",
    "ismap", "draggable", "reversed", "playsinline",
    "default", "inert", "nomodule", "formnovalidate",
};

static JSValue
nd_element_boolattr_getter(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)ctx;
    if (magic < 0 || magic >= (int)G_N_ELEMENTS(kBoolAttrs)) return JS_FALSE;
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_FALSE;
    return nd_element_get_attr(n, kBoolAttrs[magic]) ? JS_TRUE : JS_FALSE;
}

static JSValue
nd_element_boolattr_setter(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    if (magic < 0 || magic >= (int)G_N_ELEMENTS(kBoolAttrs)) return JS_UNDEFINED;
    nd_node *n = nd_unwrap_element_mut(this_val);
    if (!n) return JS_UNDEFINED;
    if (JS_ToBool(ctx, val)) nd_element_set_attr(n, kBoolAttrs[magic], "");
    else                     nd_element_remove_attr(n, kBoolAttrs[magic]);
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
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

static JSValue nd_element_get_select_length(JSContext *ctx, JSValueConst this_val);
static JSValue nd_element_get_form_elements(JSContext *ctx, JSValueConst this_val);

static JSValue
nd_element_get_text_length(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_NewInt32(ctx, 0);
    if (n->kind == ND_NODE_TEXT || n->kind == ND_NODE_COMMENT)
        return JS_NewInt32(ctx, n->text ? (int)g_utf8_strlen(n->text, -1) : 0);
    if (n->name && g_ascii_strcasecmp(n->name, "select") == 0)
        return nd_element_get_select_length(ctx, this_val);
    if (n->name && g_ascii_strcasecmp(n->name, "form") == 0) {
        JSValue arr = nd_element_get_form_elements(ctx, this_val);
        JSValue len_v = JS_GetPropertyStr(ctx, arr, "length");
        JS_FreeValue(ctx, arr);
        return len_v;
    }
    return JS_NewInt32(ctx, 0);
}

static JSValue
nd_element_substring_data(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n || !n->text || argc < 1) return JS_NewString(ctx, "");
    int32_t off = 0, cnt = 0;
    JS_ToInt32(ctx, &off, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &cnt, argv[1]);
    else cnt = (int32_t)strlen(n->text);
    glong total = g_utf8_strlen(n->text, -1);
    if (off < 0) off = 0;
    if (off > total) off = (int32_t)total;
    if (cnt < 0) cnt = 0;
    if (off + cnt > total) cnt = (int32_t)total - off;
    const char *start = g_utf8_offset_to_pointer(n->text, off);
    const char *end   = g_utf8_offset_to_pointer(start, cnt);
    return JS_NewStringLen(ctx, start, (gsize)(end - start));
}

static JSValue
nd_element_append_data(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    nd_node *n = nd_unwrap_element_mut(this_val);
    if (!n || argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_UNDEFINED;
    char *merged = g_strconcat(n->text ? n->text : "", s, NULL);
    g_free(n->text);
    n->text = merged;
    JS_FreeCString(ctx, s);
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_element_delete_data(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    nd_node *n = nd_unwrap_element_mut(this_val);
    if (!n || !n->text || argc < 2) return JS_UNDEFINED;
    int32_t off = 0, cnt = 0;
    JS_ToInt32(ctx, &off, argv[0]);
    JS_ToInt32(ctx, &cnt, argv[1]);
    glong total = g_utf8_strlen(n->text, -1);
    if (off < 0) off = 0;
    if (off > total) off = (int32_t)total;
    if (cnt < 0) cnt = 0;
    if (off + cnt > total) cnt = (int32_t)total - off;
    const char *start = g_utf8_offset_to_pointer(n->text, off);
    const char *end   = g_utf8_offset_to_pointer(start, cnt);
    gsize head = (gsize)(start - n->text);
    gsize tail = strlen(end);
    char *merged = g_malloc(head + tail + 1);
    memcpy(merged, n->text, head);
    memcpy(merged + head, end, tail);
    merged[head + tail] = '\0';
    g_free(n->text);
    n->text = merged;
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_element_insert_data(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    nd_node *n = nd_unwrap_element_mut(this_val);
    if (!n || argc < 2) return JS_UNDEFINED;
    int32_t off = 0;
    JS_ToInt32(ctx, &off, argv[0]);
    const char *ins = JS_ToCString(ctx, argv[1]);
    if (!ins) return JS_UNDEFINED;
    glong total = n->text ? g_utf8_strlen(n->text, -1) : 0;
    if (off < 0) off = 0;
    if (off > total) off = (int32_t)total;
    const char *p = n->text ? g_utf8_offset_to_pointer(n->text, off) : "";
    gsize head = n->text ? (gsize)(p - n->text) : 0;
    gsize tail = n->text ? strlen(p) : 0;
    gsize ilen = strlen(ins);
    char *merged = g_malloc(head + ilen + tail + 1);
    if (head) memcpy(merged, n->text, head);
    memcpy(merged + head, ins, ilen);
    if (tail) memcpy(merged + head + ilen, p, tail);
    merged[head + ilen + tail] = '\0';
    g_free(n->text);
    n->text = merged;
    JS_FreeCString(ctx, ins);
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_element_replace_data(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    if (argc < 3) return JS_UNDEFINED;
    JSValueConst del_args[2] = { argv[0], argv[1] };
    nd_element_delete_data(ctx, this_val, 2, del_args);
    JSValueConst ins_args[2] = { argv[0], argv[2] };
    nd_element_insert_data(ctx, this_val, 2, ins_args);
    return JS_UNDEFINED;
}

static JSValue
nd_element_split_text(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    nd_node *n = nd_unwrap_element_mut(this_val);
    if (!n || n->kind != ND_NODE_TEXT || !n->text || argc < 1) return JS_NULL;
    int32_t off = 0;
    JS_ToInt32(ctx, &off, argv[0]);
    glong total = g_utf8_strlen(n->text, -1);
    if (off < 0) off = 0;
    if (off > total) off = (int32_t)total;
    const char *split = g_utf8_offset_to_pointer(n->text, off);
    char *tail_text = g_strdup(split);
    gsize head_len = (gsize)(split - n->text);
    char *head = g_strndup(n->text, head_len);
    g_free(n->text);
    n->text = head;
    nd_node *tail = nd_node_new_text(tail_text);
    if (n->parent) {
        tail->parent = n->parent;
        tail->prev_sibling = n;
        tail->next_sibling = n->next_sibling;
        if (n->next_sibling) n->next_sibling->prev_sibling = tail;
        else n->parent->last_child = tail;
        n->next_sibling = tail;
    } else if (g_active_js) {
        g_ptr_array_add(g_active_js->orphan_nodes, tail);
    }
    if (g_active_js) g_active_js->mutated = TRUE;
    return nd_make_element(ctx, tail);
}

static JSValue
nd_element_get_nodeValue(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_NULL;
    if (n->kind == ND_NODE_TEXT || n->kind == ND_NODE_COMMENT)
        return JS_NewString(ctx, n->text ? n->text : "");
    return JS_NULL;
}

static JSValue
nd_element_set_nodeValue(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    nd_node *n = nd_unwrap_element_mut(this_val);
    if (!n) return JS_UNDEFINED;
    if (n->kind != ND_NODE_TEXT && n->kind != ND_NODE_COMMENT) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (s) {
        g_free(n->text);
        n->text = g_strdup(s);
        JS_FreeCString(ctx, s);
        if (g_active_js) g_active_js->mutated = TRUE;
    }
    return JS_UNDEFINED;
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
nd_element_set_id(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    nd_node *n = nd_unwrap_element_mut(this_val);
    if (!n) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (s) {
        nd_element_set_attr(n, "id", s);
        if (g_active_js) g_active_js->mutated = TRUE;
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
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
nd_element_set_className(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    nd_node *n = nd_unwrap_element_mut(this_val);
    if (!n) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (s) {
        nd_element_set_attr(n, "class", s);
        if (g_active_js) g_active_js->mutated = TRUE;
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

static JSValue
nd_element_get_style(JSContext *ctx, JSValueConst this_val)
{
    nd_node *n = nd_unwrap_element_mut(this_val);
    if (!n) return JS_NULL;
    JSValue obj = JS_NewObjectClass(ctx, nd_style_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, n);
    return obj;
}

static JSValue
nd_element_get_classList(JSContext *ctx, JSValueConst this_val)
{
    nd_node *n = nd_unwrap_element_mut(this_val);
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
    nd_node *n = nd_unwrap_element_mut(this_val);
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
    nd_node *n = nd_unwrap_element_mut(this_val);
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

static void nd_insert_sibling_before(nd_node *ref, nd_node *newc);

static JSValue
nd_element_set_outerHTML(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    nd_node *self = nd_unwrap_element_mut(this_val);
    if (!self || !self->parent) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (!s) return JS_UNDEFINED;
    nd_node *fragment = nd_html_parse(s, -1);
    JS_FreeCString(ctx, s);
    if (fragment) {
        nd_node *anchor = self;
        GPtrArray *kids = g_ptr_array_new();
        for (nd_node *c = fragment->first_child; c; c = c->next_sibling)
            g_ptr_array_add(kids, c);
        for (guint i = 0; i < kids->len; i++) {
            nd_node *c = kids->pdata[i];
            nd_node_remove(c);
            nd_insert_sibling_before(anchor, c);
        }
        g_ptr_array_free(kids, TRUE);
        nd_node_free(fragment);
        nd_node *parent = self->parent;
        nd_node_remove(self);
        if (g_active_js) {
            g_ptr_array_add(g_active_js->orphan_nodes, self);
            g_active_js->mutated = TRUE;
        }
        (void)parent;
    }
    return JS_UNDEFINED;
}

static JSValue
nd_element_replaceChildren(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    nd_node *self = nd_unwrap_element_mut(this_val);
    if (!self) return JS_UNDEFINED;
    nd_element_clear_children(self);
    for (int i = 0; i < argc; i++) {
        nd_node *child = nd_unwrap_element_mut(argv[i]);
        if (child) {
            if (g_active_js)
                g_ptr_array_remove_fast(g_active_js->orphan_nodes, child);
            nd_node_append_child(self, child);
        } else {
            const char *txt = JS_ToCString(ctx, argv[i]);
            if (txt) {
                nd_node_append_child(self, nd_node_new_text(g_strdup(txt)));
                JS_FreeCString(ctx, txt);
            }
        }
    }
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
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

static gboolean
cors_allows(const char *doc_url, const char *resp_url, const char *cors_header)
{
    if (nd_url_same_origin(doc_url, resp_url)) return TRUE;
    if (!cors_header || !*cors_header) return FALSE;
    char *trimmed = g_strdup(cors_header);
    g_strstrip(trimmed);
    char *doc_origin = nd_url_origin_from(doc_url);
    gboolean ok = strcmp(trimmed, "*") == 0 ||
                  (doc_origin && g_ascii_strcasecmp(trimmed, doc_origin) == 0);
    g_free(trimmed);
    g_free(doc_origin);
    return ok;
}

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
        gboolean allow = cors_allows(st->js->current_url, resp->final_url,
                                     resp->cors_allow_origin);
        JSValue r = JS_NewObject(st->ctx);
        JS_SetPropertyStr(st->ctx, r, "ok",
            JS_NewBool(st->ctx, allow && resp->status >= 200 && resp->status < 300));
        JS_SetPropertyStr(st->ctx, r, "status",
            JS_NewInt32(st->ctx, allow ? (int)resp->status : 0));
        JS_SetPropertyStr(st->ctx, r, "statusText", JS_NewString(st->ctx, ""));
        JS_SetPropertyStr(st->ctx, r, "url",
            JS_NewString(st->ctx, resp->final_url ? resp->final_url : ""));
        JS_SetPropertyStr(st->ctx, r, "type",
            JS_NewString(st->ctx, allow ? "basic" : "opaque"));
        char *body_text = NULL;
        if (allow && resp->body && resp->body->len > 0)
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
    char *method = NULL;
    char *body = NULL;
    char *content_type = NULL;
    gsize body_len = 0;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue m = JS_GetPropertyStr(ctx, argv[1], "method");
        if (JS_IsString(m)) {
            const char *s = JS_ToCString(ctx, m);
            if (s) { method = g_ascii_strup(s, -1); JS_FreeCString(ctx, s); }
        }
        JS_FreeValue(ctx, m);
        JSValue b = JS_GetPropertyStr(ctx, argv[1], "body");
        if (JS_IsString(b)) {
            const char *s = JS_ToCString(ctx, b);
            if (s) { body = g_strdup(s); body_len = strlen(s); JS_FreeCString(ctx, s); }
        }
        JS_FreeValue(ctx, b);
        JSValue h = JS_GetPropertyStr(ctx, argv[1], "headers");
        if (JS_IsObject(h)) {
            JSValue ct = JS_GetPropertyStr(ctx, h, "Content-Type");
            if (!JS_IsString(ct)) {
                JS_FreeValue(ctx, ct);
                ct = JS_GetPropertyStr(ctx, h, "content-type");
            }
            if (JS_IsString(ct)) {
                const char *s = JS_ToCString(ctx, ct);
                if (s) { content_type = g_strdup(s); JS_FreeCString(ctx, s); }
            }
            JS_FreeValue(ctx, ct);
        }
        JS_FreeValue(ctx, h);
    }
    nd_js_fetch_state *st = g_new0(nd_js_fetch_state, 1);
    st->ctx = ctx;
    st->js = g_active_js;
    st->resolve = resolving[0];
    st->reject  = resolving[1];
    if (method && g_ascii_strcasecmp(method, "POST") == 0) {
        nd_net_post_async(url, body, body_len,
                          content_type ? content_type : "text/plain",
                          NULL, nd_on_js_fetch_done, st);
    } else {
        nd_net_fetch_async(url, NULL, nd_on_js_fetch_done, st);
    }
    g_free(method); g_free(body); g_free(content_type);
    JS_FreeCString(ctx, url);
    return promise;
}

static JSValue
nd_event_noop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static void
nd_bind_fn(JSContext *ctx, JSValueConst obj, const char *name,
           JSCFunction *fn, int argc)
{
    JS_SetPropertyStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, argc));
}

typedef struct nd_fn_def { const char *name; int argc; } nd_fn_def;

static void
nd_bind_fns(JSContext *ctx, JSValueConst obj, JSCFunction *fn,
            const nd_fn_def *defs, gsize n)
{
    for (gsize i = 0; i < n; i++)
        nd_bind_fn(ctx, obj, defs[i].name, fn, defs[i].argc);
}

static JSValue
nd_event_empty_array(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewArray(ctx);
}

static void nd_js_emit(nd_js *js, const char *prefix, JSContext *ctx,
                       int argc, JSValueConst *argv);

static JSValue
nd_throws_unsupported(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "not supported by Nordstjernen");
}

static JSValue
nd_returns_resolved_undefined(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue resolvers[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolvers);
    JSValue undef = JS_UNDEFINED;
    JS_Call(ctx, resolvers[0], JS_UNDEFINED, 1, &undef);
    JS_FreeValue(ctx, resolvers[0]);
    JS_FreeValue(ctx, resolvers[1]);
    return promise;
}

static JSValue
nd_returns_rejected(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue resolvers[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolvers);
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, "not supported"));
    JS_Call(ctx, resolvers[1], JS_UNDEFINED, 1, &err);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, resolvers[0]);
    JS_FreeValue(ctx, resolvers[1]);
    return promise;
}

static JSValue
nd_window_message_channel(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    static const nd_fn_def port_methods[] = {
        { "postMessage", 1 }, { "start", 0 }, { "close", 0 },
        { "addEventListener", 2 }, { "removeEventListener", 2 },
    };
    JSValue mc = JS_NewObject(ctx);
    for (int i = 0; i < 2; i++) {
        JSValue port = JS_NewObject(ctx);
        nd_bind_fns(ctx, port, nd_event_noop, port_methods, G_N_ELEMENTS(port_methods));
        JS_SetPropertyStr(ctx, mc, i == 0 ? "port1" : "port2", port);
    }
    return mc;
}

static JSValue
nd_window_broadcast_channel(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue bc = JS_NewObject(ctx);
    if (argc >= 1) {
        const char *name = JS_ToCString(ctx, argv[0]);
        JS_SetPropertyStr(ctx, bc, "name", JS_NewString(ctx, name ? name : ""));
        if (name) JS_FreeCString(ctx, name);
    } else {
        JS_SetPropertyStr(ctx, bc, "name", JS_NewString(ctx, ""));
    }
    static const nd_fn_def bc_methods[] = {
        { "postMessage", 1 }, { "close", 0 },
        { "addEventListener", 2 }, { "removeEventListener", 2 },
    };
    nd_bind_fns(ctx, bc, nd_event_noop, bc_methods, G_N_ELEMENTS(bc_methods));
    JS_SetPropertyStr(ctx, bc, "onmessage", JS_NULL);
    return bc;
}

static JSValue
nd_window_notification(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue n = JS_NewObject(ctx);
    nd_bind_fn(ctx, n, "close", nd_event_noop, 0);
    return n;
}

static JSValue
nd_window_report_error(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    (void)this_val;
    nd_js_emit(g_active_js, "[error]", ctx, argc, argv);
    return JS_UNDEFINED;
}

static JSValue
nd_window_structured_clone(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    JSValue json = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json)) return JS_DupValue(ctx, argv[0]);
    const char *s = JS_ToCString(ctx, json);
    JSValue out = s ? JS_ParseJSON(ctx, s, strlen(s), "<structuredClone>")
                    : JS_DupValue(ctx, argv[0]);
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, json);
    return out;
}

static JSValue
nd_css_supports(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argv;
    if (argc < 1) return JS_FALSE;
    return JS_TRUE;
}

static JSValue
nd_css_escape(JSContext *ctx, JSValueConst this_val,
              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_NewString(ctx, "");
    GString *out = g_string_new(NULL);
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c >= 0x80) {
            g_string_append_c(out, c);
        } else {
            g_string_append_printf(out, "\\%c", c);
        }
    }
    JS_FreeCString(ctx, s);
    JSValue v = JS_NewString(ctx, out->str);
    g_string_free(out, TRUE);
    return v;
}

static JSValue
nd_window_get_selection(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    static const nd_fn_def sel_methods[] = {
        { "toString", 0 }, { "removeAllRanges", 0 }, { "addRange", 1 },
        { "collapse", 2 }, { "collapseToStart", 0 }, { "collapseToEnd", 0 },
        { "getRangeAt", 1 }, { "empty", 0 },
    };
    JSValue sel = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, sel, "anchorNode",   JS_NULL);
    JS_SetPropertyStr(ctx, sel, "focusNode",    JS_NULL);
    JS_SetPropertyStr(ctx, sel, "anchorOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, sel, "focusOffset",  JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, sel, "isCollapsed",  JS_TRUE);
    JS_SetPropertyStr(ctx, sel, "rangeCount",   JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, sel, "type",         JS_NewString(ctx, "None"));
    nd_bind_fns(ctx, sel, nd_event_noop, sel_methods, G_N_ELEMENTS(sel_methods));
    return sel;
}

static JSValue
nd_event_true(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_TRUE;
}

static JSValue nd_document_addEventListener(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv);
static JSValue nd_document_removeEventListener(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv);

static JSValue
nd_window_matchMedia(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    (void)this_val;
    const char *q = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    JSValue mql = JS_NewObject(ctx);
    gboolean matches = nd_css_media_query_matches(q);
    static const nd_fn_def mql_methods[] = {
        { "addListener", 1 }, { "removeListener", 1 },
        { "addEventListener", 2 }, { "removeEventListener", 2 },
    };
    JS_SetPropertyStr(ctx, mql, "matches", matches ? JS_TRUE : JS_FALSE);
    JS_SetPropertyStr(ctx, mql, "media", JS_NewString(ctx, q ? q : ""));
    nd_bind_fns(ctx, mql, nd_event_noop, mql_methods, G_N_ELEMENTS(mql_methods));
    if (q) JS_FreeCString(ctx, q);
    return mql;
}

static JSValue
nd_computed_getPropertyValue(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NewString(ctx, "");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NewString(ctx, "");
    JSValue node_v = JS_GetPropertyStr(ctx, this_val, "_node");
    const nd_node *n = nd_unwrap_element(node_v);
    JS_FreeValue(ctx, node_v);
    if (!n) { JS_FreeCString(ctx, name); return JS_NewString(ctx, ""); }
    const char *style = nd_element_get_attr(n, "style");
    char *val = nd_inline_style_get(style, name);
    JS_FreeCString(ctx, name);
    JSValue r = JS_NewString(ctx, val ? val : "");
    g_free(val);
    return r;
}

static JSValue
nd_window_getComputedStyle(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue cs = JS_NewObject(ctx);
    if (argc >= 1) JS_SetPropertyStr(ctx, cs, "_node", JS_DupValue(ctx, argv[0]));
    nd_bind_fn(ctx, cs, "getPropertyValue", nd_computed_getPropertyValue, 1);
    return cs;
}

static JSValue
nd_window_getRandomValues(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    JSValue arr = argv[0];
    JSValue len_v = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_v);
    JS_FreeValue(ctx, len_v);
    for (int32_t i = 0; i < len; i++) {
        guint32 r = g_random_int();
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewInt32(ctx, (int32_t)(r & 0xff)));
    }
    return JS_DupValue(ctx, arr);
}

static JSValue
nd_window_randomUUID(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    guint32 r[4];
    for (int i = 0; i < 4; i++) r[i] = g_random_int();
    r[1] = (r[1] & 0xffff0fff) | 0x00004000;
    r[2] = (r[2] & 0x3fffffff) | 0x80000000;
    char buf[37];
    g_snprintf(buf, sizeof(buf),
               "%08x-%04x-%04x-%04x-%04x%08x",
               r[0],
               (r[1] >> 16) & 0xffff,
               r[1] & 0xffff,
               (r[2] >> 16) & 0xffff,
               r[2] & 0xffff,
               r[3]);
    return JS_NewString(ctx, buf);
}

static JSValue
nd_window_open_method(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !g_active_js || !g_active_js->nav_cb) return JS_NULL;
    const char *url = JS_ToCString(ctx, argv[0]);
    if (url) {
        g_active_js->nav_cb(url, FALSE, g_active_js->nav_user_data);
        JS_FreeCString(ctx, url);
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue
nd_window_confirm(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_TRUE;
}

static JSValue
nd_window_prompt(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc >= 2) return JS_DupValue(ctx, argv[1]);
    return JS_NewString(ctx, "");
}

static JSValue
nd_window_performance_now(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, (double)g_get_monotonic_time() / 1000.0);
}

static JSValue nd_event_prevent_default(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv);
static JSValue nd_event_stop_propagation(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);

static char *nd_js_resolve_url(const char *base, const char *href);

static JSValue
nd_window_btoa(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_NewString(ctx, "");
    gchar *b64 = g_base64_encode((const guchar *)s, strlen(s));
    JS_FreeCString(ctx, s);
    JSValue r = JS_NewString(ctx, b64);
    g_free(b64);
    return r;
}

static JSValue
nd_window_atob(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_NewString(ctx, "");
    gsize out_len = 0;
    guchar *out = g_base64_decode(s, &out_len);
    JS_FreeCString(ctx, s);
    JSValue r = JS_NewStringLen(ctx, (const char *)out, out_len);
    g_free(out);
    return r;
}

static JSValue
nd_url_get_searchParams_object(JSContext *ctx, const char *search);

static JSValue
nd_window_url_ctor(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_NULL;
    char *resolved = NULL;
    if (argc >= 2) {
        const char *base = JS_ToCString(ctx, argv[1]);
        if (base) {
            resolved = nd_js_resolve_url(base, raw);
            JS_FreeCString(ctx, base);
        }
    }
    if (!resolved) resolved = g_strdup(raw);
    JS_FreeCString(ctx, raw);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "href", JS_NewString(ctx, resolved));
    const char *scheme_end = strstr(resolved, ":");
    if (scheme_end) {
        char *proto = g_strndup(resolved, (gsize)(scheme_end - resolved + 1));
        JS_SetPropertyStr(ctx, obj, "protocol", JS_NewString(ctx, proto));
        g_free(proto);
    } else {
        JS_SetPropertyStr(ctx, obj, "protocol", JS_NewString(ctx, ""));
    }
    const char *p = strstr(resolved, "://");
    const char *host_start = p ? p + 3 : resolved;
    const char *path_start = host_start;
    while (*path_start && *path_start != '/' && *path_start != '?' && *path_start != '#')
        path_start++;
    char *host = g_strndup(host_start, (gsize)(path_start - host_start));
    JS_SetPropertyStr(ctx, obj, "host",     JS_NewString(ctx, host));
    JS_SetPropertyStr(ctx, obj, "hostname", JS_NewString(ctx, host));
    g_free(host);
    char *origin = g_strndup(resolved, (gsize)(path_start - resolved));
    JS_SetPropertyStr(ctx, obj, "origin",   JS_NewString(ctx, origin));
    g_free(origin);
    const char *path_end = path_start;
    while (*path_end && *path_end != '?' && *path_end != '#') path_end++;
    char *path = g_strndup(path_start, (gsize)(path_end - path_start));
    JS_SetPropertyStr(ctx, obj, "pathname",
                      JS_NewString(ctx, *path ? path : "/"));
    g_free(path);
    const char *search_end = path_end;
    if (*path_end == '?') {
        while (*search_end && *search_end != '#') search_end++;
        char *search = g_strndup(path_end, (gsize)(search_end - path_end));
        JS_SetPropertyStr(ctx, obj, "search", JS_NewString(ctx, search));
        JS_SetPropertyStr(ctx, obj, "searchParams",
                          nd_url_get_searchParams_object(ctx, search + 1));
        g_free(search);
    } else {
        JS_SetPropertyStr(ctx, obj, "search", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, obj, "searchParams",
                          nd_url_get_searchParams_object(ctx, ""));
    }
    JS_SetPropertyStr(ctx, obj, "hash",
                      JS_NewString(ctx, *search_end == '#' ? search_end : ""));
    g_free(resolved);
    return obj;
}

static JSValue
nd_url_get_searchParams_object(JSContext *ctx, const char *search)
{
    JSValue obj = JS_NewObject(ctx);
    GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, g_free);
    if (search && *search) {
        char **pairs = g_strsplit(search, "&", -1);
        for (int i = 0; pairs[i]; i++) {
            char *eq = strchr(pairs[i], '=');
            char *key, *value;
            if (eq) {
                *eq = '\0';
                key = g_uri_unescape_string(pairs[i], NULL);
                value = g_uri_unescape_string(eq + 1, NULL);
            } else {
                key = g_uri_unescape_string(pairs[i], NULL);
                value = g_strdup("");
            }
            if (key) g_hash_table_replace(table, key, value ? value : g_strdup(""));
            else g_free(value);
        }
        g_strfreev(pairs);
    }
    nd_bind_fn(ctx, obj, "toString", nd_event_noop, 0);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, table);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        JS_SetPropertyStr(ctx, obj, (const char *)k,
                          JS_NewString(ctx, (const char *)v));
    }
    g_hash_table_destroy(table);
    return obj;
}

static JSValue
nd_window_image_ctor(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_active_js) return JS_NULL;
    nd_node *el = nd_node_new_element(g_strdup("img"));
    if (argc >= 1) {
        int32_t w = 0;
        if (JS_ToInt32(ctx, &w, argv[0]) == 0 && w > 0) {
            char buf[16];
            g_snprintf(buf, sizeof buf, "%d", w);
            nd_element_set_attr(el, "width", buf);
        }
    }
    if (argc >= 2) {
        int32_t h = 0;
        if (JS_ToInt32(ctx, &h, argv[1]) == 0 && h > 0) {
            char buf[16];
            g_snprintf(buf, sizeof buf, "%d", h);
            nd_element_set_attr(el, "height", buf);
        }
    }
    g_ptr_array_add(g_active_js->orphan_nodes, el);
    return nd_make_element(ctx, el);
}

static JSValue
nd_window_audio_ctor(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_active_js) return JS_NULL;
    nd_node *el = nd_node_new_element(g_strdup("audio"));
    if (argc >= 1) {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s) { nd_element_set_attr(el, "src", s); JS_FreeCString(ctx, s); }
    }
    g_ptr_array_add(g_active_js->orphan_nodes, el);
    return nd_make_element(ctx, el);
}

static JSValue
nd_window_option_ctor(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_active_js) return JS_NULL;
    nd_node *el = nd_node_new_element(g_strdup("option"));
    if (argc >= 1) {
        const char *t = JS_ToCString(ctx, argv[0]);
        if (t) {
            nd_node_append_child(el, nd_node_new_text(g_strdup(t)));
            JS_FreeCString(ctx, t);
        }
    }
    if (argc >= 2) {
        const char *v = JS_ToCString(ctx, argv[1]);
        if (v) { nd_element_set_attr(el, "value", v); JS_FreeCString(ctx, v); }
    }
    if (argc >= 3 && JS_ToBool(ctx, argv[2]))
        nd_element_set_attr(el, "defaultSelected", "");
    if (argc >= 4 && JS_ToBool(ctx, argv[3]))
        nd_element_set_attr(el, "selected", "");
    g_ptr_array_add(g_active_js->orphan_nodes, el);
    return nd_make_element(ctx, el);
}

static JSValue
nd_window_usp_ctor(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    (void)this_val;
    const char *q = "";
    if (argc >= 1) {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s) q = s;
        JSValue obj = nd_url_get_searchParams_object(ctx, q && q[0] == '?' ? q + 1 : q);
        if (s) JS_FreeCString(ctx, s);
        return obj;
    }
    return nd_url_get_searchParams_object(ctx, q);
}

static JSValue
nd_dom_parser_parseFromString(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *src = JS_ToCString(ctx, argv[0]);
    if (!src) return JS_NULL;
    nd_node *doc = nd_html_parse(src, -1);
    JS_FreeCString(ctx, src);
    if (!doc) return JS_NULL;
    if (g_active_js) g_ptr_array_add(g_active_js->orphan_nodes, doc);
    return nd_make_element(ctx, doc);
}

static JSValue
nd_window_dom_parser_ctor(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue obj = JS_NewObject(ctx);
    nd_bind_fn(ctx, obj, "parseFromString", nd_dom_parser_parseFromString, 2);
    return obj;
}

typedef struct nd_xhr_state {
    JSContext *ctx;
    JSValue obj;
    char *method;
    char *url;
} nd_xhr_state;

static void
nd_xhr_state_free(nd_xhr_state *st)
{
    if (!st) return;
    if (st->ctx) JS_FreeValue(st->ctx, st->obj);
    g_free(st->method);
    g_free(st->url);
    g_free(st);
}

static void
nd_on_xhr_done(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    nd_xhr_state *st = user_data;
    GError *err = NULL;
    nd_response *resp = nd_net_fetch_finish(result, &err);
    JSContext *ctx = st->ctx;
    if (resp && !err) {
        gboolean allow = cors_allows(g_active_js ? g_active_js->current_url : NULL,
                                     resp->final_url, resp->cors_allow_origin);
        JS_SetPropertyStr(ctx, st->obj, "status",
                          JS_NewInt32(ctx, allow ? (int)resp->status : 0));
        JS_SetPropertyStr(ctx, st->obj, "statusText",
                          JS_NewString(ctx, allow && resp->status == 200 ? "OK" : ""));
        const char *body = (allow && resp->body) ? (const char *)resp->body->data : "";
        gsize blen = (allow && resp->body) ? resp->body->len : 0;
        JS_SetPropertyStr(ctx, st->obj, "responseText",
                          JS_NewStringLen(ctx, body, blen));
        JS_SetPropertyStr(ctx, st->obj, "response",
                          JS_NewStringLen(ctx, body, blen));
        JS_SetPropertyStr(ctx, st->obj, "readyState", JS_NewInt32(ctx, 4));
    } else {
        JS_SetPropertyStr(ctx, st->obj, "status", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, st->obj, "readyState", JS_NewInt32(ctx, 4));
    }
    JSValue cb = JS_GetPropertyStr(ctx, st->obj, "onreadystatechange");
    if (JS_IsFunction(ctx, cb)) {
        JSValue r = JS_Call(ctx, cb, st->obj, 0, NULL);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, cb);
    JSValue lcb = JS_GetPropertyStr(ctx, st->obj, "onload");
    if (JS_IsFunction(ctx, lcb)) {
        JSValue r = JS_Call(ctx, lcb, st->obj, 0, NULL);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, lcb);
    if (resp) nd_response_free(resp);
    if (err) g_error_free(err);
    nd_xhr_state_free(st);
}

static JSValue
nd_xhr_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    const char *method = JS_ToCString(ctx, argv[0]);
    const char *url    = JS_ToCString(ctx, argv[1]);
    if (method) {
        JS_SetPropertyStr(ctx, this_val, "_method", JS_NewString(ctx, method));
        JS_FreeCString(ctx, method);
    }
    if (url) {
        JS_SetPropertyStr(ctx, this_val, "_url", JS_NewString(ctx, url));
        JS_FreeCString(ctx, url);
    }
    JS_SetPropertyStr(ctx, this_val, "readyState", JS_NewInt32(ctx, 1));
    return JS_UNDEFINED;
}

static JSValue
nd_xhr_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue url_v = JS_GetPropertyStr(ctx, this_val, "_url");
    const char *url = JS_ToCString(ctx, url_v);
    JS_FreeValue(ctx, url_v);
    if (!url) return JS_UNDEFINED;
    nd_xhr_state *st = g_new0(nd_xhr_state, 1);
    st->ctx = ctx;
    st->obj = JS_DupValue(ctx, this_val);
    st->url = g_strdup(url);
    JS_FreeCString(ctx, url);
    nd_net_fetch_async(st->url, NULL, nd_on_xhr_done, st);
    return JS_UNDEFINED;
}

static JSValue
nd_window_xhr_ctor(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "readyState",   JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "status",       JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "responseText", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, obj, "response",     JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, obj, "responseType", JS_NewString(ctx, ""));
    static const nd_fn_def xhr_noops[] = {
        { "setRequestHeader", 2 }, { "getResponseHeader", 1 },
        { "getAllResponseHeaders", 0 },
        { "addEventListener", 2 }, { "removeEventListener", 2 },
        { "abort", 0 },
    };
    nd_bind_fn(ctx, obj, "open", nd_xhr_open, 5);
    nd_bind_fn(ctx, obj, "send", nd_xhr_send, 1);
    nd_bind_fns(ctx, obj, nd_event_noop, xhr_noops, G_N_ELEMENTS(xhr_noops));
    return obj;
}

static JSValue
nd_form_data_method(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue entries = JS_GetPropertyStr(ctx, this_val, "_entries");
    if (!JS_IsArray(entries)) {
        JS_FreeValue(ctx, entries);
        entries = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, "_entries", JS_DupValue(ctx, entries));
    }
    return entries;
}

static JSValue
nd_form_data_append(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    JSValue entries = nd_form_data_method(ctx, this_val, 0, NULL);
    JSValue len_v = JS_GetPropertyStr(ctx, entries, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_v);
    JS_FreeValue(ctx, len_v);
    JSValue pair = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, pair, 0, JS_DupValue(ctx, argv[0]));
    JS_SetPropertyUint32(ctx, pair, 1, JS_DupValue(ctx, argv[1]));
    JS_SetPropertyUint32(ctx, entries, (uint32_t)len, pair);
    JS_FreeValue(ctx, entries);
    return JS_UNDEFINED;
}

static JSValue
nd_form_data_get(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NULL;
    JSValue entries = nd_form_data_method(ctx, this_val, 0, NULL);
    const char *key = JS_ToCString(ctx, argv[0]);
    JSValue result = JS_NULL;
    if (key) {
        JSValue len_v = JS_GetPropertyStr(ctx, entries, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, len_v);
        JS_FreeValue(ctx, len_v);
        for (int32_t i = 0; i < len; i++) {
            JSValue pair = JS_GetPropertyUint32(ctx, entries, (uint32_t)i);
            JSValue k = JS_GetPropertyUint32(ctx, pair, 0);
            const char *ks = JS_ToCString(ctx, k);
            JSValue v = JS_GetPropertyUint32(ctx, pair, 1);
            if (ks && strcmp(ks, key) == 0) {
                result = JS_DupValue(ctx, v);
                JS_FreeCString(ctx, ks);
                JS_FreeValue(ctx, k); JS_FreeValue(ctx, v); JS_FreeValue(ctx, pair);
                break;
            }
            if (ks) JS_FreeCString(ctx, ks);
            JS_FreeValue(ctx, k); JS_FreeValue(ctx, v); JS_FreeValue(ctx, pair);
        }
        JS_FreeCString(ctx, key);
    }
    JS_FreeValue(ctx, entries);
    return result;
}

static JSValue
nd_form_data_has(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    JSValue v = nd_form_data_get(ctx, this_val, argc, argv);
    gboolean has = !JS_IsNull(v);
    JS_FreeValue(ctx, v);
    return has ? JS_TRUE : JS_FALSE;
}

static JSValue
nd_window_form_data_ctor(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "_entries", JS_NewArray(ctx));
    nd_bind_fn(ctx, obj, "append", nd_form_data_append, 2);
    nd_bind_fn(ctx, obj, "set",    nd_form_data_append, 2);
    nd_bind_fn(ctx, obj, "get",    nd_form_data_get,    1);
    nd_bind_fn(ctx, obj, "getAll",  nd_form_data_method, 1);
    nd_bind_fn(ctx, obj, "has",     nd_form_data_has,    1);
    nd_bind_fn(ctx, obj, "delete",  nd_event_noop,       1);
    nd_bind_fn(ctx, obj, "entries", nd_form_data_method, 0);
    nd_bind_fn(ctx, obj, "keys",    nd_form_data_method, 0);
    nd_bind_fn(ctx, obj, "values",  nd_form_data_method, 0);
    nd_bind_fn(ctx, obj, "forEach", nd_event_noop,       1);
    return obj;
}

static JSValue
nd_window_abort_controller_ctor(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue obj = JS_NewObject(ctx);
    static const nd_fn_def sig_methods[] = {
        { "addEventListener", 2 }, { "removeEventListener", 2 },
        { "throwIfAborted", 0 },
    };
    JSValue sig = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, sig, "aborted", JS_FALSE);
    JS_SetPropertyStr(ctx, sig, "reason",  JS_UNDEFINED);
    nd_bind_fns(ctx, sig, nd_event_noop, sig_methods, G_N_ELEMENTS(sig_methods));
    JS_SetPropertyStr(ctx, obj, "signal", sig);
    nd_bind_fn(ctx, obj, "abort", nd_event_noop, 0);
    return obj;
}

static JSValue
nd_text_encoder_encode(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        JSValue arr = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, 0));
        return arr;
    }
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_NewArray(ctx);
    JSValue arr = JS_NewArray(ctx);
    gsize len = strlen(s);
    for (gsize i = 0; i < len; i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                             JS_NewInt32(ctx, (int32_t)(guchar)s[i]));
    JS_FreeCString(ctx, s);
    return arr;
}

static JSValue
nd_window_text_encoder_ctor(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "encoding", JS_NewString(ctx, "utf-8"));
    nd_bind_fn(ctx, obj, "encode", nd_text_encoder_encode, 1);
    return obj;
}

static JSValue
nd_text_decoder_decode(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    JSValue len_v = JS_GetPropertyStr(ctx, argv[0], "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_v);
    JS_FreeValue(ctx, len_v);
    if (len <= 0) return JS_NewString(ctx, "");
    GByteArray *out = g_byte_array_new();
    for (int32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        int32_t b = 0;
        JS_ToInt32(ctx, &b, v);
        JS_FreeValue(ctx, v);
        guint8 byte = (guint8)(b & 0xff);
        g_byte_array_append(out, &byte, 1);
    }
    JSValue r = JS_NewStringLen(ctx, (const char *)out->data, out->len);
    g_byte_array_free(out, TRUE);
    return r;
}

static JSValue
nd_window_text_decoder_ctor(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "encoding", JS_NewString(ctx, "utf-8"));
    nd_bind_fn(ctx, obj, "decode", nd_text_decoder_decode, 1);
    return obj;
}

static JSValue
nd_window_event_ctor(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue obj = JS_NewObject(ctx);
    if (argc >= 1) {
        JS_SetPropertyStr(ctx, obj, "type", JS_DupValue(ctx, argv[0]));
    }
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JS_SetPropertyStr(ctx, obj, "bubbles",
                          JS_GetPropertyStr(ctx, argv[1], "bubbles"));
        JS_SetPropertyStr(ctx, obj, "cancelable",
                          JS_GetPropertyStr(ctx, argv[1], "cancelable"));
        JS_SetPropertyStr(ctx, obj, "detail",
                          JS_GetPropertyStr(ctx, argv[1], "detail"));
    } else {
        JS_SetPropertyStr(ctx, obj, "bubbles", JS_FALSE);
        JS_SetPropertyStr(ctx, obj, "cancelable", JS_FALSE);
    }
    JS_SetPropertyStr(ctx, obj, "defaultPrevented", JS_FALSE);
    nd_bind_fn(ctx, obj, "preventDefault",            nd_event_prevent_default, 0);
    nd_bind_fn(ctx, obj, "stopPropagation",           nd_event_stop_propagation, 0);
    nd_bind_fn(ctx, obj, "stopImmediatePropagation",  nd_event_noop, 0);
    return obj;
}

static JSValue
nd_window_observer_ctor(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    static const nd_fn_def observer_methods[] = {
        { "observe", 2 }, { "unobserve", 1 },
        { "disconnect", 0 }, { "takeRecords", 0 },
    };
    JSValue obj = JS_NewObject(ctx);
    nd_bind_fns(ctx, obj, nd_event_noop, observer_methods, G_N_ELEMENTS(observer_methods));
    return obj;
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
    nd_bind_fn(ctx, event, "preventDefault",           nd_event_prevent_default, 0);
    nd_bind_fn(ctx, event, "stopPropagation",          nd_event_stop_propagation, 0);
    nd_bind_fn(ctx, event, "stopImmediatePropagation", nd_event_noop, 0);
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
    nd_node *parent = nd_unwrap_element_mut(this_val);
    if (!parent || argc < 1) return JS_NULL;
    nd_node *child = nd_unwrap_element_mut(argv[0]);
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
    nd_node *parent = nd_unwrap_element_mut(this_val);
    if (!parent || argc < 1) return JS_NULL;
    nd_node *child = nd_unwrap_element_mut(argv[0]);
    if (!child || child->parent != parent) return JS_NULL;
    nd_node_remove(child);
    if (g_active_js) {
        g_ptr_array_add(g_active_js->orphan_nodes, child);
        g_active_js->mutated = TRUE;
    }
    return JS_DupValue(ctx, argv[0]);
}

static JSValue
nd_element_insertBefore(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    nd_node *parent = nd_unwrap_element_mut(this_val);
    if (!parent || argc < 1) return JS_NULL;
    nd_node *newc = nd_unwrap_element_mut(argv[0]);
    if (!newc) return JS_NULL;
    nd_node *ref = argc >= 2 ? nd_unwrap_element_mut(argv[1]) : NULL;
    if (!ref || ref->parent != parent) {
        if (g_active_js)
            g_ptr_array_remove_fast(g_active_js->orphan_nodes, newc);
        nd_node_append_child(parent, newc);
    } else {
        if (newc->parent) nd_node_remove(newc);
        if (g_active_js)
            g_ptr_array_remove_fast(g_active_js->orphan_nodes, newc);
        newc->parent = parent;
        newc->next_sibling = ref;
        newc->prev_sibling = ref->prev_sibling;
        if (ref->prev_sibling) ref->prev_sibling->next_sibling = newc;
        else parent->first_child = newc;
        ref->prev_sibling = newc;
    }
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue
nd_element_replaceChild(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    nd_node *parent = nd_unwrap_element_mut(this_val);
    if (!parent || argc < 2) return JS_NULL;
    nd_node *newc = nd_unwrap_element_mut(argv[0]);
    nd_node *oldc = nd_unwrap_element_mut(argv[1]);
    if (!newc || !oldc || oldc->parent != parent) return JS_NULL;
    if (newc->parent) nd_node_remove(newc);
    if (g_active_js)
        g_ptr_array_remove_fast(g_active_js->orphan_nodes, newc);
    newc->parent = parent;
    newc->prev_sibling = oldc->prev_sibling;
    newc->next_sibling = oldc->next_sibling;
    if (oldc->prev_sibling) oldc->prev_sibling->next_sibling = newc;
    else parent->first_child = newc;
    if (oldc->next_sibling) oldc->next_sibling->prev_sibling = newc;
    else parent->last_child = newc;
    oldc->parent = NULL;
    oldc->prev_sibling = NULL;
    oldc->next_sibling = NULL;
    if (g_active_js) {
        g_ptr_array_add(g_active_js->orphan_nodes, oldc);
        g_active_js->mutated = TRUE;
    }
    return JS_DupValue(ctx, argv[1]);
}

static void
nd_insert_sibling_before(nd_node *ref, nd_node *newc)
{
    if (!ref || !ref->parent || !newc) return;
    if (newc->parent) nd_node_remove(newc);
    nd_node *parent = ref->parent;
    newc->parent = parent;
    newc->next_sibling = ref;
    newc->prev_sibling = ref->prev_sibling;
    if (ref->prev_sibling) ref->prev_sibling->next_sibling = newc;
    else parent->first_child = newc;
    ref->prev_sibling = newc;
}

static void
nd_insert_sibling_after(nd_node *ref, nd_node *newc)
{
    if (!ref || !ref->parent || !newc) return;
    if (newc->parent) nd_node_remove(newc);
    nd_node *parent = ref->parent;
    newc->parent = parent;
    newc->prev_sibling = ref;
    newc->next_sibling = ref->next_sibling;
    if (ref->next_sibling) ref->next_sibling->prev_sibling = newc;
    else parent->last_child = newc;
    ref->next_sibling = newc;
}

static JSValue
nd_element_insertAdjacentHTML(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    nd_node *self = nd_unwrap_element_mut(this_val);
    if (!self || argc < 2) return JS_UNDEFINED;
    const char *pos  = JS_ToCString(ctx, argv[0]);
    const char *html = JS_ToCString(ctx, argv[1]);
    if (!pos || !html) {
        if (pos)  JS_FreeCString(ctx, pos);
        if (html) JS_FreeCString(ctx, html);
        return JS_UNDEFINED;
    }
    nd_node *fragment = nd_html_parse(html, -1);
    if (fragment) {
        GPtrArray *kids = g_ptr_array_new();
        for (nd_node *c = fragment->first_child; c; c = c->next_sibling)
            g_ptr_array_add(kids, c);
        if (g_ascii_strcasecmp(pos, "beforebegin") == 0 && self->parent) {
            for (guint i = 0; i < kids->len; i++)
                nd_insert_sibling_before(self, (nd_node *)kids->pdata[i]);
        } else if (g_ascii_strcasecmp(pos, "afterbegin") == 0) {
            for (gint i = (gint)kids->len - 1; i >= 0; i--) {
                nd_node *c = kids->pdata[i];
                nd_node_remove(c);
                c->parent = self;
                c->next_sibling = self->first_child;
                c->prev_sibling = NULL;
                if (self->first_child) self->first_child->prev_sibling = c;
                else self->last_child = c;
                self->first_child = c;
            }
        } else if (g_ascii_strcasecmp(pos, "beforeend") == 0) {
            for (guint i = 0; i < kids->len; i++) {
                nd_node *c = kids->pdata[i];
                nd_node_remove(c);
                nd_node_append_child(self, c);
            }
        } else if (g_ascii_strcasecmp(pos, "afterend") == 0 && self->parent) {
            nd_node *ref = self;
            for (guint i = 0; i < kids->len; i++) {
                nd_node *c = kids->pdata[i];
                nd_insert_sibling_after(ref, c);
                ref = c;
            }
        }
        g_ptr_array_free(kids, TRUE);
        nd_node_free(fragment);
        if (g_active_js) g_active_js->mutated = TRUE;
    }
    JS_FreeCString(ctx, pos);
    JS_FreeCString(ctx, html);
    return JS_UNDEFINED;
}

static JSValue
nd_element_before(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv)
{
    nd_node *self = nd_unwrap_element_mut(this_val);
    if (!self || !self->parent) return JS_UNDEFINED;
    for (int i = 0; i < argc; i++) {
        nd_node *child = nd_unwrap_element_mut(argv[i]);
        nd_node *to_insert = NULL;
        if (child) {
            if (g_active_js)
                g_ptr_array_remove_fast(g_active_js->orphan_nodes, child);
            to_insert = child;
        } else {
            const char *txt = JS_ToCString(ctx, argv[i]);
            if (txt) {
                to_insert = nd_node_new_text(g_strdup(txt));
                JS_FreeCString(ctx, txt);
            }
        }
        if (to_insert) nd_insert_sibling_before(self, to_insert);
    }
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_element_after(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    nd_node *self = nd_unwrap_element_mut(this_val);
    if (!self || !self->parent) return JS_UNDEFINED;
    for (int i = argc - 1; i >= 0; i--) {
        nd_node *child = nd_unwrap_element_mut(argv[i]);
        nd_node *to_insert = NULL;
        if (child) {
            if (g_active_js)
                g_ptr_array_remove_fast(g_active_js->orphan_nodes, child);
            to_insert = child;
        } else {
            const char *txt = JS_ToCString(ctx, argv[i]);
            if (txt) {
                to_insert = nd_node_new_text(g_strdup(txt));
                JS_FreeCString(ctx, txt);
            }
        }
        if (to_insert) nd_insert_sibling_after(self, to_insert);
    }
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_element_replaceWith(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    nd_node *self = nd_unwrap_element_mut(this_val);
    if (!self || !self->parent) return JS_UNDEFINED;
    JSValue before_args[1] = { this_val };
    nd_element_before(ctx, before_args[0], argc, argv);
    nd_node_remove(self);
    if (g_active_js) {
        g_ptr_array_add(g_active_js->orphan_nodes, self);
        g_active_js->mutated = TRUE;
    }
    return JS_UNDEFINED;
}

static void
nd_node_normalize_walk(nd_node *n)
{
    if (!n) return;
    nd_node *c = n->first_child;
    while (c) {
        nd_node *next = c->next_sibling;
        if (c->kind == ND_NODE_TEXT && next && next->kind == ND_NODE_TEXT) {
            gsize la = c->text ? strlen(c->text) : 0;
            gsize lb = next->text ? strlen(next->text) : 0;
            char *merged = g_malloc(la + lb + 1);
            if (la) memcpy(merged, c->text, la);
            if (lb) memcpy(merged + la, next->text, lb);
            merged[la + lb] = '\0';
            g_free(c->text);
            c->text = merged;
            nd_node_remove(next);
            nd_node_free(next);
            continue;
        }
        if (c->kind == ND_NODE_ELEMENT) nd_node_normalize_walk(c);
        c = next;
    }
}

static JSValue
nd_element_normalize(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    nd_node *el = nd_unwrap_element_mut(this_val);
    if (!el) return JS_UNDEFINED;
    nd_node_normalize_walk(el);
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_element_cloneNode(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    (void)ctx;
    const nd_node *src = nd_unwrap_element(this_val);
    if (!src) return JS_NULL;
    gboolean deep = FALSE;
    if (argc >= 1) deep = JS_ToBool(ctx, argv[0]) ? TRUE : FALSE;
    nd_node *copy = nd_node_clone(src, deep);
    if (!copy) return JS_NULL;
    if (g_active_js) g_ptr_array_add(g_active_js->orphan_nodes, copy);
    return nd_make_element(ctx, copy);
}

static JSValue
nd_element_remove_self(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    nd_node *n = nd_unwrap_element_mut(this_val);
    if (!n || !n->parent) return JS_UNDEFINED;
    nd_node_remove(n);
    if (g_active_js) {
        g_ptr_array_add(g_active_js->orphan_nodes, n);
        g_active_js->mutated = TRUE;
    }
    return JS_UNDEFINED;
}

static JSValue
nd_element_append(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv)
{
    nd_node *parent = nd_unwrap_element_mut(this_val);
    if (!parent) return JS_UNDEFINED;
    for (int i = 0; i < argc; i++) {
        nd_node *child = nd_unwrap_element_mut(argv[i]);
        if (child) {
            if (child->parent) nd_node_remove(child);
            if (g_active_js)
                g_ptr_array_remove_fast(g_active_js->orphan_nodes, child);
            nd_node_append_child(parent, child);
        } else {
            const char *txt = JS_ToCString(ctx, argv[i]);
            if (txt) {
                nd_node *t = nd_node_new_text(g_strdup(txt));
                nd_node_append_child(parent, t);
                JS_FreeCString(ctx, txt);
            }
        }
    }
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_element_prepend(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    nd_node *parent = nd_unwrap_element_mut(this_val);
    if (!parent) return JS_UNDEFINED;
    nd_node *ref = parent->first_child;
    for (int i = 0; i < argc; i++) {
        nd_node *child = nd_unwrap_element_mut(argv[i]);
        nd_node *to_insert = NULL;
        if (child) {
            if (child->parent) nd_node_remove(child);
            if (g_active_js)
                g_ptr_array_remove_fast(g_active_js->orphan_nodes, child);
            to_insert = child;
        } else {
            const char *txt = JS_ToCString(ctx, argv[i]);
            if (txt) {
                to_insert = nd_node_new_text(g_strdup(txt));
                JS_FreeCString(ctx, txt);
            }
        }
        if (!to_insert) continue;
        if (!ref) {
            nd_node_append_child(parent, to_insert);
        } else {
            to_insert->parent = parent;
            to_insert->next_sibling = ref;
            to_insert->prev_sibling = ref->prev_sibling;
            if (ref->prev_sibling) ref->prev_sibling->next_sibling = to_insert;
            else parent->first_child = to_insert;
            ref->prev_sibling = to_insert;
        }
    }
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_element_get_attributes(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!n || n->kind != ND_NODE_ELEMENT) return arr;
    uint32_t i = 0;
    for (const nd_attr *a = n->attrs; a; a = a->next) {
        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "name",
                          JS_NewString(ctx, a->name ? a->name : ""));
        JS_SetPropertyStr(ctx, entry, "value",
                          JS_NewString(ctx, a->value ? a->value : ""));
        JS_SetPropertyUint32(ctx, arr, i++, entry);
    }
    JSValue len_v = JS_GetPropertyStr(ctx, arr, "length");
    JS_FreeValue(ctx, len_v);
    return arr;
}

static JSValue
nd_element_getAttributeNames(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    const nd_node *n = nd_unwrap_element(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!n || n->kind != ND_NODE_ELEMENT) return arr;
    uint32_t i = 0;
    for (const nd_attr *a = n->attrs; a; a = a->next)
        if (a->name)
            JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, a->name));
    return arr;
}

static JSValue nd_element_setAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue nd_element_removeAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

static JSValue
nd_element_animate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    static const nd_fn_def anim_methods[] = {
        { "play", 0 }, { "pause", 0 }, { "cancel", 0 },
        { "finish", 0 }, { "reverse", 0 },
    };
    JSValue anim = JS_NewObject(ctx);
    nd_bind_fns(ctx, anim, nd_event_noop, anim_methods, G_N_ELEMENTS(anim_methods));
    JS_SetPropertyStr(ctx, anim, "playState", JS_NewString(ctx, "finished"));
    JSValue finished = JS_NewObject(ctx);
    nd_bind_fn(ctx, finished, "then", nd_event_noop, 1);
    JS_SetPropertyStr(ctx, anim, "finished", finished);
    return anim;
}

static JSValue
nd_element_toggleAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    nd_node *n = nd_unwrap_element_mut(this_val);
    if (!n || argc < 1) return JS_FALSE;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_FALSE;
    gboolean had = nd_element_get_attr(n, name) != NULL;
    gboolean want;
    if (argc >= 2) want = JS_ToBool(ctx, argv[1]) ? TRUE : FALSE;
    else           want = !had;
    if (want && !had)      nd_element_set_attr(n, name, "");
    else if (!want && had) nd_element_remove_attr(n, name);
    JS_FreeCString(ctx, name);
    if (g_active_js) g_active_js->mutated = TRUE;
    return want ? JS_TRUE : JS_FALSE;
}

static JSValue
nd_element_getAttributeNS(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_NULL;
    JSValueConst forwarded[1] = { argv[1] };
    return nd_element_getAttribute(ctx, this_val, 1, forwarded);
}

static JSValue
nd_element_hasAttributeNS(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_FALSE;
    JSValueConst forwarded[1] = { argv[1] };
    return nd_element_hasAttribute(ctx, this_val, 1, forwarded);
}

static JSValue
nd_element_setAttributeNS(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 3) return JS_UNDEFINED;
    JSValueConst forwarded[2] = { argv[1], argv[2] };
    return nd_element_setAttribute(ctx, this_val, 2, forwarded);
}

static JSValue
nd_element_removeAttributeNS(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    JSValueConst forwarded[1] = { argv[1] };
    return nd_element_removeAttribute(ctx, this_val, 1, forwarded);
}

static JSValue
nd_element_setAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    nd_node *n = nd_unwrap_element_mut(this_val);
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
    nd_node *n = nd_unwrap_element_mut(this_val);
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
nd_element_get_parentNode(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n || !n->parent) return JS_NULL;
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

static JSValue
nd_element_get_firstChild(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    return nd_make_element(ctx, n ? n->first_child : NULL);
}

static JSValue
nd_element_get_lastChild(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    return nd_make_element(ctx, n ? n->last_child : NULL);
}

static JSValue
nd_element_get_lastElementChild(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_NULL;
    for (const nd_node *c = n->last_child; c; c = c->prev_sibling)
        if (c->kind == ND_NODE_ELEMENT)
            return nd_make_element(ctx, c);
    return JS_NULL;
}

static JSValue
nd_element_get_nextSibling(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    return nd_make_element(ctx, n ? n->next_sibling : NULL);
}

static JSValue
nd_element_get_previousSibling(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    return nd_make_element(ctx, n ? n->prev_sibling : NULL);
}

static JSValue
nd_element_get_childNodes(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!n) return arr;
    uint32_t i = 0;
    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        JS_SetPropertyUint32(ctx, arr, i++, nd_make_element(ctx, c));
    return arr;
}

static JSValue
nd_element_get_childElementCount(JSContext *ctx, JSValueConst this_val)
{
    (void)ctx;
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_NewInt32(ctx, 0);
    int count = 0;
    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        if (c->kind == ND_NODE_ELEMENT) count++;
    return JS_NewInt32(ctx, count);
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
element_has_class_token(const nd_node *n, const char *want, gsize wl)
{
    const char *cls = nd_element_get_attr(n, "class");
    if (!cls) return FALSE;
    const char *p = cls;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if ((gsize)(p - tok) == wl && strncmp(tok, want, wl) == 0) return TRUE;
    }
    return FALSE;
}

static gboolean
element_has_class(const nd_node *n, const char *want)
{
    const char *p = want;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        gsize wl = (gsize)(p - tok);
        if (wl == 0) continue;
        if (!element_has_class_token(n, tok, wl)) return FALSE;
    }
    return TRUE;
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

static JSValue
nd_element_matches(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el || argc < 1) return JS_FALSE;
    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return JS_FALSE;
    GPtrArray *sels = nd_css_parse_selector_list(sel);
    JS_FreeCString(ctx, sel);
    gboolean m = nd_matches_any_selector(sels, el);
    g_ptr_array_free(sels, TRUE);
    return m ? JS_TRUE : JS_FALSE;
}

static JSValue
nd_element_closest(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el || argc < 1) return JS_NULL;
    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return JS_NULL;
    GPtrArray *sels = nd_css_parse_selector_list(sel);
    JS_FreeCString(ctx, sel);
    const nd_node *cur = el;
    while (cur && cur->kind == ND_NODE_ELEMENT) {
        if (nd_matches_any_selector(sels, cur)) {
            g_ptr_array_free(sels, TRUE);
            return nd_make_element(ctx, cur);
        }
        cur = cur->parent;
    }
    g_ptr_array_free(sels, TRUE);
    return JS_NULL;
}

static JSValue
nd_element_contains(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    (void)ctx;
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el || argc < 1) return JS_FALSE;
    const nd_node *other = nd_unwrap_element(argv[0]);
    if (!other) return JS_FALSE;
    for (const nd_node *cur = other; cur; cur = cur->parent)
        if (cur == el) return JS_TRUE;
    return JS_FALSE;
}

static JSValue
nd_element_hasChildNodes(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    const nd_node *el = nd_unwrap_element(this_val);
    return (el && el->first_child) ? JS_TRUE : JS_FALSE;
}

static JSValue
nd_element_get_nodeType(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el) return JS_NewInt32(ctx, 0);
    switch (el->kind) {
        case ND_NODE_ELEMENT: return JS_NewInt32(ctx, 1);
        case ND_NODE_TEXT:    return JS_NewInt32(ctx, 3);
        case ND_NODE_COMMENT: return JS_NewInt32(ctx, 8);
        case ND_NODE_DOCUMENT:return JS_NewInt32(ctx, 9);
        case ND_NODE_DOCTYPE: return JS_NewInt32(ctx, 10);
    }
    return JS_NewInt32(ctx, 0);
}

static JSValue
nd_element_get_nodeName(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el || !el->name) return JS_NewString(ctx, "#text");
    char *up = g_ascii_strup(el->name, -1);
    JSValue v = JS_NewString(ctx, up);
    g_free(up);
    return v;
}

static JSValue
nd_element_getBoundingClientRect(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "x",      JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, r, "y",      JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, r, "top",    JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, r, "left",   JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, r, "right",  JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, r, "bottom", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, r, "width",  JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, r, "height", JS_NewFloat64(ctx, 0));
    return r;
}

static JSValue
nd_element_get_zero_int(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewInt32(ctx, 0);
}

static JSValue
nd_element_get_one_int(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewInt32(ctx, 1);
}

static JSValue
nd_element_get_true_prop(JSContext *ctx, JSValueConst this_val)
{
    (void)ctx; (void)this_val;
    return JS_TRUE;
}

static JSValue
nd_element_get_empty_array_prop(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewArray(ctx);
}

static JSValue
nd_element_get_validity(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    JSValue v = JS_NewObject(ctx);
    static const char *flags[] = {
        "valueMissing","typeMismatch","patternMismatch","tooLong","tooShort",
        "rangeUnderflow","rangeOverflow","stepMismatch","badInput",
        "customError",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(flags); i++)
        JS_SetPropertyStr(ctx, v, flags[i], JS_FALSE);
    JS_SetPropertyStr(ctx, v, "valid", JS_TRUE);
    return v;
}

static JSValue
nd_element_get_validation_message(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewString(ctx, "");
}

static JSValue
nd_element_get_labels(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!n) return arr;
    const char *id = nd_element_get_attr(n, "id");
    if (!id || !*id) return arr;
    if (!g_active_js || !g_active_js->current_doc) return arr;
    uint32_t idx = 0;
    GQueue q = G_QUEUE_INIT;
    g_queue_push_tail(&q, g_active_js->current_doc);
    while (!g_queue_is_empty(&q)) {
        nd_node *cur = g_queue_pop_head(&q);
        for (nd_node *c = cur->first_child; c; c = c->next_sibling) {
            if (c->kind == ND_NODE_ELEMENT && c->name &&
                g_ascii_strcasecmp(c->name, "label") == 0) {
                const char *forv = nd_element_get_attr(c, "for");
                if (forv && strcmp(forv, id) == 0)
                    JS_SetPropertyUint32(ctx, arr, idx++, nd_make_element(ctx, c));
            }
            g_queue_push_tail(&q, c);
        }
    }
    g_queue_clear(&q);
    return arr;
}

static JSValue
nd_element_get_selection_dir(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewString(ctx, "none");
}

static JSValue
nd_element_get_default_value(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_NewString(ctx, "");
    const char *v = nd_element_get_attr(n, "value");
    return JS_NewString(ctx, v ? v : "");
}

static JSValue
nd_element_get_default_checked(JSContext *ctx, JSValueConst this_val)
{
    (void)ctx;
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_FALSE;
    return nd_element_get_attr(n, "checked") ? JS_TRUE : JS_FALSE;
}

static JSValue
nd_element_get_default_selected(JSContext *ctx, JSValueConst this_val)
{
    (void)ctx;
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_FALSE;
    return nd_element_get_attr(n, "selected") ? JS_TRUE : JS_FALSE;
}

static JSValue
nd_element_get_value_as_number(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_NewFloat64(ctx, (double)NAN);
    const char *v = nd_element_get_attr(n, "value");
    if (!v || !*v) return JS_NewFloat64(ctx, (double)NAN);
    char *end = NULL;
    double d = g_ascii_strtod(v, &end);
    if (end == v) return JS_NewFloat64(ctx, (double)NAN);
    return JS_NewFloat64(ctx, d);
}

static JSValue
nd_element_get_form_enctype(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_NewString(ctx, "");
    const char *v = nd_element_get_attr(n, "enctype");
    return JS_NewString(ctx, v ? v : "");
}

static JSValue
nd_element_get_isConnected(JSContext *ctx, JSValueConst this_val)
{
    (void)ctx;
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n || !g_active_js || !g_active_js->current_doc) return JS_FALSE;
    for (const nd_node *p = n; p; p = p->parent)
        if (p == g_active_js->current_doc) return JS_TRUE;
    return JS_FALSE;
}

static JSValue
nd_element_get_ownerDocument(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!g_active_js || !g_active_js->current_doc) return JS_NULL;
    return nd_make_element(ctx, g_active_js->current_doc);
}

static JSValue
nd_element_get_namespaceURI(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewString(ctx, "http://www.w3.org/1999/xhtml");
}

static JSValue
nd_element_get_null(JSContext *ctx, JSValueConst this_val)
{
    (void)ctx; (void)this_val;
    return JS_NULL;
}

static JSValue
nd_element_attachShadow(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "attachShadow is not supported");
}

static JSValue
nd_element_getRootNode(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return JS_NULL;
    while (n->parent) n = n->parent;
    return nd_make_element(ctx, n);
}

static JSValue
nd_element_isEqualNode(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    (void)ctx;
    if (argc < 1) return JS_FALSE;
    const nd_node *a = nd_unwrap_element(this_val);
    const nd_node *b = nd_unwrap_element(argv[0]);
    return (a == b && a) ? JS_TRUE : JS_FALSE;
}

static JSValue
nd_element_compareDocumentPosition(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, 0);
}

static JSValue
nd_element_lookupNamespaceURI(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_NULL;
}

static JSValue
nd_element_isDefaultNamespace(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_TRUE;
}

static JSValue
nd_element_getClientRects(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue arr = JS_NewArray(ctx);
    JSValue rect = nd_element_getBoundingClientRect(ctx, this_val, 0, NULL);
    JS_SetPropertyUint32(ctx, arr, 0, rect);
    return arr;
}

static JSValue
nd_element_scroll_int_set(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    (void)ctx; (void)this_val; (void)val;
    return JS_UNDEFINED;
}

static JSValue
nd_element_get_hidden(JSContext *ctx, JSValueConst this_val)
{
    (void)ctx;
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el) return JS_FALSE;
    return nd_element_get_attr(el, "hidden") ? JS_TRUE : JS_FALSE;
}

static JSValue
nd_element_set_hidden(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    nd_node *el = nd_unwrap_element_mut(this_val);
    if (!el) return JS_UNDEFINED;
    if (JS_ToBool(ctx, val)) nd_element_set_attr(el, "hidden", "");
    else                     nd_element_remove_attr(el, "hidden");
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_element_get_disabled(JSContext *ctx, JSValueConst this_val)
{
    (void)ctx;
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el) return JS_FALSE;
    return nd_element_get_attr(el, "disabled") ? JS_TRUE : JS_FALSE;
}

static JSValue
nd_element_set_disabled(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    nd_node *el = nd_unwrap_element_mut(this_val);
    if (!el) return JS_UNDEFINED;
    if (JS_ToBool(ctx, val)) nd_element_set_attr(el, "disabled", "");
    else                     nd_element_remove_attr(el, "disabled");
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_element_get_checked(JSContext *ctx, JSValueConst this_val)
{
    (void)ctx;
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el) return JS_FALSE;
    return nd_element_get_attr(el, "checked") ? JS_TRUE : JS_FALSE;
}

static JSValue
nd_element_set_checked(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    nd_node *el = nd_unwrap_element_mut(this_val);
    if (!el) return JS_UNDEFINED;
    if (JS_ToBool(ctx, val)) nd_element_set_attr(el, "checked", "");
    else                     nd_element_remove_attr(el, "checked");
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static const nd_node *
nd_select_chosen_option(const nd_node *sel)
{
    const nd_node *first = NULL;
    for (const nd_node *c = sel->first_child; c; c = c->next_sibling) {
        if (c->kind != ND_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "option") == 0) {
            if (!first) first = c;
            if (nd_element_get_attr(c, "selected")) return c;
        } else if (strcmp(c->name, "optgroup") == 0) {
            for (const nd_node *cc = c->first_child; cc; cc = cc->next_sibling) {
                if (cc->kind == ND_NODE_ELEMENT && cc->name &&
                    strcmp(cc->name, "option") == 0) {
                    if (!first) first = cc;
                    if (nd_element_get_attr(cc, "selected")) return cc;
                }
            }
        }
    }
    return first;
}

static char *
nd_option_value_dup(const nd_node *opt)
{
    if (!opt) return g_strdup("");
    const char *v = nd_element_get_attr(opt, "value");
    if (v) return g_strdup(v);
    return nd_node_collect_text(opt);
}

static JSValue
nd_element_get_value_prop(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el) return JS_NewString(ctx, "");
    if (el->name && strcmp(el->name, "textarea") == 0) {
        char *t = nd_node_collect_text(el);
        JSValue v = JS_NewString(ctx, t ? t : "");
        g_free(t);
        return v;
    }
    if (el->name && strcmp(el->name, "select") == 0) {
        char *t = nd_option_value_dup(nd_select_chosen_option(el));
        JSValue v = JS_NewString(ctx, t ? t : "");
        g_free(t);
        return v;
    }
    if (el->name && strcmp(el->name, "option") == 0) {
        const char *vv = nd_element_get_attr(el, "value");
        if (vv) return JS_NewString(ctx, vv);
        char *t = nd_node_collect_text(el);
        JSValue v = JS_NewString(ctx, t ? t : "");
        g_free(t);
        return v;
    }
    const char *v = nd_element_get_attr(el, "value");
    return JS_NewString(ctx, v ? v : "");
}

static JSValue
nd_element_set_value_prop(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    nd_node *el = nd_unwrap_element_mut(this_val);
    if (!el) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (!s) return JS_UNDEFINED;
    if (el->name && strcmp(el->name, "select") == 0) {
        nd_node *chosen = NULL;
        for (nd_node *c = el->first_child; c; c = c->next_sibling) {
            if (c->kind != ND_NODE_ELEMENT || !c->name) continue;
            if (strcmp(c->name, "option") == 0) {
                char *ov = nd_option_value_dup(c);
                if (ov && strcmp(ov, s) == 0) { chosen = c; g_free(ov); break; }
                g_free(ov);
            } else if (strcmp(c->name, "optgroup") == 0) {
                for (nd_node *cc = c->first_child; cc && !chosen; cc = cc->next_sibling) {
                    if (cc->kind != ND_NODE_ELEMENT || !cc->name) continue;
                    if (strcmp(cc->name, "option") == 0) {
                        char *ov = nd_option_value_dup(cc);
                        if (ov && strcmp(ov, s) == 0) chosen = cc;
                        g_free(ov);
                    }
                }
                if (chosen) break;
            }
        }
        for (nd_node *c = el->first_child; c; c = c->next_sibling) {
            if (c->kind != ND_NODE_ELEMENT || !c->name) continue;
            if (strcmp(c->name, "option") == 0)
                nd_element_remove_attr(c, "selected");
            else if (strcmp(c->name, "optgroup") == 0) {
                for (nd_node *cc = c->first_child; cc; cc = cc->next_sibling)
                    if (cc->kind == ND_NODE_ELEMENT && cc->name &&
                        strcmp(cc->name, "option") == 0)
                        nd_element_remove_attr(cc, "selected");
            }
        }
        if (chosen) nd_element_set_attr(chosen, "selected", "");
        JS_FreeCString(ctx, s);
        if (g_active_js) g_active_js->mutated = TRUE;
        return JS_UNDEFINED;
    }
    nd_element_set_attr(el, "value", s);
    JS_FreeCString(ctx, s);
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_element_get_selectedIndex(JSContext *ctx, JSValueConst this_val)
{
    (void)ctx;
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el || !el->name || strcmp(el->name, "select") != 0)
        return JS_NewInt32(ctx, -1);
    int idx = 0;
    int first_idx = -1;
    for (const nd_node *c = el->first_child; c; c = c->next_sibling) {
        if (c->kind != ND_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "option") == 0) {
            if (first_idx < 0) first_idx = idx;
            if (nd_element_get_attr(c, "selected")) return JS_NewInt32(ctx, idx);
            idx++;
        } else if (strcmp(c->name, "optgroup") == 0) {
            for (const nd_node *cc = c->first_child; cc; cc = cc->next_sibling) {
                if (cc->kind == ND_NODE_ELEMENT && cc->name &&
                    strcmp(cc->name, "option") == 0) {
                    if (first_idx < 0) first_idx = idx;
                    if (nd_element_get_attr(cc, "selected")) return JS_NewInt32(ctx, idx);
                    idx++;
                }
            }
        }
    }
    return JS_NewInt32(ctx, first_idx);
}

static void
nd_form_collect_controls(const nd_node *form, JSContext *ctx, JSValue arr, uint32_t *idx)
{
    if (!form) return;
    for (const nd_node *c = form->first_child; c; c = c->next_sibling) {
        if (c->kind == ND_NODE_ELEMENT && c->name) {
            if (g_ascii_strcasecmp(c->name, "input") == 0 ||
                g_ascii_strcasecmp(c->name, "select") == 0 ||
                g_ascii_strcasecmp(c->name, "textarea") == 0 ||
                g_ascii_strcasecmp(c->name, "button") == 0 ||
                g_ascii_strcasecmp(c->name, "fieldset") == 0 ||
                g_ascii_strcasecmp(c->name, "output") == 0)
                JS_SetPropertyUint32(ctx, arr, (*idx)++, nd_make_element(ctx, c));
        }
        nd_form_collect_controls(c, ctx, arr, idx);
    }
}

static JSValue
nd_element_get_option_index(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *opt = nd_unwrap_element(this_val);
    if (!opt) return JS_NewInt32(ctx, -1);
    const nd_node *sel = NULL;
    for (const nd_node *p = opt->parent; p; p = p->parent) {
        if (p->kind == ND_NODE_ELEMENT && p->name &&
            g_ascii_strcasecmp(p->name, "select") == 0) { sel = p; break; }
    }
    if (!sel) return JS_NewInt32(ctx, -1);
    int idx = 0;
    for (const nd_node *c = sel->first_child; c; c = c->next_sibling) {
        if (c->kind != ND_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "option") == 0) {
            if (c == opt) return JS_NewInt32(ctx, idx);
            idx++;
        } else if (strcmp(c->name, "optgroup") == 0) {
            for (const nd_node *cc = c->first_child; cc; cc = cc->next_sibling) {
                if (cc->kind == ND_NODE_ELEMENT && cc->name &&
                    strcmp(cc->name, "option") == 0) {
                    if (cc == opt) return JS_NewInt32(ctx, idx);
                    idx++;
                }
            }
        }
    }
    return JS_NewInt32(ctx, -1);
}

static JSValue
nd_element_get_select_length(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *sel = nd_unwrap_element(this_val);
    if (!sel || !sel->name || strcmp(sel->name, "select") != 0)
        return JS_NewInt32(ctx, 0);
    int count = 0;
    for (const nd_node *c = sel->first_child; c; c = c->next_sibling) {
        if (c->kind != ND_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "option") == 0) count++;
        else if (strcmp(c->name, "optgroup") == 0) {
            for (const nd_node *cc = c->first_child; cc; cc = cc->next_sibling)
                if (cc->kind == ND_NODE_ELEMENT && cc->name &&
                    strcmp(cc->name, "option") == 0) count++;
        }
    }
    return JS_NewInt32(ctx, count);
}

static JSValue
nd_element_table_rows(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *tbl = nd_unwrap_element(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!tbl) return arr;
    uint32_t idx = 0;
    GQueue q = G_QUEUE_INIT;
    g_queue_push_tail(&q, (nd_node *)tbl);
    while (!g_queue_is_empty(&q)) {
        nd_node *n = g_queue_pop_head(&q);
        for (nd_node *c = n->first_child; c; c = c->next_sibling) {
            if (c->kind == ND_NODE_ELEMENT && c->name &&
                g_ascii_strcasecmp(c->name, "tr") == 0)
                JS_SetPropertyUint32(ctx, arr, idx++, nd_make_element(ctx, c));
            else
                g_queue_push_tail(&q, c);
        }
    }
    g_queue_clear(&q);
    return arr;
}

static JSValue
nd_element_table_section(JSContext *ctx, JSValueConst this_val, const char *tag)
{
    const nd_node *tbl = nd_unwrap_element(this_val);
    if (!tbl) return JS_NULL;
    for (const nd_node *c = tbl->first_child; c; c = c->next_sibling)
        if (c->kind == ND_NODE_ELEMENT && c->name &&
            g_ascii_strcasecmp(c->name, tag) == 0)
            return nd_make_element(ctx, c);
    return JS_NULL;
}

static JSValue
nd_element_table_caption(JSContext *ctx, JSValueConst this_val)
{ return nd_element_table_section(ctx, this_val, "caption"); }

static JSValue
nd_element_table_thead(JSContext *ctx, JSValueConst this_val)
{ return nd_element_table_section(ctx, this_val, "thead"); }

static JSValue
nd_element_table_tfoot(JSContext *ctx, JSValueConst this_val)
{ return nd_element_table_section(ctx, this_val, "tfoot"); }

static JSValue
nd_element_table_tbodies(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *tbl = nd_unwrap_element(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!tbl) return arr;
    uint32_t i = 0;
    for (const nd_node *c = tbl->first_child; c; c = c->next_sibling)
        if (c->kind == ND_NODE_ELEMENT && c->name &&
            g_ascii_strcasecmp(c->name, "tbody") == 0)
            JS_SetPropertyUint32(ctx, arr, i++, nd_make_element(ctx, c));
    return arr;
}

static JSValue
nd_element_tr_cells(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *tr = nd_unwrap_element(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!tr) return arr;
    uint32_t i = 0;
    for (const nd_node *c = tr->first_child; c; c = c->next_sibling)
        if (c->kind == ND_NODE_ELEMENT && c->name &&
            (g_ascii_strcasecmp(c->name, "td") == 0 ||
             g_ascii_strcasecmp(c->name, "th") == 0))
            JS_SetPropertyUint32(ctx, arr, i++, nd_make_element(ctx, c));
    return arr;
}

static JSValue
nd_element_get_form_elements(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *el = nd_unwrap_element(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!el || !el->name || strcmp(el->name, "form") != 0) return arr;
    uint32_t i = 0;
    nd_form_collect_controls(el, ctx, arr, &i);
    return arr;
}

static JSValue
nd_element_get_form(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el) return JS_NULL;
    for (const nd_node *p = el->parent; p; p = p->parent) {
        if (p->kind == ND_NODE_ELEMENT && p->name &&
            strcmp(p->name, "form") == 0)
            return nd_make_element(ctx, p);
    }
    return JS_NULL;
}

static JSValue
nd_element_get_options(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *el = nd_unwrap_element(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!el || !el->name || strcmp(el->name, "select") != 0) return arr;
    uint32_t i = 0;
    for (const nd_node *c = el->first_child; c; c = c->next_sibling) {
        if (c->kind != ND_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "option") == 0)
            JS_SetPropertyUint32(ctx, arr, i++, nd_make_element(ctx, c));
        else if (strcmp(c->name, "optgroup") == 0) {
            for (const nd_node *cc = c->first_child; cc; cc = cc->next_sibling)
                if (cc->kind == ND_NODE_ELEMENT && cc->name &&
                    strcmp(cc->name, "option") == 0)
                    JS_SetPropertyUint32(ctx, arr, i++, nd_make_element(ctx, cc));
        }
    }
    return arr;
}

static JSValue
nd_element_get_dataset(JSContext *ctx, JSValueConst this_val)
{
    const nd_node *el = nd_unwrap_element(this_val);
    JSValue ds = JS_NewObject(ctx);
    if (!el || el->kind != ND_NODE_ELEMENT) return ds;
    for (const nd_attr *a = el->attrs; a; a = a->next) {
        const char *name = a->name;
        const char *value = a->value;
        if (!name || strncmp(name, "data-", 5) != 0) continue;
        GString *camel = g_string_new(NULL);
        const char *p = name + 5;
        gboolean upper_next = FALSE;
        while (*p) {
            if (*p == '-') upper_next = TRUE;
            else {
                g_string_append_c(camel, upper_next ? g_ascii_toupper(*p) : *p);
                upper_next = FALSE;
            }
            p++;
        }
        JS_SetPropertyStr(ctx, ds, camel->str,
                          JS_NewString(ctx, value ? value : ""));
        g_string_free(camel, TRUE);
    }
    return ds;
}

static JSValue
nd_element_focus(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static JSValue
nd_element_scrollIntoView(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el || !g_active_js || !g_active_js->scroll_to_cb) return JS_UNDEFINED;
    g_active_js->scroll_to_cb(el, g_active_js->scroll_to_user_data);
    return JS_UNDEFINED;
}

static JSValue
nd_element_show(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    nd_node *el = nd_unwrap_element_mut(this_val);
    if (!el) return JS_UNDEFINED;
    nd_element_set_attr(el, "open", "");
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_element_close(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    nd_node *el = nd_unwrap_element_mut(this_val);
    if (!el) return JS_UNDEFINED;
    nd_element_remove_attr(el, "open");
    if (g_active_js) {
        nd_js_dispatch_event(g_active_js, el, "close", NULL);
        g_active_js->mutated = TRUE;
    }
    return JS_UNDEFINED;
}

static gboolean
nd_node_is_submit_trigger(const nd_node *el)
{
    if (!el || el->kind != ND_NODE_ELEMENT || !el->name) return FALSE;
    if (g_ascii_strcasecmp(el->name, "button") == 0) {
        const char *t = nd_element_get_attr(el, "type");
        return !t || g_ascii_strcasecmp(t, "submit") == 0;
    }
    if (g_ascii_strcasecmp(el->name, "input") == 0) {
        const char *t = nd_element_get_attr(el, "type");
        return t && (g_ascii_strcasecmp(t, "submit") == 0 ||
                     g_ascii_strcasecmp(t, "image") == 0);
    }
    return FALSE;
}

static void nd_form_reset_walk(nd_node *n);

static gboolean
nd_node_is_reset_trigger(const nd_node *el)
{
    if (!el || el->kind != ND_NODE_ELEMENT || !el->name) return FALSE;
    const char *t = nd_element_get_attr(el, "type");
    if (!t) return FALSE;
    if (g_ascii_strcasecmp(t, "reset") != 0) return FALSE;
    return g_ascii_strcasecmp(el->name, "button") == 0 ||
           g_ascii_strcasecmp(el->name, "input")  == 0;
}

static const nd_node *
nd_node_enclosing_form(const nd_node *el)
{
    for (const nd_node *p = el; p; p = p->parent)
        if (p->kind == ND_NODE_ELEMENT && p->name &&
            g_ascii_strcasecmp(p->name, "form") == 0)
            return p;
    return NULL;
}

static JSValue
nd_element_click(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el || !g_active_js) return JS_UNDEFINED;
    gboolean prevented = FALSE;
    nd_js_dispatch_event(g_active_js, el, "click", &prevented);
    if (prevented) return JS_UNDEFINED;
    if (el->kind == ND_NODE_ELEMENT && el->name &&
        g_ascii_strcasecmp(el->name, "a") == 0) {
        const char *href = nd_element_get_attr(el, "href");
        if (href && *href && g_active_js->nav_cb)
            g_active_js->nav_cb(href, FALSE, g_active_js->nav_user_data);
        return JS_UNDEFINED;
    }
    if (nd_node_is_submit_trigger(el) && g_active_js->form_submit_cb) {
        const nd_node *form = nd_node_enclosing_form(el);
        if (form)
            g_active_js->form_submit_cb(form, el, g_active_js->form_submit_user_data);
    } else if (nd_node_is_reset_trigger(el)) {
        nd_node *form = (nd_node *)nd_node_enclosing_form(el);
        if (form) {
            nd_form_reset_walk(form);
            if (g_active_js) g_active_js->mutated = TRUE;
        }
    }
    return JS_UNDEFINED;
}

static JSValue
nd_element_form_submit(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el || !g_active_js || !g_active_js->form_submit_cb) return JS_UNDEFINED;
    if (el->kind != ND_NODE_ELEMENT || !el->name ||
        g_ascii_strcasecmp(el->name, "form") != 0) return JS_UNDEFINED;
    g_active_js->form_submit_cb(el, NULL, g_active_js->form_submit_user_data);
    return JS_UNDEFINED;
}

static void
nd_form_reset_walk(nd_node *n)
{
    if (!n) return;
    if (n->kind == ND_NODE_ELEMENT && n->name) {
        if (g_ascii_strcasecmp(n->name, "input") == 0 ||
            g_ascii_strcasecmp(n->name, "textarea") == 0) {
            const char *defv = nd_element_get_attr(n, "value");
            (void)defv;
            const char *type = nd_element_get_attr(n, "type");
            if (type && (g_ascii_strcasecmp(type, "checkbox") == 0 ||
                         g_ascii_strcasecmp(type, "radio") == 0)) {
                if (nd_element_get_attr(n, "defaultChecked"))
                    nd_element_set_attr(n, "checked", "");
                else
                    nd_element_remove_attr(n, "checked");
            }
        } else if (g_ascii_strcasecmp(n->name, "select") == 0) {
            for (nd_node *o = n->first_child; o; o = o->next_sibling) {
                if (o->kind == ND_NODE_ELEMENT && o->name &&
                    g_ascii_strcasecmp(o->name, "option") == 0)
                    nd_element_remove_attr(o, "selected");
            }
        }
    }
    for (nd_node *c = n->first_child; c; c = c->next_sibling)
        nd_form_reset_walk(c);
}

static JSValue
nd_element_form_reset(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    nd_node *el = nd_unwrap_element_mut(this_val);
    if (!el || el->kind != ND_NODE_ELEMENT || !el->name) return JS_UNDEFINED;
    if (g_ascii_strcasecmp(el->name, "form") != 0) return JS_UNDEFINED;
    nd_form_reset_walk(el);
    if (g_active_js) g_active_js->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_element_getContext(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue obj = JS_NewObject(ctx);
    static const char *methods[] = {
        "save","restore","translate","rotate","scale","setTransform","transform",
        "resetTransform","beginPath","closePath","moveTo","lineTo","arc","arcTo",
        "rect","fillRect","strokeRect","clearRect","fill","stroke","clip",
        "fillText","strokeText","drawImage","createImageData","getImageData",
        "putImageData","measureText","createLinearGradient","createRadialGradient",
        "createPattern", NULL,
    };
    for (int i = 0; methods[i]; i++)
        JS_SetPropertyStr(ctx, obj, methods[i],
            JS_NewCFunction(ctx, nd_event_noop, methods[i], 0));
    JS_SetPropertyStr(ctx, obj, "canvas", JS_DupValue(ctx, this_val));
    JS_SetPropertyStr(ctx, obj, "fillStyle",   JS_NewString(ctx, "#000"));
    JS_SetPropertyStr(ctx, obj, "strokeStyle", JS_NewString(ctx, "#000"));
    JS_SetPropertyStr(ctx, obj, "lineWidth",   JS_NewFloat64(ctx, 1));
    JS_SetPropertyStr(ctx, obj, "font",        JS_NewString(ctx, "10px sans-serif"));
    return obj;
}

static JSValue
nd_element_toDataURL(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewString(ctx, "data:,");
}

static JSValue
nd_element_dispatchEvent(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el || !g_active_js || argc < 1) return JS_FALSE;
    JSValue type_v = JS_GetPropertyStr(ctx, argv[0], "type");
    const char *type = JS_ToCString(ctx, type_v);
    JS_FreeValue(ctx, type_v);
    gboolean prevented = FALSE;
    if (type) nd_js_dispatch_event(g_active_js, el, type, &prevented);
    if (type) JS_FreeCString(ctx, type);
    return prevented ? JS_FALSE : JS_TRUE;
}

static const JSCFunctionListEntry nd_element_proto_funcs[] = {
    JS_CGETSET_DEF("tagName",                nd_element_get_tagName,                NULL),
    JS_CGETSET_DEF("localName",              nd_element_get_localName,              NULL),
    JS_CGETSET_DEF("textContent",            nd_element_get_textContent,            nd_element_set_textContent),
    JS_CGETSET_DEF("innerText",              nd_element_get_textContent,            nd_element_set_textContent),
    JS_CGETSET_DEF("outerText",              nd_element_get_textContent,            nd_element_set_textContent),
    JS_CGETSET_DEF("id",                     nd_element_get_id,                     nd_element_set_id),
    JS_CGETSET_DEF("className",              nd_element_get_className,              nd_element_set_className),
    JS_CGETSET_DEF("innerHTML",              nd_element_get_innerHTML,              nd_element_set_innerHTML),
    JS_CGETSET_DEF("outerHTML",              nd_element_get_outerHTML,              nd_element_set_outerHTML),
    JS_CGETSET_DEF("style",                  nd_element_get_style,                  NULL),
    JS_CGETSET_DEF("classList",              nd_element_get_classList,              NULL),
    JS_CGETSET_DEF("parentElement",          nd_element_get_parentElement,          NULL),
    JS_CGETSET_DEF("parentNode",             nd_element_get_parentNode,             NULL),
    JS_CGETSET_DEF("firstElementChild",      nd_element_get_firstElementChild,      NULL),
    JS_CGETSET_DEF("lastElementChild",       nd_element_get_lastElementChild,       NULL),
    JS_CGETSET_DEF("nextElementSibling",     nd_element_get_nextElementSibling,     NULL),
    JS_CGETSET_DEF("previousElementSibling", nd_element_get_previousElementSibling, NULL),
    JS_CGETSET_DEF("firstChild",             nd_element_get_firstChild,             NULL),
    JS_CGETSET_DEF("lastChild",              nd_element_get_lastChild,              NULL),
    JS_CGETSET_DEF("nextSibling",            nd_element_get_nextSibling,            NULL),
    JS_CGETSET_DEF("previousSibling",        nd_element_get_previousSibling,        NULL),
    JS_CGETSET_DEF("childNodes",             nd_element_get_childNodes,             NULL),
    JS_CGETSET_DEF("childElementCount",      nd_element_get_childElementCount,      NULL),
    JS_CGETSET_DEF("children",               nd_element_get_children,               NULL),
    JS_CFUNC_DEF("getAttribute",            1, nd_element_getAttribute),
    JS_CFUNC_DEF("hasAttribute",            1, nd_element_hasAttribute),
    JS_CFUNC_DEF("setAttribute",            2, nd_element_setAttribute),
    JS_CFUNC_DEF("removeAttribute",         1, nd_element_removeAttribute),
    JS_CFUNC_DEF("toggleAttribute",         1, nd_element_toggleAttribute),
    JS_CFUNC_DEF("getAttributeNS",          2, nd_element_getAttributeNS),
    JS_CFUNC_DEF("hasAttributeNS",          2, nd_element_hasAttributeNS),
    JS_CFUNC_DEF("setAttributeNS",          3, nd_element_setAttributeNS),
    JS_CFUNC_DEF("removeAttributeNS",       2, nd_element_removeAttributeNS),
    JS_CFUNC_DEF("requestFullscreen",       0, nd_event_noop),
    JS_CFUNC_DEF("getAnimations",           0, nd_event_empty_array),
    JS_CFUNC_DEF("animate",                 2, nd_element_animate),
    JS_CFUNC_DEF("getRootNode",             1, nd_element_getRootNode),
    JS_CFUNC_DEF("isEqualNode",             1, nd_element_isEqualNode),
    JS_CFUNC_DEF("isSameNode",              1, nd_element_isEqualNode),
    JS_CFUNC_DEF("compareDocumentPosition", 1, nd_element_compareDocumentPosition),
    JS_CFUNC_DEF("lookupPrefix",            1, nd_element_lookupNamespaceURI),
    JS_CFUNC_DEF("lookupNamespaceURI",      1, nd_element_lookupNamespaceURI),
    JS_CFUNC_DEF("isDefaultNamespace",      1, nd_element_isDefaultNamespace),
    JS_CFUNC_DEF("getClientRects",          0, nd_element_getClientRects),
    JS_CFUNC_DEF("scrollBy",                2, nd_event_noop),
    JS_CFUNC_DEF("scrollTo",                2, nd_event_noop),
    JS_CFUNC_DEF("scroll",                  2, nd_event_noop),
    JS_CFUNC_DEF("scrollIntoViewIfNeeded",  1, nd_element_scrollIntoView),
    JS_CFUNC_DEF("requestPointerLock",      0, nd_event_noop),
    JS_CFUNC_DEF("releasePointerLock",      0, nd_event_noop),
    JS_CFUNC_DEF("releaseCapture",          0, nd_event_noop),
    JS_CFUNC_DEF("setCapture",              0, nd_event_noop),
    JS_CGETSET_DEF("length",            nd_element_get_text_length, NULL),
    JS_CFUNC_DEF("substringData", 2, nd_element_substring_data),
    JS_CFUNC_DEF("appendData",    1, nd_element_append_data),
    JS_CFUNC_DEF("deleteData",    2, nd_element_delete_data),
    JS_CFUNC_DEF("insertData",    2, nd_element_insert_data),
    JS_CFUNC_DEF("replaceData",   3, nd_element_replace_data),
    JS_CFUNC_DEF("splitText",     1, nd_element_split_text),
    JS_CFUNC_DEF("select",              0, nd_event_noop),
    JS_CFUNC_DEF("setSelectionRange",   3, nd_event_noop),
    JS_CFUNC_DEF("setRangeText",        1, nd_event_noop),
    JS_CFUNC_DEF("stepUp",              0, nd_event_noop),
    JS_CFUNC_DEF("stepDown",            0, nd_event_noop),
    JS_CFUNC_DEF("play",                0, nd_returns_resolved_undefined),
    JS_CFUNC_DEF("pause",               0, nd_event_noop),
    JS_CFUNC_DEF("load",                0, nd_event_noop),
    JS_CFUNC_DEF("canPlayType",         1, nd_event_noop),
    JS_CFUNC_DEF("fastSeek",            1, nd_event_noop),
    JS_CFUNC_DEF("addTextTrack",        3, nd_event_noop),
    JS_CGETSET_DEF("validity",          nd_element_get_validity,          NULL),
    JS_CGETSET_DEF("validationMessage", nd_element_get_validation_message, NULL),
    JS_CGETSET_DEF("willValidate",      nd_element_get_zero_int,          NULL),
    JS_CGETSET_DEF("labels",            nd_element_get_labels,            NULL),
    JS_CGETSET_DEF("files",             nd_element_get_null,              NULL),
    JS_CGETSET_DEF("indeterminate",     nd_element_get_zero_int,          NULL),
    JS_CGETSET_DEF("selectionStart",    nd_element_get_zero_int,          NULL),
    JS_CGETSET_DEF("selectionEnd",      nd_element_get_zero_int,          NULL),
    JS_CGETSET_DEF("selectionDirection", nd_element_get_selection_dir,    NULL),
    JS_CGETSET_DEF("defaultValue",      nd_element_get_default_value,     NULL),
    JS_CGETSET_DEF("defaultChecked",    nd_element_get_default_checked,   NULL),
    JS_CGETSET_DEF("defaultSelected",   nd_element_get_default_selected,  NULL),
    JS_CGETSET_DEF("currentTime",       nd_element_get_zero_int,          nd_element_scroll_int_set),
    JS_CGETSET_DEF("duration",          nd_element_get_zero_int,          NULL),
    JS_CGETSET_DEF("paused",            nd_element_get_true_prop,         NULL),
    JS_CGETSET_DEF("ended",             nd_element_get_zero_int,          NULL),
    JS_CGETSET_DEF("seeking",           nd_element_get_zero_int,          NULL),
    JS_CGETSET_DEF("volume",            nd_element_get_one_int,           NULL),
    JS_CGETSET_DEF("playbackRate",      nd_element_get_one_int,           NULL),
    JS_CGETSET_DEF("muted",             nd_element_get_zero_int,          NULL),
    JS_CGETSET_DEF("readyState",        nd_element_get_zero_int,          NULL),
    JS_CGETSET_DEF("networkState",      nd_element_get_zero_int,          NULL),
    JS_CGETSET_DEF("seekable",          nd_element_get_empty_array_prop,  NULL),
    JS_CGETSET_DEF("buffered",          nd_element_get_empty_array_prop,  NULL),
    JS_CGETSET_DEF("played",            nd_element_get_empty_array_prop,  NULL),
    JS_CGETSET_DEF("textTracks",        nd_element_get_empty_array_prop,  NULL),
    JS_CGETSET_DEF("videoTracks",       nd_element_get_empty_array_prop,  NULL),
    JS_CGETSET_DEF("audioTracks",       nd_element_get_empty_array_prop,  NULL),
    JS_CGETSET_DEF("valueAsNumber",     nd_element_get_value_as_number,   NULL),
    JS_CGETSET_DEF("valueAsDate",       nd_element_get_null,              NULL),
    JS_CGETSET_DEF("encoding",          nd_element_get_form_enctype,      NULL),
    JS_CGETSET_DEF("isContentEditable", nd_element_get_zero_int,          NULL),
    JS_CGETSET_DEF("translate",         nd_element_get_true_prop,         NULL),
    JS_CGETSET_DEF("offsetParent",      nd_element_get_null,              NULL),
    JS_CGETSET_DEF("videoWidth",        nd_element_get_zero_int,          NULL),
    JS_CGETSET_DEF("videoHeight",       nd_element_get_zero_int,          NULL),
    JS_CGETSET_DEF("clientInformation", nd_element_get_null,              NULL),
    JS_CFUNC_DEF("decode",            0, nd_returns_resolved_undefined),
    JS_CFUNC_DEF("toBlob",            1, nd_event_noop),
    JS_CFUNC_DEF("attachInternals",   0, nd_throws_unsupported),
    JS_CGETSET_DEF("index",           nd_element_get_option_index,   NULL),
    JS_CGETSET_DEF("rows",            nd_element_table_rows,         NULL),
    JS_CGETSET_DEF("caption",         nd_element_table_caption,      NULL),
    JS_CGETSET_DEF("tHead",           nd_element_table_thead,        NULL),
    JS_CGETSET_DEF("tFoot",           nd_element_table_tfoot,        NULL),
    JS_CGETSET_DEF("tBodies",         nd_element_table_tbodies,      NULL),
    JS_CGETSET_DEF("cells",           nd_element_tr_cells,           NULL),
    JS_CGETSET_DEF("rowIndex",        nd_element_get_zero_int,       NULL),
    JS_CGETSET_DEF("sectionRowIndex", nd_element_get_zero_int,       NULL),
    JS_CGETSET_DEF("cellIndex",       nd_element_get_zero_int,       NULL),
    JS_CGETSET_DEF("colSpan",         nd_element_get_one_int,        NULL),
    JS_CGETSET_DEF("rowSpan",         nd_element_get_one_int,        NULL),
    JS_CGETSET_DEF("returnValue",     nd_element_get_default_value,  NULL),
    JS_CFUNC_DEF("createCaption",  0, nd_event_noop),
    JS_CFUNC_DEF("createTHead",    0, nd_event_noop),
    JS_CFUNC_DEF("createTFoot",    0, nd_event_noop),
    JS_CFUNC_DEF("createTBody",    0, nd_event_noop),
    JS_CFUNC_DEF("deleteCaption",  0, nd_event_noop),
    JS_CFUNC_DEF("deleteTHead",    0, nd_event_noop),
    JS_CFUNC_DEF("deleteTFoot",    0, nd_event_noop),
    JS_CFUNC_DEF("insertRow",      1, nd_event_noop),
    JS_CFUNC_DEF("deleteRow",      1, nd_event_noop),
    JS_CFUNC_DEF("insertCell",     1, nd_event_noop),
    JS_CFUNC_DEF("deleteCell",     1, nd_event_noop),
    JS_CFUNC_DEF("namedItem",      1, nd_event_noop),
    JS_CFUNC_DEF("item",           1, nd_event_noop),
    JS_CFUNC_DEF("add",            2, nd_event_noop),
    JS_CFUNC_DEF("appendChild",             1, nd_element_appendChild),
    JS_CFUNC_DEF("removeChild",             1, nd_element_removeChild),
    JS_CFUNC_DEF("insertBefore",            2, nd_element_insertBefore),
    JS_CFUNC_DEF("replaceChild",            2, nd_element_replaceChild),
    JS_CFUNC_DEF("insertAdjacentHTML",      2, nd_element_insertAdjacentHTML),
    JS_CFUNC_DEF("replaceChildren",         0, nd_element_replaceChildren),
    JS_CFUNC_DEF("getAttributeNames",       0, nd_element_getAttributeNames),
    JS_CFUNC_DEF("remove",                  0, nd_element_remove_self),
    JS_CFUNC_DEF("cloneNode",               1, nd_element_cloneNode),
    JS_CFUNC_DEF("normalize",               0, nd_element_normalize),
    JS_CFUNC_DEF("append",                  0, nd_element_append),
    JS_CFUNC_DEF("prepend",                 0, nd_element_prepend),
    JS_CFUNC_DEF("before",                  0, nd_element_before),
    JS_CFUNC_DEF("after",                   0, nd_element_after),
    JS_CFUNC_DEF("replaceWith",             0, nd_element_replaceWith),
    JS_CFUNC_DEF("addEventListener",        2, nd_element_addEventListener),
    JS_CFUNC_DEF("removeEventListener",     2, nd_element_removeEventListener),    JS_CFUNC_DEF("getElementsByTagName",    1, nd_element_getElementsByTagName),
    JS_CFUNC_DEF("getElementsByClassName",  1, nd_element_getElementsByClassName),
    JS_CFUNC_DEF("querySelector",           1, nd_element_querySelector),
    JS_CFUNC_DEF("querySelectorAll",        1, nd_element_querySelectorAll),
    JS_CFUNC_DEF("matches",                 1, nd_element_matches),
    JS_CFUNC_DEF("closest",                 1, nd_element_closest),
    JS_CFUNC_DEF("contains",                1, nd_element_contains),
    JS_CFUNC_DEF("hasChildNodes",           0, nd_element_hasChildNodes),
    JS_CFUNC_DEF("getBoundingClientRect",   0, nd_element_getBoundingClientRect),
    JS_CFUNC_DEF("focus",                   0, nd_element_focus),
    JS_CFUNC_DEF("blur",                    0, nd_element_focus),
    JS_CFUNC_DEF("click",                   0, nd_element_click),
    JS_CFUNC_DEF("submit",                  0, nd_element_form_submit),
    JS_CFUNC_DEF("requestSubmit",           0, nd_element_form_submit),
    JS_CFUNC_DEF("reset",                   0, nd_element_form_reset),
    JS_CFUNC_DEF("checkValidity",           0, nd_event_true),
    JS_CFUNC_DEF("reportValidity",          0, nd_event_true),
    JS_CFUNC_DEF("setCustomValidity",       1, nd_event_noop),
    JS_CFUNC_DEF("scrollIntoView",          0, nd_element_scrollIntoView),
    JS_CFUNC_DEF("show",                    0, nd_element_show),
    JS_CFUNC_DEF("showModal",               0, nd_element_show),
    JS_CFUNC_DEF("close",                   0, nd_element_close),
    JS_CFUNC_DEF("dispatchEvent",           1, nd_element_dispatchEvent),
    JS_CFUNC_DEF("getContext",              1, nd_element_getContext),
    JS_CFUNC_DEF("toDataURL",               0, nd_element_toDataURL),
    JS_CGETSET_DEF("nodeType",      nd_element_get_nodeType, NULL),
    JS_CGETSET_DEF("nodeValue",     nd_element_get_nodeValue, nd_element_set_nodeValue),
    JS_CGETSET_DEF("data",          nd_element_get_nodeValue, nd_element_set_nodeValue),
    JS_CGETSET_DEF("nodeName",      nd_element_get_nodeName, NULL),
    JS_CGETSET_DEF("dataset",       nd_element_get_dataset,  NULL),
    JS_CGETSET_DEF("offsetTop",     nd_element_get_zero_int, NULL),
    JS_CGETSET_DEF("offsetLeft",    nd_element_get_zero_int, NULL),
    JS_CGETSET_DEF("offsetWidth",   nd_element_get_zero_int, NULL),
    JS_CGETSET_DEF("offsetHeight",  nd_element_get_zero_int, NULL),
    JS_CGETSET_DEF("clientTop",     nd_element_get_zero_int, NULL),
    JS_CGETSET_DEF("clientLeft",    nd_element_get_zero_int, NULL),
    JS_CGETSET_DEF("clientWidth",   nd_element_get_zero_int, NULL),
    JS_CGETSET_DEF("clientHeight",  nd_element_get_zero_int, NULL),
    JS_CGETSET_DEF("scrollTop",     nd_element_get_zero_int, nd_element_scroll_int_set),
    JS_CGETSET_DEF("scrollLeft",    nd_element_get_zero_int, nd_element_scroll_int_set),
    JS_CGETSET_DEF("scrollWidth",   nd_element_get_zero_int, NULL),
    JS_CGETSET_DEF("scrollHeight",  nd_element_get_zero_int, NULL),
    JS_CGETSET_DEF("attributes",    nd_element_get_attributes, NULL),
    JS_CGETSET_DEF("naturalWidth",  nd_element_get_zero_int, NULL),
    JS_CGETSET_DEF("naturalHeight", nd_element_get_zero_int, NULL),
    JS_CGETSET_DEF("complete",      nd_element_get_zero_int, NULL),
    JS_CGETSET_DEF("hidden",        nd_element_get_hidden,     nd_element_set_hidden),
    JS_CGETSET_MAGIC_DEF("title",       nd_element_attr_getter, nd_element_attr_setter, 0),
    JS_CGETSET_MAGIC_DEF("name",        nd_element_attr_getter, nd_element_attr_setter, 1),
    JS_CGETSET_MAGIC_DEF("alt",         nd_element_attr_getter, nd_element_attr_setter, 2),
    JS_CGETSET_MAGIC_DEF("src",         nd_element_attr_getter, nd_element_attr_setter, 3),
    JS_CGETSET_MAGIC_DEF("href",        nd_element_attr_getter, nd_element_attr_setter, 4),
    JS_CGETSET_MAGIC_DEF("type",        nd_element_attr_getter, nd_element_attr_setter, 5),
    JS_CGETSET_MAGIC_DEF("placeholder", nd_element_attr_getter, nd_element_attr_setter, 6),
    JS_CGETSET_MAGIC_DEF("lang",        nd_element_attr_getter, nd_element_attr_setter, 7),
    JS_CGETSET_MAGIC_DEF("dir",         nd_element_attr_getter, nd_element_attr_setter,  8),
    JS_CGETSET_MAGIC_DEF("action",      nd_element_attr_getter, nd_element_attr_setter,  9),
    JS_CGETSET_MAGIC_DEF("method",      nd_element_attr_getter, nd_element_attr_setter, 10),
    JS_CGETSET_MAGIC_DEF("enctype",     nd_element_attr_getter, nd_element_attr_setter, 11),
    JS_CGETSET_MAGIC_DEF("target",      nd_element_attr_getter, nd_element_attr_setter, 12),
    JS_CGETSET_MAGIC_DEF("rel",         nd_element_attr_getter, nd_element_attr_setter, 13),
    JS_CGETSET_MAGIC_DEF("accept",      nd_element_attr_getter, nd_element_attr_setter, 14),
    JS_CGETSET_MAGIC_DEF("acceptCharset", nd_element_attr_getter, nd_element_attr_setter, 15),
    JS_CGETSET_MAGIC_DEF("autocomplete", nd_element_attr_getter, nd_element_attr_setter, 16),
    JS_CGETSET_MAGIC_DEF("list",        nd_element_attr_getter, nd_element_attr_setter, 17),
    JS_CGETSET_MAGIC_DEF("min",         nd_element_attr_getter, nd_element_attr_setter, 18),
    JS_CGETSET_MAGIC_DEF("max",         nd_element_attr_getter, nd_element_attr_setter, 19),
    JS_CGETSET_MAGIC_DEF("step",        nd_element_attr_getter, nd_element_attr_setter, 20),
    JS_CGETSET_MAGIC_DEF("pattern",     nd_element_attr_getter, nd_element_attr_setter, 21),
    JS_CGETSET_MAGIC_DEF("spellcheck",  nd_element_attr_getter, nd_element_attr_setter, 22),
    JS_CGETSET_MAGIC_DEF("crossOrigin",    nd_element_attr_getter, nd_element_attr_setter, 23),
    JS_CGETSET_MAGIC_DEF("referrerPolicy", nd_element_attr_getter, nd_element_attr_setter, 24),
    JS_CGETSET_MAGIC_DEF("decoding",       nd_element_attr_getter, nd_element_attr_setter, 25),
    JS_CGETSET_MAGIC_DEF("loading",        nd_element_attr_getter, nd_element_attr_setter, 26),
    JS_CGETSET_MAGIC_DEF("fetchPriority",  nd_element_attr_getter, nd_element_attr_setter, 27),
    JS_CGETSET_MAGIC_DEF("sizes",          nd_element_attr_getter, nd_element_attr_setter, 28),
    JS_CGETSET_MAGIC_DEF("srcset",         nd_element_attr_getter, nd_element_attr_setter, 29),
    JS_CGETSET_MAGIC_DEF("useMap",         nd_element_attr_getter, nd_element_attr_setter, 30),
    JS_CGETSET_MAGIC_DEF("inputMode",      nd_element_attr_getter, nd_element_attr_setter, 31),
    JS_CGETSET_MAGIC_DEF("size",           nd_element_attr_getter, nd_element_attr_setter, 32),
    JS_CGETSET_MAGIC_DEF("cols",           nd_element_attr_getter, nd_element_attr_setter, 33),
    JS_CGETSET_MAGIC_DEF("rows",           nd_element_attr_getter, nd_element_attr_setter, 34),
    JS_CGETSET_MAGIC_DEF("maxLength",      nd_element_attr_getter, nd_element_attr_setter, 35),
    JS_CGETSET_MAGIC_DEF("minLength",      nd_element_attr_getter, nd_element_attr_setter, 36),
    JS_CGETSET_MAGIC_DEF("coords",         nd_element_attr_getter, nd_element_attr_setter, 37),
    JS_CGETSET_MAGIC_DEF("shape",          nd_element_attr_getter, nd_element_attr_setter, 38),
    JS_CGETSET_MAGIC_DEF("formAction",     nd_element_attr_getter, nd_element_attr_setter, 40),
    JS_CGETSET_MAGIC_DEF("formMethod",     nd_element_attr_getter, nd_element_attr_setter, 41),
    JS_CGETSET_MAGIC_DEF("formEnctype",    nd_element_attr_getter, nd_element_attr_setter, 42),
    JS_CGETSET_MAGIC_DEF("formTarget",     nd_element_attr_getter, nd_element_attr_setter, 43),
    JS_CGETSET_MAGIC_DEF("integrity",      nd_element_attr_getter, nd_element_attr_setter, 44),
    JS_CGETSET_MAGIC_DEF("kind",           nd_element_attr_getter, nd_element_attr_setter, 45),
    JS_CGETSET_MAGIC_DEF("hreflang",       nd_element_attr_getter, nd_element_attr_setter, 47),
    JS_CGETSET_MAGIC_DEF("content",        nd_element_attr_getter, nd_element_attr_setter, 49),
    JS_CGETSET_MAGIC_DEF("httpEquiv",      nd_element_attr_getter, nd_element_attr_setter, 50),
    JS_CGETSET_MAGIC_DEF("contentEditable", nd_element_attr_getter, nd_element_attr_setter, 51),
    JS_CGETSET_MAGIC_DEF("slot",           nd_element_attr_getter, nd_element_attr_setter, 52),
    JS_CGETSET_MAGIC_DEF("role",           nd_element_attr_getter, nd_element_attr_setter, 54),
    JS_CGETSET_MAGIC_DEF("ariaLabel",      nd_element_attr_getter, nd_element_attr_setter, 55),
    JS_CGETSET_MAGIC_DEF("ariaHidden",     nd_element_attr_getter, nd_element_attr_setter, 56),
    JS_CGETSET_MAGIC_DEF("ariaDisabled",   nd_element_attr_getter, nd_element_attr_setter, 57),
    JS_CGETSET_MAGIC_DEF("ariaPressed",    nd_element_attr_getter, nd_element_attr_setter, 58),
    JS_CGETSET_MAGIC_DEF("ariaExpanded",   nd_element_attr_getter, nd_element_attr_setter, 59),
    JS_CGETSET_MAGIC_DEF("ariaControls",   nd_element_attr_getter, nd_element_attr_setter, 60),
    JS_CGETSET_MAGIC_DEF("ariaDescribedBy", nd_element_attr_getter, nd_element_attr_setter, 61),
    JS_CGETSET_MAGIC_DEF("ariaLabelledBy", nd_element_attr_getter, nd_element_attr_setter, 62),
    JS_CGETSET_MAGIC_DEF("ariaLive",       nd_element_attr_getter, nd_element_attr_setter, 63),
    JS_CGETSET_MAGIC_DEF("ariaBusy",       nd_element_attr_getter, nd_element_attr_setter, 64),
    JS_CGETSET_MAGIC_DEF("ariaChecked",    nd_element_attr_getter, nd_element_attr_setter, 65),
    JS_CGETSET_MAGIC_DEF("ariaCurrent",    nd_element_attr_getter, nd_element_attr_setter, 66),
    JS_CGETSET_MAGIC_DEF("ariaSelected",   nd_element_attr_getter, nd_element_attr_setter, 67),
    JS_CGETSET_MAGIC_DEF("open",        nd_element_boolattr_getter, nd_element_boolattr_setter,  0),
    JS_CGETSET_MAGIC_DEF("selected",    nd_element_boolattr_getter, nd_element_boolattr_setter,  1),
    JS_CGETSET_MAGIC_DEF("multiple",    nd_element_boolattr_getter, nd_element_boolattr_setter,  2),
    JS_CGETSET_MAGIC_DEF("readOnly",    nd_element_boolattr_getter, nd_element_boolattr_setter,  3),
    JS_CGETSET_MAGIC_DEF("autofocus",   nd_element_boolattr_getter, nd_element_boolattr_setter,  4),
    JS_CGETSET_MAGIC_DEF("controls",    nd_element_boolattr_getter, nd_element_boolattr_setter,  5),
    JS_CGETSET_MAGIC_DEF("loop",        nd_element_boolattr_getter, nd_element_boolattr_setter,  6),
    JS_CGETSET_MAGIC_DEF("muted",       nd_element_boolattr_getter, nd_element_boolattr_setter,  7),
    JS_CGETSET_MAGIC_DEF("autoplay",    nd_element_boolattr_getter, nd_element_boolattr_setter,  8),
    JS_CGETSET_MAGIC_DEF("defer",       nd_element_boolattr_getter, nd_element_boolattr_setter,  9),
    JS_CGETSET_MAGIC_DEF("async",       nd_element_boolattr_getter, nd_element_boolattr_setter, 10),
    JS_CGETSET_MAGIC_DEF("noValidate",  nd_element_boolattr_getter, nd_element_boolattr_setter, 11),
    JS_CGETSET_MAGIC_DEF("isMap",       nd_element_boolattr_getter, nd_element_boolattr_setter, 12),
    JS_CGETSET_MAGIC_DEF("draggable",   nd_element_boolattr_getter, nd_element_boolattr_setter, 13),
    JS_CGETSET_MAGIC_DEF("reversed",    nd_element_boolattr_getter, nd_element_boolattr_setter, 14),
    JS_CGETSET_MAGIC_DEF("playsInline", nd_element_boolattr_getter, nd_element_boolattr_setter, 15),
    JS_CGETSET_MAGIC_DEF("inert",       nd_element_boolattr_getter, nd_element_boolattr_setter, 17),
    JS_CGETSET_MAGIC_DEF("noModule",    nd_element_boolattr_getter, nd_element_boolattr_setter, 18),
    JS_CGETSET_MAGIC_DEF("formNoValidate", nd_element_boolattr_getter, nd_element_boolattr_setter, 19),
    JS_CGETSET_DEF("htmlFor",                nd_element_get_htmlFor, nd_element_set_htmlFor),
    JS_CGETSET_DEF("tabIndex",               nd_element_get_tabIndex, nd_element_set_tabIndex),
    JS_CGETSET_DEF("isConnected",            nd_element_get_isConnected,    NULL),
    JS_CGETSET_DEF("ownerDocument",          nd_element_get_ownerDocument,  NULL),
    JS_CGETSET_DEF("namespaceURI",           nd_element_get_namespaceURI,   NULL),
    JS_CGETSET_DEF("shadowRoot",             nd_element_get_null,           NULL),
    JS_CFUNC_DEF("attachShadow",             1, nd_element_attachShadow),
    JS_CGETSET_DEF("disabled",      nd_element_get_disabled,   nd_element_set_disabled),
    JS_CGETSET_DEF("checked",       nd_element_get_checked,    nd_element_set_checked),
    JS_CGETSET_DEF("value",         nd_element_get_value_prop, nd_element_set_value_prop),
    JS_CGETSET_DEF("selectedIndex", nd_element_get_selectedIndex, NULL),
    JS_CGETSET_DEF("options",       nd_element_get_options,       NULL),
    JS_CGETSET_DEF("elements",      nd_element_get_form_elements, NULL),
    JS_CGETSET_DEF("form",          nd_element_get_form,          NULL),
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

static JSValue
nd_document_get_head(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!g_active_js || !g_active_js->current_doc) return JS_NULL;
    nd_node *head = nd_node_find_first_element(g_active_js->current_doc, "head");
    return nd_make_element(ctx, head);
}

static JSValue
nd_document_get_activeElement(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!g_active_js || !g_active_js->current_doc) return JS_NULL;
    nd_node *body = nd_node_find_first_element(g_active_js->current_doc, "body");
    return nd_make_element(ctx, body);
}

static JSValue
nd_document_collect_by_tag(JSContext *ctx, const char *tag)
{
    JSValue arr = JS_NewArray(ctx);
    if (!g_active_js || !g_active_js->current_doc) return arr;
    uint32_t idx = 0;
    GQueue q = G_QUEUE_INIT;
    g_queue_push_tail(&q, g_active_js->current_doc);
    while (!g_queue_is_empty(&q)) {
        nd_node *n = g_queue_pop_head(&q);
        for (nd_node *c = n->first_child; c; c = c->next_sibling) {
            if (c->kind == ND_NODE_ELEMENT && c->name &&
                g_ascii_strcasecmp(c->name, tag) == 0)
                JS_SetPropertyUint32(ctx, arr, idx++, nd_make_element(ctx, c));
            g_queue_push_tail(&q, c);
        }
    }
    g_queue_clear(&q);
    return arr;
}

static JSValue
nd_document_get_forms(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return nd_document_collect_by_tag(ctx, "form");
}

static JSValue
nd_document_get_images(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return nd_document_collect_by_tag(ctx, "img");
}

static JSValue
nd_document_get_scripts(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return nd_document_collect_by_tag(ctx, "script");
}

static JSValue
nd_document_get_styleSheets(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewArray(ctx);
}

static JSValue
nd_document_get_embeds(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewArray(ctx);
}

static JSValue
nd_document_get_plugins(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewArray(ctx);
}

static JSValue
nd_document_get_designMode(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewString(ctx, "off");
}

static JSValue
nd_document_get_lastModified(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewString(ctx, "");
}

static JSValue
nd_document_get_all(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return nd_document_collect_by_tag(ctx, "*");
}

static JSValue
nd_document_get_anchors(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    JSValue arr = JS_NewArray(ctx);
    if (!g_active_js || !g_active_js->current_doc) return arr;
    uint32_t idx = 0;
    GQueue q = G_QUEUE_INIT;
    g_queue_push_tail(&q, g_active_js->current_doc);
    while (!g_queue_is_empty(&q)) {
        nd_node *n = g_queue_pop_head(&q);
        for (nd_node *c = n->first_child; c; c = c->next_sibling) {
            if (c->kind == ND_NODE_ELEMENT && c->name &&
                g_ascii_strcasecmp(c->name, "a") == 0 &&
                nd_element_get_attr(c, "name"))
                JS_SetPropertyUint32(ctx, arr, idx++, nd_make_element(ctx, c));
            g_queue_push_tail(&q, c);
        }
    }
    g_queue_clear(&q);
    return arr;
}

static JSValue
nd_document_get_applets(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewArray(ctx);
}

static JSValue
nd_document_get_fonts(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    JSValue fs = JS_NewObject(ctx);
    JSValue resolvers[2];
    JSValue ready = JS_NewPromiseCapability(ctx, resolvers);
    JS_Call(ctx, resolvers[0], JS_UNDEFINED, 1, (JSValueConst[]){fs});
    JS_FreeValue(ctx, resolvers[0]);
    JS_FreeValue(ctx, resolvers[1]);
    JS_SetPropertyStr(ctx, fs, "ready",  ready);
    JS_SetPropertyStr(ctx, fs, "status", JS_NewString(ctx, "loaded"));
    nd_bind_fn(ctx, fs, "check", nd_event_true,                    1);
    nd_bind_fn(ctx, fs, "load",  nd_returns_resolved_undefined,    2);
    nd_bind_fn(ctx, fs, "add",   nd_event_noop,                    1);
    nd_bind_fn(ctx, fs, "delete",  nd_event_noop, 1);
    nd_bind_fn(ctx, fs, "clear",   nd_event_noop, 0);
    nd_bind_fn(ctx, fs, "forEach", nd_event_noop, 1);
    JS_SetPropertyStr(ctx, fs, "size", JS_NewInt32(ctx, 0));
    return fs;
}

static JSValue
nd_document_has_focus(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_TRUE;
}

static JSValue
nd_document_element_from_point(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (!g_active_js || !g_active_js->current_doc) return JS_NULL;
    nd_node *body = nd_node_find_first_element(g_active_js->current_doc, "body");
    return nd_make_element(ctx, body);
}

static JSValue
nd_document_elements_from_point(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue arr = JS_NewArray(ctx);
    if (!g_active_js || !g_active_js->current_doc) return arr;
    nd_node *body = nd_node_find_first_element(g_active_js->current_doc, "body");
    if (body) JS_SetPropertyUint32(ctx, arr, 0, nd_make_element(ctx, body));
    return arr;
}

static JSValue
nd_document_create_range(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "collapsed", JS_TRUE);
    JS_SetPropertyStr(ctx, r, "startContainer", JS_NULL);
    JS_SetPropertyStr(ctx, r, "endContainer",   JS_NULL);
    JS_SetPropertyStr(ctx, r, "startOffset",    JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, r, "endOffset",      JS_NewInt32(ctx, 0));
    static const char *methods[] = {
        "setStart","setEnd","setStartBefore","setStartAfter",
        "setEndBefore","setEndAfter","selectNode","selectNodeContents",
        "collapse","cloneContents","cloneRange","deleteContents",
        "extractContents","insertNode","surroundContents","toString",
        "detach","compareBoundaryPoints","intersectsNode","isPointInRange",
        "comparePoint","createContextualFragment","getBoundingClientRect",
        "getClientRects",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(methods); i++)
        JS_SetPropertyStr(ctx, r, methods[i],
            JS_NewCFunction(ctx, nd_event_noop, methods[i], 0));
    return r;
}

static JSValue
nd_document_create_tree_walker(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc;
    JSValue tw = JS_NewObject(ctx);
    if (argc >= 1)
        JS_SetPropertyStr(ctx, tw, "root", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, tw, "currentNode",
                      argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_NULL);
    JS_SetPropertyStr(ctx, tw, "whatToShow",
                      argc >= 2 ? JS_DupValue(ctx, argv[1]) : JS_NewInt32(ctx, -1));
    static const char *methods[] = {
        "nextNode","previousNode","parentNode","firstChild",
        "lastChild","previousSibling","nextSibling",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(methods); i++)
        JS_SetPropertyStr(ctx, tw, methods[i],
            JS_NewCFunction(ctx, nd_event_noop, methods[i], 0));
    return tw;
}

static JSValue
nd_document_implementation(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    JSValue impl = JS_NewObject(ctx);
    nd_bind_fn(ctx, impl, "hasFeature",          nd_event_true, 2);
    nd_bind_fn(ctx, impl, "createHTMLDocument",  nd_event_noop, 1);
    nd_bind_fn(ctx, impl, "createDocument",      nd_event_noop, 3);
    nd_bind_fn(ctx, impl, "createDocumentType",  nd_event_noop, 3);
    return impl;
}

static JSValue
nd_document_adopt_node(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NULL;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue
nd_document_import_node(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NULL;
    nd_node *src = nd_unwrap_element_mut(argv[0]);
    if (!src) return JS_NULL;
    gboolean deep = (argc >= 2) ? (JS_ToBool(ctx, argv[1]) ? TRUE : FALSE) : FALSE;
    nd_node *copy = nd_node_clone(src, deep);
    if (!copy) return JS_NULL;
    if (g_active_js) g_ptr_array_add(g_active_js->orphan_nodes, copy);
    (void)this_val;
    return nd_make_element(ctx, copy);
}

static JSValue
nd_document_get_links(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    JSValue arr = JS_NewArray(ctx);
    if (!g_active_js || !g_active_js->current_doc) return arr;
    uint32_t idx = 0;
    GQueue q = G_QUEUE_INIT;
    g_queue_push_tail(&q, g_active_js->current_doc);
    while (!g_queue_is_empty(&q)) {
        nd_node *n = g_queue_pop_head(&q);
        for (nd_node *c = n->first_child; c; c = c->next_sibling) {
            if (c->kind == ND_NODE_ELEMENT && c->name &&
                (g_ascii_strcasecmp(c->name, "a") == 0 ||
                 g_ascii_strcasecmp(c->name, "area") == 0) &&
                nd_element_get_attr(c, "href"))
                JS_SetPropertyUint32(ctx, arr, idx++, nd_make_element(ctx, c));
            g_queue_push_tail(&q, c);
        }
    }
    g_queue_clear(&q);
    return arr;
}

static void
nd_js_emit(nd_js *js, const char *prefix, JSContext *ctx, int argc, JSValueConst *argv)
{
    if (!js || !js->log_cb) return;
    GString *out = g_string_new(prefix);
    for (int i = 0; i < argc; i++) {
        if (i > 0 || (prefix && *prefix)) g_string_append_c(out, ' ');
        if (JS_IsObject(argv[i]) && !JS_IsFunction(ctx, argv[i])) {
            JSValue json = JS_JSONStringify(ctx, argv[i], JS_UNDEFINED, JS_UNDEFINED);
            if (!JS_IsException(json) && !JS_IsUndefined(json)) {
                const char *s = JS_ToCString(ctx, json);
                if (s) {
                    g_string_append(out, s);
                    JS_FreeCString(ctx, s);
                }
            } else {
                const char *s = JS_ToCString(ctx, argv[i]);
                if (s) { g_string_append(out, s); JS_FreeCString(ctx, s); }
                if (JS_IsException(json)) {
                    JSValue ex = JS_GetException(ctx);
                    JS_FreeValue(ctx, ex);
                }
            }
            JS_FreeValue(ctx, json);
        } else {
            const char *s = JS_ToCString(ctx, argv[i]);
            if (s) { g_string_append(out, s); JS_FreeCString(ctx, s); }
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
    if (js->rt) {
        const nd_config *c = nd_config_get();
        int mb = c ? c->js_memory_cap_mb : 128;
        if (mb <= 0) mb = 128;
        JS_SetInterruptHandler(js->rt, nd_js_interrupt_cb, js);
        JS_SetMemoryLimit(js->rt, (size_t)mb * 1024 * 1024);
    }
    if (!js->rt) { g_free(js); return NULL; }
    js->ctx = JS_NewContext(js->rt);
    if (!js->ctx) { JS_FreeRuntime(js->rt); g_free(js); return NULL; }
    JSContext *ctx = js->ctx;
    js->log_cb = log_cb;
    js->log_user_data = log_user_data;
    js->mut_cb = mut_cb;
    js->mut_user_data = mut_user_data;
    js->nav_cb = nav_cb;
    js->nav_user_data = nav_user_data;
    js->scroll_to_cb = NULL;
    js->scroll_to_user_data = NULL;
    js->form_submit_cb = NULL;
    js->form_submit_user_data = NULL;
    js->timers = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                       NULL, nd_timer_free);
    js->orphan_nodes = g_ptr_array_new();
    js->listeners    = g_ptr_array_new();
    js->local_storage   = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    js->session_storage = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    {
        const nd_config *c = nd_config_get();
        js->local_storage_disabled = c ? !c->local_storage_enabled : FALSE;
    }

    if (!nd_element_class_id)
        JS_NewClassID(js->rt, &nd_element_class_id);
    JS_NewClass(js->rt, nd_element_class_id, &nd_element_class);
    JSValue element_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, element_proto, nd_element_proto_funcs,
                               G_N_ELEMENTS(nd_element_proto_funcs));
    JS_SetClassProto(ctx, nd_element_class_id, element_proto);

    if (!nd_style_class_id)
        JS_NewClassID(js->rt, &nd_style_class_id);
    JS_NewClass(js->rt, nd_style_class_id, &nd_style_class);
    JSValue style_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, style_proto, nd_style_proto_funcs,
                               G_N_ELEMENTS(nd_style_proto_funcs));
    JS_SetClassProto(ctx, nd_style_class_id, style_proto);

    if (!nd_token_list_class_id)
        JS_NewClassID(js->rt, &nd_token_list_class_id);
    JS_NewClass(js->rt, nd_token_list_class_id, &nd_token_list_class);
    JSValue tlist_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, tlist_proto, nd_tlist_proto_funcs,
                               G_N_ELEMENTS(nd_tlist_proto_funcs));
    JS_SetClassProto(ctx, nd_token_list_class_id, tlist_proto);

    if (!nd_storage_class_id)
        JS_NewClassID(js->rt, &nd_storage_class_id);
    JS_NewClass(js->rt, nd_storage_class_id, &nd_storage_class);
    JSValue storage_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, storage_proto, nd_storage_proto_funcs,
                               G_N_ELEMENTS(nd_storage_proto_funcs));
    JS_SetClassProto(ctx, nd_storage_class_id, storage_proto);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    static const nd_fn_def console_log_methods[] = {
        { "log", 1 }, { "info", 1 }, { "debug", 1 }, { "trace", 1 },
        { "table", 1 }, { "group", 1 }, { "groupCollapsed", 1 },
        { "timeEnd", 1 }, { "timeLog", 1 }, { "count", 1 },
        { "assert", 2 }, { "dir", 1 }, { "dirxml", 1 },
    };
    static const nd_fn_def console_noop_methods[] = {
        { "groupEnd", 0 }, { "profile", 1 }, { "profileEnd", 1 },
        { "timeStamp", 1 }, { "context", 1 }, { "time", 1 },
        { "countReset", 1 }, { "clear", 0 },
    };
    nd_bind_fns(ctx, console, nd_js_console_log,
                console_log_methods, G_N_ELEMENTS(console_log_methods));
    nd_bind_fn(ctx,  console, "warn",  nd_js_console_warn,  1);
    nd_bind_fn(ctx,  console, "error", nd_js_console_error, 1);
    nd_bind_fns(ctx, console, nd_event_noop,
                console_noop_methods, G_N_ELEMENTS(console_noop_methods));
    JS_SetPropertyStr(ctx, console, "memory", JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, global, "console", console);

    nd_bind_fn(ctx, global, "alert",         nd_js_alert,             1);
    nd_bind_fn(ctx, global, "setTimeout",    nd_js_setTimeout_wrap,   2);
    nd_bind_fn(ctx, global, "setInterval",   nd_js_setInterval_wrap,  2);
    nd_bind_fn(ctx, global, "clearTimeout",  nd_js_clearTimer,        1);
    nd_bind_fn(ctx, global, "clearInterval", nd_js_clearTimer,        1);
    nd_bind_fn(ctx, global, "fetch",         nd_js_fetch,             1);

    JSValue navigator = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, navigator, "userAgent",
                      JS_NewString(ctx, "Nordstjernen/0.0.1"));
    JS_SetPropertyStr(ctx, navigator, "appName",
                      JS_NewString(ctx, "Nordstjernen"));
    JS_SetPropertyStr(ctx, navigator, "appVersion",
                      JS_NewString(ctx, "0.0.1"));
    JS_SetPropertyStr(ctx, navigator, "platform",
                      JS_NewString(ctx, "Linux x86_64"));
    JS_SetPropertyStr(ctx, navigator, "language",
                      JS_NewString(ctx, "en-US"));
    JSValue langs = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, langs, 0, JS_NewString(ctx, "en-US"));
    JS_SetPropertyUint32(ctx, langs, 1, JS_NewString(ctx, "en"));
    JS_SetPropertyStr(ctx, navigator, "languages", langs);
    JS_SetPropertyStr(ctx, navigator, "onLine", JS_TRUE);
    JS_SetPropertyStr(ctx, navigator, "doNotTrack",
                      JS_NewString(ctx, "1"));
    JS_SetPropertyStr(ctx, navigator, "cookieEnabled", JS_TRUE);
    JS_SetPropertyStr(ctx, navigator, "hardwareConcurrency",
                      JS_NewInt32(ctx, 4));
    JS_SetPropertyStr(ctx, navigator, "vendor",
                      JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, navigator, "product",
                      JS_NewString(ctx, "Gecko"));
    JS_SetPropertyStr(ctx, navigator, "productSub",
                      JS_NewString(ctx, "20030107"));
    JS_SetPropertyStr(ctx, navigator, "maxTouchPoints",
                      JS_NewInt32(ctx, 0));
    JSValue sw_stub = JS_NewObject(ctx);
    nd_bind_fn(ctx, sw_stub, "register",        nd_event_noop, 1);
    nd_bind_fn(ctx, sw_stub, "getRegistration", nd_event_noop, 0);
    JS_SetPropertyStr(ctx, sw_stub, "ready", JS_NULL);
    JS_SetPropertyStr(ctx, navigator, "serviceWorker", sw_stub);

    JS_SetPropertyStr(ctx, navigator, "deviceMemory",
                      JS_NewInt32(ctx, 4));
    JS_SetPropertyStr(ctx, navigator, "pdfViewerEnabled", JS_TRUE);
    JS_SetPropertyStr(ctx, navigator, "webdriver", JS_FALSE);

    JSValue connection = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, connection, "effectiveType",
                      JS_NewString(ctx, "4g"));
    JS_SetPropertyStr(ctx, connection, "type",
                      JS_NewString(ctx, "wifi"));
    JS_SetPropertyStr(ctx, connection, "downlink",
                      JS_NewFloat64(ctx, 10.0));
    JS_SetPropertyStr(ctx, connection, "downlinkMax",
                      JS_NewFloat64(ctx, 10.0));
    JS_SetPropertyStr(ctx, connection, "rtt",
                      JS_NewInt32(ctx, 50));
    JS_SetPropertyStr(ctx, connection, "saveData", JS_FALSE);
    nd_bind_fn(ctx, connection, "addEventListener",    nd_event_noop, 2);
    nd_bind_fn(ctx, connection, "removeEventListener", nd_event_noop, 2);
    JS_SetPropertyStr(ctx, navigator, "connection", connection);

    JSValue geolocation = JS_NewObject(ctx);
    nd_bind_fn(ctx, geolocation, "getCurrentPosition", nd_event_noop, 3);
    nd_bind_fn(ctx, geolocation, "watchPosition",      nd_event_noop, 3);
    nd_bind_fn(ctx, geolocation, "clearWatch",         nd_event_noop, 1);
    JS_SetPropertyStr(ctx, navigator, "geolocation", geolocation);

    JSValue clipboard = JS_NewObject(ctx);
    nd_bind_fn(ctx, clipboard, "writeText", nd_returns_rejected, 1);
    nd_bind_fn(ctx, clipboard, "readText",  nd_returns_rejected, 0);
    nd_bind_fn(ctx, clipboard, "write",     nd_returns_rejected, 1);
    nd_bind_fn(ctx, clipboard, "read",      nd_returns_rejected, 0);
    JS_SetPropertyStr(ctx, navigator, "clipboard", clipboard);

    JSValue permissions = JS_NewObject(ctx);
    nd_bind_fn(ctx, permissions, "query",   nd_returns_rejected, 1);
    nd_bind_fn(ctx, permissions, "request", nd_returns_rejected, 1);
    JS_SetPropertyStr(ctx, navigator, "permissions", permissions);

    JSValue media_devices = JS_NewObject(ctx);
    nd_bind_fn(ctx, media_devices, "getUserMedia",            nd_returns_rejected, 1);
    nd_bind_fn(ctx, media_devices, "getDisplayMedia",         nd_returns_rejected, 1);
    nd_bind_fn(ctx, media_devices, "enumerateDevices",        nd_returns_rejected, 0);
    nd_bind_fn(ctx, media_devices, "getSupportedConstraints", nd_event_noop,       0);
    JS_SetPropertyStr(ctx, navigator, "mediaDevices", media_devices);

    nd_bind_fn(ctx, navigator, "share",                     nd_returns_rejected, 1);
    nd_bind_fn(ctx, navigator, "canShare",                  nd_event_noop,       1);
    nd_bind_fn(ctx, navigator, "vibrate",                   nd_event_noop,       1);
    nd_bind_fn(ctx, navigator, "sendBeacon",                nd_event_noop,       2);
    nd_bind_fn(ctx, navigator, "registerProtocolHandler",   nd_event_noop,       2);
    nd_bind_fn(ctx, navigator, "unregisterProtocolHandler", nd_event_noop,       2);

    JSValue userAgentData = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, userAgentData, "brands", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, userAgentData, "mobile", JS_FALSE);
    JS_SetPropertyStr(ctx, userAgentData, "platform",
                      JS_NewString(ctx, "Linux"));
    nd_bind_fn(ctx, userAgentData, "getHighEntropyValues",
               nd_returns_resolved_undefined, 1);
    nd_bind_fn(ctx, userAgentData, "toJSON", nd_event_noop, 0);
    JS_SetPropertyStr(ctx, navigator, "userAgentData", userAgentData);

    JSValue plugins = JS_NewArray(ctx);
    JS_SetPropertyStr(ctx, plugins, "length", JS_NewInt32(ctx, 0));
    nd_bind_fn(ctx, plugins, "namedItem", nd_event_noop, 1);
    nd_bind_fn(ctx, plugins, "refresh",   nd_event_noop, 0);
    JS_SetPropertyStr(ctx, navigator, "plugins", plugins);

    JSValue mime_types = JS_NewArray(ctx);
    JS_SetPropertyStr(ctx, mime_types, "length", JS_NewInt32(ctx, 0));
    nd_bind_fn(ctx, mime_types, "namedItem", nd_event_noop, 1);
    JS_SetPropertyStr(ctx, navigator, "mimeTypes", mime_types);

    nd_bind_fn(ctx, navigator, "javaEnabled",       nd_event_noop, 0);
    nd_bind_fn(ctx, navigator, "taintEnabled",      nd_event_noop, 0);
    nd_bind_fn(ctx, navigator, "getAutoplayPolicy", nd_event_noop, 1);
    nd_bind_fn(ctx, navigator, "getBattery",  nd_returns_rejected,  0);
    nd_bind_fn(ctx, navigator, "getGamepads", nd_event_empty_array, 0);
    nd_bind_fn(ctx, navigator, "requestMIDIAccess",            nd_returns_rejected, 1);
    nd_bind_fn(ctx, navigator, "requestMediaKeySystemAccess",  nd_returns_rejected, 2);

    JS_SetPropertyStr(ctx, global, "navigator", navigator);

    JSValue performance = JS_NewObject(ctx);
    nd_bind_fn(ctx, performance, "now", nd_window_performance_now, 0);
    JS_SetPropertyStr(ctx, performance, "timeOrigin", JS_NewFloat64(ctx, 0));
    nd_bind_fn(ctx, performance, "mark",          nd_event_noop, 1);
    nd_bind_fn(ctx, performance, "measure",       nd_event_noop, 3);
    nd_bind_fn(ctx, performance, "clearMarks",    nd_event_noop, 1);
    nd_bind_fn(ctx, performance, "clearMeasures", nd_event_noop, 1);
    nd_bind_fn(ctx, performance, "getEntries",       nd_event_empty_array, 0);
    nd_bind_fn(ctx, performance, "getEntriesByName", nd_event_empty_array, 2);
    nd_bind_fn(ctx, performance, "getEntriesByType", nd_event_empty_array, 1);
    JSValue perf_timing = JS_NewObject(ctx);
    static const char *timing_keys[] = {
        "navigationStart","unloadEventStart","unloadEventEnd","redirectStart",
        "redirectEnd","fetchStart","domainLookupStart","domainLookupEnd",
        "connectStart","connectEnd","secureConnectionStart","requestStart",
        "responseStart","responseEnd","domLoading","domInteractive",
        "domContentLoadedEventStart","domContentLoadedEventEnd","domComplete",
        "loadEventStart","loadEventEnd",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(timing_keys); i++)
        JS_SetPropertyStr(ctx, perf_timing, timing_keys[i],
                          JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, performance, "timing", perf_timing);

    JSValue perf_nav = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, perf_nav, "type",         JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, perf_nav, "redirectCount", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, performance, "navigation", perf_nav);

    JSValue perf_mem = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, perf_mem, "jsHeapSizeLimit",
                      JS_NewInt32(ctx, 128 * 1024 * 1024));
    JS_SetPropertyStr(ctx, perf_mem, "totalJSHeapSize",
                      JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, perf_mem, "usedJSHeapSize",
                      JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, performance, "memory", perf_mem);

    JS_SetPropertyStr(ctx, performance, "eventCounts", JS_NewObject(ctx));
    nd_bind_fn(ctx, performance, "clearResourceTimings",        nd_event_noop, 0);
    nd_bind_fn(ctx, performance, "setResourceTimingBufferSize", nd_event_noop, 1);
    nd_bind_fn(ctx, performance, "toJSON",                      nd_event_noop, 0);

    JS_SetPropertyStr(ctx, global, "performance", performance);

    nd_bind_fn(ctx, global, "MutationObserver",      nd_window_observer_ctor, 1);
    nd_bind_fn(ctx, global, "IntersectionObserver",  nd_window_observer_ctor, 1);
    nd_bind_fn(ctx, global, "ResizeObserver",        nd_window_observer_ctor, 1);
    nd_bind_fn(ctx, global, "PerformanceObserver",   nd_window_observer_ctor, 1);

    nd_bind_fn(ctx, global, "addEventListener",    nd_document_addEventListener,    2);
    nd_bind_fn(ctx, global, "removeEventListener", nd_document_removeEventListener, 2);
    JS_SetPropertyStr(ctx, global, "scrollY", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "scrollX", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "pageYOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "pageXOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "innerWidth",  JS_NewInt32(ctx, 1000));
    JS_SetPropertyStr(ctx, global, "innerHeight", JS_NewInt32(ctx, 800));
    JS_SetPropertyStr(ctx, global, "outerWidth",  JS_NewInt32(ctx, 1000));
    JS_SetPropertyStr(ctx, global, "outerHeight", JS_NewInt32(ctx, 800));
    JS_SetPropertyStr(ctx, global, "devicePixelRatio", JS_NewFloat64(ctx, 1.0));
    static const nd_fn_def window_noops[] = {
        { "scrollTo", 2 }, { "scrollBy", 2 }, { "scroll", 2 },
        { "print", 0 }, { "close", 0 }, { "focus", 0 }, { "blur", 0 },
        { "stop", 0 }, { "find", 7 },
        { "moveTo", 2 }, { "moveBy", 2 },
        { "resizeTo", 2 }, { "resizeBy", 2 },
        { "cancelAnimationFrame", 1 },
    };
    nd_bind_fns(ctx, global, nd_event_noop, window_noops, G_N_ELEMENTS(window_noops));
    nd_bind_fn(ctx, global, "open",                  nd_window_open_method,            3);
    nd_bind_fn(ctx, global, "confirm",               nd_window_confirm,                1);
    nd_bind_fn(ctx, global, "prompt",                nd_window_prompt,                 2);
    nd_bind_fn(ctx, global, "matchMedia",            nd_window_matchMedia,             1);
    nd_bind_fn(ctx, global, "getComputedStyle",      nd_window_getComputedStyle,       1);
    nd_bind_fn(ctx, global, "requestAnimationFrame", nd_window_requestAnimationFrame,  1);

    JSValue history = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, history, "length", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, history, "state",  JS_NULL);
    JS_SetPropertyStr(ctx, history, "scrollRestoration", JS_NewString(ctx, "auto"));
    static const nd_fn_def history_noops[] = {
        { "pushState", 3 }, { "replaceState", 3 },
        { "back", 0 }, { "forward", 0 }, { "go", 1 },
    };
    nd_bind_fns(ctx, history, nd_event_noop, history_noops, G_N_ELEMENTS(history_noops));
    JS_SetPropertyStr(ctx, global, "history", history);

    JSValue crypto = JS_NewObject(ctx);
    nd_bind_fn(ctx, crypto, "getRandomValues", nd_window_getRandomValues, 1);
    nd_bind_fn(ctx, crypto, "randomUUID",      nd_window_randomUUID,      0);
    JS_SetPropertyStr(ctx, global, "crypto", crypto);

    nd_bind_fn(ctx, global, "btoa", nd_window_btoa, 1);
    nd_bind_fn(ctx, global, "atob", nd_window_atob, 1);
    JSValue url_ctor = JS_NewCFunction(ctx, nd_window_url_ctor, "URL", 2);
    nd_bind_fn(ctx, url_ctor, "canParse",        nd_event_true,        2);
    nd_bind_fn(ctx, url_ctor, "parse",           nd_window_url_ctor,   2);
    nd_bind_fn(ctx, url_ctor, "createObjectURL", nd_event_noop,        1);
    nd_bind_fn(ctx, url_ctor, "revokeObjectURL", nd_event_noop,        1);
    JS_SetPropertyStr(ctx, global, "URL", url_ctor);
    JSValue custom_elements = JS_NewObject(ctx);
    nd_bind_fn(ctx, custom_elements, "define", nd_event_noop, 3);
    nd_bind_fn(ctx, custom_elements, "get",         nd_event_noop, 1);
    nd_bind_fn(ctx, custom_elements, "upgrade",     nd_event_noop, 1);
    nd_bind_fn(ctx, custom_elements, "whenDefined", nd_event_noop, 1);
    JS_SetPropertyStr(ctx, global, "customElements", custom_elements);

    nd_bind_fn(ctx, global, "Image",           nd_window_image_ctor,           2);
    nd_bind_fn(ctx, global, "Audio",           nd_window_audio_ctor,           1);
    nd_bind_fn(ctx, global, "Option",          nd_window_option_ctor,          4);
    nd_bind_fn(ctx, global, "URLSearchParams", nd_window_usp_ctor,             1);
    nd_bind_fn(ctx, global, "XMLHttpRequest",  nd_window_xhr_ctor,             0);
    nd_bind_fn(ctx, global, "DOMParser",       nd_window_dom_parser_ctor,      0);
    nd_bind_fn(ctx, global, "FormData",        nd_window_form_data_ctor,       1);
    nd_bind_fn(ctx, global, "AbortController", nd_window_abort_controller_ctor, 0);

    JSValue abort_signal_ctor = JS_NewObject(ctx);
    nd_bind_fn(ctx, abort_signal_ctor, "abort",   nd_event_noop, 1);
    nd_bind_fn(ctx, abort_signal_ctor, "timeout", nd_event_noop, 1);
    nd_bind_fn(ctx, abort_signal_ctor, "any",     nd_event_noop, 1);
    JS_SetPropertyStr(ctx, global, "AbortSignal", abort_signal_ctor);

    JSValue caches_obj = JS_NewObject(ctx);
    nd_bind_fn(ctx, caches_obj, "open",   nd_returns_rejected, 1);
    nd_bind_fn(ctx, caches_obj, "has",    nd_returns_rejected, 1);
    nd_bind_fn(ctx, caches_obj, "delete", nd_returns_rejected, 1);
    nd_bind_fn(ctx, caches_obj, "keys",   nd_returns_rejected, 0);
    nd_bind_fn(ctx, caches_obj, "match",  nd_returns_rejected, 2);
    {
        JSValue prev = JS_GetPropertyStr(ctx, global, "caches");
        JS_FreeValue(ctx, prev);
        JS_SetPropertyStr(ctx, global, "caches", caches_obj);
    }
    nd_bind_fn(ctx, global, "TextEncoder", nd_window_text_encoder_ctor, 0);
    nd_bind_fn(ctx, global, "TextDecoder", nd_window_text_decoder_ctor, 0);

    nd_bind_fn(ctx, global, "Event",         nd_window_event_ctor, 2);
    nd_bind_fn(ctx, global, "CustomEvent",   nd_window_event_ctor, 2);
    nd_bind_fn(ctx, global, "KeyboardEvent", nd_window_event_ctor, 2);
    nd_bind_fn(ctx, global, "MouseEvent",    nd_window_event_ctor, 2);
    static const char *event_subclasses[] = {
        "ProgressEvent","ErrorEvent","HashChangeEvent","PopStateEvent",
        "MessageEvent","StorageEvent","PageTransitionEvent","BeforeUnloadEvent",
        "SubmitEvent","InputEvent","TouchEvent","DragEvent","WheelEvent",
        "FocusEvent","AnimationEvent","TransitionEvent","ClipboardEvent",
        "CompositionEvent","PointerEvent","UIEvent","CloseEvent",
        "MediaQueryListEvent","BlobEvent","FontFaceSetLoadEvent",
        "GamepadEvent","DeviceMotionEvent","DeviceOrientationEvent",
        "PromiseRejectionEvent","SecurityPolicyViolationEvent",
        "TrustedTypePolicyFactory",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(event_subclasses); i++)
        JS_SetPropertyStr(ctx, global, event_subclasses[i],
            JS_NewCFunction(ctx, nd_window_event_ctor,
                            event_subclasses[i], 2));

    static const nd_fn_def event_base_ctors[] = {
        { "EventTarget", 0 }, { "Node", 0 }, { "Element", 0 },
        { "HTMLElement", 0 }, { "Document", 0 }, { "HTMLDocument", 0 },
        { "Window", 0 },
    };
    nd_bind_fns(ctx, global, nd_window_event_ctor,
                event_base_ctors, G_N_ELEMENTS(event_base_ctors));

    JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "self",   JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "top",    JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "parent", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "globalThis", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "frames", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, global, "length", JS_NewInt32(ctx, 0));

    nd_bind_fn(ctx, global, "getSelection",        nd_window_get_selection, 0);
    nd_bind_fn(ctx, global, "requestIdleCallback", nd_event_noop, 1);
    nd_bind_fn(ctx, global, "cancelIdleCallback",  nd_event_noop, 1);

    JS_SetPropertyStr(ctx, global, "screenX",     JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "screenY",     JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "screenLeft",  JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "screenTop",   JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "isSecureContext",
                      js->current_url && g_str_has_prefix(js->current_url, "https:") ? JS_TRUE : JS_FALSE);
    JS_SetPropertyStr(ctx, global, "origin",
                      JS_NewString(ctx, js->current_url ? js->current_url : ""));
    JS_SetPropertyStr(ctx, global, "name",   JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, global, "status", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, global, "closed", JS_FALSE);
    JS_SetPropertyStr(ctx, global, "opener", JS_NULL);
    JS_SetPropertyStr(ctx, global, "event",  JS_NULL);
    JS_SetPropertyStr(ctx, global, "indexedDB", JS_NULL);
    JS_SetPropertyStr(ctx, global, "caches",    JS_NULL);

    JSValue screen = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, screen, "width",       JS_NewInt32(ctx, 1920));
    JS_SetPropertyStr(ctx, screen, "height",      JS_NewInt32(ctx, 1080));
    JS_SetPropertyStr(ctx, screen, "availWidth",  JS_NewInt32(ctx, 1920));
    JS_SetPropertyStr(ctx, screen, "availHeight", JS_NewInt32(ctx, 1040));
    JS_SetPropertyStr(ctx, screen, "availLeft",   JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, screen, "availTop",    JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, screen, "colorDepth",  JS_NewInt32(ctx, 24));
    JS_SetPropertyStr(ctx, screen, "pixelDepth",  JS_NewInt32(ctx, 24));
    JSValue orientation = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, orientation, "type",
                      JS_NewString(ctx, "landscape-primary"));
    JS_SetPropertyStr(ctx, orientation, "angle",
                      JS_NewInt32(ctx, 0));
    nd_bind_fn(ctx, orientation, "lock",   nd_returns_rejected, 1);
    nd_bind_fn(ctx, orientation, "unlock", nd_event_noop,       0);
    JS_SetPropertyStr(ctx, screen, "orientation", orientation);
    JS_SetPropertyStr(ctx, global, "screen", screen);

    nd_bind_fn(ctx, global, "structuredClone",  nd_window_structured_clone,  1);
    nd_bind_fn(ctx, global, "reportError",      nd_window_report_error,      1);
    nd_bind_fn(ctx, global, "MessageChannel",   nd_window_message_channel,   0);
    nd_bind_fn(ctx, global, "BroadcastChannel", nd_window_broadcast_channel, 1);
    nd_bind_fn(ctx, global, "Notification",     nd_window_notification,      2);
    nd_bind_fn(ctx, global, "Worker",       nd_throws_unsupported, 1);
    nd_bind_fn(ctx, global, "SharedWorker", nd_throws_unsupported, 1);
    nd_bind_fn(ctx, global, "WebSocket",    nd_throws_unsupported, 2);
    nd_bind_fn(ctx, global, "EventSource",  nd_throws_unsupported, 2);

    JSValue notif_perm = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, notif_perm, "permission", JS_NewString(ctx, "denied"));
    nd_bind_fn(ctx, notif_perm, "requestPermission",
               nd_returns_resolved_undefined, 0);
    (void)notif_perm;

    JSValue css_obj = JS_NewObject(ctx);
    nd_bind_fn(ctx, css_obj, "supports", nd_css_supports, 2);
    nd_bind_fn(ctx, css_obj, "escape",   nd_css_escape,   1);
    JS_SetPropertyStr(ctx, global, "CSS", css_obj);

    JSValue subtle = JS_NewObject(ctx);
    static const nd_fn_def subtle_methods[] = {
        { "digest", 2 }, { "encrypt", 3 }, { "decrypt", 3 },
        { "sign", 3 }, { "verify", 4 },
        { "generateKey", 3 }, { "importKey", 5 }, { "exportKey", 2 },
        { "deriveBits", 3 }, { "deriveKey", 5 },
    };
    nd_bind_fns(ctx, subtle, nd_returns_rejected,
                subtle_methods, G_N_ELEMENTS(subtle_methods));
    JSValue crypto_obj = JS_GetPropertyStr(ctx, global, "crypto");
    if (!JS_IsUndefined(crypto_obj) && !JS_IsNull(crypto_obj))
        JS_SetPropertyStr(ctx, crypto_obj, "subtle", subtle);
    else
        JS_FreeValue(ctx, subtle);
    JS_FreeValue(ctx, crypto_obj);

    JSValue perf_observer = JS_NewCFunction(ctx, nd_event_noop, "PerformanceObserver", 1);
    JSValue perf_proto = JS_NewObject(ctx);
    nd_bind_fn(ctx, perf_proto, "observe",     nd_event_noop,        1);
    nd_bind_fn(ctx, perf_proto, "disconnect",  nd_event_noop,        0);
    nd_bind_fn(ctx, perf_proto, "takeRecords", nd_event_empty_array, 0);
    JS_SetPropertyStr(ctx, perf_observer, "prototype", perf_proto);
    JS_SetPropertyStr(ctx, global, "PerformanceObserver", perf_observer);

    JSValue local_obj = JS_NewObjectClass(ctx, nd_storage_class_id);
    JS_SetOpaque(local_obj, js->local_storage);
    JS_SetPropertyStr(ctx, global, "localStorage", local_obj);

    JSValue session_obj = JS_NewObjectClass(ctx, nd_storage_class_id);
    JS_SetOpaque(session_obj, js->session_storage);
    JS_SetPropertyStr(ctx, global, "sessionStorage", session_obj);

    JS_FreeValue(ctx, global);
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
nd_document_createElementNS(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_active_js || argc < 2) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[1]);
    if (!name) return JS_NULL;
    char *lower = g_ascii_strdown(name, -1);
    JS_FreeCString(ctx, name);
    nd_node *el = nd_node_new_element(lower);
    g_ptr_array_add(g_active_js->orphan_nodes, el);
    return nd_make_element(ctx, el);
}

static JSValue
nd_document_createComment(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_active_js || argc < 1) return JS_NULL;
    const char *text = JS_ToCString(ctx, argv[0]);
    char *dup = text ? g_strdup(text) : g_strdup("");
    if (text) JS_FreeCString(ctx, text);
    nd_node *n = nd_node_new_comment(dup);
    g_ptr_array_add(g_active_js->orphan_nodes, n);
    return nd_make_element(ctx, n);
}

static JSValue
nd_document_createDocumentFragment(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (!g_active_js) return JS_NULL;
    nd_node *frag = nd_node_new_document();
    g_ptr_array_add(g_active_js->orphan_nodes, frag);
    return nd_make_element(ctx, frag);
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

static nd_node *
nd_doc_find_title_node(void)
{
    if (!g_active_js || !g_active_js->current_doc) return NULL;
    return nd_node_find_first_element(g_active_js->current_doc, "title");
}

static void
nd_collect_by_name(const nd_node *root, const char *name,
                   JSContext *ctx, JSValue arr, uint32_t *idx)
{
    if (!root) return;
    if (root->kind == ND_NODE_ELEMENT) {
        const char *n = nd_element_get_attr(root, "name");
        if (n && strcmp(n, name) == 0)
            JS_SetPropertyUint32(ctx, arr, (*idx)++, nd_make_element(ctx, root));
    }
    for (const nd_node *c = root->first_child; c; c = c->next_sibling)
        nd_collect_by_name(c, name, ctx, arr, idx);
}

static JSValue
nd_document_getElementsByName(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue arr = JS_NewArray(ctx);
    if (!g_active_js || !g_active_js->current_doc || argc < 1) return arr;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return arr;
    uint32_t i = 0;
    nd_collect_by_name(g_active_js->current_doc, name, ctx, arr, &i);
    JS_FreeCString(ctx, name);
    return arr;
}

static JSValue
nd_document_get_title(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    nd_node *t = nd_doc_find_title_node();
    if (!t) return JS_NewString(ctx, "");
    char *text = nd_node_collect_text(t);
    JSValue v = JS_NewString(ctx, text ? text : "");
    g_free(text);
    return v;
}

static JSValue
nd_document_set_title(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    (void)this_val;
    const char *s = JS_ToCString(ctx, val);
    if (!s) return JS_UNDEFINED;
    nd_node *t = nd_doc_find_title_node();
    if (!t && g_active_js && g_active_js->current_doc) {
        nd_node *head = nd_node_find_first_element(g_active_js->current_doc, "head");
        if (!head) head = g_active_js->current_doc;
        t = nd_node_new_element(g_strdup("title"));
        nd_node_append_child(head, t);
    }
    if (t) {
        nd_node *c = t->first_child;
        while (c) {
            nd_node *next = c->next_sibling;
            nd_node_remove(c);
            nd_node_free(c);
            c = next;
        }
        nd_node *text = nd_node_new_text(g_strdup(s));
        nd_node_append_child(t, text);
        if (g_active_js) g_active_js->mutated = TRUE;
    }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue
nd_document_get_cookie(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!g_active_js) return JS_NewString(ctx, "");
    return JS_NewString(ctx, g_active_js->cookie_value ? g_active_js->cookie_value : "");
}

static JSValue
nd_document_set_cookie(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    (void)this_val;
    if (!g_active_js) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (!s) return JS_UNDEFINED;
    const char *eq = strchr(s, '=');
    const char *semi = strchr(s, ';');
    if (!eq) { JS_FreeCString(ctx, s); return JS_UNDEFINED; }
    gsize key_len = (gsize)(eq - s);
    gsize pair_len = semi ? (gsize)(semi - s) : strlen(s);
    char *pair = g_strndup(s, pair_len);
    char *new_jar = NULL;
    if (g_active_js->cookie_value) {
        new_jar = g_strdup(g_active_js->cookie_value);
        char *needle = g_strndup(s, key_len + 1);
        char *found = strstr(new_jar, needle);
        if (found && (found == new_jar || *(found - 1) == ' ' || *(found - 1) == ';')) {
            char *end = strstr(found, "; ");
            char *rest = end ? end + 2 : NULL;
            *found = '\0';
            char *merged = g_strconcat(new_jar, rest ? rest : "", NULL);
            g_free(new_jar);
            new_jar = merged;
        }
        g_free(needle);
        gsize len = strlen(new_jar);
        while (len > 0 && (new_jar[len - 1] == ';' || new_jar[len - 1] == ' ')) {
            new_jar[--len] = '\0';
        }
        char *with_pair = len > 0 ? g_strconcat(new_jar, "; ", pair, NULL)
                                  : g_strdup(pair);
        g_free(new_jar);
        new_jar = with_pair;
    } else {
        new_jar = g_strdup(pair);
    }
    g_free(pair);
    g_free(g_active_js->cookie_value);
    g_active_js->cookie_value = new_jar;
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue
nd_document_get_referrer(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!g_active_js) return JS_NewString(ctx, "");
    return JS_NewString(ctx, g_active_js->referrer ? g_active_js->referrer : "");
}

static JSValue
nd_document_get_readyState(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!g_active_js) return JS_NewString(ctx, "loading");
    static const char *names[] = { "loading", "interactive", "complete" };
    int idx = g_active_js->ready_state;
    if (idx < 0 || idx > 2) idx = 0;
    return JS_NewString(ctx, names[idx]);
}

static JSValue
nd_document_get_hidden(JSContext *ctx, JSValueConst this_val)
{
    (void)ctx; (void)this_val;
    return JS_FALSE;
}

static JSValue
nd_document_get_visibilityState(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    return JS_NewString(ctx, "visible");
}

static const JSCFunctionListEntry nd_document_funcs[] = {
    JS_CFUNC_DEF("getElementById",          1, nd_document_getElementById),
    JS_CFUNC_DEF("createElement",            1, nd_document_createElement),
    JS_CFUNC_DEF("createElementNS",          2, nd_document_createElementNS),
    JS_CFUNC_DEF("createTextNode",           1, nd_document_createTextNode),
    JS_CFUNC_DEF("createComment",            1, nd_document_createComment),
    JS_CFUNC_DEF("createDocumentFragment",   0, nd_document_createDocumentFragment),
    JS_CFUNC_DEF("getElementsByTagName",    1, nd_document_getElementsByTagName),
    JS_CFUNC_DEF("getElementsByClassName",  1, nd_document_getElementsByClassName),
    JS_CFUNC_DEF("querySelector",           1, nd_document_querySelector),
    JS_CFUNC_DEF("querySelectorAll",        1, nd_document_querySelectorAll),
    JS_CGETSET_DEF("documentElement", nd_document_get_documentElement, NULL),
    JS_CGETSET_DEF("body",            nd_document_get_body,            NULL),
    JS_CGETSET_DEF("head",            nd_document_get_head,            NULL),
    JS_CGETSET_DEF("activeElement",   nd_document_get_activeElement,   NULL),
    JS_CGETSET_DEF("forms",           nd_document_get_forms,           NULL),
    JS_CGETSET_DEF("images",          nd_document_get_images,          NULL),
    JS_CGETSET_DEF("links",           nd_document_get_links,           NULL),
    JS_CGETSET_DEF("scripts",         nd_document_get_scripts,         NULL),
    JS_CGETSET_DEF("styleSheets",     nd_document_get_styleSheets,     NULL),
    JS_CGETSET_DEF("embeds",          nd_document_get_embeds,          NULL),
    JS_CGETSET_DEF("plugins",         nd_document_get_plugins,         NULL),
    JS_CGETSET_DEF("designMode",      nd_document_get_designMode,      NULL),
    JS_CGETSET_DEF("lastModified",    nd_document_get_lastModified,    NULL),
    JS_CGETSET_DEF("all",             nd_document_get_all,             NULL),
    JS_CGETSET_DEF("anchors",         nd_document_get_anchors,         NULL),
    JS_CGETSET_DEF("applets",         nd_document_get_applets,         NULL),
    JS_CGETSET_DEF("fonts",           nd_document_get_fonts,           NULL),
    JS_CGETSET_DEF("implementation",  nd_document_implementation,      NULL),
    JS_CFUNC_DEF("write",      1, nd_event_noop),
    JS_CFUNC_DEF("writeln",    1, nd_event_noop),
    JS_CFUNC_DEF("open",       0, nd_event_noop),
    JS_CFUNC_DEF("close",      0, nd_event_noop),
    JS_CFUNC_DEF("execCommand", 3, nd_event_noop),
    JS_CFUNC_DEF("hasFocus",          0, nd_document_has_focus),
    JS_CFUNC_DEF("elementFromPoint",  2, nd_document_element_from_point),
    JS_CFUNC_DEF("elementsFromPoint", 2, nd_document_elements_from_point),
    JS_CFUNC_DEF("createRange",       0, nd_document_create_range),
    JS_CFUNC_DEF("createTreeWalker",  3, nd_document_create_tree_walker),
    JS_CFUNC_DEF("createNodeIterator",3, nd_document_create_tree_walker),
    JS_CFUNC_DEF("adoptNode",         1, nd_document_adopt_node),
    JS_CFUNC_DEF("importNode",        2, nd_document_import_node),
    JS_CFUNC_DEF("exitFullscreen", 0, nd_event_noop),
    JS_CFUNC_DEF("queryCommandSupported", 1, nd_event_noop),
    JS_CFUNC_DEF("queryCommandEnabled",   1, nd_event_noop),
    JS_CFUNC_DEF("queryCommandState",     1, nd_event_noop),
    JS_CFUNC_DEF("queryCommandValue",     1, nd_event_noop),
    JS_CGETSET_DEF("currentScript",      nd_element_get_null,     NULL),
    JS_CGETSET_DEF("rootElement",        nd_document_get_documentElement, NULL),
    JS_CGETSET_DEF("fullscreenElement",  nd_element_get_null,  NULL),
    JS_CGETSET_DEF("fullscreenEnabled",  nd_element_get_zero_int, NULL),
    JS_CGETSET_DEF("scrollingElement",   nd_document_get_body, NULL),
    JS_CFUNC_DEF("addEventListener",    2, nd_document_addEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, nd_document_removeEventListener),
    JS_CFUNC_DEF("getElementsByName",   1, nd_document_getElementsByName),
    JS_CGETSET_DEF("title",           nd_document_get_title,  nd_document_set_title),
    JS_CGETSET_DEF("cookie",          nd_document_get_cookie, nd_document_set_cookie),
    JS_CGETSET_DEF("referrer",        nd_document_get_referrer,        NULL),
    JS_CGETSET_DEF("readyState",      nd_document_get_readyState,      NULL),
    JS_CGETSET_DEF("hidden",          nd_document_get_hidden,          NULL),
    JS_CGETSET_DEF("visibilityState", nd_document_get_visibilityState, NULL),
};

static JSValue
nd_location_get_href(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!g_active_js) return JS_NewString(ctx, "");
    return JS_NewString(ctx, g_active_js->current_url ? g_active_js->current_url : "");
}

static const char *
nd_loc_url(void)
{
    return g_active_js && g_active_js->current_url ? g_active_js->current_url : "";
}

static JSValue
nd_location_get_protocol(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    const char *u = nd_loc_url();
    const char *colon = strchr(u, ':');
    if (!colon) return JS_NewString(ctx, "");
    char *s = g_strndup(u, (gsize)(colon - u + 1));
    JSValue v = JS_NewString(ctx, s);
    g_free(s);
    return v;
}

static const char *
nd_loc_host_start(const char *u)
{
    const char *p = strstr(u, "://");
    return p ? p + 3 : u;
}

static JSValue
nd_location_get_host(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    const char *u = nd_loc_url();
    const char *h = nd_loc_host_start(u);
    const char *e = h;
    while (*e && *e != '/' && *e != '?' && *e != '#') e++;
    return JS_NewStringLen(ctx, h, (gsize)(e - h));
}

static JSValue
nd_location_get_hostname(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    const char *u = nd_loc_url();
    const char *h = nd_loc_host_start(u);
    const char *e = h;
    while (*e && *e != ':' && *e != '/' && *e != '?' && *e != '#') e++;
    return JS_NewStringLen(ctx, h, (gsize)(e - h));
}

static JSValue
nd_location_get_port(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    const char *u = nd_loc_url();
    const char *h = nd_loc_host_start(u);
    const char *colon = NULL;
    for (const char *p = h; *p && *p != '/' && *p != '?' && *p != '#'; p++) {
        if (*p == ':') { colon = p; break; }
    }
    if (!colon) return JS_NewString(ctx, "");
    const char *e = colon + 1;
    while (*e && *e != '/' && *e != '?' && *e != '#') e++;
    return JS_NewStringLen(ctx, colon + 1, (gsize)(e - colon - 1));
}

static JSValue
nd_location_get_pathname(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    const char *u = nd_loc_url();
    const char *h = nd_loc_host_start(u);
    while (*h && *h != '/' && *h != '?' && *h != '#') h++;
    if (!*h || *h != '/') return JS_NewString(ctx, "/");
    const char *e = h;
    while (*e && *e != '?' && *e != '#') e++;
    return JS_NewStringLen(ctx, h, (gsize)(e - h));
}

static JSValue
nd_location_get_search(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    const char *u = nd_loc_url();
    const char *q = strchr(u, '?');
    if (!q) return JS_NewString(ctx, "");
    const char *e = q;
    while (*e && *e != '#') e++;
    return JS_NewStringLen(ctx, q, (gsize)(e - q));
}

static JSValue
nd_location_get_hash(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    const char *u = nd_loc_url();
    const char *h = strchr(u, '#');
    return JS_NewString(ctx, h ? h : "");
}

static JSValue
nd_location_get_origin(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    const char *u = nd_loc_url();
    const char *scheme_end = strstr(u, "://");
    if (!scheme_end) return JS_NewString(ctx, "");
    const char *host_end = scheme_end + 3;
    while (*host_end && *host_end != '/' && *host_end != '?' && *host_end != '#')
        host_end++;
    return JS_NewStringLen(ctx, u, (gsize)(host_end - u));
}

static JSValue
nd_location_toString(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return nd_location_get_href(ctx, this_val);
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
    JS_CGETSET_DEF("href",     nd_location_get_href, nd_location_set_href),
    JS_CGETSET_DEF("protocol", nd_location_get_protocol, NULL),
    JS_CGETSET_DEF("host",     nd_location_get_host, NULL),
    JS_CGETSET_DEF("hostname", nd_location_get_hostname, NULL),
    JS_CGETSET_DEF("port",     nd_location_get_port, NULL),
    JS_CGETSET_DEF("pathname", nd_location_get_pathname, NULL),
    JS_CGETSET_DEF("search",   nd_location_get_search, NULL),
    JS_CGETSET_DEF("hash",     nd_location_get_hash, NULL),
    JS_CGETSET_DEF("origin",   nd_location_get_origin, NULL),
    JS_CFUNC_DEF("assign",   1, nd_location_assign),
    JS_CFUNC_DEF("reload",   0, nd_location_reload),
    JS_CFUNC_DEF("replace",  1, nd_location_assign),
    JS_CFUNC_DEF("toString", 0, nd_location_toString),
};

static void
nd_js_install_document(nd_js *js, nd_node *doc, const char *base_url)
{
    js->current_doc = doc;
    g_free(js->current_url);
    js->current_url = g_strdup(base_url ? base_url : "");

    nd_storage_load_for(js, js->current_url);

    JSContext *ctx = js->ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue document = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, document, "URL",         JS_NewString(ctx, js->current_url));
    JS_SetPropertyStr(ctx, document, "documentURI", JS_NewString(ctx, js->current_url));
    JS_SetPropertyStr(ctx, document, "baseURI",     JS_NewString(ctx, js->current_url));
    JS_SetPropertyStr(ctx, document, "characterSet", JS_NewString(ctx, "UTF-8"));
    JS_SetPropertyStr(ctx, document, "charset",      JS_NewString(ctx, "UTF-8"));
    JS_SetPropertyStr(ctx, document, "inputEncoding", JS_NewString(ctx, "UTF-8"));
    JS_SetPropertyStr(ctx, document, "compatMode",   JS_NewString(ctx, "CSS1Compat"));
    JS_SetPropertyStr(ctx, document, "contentType",  JS_NewString(ctx, "text/html"));
    JS_SetPropertyStr(ctx, document, "domain", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, document, "defaultView",  JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, document, "ownerDocument", JS_NULL);
    JS_SetPropertyStr(ctx, document, "nodeName",     JS_NewString(ctx, "#document"));
    JS_SetPropertyStr(ctx, document, "nodeType",     JS_NewInt32(ctx, 9));
    JS_SetPropertyStr(ctx, document, "doctype",      JS_NULL);
    JS_SetPropertyStr(ctx, document, "xmlVersion",   JS_NewString(ctx, "1.0"));
    JS_SetPropertyStr(ctx, document, "xmlEncoding",  JS_NULL);
    JS_SetPropertyStr(ctx, document, "xmlStandalone", JS_FALSE);
    JS_SetPropertyFunctionList(ctx, document, nd_document_funcs,
                               G_N_ELEMENTS(nd_document_funcs));
    JS_SetPropertyStr(ctx, global, "document", document);

    JSValue location = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, location, nd_location_funcs,
                               G_N_ELEMENTS(nd_location_funcs));
    JS_SetPropertyStr(ctx, global, "location", location);
    JS_SetPropertyStr(ctx, document, "location", JS_DupValue(ctx, location));

    JSValue xml_serializer = JS_NewObject(ctx);
    nd_bind_fn(ctx, xml_serializer, "serializeToString", nd_event_noop, 1);
    static const nd_fn_def shim_ctors[] = {
        { "XMLSerializer", 0 }, { "XMLDocument", 0 }, { "XSLTProcessor", 0 },
        { "Range", 0 }, { "NodeFilter", 0 }, { "DOMException", 2 },
        { "DOMTokenList", 0 }, { "NodeList", 0 }, { "HTMLCollection", 0 },
        { "CSSStyleSheet", 0 }, { "CSSStyleDeclaration", 0 },
        { "CSSRule", 0 }, { "CSSStyleRule", 0 },
        { "MediaList", 0 }, { "MediaQueryList", 0 },
        { "ShadowRoot", 0 }, { "Selection", 0 }, { "Animation", 0 },
        { "Headers", 1 }, { "Request", 2 }, { "Response", 2 },
        { "Blob", 2 }, { "File", 3 }, { "FileReader", 0 }, { "FileList", 0 },
        { "Storage", 0 },
        { "HTMLInputElement", 0 }, { "HTMLAnchorElement", 0 },
        { "HTMLImageElement", 0 }, { "HTMLFormElement", 0 },
        { "HTMLSelectElement", 0 }, { "HTMLOptionElement", 0 },
        { "HTMLButtonElement", 0 }, { "HTMLDivElement", 0 },
        { "HTMLSpanElement", 0 }, { "HTMLTableElement", 0 },
        { "HTMLTableRowElement", 0 }, { "HTMLTableCellElement", 0 },
        { "HTMLLabelElement", 0 }, { "HTMLTextAreaElement", 0 },
        { "HTMLVideoElement", 0 }, { "HTMLAudioElement", 0 },
        { "HTMLMediaElement", 0 }, { "HTMLDialogElement", 0 },
        { "HTMLDetailsElement", 0 }, { "HTMLScriptElement", 0 },
        { "HTMLLinkElement", 0 }, { "HTMLMetaElement", 0 },
        { "HTMLStyleElement", 0 }, { "HTMLBodyElement", 0 },
        { "HTMLHtmlElement", 0 }, { "HTMLHeadElement", 0 },
        { "HTMLIFrameElement", 0 }, { "HTMLCanvasElement", 0 },
        { "Text", 0 }, { "Comment", 0 }, { "Attr", 0 },
        { "DocumentFragment", 0 }, { "DocumentType", 0 },
        { "HTMLOptionsCollection", 0 }, { "HTMLAllCollection", 0 },
        { "RadioNodeList", 0 }, { "TextMetrics", 0 },
        { "CanvasRenderingContext2D", 0 }, { "ImageData", 4 },
        { "ImageBitmap", 0 }, { "OffscreenCanvas", 2 }, { "Path2D", 1 },
        { "ValidityState", 0 },
        { "DOMRect", 4 }, { "DOMRectReadOnly", 4 },
        { "DOMPoint", 4 }, { "DOMPointReadOnly", 4 },
        { "DOMMatrix", 1 }, { "DOMMatrixReadOnly", 1 },
        { "DOMQuad", 4 }, { "DOMStringList", 0 }, { "DOMStringMap", 0 },
        { "NamedNodeMap", 0 }, { "TreeWalker", 0 }, { "NodeIterator", 0 },
        { "MutationRecord", 0 }, { "IntersectionObserverEntry", 0 },
        { "ResizeObserverEntry", 0 },
        { "PerformanceEntry", 0 }, { "PerformanceMark", 0 },
        { "PerformanceMeasure", 0 }, { "PerformanceResourceTiming", 0 },
        { "PerformanceNavigationTiming", 0 },
        { "FontFace", 3 }, { "FontFaceSet", 0 },
        { "ReadableStream", 1 }, { "WritableStream", 1 },
        { "TransformStream", 1 },
        { "ByteLengthQueuingStrategy", 1 }, { "CountQueuingStrategy", 1 },
        { "ServiceWorker", 0 }, { "ServiceWorkerRegistration", 0 },
        { "ServiceWorkerContainer", 0 },
        { "Geolocation", 0 }, { "Permissions", 0 },
        { "Crypto", 0 }, { "SubtleCrypto", 0 }, { "CryptoKey", 0 },
    };
    nd_bind_fns(ctx, global, nd_window_event_ctor, shim_ctors, G_N_ELEMENTS(shim_ctors));
    JS_FreeValue(ctx, xml_serializer);

    JS_FreeValue(ctx, global);
}

void
nd_js_free(nd_js *js)
{
    if (!js) return;
    nd_storage_flush(js);
    g_free(js->local_storage_origin);
    g_free(js->local_storage_path);
    g_free(js->cookie_value);
    g_free(js->referrer);
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
    js->eval_deadline_us = g_get_monotonic_time() + nd_js_eval_budget_us();
    JSValue v = JS_Eval(js->ctx, src, len, origin, JS_EVAL_TYPE_GLOBAL);
    js->eval_deadline_us = 0;
    if (JS_IsException(v)) {
        JSValue ex = JS_GetException(js->ctx);
        const char *msg = JS_ToCString(js->ctx, ex);
        if (msg && js->log_cb) {
            JSValue stk = JS_GetPropertyStr(js->ctx, ex, "stack");
            const char *stack = JS_ToCString(js->ctx, stk);
            char *line = g_strdup_printf("JS error in %s: %s%s%s",
                                         origin ? origin : "inline",
                                         msg,
                                         stack && *stack ? "\n" : "",
                                         stack ? stack : "");
            js->log_cb(line, js->log_user_data);
            g_free(line);
            if (stack) JS_FreeCString(js->ctx, stack);
            JS_FreeValue(js->ctx, stk);
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
nd_js_run_scripts_in_doc(nd_js *js, nd_node *doc, const char *base_url)
{
    if (!js || !doc) return;
    js->ready_state = 0;
    nd_js_install_document(js, doc, base_url);
    nd_js_walk_scripts(js, doc, base_url && *base_url ? base_url : "inline");
    js->ready_state = 1;
    nd_js_dispatch_event(js, doc, "DOMContentLoaded", NULL);
    js->ready_state = 2;
    nd_js_dispatch_event(js, doc, "load", NULL);
}

void
nd_js_set_scroll_to_cb(nd_js *js, nd_js_scroll_to_cb cb, gpointer user_data)
{
    if (!js) return;
    js->scroll_to_cb = cb;
    js->scroll_to_user_data = user_data;
}

void
nd_js_set_form_submit_cb(nd_js *js, nd_js_form_submit_cb cb, gpointer user_data)
{
    if (!js) return;
    js->form_submit_cb = cb;
    js->form_submit_user_data = user_data;
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
