/* Nordstjernen — WebExtensions loader and content-script host.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "ext.h"

#include <string.h>

#include <glib/gstdio.h>
#include <libpsl.h>

#include "config.h"

typedef struct {
    GPtrArray *matches;
    char      *all_js;
    char      *all_css;
    gboolean   at_start;
} ns_ext_cs;

typedef struct {
    int        priority;
    int        action;
    int        third_party;
    char      *url_filter;
    GRegex    *regex;
    gboolean   case_sensitive;
    GPtrArray *request_domains;
    GPtrArray *excluded_request_domains;
    GPtrArray *initiator_domains;
    GPtrArray *excluded_initiator_domains;
    GPtrArray *resource_types;
    GPtrArray *excluded_resource_types;
} ns_dnr_rule;

typedef struct {
    GPtrArray *domains;
    GPtrArray *excluded_domains;
    char      *selector;
} ns_cosmetic_rule;

typedef struct {
    char      *id;
    char      *name;
    char      *version;
    char      *base_dir;
    char      *manifest_json;
    GPtrArray *content_scripts;
    GPtrArray *dnr_rules;
    GPtrArray *cosmetic_rules;
} ns_ext;

static GPtrArray  *g_exts;
static GHashTable *g_blocked_hosts;

static gboolean
ns_ext_area_ok(const char *area)
{
    return area && (strcmp(area, "local") == 0 ||
                    strcmp(area, "sync") == 0 ||
                    strcmp(area, "managed") == 0);
}

static gboolean
ns_ext_wildcard(const char *pat, const char *str)
{
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return TRUE;
            for (; *str; str++)
                if (ns_ext_wildcard(pat, str)) return TRUE;
            return FALSE;
        }
        if (*pat != *str) return FALSE;
        pat++;
        str++;
    }
    return *str == 0;
}

static void
ns_ext_url_split(const char *url, char **scheme, char **host, char **path)
{
    *scheme = NULL;
    *host = NULL;
    *path = NULL;
    if (!url) return;
    const char *sep = strstr(url, "://");
    if (!sep) return;
    *scheme = g_ascii_strdown(url, sep - url);
    const char *h = sep + 3;
    const char *he = h;
    while (*he && *he != '/' && *he != '?' && *he != '#') he++;
    *host = g_ascii_strdown(h, he - h);
    if (*he == '/') {
        const char *pe = he;
        while (*pe && *pe != '?' && *pe != '#') pe++;
        *path = g_strndup(he, pe - he);
    } else {
        *path = g_strdup("/");
    }
}

static gboolean
ns_ext_scheme_generic(const char *scheme)
{
    return scheme && (strcmp(scheme, "http") == 0 ||
                      strcmp(scheme, "https") == 0 ||
                      strcmp(scheme, "ws") == 0 ||
                      strcmp(scheme, "wss") == 0);
}

static gboolean
ns_ext_host_match(const char *pat, const char *host)
{
    if (!pat || !host) return FALSE;
    if (strcmp(pat, "*") == 0) return TRUE;
    if (pat[0] == '*' && pat[1] == '.') {
        const char *suffix = pat + 2;
        if (strcmp(host, suffix) == 0) return TRUE;
        gsize hl = strlen(host), sl = strlen(suffix);
        return hl > sl && host[hl - sl - 1] == '.' &&
               strcmp(host + hl - sl, suffix) == 0;
    }
    return strcmp(pat, host) == 0;
}

static gboolean
ns_ext_pattern_match(const char *pattern, const char *url)
{
    if (!pattern || !url) return FALSE;
    g_autofree char *uscheme = NULL;
    g_autofree char *uhost = NULL;
    g_autofree char *upath = NULL;
    ns_ext_url_split(url, &uscheme, &uhost, &upath);
    if (!uscheme || !uhost || !upath) return FALSE;

    if (strcmp(pattern, "<all_urls>") == 0)
        return ns_ext_scheme_generic(uscheme) ||
               strcmp(uscheme, "ftp") == 0 ||
               strcmp(uscheme, "file") == 0 ||
               strcmp(uscheme, "data") == 0;

    g_autofree char *pscheme = NULL;
    g_autofree char *phost = NULL;
    g_autofree char *ppath = NULL;
    ns_ext_url_split(pattern, &pscheme, &phost, &ppath);
    if (!pscheme || !phost || !ppath) return FALSE;

    if (strcmp(pscheme, "*") == 0) {
        if (!ns_ext_scheme_generic(uscheme)) return FALSE;
    } else if (strcmp(pscheme, uscheme) != 0) {
        return FALSE;
    }
    if (!ns_ext_host_match(phost, uhost)) return FALSE;
    return ns_ext_wildcard(ppath, upath);
}

static ns_ext *
ns_ext_lookup(const char *id)
{
    if (!g_exts || !id) return NULL;
    for (guint i = 0; i < g_exts->len; i++) {
        ns_ext *e = g_ptr_array_index(g_exts, i);
        if (e->id && strcmp(e->id, id) == 0) return e;
    }
    return NULL;
}

static char *
ns_ext_storage_path(const char *id, const char *area)
{
    const ns_config *c = ns_config_get();
    if (c && c->private_mode) return NULL;
    if (!id || !ns_ext_area_ok(area)) return NULL;
    g_autofree char *hash =
        g_compute_checksum_for_string(G_CHECKSUM_SHA256, id, -1);
    g_autofree char *dir = g_build_filename(g_get_user_data_dir(),
                                            NS_APP_DIR_NAME,
                                            "ext-storage", hash, NULL);
    g_mkdir_with_parents(dir, 0700);
    g_chmod(dir, 0700);
    g_autofree char *file = g_strdup_printf("%s.json", area);
    return g_build_filename(dir, file, NULL);
}

static JSValue
ns_ext_js_manifest(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "{}");
    const char *id = JS_ToCString(ctx, argv[0]);
    ns_ext *e = ns_ext_lookup(id);
    JSValue r = JS_NewString(ctx, e && e->manifest_json ? e->manifest_json : "{}");
    JS_FreeCString(ctx, id);
    return r;
}

static JSValue
ns_ext_js_base(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    const char *id = JS_ToCString(ctx, argv[0]);
    ns_ext *e = ns_ext_lookup(id);
    JSValue r = JS_NewString(ctx, e && e->base_dir ? e->base_dir : "");
    JS_FreeCString(ctx, id);
    return r;
}

static JSValue
ns_ext_js_sread(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_NewString(ctx, "{}");
    const char *id = JS_ToCString(ctx, argv[0]);
    const char *area = JS_ToCString(ctx, argv[1]);
    char *out = NULL;
    g_autofree char *path = ns_ext_storage_path(id, area);
    if (path) {
        char *data = NULL;
        if (g_file_get_contents(path, &data, NULL, NULL)) out = data;
    }
    JSValue r = JS_NewString(ctx, out ? out : "{}");
    g_free(out);
    JS_FreeCString(ctx, id);
    JS_FreeCString(ctx, area);
    return r;
}

static JSValue
ns_ext_js_swrite(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3) return JS_FALSE;
    const char *id = JS_ToCString(ctx, argv[0]);
    const char *area = JS_ToCString(ctx, argv[1]);
    const char *json = JS_ToCString(ctx, argv[2]);
    gboolean ok = FALSE;
    g_autofree char *path = ns_ext_storage_path(id, area);
    if (path && json && g_file_set_contents(path, json, -1, NULL)) {
        g_chmod(path, 0600);
        ok = TRUE;
    }
    JS_FreeCString(ctx, id);
    JS_FreeCString(ctx, area);
    JS_FreeCString(ctx, json);
    return JS_NewBool(ctx, ok);
}

static JSValue
ns_ext_js_platform(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
#if defined(G_OS_WIN32)
    return JS_NewString(ctx, "win");
#elif defined(__APPLE__)
    return JS_NewString(ctx, "mac");
#else
    return JS_NewString(ctx, "linux");
#endif
}

static JSValue
ns_ext_js_uilang(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    const char * const *names = g_get_language_names();
    const char *src = names && names[0] ? names[0] : "en";
    GString *out = g_string_new(NULL);
    for (const char *p = src; *p && *p != '.' && *p != '@'; p++)
        g_string_append_c(out, *p == '_' ? '-' : *p);
    if (out->len == 0) g_string_append(out, "en");
    JSValue r = JS_NewString(ctx, out->str);
    g_string_free(out, TRUE);
    return r;
}

static const char ns_ext_shim_prelude[] =
";(function(){"
"var M=globalThis.__nd_ext_manifest,B=globalThis.__nd_ext_base,"
"SR=globalThis.__nd_ext_sread,SW=globalThis.__nd_ext_swrite,"
"PL=globalThis.__nd_ext_platform,UL=globalThis.__nd_ext_uilang;"
"delete globalThis.__nd_ext_manifest;delete globalThis.__nd_ext_base;"
"delete globalThis.__nd_ext_sread;delete globalThis.__nd_ext_swrite;"
"delete globalThis.__nd_ext_platform;delete globalThis.__nd_ext_uilang;"
"function area(id,name){"
"function rd(){try{return JSON.parse(SR(id,name))||{};}catch(e){return{};}}"
"function wr(o){return SW(id,name,JSON.stringify(o));}"
"return {get:function(keys){return new Promise(function(res){var a=rd(),o={};"
"if(keys==null)o=a;"
"else if(typeof keys==='string'){if(keys in a)o[keys]=a[keys];}"
"else if(Array.isArray(keys)){keys.forEach(function(k){if(k in a)o[k]=a[k];});}"
"else if(typeof keys==='object'){Object.keys(keys).forEach(function(k){o[k]=(k in a)?a[k]:keys[k];});}"
"res(o);});},"
"set:function(items){return new Promise(function(res){var a=rd();"
"Object.keys(items||{}).forEach(function(k){a[k]=items[k];});wr(a);res();});},"
"remove:function(keys){return new Promise(function(res){var a=rd();"
"(Array.isArray(keys)?keys:[keys]).forEach(function(k){delete a[k];});wr(a);res();});},"
"clear:function(){return new Promise(function(res){wr({});res();});}};}"
"function make_api(id){"
"var man=null;"
"function getManifest(){if(man===null){try{man=JSON.parse(M(id));}catch(e){man={};}}return man;}"
"function getURL(p){var b=B(id);p=String(p==null?'':p);"
"if(p.charAt(0)==='/')p=p.slice(1);return b?('file://'+b+'/'+p):p;}"
"var listeners=[];"
"var runtime={id:id,lastError:null,getManifest:getManifest,getURL:getURL,"
"getPlatformInfo:function(){return Promise.resolve({os:PL(),arch:'x86-64'});},"
"sendMessage:function(){var msg=arguments.length>1?arguments[1]:arguments[0];"
"return new Promise(function(res){var s={id:id},rep;"
"for(var i=0;i<listeners.length;i++){try{var r=listeners[i](msg,s,function(x){rep=x;});"
"if(r&&typeof r.then==='function'){r.then(res);return;}}catch(e){}}"
"res(rep);});},"
"onMessage:{addListener:function(f){if(typeof f==='function')listeners.push(f);},"
"removeListener:function(f){var i=listeners.indexOf(f);if(i>=0)listeners.splice(i,1);},"
"hasListener:function(f){return listeners.indexOf(f)>=0;}}};"
"var i18n={getMessage:function(k){return k==null?'':String(k);},"
"getUILanguage:function(){return UL();},"
"getAcceptLanguages:function(){return Promise.resolve([UL()]);}};"
"return {runtime:runtime,i18n:i18n,extension:{getURL:getURL},"
"storage:{local:area(id,'local'),sync:area(id,'sync'),managed:area(id,'managed')}};"
"}\n";

static const char ns_ext_shim_epilogue[] = "})();\n";

static char *
ns_ext_js_string(JSContext *ctx, JSValueConst obj, const char *key)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    char *out = NULL;
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) out = g_strdup(s);
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    return out;
}

static void
ns_ext_collect_strings(JSContext *ctx, JSValueConst arr, GPtrArray *out)
{
    if (!JS_IsObject(arr)) return;
    JSValue lenv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, lenv);
    JS_FreeValue(ctx, lenv);
    for (uint32_t i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        const char *s = JS_ToCString(ctx, v);
        if (s) g_ptr_array_add(out, g_strdup(s));
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, v);
    }
}

static char *
ns_ext_safe_path(const char *base_dir, const char *rel)
{
    if (!base_dir || !rel || !*rel) return NULL;
    g_autofree char *canon_base = g_canonicalize_filename(base_dir, NULL);
    g_autofree char *joined = g_build_filename(base_dir, rel, NULL);
    char *canon = g_canonicalize_filename(joined, NULL);
    gsize blen = strlen(canon_base);
    if (strncmp(canon, canon_base, blen) != 0 ||
        (canon[blen] != '\0' && canon[blen] != G_DIR_SEPARATOR)) {
        g_free(canon);
        return NULL;
    }
    return canon;
}

static char *
ns_ext_read_files(const char *base_dir, JSContext *ctx, JSValueConst entry,
                  const char *key)
{
    JSValue jsv = JS_GetPropertyStr(ctx, entry, key);
    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    ns_ext_collect_strings(ctx, jsv, files);
    JS_FreeValue(ctx, jsv);
    GString *src = g_string_new(NULL);
    for (guint i = 0; i < files->len; i++) {
        const char *rel = g_ptr_array_index(files, i);
        g_autofree char *path = ns_ext_safe_path(base_dir, rel);
        char *data = NULL;
        if (path && g_file_get_contents(path, &data, NULL, NULL)) {
            g_string_append(src, data);
            g_string_append_c(src, '\n');
            g_free(data);
        }
    }
    g_ptr_array_free(files, TRUE);
    if (src->len == 0) { g_string_free(src, TRUE); return NULL; }
    return g_string_free(src, FALSE);
}

static void
ns_ext_cs_free(gpointer p)
{
    ns_ext_cs *cs = p;
    if (cs->matches) g_ptr_array_free(cs->matches, TRUE);
    g_free(cs->all_js);
    g_free(cs->all_css);
    g_free(cs);
}

static void
ns_ext_parse_content_scripts(ns_ext *e, JSContext *ctx, JSValueConst manifest)
{
    JSValue arr = JS_GetPropertyStr(ctx, manifest, "content_scripts");
    if (JS_IsObject(arr)) {
        JSValue lenv = JS_GetPropertyStr(ctx, arr, "length");
        uint32_t n = 0;
        JS_ToUint32(ctx, &n, lenv);
        JS_FreeValue(ctx, lenv);
        for (uint32_t i = 0; i < n; i++) {
            JSValue entry = JS_GetPropertyUint32(ctx, arr, i);
            if (JS_IsObject(entry)) {
                ns_ext_cs *cs = g_new0(ns_ext_cs, 1);
                cs->matches = g_ptr_array_new_with_free_func(g_free);
                JSValue m = JS_GetPropertyStr(ctx, entry, "matches");
                ns_ext_collect_strings(ctx, m, cs->matches);
                JS_FreeValue(ctx, m);
                g_autofree char *run_at =
                    ns_ext_js_string(ctx, entry, "run_at");
                cs->at_start = run_at && strcmp(run_at, "document_start") == 0;
                cs->all_js = ns_ext_read_files(e->base_dir, ctx, entry, "js");
                cs->all_css = ns_ext_read_files(e->base_dir, ctx, entry, "css");
                if (cs->matches->len > 0 && (cs->all_js || cs->all_css))
                    g_ptr_array_add(e->content_scripts, cs);
                else
                    ns_ext_cs_free(cs);
            }
            JS_FreeValue(ctx, entry);
        }
    }
    JS_FreeValue(ctx, arr);
}

static void
ns_dnr_rule_free(gpointer p)
{
    ns_dnr_rule *r = p;
    g_free(r->url_filter);
    if (r->regex) g_regex_unref(r->regex);
    if (r->request_domains) g_ptr_array_free(r->request_domains, TRUE);
    if (r->excluded_request_domains)
        g_ptr_array_free(r->excluded_request_domains, TRUE);
    if (r->initiator_domains) g_ptr_array_free(r->initiator_domains, TRUE);
    if (r->excluded_initiator_domains)
        g_ptr_array_free(r->excluded_initiator_domains, TRUE);
    if (r->resource_types) g_ptr_array_free(r->resource_types, TRUE);
    if (r->excluded_resource_types)
        g_ptr_array_free(r->excluded_resource_types, TRUE);
    g_free(r);
}

static GPtrArray *
ns_dnr_domains(JSContext *ctx, JSValueConst cond, const char *key)
{
    JSValue v = JS_GetPropertyStr(ctx, cond, key);
    GPtrArray *out = NULL;
    if (JS_IsObject(v)) {
        out = g_ptr_array_new_with_free_func(g_free);
        ns_ext_collect_strings(ctx, v, out);
        for (guint i = 0; i < out->len; i++) {
            char *low = g_ascii_strdown(g_ptr_array_index(out, i), -1);
            g_free(g_ptr_array_index(out, i));
            out->pdata[i] = low;
        }
        if (out->len == 0) { g_ptr_array_free(out, TRUE); out = NULL; }
    }
    JS_FreeValue(ctx, v);
    return out;
}

static ns_dnr_rule *
ns_dnr_parse_rule(JSContext *ctx, JSValueConst r)
{
    if (!JS_IsObject(r)) return NULL;
    JSValue act = JS_GetPropertyStr(ctx, r, "action");
    g_autofree char *atype =
        JS_IsObject(act) ? ns_ext_js_string(ctx, act, "type") : NULL;
    JS_FreeValue(ctx, act);
    int action;
    if (atype && strcmp(atype, "block") == 0)
        action = 0;
    else if (atype && (strcmp(atype, "allow") == 0 ||
                       strcmp(atype, "allowAllRequests") == 0))
        action = 1;
    else
        return NULL;

    ns_dnr_rule *rule = g_new0(ns_dnr_rule, 1);
    rule->action = action;
    rule->priority = 1;
    JSValue pv = JS_GetPropertyStr(ctx, r, "priority");
    if (JS_IsNumber(pv)) { int32_t pr = 1; JS_ToInt32(ctx, &pr, pv); rule->priority = pr; }
    JS_FreeValue(ctx, pv);

    JSValue cond = JS_GetPropertyStr(ctx, r, "condition");
    if (JS_IsObject(cond)) {
        rule->url_filter = ns_ext_js_string(ctx, cond, "urlFilter");
        JSValue csv = JS_GetPropertyStr(ctx, cond, "isUrlFilterCaseSensitive");
        rule->case_sensitive = JS_ToBool(ctx, csv) > 0;
        JS_FreeValue(ctx, csv);
        g_autofree char *rx = ns_ext_js_string(ctx, cond, "regexFilter");
        if (rx)
            rule->regex = g_regex_new(rx,
                rule->case_sensitive ? 0 : G_REGEX_CASELESS, 0, NULL);
        rule->request_domains = ns_dnr_domains(ctx, cond, "requestDomains");
        rule->excluded_request_domains =
            ns_dnr_domains(ctx, cond, "excludedRequestDomains");
        rule->initiator_domains = ns_dnr_domains(ctx, cond, "initiatorDomains");
        rule->excluded_initiator_domains =
            ns_dnr_domains(ctx, cond, "excludedInitiatorDomains");
        rule->resource_types = ns_dnr_domains(ctx, cond, "resourceTypes");
        rule->excluded_resource_types =
            ns_dnr_domains(ctx, cond, "excludedResourceTypes");
    }
    JS_FreeValue(ctx, cond);
    return rule;
}

static void
ns_ext_load_rule_file(ns_ext *e, JSContext *ctx, const char *path)
{
    g_autofree char *full = ns_ext_safe_path(e->base_dir, path);
    char *raw = NULL;
    gsize len = 0;
    if (!full || !g_file_get_contents(full, &raw, &len, NULL)) return;
    JSValue arr = JS_ParseJSON(ctx, raw, len, full);
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        g_free(raw);
        return;
    }
    if (JS_IsObject(arr)) {
        JSValue lenv = JS_GetPropertyStr(ctx, arr, "length");
        uint32_t n = 0;
        JS_ToUint32(ctx, &n, lenv);
        JS_FreeValue(ctx, lenv);
        for (uint32_t i = 0; i < n; i++) {
            JSValue rv = JS_GetPropertyUint32(ctx, arr, i);
            ns_dnr_rule *rule = ns_dnr_parse_rule(ctx, rv);
            if (rule) g_ptr_array_add(e->dnr_rules, rule);
            JS_FreeValue(ctx, rv);
        }
    }
    JS_FreeValue(ctx, arr);
    g_free(raw);
}

static void
ns_ext_parse_dnr(ns_ext *e, JSContext *ctx, JSValueConst manifest)
{
    JSValue dnr = JS_GetPropertyStr(ctx, manifest, "declarative_net_request");
    if (JS_IsObject(dnr)) {
        JSValue res = JS_GetPropertyStr(ctx, dnr, "rule_resources");
        if (JS_IsObject(res)) {
            JSValue lenv = JS_GetPropertyStr(ctx, res, "length");
            uint32_t n = 0;
            JS_ToUint32(ctx, &n, lenv);
            JS_FreeValue(ctx, lenv);
            for (uint32_t i = 0; i < n; i++) {
                JSValue item = JS_GetPropertyUint32(ctx, res, i);
                if (JS_IsObject(item)) {
                    gboolean enabled = TRUE;
                    JSValue ev = JS_GetPropertyStr(ctx, item, "enabled");
                    if (JS_IsBool(ev)) enabled = JS_ToBool(ctx, ev) > 0;
                    JS_FreeValue(ctx, ev);
                    g_autofree char *path = ns_ext_js_string(ctx, item, "path");
                    if (enabled && path)
                        ns_ext_load_rule_file(e, ctx, path);
                }
                JS_FreeValue(ctx, item);
            }
        }
        JS_FreeValue(ctx, res);
    }
    JS_FreeValue(ctx, dnr);
}

static void
ns_cosmetic_rule_free(gpointer p)
{
    ns_cosmetic_rule *c = p;
    if (c->domains) g_ptr_array_free(c->domains, TRUE);
    if (c->excluded_domains) g_ptr_array_free(c->excluded_domains, TRUE);
    g_free(c->selector);
    g_free(c);
}

static void
ns_abp_split_domains(const char *spec, char sep,
                     GPtrArray **inc, GPtrArray **exc)
{
    if (!spec || !*spec) return;
    gchar **parts = g_strsplit(spec, sep == ',' ? "," : "|", -1);
    for (gchar **p = parts; *p; p++) {
        char *d = g_strstrip(*p);
        if (!*d) continue;
        if (*d == '~') {
            d++;
            if (!*d) continue;
            if (!*exc) *exc = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(*exc, g_ascii_strdown(d, -1));
        } else {
            if (!*inc) *inc = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(*inc, g_ascii_strdown(d, -1));
        }
    }
    g_strfreev(parts);
}

static const char *
ns_abp_type_token(const char *t)
{
    if (strcmp(t, "script") == 0)     return "script";
    if (strcmp(t, "stylesheet") == 0) return "stylesheet";
    if (strcmp(t, "image") == 0)      return "image";
    if (strcmp(t, "font") == 0)       return "font";
    if (strcmp(t, "media") == 0)      return "media";
    return NULL;
}

static gboolean
ns_abp_option_unsupported(const char *o)
{
    static const char *skip[] = {
        "redirect", "redirect-rule", "removeparam", "csp", "replace",
        "rewrite", "removeheader", "cookie", "header", "empty", "mp4",
        "popup", "popunder", "webrtc", "inline-script", "inline-font",
        "genericblock", "generichide", "specifichide", "elemhide",
        "ehide", "document", "doc",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(skip); i++)
        if (strcmp(o, skip[i]) == 0) return TRUE;
    return FALSE;
}

static gboolean
ns_abp_is_simple_host(const char *pat)
{
    if (!g_str_has_prefix(pat, "||") || !g_str_has_suffix(pat, "^")) return FALSE;
    for (const char *p = pat + 2; *p && *(p + 1); p++)
        if (!(g_ascii_isalnum((guchar)*p) || *p == '.' || *p == '-'))
            return FALSE;
    return strlen(pat) > 3;
}

static void
ns_abp_parse_network(ns_ext *e, const char *line)
{
    gboolean exception = FALSE;
    const char *pat = line;
    if (g_str_has_prefix(pat, "@@")) { exception = TRUE; pat += 2; }

    g_autofree char *pattern = NULL;
    g_autofree char *options = NULL;
    if (*pat == '/') {
        const char *close = strrchr(pat, '/');
        if (close && close != pat) {
            pattern = g_strndup(pat, close - pat + 1);
            if (*(close + 1) == '$') options = g_strdup(close + 2);
        } else {
            pattern = g_strdup(pat);
        }
    } else {
        const char *dollar = strchr(pat, '$');
        if (dollar) {
            pattern = g_strndup(pat, dollar - pat);
            options = g_strdup(dollar + 1);
        } else {
            pattern = g_strdup(pat);
        }
    }
    if (!pattern || !*pattern) return;

    if (!exception && !options && ns_abp_is_simple_host(pattern)) {
        char *host = g_ascii_strdown(pattern + 2, strlen(pattern) - 3);
        g_hash_table_add(g_blocked_hosts, host);
        return;
    }

    ns_dnr_rule *rule = g_new0(ns_dnr_rule, 1);
    rule->action = exception ? 1 : 0;
    rule->priority = 1;

    gboolean is_regex = (*pattern == '/' && g_str_has_suffix(pattern, "/") &&
                         strlen(pattern) > 1);
    if (is_regex) {
        g_autofree char *rx = g_strndup(pattern + 1, strlen(pattern) - 2);
        rule->regex = g_regex_new(rx, G_REGEX_CASELESS, 0, NULL);
        if (!rule->regex) { ns_dnr_rule_free(rule); return; }
    } else {
        rule->url_filter = g_strdup(pattern);
    }

    if (options) {
        gchar **opts = g_strsplit(options, ",", -1);
        for (gchar **o = opts; *o; o++) {
            char *opt = g_strstrip(*o);
            gboolean neg = (*opt == '~');
            if (neg) opt++;
            if (g_str_has_prefix(opt, "domain=")) {
                ns_abp_split_domains(opt + 7, '|',
                                     &rule->initiator_domains,
                                     &rule->excluded_initiator_domains);
            } else if (strcmp(opt, "third-party") == 0) {
                rule->third_party = neg ? -1 : 1;
            } else if (strcmp(opt, "match-case") == 0) {
                rule->case_sensitive = TRUE;
            } else if (ns_abp_type_token(opt)) {
                GPtrArray **dst = neg ? &rule->excluded_resource_types
                                      : &rule->resource_types;
                if (!*dst) *dst = g_ptr_array_new_with_free_func(g_free);
                g_ptr_array_add(*dst, g_strdup(ns_abp_type_token(opt)));
            } else if (ns_abp_option_unsupported(opt)) {
                g_strfreev(opts);
                ns_dnr_rule_free(rule);
                return;
            }
        }
        g_strfreev(opts);
    }
    g_ptr_array_add(e->dnr_rules, rule);
}

static void
ns_abp_parse_cosmetic(ns_ext *e, const char *line, const char *sep)
{
    const char *at = strstr(line, sep);
    if (!at) return;
    g_autofree char *dom_spec = g_strndup(line, at - line);
    const char *selector = at + strlen(sep);
    while (*selector == ' ') selector++;
    if (!*selector || strchr(selector, '{')) return;
    if (g_str_has_prefix(selector, "+js") || g_str_has_prefix(selector, "script:"))
        return;

    ns_cosmetic_rule *c = g_new0(ns_cosmetic_rule, 1);
    ns_abp_split_domains(dom_spec, ',', &c->domains, &c->excluded_domains);
    c->selector = g_strstrip(g_strdup(selector));
    g_ptr_array_add(e->cosmetic_rules, c);
}

static void
ns_ext_load_filter_list(ns_ext *e, const char *path)
{
    g_autofree char *full = ns_ext_safe_path(e->base_dir, path);
    char *raw = NULL;
    if (!full || !g_file_get_contents(full, &raw, NULL, NULL)) return;
    gchar **lines = g_strsplit(raw, "\n", -1);
    for (gchar **lp = lines; *lp; lp++) {
        char *line = g_strstrip(*lp);
        if (!*line || *line == '!' || *line == '[') continue;
        if (strstr(line, "#@#") || strstr(line, "#?#") ||
            strstr(line, "#$#") || strstr(line, "#%#"))
            continue;
        if (strstr(line, "##"))
            ns_abp_parse_cosmetic(e, line, "##");
        else
            ns_abp_parse_network(e, line);
    }
    g_strfreev(lines);
    g_free(raw);
}

static void
ns_ext_parse_filter_lists(ns_ext *e, JSContext *ctx, JSValueConst manifest)
{
    JSValue arr = JS_GetPropertyStr(ctx, manifest, "nordstjernen_filter_lists");
    if (JS_IsObject(arr)) {
        GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
        ns_ext_collect_strings(ctx, arr, paths);
        for (guint i = 0; i < paths->len; i++)
            ns_ext_load_filter_list(e, g_ptr_array_index(paths, i));
        g_ptr_array_free(paths, TRUE);
    }
    JS_FreeValue(ctx, arr);
}

static char *
ns_ext_parse_id(JSContext *ctx, JSValueConst manifest, const char *dir)
{
    const char *keys[] = { "browser_specific_settings", "applications" };
    for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
        JSValue bss = JS_GetPropertyStr(ctx, manifest, keys[i]);
        if (JS_IsObject(bss)) {
            JSValue gecko = JS_GetPropertyStr(ctx, bss, "gecko");
            char *id = NULL;
            if (JS_IsObject(gecko))
                id = ns_ext_js_string(ctx, gecko, "id");
            JS_FreeValue(ctx, gecko);
            JS_FreeValue(ctx, bss);
            if (id) return id;
        } else {
            JS_FreeValue(ctx, bss);
        }
    }
    return g_path_get_basename(dir);
}

static void
ns_ext_load_one(const char *dir, JSContext *ctx)
{
    g_autofree char *mpath = g_build_filename(dir, "manifest.json", NULL);
    char *raw = NULL;
    gsize len = 0;
    if (!g_file_get_contents(mpath, &raw, &len, NULL)) return;
    JSValue obj = JS_ParseJSON(ctx, raw, len, mpath);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        g_free(raw);
        return;
    }
    ns_ext *e = g_new0(ns_ext, 1);
    e->base_dir = g_strdup(dir);
    e->manifest_json = raw;
    e->name = ns_ext_js_string(ctx, obj, "name");
    e->version = ns_ext_js_string(ctx, obj, "version");
    e->id = ns_ext_parse_id(ctx, obj, dir);
    e->content_scripts = g_ptr_array_new_with_free_func(ns_ext_cs_free);
    e->dnr_rules = g_ptr_array_new_with_free_func(ns_dnr_rule_free);
    e->cosmetic_rules = g_ptr_array_new_with_free_func(ns_cosmetic_rule_free);
    ns_ext_parse_content_scripts(e, ctx, obj);
    ns_ext_parse_dnr(e, ctx, obj);
    ns_ext_parse_filter_lists(e, ctx, obj);
    JS_FreeValue(ctx, obj);
    g_ptr_array_add(g_exts, e);
}

static void
ns_ext_scan_root(const char *root, JSContext *ctx)
{
    if (!root || !*root) return;
    g_autofree char *self = g_build_filename(root, "manifest.json", NULL);
    if (g_file_test(self, G_FILE_TEST_EXISTS)) {
        ns_ext_load_one(root, ctx);
        return;
    }
    GDir *d = g_dir_open(root, 0, NULL);
    if (!d) return;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        g_autofree char *child = g_build_filename(root, name, NULL);
        if (g_file_test(child, G_FILE_TEST_IS_DIR))
            ns_ext_load_one(child, ctx);
    }
    g_dir_close(d);
}

static void
ns_ext_do_init(void)
{
    g_exts = g_ptr_array_new();
    g_blocked_hosts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, NULL);

    JSRuntime *rt = JS_NewRuntime();
    if (!rt) return;
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); return; }

    const char *envd = g_getenv("NS_EXTENSIONS_DIR");
    if (envd && *envd) {
        gchar **parts = g_strsplit(envd, G_SEARCHPATH_SEPARATOR_S, -1);
        for (gchar **p = parts; *p; p++)
            if (**p) ns_ext_scan_root(*p, ctx);
        g_strfreev(parts);
    }
    g_autofree char *def = g_build_filename(g_get_user_data_dir(),
                                            NS_APP_DIR_NAME,
                                            "extensions", NULL);
    ns_ext_scan_root(def, ctx);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static void
ns_ext_init(void)
{
    static gsize once;
    if (g_once_init_enter(&once)) {
        ns_ext_do_init();
        g_once_init_leave(&once, 1);
    }
}

static void
ns_ext_append_id(GString *out, const char *id)
{
    for (const char *p = id; *p; p++) {
        if (*p == '\\' || *p == '"') g_string_append_c(out, '\\');
        g_string_append_c(out, *p);
    }
}

static void
ns_ext_append_js_string(GString *out, const char *s)
{
    g_string_append_c(out, '"');
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '\\': g_string_append(out, "\\\\"); break;
        case '"':  g_string_append(out, "\\\""); break;
        case '\n': g_string_append(out, "\\n"); break;
        case '\r': g_string_append(out, "\\r"); break;
        case '\t': g_string_append(out, "\\t"); break;
        default:   g_string_append_c(out, *p);
        }
    }
    g_string_append_c(out, '"');
}

static char *ns_ext_cosmetic_css_for_host(const char *host);
static char *ns_ext_strip_port(const char *host);

char *
ns_ext_content_scripts_for_url(JSContext *ctx, JSValueConst global,
                               const char *url, gboolean at_start)
{
    ns_ext_init();
    if (!url || !*url || g_exts->len == 0) return NULL;
    GString *body = g_string_new(NULL);
    if (at_start) {
        g_autofree char *s = NULL, *h = NULL, *pth = NULL;
        ns_ext_url_split(url, &s, &h, &pth);
        g_autofree char *h_np = ns_ext_strip_port(h);
        g_autofree char *cosmetic = ns_ext_cosmetic_css_for_host(h_np);
        if (cosmetic) {
            g_string_append(body,
                ";(function(){try{var __ndch=document.createElement('style');"
                "__ndch.textContent=");
            ns_ext_append_js_string(body, cosmetic);
            g_string_append(body,
                ";(document.head||document.documentElement||document)"
                ".appendChild(__ndch);}catch(e){}})();\n");
        }
    }
    for (guint i = 0; i < g_exts->len; i++) {
        ns_ext *e = g_ptr_array_index(g_exts, i);
        for (guint j = 0; j < e->content_scripts->len; j++) {
            ns_ext_cs *cs = g_ptr_array_index(e->content_scripts, j);
            if (cs->at_start != at_start) continue;
            gboolean hit = FALSE;
            for (guint k = 0; k < cs->matches->len && !hit; k++)
                hit = ns_ext_pattern_match(g_ptr_array_index(cs->matches, k),
                                           url);
            if (!hit) continue;
            g_string_append(body,
                ";(function(browser){var chrome=browser,"
                "make_api,area,SR,SW,M,B,PL,UL;try{\n");
            if (cs->all_css) {
                g_string_append(body,
                    "var __ndcss=document.createElement('style');"
                    "__ndcss.textContent=");
                ns_ext_append_js_string(body, cs->all_css);
                g_string_append(body,
                    ";(document.head||document.documentElement||document)"
                    ".appendChild(__ndcss);\n");
            }
            if (cs->all_js) {
                g_string_append(body, cs->all_js);
                g_string_append(body, "\n");
            }
            g_string_append(body,
                "}catch(e){try{console.error(\"[nordstjernen ext]\",e);}"
                "catch(_){}}})(");
            if (cs->all_js) {
                g_string_append(body, "make_api(\"");
                ns_ext_append_id(body, e->id);
                g_string_append(body, "\")");
            } else {
                g_string_append(body, "null");
            }
            g_string_append(body, ");\n");
        }
    }
    if (body->len == 0) { g_string_free(body, TRUE); return NULL; }

    JS_DefinePropertyValueStr(ctx, global, "__nd_ext_manifest",
        JS_NewCFunction(ctx, ns_ext_js_manifest, "m", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global, "__nd_ext_base",
        JS_NewCFunction(ctx, ns_ext_js_base, "b", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global, "__nd_ext_sread",
        JS_NewCFunction(ctx, ns_ext_js_sread, "r", 2), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global, "__nd_ext_swrite",
        JS_NewCFunction(ctx, ns_ext_js_swrite, "w", 3), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global, "__nd_ext_platform",
        JS_NewCFunction(ctx, ns_ext_js_platform, "p", 0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global, "__nd_ext_uilang",
        JS_NewCFunction(ctx, ns_ext_js_uilang, "l", 0), JS_PROP_C_W_E);

    GString *out = g_string_new(ns_ext_shim_prelude);
    g_string_append(out, body->str);
    g_string_append(out, ns_ext_shim_epilogue);
    g_string_free(body, TRUE);
    return g_string_free(out, FALSE);
}

static gboolean
ns_dnr_sep(char c)
{
    return !(g_ascii_isalnum((guchar)c) ||
             c == '_' || c == '-' || c == '.' || c == '%');
}

static gboolean
ns_dnr_match_here(const char *p, const char *s, gboolean end_anchor)
{
    while (*p) {
        if (*p == '*') {
            p++;
            if (!*p) return TRUE;
            for (;;) {
                if (ns_dnr_match_here(p, s, end_anchor)) return TRUE;
                if (!*s) return FALSE;
                s++;
            }
        }
        if (*p == '^') {
            if (*s == '\0') { p++; continue; }
            if (ns_dnr_sep(*s)) { p++; s++; continue; }
            return FALSE;
        }
        if (*s != *p) return FALSE;
        p++;
        s++;
    }
    return end_anchor ? (*s == '\0') : TRUE;
}

static gboolean
ns_dnr_urlfilter_match(const char *filter, const char *url, gboolean cs)
{
    if (!filter || !*filter) return TRUE;
    g_autofree char *u = cs ? g_strdup(url) : g_ascii_strdown(url, -1);
    g_autofree char *f = cs ? g_strdup(filter) : g_ascii_strdown(filter, -1);
    const char *p = f;
    gboolean domain_anchor = FALSE, start_anchor = FALSE, end_anchor = FALSE;
    if (p[0] == '|' && p[1] == '|') { domain_anchor = TRUE; p += 2; }
    else if (p[0] == '|')           { start_anchor = TRUE; p += 1; }
    g_autofree char *body = g_strdup(p);
    gsize bl = strlen(body);
    if (bl > 0 && body[bl - 1] == '|') { end_anchor = TRUE; body[bl - 1] = '\0'; }

    if (domain_anchor) {
        const char *h = strstr(u, "://");
        const char *hs = h ? h + 3 : u;
        const char *he = hs;
        while (*he && *he != '/' && *he != '?' && *he != '#') he++;
        for (const char *pos = hs; pos <= he; pos++)
            if (pos == hs || pos[-1] == '.')
                if (ns_dnr_match_here(body, pos, end_anchor)) return TRUE;
        return FALSE;
    }
    if (start_anchor)
        return ns_dnr_match_here(body, u, end_anchor);
    for (const char *pos = u; ; pos++) {
        if (ns_dnr_match_here(body, pos, end_anchor)) return TRUE;
        if (!*pos) return FALSE;
    }
}

static gboolean
ns_dnr_domain_match(const char *domain, const char *host)
{
    if (!domain || !host) return FALSE;
    if (strcmp(domain, host) == 0) return TRUE;
    gsize dl = strlen(domain), hl = strlen(host);
    return hl > dl && host[hl - dl - 1] == '.' &&
           strcmp(host + hl - dl, domain) == 0;
}

static gboolean
ns_dnr_domain_list_match(GPtrArray *list, const char *host)
{
    if (!host) return FALSE;
    for (guint i = 0; i < list->len; i++)
        if (ns_dnr_domain_match(g_ptr_array_index(list, i), host)) return TRUE;
    return FALSE;
}

static char *
ns_ext_cosmetic_css_for_host(const char *host)
{
    if (!host || !*host || !g_exts) return NULL;
    GString *css = g_string_new(NULL);
    for (guint i = 0; i < g_exts->len; i++) {
        ns_ext *e = g_ptr_array_index(g_exts, i);
        if (!e->cosmetic_rules) continue;
        for (guint j = 0; j < e->cosmetic_rules->len; j++) {
            ns_cosmetic_rule *c = g_ptr_array_index(e->cosmetic_rules, j);
            if (c->excluded_domains &&
                ns_dnr_domain_list_match(c->excluded_domains, host))
                continue;
            if (c->domains && !ns_dnr_domain_list_match(c->domains, host))
                continue;
            g_string_append(css, c->selector);
            g_string_append(css, "{display:none !important}\n");
        }
    }
    if (css->len == 0) { g_string_free(css, TRUE); return NULL; }
    return g_string_free(css, FALSE);
}

static const char *
ns_dnr_infer_type(const char *url)
{
    const char *end = strpbrk(url, "?#");
    const char *path_end = end ? end : url + strlen(url);
    const char *dot = NULL;
    for (const char *p = url; p < path_end; p++) {
        if (*p == '.') dot = p;
        else if (*p == '/') dot = NULL;
    }
    if (!dot) return NULL;
    g_autofree char *ext = g_ascii_strdown(dot + 1, path_end - dot - 1);
    static const struct { const char *ext; const char *type; } map[] = {
        { "js", "script" }, { "mjs", "script" },
        { "css", "stylesheet" },
        { "png", "image" }, { "jpg", "image" }, { "jpeg", "image" },
        { "gif", "image" }, { "webp", "image" }, { "svg", "image" },
        { "ico", "image" }, { "bmp", "image" }, { "avif", "image" },
        { "apng", "image" },
        { "woff", "font" }, { "woff2", "font" }, { "ttf", "font" },
        { "otf", "font" }, { "eot", "font" },
        { "mp4", "media" }, { "webm", "media" }, { "mp3", "media" },
        { "ogg", "media" }, { "oga", "media" }, { "ogv", "media" },
        { "wav", "media" }, { "m4a", "media" }, { "m4v", "media" },
        { "mpg", "media" }, { "mpeg", "media" }, { "mov", "media" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(map); i++)
        if (strcmp(ext, map[i].ext) == 0) return map[i].type;
    return NULL;
}

static gboolean
ns_dnr_type_in_list(GPtrArray *list, const char *type)
{
    if (!type) return FALSE;
    for (guint i = 0; i < list->len; i++)
        if (strcmp(g_ptr_array_index(list, i), type) == 0) return TRUE;
    return FALSE;
}

static char *
ns_ext_strip_port(const char *host)
{
    if (!host) return NULL;
    const char *c = strchr(host, ':');
    return c ? g_strndup(host, c - host) : g_strdup(host);
}

static int
ns_ext_third_party(const char *req_host, const char *init_host)
{
    if (!req_host || !init_host) return -1;
    g_autofree char *rh = ns_ext_strip_port(req_host);
    g_autofree char *ih = ns_ext_strip_port(init_host);
    if (!*rh || !*ih) return -1;
    if (strcmp(rh, ih) == 0) return 0;
    const psl_ctx_t *psl = psl_builtin();
    if (!psl) return strcmp(rh, ih) == 0 ? 0 : 1;
    const char *rr = psl_registrable_domain(psl, rh);
    const char *ir = psl_registrable_domain(psl, ih);
    if (!rr || !ir) return strcmp(rh, ih) == 0 ? 0 : 1;
    return strcmp(rr, ir) == 0 ? 0 : 1;
}

static gboolean
ns_ext_host_indexed(const char *host)
{
    if (!host || g_hash_table_size(g_blocked_hosts) == 0) return FALSE;
    g_autofree char *h = ns_ext_strip_port(host);
    for (const char *p = h; p && *p; ) {
        if (g_hash_table_contains(g_blocked_hosts, p)) return TRUE;
        const char *dot = strchr(p, '.');
        p = dot ? dot + 1 : NULL;
    }
    return FALSE;
}

static gboolean
ns_dnr_rule_matches(const ns_dnr_rule *r, const char *url,
                    const char *host, const char *init_host, int tp)
{
    if (r->third_party == 1 && tp != 1) return FALSE;
    if (r->third_party == -1 && tp != 0) return FALSE;
    if (r->request_domains && !ns_dnr_domain_list_match(r->request_domains, host))
        return FALSE;
    if (r->excluded_request_domains &&
        ns_dnr_domain_list_match(r->excluded_request_domains, host))
        return FALSE;
    if (r->initiator_domains &&
        !ns_dnr_domain_list_match(r->initiator_domains, init_host))
        return FALSE;
    if (r->excluded_initiator_domains && init_host &&
        ns_dnr_domain_list_match(r->excluded_initiator_domains, init_host))
        return FALSE;
    if (r->url_filter &&
        !ns_dnr_urlfilter_match(r->url_filter, url, r->case_sensitive))
        return FALSE;
    if (r->regex && !g_regex_match(r->regex, url, 0, NULL))
        return FALSE;
    if (r->resource_types || r->excluded_resource_types) {
        const char *t = ns_dnr_infer_type(url);
        if (r->resource_types && !ns_dnr_type_in_list(r->resource_types, t))
            return FALSE;
        if (r->excluded_resource_types &&
            ns_dnr_type_in_list(r->excluded_resource_types, t))
            return FALSE;
    }
    return TRUE;
}

gboolean
ns_ext_should_block(const char *url, const char *initiator)
{
    ns_ext_init();
    if (!url || !g_exts || g_exts->len == 0) return FALSE;
    if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://"))
        return FALSE;

    g_autofree char *us = NULL, *uh = NULL, *up = NULL;
    ns_ext_url_split(url, &us, &uh, &up);
    g_autofree char *is = NULL, *ih = NULL, *ip = NULL;
    if (initiator) ns_ext_url_split(initiator, &is, &ih, &ip);
    int tp = ns_ext_third_party(uh, ih);
    g_autofree char *uh_np = ns_ext_strip_port(uh);
    g_autofree char *ih_np = ih ? ns_ext_strip_port(ih) : NULL;

    int best_pri = -1;
    gboolean best_allow = FALSE, best_block = FALSE;
    if (ns_ext_host_indexed(uh)) { best_pri = 1; best_block = TRUE; }

    for (guint i = 0; i < g_exts->len; i++) {
        ns_ext *e = g_ptr_array_index(g_exts, i);
        if (!e->dnr_rules) continue;
        for (guint j = 0; j < e->dnr_rules->len; j++) {
            const ns_dnr_rule *r = g_ptr_array_index(e->dnr_rules, j);
            if (!ns_dnr_rule_matches(r, url, uh_np, ih_np, tp)) continue;
            if (r->priority > best_pri) {
                best_pri = r->priority;
                best_allow = (r->action == 1);
                best_block = (r->action == 0);
            } else if (r->priority == best_pri) {
                if (r->action == 1) best_allow = TRUE;
                else best_block = TRUE;
            }
        }
    }
    return best_pri >= 0 && best_block && !best_allow;
}
