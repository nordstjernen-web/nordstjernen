/* Nordstjernen — JavaScript engine binding (QuickJS).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "js.h"
#include "jquery_shim.h"
#include "version.h"

#include <string.h>

#include <cairo.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <pango/pangocairo.h>
#include <quickjs.h>

#include "config.h"
#include "css.h"
#include "html.h"
#include "layout.h"
#include "net.h"

typedef struct nd_mut_target {
    nd_node *target;
    gboolean subtree;
    gboolean child_list;
    gboolean attributes;
    gboolean character_data;
    gboolean attribute_old_value;
    gboolean character_data_old_value;
    GPtrArray *attribute_filter;
} nd_mut_target;

typedef struct nd_mut_observer {
    JSValue   cb;
    JSValue   wrapper;
    gboolean  disconnected;
    GArray   *targets;
    GPtrArray *records;
} nd_mut_observer;

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
    GArray       *raf_pending;
    int           next_raf_id;
    gint64        raf_start_us;
    gint64        raf_last_us;
    GHashTable   *style_table;
    const struct nd_box *layout_root;
    GHashTable   *canvas_states;
    GPtrArray    *orphan_nodes;
    GPtrArray    *listeners;
    GPtrArray    *pending_fetches;
    GPtrArray    *pending_xhrs;
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
    int           dispatch_depth;
    GPtrArray    *mutation_observers;
    gboolean      mutation_drain_scheduled;
    const nd_csp *csp;
};

static nd_js *g_active_js;

static void nd_js_set_attr_recorded(nd_js *js, nd_node *n, const char *name, const char *value);
static void nd_js_record_child_change(nd_js *js, nd_node *parent,
                                      nd_node *added, nd_node *removed,
                                      nd_node *previous_sibling, nd_node *next_sibling);
static void nd_js_record_attr_change(nd_js *js, nd_node *target,
                                     const char *name, const char *old_value);
static void nd_js_record_character_data(nd_js *js, nd_node *target, const char *old_value);
static gboolean nd_mut_target_covers(const nd_mut_target *t, nd_node *node);

static gint64
nd_js_eval_budget_us(void)
{
    const nd_config *c = nd_config_get();
    int ms = c ? c->js_eval_budget_ms : 5000;
    if (ms <= 0) ms = 5000;
    if (ms > ND_JS_EVAL_BUDGET_MAX_MS) ms = ND_JS_EVAL_BUDGET_MAX_MS;
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

typedef struct nd_budget_guard {
    gint64 saved;
} nd_budget_guard;

static void
nd_js_budget_push(nd_js *js, nd_budget_guard *g)
{
    if (!js || !g) return;
    g->saved = js->eval_deadline_us;
    gint64 fresh = g_get_monotonic_time() + nd_js_eval_budget_us();
    if (g->saved == 0 || fresh < g->saved)
        js->eval_deadline_us = fresh;
}

static void
nd_js_budget_pop(nd_js *js, nd_budget_guard *g)
{
    if (!js || !g) return;
    js->eval_deadline_us = g->saved;
}

typedef struct nd_listener {
    const nd_node *target;
    char          *type;
    JSValue        cb;
    gboolean       capture;
    gboolean       once;
} nd_listener;

typedef struct nd_timer {
    nd_js  *js;
    JSValue cb;
    int     id;
    guint   glib_source;
    gboolean is_interval;
} nd_timer;

typedef struct nd_raf_entry {
    int     id;
    JSValue cb;
} nd_raf_entry;

typedef struct nd_canvas_state {
    int w, h;
    cairo_surface_t *surf;
    cairo_t         *cr;
    double fill_r, fill_g, fill_b, fill_a;
    double stroke_r, stroke_g, stroke_b, stroke_a;
    double line_width;
    char  *font;
} nd_canvas_state;

static void
nd_canvas_state_free(gpointer data)
{
    nd_canvas_state *st = data;
    if (!st) return;
    if (st->cr)   cairo_destroy(st->cr);
    if (st->surf) cairo_surface_destroy(st->surf);
    g_free(st->font);
    g_free(st);
}

static inline nd_js *
js_from_ctx(JSContext *ctx)
{
    return ctx ? (nd_js *)JS_GetContextOpaque(ctx) : NULL;
}

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
    nd_budget_guard g = {0};
    nd_js_budget_push(js, &g);
    JSContext *ctx_out = NULL;
    int r = 0;
    int safety = 100000;
    while (safety-- > 0 && (r = JS_ExecutePendingJob(js->rt, &ctx_out)) > 0)
        ;
    nd_js_budget_pop(js, &g);
    if (r < 0 && js->log_cb)
        js->log_cb("[error] microtask threw", js->log_user_data);
    if (safety <= 0 && js->log_cb)
        js->log_cb("[warning] microtask drain hit safety limit", js->log_user_data);
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
    nd_budget_guard bg;
    nd_js_budget_push(js, &bg);
    JSValue ret = JS_Call(js->ctx, t->cb, JS_UNDEFINED, 0, NULL);
    nd_js_budget_pop(js, &bg);
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
    if (!js_from_ctx(ctx) || argc < 1) return JS_NewInt32(ctx, 0);
    if (!JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    int32_t ms = 0;
    if (argc >= 2) JS_ToInt32(ctx, &ms, argv[1]);
    if (ms < 4) ms = 4;

    nd_js *js = js_from_ctx(ctx);
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
    if (!js_from_ctx(ctx) || argc < 1) return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    nd_timer *t = g_hash_table_lookup(js_from_ctx(ctx)->timers, GINT_TO_POINTER(id));
    if (t) {
        if (t->glib_source) { g_source_remove(t->glib_source); t->glib_source = 0; }
        g_hash_table_remove(js_from_ctx(ctx)->timers, GINT_TO_POINTER(id));
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
            nd_js_set_attr_recorded(js_from_ctx(ctx), n, "style", s);
            JS_FreeCString(ctx, s);
        }
        return TRUE;
    }
    char *css = camel_to_kebab(name);
    JS_FreeCString(ctx, name);
    const char *vstr = JS_ToCString(ctx, val);
    const char *old = nd_element_get_attr(n, "style");
    char *new_style = nd_inline_style_set(old, css, vstr ? vstr : "");
    nd_js_set_attr_recorded(js_from_ctx(ctx), n, "style", new_style);
    g_free(new_style);
    g_free(css);
    if (vstr) JS_FreeCString(ctx, vstr);
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
        nd_js_set_attr_recorded(js_from_ctx(ctx), n, "class", next);
        g_free(next);
        JS_FreeCString(ctx, t);
    }
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
        nd_js_set_attr_recorded(js_from_ctx(ctx), n, "class", next);
        g_free(next);
        JS_FreeCString(ctx, t);
    }
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
            nd_js_set_attr_recorded(js_from_ctx(ctx), n, "class", step2);
            g_free(step1); g_free(step2);
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
    nd_js_set_attr_recorded(js_from_ctx(ctx), n, "class", next);
    g_free(next);
    JS_FreeCString(ctx, t);
    return has ? JS_FALSE : JS_TRUE;
}

static char *
nd_storage_path_for_origin(const char *origin)
{
    if (!origin || !*origin) return NULL;
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, origin, -1);
    char *dir = g_build_filename(g_get_user_data_dir(), ND_APP_DIR_NAME,
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
    char *new_origin = nd_url_origin_from(new_url);
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

static void nd_storage_maybe_dirty(JSContext *ctx, GHashTable *store);

static gboolean
nd_storage_name_is_builtin(const char *name)
{
    static const char *const builtins[] = {
        "length", "constructor", "getItem", "setItem",
        "removeItem", "clear", "key"
    };
    for (size_t i = 0; i < G_N_ELEMENTS(builtins); i++)
        if (strcmp(name, builtins[i]) == 0) return TRUE;
    return FALSE;
}

static int
nd_storage_get_own(JSContext *ctx, JSPropertyDescriptor *desc,
                   JSValueConst obj, JSAtom prop)
{
    GHashTable *store = JS_GetOpaque(obj, nd_storage_class_id);
    if (!store) return 0;
    const char *name = JS_AtomToCString(ctx, prop);
    if (!name) return 0;
    if (nd_storage_name_is_builtin(name)) {
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
        nd_storage_maybe_dirty(ctx, store);
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
    if (removed) nd_storage_maybe_dirty(ctx, store);
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
nd_storage_maybe_dirty(JSContext *ctx, GHashTable *store)
{
    nd_js *js = js_from_ctx(ctx);
    if (js && store == js->local_storage)
        js->local_storage_dirty = TRUE;
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
        nd_storage_maybe_dirty(ctx, store);
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
        nd_storage_maybe_dirty(ctx, store);
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
        nd_storage_maybe_dirty(ctx, store);
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
        { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
        { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
    (void)rt;
    nd_node *n = JS_GetOpaque(val, nd_element_class_id);
    if (n) {
        n->js_wrapper = NULL;
        n->js_invalidate = NULL;
    }
}

static void
nd_element_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    nd_node *n = JS_GetOpaque(val, nd_element_class_id);
    if (!n) return;
    for (nd_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->js_wrapper) {
            JSValue cv = JS_MKPTR(JS_TAG_OBJECT, c->js_wrapper);
            JS_MarkValue(rt, cv, mark_func);
        }
    }
}

static JSClassDef nd_element_class = {
    .class_name = "Element",
    .finalizer  = nd_element_finalizer,
    .gc_mark    = nd_element_gc_mark,
};

static inline gboolean
nd_listener_is_tombstoned(const nd_listener *l)
{
    return !l || l->type == NULL;
}

static void
nd_listener_tombstone(JSContext *ctx, nd_listener *l)
{
    if (!l || nd_listener_is_tombstoned(l)) return;
    JS_FreeValue(ctx, l->cb);
    l->cb = JS_UNDEFINED;
    g_free(l->type);
    l->type = NULL;
}

static void
nd_listeners_sweep(nd_js *js)
{
    if (!js || !js->listeners || js->dispatch_depth > 0) return;
    guint w = 0;
    for (guint r = 0; r < js->listeners->len; r++) {
        nd_listener *l = g_ptr_array_index(js->listeners, r);
        if (nd_listener_is_tombstoned(l)) {
            g_free(l);
            continue;
        }
        js->listeners->pdata[w++] = l;
    }
    g_ptr_array_set_size(js->listeners, w);
}

static void
nd_invalidate_wrapper(nd_node *n)
{
    if (!n) return;
    if (n->js_wrapper) {
        JSValue obj = JS_MKPTR(JS_TAG_OBJECT, n->js_wrapper);
        JS_SetOpaque(obj, NULL);
        n->js_wrapper = NULL;
    }
    n->js_invalidate = NULL;

    nd_js *js = g_active_js;
    if (js && js->listeners) {
        for (guint i = 0; i < js->listeners->len; i++) {
            nd_listener *l = g_ptr_array_index(js->listeners, i);
            if (!nd_listener_is_tombstoned(l) && l->target == n)
                nd_listener_tombstone(js->ctx, l);
        }
        nd_listeners_sweep(js);
    }
}

static inline void
nd_node_arm_js_invalidate(nd_node *n)
{
    if (n && !n->js_invalidate) n->js_invalidate = nd_invalidate_wrapper;
}

static JSValue
nd_make_element(JSContext *ctx, const nd_node *cnode)
{
    if (!cnode) return JS_NULL;
    nd_node *node = (nd_node *)cnode;
    if (node->js_wrapper) {
        JSValue cached = JS_MKPTR(JS_TAG_OBJECT, node->js_wrapper);
        return JS_DupValue(ctx, cached);
    }
    JSValue obj = JS_NewObjectClass(ctx, nd_element_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, node);
    node->js_wrapper = JS_VALUE_GET_PTR(obj);
    node->js_invalidate = nd_invalidate_wrapper;
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
        { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
    return JS_NewInt32(ctx, nd_parse_int(v, 0, G_MININT, G_MAXINT));
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
        { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
        { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
    if (ilen > G_MAXSIZE - head - tail - 1) {
        JS_FreeCString(ctx, ins);
        return JS_ThrowRangeError(ctx, "insertData: combined string too large");
    }
    char *merged = g_malloc(head + ilen + tail + 1);
    if (head) memcpy(merged, n->text, head);
    memcpy(merged + head, ins, ilen);
    if (tail) memcpy(merged + head + ilen, p, tail);
    merged[head + ilen + tail] = '\0';
    g_free(n->text);
    n->text = merged;
    JS_FreeCString(ctx, ins);
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
    } else if (js_from_ctx(ctx)) {
        g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, tail);
    }
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
        char *old_copy = n->text ? g_strdup(n->text) : g_strdup("");
        g_free(n->text);
        n->text = g_strdup(s);
        JS_FreeCString(ctx, s);
        nd_js *_j = js_from_ctx(ctx);
        if (_j) {
            _j->mutated = TRUE;
            if (n->kind == ND_NODE_TEXT)
                nd_js_record_character_data(_j, n, old_copy);
        }
        g_free(old_copy);
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
        nd_js_set_attr_recorded(js_from_ctx(ctx), n, "id", s);
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
        nd_js_set_attr_recorded(js_from_ctx(ctx), n, "class", s);
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

static gboolean
nd_js_has_childlist_observer(const nd_js *js, const nd_node *target)
{
    if (!js || !js->mutation_observers || !target) return FALSE;
    for (guint oi = 0; oi < js->mutation_observers->len; oi++) {
        nd_mut_observer *o = g_ptr_array_index(js->mutation_observers, oi);
        if (!o || o->disconnected || !o->targets) continue;
        for (guint ti = 0; ti < o->targets->len; ti++) {
            const nd_mut_target *t = &g_array_index(o->targets, nd_mut_target, ti);
            if (!t->child_list) continue;
            if (nd_mut_target_covers(t, (nd_node *)target)) return TRUE;
        }
    }
    return FALSE;
}

static void
nd_element_clear_children_recorded(nd_js *js, nd_node *n)
{
    if (!js || !nd_js_has_childlist_observer(js, n)) {
        nd_element_clear_children(n);
        return;
    }
    nd_node *c = n->first_child;
    while (c) {
        nd_node *next = c->next_sibling;
        nd_node *saved_prev = c->prev_sibling;
        nd_node *saved_next = c->next_sibling;
        nd_node_remove(c);
        g_ptr_array_add(js->orphan_nodes, c);
        nd_js_record_child_change(js, n, NULL, c, saved_prev, saved_next);
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
    nd_js *_j = js_from_ctx(ctx);
    nd_element_clear_children_recorded(_j, n);
    if (*s) {
        nd_node *added = nd_node_new_text(g_strdup(s));
        nd_node_append_child(n, added);
        if (_j) nd_js_record_child_change(_j, n, added, NULL,
                                          added->prev_sibling, added->next_sibling);
    }
    JS_FreeCString(ctx, s);
    if (_j) _j->mutated = TRUE;
    return JS_UNDEFINED;
}

static JSValue
nd_element_set_innerText(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    nd_node *n = nd_unwrap_element_mut(this_val);
    if (!n || n->kind != ND_NODE_ELEMENT) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (!s) return JS_UNDEFINED;
    nd_element_clear_children(n);
    const char *p = s;
    while (*p) {
        const char *nl = strchr(p, '\n');
        gsize seg = nl ? (gsize)(nl - p) : strlen(p);
        if (seg > 0)
            nd_node_append_child(n, nd_node_new_text(g_strndup(p, seg)));
        if (!nl) break;
        nd_node_append_child(n, nd_node_new_element(g_strdup("br")));
        p = nl + 1;
    }
    JS_FreeCString(ctx, s);
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

static JSValue
nd_element_set_innerHTML(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    nd_node *n = nd_unwrap_element_mut(this_val);
    if (!n || n->kind != ND_NODE_ELEMENT) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (!s) return JS_UNDEFINED;
    nd_js *_j = js_from_ctx(ctx);
    nd_element_clear_children_recorded(_j, n);
    nd_node *fragment = nd_html_parse_fragment_in(n->name, s, -1);
    JS_FreeCString(ctx, s);
    if (fragment) {
        nd_node *c = fragment->first_child;
        while (c) {
            nd_node *next = c->next_sibling;
            nd_node_remove(c);
            nd_node_append_child(n, c);
            if (_j) nd_js_record_child_change(_j, n, c, NULL,
                                              c->prev_sibling, c->next_sibling);
            c = next;
        }
        nd_node_free(fragment);
    }
    if (_j) _j->mutated = TRUE;
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
    const char *ctx_tag = (self->parent && self->parent->kind == ND_NODE_ELEMENT)
                          ? self->parent->name : NULL;
    nd_node *fragment = nd_html_parse_fragment_in(ctx_tag, s, -1);
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
        if (js_from_ctx(ctx)) {
            g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, self);
            { nd_js *_j2 = js_from_ctx(ctx); if (_j2) _j2->mutated = TRUE; }
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
    nd_js *_j = js_from_ctx(ctx);
    nd_element_clear_children_recorded(_j, self);
    for (int i = 0; i < argc; i++) {
        nd_node *child = nd_unwrap_element_mut(argv[i]);
        if (child) {
            if (_j)
                g_ptr_array_remove_fast(_j->orphan_nodes, child);
            nd_node_append_child(self, child);
            if (_j) nd_js_record_child_change(_j, self, child, NULL,
                                              child->prev_sibling, child->next_sibling);
        } else {
            const char *txt = JS_ToCString(ctx, argv[i]);
            if (txt) {
                nd_node *added = nd_node_new_text(g_strdup(txt));
                nd_node_append_child(self, added);
                JS_FreeCString(ctx, txt);
                if (_j) nd_js_record_child_change(_j, self, added, NULL,
                                                  added->prev_sibling, added->next_sibling);
            }
        }
    }
    if (_j) _j->mutated = TRUE;
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

static void
nd_listener_parse_options(JSContext *ctx, JSValueConst opts,
                          gboolean *capture, gboolean *once)
{
    *capture = FALSE;
    *once = FALSE;
    if (JS_IsBool(opts)) {
        *capture = JS_ToBool(ctx, opts) ? TRUE : FALSE;
        return;
    }
    if (JS_IsObject(opts)) {
        JSValue cap = JS_GetPropertyStr(ctx, opts, "capture");
        if (!JS_IsUndefined(cap)) *capture = JS_ToBool(ctx, cap) ? TRUE : FALSE;
        JS_FreeValue(ctx, cap);
        JSValue oc = JS_GetPropertyStr(ctx, opts, "once");
        if (!JS_IsUndefined(oc)) *once = JS_ToBool(ctx, oc) ? TRUE : FALSE;
        JS_FreeValue(ctx, oc);
    }
}

static JSValue
nd_element_addEventListener(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n || argc < 2 || !js_from_ctx(ctx)) return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    if (!JS_IsFunction(ctx, argv[1])) { JS_FreeCString(ctx, type); return JS_UNDEFINED; }
    gboolean capture = FALSE, once = FALSE;
    if (argc >= 3) nd_listener_parse_options(ctx, argv[2], &capture, &once);
    nd_listener *l = g_new0(nd_listener, 1);
    l->target = n;
    l->type   = g_strdup(type);
    l->cb     = JS_DupValue(ctx, argv[1]);
    l->capture = capture;
    l->once    = once;
    g_ptr_array_add(js_from_ctx(ctx)->listeners, l);
    nd_node_arm_js_invalidate((nd_node *)n);
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

static JSValue
nd_element_removeEventListener(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n || argc < 2 || !js_from_ctx(ctx)) return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    nd_js *js = js_from_ctx(ctx);
    for (guint i = 0; i < js->listeners->len; i++) {
        nd_listener *l = g_ptr_array_index(js->listeners, i);
        if (nd_listener_is_tombstoned(l)) continue;
        if (l->target == n && strcmp(l->type, type) == 0 &&
            JS_VALUE_GET_PTR(l->cb) == JS_VALUE_GET_PTR(argv[1])) {
            nd_listener_tombstone(ctx, l);
            break;
        }
    }
    nd_listeners_sweep(js);
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

typedef struct nd_js_fetch_state {
    JSContext *ctx;
    nd_js     *js;
    JSValue    resolve;
    JSValue    reject;
    char      *requested_url;
} nd_js_fetch_state;

static void
nd_js_fetch_state_free(nd_js_fetch_state *st)
{
    if (!st) return;
    if (st->ctx) {
        JS_FreeValue(st->ctx, st->resolve);
        JS_FreeValue(st->ctx, st->reject);
    }
    g_free(st->requested_url);
    g_free(st);
}

static gboolean
cors_allows(const char *doc_url, const char *resp_url, const char *cors_header)
{
    if (nd_url_same_origin(doc_url, resp_url)) return TRUE;
    if (!cors_header || !*cors_header) return FALSE;
    char *trimmed = g_strdup(cors_header);
    g_strstrip(trimmed);
    char *doc_origin = nd_url_origin_from(doc_url);
    gboolean ok = doc_origin &&
                  g_ascii_strcasecmp(trimmed, doc_origin) == 0;
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
    if (!st->ctx) {
        nd_response_free(resp);
        g_clear_error(&err);
        nd_js_fetch_state_free(st);
        return;
    }
    nd_budget_guard bg;
    nd_js_budget_push(st->js, &bg);
    if (st->js && st->js->pending_fetches)
        g_ptr_array_remove_fast(st->js->pending_fetches, st);
    if (!resp || resp->error) {
        const char *msg = resp ? resp->error :
                                (err ? err->message : "fetch failed");
        JSValue m = JS_NewString(st->ctx, msg ? msg : "fetch failed");
        JS_Call(st->ctx, st->reject, JS_UNDEFINED, 1, &m);
        JS_FreeValue(st->ctx, m);
        nd_response_free(resp);
        g_clear_error(&err);
    } else {
        gboolean allow = cors_allows(st->js->current_url, resp->final_url,
                                     resp->cors_allow_origin);
        JSValue r = JS_NewObject(st->ctx);
        JS_SetPropertyStr(st->ctx, r, "ok",
            JS_NewBool(st->ctx, allow && resp->status >= 200 && resp->status < 300));
        long status = allow ? resp->status : 0;
        JS_SetPropertyStr(st->ctx, r, "status",
            JS_NewInt32(st->ctx, (int)status));
        const char *status_text = "";
        switch (status) {
        case 200: status_text = "OK"; break;
        case 201: status_text = "Created"; break;
        case 202: status_text = "Accepted"; break;
        case 204: status_text = "No Content"; break;
        case 301: status_text = "Moved Permanently"; break;
        case 302: status_text = "Found"; break;
        case 303: status_text = "See Other"; break;
        case 304: status_text = "Not Modified"; break;
        case 307: status_text = "Temporary Redirect"; break;
        case 308: status_text = "Permanent Redirect"; break;
        case 400: status_text = "Bad Request"; break;
        case 401: status_text = "Unauthorized"; break;
        case 403: status_text = "Forbidden"; break;
        case 404: status_text = "Not Found"; break;
        case 405: status_text = "Method Not Allowed"; break;
        case 408: status_text = "Request Timeout"; break;
        case 410: status_text = "Gone"; break;
        case 429: status_text = "Too Many Requests"; break;
        case 500: status_text = "Internal Server Error"; break;
        case 502: status_text = "Bad Gateway"; break;
        case 503: status_text = "Service Unavailable"; break;
        case 504: status_text = "Gateway Timeout"; break;
        }
        JS_SetPropertyStr(st->ctx, r, "statusText", JS_NewString(st->ctx, status_text));
        JS_SetPropertyStr(st->ctx, r, "url",
            JS_NewString(st->ctx, resp->final_url ? resp->final_url : ""));
        JS_SetPropertyStr(st->ctx, r, "type",
            JS_NewString(st->ctx, allow ? "basic" : "opaque"));
        JS_SetPropertyStr(st->ctx, r, "redirected",
            JS_NewBool(st->ctx, resp->redirect_count > 0));
        JS_SetPropertyStr(st->ctx, r, "redirectCount",
            JS_NewInt32(st->ctx, allow ? resp->redirect_count : 0));
        JS_SetPropertyStr(st->ctx, r, "bodyUsed", JS_FALSE);
        const char *body_data = "";
        gsize body_data_len = 0;
        if (allow && resp->body && resp->body->len > 0) {
            body_data = (const char *)resp->body->data;
            body_data_len = resp->body->len;
        }
        JS_SetPropertyStr(st->ctx, r, "body",
            JS_NewStringLen(st->ctx, body_data, body_data_len));
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
        nd_response_free(resp);
    }
    nd_drain_mutations(st->js);
    nd_js_budget_pop(st->js, &bg);
    nd_js_fetch_state_free(st);
}

static JSValue
nd_js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!js_from_ctx(ctx) || argc < 1)
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
    st->js = js_from_ctx(ctx);
    st->resolve = resolving[0];
    st->reject  = resolving[1];
    st->requested_url = g_strdup(url);
    if (st->js && st->js->pending_fetches)
        g_ptr_array_add(st->js->pending_fetches, st);
    const char *top = st->js ? st->js->current_url : NULL;
    if (method && g_ascii_strcasecmp(method, "POST") == 0) {
        nd_net_post_async(url, top, body, body_len,
                          content_type ? content_type : "text/plain",
                          NULL, nd_on_js_fetch_done, st);
    } else {
        nd_net_fetch_async(url, top, NULL, nd_on_js_fetch_done, st);
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

static JSValue
nd_make_ctor(JSContext *ctx, JSCFunction *fn, const char *name, int argc)
{
    JSValue func = JS_NewCFunction2(ctx, fn, name, argc,
                                    JS_CFUNC_constructor_or_func, 0);
    JSValue proto = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, proto, "constructor",
                              JS_DupValue(ctx, func),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, func, "prototype", proto, JS_PROP_WRITABLE);
    return func;
}

static void
nd_bind_ctor(JSContext *ctx, JSValueConst obj, const char *name,
             JSCFunction *fn, int argc)
{
    JS_SetPropertyStr(ctx, obj, name, nd_make_ctor(ctx, fn, name, argc));
}

typedef struct nd_fn_def { const char *name; int argc; } nd_fn_def;

static void
nd_bind_fns(JSContext *ctx, JSValueConst obj, JSCFunction *fn,
            const nd_fn_def *defs, gsize n)
{
    for (gsize i = 0; i < n; i++)
        nd_bind_fn(ctx, obj, defs[i].name, fn, defs[i].argc);
}

static void
nd_bind_ctors(JSContext *ctx, JSValueConst obj, JSCFunction *fn,
              const nd_fn_def *defs, gsize n)
{
    for (gsize i = 0; i < n; i++)
        nd_bind_ctor(ctx, obj, defs[i].name, fn, defs[i].argc);
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
nd_microtask_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)argc;
    return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
}

static JSValue
nd_window_queue_microtask(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    JSValueConst args[1] = { argv[0] };
    JS_EnqueueJob(ctx, nd_microtask_job, 1, args);
    return JS_UNDEFINED;
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
    nd_js_emit(js_from_ctx(ctx), "[error]", ctx, argc, argv);
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

static char *
nd_computed_lookup(JSContext *ctx, const nd_node *n, const char *name)
{
    if (!n || !name) return NULL;
    nd_js *js = js_from_ctx(ctx);
    int pid = nd_css_prop_id(name);
    if (pid >= 0 && js && js->style_table) {
        const nd_style *s = g_hash_table_lookup(js->style_table, n);
        if (s && s->values[pid])
            return nd_css_value_serialize(s->values[pid]);
    }
    const char *style = nd_element_get_attr(n, "style");
    if (style) {
        char *v = nd_inline_style_get(style, name);
        if (v) return v;
    }
    return NULL;
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
    char *val = nd_computed_lookup(ctx, n, name);
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
    if (argc >= 1) {
        const nd_node *n = nd_unwrap_element(argv[0]);
        if (n) {
            for (int p = 0; p < ND_CSS_PROP_COUNT; p++) {
                const char *kname = nd_css_prop_name((nd_css_prop)p);
                if (!kname) continue;
                char *val = nd_computed_lookup(ctx, n, kname);
                if (!val) continue;
                JS_SetPropertyStr(ctx, cs, kname, JS_NewString(ctx, val));
                GString *camel = g_string_new(NULL);
                gboolean upper = FALSE;
                for (const char *q = kname; *q; q++) {
                    if (*q == '-') { upper = TRUE; continue; }
                    g_string_append_c(camel, upper ? g_ascii_toupper(*q) : *q);
                    upper = FALSE;
                }
                if (strcmp(camel->str, kname) != 0)
                    JS_SetPropertyStr(ctx, cs, camel->str, JS_NewString(ctx, val));
                g_string_free(camel, TRUE);
                g_free(val);
            }
        }
    }
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
    if (argc < 1 || !js_from_ctx(ctx) || !js_from_ctx(ctx)->nav_cb) return JS_NULL;
    const char *url = JS_ToCString(ctx, argv[0]);
    if (url) {
        js_from_ctx(ctx)->nav_cb(url, FALSE, js_from_ctx(ctx)->nav_user_data);
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
            resolved = nd_url_resolve(base, raw);
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
    const char *authority_start = p ? p + 3 : resolved;
    const char *path_start = authority_start;
    while (*path_start && *path_start != '/' && *path_start != '?' && *path_start != '#')
        path_start++;
    const char *host_start = authority_start;
    for (const char *c = authority_start; c < path_start; c++)
        if (*c == '@') { host_start = c + 1; break; }
    const char *port_start = NULL;
    for (const char *c = host_start; c < path_start; c++)
        if (*c == ':') { port_start = c; break; }
    char *host = g_strndup(host_start, (gsize)(path_start - host_start));
    JS_SetPropertyStr(ctx, obj, "host", JS_NewString(ctx, host));
    g_free(host);
    char *hostname = g_strndup(host_start,
                               (gsize)((port_start ? port_start : path_start) - host_start));
    JS_SetPropertyStr(ctx, obj, "hostname", JS_NewString(ctx, hostname));
    g_free(hostname);
    gsize scheme_len = p ? (gsize)(p + 3 - resolved) : 0;
    gsize host_part_len = (gsize)(path_start - host_start);
    if (scheme_len > G_MAXSIZE - host_part_len - 1) {
        JS_SetPropertyStr(ctx, obj, "origin", JS_NewString(ctx, ""));
    } else {
        char *origin = g_malloc(scheme_len + host_part_len + 1);
        if (scheme_len) memcpy(origin, resolved, scheme_len);
        memcpy(origin + scheme_len, host_start, host_part_len);
        origin[scheme_len + host_part_len] = '\0';
        JS_SetPropertyStr(ctx, obj, "origin",   JS_NewString(ctx, origin));
        g_free(origin);
    }
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
    if (!js_from_ctx(ctx)) return JS_NULL;
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
    g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, el);
    return nd_make_element(ctx, el);
}

static JSValue
nd_window_audio_ctor(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!js_from_ctx(ctx)) return JS_NULL;
    nd_node *el = nd_node_new_element(g_strdup("audio"));
    if (argc >= 1) {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s) { nd_element_set_attr(el, "src", s); JS_FreeCString(ctx, s); }
    }
    g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, el);
    return nd_make_element(ctx, el);
}

static JSValue
nd_window_option_ctor(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!js_from_ctx(ctx)) return JS_NULL;
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
    g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, el);
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
    if (js_from_ctx(ctx)) g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, doc);
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
    nd_js     *js;
    JSValue    obj;
    char      *method;
    char      *url;
    GPtrArray *request_headers;
} nd_xhr_state;

static void
nd_xhr_state_free(nd_xhr_state *st)
{
    if (!st) return;
    if (st->ctx) JS_FreeValue(st->ctx, st->obj);
    g_free(st->method);
    g_free(st->url);
    if (st->request_headers) g_ptr_array_free(st->request_headers, TRUE);
    g_free(st);
}

static void
nd_on_xhr_done(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    nd_xhr_state *st = user_data;
    GError *err = NULL;
    nd_response *resp = nd_net_fetch_finish(result, &err);
    if (!st->ctx) {
        nd_response_free(resp);
        g_clear_error(&err);
        nd_xhr_state_free(st);
        return;
    }
    nd_budget_guard bg;
    nd_js_budget_push(st->js, &bg);
    if (st->js && st->js->pending_xhrs)
        g_ptr_array_remove_fast(st->js->pending_xhrs, st);
    JSContext *ctx = st->ctx;
    if (resp && !err) {
        gboolean allow = cors_allows(js_from_ctx(ctx) ? js_from_ctx(ctx)->current_url : NULL,
                                     resp->final_url, resp->cors_allow_origin);
        JS_SetPropertyStr(ctx, st->obj, "status",
                          JS_NewInt32(ctx, allow ? (int)resp->status : 0));
        JS_SetPropertyStr(ctx, st->obj, "statusText",
                          JS_NewString(ctx, allow && resp->status == 200 ? "OK" : ""));
        const char *body = (allow && resp->body) ? (const char *)resp->body->data : "";
        gsize blen = (allow && resp->body) ? resp->body->len : 0;
        JS_SetPropertyStr(ctx, st->obj, "responseText",
                          JS_NewStringLen(ctx, body, blen));
        JSValue rt_v = JS_GetPropertyStr(ctx, st->obj, "responseType");
        const char *rt = JS_ToCString(ctx, rt_v);
        JS_FreeValue(ctx, rt_v);
        if (rt && strcmp(rt, "json") == 0 && blen > 0) {
            JSValue parsed = JS_ParseJSON(ctx, body, blen, "<XHR response>");
            if (JS_IsException(parsed)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_SetPropertyStr(ctx, st->obj, "response", JS_NULL);
            } else {
                JS_SetPropertyStr(ctx, st->obj, "response", parsed);
            }
        } else {
            JS_SetPropertyStr(ctx, st->obj, "response",
                              JS_NewStringLen(ctx, body, blen));
        }
        if (rt) JS_FreeCString(ctx, rt);
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
    nd_response_free(resp);
    g_clear_error(&err);
    nd_js_budget_pop(st->js, &bg);
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
    JS_SetPropertyStr(ctx, this_val, "_headers", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, this_val, "readyState", JS_NewInt32(ctx, 1));
    return JS_UNDEFINED;
}

static JSValue
nd_xhr_setRequestHeader(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    const char *name  = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (name && value) {
        char *line = g_strdup_printf("%s: %s", name, value);
        JSValue arr = JS_GetPropertyStr(ctx, this_val, "_headers");
        if (!JS_IsArray(arr)) {
            JS_FreeValue(ctx, arr);
            arr = JS_NewArray(ctx);
            JS_SetPropertyStr(ctx, this_val, "_headers", JS_DupValue(ctx, arr));
        }
        uint32_t len = 0;
        JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
        JS_ToUint32(ctx, &len, lv);
        JS_FreeValue(ctx, lv);
        JS_SetPropertyUint32(ctx, arr, len, JS_NewString(ctx, line));
        JS_FreeValue(ctx, arr);
        g_free(line);
    }
    if (name)  JS_FreeCString(ctx, name);
    if (value) JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

static JSValue
nd_xhr_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue url_v = JS_GetPropertyStr(ctx, this_val, "_url");
    const char *url = JS_ToCString(ctx, url_v);
    JS_FreeValue(ctx, url_v);
    if (!url) return JS_UNDEFINED;
    JSValue method_v = JS_GetPropertyStr(ctx, this_val, "_method");
    const char *method = JS_ToCString(ctx, method_v);
    JS_FreeValue(ctx, method_v);
    gboolean is_post = method && g_ascii_strcasecmp(method, "POST") == 0;
    char *body = NULL;
    gsize body_len = 0;
    if (is_post && argc >= 1 && JS_IsString(argv[0])) {
        const char *b = JS_ToCString(ctx, argv[0]);
        if (b) { body = g_strdup(b); body_len = strlen(b); JS_FreeCString(ctx, b); }
    }
    nd_xhr_state *st = g_new0(nd_xhr_state, 1);
    st->ctx = ctx;
    st->js  = js_from_ctx(ctx);
    st->obj = JS_DupValue(ctx, this_val);
    st->url = g_strdup(url);
    if (method) st->method = g_strdup(method);

    JSValue headers_arr = JS_GetPropertyStr(ctx, this_val, "_headers");
    GPtrArray *hdrs = NULL;
    if (JS_IsArray(headers_arr)) {
        uint32_t hlen = 0;
        JSValue lv = JS_GetPropertyStr(ctx, headers_arr, "length");
        JS_ToUint32(ctx, &hlen, lv);
        JS_FreeValue(ctx, lv);
        if (hlen > 0) {
            hdrs = g_ptr_array_new_with_free_func(g_free);
            for (uint32_t i = 0; i < hlen; i++) {
                JSValue ev = JS_GetPropertyUint32(ctx, headers_arr, i);
                const char *s = JS_ToCString(ctx, ev);
                if (s) {
                    g_ptr_array_add(hdrs, g_strdup(s));
                    JS_FreeCString(ctx, s);
                }
                JS_FreeValue(ctx, ev);
            }
        }
    }
    JS_FreeValue(ctx, headers_arr);
    if (hdrs) {
        g_ptr_array_add(hdrs, g_strdup("X-Requested-With: XMLHttpRequest"));
    } else {
        hdrs = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(hdrs, g_strdup("X-Requested-With: XMLHttpRequest"));
    }
    st->request_headers = hdrs;

    if (st->js && st->js->pending_xhrs)
        g_ptr_array_add(st->js->pending_xhrs, st);
    JS_FreeCString(ctx, url);
    if (method) JS_FreeCString(ctx, method);

    GPtrArray *hdr_terminated = g_ptr_array_new();
    for (guint i = 0; i < hdrs->len; i++)
        g_ptr_array_add(hdr_terminated, hdrs->pdata[i]);
    g_ptr_array_add(hdr_terminated, NULL);

    nd_net_request_async(st->url,
                         st->js ? st->js->current_url : NULL,
                         is_post ? "POST" : (st->method ? st->method : "GET"),
                         body, body_len,
                         is_post ? "application/x-www-form-urlencoded" : NULL,
                         (const char *const *)hdr_terminated->pdata,
                         NULL, nd_on_xhr_done, st);
    g_ptr_array_free(hdr_terminated, TRUE);
    g_free(body);
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
        { "getResponseHeader", 1 },
        { "getAllResponseHeaders", 0 },
        { "addEventListener", 2 }, { "removeEventListener", 2 },
        { "abort", 0 },
    };
    nd_bind_fn(ctx, obj, "open",             nd_xhr_open, 5);
    nd_bind_fn(ctx, obj, "send",             nd_xhr_send, 1);
    nd_bind_fn(ctx, obj, "setRequestHeader", nd_xhr_setRequestHeader, 2);
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

static void nd_form_collect_controls(const nd_node *form, JSContext *ctx,
                                     JSValue arr, uint32_t *idx);

static JSValue
nd_form_data_delete(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_UNDEFINED;
    JSValue entries = nd_form_data_method(ctx, this_val, 0, NULL);
    uint32_t len = 0;
    JSValue lv = JS_GetPropertyStr(ctx, entries, "length");
    JS_ToUint32(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    JSValue kept = JS_NewArray(ctx);
    uint32_t out = 0;
    for (uint32_t i = 0; i < len; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, entries, i);
        JSValue k = JS_GetPropertyUint32(ctx, pair, 0);
        const char *ks = JS_ToCString(ctx, k);
        if (ks && strcmp(ks, name) != 0) {
            JS_SetPropertyUint32(ctx, kept, out++, JS_DupValue(ctx, pair));
        }
        if (ks) JS_FreeCString(ctx, ks);
        JS_FreeValue(ctx, k);
        JS_FreeValue(ctx, pair);
    }
    JS_SetPropertyStr(ctx, this_val, "_entries", kept);
    JS_FreeValue(ctx, entries);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue
nd_form_data_forEach(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    JSValue entries = nd_form_data_method(ctx, this_val, 0, NULL);
    uint32_t len = 0;
    JSValue lv = JS_GetPropertyStr(ctx, entries, "length");
    JS_ToUint32(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    for (uint32_t i = 0; i < len; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, entries, i);
        JSValue k = JS_GetPropertyUint32(ctx, pair, 0);
        JSValue v = JS_GetPropertyUint32(ctx, pair, 1);
        JSValueConst args[3] = { v, k, this_val };
        JSValue r = JS_Call(ctx, argv[0], JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, k);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, pair);
    }
    JS_FreeValue(ctx, entries);
    return JS_UNDEFINED;
}

static void
nd_form_data_populate_from_form(JSContext *ctx, JSValueConst fd, const nd_node *form)
{
    JSValue controls = JS_NewArray(ctx);
    uint32_t i = 0;
    nd_form_collect_controls(form, ctx, controls, &i);
    uint32_t len = 0;
    JSValue lv = JS_GetPropertyStr(ctx, controls, "length");
    JS_ToUint32(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    for (uint32_t k = 0; k < len; k++) {
        JSValue elv = JS_GetPropertyUint32(ctx, controls, k);
        const nd_node *el = nd_unwrap_element(elv);
        if (!el) { JS_FreeValue(ctx, elv); continue; }
        const char *name = nd_element_get_attr(el, "name");
        if (!name || !*name) { JS_FreeValue(ctx, elv); continue; }
        const char *type = nd_element_get_attr(el, "type");
        if (type && (g_ascii_strcasecmp(type, "submit") == 0 ||
                     g_ascii_strcasecmp(type, "button") == 0 ||
                     g_ascii_strcasecmp(type, "reset") == 0 ||
                     g_ascii_strcasecmp(type, "image") == 0 ||
                     g_ascii_strcasecmp(type, "file") == 0)) {
            JS_FreeValue(ctx, elv); continue;
        }
        if (type && (g_ascii_strcasecmp(type, "checkbox") == 0 ||
                     g_ascii_strcasecmp(type, "radio") == 0)) {
            if (!nd_element_get_attr(el, "checked")) { JS_FreeValue(ctx, elv); continue; }
        }
        const char *value = nd_element_get_attr(el, "value");
        JSValueConst args[2] = {
            JS_NewString(ctx, name),
            JS_NewString(ctx, value ? value : ""),
        };
        JSValue r = nd_form_data_append(ctx, fd, 2, args);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, (JSValue)args[0]);
        JS_FreeValue(ctx, (JSValue)args[1]);
        JS_FreeValue(ctx, elv);
    }
    JS_FreeValue(ctx, controls);
}

static JSValue
nd_window_form_data_ctor(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "_entries", JS_NewArray(ctx));
    nd_bind_fn(ctx, obj, "append",  nd_form_data_append, 2);
    nd_bind_fn(ctx, obj, "set",     nd_form_data_append, 2);
    nd_bind_fn(ctx, obj, "get",     nd_form_data_get,    1);
    nd_bind_fn(ctx, obj, "getAll",  nd_form_data_method, 1);
    nd_bind_fn(ctx, obj, "has",     nd_form_data_has,    1);
    nd_bind_fn(ctx, obj, "delete",  nd_form_data_delete, 1);
    nd_bind_fn(ctx, obj, "entries", nd_form_data_method, 0);
    nd_bind_fn(ctx, obj, "keys",    nd_form_data_method, 0);
    nd_bind_fn(ctx, obj, "values",  nd_form_data_method, 0);
    nd_bind_fn(ctx, obj, "forEach", nd_form_data_forEach, 1);
    if (argc >= 1) {
        const nd_node *form = nd_unwrap_element(argv[0]);
        if (form && form->name && strcmp(form->name, "form") == 0)
            nd_form_data_populate_from_form(ctx, obj, form);
    }
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

static JSClassID nd_mut_observer_class_id;

static void
nd_mut_target_clear(nd_mut_target *t)
{
    if (!t) return;
    if (t->attribute_filter) {
        g_ptr_array_free(t->attribute_filter, TRUE);
        t->attribute_filter = NULL;
    }
}

static void
nd_mut_observer_targets_clear(nd_mut_observer *o)
{
    if (!o || !o->targets) return;
    for (guint i = 0; i < o->targets->len; i++)
        nd_mut_target_clear(&g_array_index(o->targets, nd_mut_target, i));
    g_array_set_size(o->targets, 0);
}

static void
nd_mut_observer_free(nd_js *js, nd_mut_observer *o)
{
    if (!o) return;
    if (js && js->ctx) JS_FreeValue(js->ctx, o->cb);
    if (o->targets) {
        nd_mut_observer_targets_clear(o);
        g_array_free(o->targets, TRUE);
    }
    if (o->records) g_ptr_array_free(o->records, TRUE);
    g_free(o);
}

static void
nd_mut_observer_finalizer(JSRuntime *rt, JSValue val)
{
    nd_mut_observer *o = JS_GetOpaque(val, nd_mut_observer_class_id);
    if (!o) return;
    nd_js *js = JS_GetRuntimeOpaque(rt);
    if (js && js->mutation_observers)
        g_ptr_array_remove_fast(js->mutation_observers, o);
    nd_mut_observer_free(js, o);
}

static JSClassDef nd_mut_observer_class = {
    "MutationObserver",
    .finalizer = nd_mut_observer_finalizer,
};

static nd_mut_observer *
nd_unwrap_mut_observer(JSValueConst v)
{
    return JS_GetOpaque(v, nd_mut_observer_class_id);
}

typedef struct nd_mut_record_data {
    char    *type;
    nd_node *target;
    GPtrArray *added;
    GPtrArray *removed;
    nd_node *previous_sibling;
    nd_node *next_sibling;
    char    *attribute_name;
    char    *old_value;
} nd_mut_record_data;

static void
nd_mut_record_free(gpointer p)
{
    nd_mut_record_data *r = p;
    if (!r) return;
    g_free(r->type);
    if (r->added)   g_ptr_array_free(r->added, TRUE);
    if (r->removed) g_ptr_array_free(r->removed, TRUE);
    g_free(r->attribute_name);
    g_free(r->old_value);
    g_free(r);
}

static gboolean
nd_mut_target_covers(const nd_mut_target *t, nd_node *node)
{
    if (!t || !t->target || !node) return FALSE;
    if (t->target == node) return TRUE;
    if (!t->subtree) return FALSE;
    for (nd_node *p = node->parent; p; p = p->parent)
        if (p == t->target) return TRUE;
    return FALSE;
}

static JSValue
nd_mut_record_to_jsvalue(JSContext *ctx, const nd_mut_record_data *rd)
{
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "type",
        JS_NewString(ctx, rd->type ? rd->type : ""));
    JS_SetPropertyStr(ctx, r, "target",
        rd->target ? nd_make_element(ctx, rd->target) : JS_NULL);
    JSValue added_arr = JS_NewArray(ctx);
    if (rd->added) {
        for (guint k = 0; k < rd->added->len; k++)
            JS_SetPropertyUint32(ctx, added_arr, k,
                nd_make_element(ctx, g_ptr_array_index(rd->added, k)));
    }
    JS_SetPropertyStr(ctx, r, "addedNodes", added_arr);
    JSValue removed_arr = JS_NewArray(ctx);
    if (rd->removed) {
        for (guint k = 0; k < rd->removed->len; k++)
            JS_SetPropertyUint32(ctx, removed_arr, k,
                nd_make_element(ctx, g_ptr_array_index(rd->removed, k)));
    }
    JS_SetPropertyStr(ctx, r, "removedNodes", removed_arr);
    JS_SetPropertyStr(ctx, r, "previousSibling",
        rd->previous_sibling ? nd_make_element(ctx, rd->previous_sibling) : JS_NULL);
    JS_SetPropertyStr(ctx, r, "nextSibling",
        rd->next_sibling ? nd_make_element(ctx, rd->next_sibling) : JS_NULL);
    JS_SetPropertyStr(ctx, r, "attributeName",
        rd->attribute_name ? JS_NewString(ctx, rd->attribute_name) : JS_NULL);
    JS_SetPropertyStr(ctx, r, "attributeNamespace", JS_NULL);
    JS_SetPropertyStr(ctx, r, "oldValue",
        rd->old_value ? JS_NewString(ctx, rd->old_value) : JS_NULL);
    return r;
}

static JSValue
nd_mut_drain_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    nd_js *js = js_from_ctx(ctx);
    if (!js || !js->mutation_observers) return JS_UNDEFINED;
    js->mutation_drain_scheduled = FALSE;
    for (guint oi = 0; oi < js->mutation_observers->len; oi++) {
        nd_mut_observer *o = g_ptr_array_index(js->mutation_observers, oi);
        if (!o || o->disconnected || !o->records || o->records->len == 0) continue;
        GPtrArray *recs = o->records;
        o->records = g_ptr_array_new_with_free_func(nd_mut_record_free);
        JSValue arr = JS_NewArray(ctx);
        for (guint i = 0; i < recs->len; i++) {
            nd_mut_record_data *rd = g_ptr_array_index(recs, i);
            JS_SetPropertyUint32(ctx, arr, i, nd_mut_record_to_jsvalue(ctx, rd));
        }
        g_ptr_array_free(recs, TRUE);
        JSValueConst call_args[2] = { arr, JS_DupValue(ctx, o->wrapper) };
        JSValue ret = JS_Call(ctx, o->cb, o->wrapper, 2, call_args);
        if (JS_IsException(ret)) {
            JSValue ex = JS_GetException(ctx);
            if (js->log_cb) {
                const char *msg = JS_ToCString(ctx, ex);
                if (msg) {
                    char *line = g_strdup_printf("JS error in MutationObserver: %s", msg);
                    js->log_cb(line, js->log_user_data);
                    g_free(line);
                    JS_FreeCString(ctx, msg);
                }
            }
            JS_FreeValue(ctx, ex);
        }
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, arr);
        JS_FreeValue(ctx, (JSValue)call_args[1]);
    }
    return JS_UNDEFINED;
}

static void
nd_mut_schedule_drain(nd_js *js)
{
    if (!js || !js->ctx || js->mutation_drain_scheduled) return;
    js->mutation_drain_scheduled = TRUE;
    JS_EnqueueJob(js->ctx, nd_mut_drain_job, 0, NULL);
}

static gboolean
nd_mut_attribute_filter_matches(const nd_mut_target *t, const char *name)
{
    if (!t->attribute_filter) return TRUE;
    if (!name) return FALSE;
    for (guint i = 0; i < t->attribute_filter->len; i++) {
        const char *f = g_ptr_array_index(t->attribute_filter, i);
        if (f && g_ascii_strcasecmp(f, name) == 0) return TRUE;
    }
    return FALSE;
}

static void
nd_mut_record_emit(nd_js *js, const char *type, nd_node *target,
                   nd_node *added, nd_node *removed,
                   nd_node *previous_sibling, nd_node *next_sibling,
                   const char *attr_name, const char *old_value)
{
    if (!js || !js->mutation_observers || !target) return;
    gboolean wants_child = (g_strcmp0(type, "childList") == 0);
    gboolean wants_attr  = (g_strcmp0(type, "attributes") == 0);
    gboolean wants_cdata = (g_strcmp0(type, "characterData") == 0);
    if (wants_child && !added && !removed) return;
    for (guint oi = 0; oi < js->mutation_observers->len; oi++) {
        nd_mut_observer *o = g_ptr_array_index(js->mutation_observers, oi);
        if (!o || o->disconnected || !o->targets) continue;
        for (guint ti = 0; ti < o->targets->len; ti++) {
            const nd_mut_target *t = &g_array_index(o->targets, nd_mut_target, ti);
            if (!nd_mut_target_covers(t, target)) continue;
            if (wants_child && !t->child_list) continue;
            if (wants_attr  && !t->attributes)  continue;
            if (wants_cdata && !t->character_data) continue;
            if (wants_attr && !nd_mut_attribute_filter_matches(t, attr_name)) continue;
            nd_mut_record_data *rd = g_new0(nd_mut_record_data, 1);
            rd->type = g_strdup(type);
            rd->target = target;
            if (added) {
                rd->added = g_ptr_array_new();
                g_ptr_array_add(rd->added, added);
            }
            if (removed) {
                rd->removed = g_ptr_array_new();
                g_ptr_array_add(rd->removed, removed);
            }
            rd->previous_sibling = previous_sibling;
            rd->next_sibling = next_sibling;
            if (attr_name) rd->attribute_name = g_strdup(attr_name);
            if (wants_attr && t->attribute_old_value && old_value)
                rd->old_value = g_strdup(old_value);
            else if (wants_cdata && t->character_data_old_value && old_value)
                rd->old_value = g_strdup(old_value);
            g_ptr_array_add(o->records, rd);
            break;
        }
    }
    if (js->mutation_observers->len > 0) nd_mut_schedule_drain(js);
}

static void
nd_js_record_child_change(nd_js *js, nd_node *parent,
                          nd_node *added, nd_node *removed,
                          nd_node *previous_sibling, nd_node *next_sibling)
{
    nd_mut_record_emit(js, "childList", parent, added, removed,
                       previous_sibling, next_sibling, NULL, NULL);
}

static void
nd_js_record_attr_change(nd_js *js, nd_node *target,
                         const char *name, const char *old_value)
{
    nd_mut_record_emit(js, "attributes", target, NULL, NULL,
                       NULL, NULL, name, old_value);
}

static void
nd_js_record_character_data(nd_js *js, nd_node *target, const char *old_value)
{
    nd_mut_record_emit(js, "characterData", target, NULL, NULL,
                       NULL, NULL, NULL, old_value);
}

static void
nd_js_set_attr_recorded(nd_js *js, nd_node *n, const char *name, const char *value)
{
    if (!n || !name) return;
    const char *old = nd_element_get_attr(n, name);
    char *old_copy = old ? g_strdup(old) : NULL;
    nd_element_set_attr(n, name, value ? value : "");
    if (js) {
        js->mutated = TRUE;
        nd_js_record_attr_change(js, n, name, old_copy);
    }
    g_free(old_copy);
}

static JSValue
nd_mut_observer_observe(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    nd_mut_observer *o = nd_unwrap_mut_observer(this_val);
    if (!o || argc < 1) return JS_UNDEFINED;
    nd_node *target = nd_unwrap_element_mut(argv[0]);
    if (!target) return JS_UNDEFINED;
    nd_mut_target t = { .target = target, .child_list = TRUE };
    gboolean child_list_set = FALSE;
    gboolean attributes_set = FALSE;
    gboolean character_data_set = FALSE;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue sv = JS_GetPropertyStr(ctx, argv[1], "subtree");
        t.subtree = JS_ToBool(ctx, sv) > 0;
        JS_FreeValue(ctx, sv);
        JSValue cv = JS_GetPropertyStr(ctx, argv[1], "childList");
        if (!JS_IsUndefined(cv)) {
            t.child_list = JS_ToBool(ctx, cv) > 0;
            child_list_set = TRUE;
        }
        JS_FreeValue(ctx, cv);
        JSValue av = JS_GetPropertyStr(ctx, argv[1], "attributes");
        if (!JS_IsUndefined(av)) {
            t.attributes = JS_ToBool(ctx, av) > 0;
            attributes_set = TRUE;
        }
        JS_FreeValue(ctx, av);
        JSValue dv = JS_GetPropertyStr(ctx, argv[1], "characterData");
        if (!JS_IsUndefined(dv)) {
            t.character_data = JS_ToBool(ctx, dv) > 0;
            character_data_set = TRUE;
        }
        JS_FreeValue(ctx, dv);
        JSValue aov = JS_GetPropertyStr(ctx, argv[1], "attributeOldValue");
        if (JS_ToBool(ctx, aov) > 0) {
            t.attribute_old_value = TRUE;
            if (!attributes_set) { t.attributes = TRUE; attributes_set = TRUE; }
        }
        JS_FreeValue(ctx, aov);
        JSValue cov = JS_GetPropertyStr(ctx, argv[1], "characterDataOldValue");
        if (JS_ToBool(ctx, cov) > 0) {
            t.character_data_old_value = TRUE;
            if (!character_data_set) { t.character_data = TRUE; character_data_set = TRUE; }
        }
        JS_FreeValue(ctx, cov);
        JSValue afv = JS_GetPropertyStr(ctx, argv[1], "attributeFilter");
        if (JS_IsObject(afv) && !JS_IsNull(afv)) {
            JSValue lv = JS_GetPropertyStr(ctx, afv, "length");
            uint32_t len = 0;
            if (!JS_IsUndefined(lv)) JS_ToUint32(ctx, &len, lv);
            JS_FreeValue(ctx, lv);
            t.attribute_filter = g_ptr_array_new_with_free_func(g_free);
            for (uint32_t i = 0; i < len; i++) {
                JSValue iv = JS_GetPropertyUint32(ctx, afv, i);
                const char *s = JS_ToCString(ctx, iv);
                if (s) {
                    g_ptr_array_add(t.attribute_filter, g_strdup(s));
                    JS_FreeCString(ctx, s);
                }
                JS_FreeValue(ctx, iv);
            }
            if (!attributes_set) { t.attributes = TRUE; attributes_set = TRUE; }
        }
        JS_FreeValue(ctx, afv);
        if (!child_list_set) t.child_list = FALSE;
    }
    if (!t.child_list && !t.attributes && !t.character_data) {
        nd_mut_target_clear(&t);
        return JS_ThrowTypeError(ctx,
            "MutationObserver.observe: at least one of childList, attributes, characterData required");
    }
    if ((t.attribute_old_value || t.attribute_filter) && !t.attributes) {
        nd_mut_target_clear(&t);
        return JS_ThrowTypeError(ctx,
            "MutationObserver.observe: attributeOldValue/attributeFilter require attributes:true");
    }
    if (t.character_data_old_value && !t.character_data) {
        nd_mut_target_clear(&t);
        return JS_ThrowTypeError(ctx,
            "MutationObserver.observe: characterDataOldValue requires characterData:true");
    }
    for (guint i = 0; i < o->targets->len; i++) {
        nd_mut_target *existing = &g_array_index(o->targets, nd_mut_target, i);
        if (existing->target == target) {
            nd_mut_target_clear(existing);
            g_array_remove_index(o->targets, i);
            break;
        }
    }
    g_array_append_val(o->targets, t);
    o->disconnected = FALSE;
    return JS_UNDEFINED;
}

static JSValue
nd_mut_observer_disconnect(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    nd_mut_observer *o = nd_unwrap_mut_observer(this_val);
    if (!o) return JS_UNDEFINED;
    o->disconnected = TRUE;
    nd_mut_observer_targets_clear(o);
    if (o->records) g_ptr_array_set_size(o->records, 0);
    return JS_UNDEFINED;
}

static JSValue
nd_mut_observer_takeRecords(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    nd_mut_observer *o = nd_unwrap_mut_observer(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!o || !o->records) return arr;
    GPtrArray *recs = o->records;
    o->records = g_ptr_array_new_with_free_func(nd_mut_record_free);
    for (guint i = 0; i < recs->len; i++) {
        nd_mut_record_data *rd = g_ptr_array_index(recs, i);
        JS_SetPropertyUint32(ctx, arr, i, nd_mut_record_to_jsvalue(ctx, rd));
    }
    g_ptr_array_free(recs, TRUE);
    return arr;
}

static JSValue
nd_window_observer_ctor(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val;
    nd_js *js = js_from_ctx(ctx);
    if (!nd_mut_observer_class_id) JS_NewClassID(JS_GetRuntime(ctx), &nd_mut_observer_class_id);
    JS_NewClass(JS_GetRuntime(ctx), nd_mut_observer_class_id, &nd_mut_observer_class);
    JSValue obj = JS_NewObjectClass(ctx, nd_mut_observer_class_id);
    nd_mut_observer *o = g_new0(nd_mut_observer, 1);
    o->targets = g_array_new(FALSE, FALSE, sizeof(nd_mut_target));
    o->records = g_ptr_array_new_with_free_func(nd_mut_record_free);
    o->cb = (argc >= 1 && JS_IsFunction(ctx, argv[0]))
        ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    o->wrapper = obj;
    JS_SetOpaque(obj, o);
    nd_bind_fn(ctx, obj, "observe",      nd_mut_observer_observe,     2);
    nd_bind_fn(ctx, obj, "disconnect",   nd_mut_observer_disconnect,  0);
    nd_bind_fn(ctx, obj, "takeRecords",  nd_mut_observer_takeRecords, 0);
    if (js) {
        if (!js->mutation_observers)
            js->mutation_observers = g_ptr_array_new();
        g_ptr_array_add(js->mutation_observers, o);
    }
    return obj;
}

static JSValue
nd_visibility_observer_zero_rect(JSContext *ctx)
{
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "x",      JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, r, "y",      JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, r, "width",  JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, r, "height", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, r, "top",    JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, r, "right",  JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, r, "bottom", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, r, "left",   JS_NewFloat64(ctx, 0));
    return r;
}

static JSValue
nd_intersection_observer_observe(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    JSValue cb = JS_GetPropertyStr(ctx, this_val, "__cb");
    if (!JS_IsFunction(ctx, cb)) { JS_FreeValue(ctx, cb); return JS_UNDEFINED; }

    JSValue entry = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entry, "isIntersecting",    JS_TRUE);
    JS_SetPropertyStr(ctx, entry, "isVisible",         JS_TRUE);
    JS_SetPropertyStr(ctx, entry, "intersectionRatio", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, entry, "time",              JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, entry, "target",            JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, entry, "boundingClientRect",
                      nd_visibility_observer_zero_rect(ctx));
    JS_SetPropertyStr(ctx, entry, "intersectionRect",
                      nd_visibility_observer_zero_rect(ctx));
    JS_SetPropertyStr(ctx, entry, "rootBounds",
                      nd_visibility_observer_zero_rect(ctx));

    JSValue entries = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, entries, 0, entry);

    JSValueConst call_args[2] = { entries, this_val };
    JSValue ret = JS_Call(ctx, cb, this_val, 2, call_args);
    if (JS_IsException(ret)) {
        JSValue ex = JS_GetException(ctx);
        if (js_from_ctx(ctx) && js_from_ctx(ctx)->log_cb) {
            const char *msg = JS_ToCString(ctx, ex);
            if (msg) {
                char *line = g_strdup_printf("JS error in IntersectionObserver: %s", msg);
                js_from_ctx(ctx)->log_cb(line, js_from_ctx(ctx)->log_user_data);
                g_free(line);
                JS_FreeCString(ctx, msg);
            }
        }
        JS_FreeValue(ctx, ex);
    }
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, entries);
    JS_FreeValue(ctx, cb);
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

static JSValue
nd_intersection_observer_ctor(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    static const nd_fn_def methods[] = {
        { "unobserve", 1 }, { "disconnect", 0 }, { "takeRecords", 0 },
    };
    JSValue obj = JS_NewObject(ctx);
    if (argc >= 1 && JS_IsFunction(ctx, argv[0]))
        JS_SetPropertyStr(ctx, obj, "__cb", JS_DupValue(ctx, argv[0]));
    JSValue root_margin = JS_NewString(ctx, "0px");
    JS_SetPropertyStr(ctx, obj, "root", JS_NULL);
    JS_SetPropertyStr(ctx, obj, "rootMargin", root_margin);
    JS_SetPropertyStr(ctx, obj, "thresholds", JS_NewArray(ctx));
    nd_bind_fn(ctx, obj, "observe", nd_intersection_observer_observe, 1);
    nd_bind_fns(ctx, obj, nd_event_noop, methods, G_N_ELEMENTS(methods));
    return obj;
}

static JSValue
nd_resize_observer_observe(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    JSValue cb = JS_GetPropertyStr(ctx, this_val, "__cb");
    if (!JS_IsFunction(ctx, cb)) { JS_FreeValue(ctx, cb); return JS_UNDEFINED; }

    JSValue size = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, size, "inlineSize", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, size, "blockSize",  JS_NewFloat64(ctx, 0));
    JSValue size_arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, size_arr, 0, size);

    JSValue entry = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entry, "target", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, entry, "contentRect",
                      nd_visibility_observer_zero_rect(ctx));
    JS_SetPropertyStr(ctx, entry, "borderBoxSize",       JS_DupValue(ctx, size_arr));
    JS_SetPropertyStr(ctx, entry, "contentBoxSize",      JS_DupValue(ctx, size_arr));
    JS_SetPropertyStr(ctx, entry, "devicePixelContentBoxSize", size_arr);

    JSValue entries = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, entries, 0, entry);

    JSValueConst call_args[2] = { entries, this_val };
    JSValue ret = JS_Call(ctx, cb, this_val, 2, call_args);
    if (JS_IsException(ret)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, entries);
    JS_FreeValue(ctx, cb);
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

static JSValue
nd_resize_observer_ctor(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val;
    static const nd_fn_def methods[] = {
        { "unobserve", 1 }, { "disconnect", 0 },
    };
    JSValue obj = JS_NewObject(ctx);
    if (argc >= 1 && JS_IsFunction(ctx, argv[0]))
        JS_SetPropertyStr(ctx, obj, "__cb", JS_DupValue(ctx, argv[0]));
    nd_bind_fn(ctx, obj, "observe", nd_resize_observer_observe, 2);
    nd_bind_fns(ctx, obj, nd_event_noop, methods, G_N_ELEMENTS(methods));
    return obj;
}

static JSValue
nd_window_requestAnimationFrame(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!js_from_ctx(ctx) || argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_NewInt32(ctx, 0);
    nd_js *js = js_from_ctx(ctx);
    if (!js->raf_pending)
        js->raf_pending = g_array_new(FALSE, FALSE, sizeof(nd_raf_entry));
    if (js->raf_start_us == 0) js->raf_start_us = g_get_monotonic_time();
    nd_raf_entry e = { .id = ++js->next_raf_id, .cb = JS_DupValue(ctx, argv[0]) };
    g_array_append_val(js->raf_pending, e);
    return JS_NewInt32(ctx, e.id);
}

static JSValue
nd_window_cancelAnimationFrame(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!js_from_ctx(ctx) || argc < 1) return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    nd_js *js = js_from_ctx(ctx);
    if (!js->raf_pending) return JS_UNDEFINED;
    for (guint i = 0; i < js->raf_pending->len; i++) {
        nd_raf_entry *e = &g_array_index(js->raf_pending, nd_raf_entry, i);
        if (e->id == id) {
            JS_FreeValue(ctx, e->cb);
            g_array_remove_index(js->raf_pending, i);
            return JS_UNDEFINED;
        }
    }
    return JS_UNDEFINED;
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
nd_event_stop_immediate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, this_val, "_propagation_stopped", JS_TRUE);
    JS_SetPropertyStr(ctx, this_val, "_immediate_stopped",   JS_TRUE);
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
    JS_SetPropertyStr(ctx, event, "eventPhase", JS_NewInt32(ctx, 0));
    nd_bind_fn(ctx, event, "preventDefault",           nd_event_prevent_default, 0);
    nd_bind_fn(ctx, event, "stopPropagation",          nd_event_stop_propagation, 0);
    nd_bind_fn(ctx, event, "stopImmediatePropagation", nd_event_stop_immediate, 0);
    return event;
}

static JSValue
nd_event_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    const char *type = argc >= 1 ? JS_ToCString(ctx, argv[0]) : NULL;
    JSValue ev = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ev, "type",
        type ? JS_NewString(ctx, type) : JS_NewString(ctx, ""));
    if (type) JS_FreeCString(ctx, type);
    JS_SetPropertyStr(ctx, ev, "target", JS_NULL);
    JS_SetPropertyStr(ctx, ev, "currentTarget", JS_NULL);
    JS_SetPropertyStr(ctx, ev, "defaultPrevented", JS_FALSE);
    JS_SetPropertyStr(ctx, ev, "eventPhase", JS_NewInt32(ctx, 0));
    gboolean bubbles = FALSE, cancelable = FALSE, composed = FALSE;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue b = JS_GetPropertyStr(ctx, argv[1], "bubbles");
        bubbles = JS_ToBool(ctx, b) ? TRUE : FALSE;
        JS_FreeValue(ctx, b);
        JSValue c = JS_GetPropertyStr(ctx, argv[1], "cancelable");
        cancelable = JS_ToBool(ctx, c) ? TRUE : FALSE;
        JS_FreeValue(ctx, c);
        JSValue cp = JS_GetPropertyStr(ctx, argv[1], "composed");
        composed = JS_ToBool(ctx, cp) ? TRUE : FALSE;
        JS_FreeValue(ctx, cp);
    }
    JS_SetPropertyStr(ctx, ev, "bubbles",    bubbles ? JS_TRUE : JS_FALSE);
    JS_SetPropertyStr(ctx, ev, "cancelable", cancelable ? JS_TRUE : JS_FALSE);
    JS_SetPropertyStr(ctx, ev, "composed",   composed ? JS_TRUE : JS_FALSE);
    nd_bind_fn(ctx, ev, "preventDefault",           nd_event_prevent_default, 0);
    nd_bind_fn(ctx, ev, "stopPropagation",          nd_event_stop_propagation, 0);
    nd_bind_fn(ctx, ev, "stopImmediatePropagation", nd_event_stop_immediate, 0);
    return ev;
}

static JSValue
nd_custom_event_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue ev = nd_event_ctor(ctx, this_val, argc, argv);
    if (JS_IsException(ev)) return ev;
    JSValue detail = JS_NULL;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        detail = JS_GetPropertyStr(ctx, argv[1], "detail");
        if (JS_IsUndefined(detail)) { JS_FreeValue(ctx, detail); detail = JS_NULL; }
    }
    JS_SetPropertyStr(ctx, ev, "detail", detail);
    return ev;
}

static JSValue
nd_mouse_event_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue ev = nd_event_ctor(ctx, this_val, argc, argv);
    if (JS_IsException(ev)) return ev;
    int cx = 0, cy = 0, btn = 0;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue v;
        v = JS_GetPropertyStr(ctx, argv[1], "clientX"); JS_ToInt32(ctx, &cx, v); JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "clientY"); JS_ToInt32(ctx, &cy, v); JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "button");  JS_ToInt32(ctx, &btn, v); JS_FreeValue(ctx, v);
    }
    JS_SetPropertyStr(ctx, ev, "clientX", JS_NewInt32(ctx, cx));
    JS_SetPropertyStr(ctx, ev, "clientY", JS_NewInt32(ctx, cy));
    JS_SetPropertyStr(ctx, ev, "pageX",   JS_NewInt32(ctx, cx));
    JS_SetPropertyStr(ctx, ev, "pageY",   JS_NewInt32(ctx, cy));
    JS_SetPropertyStr(ctx, ev, "button",  JS_NewInt32(ctx, btn));
    JS_SetPropertyStr(ctx, ev, "buttons", JS_NewInt32(ctx, btn ? (1 << btn) : 0));
    return ev;
}

static gboolean
nd_fire_inline_on_handler(nd_js *js, const nd_node *target, const char *type,
                          JSValue event)
{
    if (!js || !target || target->kind != ND_NODE_ELEMENT || !type) return FALSE;

    char attr_name[48];
    g_snprintf(attr_name, sizeof attr_name, "on%s", type);
    const char *body = nd_element_get_attr(target, attr_name);
    if (!body || !*body) return FALSE;

    if (!nd_csp_inline_event_handler_allowed(js->csp)) {
        if (js->log_cb) {
            char *line = g_strdup_printf(
                "CSP blocked: inline event handler %s", attr_name);
            js->log_cb(line, js->log_user_data);
            g_free(line);
        }
        return FALSE;
    }

    GString *src = g_string_new("(function(event){\n");
    g_string_append(src, body);
    g_string_append(src, "\n})");
    JSValue fn = JS_Eval(js->ctx, src->str, src->len, "<inline>",
                         JS_EVAL_TYPE_GLOBAL);
    g_string_free(src, TRUE);

    if (JS_IsException(fn)) {
        JSValue ex = JS_GetException(js->ctx);
        const char *m = JS_ToCString(js->ctx, ex);
        if (m && js->log_cb) {
            char *line = g_strdup_printf("JS error compiling %s: %s", attr_name, m);
            js->log_cb(line, js->log_user_data);
            g_free(line);
        }
        if (m) JS_FreeCString(js->ctx, m);
        JS_FreeValue(js->ctx, ex);
        return FALSE;
    }

    JSValue this_val = nd_make_element(js->ctx, target);
    JSValue ret = JS_Call(js->ctx, fn, this_val, 1, &event);
    if (JS_IsException(ret)) {
        JSValue ex = JS_GetException(js->ctx);
        const char *m = JS_ToCString(js->ctx, ex);
        if (m && js->log_cb) {
            char *line = g_strdup_printf("JS error in %s: %s", attr_name, m);
            js->log_cb(line, js->log_user_data);
            g_free(line);
        }
        if (m) JS_FreeCString(js->ctx, m);
        JS_FreeValue(js->ctx, ex);
    } else if (JS_IsBool(ret) && !JS_ToBool(js->ctx, ret)) {
        JS_SetPropertyStr(js->ctx, event, "defaultPrevented", JS_TRUE);
    }
    JS_FreeValue(js->ctx, ret);
    JS_FreeValue(js->ctx, this_val);
    JS_FreeValue(js->ctx, fn);
    return TRUE;
}

static gboolean
nd_invoke_listeners_at(nd_js *js, const nd_node *cur, const char *type,
                       JSValue event, gboolean capture_phase,
                       gboolean *fired)
{
    if (!capture_phase) {
        if (nd_fire_inline_on_handler(js, cur, type, event))
            *fired = TRUE;
    }

    js->dispatch_depth++;

    GPtrArray *to_call = g_ptr_array_new();
    for (guint i = 0; i < js->listeners->len; i++) {
        nd_listener *l = g_ptr_array_index(js->listeners, i);
        if (nd_listener_is_tombstoned(l)) continue;
        if (l->target != cur || strcmp(l->type, type) != 0) continue;
        if (!!l->capture != !!capture_phase) continue;
        g_ptr_array_add(to_call, l);
    }
    gboolean stopped = FALSE;
    for (guint i = 0; i < to_call->len; i++) {
        nd_listener *l = to_call->pdata[i];
        if (nd_listener_is_tombstoned(l)) continue;
        JS_SetPropertyStr(js->ctx, event, "currentTarget",
                          nd_make_element(js->ctx, cur));
        JS_SetPropertyStr(js->ctx, event, "eventPhase",
                          JS_NewInt32(js->ctx, capture_phase ? 1 :
                                      (cur == JS_VALUE_GET_PTR(event) ? 2 : 3)));
        JSValue ret = JS_Call(js->ctx, l->cb, JS_UNDEFINED, 1, &event);
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
        *fired = TRUE;
        if (!nd_listener_is_tombstoned(l) && l->once)
            nd_listener_tombstone(js->ctx, l);
        JSValue imm = JS_GetPropertyStr(js->ctx, event, "_immediate_stopped");
        gboolean immediate = JS_ToBool(js->ctx, imm);
        JS_FreeValue(js->ctx, imm);
        if (immediate) { stopped = TRUE; break; }
    }
    g_ptr_array_free(to_call, TRUE);
    if (!stopped) {
        JSValue sp = JS_GetPropertyStr(js->ctx, event, "_propagation_stopped");
        stopped = JS_ToBool(js->ctx, sp) ? TRUE : FALSE;
        JS_FreeValue(js->ctx, sp);
    }

    js->dispatch_depth--;
    nd_listeners_sweep(js);
    return stopped;
}

static gboolean
nd_js_dispatch_built_event(nd_js *js, const nd_node *target, const char *type,
                           JSValue event, gboolean *default_prevented)
{
    gboolean fired = FALSE;
    nd_budget_guard bg;
    nd_js_budget_push(js, &bg);

    GPtrArray *path = g_ptr_array_new();
    for (const nd_node *cur = target; cur; cur = cur->parent)
        g_ptr_array_add(path, (gpointer)cur);

    gboolean stopped = FALSE;
    for (gint i = (gint)path->len - 1; i > 0 && !stopped; i--) {
        const nd_node *cur = path->pdata[i];
        stopped = nd_invoke_listeners_at(js, cur, type, event, TRUE, &fired);
    }
    if (!stopped && path->len > 0) {
        const nd_node *cur = path->pdata[0];
        stopped = nd_invoke_listeners_at(js, cur, type, event, FALSE, &fired);
        if (!stopped)
            stopped = nd_invoke_listeners_at(js, cur, type, event, TRUE, &fired);
    }
    JSValue bub = JS_GetPropertyStr(js->ctx, event, "bubbles");
    gboolean bubbles = JS_ToBool(js->ctx, bub) ? TRUE : FALSE;
    JS_FreeValue(js->ctx, bub);
    if (!stopped && bubbles) {
        for (guint i = 1; i < path->len && !stopped; i++) {
            const nd_node *cur = path->pdata[i];
            stopped = nd_invoke_listeners_at(js, cur, type, event, FALSE, &fired);
        }
    }
    g_ptr_array_free(path, TRUE);

    if (default_prevented) {
        JSValue dp = JS_GetPropertyStr(js->ctx, event, "defaultPrevented");
        *default_prevented = JS_ToBool(js->ctx, dp) ? TRUE : FALSE;
        JS_FreeValue(js->ctx, dp);
    }
    JS_FreeValue(js->ctx, event);
    nd_drain_mutations(js);
    nd_js_budget_pop(js, &bg);
    return fired;
}

void
nd_js_set_style_table(nd_js *js, GHashTable *styles)
{
    if (!js) return;
    js->style_table = styles;
}

gboolean
nd_js_run_animation_frame(nd_js *js)
{
    if (!js || !js->raf_pending || js->raf_pending->len == 0) return FALSE;
    gint64 now_us = g_get_monotonic_time();
    if (js->raf_last_us != 0 && now_us - js->raf_last_us < 100000)
        return FALSE;
    js->raf_last_us = now_us;
    GArray *fired = js->raf_pending;
    js->raf_pending = g_array_new(FALSE, FALSE, sizeof(nd_raf_entry));
    if (js->raf_start_us == 0) js->raf_start_us = now_us;
    double ts_ms = (now_us - js->raf_start_us) / 1000.0;
    nd_budget_guard bg;
    nd_js_budget_push(js, &bg);
    for (guint i = 0; i < fired->len; i++) {
        nd_raf_entry *e = &g_array_index(fired, nd_raf_entry, i);
        JSValue arg = JS_NewFloat64(js->ctx, ts_ms);
        JSValueConst argv[1] = { arg };
        JSValue ret = JS_Call(js->ctx, e->cb, JS_UNDEFINED, 1, argv);
        if (JS_IsException(ret)) {
            JSValue ex = JS_GetException(js->ctx);
            const char *msg = JS_ToCString(js->ctx, ex);
            if (msg && js->log_cb) {
                char *line = g_strdup_printf(
                    "JS error in requestAnimationFrame: %s", msg);
                js->log_cb(line, js->log_user_data);
                g_free(line);
            }
            if (msg) JS_FreeCString(js->ctx, msg);
            JS_FreeValue(js->ctx, ex);
        }
        JS_FreeValue(js->ctx, ret);
        JS_FreeValue(js->ctx, arg);
        JS_FreeValue(js->ctx, e->cb);
    }
    g_array_free(fired, TRUE);
    nd_drain_mutations(js);
    nd_js_budget_pop(js, &bg);
    return TRUE;
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
    nd_js *_j = js_from_ctx(ctx);
    if (child->kind == ND_NODE_DOCUMENT && !child->parent) {
        nd_node *c = child->first_child;
        while (c) {
            nd_node *next = c->next_sibling;
            nd_node_remove(c);
            if (_j) g_ptr_array_remove_fast(_j->orphan_nodes, c);
            nd_node_append_child(parent, c);
            if (_j) nd_js_record_child_change(_j, parent, c, NULL,
                                              c->prev_sibling, c->next_sibling);
            c = next;
        }
        if (_j) _j->mutated = TRUE;
        return JS_DupValue(ctx, argv[0]);
    }
    if (_j) g_ptr_array_remove_fast(_j->orphan_nodes, child);
    nd_node_append_child(parent, child);
    if (_j) {
        _j->mutated = TRUE;
        nd_js_record_child_change(_j, parent, child, NULL,
                                  child->prev_sibling, child->next_sibling);
    }
    return JS_DupValue(ctx, argv[0]);
}

static JSValue
nd_element_removeChild(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    nd_node *parent = nd_unwrap_element_mut(this_val);
    if (!parent || argc < 1) return JS_NULL;
    nd_node *child = nd_unwrap_element_mut(argv[0]);
    if (!child || child->parent != parent) return JS_NULL;
    nd_node *saved_prev = child->prev_sibling;
    nd_node *saved_next = child->next_sibling;
    nd_node_remove(child);
    if (js_from_ctx(ctx)) {
        g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, child);
        nd_js *_j2 = js_from_ctx(ctx);
        if (_j2) {
            _j2->mutated = TRUE;
            nd_js_record_child_change(_j2, parent, NULL, child,
                                      saved_prev, saved_next);
        }
    }
    return JS_DupValue(ctx, argv[0]);
}

static void
nd_element_insert_before_single(nd_js *_j, nd_node *parent, nd_node *newc, nd_node *ref)
{
    if (newc->parent) nd_node_remove(newc);
    if (_j) g_ptr_array_remove_fast(_j->orphan_nodes, newc);
    newc->parent = parent;
    newc->next_sibling = ref;
    newc->prev_sibling = ref->prev_sibling;
    if (ref->prev_sibling) ref->prev_sibling->next_sibling = newc;
    else parent->first_child = newc;
    ref->prev_sibling = newc;
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
    nd_js *_j = js_from_ctx(ctx);
    if (newc->kind == ND_NODE_DOCUMENT && !newc->parent) {
        nd_node *c = newc->first_child;
        while (c) {
            nd_node *next = c->next_sibling;
            nd_node_remove(c);
            if (!ref || ref->parent != parent) {
                if (_j) g_ptr_array_remove_fast(_j->orphan_nodes, c);
                nd_node_append_child(parent, c);
            } else {
                nd_element_insert_before_single(_j, parent, c, ref);
            }
            if (_j) nd_js_record_child_change(_j, parent, c, NULL,
                                              c->prev_sibling, c->next_sibling);
            c = next;
        }
        if (_j) _j->mutated = TRUE;
        return JS_DupValue(ctx, argv[0]);
    }
    if (!ref || ref->parent != parent) {
        if (_j) g_ptr_array_remove_fast(_j->orphan_nodes, newc);
        nd_node_append_child(parent, newc);
    } else {
        nd_element_insert_before_single(_j, parent, newc, ref);
    }
    if (_j) {
        _j->mutated = TRUE;
        nd_js_record_child_change(_j, parent, newc, NULL,
                                  newc->prev_sibling, newc->next_sibling);
    }
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
    if (js_from_ctx(ctx))
        g_ptr_array_remove_fast(js_from_ctx(ctx)->orphan_nodes, newc);
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
    if (js_from_ctx(ctx)) {
        g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, oldc);
        nd_js *_j2 = js_from_ctx(ctx);
        if (_j2) {
            _j2->mutated = TRUE;
            nd_js_record_child_change(_j2, parent, newc, oldc,
                                      newc->prev_sibling, newc->next_sibling);
        }
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
    gboolean adjacent_to_self = (g_ascii_strcasecmp(pos, "beforebegin") == 0 ||
                                 g_ascii_strcasecmp(pos, "afterend") == 0);
    const char *ctx_tag = adjacent_to_self
        ? ((self->parent && self->parent->kind == ND_NODE_ELEMENT)
           ? self->parent->name : NULL)
        : self->name;
    nd_node *fragment = nd_html_parse_fragment_in(ctx_tag, html, -1);
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
        { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
            if (js_from_ctx(ctx))
                g_ptr_array_remove_fast(js_from_ctx(ctx)->orphan_nodes, child);
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
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
            if (js_from_ctx(ctx))
                g_ptr_array_remove_fast(js_from_ctx(ctx)->orphan_nodes, child);
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
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
    if (js_from_ctx(ctx)) {
        g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, self);
        { nd_js *_j2 = js_from_ctx(ctx); if (_j2) _j2->mutated = TRUE; }
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
            if (la > G_MAXSIZE - lb - 1) { c = next; continue; }
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
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
    if (js_from_ctx(ctx)) g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, copy);
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
    if (js_from_ctx(ctx)) {
        g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, n);
        { nd_js *_j2 = js_from_ctx(ctx); if (_j2) _j2->mutated = TRUE; }
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
            if (js_from_ctx(ctx))
                g_ptr_array_remove_fast(js_from_ctx(ctx)->orphan_nodes, child);
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
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
            if (js_from_ctx(ctx))
                g_ptr_array_remove_fast(js_from_ctx(ctx)->orphan_nodes, child);
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
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
        const char *old = nd_element_get_attr(n, name);
        char *old_copy = old ? g_strdup(old) : NULL;
        nd_element_set_attr(n, name, val);
        nd_js *_j = js_from_ctx(ctx);
        if (_j) {
            _j->mutated = TRUE;
            nd_js_record_attr_change(_j, n, name, old_copy);
        }
        g_free(old_copy);
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
            char *old_copy = g_strdup(a->value);
            *prev = a->next;
            g_free(a->name);
            g_free(a->value);
            g_free(a);
            nd_js *_j = js_from_ctx(ctx);
            if (_j) {
                _j->mutated = TRUE;
                nd_js_record_attr_change(_j, n, name, old_copy);
            }
            g_free(old_copy);
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

static const nd_box *
nd_box_find_by_dom(const nd_box *root, const nd_node *target)
{
    if (!root || !target) return NULL;
    if (root->dom == target) return root;
    for (const nd_box *c = root->first_child; c; c = c->next_sibling) {
        const nd_box *m = nd_box_find_by_dom(c, target);
        if (m) return m;
    }
    return NULL;
}

static void
nd_box_border_box(const nd_box *b, double *x, double *y, double *w, double *h)
{
    *x = b->x - b->border.left;
    *y = b->y - b->border.top;
    *w = b->content_width  + b->padding.left + b->padding.right
                           + b->border.left  + b->border.right;
    *h = b->content_height + b->padding.top  + b->padding.bottom
                           + b->border.top   + b->border.bottom;
}

static const nd_box *
nd_box_for_this(JSContext *ctx, JSValueConst this_val)
{
    nd_js *js = js_from_ctx(ctx);
    if (!js || !js->layout_root) return NULL;
    const nd_node *n = nd_unwrap_element(this_val);
    if (!n) return NULL;
    return nd_box_find_by_dom(js->layout_root, n);
}

static JSValue
nd_element_getBoundingClientRect(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    double x = 0, y = 0, w = 0, h = 0;
    const nd_box *b = nd_box_for_this(ctx, this_val);
    if (b) nd_box_border_box(b, &x, &y, &w, &h);
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "x",      JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, r, "y",      JS_NewFloat64(ctx, y));
    JS_SetPropertyStr(ctx, r, "top",    JS_NewFloat64(ctx, y));
    JS_SetPropertyStr(ctx, r, "left",   JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, r, "right",  JS_NewFloat64(ctx, x + w));
    JS_SetPropertyStr(ctx, r, "bottom", JS_NewFloat64(ctx, y + h));
    JS_SetPropertyStr(ctx, r, "width",  JS_NewFloat64(ctx, w));
    JS_SetPropertyStr(ctx, r, "height", JS_NewFloat64(ctx, h));
    return r;
}

static JSValue
nd_element_get_offsetWidth(JSContext *ctx, JSValueConst this_val)
{
    double x, y, w, h;
    const nd_box *b = nd_box_for_this(ctx, this_val);
    if (!b) return JS_NewInt32(ctx, 0);
    nd_box_border_box(b, &x, &y, &w, &h);
    return JS_NewInt32(ctx, (int)(w + 0.5));
}

static JSValue
nd_element_get_offsetHeight(JSContext *ctx, JSValueConst this_val)
{
    double x, y, w, h;
    const nd_box *b = nd_box_for_this(ctx, this_val);
    if (!b) return JS_NewInt32(ctx, 0);
    nd_box_border_box(b, &x, &y, &w, &h);
    return JS_NewInt32(ctx, (int)(h + 0.5));
}

static JSValue
nd_element_get_offsetTop(JSContext *ctx, JSValueConst this_val)
{
    const nd_box *b = nd_box_for_this(ctx, this_val);
    return JS_NewInt32(ctx, b ? (int)(b->y - b->border.top + 0.5) : 0);
}

static JSValue
nd_element_get_offsetLeft(JSContext *ctx, JSValueConst this_val)
{
    const nd_box *b = nd_box_for_this(ctx, this_val);
    return JS_NewInt32(ctx, b ? (int)(b->x - b->border.left + 0.5) : 0);
}

void
nd_js_set_layout_root(nd_js *js, const struct nd_box *root)
{
    if (!js) return;
    js->layout_root = root;
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
    if (!js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc) return arr;
    uint32_t idx = 0;
    GQueue q = G_QUEUE_INIT;
    g_queue_push_tail(&q, js_from_ctx(ctx)->current_doc);
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
    if (!n || !js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc) return JS_FALSE;
    for (const nd_node *p = n; p; p = p->parent)
        if (p == js_from_ctx(ctx)->current_doc) return JS_TRUE;
    return JS_FALSE;
}

static JSValue
nd_element_get_ownerDocument(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc) return JS_NULL;
    return nd_make_element(ctx, js_from_ctx(ctx)->current_doc);
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
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
        { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
        return JS_UNDEFINED;
    }
    nd_element_set_attr(el, "value", s);
    JS_FreeCString(ctx, s);
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
    if (!el || !js_from_ctx(ctx) || !js_from_ctx(ctx)->scroll_to_cb) return JS_UNDEFINED;
    js_from_ctx(ctx)->scroll_to_cb(el, js_from_ctx(ctx)->scroll_to_user_data);
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
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
    if (js_from_ctx(ctx)) {
        nd_js_dispatch_event(js_from_ctx(ctx), el, "close", NULL);
        { nd_js *_j2 = js_from_ctx(ctx); if (_j2) _j2->mutated = TRUE; }
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
    if (!el || !js_from_ctx(ctx)) return JS_UNDEFINED;
    gboolean prevented = FALSE;
    nd_js_dispatch_event(js_from_ctx(ctx), el, "click", &prevented);
    if (prevented) return JS_UNDEFINED;
    if (el->kind == ND_NODE_ELEMENT && el->name &&
        g_ascii_strcasecmp(el->name, "a") == 0) {
        const char *href = nd_element_get_attr(el, "href");
        if (href && *href && js_from_ctx(ctx)->nav_cb)
            js_from_ctx(ctx)->nav_cb(href, FALSE, js_from_ctx(ctx)->nav_user_data);
        return JS_UNDEFINED;
    }
    if (nd_node_is_submit_trigger(el) && js_from_ctx(ctx)->form_submit_cb) {
        const nd_node *form = nd_node_enclosing_form(el);
        if (form)
            js_from_ctx(ctx)->form_submit_cb(form, el, js_from_ctx(ctx)->form_submit_user_data);
    } else if (nd_node_is_reset_trigger(el)) {
        nd_node *form = (nd_node *)nd_node_enclosing_form(el);
        if (form) {
            nd_form_reset_walk(form);
            { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
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
    if (!el || !js_from_ctx(ctx) || !js_from_ctx(ctx)->form_submit_cb) return JS_UNDEFINED;
    if (el->kind != ND_NODE_ELEMENT || !el->name ||
        g_ascii_strcasecmp(el->name, "form") != 0) return JS_UNDEFINED;
    js_from_ctx(ctx)->form_submit_cb(el, NULL, js_from_ctx(ctx)->form_submit_user_data);
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
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

static int
nd_canvas_dim_from_attr(const nd_node *el, const char *name, int defv)
{
    const char *v = nd_element_get_attr(el, name);
    if (!v || !*v) return defv;
    int n = nd_parse_int(v, defv, 0, 8192);
    if (n < 1) return defv;
    return n;
}

static gboolean
nd_canvas_parse_color(const char *s, double *r, double *g, double *b, double *a)
{
    if (!s) return FALSE;
    *r = *g = *b = 0; *a = 1;
    if (*s == '#') {
        gsize len = strlen(s);
        unsigned int rv = 0, gv = 0, bv = 0;
        if (len == 7 && sscanf(s + 1, "%2x%2x%2x", &rv, &gv, &bv) == 3) {
            *r = rv / 255.0; *g = gv / 255.0; *b = bv / 255.0;
            return TRUE;
        }
        if (len == 4 && sscanf(s + 1, "%1x%1x%1x", &rv, &gv, &bv) == 3) {
            *r = (rv * 17) / 255.0; *g = (gv * 17) / 255.0; *b = (bv * 17) / 255.0;
            return TRUE;
        }
    }
    if (g_str_has_prefix(s, "rgb")) {
        gboolean has_a = s[3] == 'a' || s[3] == 'A';
        const char *paren = strchr(s, '(');
        if (!paren) return FALSE;
        double rv = 0, gv = 0, bv = 0, av = 1;
        if (has_a) {
            if (sscanf(paren + 1, "%lf , %lf , %lf , %lf", &rv, &gv, &bv, &av) < 3)
                return FALSE;
        } else {
            if (sscanf(paren + 1, "%lf , %lf , %lf", &rv, &gv, &bv) < 3)
                return FALSE;
        }
        *r = rv / 255.0; *g = gv / 255.0; *b = bv / 255.0; *a = av;
        return TRUE;
    }
    if (strcmp(s, "black") == 0)        { *r=0; *g=0; *b=0; return TRUE; }
    if (strcmp(s, "white") == 0)        { *r=1; *g=1; *b=1; return TRUE; }
    if (strcmp(s, "red") == 0)          { *r=1; *g=0; *b=0; return TRUE; }
    if (strcmp(s, "green") == 0)        { *r=0; *g=128/255.0; *b=0; return TRUE; }
    if (strcmp(s, "blue") == 0)         { *r=0; *g=0; *b=1; return TRUE; }
    if (strcmp(s, "transparent") == 0)  { *r=0; *g=0; *b=0; *a=0; return TRUE; }
    return FALSE;
}

static nd_canvas_state *
nd_canvas_state_for(nd_js *js, const nd_node *el)
{
    if (!js || !el) return NULL;
    if (!js->canvas_states)
        js->canvas_states = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                  NULL, nd_canvas_state_free);
    nd_canvas_state *st = g_hash_table_lookup(js->canvas_states, el);
    int w = nd_canvas_dim_from_attr(el, "width",  300);
    int h = nd_canvas_dim_from_attr(el, "height", 150);
    if (st && (st->w != w || st->h != h)) {
        g_hash_table_remove(js->canvas_states, el);
        st = NULL;
    }
    if (!st) {
        st = g_new0(nd_canvas_state, 1);
        st->w = w;
        st->h = h;
        st->surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        st->cr   = cairo_create(st->surf);
        st->fill_r = st->fill_g = st->fill_b = 0; st->fill_a = 1;
        st->stroke_r = st->stroke_g = st->stroke_b = 0; st->stroke_a = 1;
        st->line_width = 1;
        st->font = g_strdup("10px sans-serif");
        g_hash_table_insert(js->canvas_states, (gpointer)el, st);
    }
    return st;
}

cairo_surface_t *
nd_js_canvas_surface(nd_js *js, const nd_node *n)
{
    if (!js || !js->canvas_states || !n) return NULL;
    nd_canvas_state *st = g_hash_table_lookup(js->canvas_states, n);
    return st ? st->surf : NULL;
}

static nd_canvas_state *
nd_ctx_state(JSContext *ctx, JSValueConst this_val)
{
    if (!js_from_ctx(ctx)) return NULL;
    JSValue node_v = JS_GetPropertyStr(ctx, this_val, "_node");
    const nd_node *n = nd_unwrap_element(node_v);
    JS_FreeValue(ctx, node_v);
    return nd_canvas_state_for(js_from_ctx(ctx), n);
}

static void
nd_ctx_sync_styles(JSContext *ctx, JSValueConst this_val, nd_canvas_state *st)
{
    JSValue v;
    v = JS_GetPropertyStr(ctx, this_val, "fillStyle");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) {
            double r, g, b, a;
            if (nd_canvas_parse_color(s, &r, &g, &b, &a)) {
                st->fill_r = r; st->fill_g = g; st->fill_b = b; st->fill_a = a;
            }
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, this_val, "strokeStyle");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) {
            double r, g, b, a;
            if (nd_canvas_parse_color(s, &r, &g, &b, &a)) {
                st->stroke_r = r; st->stroke_g = g; st->stroke_b = b; st->stroke_a = a;
            }
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, this_val, "lineWidth");
    double lw;
    if (JS_ToFloat64(ctx, &lw, v) == 0 && lw > 0) st->line_width = lw;
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, this_val, "font");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) { g_free(st->font); st->font = g_strdup(s); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
}

static double
nd_arg_d(JSContext *ctx, JSValueConst v)
{
    double d = 0; JS_ToFloat64(ctx, &d, v); return d;
}

static JSValue
nd_ctx_fillRect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 4) return JS_UNDEFINED;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    nd_ctx_sync_styles(ctx, this_val, st);
    cairo_set_source_rgba(st->cr, st->fill_r, st->fill_g, st->fill_b, st->fill_a);
    cairo_rectangle(st->cr,
        nd_arg_d(ctx, argv[0]), nd_arg_d(ctx, argv[1]),
        nd_arg_d(ctx, argv[2]), nd_arg_d(ctx, argv[3]));
    cairo_fill(st->cr);
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_strokeRect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 4) return JS_UNDEFINED;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    nd_ctx_sync_styles(ctx, this_val, st);
    cairo_set_source_rgba(st->cr, st->stroke_r, st->stroke_g, st->stroke_b, st->stroke_a);
    cairo_set_line_width(st->cr, st->line_width);
    cairo_rectangle(st->cr,
        nd_arg_d(ctx, argv[0]), nd_arg_d(ctx, argv[1]),
        nd_arg_d(ctx, argv[2]), nd_arg_d(ctx, argv[3]));
    cairo_stroke(st->cr);
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_clearRect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 4) return JS_UNDEFINED;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    cairo_save(st->cr);
    cairo_set_operator(st->cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(st->cr,
        nd_arg_d(ctx, argv[0]), nd_arg_d(ctx, argv[1]),
        nd_arg_d(ctx, argv[2]), nd_arg_d(ctx, argv[3]));
    cairo_fill(st->cr);
    cairo_restore(st->cr);
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_beginPath(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (st) cairo_new_path(st->cr);
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_closePath(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (st) cairo_close_path(st->cr);
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_moveTo(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (st) cairo_move_to(st->cr, nd_arg_d(ctx, argv[0]), nd_arg_d(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_lineTo(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (st) cairo_line_to(st->cr, nd_arg_d(ctx, argv[0]), nd_arg_d(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_arc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 5) return JS_UNDEFINED;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    double x = nd_arg_d(ctx, argv[0]);
    double y = nd_arg_d(ctx, argv[1]);
    double r = nd_arg_d(ctx, argv[2]);
    double a0 = nd_arg_d(ctx, argv[3]);
    double a1 = nd_arg_d(ctx, argv[4]);
    gboolean ccw = argc >= 6 && JS_ToBool(ctx, argv[5]);
    if (ccw) cairo_arc_negative(st->cr, x, y, r, a0, a1);
    else     cairo_arc(st->cr, x, y, r, a0, a1);
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 4) return JS_UNDEFINED;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (st)
        cairo_rectangle(st->cr,
            nd_arg_d(ctx, argv[0]), nd_arg_d(ctx, argv[1]),
            nd_arg_d(ctx, argv[2]), nd_arg_d(ctx, argv[3]));
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_fill(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    nd_ctx_sync_styles(ctx, this_val, st);
    cairo_set_source_rgba(st->cr, st->fill_r, st->fill_g, st->fill_b, st->fill_a);
    cairo_fill_preserve(st->cr);
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_stroke(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    nd_ctx_sync_styles(ctx, this_val, st);
    cairo_set_source_rgba(st->cr, st->stroke_r, st->stroke_g, st->stroke_b, st->stroke_a);
    cairo_set_line_width(st->cr, st->line_width);
    cairo_stroke_preserve(st->cr);
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_save(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (st) cairo_save(st->cr);
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_restore(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (st) cairo_restore(st->cr);
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_translate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (st) cairo_translate(st->cr, nd_arg_d(ctx, argv[0]), nd_arg_d(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_scale(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (st) cairo_scale(st->cr, nd_arg_d(ctx, argv[0]), nd_arg_d(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_rotate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (st) cairo_rotate(st->cr, nd_arg_d(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_fillText(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 3) return JS_UNDEFINED;
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    if (!st) return JS_UNDEFINED;
    nd_ctx_sync_styles(ctx, this_val, st);
    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_UNDEFINED;
    double x = nd_arg_d(ctx, argv[1]);
    double y = nd_arg_d(ctx, argv[2]);
    PangoLayout *layout = pango_cairo_create_layout(st->cr);
    PangoFontDescription *desc = pango_font_description_from_string(
        st->font ? st->font : "10px sans-serif");
    if (pango_font_description_get_size(desc) == 0)
        pango_font_description_set_absolute_size(desc, 10 * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(layout, text, -1);
    int pw, ph;
    pango_layout_get_pixel_size(layout, &pw, &ph);
    cairo_save(st->cr);
    cairo_set_source_rgba(st->cr, st->fill_r, st->fill_g, st->fill_b, st->fill_a);
    cairo_move_to(st->cr, x, y - ph);
    pango_cairo_show_layout(st->cr, layout);
    cairo_restore(st->cr);
    g_object_unref(layout);
    JS_FreeCString(ctx, text);
    { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    return JS_UNDEFINED;
}

static JSValue
nd_ctx_measureText(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue obj = JS_NewObject(ctx);
    if (argc < 1) { JS_SetPropertyStr(ctx, obj, "width", JS_NewFloat64(ctx, 0)); return obj; }
    nd_canvas_state *st = nd_ctx_state(ctx, this_val);
    const char *text = JS_ToCString(ctx, argv[0]);
    double w = 0;
    if (text && st) {
        PangoLayout *layout = pango_cairo_create_layout(st->cr);
        PangoFontDescription *desc = pango_font_description_from_string(
            st->font ? st->font : "10px sans-serif");
        if (pango_font_description_get_size(desc) == 0)
            pango_font_description_set_absolute_size(desc, 10 * PANGO_SCALE);
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);
        pango_layout_set_text(layout, text, -1);
        int pw, ph;
        pango_layout_get_pixel_size(layout, &pw, &ph);
        w = pw;
        g_object_unref(layout);
    }
    if (text) JS_FreeCString(ctx, text);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewFloat64(ctx, w));
    return obj;
}

static JSValue
nd_element_getContext(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el || !js_from_ctx(ctx)) return JS_NULL;
    nd_canvas_state *st = nd_canvas_state_for(js_from_ctx(ctx), el);
    if (!st) return JS_NULL;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "_node", JS_DupValue(ctx, this_val));
    JS_SetPropertyStr(ctx, obj, "canvas", JS_DupValue(ctx, this_val));
    JS_SetPropertyStr(ctx, obj, "fillStyle",   JS_NewString(ctx, "#000"));
    JS_SetPropertyStr(ctx, obj, "strokeStyle", JS_NewString(ctx, "#000"));
    JS_SetPropertyStr(ctx, obj, "lineWidth",   JS_NewFloat64(ctx, 1));
    JS_SetPropertyStr(ctx, obj, "font",        JS_NewString(ctx, st->font ? st->font : "10px sans-serif"));
    JS_SetPropertyStr(ctx, obj, "textBaseline", JS_NewString(ctx, "alphabetic"));
    JS_SetPropertyStr(ctx, obj, "globalAlpha",  JS_NewFloat64(ctx, 1));
    nd_bind_fn(ctx, obj, "fillRect",    nd_ctx_fillRect,    4);
    nd_bind_fn(ctx, obj, "strokeRect",  nd_ctx_strokeRect,  4);
    nd_bind_fn(ctx, obj, "clearRect",   nd_ctx_clearRect,   4);
    nd_bind_fn(ctx, obj, "beginPath",   nd_ctx_beginPath,   0);
    nd_bind_fn(ctx, obj, "closePath",   nd_ctx_closePath,   0);
    nd_bind_fn(ctx, obj, "moveTo",      nd_ctx_moveTo,      2);
    nd_bind_fn(ctx, obj, "lineTo",      nd_ctx_lineTo,      2);
    nd_bind_fn(ctx, obj, "arc",         nd_ctx_arc,         6);
    nd_bind_fn(ctx, obj, "rect",        nd_ctx_rect,        4);
    nd_bind_fn(ctx, obj, "fill",        nd_ctx_fill,        0);
    nd_bind_fn(ctx, obj, "stroke",      nd_ctx_stroke,      0);
    nd_bind_fn(ctx, obj, "save",        nd_ctx_save,        0);
    nd_bind_fn(ctx, obj, "restore",     nd_ctx_restore,     0);
    nd_bind_fn(ctx, obj, "translate",   nd_ctx_translate,   2);
    nd_bind_fn(ctx, obj, "scale",       nd_ctx_scale,       2);
    nd_bind_fn(ctx, obj, "rotate",      nd_ctx_rotate,      1);
    nd_bind_fn(ctx, obj, "fillText",    nd_ctx_fillText,    4);
    nd_bind_fn(ctx, obj, "strokeText",  nd_ctx_fillText,    4);
    nd_bind_fn(ctx, obj, "measureText", nd_ctx_measureText, 1);
    nd_bind_fn(ctx, obj, "clip",        nd_event_noop,      0);
    nd_bind_fn(ctx, obj, "drawImage",   nd_event_noop,      9);
    nd_bind_fn(ctx, obj, "arcTo",       nd_event_noop,      5);
    nd_bind_fn(ctx, obj, "setTransform", nd_event_noop,     6);
    nd_bind_fn(ctx, obj, "transform",    nd_event_noop,     6);
    nd_bind_fn(ctx, obj, "resetTransform", nd_event_noop,   0);
    nd_bind_fn(ctx, obj, "createLinearGradient", nd_event_noop, 4);
    nd_bind_fn(ctx, obj, "createRadialGradient", nd_event_noop, 6);
    nd_bind_fn(ctx, obj, "createPattern",        nd_event_noop, 2);
    nd_bind_fn(ctx, obj, "createImageData",      nd_event_noop, 2);
    nd_bind_fn(ctx, obj, "getImageData",         nd_event_noop, 4);
    nd_bind_fn(ctx, obj, "putImageData",         nd_event_noop, 7);
    return obj;
}

static cairo_status_t
nd_canvas_png_write(void *closure, const unsigned char *data, unsigned int length)
{
    g_byte_array_append((GByteArray *)closure, data, length);
    return CAIRO_STATUS_SUCCESS;
}

static JSValue
nd_element_toDataURL(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el || !js_from_ctx(ctx)) return JS_NewString(ctx, "data:,");
    nd_canvas_state *st = nd_canvas_state_for(js_from_ctx(ctx), el);
    if (!st || !st->surf) return JS_NewString(ctx, "data:,");
    GByteArray *buf = g_byte_array_new();
    cairo_status_t s = cairo_surface_write_to_png_stream(st->surf,
        nd_canvas_png_write, buf);
    if (s != CAIRO_STATUS_SUCCESS) {
        g_byte_array_free(buf, TRUE);
        return JS_NewString(ctx, "data:,");
    }
    gchar *b64 = g_base64_encode(buf->data, buf->len);
    g_byte_array_free(buf, TRUE);
    char *url = g_strconcat("data:image/png;base64,", b64, NULL);
    JSValue ret = JS_NewString(ctx, url);
    g_free(url);
    g_free(b64);
    return ret;
}

static JSValue
nd_element_dispatchEvent(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    const nd_node *el = nd_unwrap_element(this_val);
    if (!el || !js_from_ctx(ctx) || argc < 1) return JS_FALSE;
    JSValue type_v = JS_GetPropertyStr(ctx, argv[0], "type");
    const char *type = JS_ToCString(ctx, type_v);
    JS_FreeValue(ctx, type_v);
    if (!type) return JS_TRUE;
    JSValue ev = JS_DupValue(ctx, argv[0]);
    JS_SetPropertyStr(ctx, ev, "target", nd_make_element(ctx, el));
    JS_SetPropertyStr(ctx, ev, "defaultPrevented", JS_FALSE);
    if (JS_IsUndefined(JS_GetPropertyStr(ctx, ev, "bubbles")))
        JS_SetPropertyStr(ctx, ev, "bubbles", JS_TRUE);
    nd_bind_fn(ctx, ev, "preventDefault",           nd_event_prevent_default, 0);
    nd_bind_fn(ctx, ev, "stopPropagation",          nd_event_stop_propagation, 0);
    nd_bind_fn(ctx, ev, "stopImmediatePropagation", nd_event_stop_immediate, 0);
    gboolean prevented = FALSE;
    nd_js_dispatch_built_event(js_from_ctx(ctx), el, type, ev, &prevented);
    JS_FreeCString(ctx, type);
    return prevented ? JS_FALSE : JS_TRUE;
}

static const JSCFunctionListEntry nd_element_proto_funcs[] = {
    JS_CGETSET_DEF("tagName",                nd_element_get_tagName,                NULL),
    JS_CGETSET_DEF("localName",              nd_element_get_localName,              NULL),
    JS_CGETSET_DEF("textContent",            nd_element_get_textContent,            nd_element_set_textContent),
    JS_CGETSET_DEF("innerText",              nd_element_get_textContent,            nd_element_set_innerText),
    JS_CGETSET_DEF("outerText",              nd_element_get_textContent,            nd_element_set_innerText),
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
    JS_CGETSET_DEF("offsetTop",     nd_element_get_offsetTop,    NULL),
    JS_CGETSET_DEF("offsetLeft",    nd_element_get_offsetLeft,   NULL),
    JS_CGETSET_DEF("offsetWidth",   nd_element_get_offsetWidth,  NULL),
    JS_CGETSET_DEF("offsetHeight",  nd_element_get_offsetHeight, NULL),
    JS_CGETSET_DEF("clientWidth",   nd_element_get_offsetWidth,  NULL),
    JS_CGETSET_DEF("clientHeight",  nd_element_get_offsetHeight, NULL),
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
    if (!js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc || argc < 1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_NULL;
    nd_node *found = nd_node_find_by_id(js_from_ctx(ctx)->current_doc, id);
    JS_FreeCString(ctx, id);
    return nd_make_element(ctx, found);
}

static JSValue
nd_document_get_documentElement(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc) return JS_NULL;
    nd_node *root = nd_node_find_first_element(js_from_ctx(ctx)->current_doc, "html");
    return nd_make_element(ctx, root);
}

static JSValue
nd_document_get_body(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc) return JS_NULL;
    nd_node *body = nd_node_find_first_element(js_from_ctx(ctx)->current_doc, "body");
    return nd_make_element(ctx, body);
}

static JSValue
nd_document_get_head(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc) return JS_NULL;
    nd_node *head = nd_node_find_first_element(js_from_ctx(ctx)->current_doc, "head");
    return nd_make_element(ctx, head);
}

static JSValue
nd_document_get_activeElement(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc) return JS_NULL;
    nd_node *body = nd_node_find_first_element(js_from_ctx(ctx)->current_doc, "body");
    return nd_make_element(ctx, body);
}

static JSValue
nd_document_collect_by_tag(JSContext *ctx, const char *tag)
{
    JSValue arr = JS_NewArray(ctx);
    if (!js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc) return arr;
    uint32_t idx = 0;
    GQueue q = G_QUEUE_INIT;
    g_queue_push_tail(&q, js_from_ctx(ctx)->current_doc);
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
    if (!js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc) return arr;
    uint32_t idx = 0;
    GQueue q = G_QUEUE_INIT;
    g_queue_push_tail(&q, js_from_ctx(ctx)->current_doc);
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
    if (!js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc) return JS_NULL;
    nd_node *body = nd_node_find_first_element(js_from_ctx(ctx)->current_doc, "body");
    return nd_make_element(ctx, body);
}

static JSValue
nd_document_elements_from_point(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue arr = JS_NewArray(ctx);
    if (!js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc) return arr;
    nd_node *body = nd_node_find_first_element(js_from_ctx(ctx)->current_doc, "body");
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
    if (js_from_ctx(ctx)) g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, copy);
    (void)this_val;
    return nd_make_element(ctx, copy);
}

static JSValue
nd_document_get_links(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    JSValue arr = JS_NewArray(ctx);
    if (!js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc) return arr;
    uint32_t idx = 0;
    GQueue q = G_QUEUE_INIT;
    g_queue_push_tail(&q, js_from_ctx(ctx)->current_doc);
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
    nd_js_emit(js_from_ctx(ctx), "", ctx, argc, argv);
    return JS_UNDEFINED;
}

static JSValue
nd_js_console_warn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    nd_js_emit(js_from_ctx(ctx), "[warn]", ctx, argc, argv);
    return JS_UNDEFINED;
}

static JSValue
nd_js_console_error(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    nd_js_emit(js_from_ctx(ctx), "[error]", ctx, argc, argv);
    return JS_UNDEFINED;
}

static JSValue
nd_js_alert(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    nd_js_emit(js_from_ctx(ctx), "[alert]", ctx, argc, argv);
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
#ifdef G_OS_WIN32
        JS_SetMaxStackSize(js->rt, (size_t)512 * 1024);
#endif
    }
    if (!js->rt) { g_free(js); return NULL; }
    js->ctx = JS_NewContext(js->rt);
    if (!js->ctx) { JS_FreeRuntime(js->rt); g_free(js); return NULL; }
    JS_SetContextOpaque(js->ctx, js);
    JS_SetRuntimeOpaque(js->rt, js);
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
    js->pending_fetches = g_ptr_array_new();
    js->pending_xhrs    = g_ptr_array_new();
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
                      JS_NewString(ctx, "Nordstjernen/" ND_VERSION));
    JS_SetPropertyStr(ctx, navigator, "appName",
                      JS_NewString(ctx, "Nordstjernen"));
    JS_SetPropertyStr(ctx, navigator, "appVersion",
                      JS_NewString(ctx, ND_VERSION));
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

    nd_bind_ctor(ctx, global, "MutationObserver",     nd_window_observer_ctor,       1);
    nd_bind_ctor(ctx, global, "IntersectionObserver", nd_intersection_observer_ctor, 1);
    nd_bind_ctor(ctx, global, "ResizeObserver",       nd_resize_observer_ctor,       1);
    nd_bind_ctor(ctx, global, "PerformanceObserver",  nd_window_observer_ctor,       1);

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
    };
    nd_bind_fns(ctx, global, nd_event_noop, window_noops, G_N_ELEMENTS(window_noops));
    nd_bind_fn(ctx, global, "open",                  nd_window_open_method,            3);
    nd_bind_fn(ctx, global, "confirm",               nd_window_confirm,                1);
    nd_bind_fn(ctx, global, "prompt",                nd_window_prompt,                 2);
    nd_bind_fn(ctx, global, "matchMedia",            nd_window_matchMedia,             1);
    nd_bind_fn(ctx, global, "getComputedStyle",      nd_window_getComputedStyle,       1);
    nd_bind_fn(ctx, global, "requestAnimationFrame", nd_window_requestAnimationFrame,  1);
    nd_bind_fn(ctx, global, "cancelAnimationFrame",  nd_window_cancelAnimationFrame,   1);

    nd_bind_ctor(ctx, global, "Event",        nd_event_ctor,        2);
    nd_bind_ctor(ctx, global, "CustomEvent",  nd_custom_event_ctor, 2);
    nd_bind_ctor(ctx, global, "MouseEvent",   nd_mouse_event_ctor,  2);
    nd_bind_ctor(ctx, global, "PointerEvent", nd_mouse_event_ctor,  2);
    nd_bind_ctor(ctx, global, "UIEvent",      nd_event_ctor,        2);

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
    JSValue url_ctor = nd_make_ctor(ctx, nd_window_url_ctor, "URL", 2);
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

    nd_bind_ctor(ctx, global, "Image",           nd_window_image_ctor,           2);
    nd_bind_ctor(ctx, global, "Audio",           nd_window_audio_ctor,           1);
    nd_bind_ctor(ctx, global, "Option",          nd_window_option_ctor,          4);
    nd_bind_ctor(ctx, global, "URLSearchParams", nd_window_usp_ctor,             1);
    nd_bind_ctor(ctx, global, "XMLHttpRequest",  nd_window_xhr_ctor,             0);
    nd_bind_ctor(ctx, global, "DOMParser",       nd_window_dom_parser_ctor,      0);
    nd_bind_ctor(ctx, global, "FormData",        nd_window_form_data_ctor,       1);
    nd_bind_ctor(ctx, global, "AbortController", nd_window_abort_controller_ctor, 0);

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
    nd_bind_ctor(ctx, global, "TextEncoder", nd_window_text_encoder_ctor, 0);
    nd_bind_ctor(ctx, global, "TextDecoder", nd_window_text_decoder_ctor, 0);

    nd_bind_ctor(ctx, global, "KeyboardEvent", nd_window_event_ctor, 2);
    static const char *event_subclasses[] = {
        "ProgressEvent","ErrorEvent","HashChangeEvent","PopStateEvent",
        "MessageEvent","StorageEvent","PageTransitionEvent","BeforeUnloadEvent",
        "SubmitEvent","InputEvent","TouchEvent","DragEvent","WheelEvent",
        "FocusEvent","AnimationEvent","TransitionEvent","ClipboardEvent",
        "CompositionEvent","CloseEvent",
        "MediaQueryListEvent","BlobEvent","FontFaceSetLoadEvent",
        "GamepadEvent","DeviceMotionEvent","DeviceOrientationEvent",
        "PromiseRejectionEvent","SecurityPolicyViolationEvent",
        "TrustedTypePolicyFactory",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(event_subclasses); i++)
        nd_bind_ctor(ctx, global, event_subclasses[i], nd_event_ctor, 2);

    static const nd_fn_def event_base_ctors[] = {
        { "EventTarget", 0 }, { "Node", 0 }, { "Element", 0 },
        { "HTMLElement", 0 }, { "SVGElement", 0 }, { "SVGSVGElement", 0 },
        { "Document", 0 }, { "HTMLDocument", 0 },
        { "Window", 0 },
    };
    nd_bind_ctors(ctx, global, nd_window_event_ctor,
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
    nd_bind_fn(ctx, global, "queueMicrotask",   nd_window_queue_microtask,   1);
    JS_AddIntrinsicDOMException(ctx);
    nd_bind_ctor(ctx, global, "MessageChannel",   nd_window_message_channel,   0);
    nd_bind_ctor(ctx, global, "BroadcastChannel", nd_window_broadcast_channel, 1);
    nd_bind_ctor(ctx, global, "Notification",   nd_window_notification,      2);
    nd_bind_ctor(ctx, global, "Worker",         nd_throws_unsupported,       1);
    nd_bind_ctor(ctx, global, "SharedWorker",   nd_throws_unsupported,       1);
    nd_bind_ctor(ctx, global, "WebSocket",      nd_throws_unsupported,       2);
    nd_bind_ctor(ctx, global, "EventSource",    nd_throws_unsupported,       2);

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
    g_active_js = js;
    return js;
}

static JSValue
nd_document_getElementsByTagName(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue arr = JS_NewArray(ctx);
    if (!js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc || argc < 1) return arr;
    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return arr;
    uint32_t i = 0;
    nd_collect_by_tag(js_from_ctx(ctx)->current_doc, tag, ctx, arr, &i);
    JS_FreeCString(ctx, tag);
    return arr;
}

static JSValue
nd_document_getElementsByClassName(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue arr = JS_NewArray(ctx);
    if (!js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc || argc < 1) return arr;
    const char *cls = JS_ToCString(ctx, argv[0]);
    if (!cls) return arr;
    uint32_t i = 0;
    nd_collect_by_class(js_from_ctx(ctx)->current_doc, cls, ctx, arr, &i);
    JS_FreeCString(ctx, cls);
    return arr;
}

static JSValue
nd_document_createElement(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!js_from_ctx(ctx) || argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;
    char *lower = g_ascii_strdown(name, -1);
    JS_FreeCString(ctx, name);
    nd_node *el = nd_node_new_element(lower);
    g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, el);
    return nd_make_element(ctx, el);
}

static JSValue
nd_document_createTextNode(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!js_from_ctx(ctx) || argc < 1) return JS_NULL;
    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_NULL;
    char *dup = g_strdup(text);
    JS_FreeCString(ctx, text);
    nd_node *n = nd_node_new_text(dup);
    g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, n);
    return nd_make_element(ctx, n);
}

static JSValue
nd_document_createElementNS(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!js_from_ctx(ctx) || argc < 2) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[1]);
    if (!name) return JS_NULL;
    char *lower = g_ascii_strdown(name, -1);
    JS_FreeCString(ctx, name);
    nd_node *el = nd_node_new_element(lower);
    g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, el);
    return nd_make_element(ctx, el);
}

static JSValue
nd_document_createComment(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!js_from_ctx(ctx) || argc < 1) return JS_NULL;
    const char *text = JS_ToCString(ctx, argv[0]);
    char *dup = text ? g_strdup(text) : g_strdup("");
    if (text) JS_FreeCString(ctx, text);
    nd_node *n = nd_node_new_comment(dup);
    g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, n);
    return nd_make_element(ctx, n);
}

static JSValue
nd_document_createDocumentFragment(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (!js_from_ctx(ctx)) return JS_NULL;
    nd_node *frag = nd_node_new_document();
    g_ptr_array_add(js_from_ctx(ctx)->orphan_nodes, frag);
    return nd_make_element(ctx, frag);
}

static JSValue
nd_document_querySelector(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!js_from_ctx(ctx)) return JS_NULL;
    return nd_query_selector_impl(ctx, js_from_ctx(ctx)->current_doc, argc, argv,
                                  FALSE, TRUE);
}

static JSValue
nd_document_querySelectorAll(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!js_from_ctx(ctx)) return JS_NewArray(ctx);
    return nd_query_selector_impl(ctx, js_from_ctx(ctx)->current_doc, argc, argv,
                                  TRUE, TRUE);
}

static JSValue
nd_document_addEventListener(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc || argc < 2) return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    if (!JS_IsFunction(ctx, argv[1])) { JS_FreeCString(ctx, type); return JS_UNDEFINED; }
    gboolean capture = FALSE, once = FALSE;
    if (argc >= 3) nd_listener_parse_options(ctx, argv[2], &capture, &once);
    nd_listener *l = g_new0(nd_listener, 1);
    l->target = js_from_ctx(ctx)->current_doc;
    l->type   = g_strdup(type);
    l->cb     = JS_DupValue(ctx, argv[1]);
    l->capture = capture;
    l->once    = once;
    g_ptr_array_add(js_from_ctx(ctx)->listeners, l);
    nd_node_arm_js_invalidate(js_from_ctx(ctx)->current_doc);
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

static JSValue
nd_document_removeEventListener(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    nd_js *js = js_from_ctx(ctx);
    if (!js || !js->current_doc || argc < 2) return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    for (guint i = 0; i < js->listeners->len; i++) {
        nd_listener *l = g_ptr_array_index(js->listeners, i);
        if (nd_listener_is_tombstoned(l)) continue;
        if (l->target == js->current_doc && strcmp(l->type, type) == 0 &&
            JS_VALUE_GET_TAG(l->cb) == JS_VALUE_GET_TAG(argv[1]) &&
            JS_VALUE_GET_PTR(l->cb) == JS_VALUE_GET_PTR(argv[1])) {
            nd_listener_tombstone(ctx, l);
            break;
        }
    }
    nd_listeners_sweep(js);
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

static nd_node *
nd_doc_find_title_node(JSContext *ctx)
{
    nd_js *js = js_from_ctx(ctx);
    if (!js || !js->current_doc) return NULL;
    return nd_node_find_first_element(js->current_doc, "title");
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
    if (!js_from_ctx(ctx) || !js_from_ctx(ctx)->current_doc || argc < 1) return arr;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return arr;
    uint32_t i = 0;
    nd_collect_by_name(js_from_ctx(ctx)->current_doc, name, ctx, arr, &i);
    JS_FreeCString(ctx, name);
    return arr;
}

static JSValue
nd_document_get_title(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    nd_node *t = nd_doc_find_title_node(ctx);
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
    nd_node *t = nd_doc_find_title_node(ctx);
    if (!t && js_from_ctx(ctx) && js_from_ctx(ctx)->current_doc) {
        nd_node *head = nd_node_find_first_element(js_from_ctx(ctx)->current_doc, "head");
        if (!head) head = js_from_ctx(ctx)->current_doc;
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
        { nd_js *_j = js_from_ctx(ctx); if (_j) _j->mutated = TRUE; }
    }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue
nd_document_get_cookie(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!js_from_ctx(ctx)) return JS_NewString(ctx, "");
    return JS_NewString(ctx, js_from_ctx(ctx)->cookie_value ? js_from_ctx(ctx)->cookie_value : "");
}

static JSValue
nd_document_set_cookie(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    (void)this_val;
    if (!js_from_ctx(ctx)) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (!s) return JS_UNDEFINED;
    const char *eq = strchr(s, '=');
    const char *semi = strchr(s, ';');
    if (!eq) { JS_FreeCString(ctx, s); return JS_UNDEFINED; }
    gsize key_len = (gsize)(eq - s);
    gsize pair_len = semi ? (gsize)(semi - s) : strlen(s);
    char *pair = g_strndup(s, pair_len);
    char *new_jar = NULL;
    if (js_from_ctx(ctx)->cookie_value) {
        new_jar = g_strdup(js_from_ctx(ctx)->cookie_value);
        char *needle = g_strndup(s, key_len + 1);
        char *scan = new_jar;
        while ((scan = strstr(scan, needle)) != NULL) {
            if (scan == new_jar || *(scan - 1) == ' ' || *(scan - 1) == ';') {
                char *end = strstr(scan, "; ");
                char *rest = end ? end + 2 : NULL;
                *scan = '\0';
                char *merged = g_strconcat(new_jar, rest ? rest : "", NULL);
                gsize off = (gsize)(scan - new_jar);
                g_free(new_jar);
                new_jar = merged;
                scan = new_jar + off;
            } else {
                scan++;
            }
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
    g_free(js_from_ctx(ctx)->cookie_value);
    js_from_ctx(ctx)->cookie_value = new_jar;
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue
nd_document_get_referrer(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!js_from_ctx(ctx)) return JS_NewString(ctx, "");
    return JS_NewString(ctx, js_from_ctx(ctx)->referrer ? js_from_ctx(ctx)->referrer : "");
}

static JSValue
nd_document_get_readyState(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    if (!js_from_ctx(ctx)) return JS_NewString(ctx, "loading");
    static const char *names[] = { "loading", "interactive", "complete" };
    int idx = js_from_ctx(ctx)->ready_state;
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
    if (!js_from_ctx(ctx)) return JS_NewString(ctx, "");
    return JS_NewString(ctx, js_from_ctx(ctx)->current_url ? js_from_ctx(ctx)->current_url : "");
}

static const char *
nd_loc_url(JSContext *ctx)
{
    nd_js *js = js_from_ctx(ctx);
    return js && js->current_url ? js->current_url : "";
}

static JSValue
nd_location_get_protocol(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    const char *u = nd_loc_url(ctx);
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
    const char *start = p ? p + 3 : u;
    const char *e = start;
    while (*e && *e != '/' && *e != '?' && *e != '#') {
        if (*e == '@') return e + 1;
        e++;
    }
    return start;
}

static JSValue
nd_location_get_host(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    const char *u = nd_loc_url(ctx);
    const char *h = nd_loc_host_start(u);
    const char *e = h;
    while (*e && *e != '/' && *e != '?' && *e != '#') e++;
    return JS_NewStringLen(ctx, h, (gsize)(e - h));
}

static JSValue
nd_location_get_hostname(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    const char *u = nd_loc_url(ctx);
    const char *h = nd_loc_host_start(u);
    const char *e = h;
    while (*e && *e != ':' && *e != '/' && *e != '?' && *e != '#') e++;
    return JS_NewStringLen(ctx, h, (gsize)(e - h));
}

static JSValue
nd_location_get_port(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    const char *u = nd_loc_url(ctx);
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
    const char *u = nd_loc_url(ctx);
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
    const char *u = nd_loc_url(ctx);
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
    const char *u = nd_loc_url(ctx);
    const char *h = strchr(u, '#');
    return JS_NewString(ctx, h ? h : "");
}

static JSValue
nd_location_get_origin(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    const char *u = nd_loc_url(ctx);
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

static gboolean
nd_location_target_allowed(const char *s)
{
    if (!s || !*s) return FALSE;
    if (s[0] == '/' || s[0] == '?' || s[0] == '#') return TRUE;
    const char *colon = strchr(s, ':');
    const char *slash = strchr(s, '/');
    if (!colon || (slash && slash < colon)) return TRUE;
    static const char *const allowed[] = {
        "http:", "https:", "about:", "data:", "mailto:", NULL,
    };
    for (int i = 0; allowed[i]; i++)
        if (g_ascii_strncasecmp(s, allowed[i], strlen(allowed[i])) == 0)
            return TRUE;
    return FALSE;
}

static void
nd_location_log_blocked(nd_js *js, const char *s)
{
    if (!js || !js->log_cb) return;
    char *line = g_strdup_printf(
        "blocked navigation: scheme not allowed (%.64s)", s ? s : "");
    js->log_cb(line, js->log_user_data);
    g_free(line);
}

static JSValue
nd_location_set_href(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    (void)this_val;
    nd_js *js = js_from_ctx(ctx);
    if (!js || !js->nav_cb) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, val);
    if (!s) return JS_UNDEFINED;
    if (!nd_location_target_allowed(s)) {
        nd_location_log_blocked(js, s);
        JS_FreeCString(ctx, s);
        return JS_UNDEFINED;
    }
    js->nav_cb(s, FALSE, js->nav_user_data);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue
nd_location_assign(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    nd_js *js = js_from_ctx(ctx);
    if (!js || !js->nav_cb || argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_UNDEFINED;
    if (!nd_location_target_allowed(s)) {
        nd_location_log_blocked(js, s);
        JS_FreeCString(ctx, s);
        return JS_UNDEFINED;
    }
    js->nav_cb(s, FALSE, js->nav_user_data);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue
nd_location_reload(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (js_from_ctx(ctx) && js_from_ctx(ctx)->nav_cb)
        js_from_ctx(ctx)->nav_cb(js_from_ctx(ctx)->current_url, TRUE, js_from_ctx(ctx)->nav_user_data);
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
nd_js_reset_runtime_state(nd_js *js)
{
    if (!js) return;

    if (js->timers)
        g_hash_table_remove_all(js->timers);

    if (js->raf_pending) {
        for (guint i = 0; i < js->raf_pending->len; i++) {
            nd_raf_entry *e = &g_array_index(js->raf_pending, nd_raf_entry, i);
            JS_FreeValue(js->ctx, e->cb);
        }
        g_array_set_size(js->raf_pending, 0);
    }

    if (js->listeners) {
        for (guint i = 0; i < js->listeners->len; i++) {
            nd_listener *l = g_ptr_array_index(js->listeners, i);
            JS_FreeValue(js->ctx, l->cb);
            g_free(l->type);
            g_free(l);
        }
        g_ptr_array_set_size(js->listeners, 0);
    }

    if (js->pending_fetches) {
        for (guint i = 0; i < js->pending_fetches->len; i++) {
            nd_js_fetch_state *st = g_ptr_array_index(js->pending_fetches, i);
            JS_FreeValue(js->ctx, st->resolve);
            JS_FreeValue(js->ctx, st->reject);
            st->resolve = JS_UNDEFINED;
            st->reject  = JS_UNDEFINED;
            st->ctx = NULL;
            st->js  = NULL;
        }
        g_ptr_array_set_size(js->pending_fetches, 0);
    }

    if (js->pending_xhrs) {
        for (guint i = 0; i < js->pending_xhrs->len; i++) {
            nd_xhr_state *st = g_ptr_array_index(js->pending_xhrs, i);
            JS_FreeValue(js->ctx, st->obj);
            st->obj = JS_UNDEFINED;
            st->ctx = NULL;
            st->js  = NULL;
        }
        g_ptr_array_set_size(js->pending_xhrs, 0);
    }

    if (js->orphan_nodes) {
        for (guint i = 0; i < js->orphan_nodes->len; i++)
            nd_node_free(g_ptr_array_index(js->orphan_nodes, i));
        g_ptr_array_set_size(js->orphan_nodes, 0);
    }

    nd_drain_microtasks(js);
    JS_RunGC(js->rt);
}

static void
nd_js_install_document(nd_js *js, nd_node *doc, const char *base_url)
{
    nd_js_reset_runtime_state(js);

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
        { "Range", 0 }, { "NodeFilter", 0 },
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
        { "HTMLAreaElement", 0 }, { "HTMLBaseElement", 0 },
        { "HTMLBRElement", 0 }, { "HTMLDataElement", 0 },
        { "HTMLDataListElement", 0 }, { "HTMLDListElement", 0 },
        { "HTMLEmbedElement", 0 }, { "HTMLFieldSetElement", 0 },
        { "HTMLHeadingElement", 0 }, { "HTMLHRElement", 0 },
        { "HTMLLegendElement", 0 }, { "HTMLLIElement", 0 },
        { "HTMLMapElement", 0 }, { "HTMLMenuElement", 0 },
        { "HTMLMeterElement", 0 }, { "HTMLModElement", 0 },
        { "HTMLObjectElement", 0 }, { "HTMLOListElement", 0 },
        { "HTMLOptGroupElement", 0 }, { "HTMLOutputElement", 0 },
        { "HTMLParagraphElement", 0 }, { "HTMLPictureElement", 0 },
        { "HTMLPreElement", 0 }, { "HTMLProgressElement", 0 },
        { "HTMLQuoteElement", 0 }, { "HTMLSlotElement", 0 },
        { "HTMLSourceElement", 0 }, { "HTMLTableCaptionElement", 0 },
        { "HTMLTableColElement", 0 }, { "HTMLTableSectionElement", 0 },
        { "HTMLTemplateElement", 0 }, { "HTMLTimeElement", 0 },
        { "HTMLTitleElement", 0 }, { "HTMLTrackElement", 0 },
        { "HTMLUListElement", 0 }, { "HTMLUnknownElement", 0 },
        { "HTMLFontElement", 0 }, { "HTMLMarqueeElement", 0 },
        { "HTMLFrameElement", 0 }, { "HTMLFrameSetElement", 0 },
        { "HTMLParamElement", 0 }, { "HTMLDirectoryElement", 0 },
        { "CharacterData", 0 },
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
    nd_bind_ctors(ctx, global, nd_window_event_ctor, shim_ctors, G_N_ELEMENTS(shim_ctors));
    JS_FreeValue(ctx, xml_serializer);

    JS_FreeValue(ctx, global);

    JSValue shim = JS_Eval(ctx, nd_js_jquery_shim_src,
                           sizeof(nd_js_jquery_shim_src) - 1,
                           "<jquery-shim>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(shim)) {
        JSValue ex = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, ex);
        if (js->log_cb && msg)
            js->log_cb(msg, js->log_user_data);
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, ex);
    }
    JS_FreeValue(ctx, shim);
    nd_drain_microtasks(js);
}

void
nd_js_free(nd_js *js)
{
    if (!js) return;
    if (g_active_js == js) g_active_js = NULL;
    nd_storage_flush(js);
    g_free(js->local_storage_origin);
    g_free(js->local_storage_path);
    g_free(js->cookie_value);
    g_free(js->referrer);
    g_free(js->current_url);
    if (js->timers) g_hash_table_destroy(js->timers);
    if (js->raf_pending) {
        for (guint i = 0; i < js->raf_pending->len; i++) {
            nd_raf_entry *e = &g_array_index(js->raf_pending, nd_raf_entry, i);
            JS_FreeValue(js->ctx, e->cb);
        }
        g_array_free(js->raf_pending, TRUE);
    }
    if (js->canvas_states) g_hash_table_destroy(js->canvas_states);
    if (js->listeners) {
        for (guint i = 0; i < js->listeners->len; i++) {
            nd_listener *l = g_ptr_array_index(js->listeners, i);
            if (!nd_listener_is_tombstoned(l)) {
                JS_FreeValue(js->ctx, l->cb);
                g_free(l->type);
            }
            g_free(l);
        }
        g_ptr_array_free(js->listeners, TRUE);
        js->listeners = NULL;
    }
    if (js->pending_fetches) {
        for (guint i = 0; i < js->pending_fetches->len; i++) {
            nd_js_fetch_state *st = g_ptr_array_index(js->pending_fetches, i);
            JS_FreeValue(js->ctx, st->resolve);
            JS_FreeValue(js->ctx, st->reject);
            st->ctx = NULL;
            st->js  = NULL;
        }
        g_ptr_array_free(js->pending_fetches, TRUE);
        js->pending_fetches = NULL;
    }
    if (js->pending_xhrs) {
        for (guint i = 0; i < js->pending_xhrs->len; i++) {
            nd_xhr_state *st = g_ptr_array_index(js->pending_xhrs, i);
            JS_FreeValue(js->ctx, st->obj);
            st->ctx = NULL;
            st->js  = NULL;
        }
        g_ptr_array_free(js->pending_xhrs, TRUE);
        js->pending_xhrs = NULL;
    }
    if (js->orphan_nodes) {
        for (guint i = 0; i < js->orphan_nodes->len; i++)
            nd_node_free(g_ptr_array_index(js->orphan_nodes, i));
        g_ptr_array_free(js->orphan_nodes, TRUE);
    }
    if (js->mutation_observers) {
        for (guint i = 0; i < js->mutation_observers->len; i++) {
            nd_mut_observer *o = g_ptr_array_index(js->mutation_observers, i);
            if (!o) continue;
            o->disconnected = TRUE;
            if (o->records) g_ptr_array_set_size(o->records, 0);
            if (o->targets) g_array_set_size(o->targets, 0);
        }
        g_ptr_array_free(js->mutation_observers, TRUE);
        js->mutation_observers = NULL;
    }
    if (js->local_storage)   g_hash_table_destroy(js->local_storage);
    if (js->session_storage) g_hash_table_destroy(js->session_storage);
    for (int i = 0; i < 4; i++) {
        int r;
        JSContext *ctx_out = NULL;
        while ((r = JS_ExecutePendingJob(js->rt, &ctx_out)) > 0) ;
        if (r < 0) {
            JSValue ex = JS_GetException(ctx_out ? ctx_out : js->ctx);
            JS_FreeValue(ctx_out ? ctx_out : js->ctx, ex);
        }
        JS_RunGC(js->rt);
    }
    JS_FreeContext(js->ctx);
    JS_FreeRuntime(js->rt);
    g_free(js);
}

static void
nd_js_eval(nd_js *js, const char *src, gsize len, const char *origin)
{
    char *copy = g_strndup(src ? src : "", len);
    js->eval_deadline_us = g_get_monotonic_time() + nd_js_eval_budget_us();
    JSValue v = JS_Eval(js->ctx, copy, len, origin, JS_EVAL_TYPE_GLOBAL);
    g_free(copy);
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
}

#define ND_MAX_SCRIPT_BYTES (16u * 1024u * 1024u)

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
        const char *nonce = nd_element_get_attr(n, "nonce");
        const char *src = nd_element_get_attr(n, "src");
        if (src && *src) {
            char *abs = nd_url_resolve(origin, src);
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
            if (js->csp && !nd_csp_allows(js->csp, ND_CSP_SCRIPT, abs, origin)) {
                if (js->log_cb) {
                    char *line = g_strdup_printf("CSP blocked: script %s", abs);
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
            nd_response_free(resp);
            g_clear_error(&err);
            g_free(abs);
            return;
        }
        for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
            if (c->kind == ND_NODE_TEXT && c->text) {
                gsize tlen = strlen(c->text);
                if (!nd_csp_inline_script_allowed(js->csp, c->text, tlen, nonce)) {
                    if (js->log_cb) {
                        char *line = g_strdup_printf(
                            "CSP blocked: inline <script> on %s", origin);
                        js->log_cb(line, js->log_user_data);
                        g_free(line);
                    }
                    continue;
                }
                nd_js_eval(js, c->text, tlen, origin);
            }
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
nd_js_set_csp(nd_js *js, const nd_csp *csp)
{
    if (!js) return;
    js->csp = csp;
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
    js->eval_deadline_us = g_get_monotonic_time() + nd_js_eval_budget_us();
    JSValue v = JS_Eval(js->ctx, src, strlen(src), origin ? origin : "console", JS_EVAL_TYPE_GLOBAL);
    js->eval_deadline_us = 0;
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
    return out;
}
