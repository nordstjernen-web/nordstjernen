/* Nordstjernen — native ECMA-402 (Intl) implementation over QuickJS, ICU-free. */

#include "js_intl.h"

#include <math.h>
#include <string.h>
#include <time.h>
#include <glib.h>
#include <pango/pango.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "i18n.h"

static gboolean
intl_gmtime(const time_t *t, struct tm *out)
{
#ifdef _WIN32
    return gmtime_s(out, t) == 0;
#else
    return gmtime_r(t, out) != NULL;
#endif
}

static gboolean
intl_localtime(const time_t *t, struct tm *out)
{
#ifdef _WIN32
    return localtime_s(out, t) == 0;
#else
    return localtime_r(t, out) != NULL;
#endif
}

static void
intl_bind_bound(JSContext *ctx, JSValueConst obj, const char *name,
                JSCFunctionData *fn, int argc, JSValueConst instance);
static JSValue intl_collator_compare_b(JSContext *, JSValueConst, int,
                                       JSValueConst *, int, JSValueConst *);
static JSValue intl_nf_format_b(JSContext *, JSValueConst, int,
                                JSValueConst *, int, JSValueConst *);
static JSValue intl_dtf_format_b(JSContext *, JSValueConst, int,
                                 JSValueConst *, int, JSValueConst *);

static JSValue
intl_str(JSContext *ctx, const char *s)
{
    return JS_NewString(ctx, s ? s : "");
}

static char *
intl_norm_locale(const char *s)
{
    if (!s || !*s) return NULL;
    char *out = g_strdup(s);
    for (char *p = out; *p; p++)
        if (*p == '_') *p = '-';
    return out;
}

static char *
intl_default_locale(void)
{
    char *loc = intl_norm_locale(ns_i18n_language());
    if (loc) return loc;
    const gchar *const *names = g_get_language_names();
    for (int i = 0; names && names[i]; i++) {
        if (!strcmp(names[i], "C") || !strcmp(names[i], "POSIX")) continue;
        if (strchr(names[i], '.') || strchr(names[i], '@')) continue;
        char *n = intl_norm_locale(names[i]);
        if (n) return n;
    }
    return g_strdup("en-US");
}

static char *
intl_arg_locale(JSContext *ctx, JSValueConst v)
{
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        char *r = intl_norm_locale(s);
        if (s) JS_FreeCString(ctx, s);
        return r ? r : intl_default_locale();
    }
    if (JS_IsArray(v)) {
        JSValue first = JS_GetPropertyUint32(ctx, v, 0);
        char *r = JS_IsString(first) ? intl_arg_locale(ctx, first) : NULL;
        JS_FreeValue(ctx, first);
        if (r) return r;
    }
    return intl_default_locale();
}

static void
intl_lang_subtag(const char *locale, char *out, size_t n)
{
    size_t i = 0;
    for (; locale && locale[i] && locale[i] != '-' && i + 1 < n; i++)
        out[i] = (char)g_ascii_tolower(locale[i]);
    out[i] = '\0';
}

static gboolean
intl_lang_in(const char *locale, const char *const *langs)
{
    char lang[16];
    intl_lang_subtag(locale, lang, sizeof lang);
    for (int i = 0; langs[i]; i++)
        if (!strcmp(lang, langs[i])) return TRUE;
    return FALSE;
}

static void
intl_hide(JSContext *ctx, JSValueConst obj, const char *key, JSValue val)
{
    JS_DefinePropertyValueStr(ctx, obj, key, val, 0);
}

static char *
intl_hget_str(JSContext *ctx, JSValueConst obj, const char *key)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    char *r = NULL;
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) { r = g_strdup(s); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    return r;
}

static int
intl_hget_int(JSContext *ctx, JSValueConst obj, const char *key, int dflt)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    int r = dflt;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        int32_t t; if (JS_ToInt32(ctx, &t, v) == 0) r = t;
    }
    JS_FreeValue(ctx, v);
    return r;
}

static char *
intl_opt_str(JSContext *ctx, JSValueConst o, const char *key)
{
    if (!JS_IsObject(o)) return NULL;
    JSValue v = JS_GetPropertyStr(ctx, o, key);
    char *r = NULL;
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) { r = g_strdup(s); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    return r;
}

static gboolean
intl_opt_present(JSContext *ctx, JSValueConst o, const char *key)
{
    if (!JS_IsObject(o)) return FALSE;
    JSValue v = JS_GetPropertyStr(ctx, o, key);
    gboolean p = !JS_IsUndefined(v) && !JS_IsNull(v);
    JS_FreeValue(ctx, v);
    return p;
}

static gboolean
intl_opt_num(JSContext *ctx, JSValueConst o, const char *key, double *out)
{
    if (!JS_IsObject(o)) return FALSE;
    JSValue v = JS_GetPropertyStr(ctx, o, key);
    gboolean ok = FALSE;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v) == 0) { *out = d; ok = TRUE; }
    }
    JS_FreeValue(ctx, v);
    return ok;
}

static int
intl_opt_bool(JSContext *ctx, JSValueConst o, const char *key, int dflt)
{
    if (!JS_IsObject(o)) return dflt;
    JSValue v = JS_GetPropertyStr(ctx, o, key);
    int r = dflt;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) r = JS_ToBool(ctx, v) ? 1 : 0;
    JS_FreeValue(ctx, v);
    return r;
}

static JSValue
intl_instance_proto(JSContext *ctx, JSValueConst this_val, const char *service)
{
    JSValue p = JS_GetPropertyStr(ctx, this_val, "prototype");
    if (JS_IsObject(p)) return p;
    JS_FreeValue(ctx, p);
    JSValue ctor = JS_GetPropertyStr(ctx, this_val, service);
    p = JS_GetPropertyStr(ctx, ctor, "prototype");
    JS_FreeValue(ctx, ctor);
    if (JS_IsObject(p)) return p;
    JS_FreeValue(ctx, p);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue intl = JS_GetPropertyStr(ctx, global, "Intl");
    JS_FreeValue(ctx, global);
    ctor = JS_GetPropertyStr(ctx, intl, service);
    JS_FreeValue(ctx, intl);
    p = JS_GetPropertyStr(ctx, ctor, "prototype");
    JS_FreeValue(ctx, ctor);
    if (JS_IsObject(p)) return p;
    JS_FreeValue(ctx, p);
    return JS_NULL;
}

static JSValue
intl_new(JSContext *ctx, JSValueConst this_val, const char *service)
{
    JSValue proto = intl_instance_proto(ctx, this_val, service);
    JSValue obj = JS_IsObject(proto) ? JS_NewObjectProto(ctx, proto)
                                     : JS_NewObject(ctx);
    JS_FreeValue(ctx, proto);
    return obj;
}

static JSValue
intl_part(JSContext *ctx, const char *type, const char *value)
{
    JSValue p = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, p, "type", intl_str(ctx, type));
    JS_SetPropertyStr(ctx, p, "value", intl_str(ctx, value));
    return p;
}

static JSValue
intl_supportedLocalesOf(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue out = JS_NewArray(ctx);
    if (argc < 1) return out;
    uint32_t n = 0;
    if (JS_IsString(argv[0])) {
        JS_SetPropertyUint32(ctx, out, n++, JS_DupValue(ctx, argv[0]));
    } else if (JS_IsArray(argv[0])) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        uint32_t len = 0; JS_ToUint32(ctx, &len, lv); JS_FreeValue(ctx, lv);
        for (uint32_t i = 0; i < len; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, argv[0], i);
            if (JS_IsString(e)) JS_SetPropertyUint32(ctx, out, n++, e);
            else JS_FreeValue(ctx, e);
        }
    }
    return out;
}

static void
intl_bind(JSContext *ctx, JSValueConst obj, const char *name,
          JSCFunction *fn, int argc)
{
    JS_SetPropertyStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, argc));
}

typedef struct { const char *name; JSCFunction *fn; int argc; } intl_method;

static JSValue
intl_register(JSContext *ctx, JSValueConst intl, const char *name,
              JSCFunction *ctor, int ctor_argc,
              const intl_method *methods, int n_methods)
{
    JSValue func = JS_NewCFunction2(ctx, ctor, name, ctor_argc,
                                    JS_CFUNC_constructor_or_func, 0);
    JSValue proto = JS_NewObject(ctx);
    for (int i = 0; i < n_methods; i++)
        intl_bind(ctx, proto, methods[i].name, methods[i].fn, methods[i].argc);
    JS_SetConstructor(ctx, func, proto);
    JS_FreeValue(ctx, proto);
    intl_bind(ctx, func, "supportedLocalesOf", intl_supportedLocalesOf, 1);
    JS_SetPropertyStr(ctx, intl, name, JS_DupValue(ctx, func));
    return func;
}

/* ---- locale canonicalization ------------------------------------------- */

static char *
intl_canonicalize(const char *tag)
{
    if (!tag || !*tag) return g_strdup("");
    char **parts = g_strsplit(tag, "-", -1);
    GString *out = g_string_new(NULL);
    for (int i = 0; parts[i]; i++) {
        char *p = parts[i];
        size_t len = strlen(p);
        if (out->len) g_string_append_c(out, '-');
        if (i == 0) {
            for (size_t j = 0; j < len; j++)
                g_string_append_c(out, g_ascii_tolower(p[j]));
        } else if (len == 4 && g_ascii_isalpha(p[0])) {
            g_string_append_c(out, g_ascii_toupper(p[0]));
            for (size_t j = 1; j < len; j++)
                g_string_append_c(out, g_ascii_tolower(p[j]));
        } else if (len == 2 && g_ascii_isalpha(p[0])) {
            for (size_t j = 0; j < len; j++)
                g_string_append_c(out, g_ascii_toupper(p[j]));
        } else {
            for (size_t j = 0; j < len; j++)
                g_string_append_c(out, g_ascii_tolower(p[j]));
        }
    }
    g_strfreev(parts);
    return g_string_free(out, FALSE);
}

static JSValue
intl_getCanonicalLocales(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue out = JS_NewArray(ctx);
    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) return out;
    uint32_t n = 0;
    GPtrArray *seen = g_ptr_array_new_with_free_func(g_free);
    JSValueConst src = argv[0];
    gboolean is_arr = JS_IsArray(src);
    uint32_t len = 1;
    if (is_arr) {
        JSValue lv = JS_GetPropertyStr(ctx, src, "length");
        JS_ToUint32(ctx, &len, lv); JS_FreeValue(ctx, lv);
    }
    for (uint32_t i = 0; i < len; i++) {
        JSValue e = is_arr ? JS_GetPropertyUint32(ctx, src, i)
                           : JS_DupValue(ctx, src);
        const char *s = JS_ToCString(ctx, e);
        if (s) {
            char *c = intl_canonicalize(s);
            gboolean dup = FALSE;
            for (guint k = 0; k < seen->len; k++)
                if (!strcmp(g_ptr_array_index(seen, k), c)) { dup = TRUE; break; }
            if (!dup) {
                JS_SetPropertyUint32(ctx, out, n++, intl_str(ctx, c));
                g_ptr_array_add(seen, c);
            } else {
                g_free(c);
            }
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, e);
    }
    g_ptr_array_free(seen, TRUE);
    return out;
}

static JSValue
intl_make_array(JSContext *ctx, const char *const *items)
{
    JSValue a = JS_NewArray(ctx);
    for (uint32_t i = 0; items[i]; i++)
        JS_SetPropertyUint32(ctx, a, i, intl_str(ctx, items[i]));
    return a;
}

static JSValue
intl_supportedValuesOf(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    (void)this_val;
    const char *key = argc >= 1 ? JS_ToCString(ctx, argv[0]) : NULL;
    JSValue out;
    static const char *const calendars[] = { "gregory", "iso8601", NULL };
    static const char *const collations[] = { "default", "emoji", "eor", NULL };
    static const char *const numbering[] = { "latn", NULL };
    static const char *const zones[] = { "UTC", NULL };
    static const char *const currencies[] = {
        "AUD","BRL","CAD","CHF","CNY","EUR","GBP","HKD","INR","JPY","KRW",
        "MXN","NOK","NZD","RUB","SEK","USD","ZAR", NULL };
    static const char *const units[] = {
        "acre","bit","byte","celsius","centimeter","day","degree","fahrenheit",
        "gigabyte","gram","hectare","hour","inch","kilogram","kilometer",
        "liter","megabyte","meter","mile","milliliter","millimeter",
        "millisecond","minute","month","ounce","percent","pound","second",
        "week","year", NULL };
    if (key && !strcmp(key, "calendar")) out = intl_make_array(ctx, calendars);
    else if (key && !strcmp(key, "collation")) out = intl_make_array(ctx, collations);
    else if (key && !strcmp(key, "currency")) out = intl_make_array(ctx, currencies);
    else if (key && !strcmp(key, "numberingSystem")) out = intl_make_array(ctx, numbering);
    else if (key && !strcmp(key, "timeZone")) out = intl_make_array(ctx, zones);
    else if (key && !strcmp(key, "unit")) out = intl_make_array(ctx, units);
    else out = JS_NewArray(ctx);
    if (key) JS_FreeCString(ctx, key);
    return out;
}

/* ---- Intl.Locale -------------------------------------------------------- */

static char *
intl_subtag_after(const char *tag, char kind)
{
    char want[3] = { 0 };
    if (kind == 'c') { want[0] = 'c'; want[1] = 'a'; }
    else if (kind == 'o') { want[0] = 'c'; want[1] = 'o'; }
    else if (kind == 'n') { want[0] = 'n'; want[1] = 'u'; }
    else if (kind == 'h') { want[0] = 'h'; want[1] = 'c'; }
    char **parts = g_strsplit(tag, "-", -1);
    char *out = NULL;
    for (int i = 0; parts[i] && !out; i++) {
        if (!g_ascii_strcasecmp(parts[i], "u")) {
            for (int j = i + 1; parts[j]; j++) {
                if (!g_ascii_strcasecmp(parts[j], want) && parts[j + 1]) {
                    out = g_ascii_strdown(parts[j + 1], -1);
                    break;
                }
            }
        }
    }
    g_strfreev(parts);
    return out;
}

static JSValue
intl_locale_ctor(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    const char *tag = (argc >= 1 && JS_IsString(argv[0]))
        ? JS_ToCString(ctx, argv[0]) : NULL;
    char *canon = intl_canonicalize(tag ? tag : "und");
    if (tag) JS_FreeCString(ctx, tag);

    JSValueConst options = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    char **parts = g_strsplit(canon, "-", -1);
    char *language = parts[0] ? g_ascii_strdown(parts[0], -1) : g_strdup("und");
    char *script = NULL, *region = NULL;
    for (int i = 1; parts[i]; i++) {
        size_t len = strlen(parts[i]);
        if (!script && len == 4 && g_ascii_isalpha(parts[i][0]))
            script = g_strdup(parts[i]);
        else if (!region && (len == 2 || len == 3))
            region = g_ascii_strup(parts[i], -1);
    }
    g_strfreev(parts);

    char *calendar = intl_opt_str(ctx, options, "calendar");
    if (!calendar) calendar = intl_subtag_after(canon, 'c');
    char *collation = intl_opt_str(ctx, options, "collation");
    if (!collation) collation = intl_subtag_after(canon, 'o');
    char *numbering = intl_opt_str(ctx, options, "numberingSystem");
    if (!numbering) numbering = intl_subtag_after(canon, 'n');
    char *hourCycle = intl_opt_str(ctx, options, "hourCycle");
    if (!hourCycle) hourCycle = intl_subtag_after(canon, 'h');

    JSValue obj = intl_new(ctx, this_val, "Locale");
    {
        GString *base = g_string_new(language);
        if (script) { g_string_append_c(base, '-'); g_string_append(base, script); }
        if (region) { g_string_append_c(base, '-'); g_string_append(base, region); }
        JS_SetPropertyStr(ctx, obj, "baseName",
                          intl_str(ctx, base->str));
        g_string_free(base, TRUE);
    }
    JS_SetPropertyStr(ctx, obj, "language", intl_str(ctx, language));
    if (script) JS_SetPropertyStr(ctx, obj, "script", intl_str(ctx, script));
    else JS_SetPropertyStr(ctx, obj, "script", JS_UNDEFINED);
    if (region) JS_SetPropertyStr(ctx, obj, "region", intl_str(ctx, region));
    else JS_SetPropertyStr(ctx, obj, "region", JS_UNDEFINED);
    JS_SetPropertyStr(ctx, obj, "calendar",
                      calendar ? intl_str(ctx, calendar) : JS_UNDEFINED);
    JS_SetPropertyStr(ctx, obj, "collation",
                      collation ? intl_str(ctx, collation) : JS_UNDEFINED);
    JS_SetPropertyStr(ctx, obj, "numberingSystem",
                      numbering ? intl_str(ctx, numbering) : JS_UNDEFINED);
    JS_SetPropertyStr(ctx, obj, "hourCycle",
                      hourCycle ? intl_str(ctx, hourCycle) : JS_UNDEFINED);
    intl_hide(ctx, obj, "_tag", intl_str(ctx, canon));

    g_free(canon); g_free(language); g_free(script); g_free(region);
    g_free(calendar); g_free(collation); g_free(numbering); g_free(hourCycle);
    return obj;
}

static JSValue
intl_locale_toString(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue b = JS_GetPropertyStr(ctx, this_val, "baseName");
    if (JS_IsString(b)) return b;
    JS_FreeValue(ctx, b);
    return intl_str(ctx, "und");
}

static JSValue
intl_locale_identity(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    return JS_DupValue(ctx, this_val);
}

/* ---- Intl.Collator ------------------------------------------------------ */

static JSValue
intl_collator_ctor(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    char *locale = intl_arg_locale(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED);
    JSValueConst o = argc >= 2 ? argv[1] : JS_UNDEFINED;
    JSValue obj = intl_new(ctx, this_val, "Collator");
    char *usage = intl_opt_str(ctx, o, "usage");
    char *sensitivity = intl_opt_str(ctx, o, "sensitivity");
    char *caseFirst = intl_opt_str(ctx, o, "caseFirst");
    intl_hide(ctx, obj, "_locale", intl_str(ctx, locale));
    intl_hide(ctx, obj, "_usage", intl_str(ctx, usage ? usage : "sort"));
    intl_hide(ctx, obj, "_sensitivity",
              intl_str(ctx, sensitivity ? sensitivity : "variant"));
    intl_hide(ctx, obj, "_caseFirst",
              intl_str(ctx, caseFirst ? caseFirst : "false"));
    intl_hide(ctx, obj, "_numeric",
              JS_NewBool(ctx, intl_opt_bool(ctx, o, "numeric", 0)));
    intl_bind_bound(ctx, obj, "compare", intl_collator_compare_b, 2, obj);
    g_free(locale); g_free(usage); g_free(sensitivity); g_free(caseFirst);
    return obj;
}

static char *
intl_collation_key(const char *s, const char *sensitivity)
{
    char *norm = g_utf8_normalize(s, -1, G_NORMALIZE_ALL);
    if (!norm) norm = g_strdup(s);
    gboolean fold = strcmp(sensitivity, "base") == 0 ||
                    strcmp(sensitivity, "accent") == 0;
    gboolean strip = strcmp(sensitivity, "base") == 0 ||
                     strcmp(sensitivity, "case") == 0;
    char *cur = norm;
    if (strip) {
        GString *g = g_string_new(NULL);
        for (char *p = cur; *p; p = g_utf8_next_char(p)) {
            gunichar c = g_utf8_get_char(p);
            GUnicodeType t = g_unichar_type(c);
            if (t == G_UNICODE_NON_SPACING_MARK ||
                t == G_UNICODE_SPACING_MARK ||
                t == G_UNICODE_ENCLOSING_MARK)
                continue;
            g_string_append_unichar(g, c);
        }
        char *stripped = g_string_free(g, FALSE);
        g_free(cur);
        cur = stripped;
    }
    if (fold) {
        char *f = g_utf8_casefold(cur, -1);
        g_free(cur);
        cur = f;
    }
    char *key = g_utf8_collate_key(cur, -1);
    g_free(cur);
    return key;
}

static int
intl_numeric_compare(const char *a, const char *b)
{
    while (*a && *b) {
        if (g_ascii_isdigit(*a) && g_ascii_isdigit(*b)) {
            const char *ea = a, *eb = b;
            while (g_ascii_isdigit(*ea)) ea++;
            while (g_ascii_isdigit(*eb)) eb++;
            while (*a == '0' && a + 1 < ea) a++;
            while (*b == '0' && b + 1 < eb) b++;
            long la = ea - a, lb = eb - b;
            if (la != lb) return la < lb ? -1 : 1;
            int c = strncmp(a, b, (size_t)la);
            if (c) return c < 0 ? -1 : 1;
            a = ea; b = eb;
        } else {
            if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
            a++; b++;
        }
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

static JSValue
intl_collator_compare(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    const char *a = JS_ToCString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED);
    const char *b = JS_ToCString(ctx, argc >= 2 ? argv[1] : JS_UNDEFINED);
    if (!a || !b) {
        JS_FreeCString(ctx, a);
        JS_FreeCString(ctx, b);
        return JS_EXCEPTION;
    }
    char *sens = intl_hget_str(ctx, this_val, "_sensitivity");
    int numeric = intl_hget_int(ctx, this_val, "_numeric", 0);
    int r;
    if (numeric) {
        r = intl_numeric_compare(a, b);
    } else {
        char *ka = intl_collation_key(a, sens ? sens : "variant");
        char *kb = intl_collation_key(b, sens ? sens : "variant");
        r = strcmp(ka, kb);
        if (r == 0 && strcmp(sens ? sens : "variant", "variant") == 0)
            r = g_utf8_collate(a, b);
        r = r < 0 ? -1 : r > 0 ? 1 : 0;
        g_free(ka); g_free(kb);
    }
    g_free(sens);
    JS_FreeCString(ctx, a);
    JS_FreeCString(ctx, b);
    return JS_NewInt32(ctx, r);
}

static JSValue
intl_collator_resolved(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    char *loc = intl_hget_str(ctx, this_val, "_locale");
    char *usage = intl_hget_str(ctx, this_val, "_usage");
    char *sens = intl_hget_str(ctx, this_val, "_sensitivity");
    char *cf = intl_hget_str(ctx, this_val, "_caseFirst");
    JS_SetPropertyStr(ctx, o, "locale", intl_str(ctx, loc ? loc : "en-US"));
    JS_SetPropertyStr(ctx, o, "usage", intl_str(ctx, usage ? usage : "sort"));
    JS_SetPropertyStr(ctx, o, "sensitivity",
                      intl_str(ctx, sens ? sens : "variant"));
    JS_SetPropertyStr(ctx, o, "caseFirst", intl_str(ctx, cf ? cf : "false"));
    JS_SetPropertyStr(ctx, o, "collation", intl_str(ctx, "default"));
    JS_SetPropertyStr(ctx, o, "numeric",
                      JS_NewBool(ctx, intl_hget_int(ctx, this_val, "_numeric", 0)));
    JS_SetPropertyStr(ctx, o, "ignorePunctuation", JS_FALSE);
    g_free(loc); g_free(usage); g_free(sens); g_free(cf);
    return o;
}

/* ---- Intl.NumberFormat -------------------------------------------------- */

static void
intl_nf_separators(const char *locale, const char **group, const char **decimal)
{
    static const char *const comma_decimal[] = {
        "de","fr","es","it","nl","pt","ru","pl","tr","sv","nb","nn","no","da",
        "fi","cs","el","hu","ro","uk","id","vi","ca","hr","sk","sl","bg","lt",
        "lv","et","is","af","sr","gl","eu", NULL };
    static const char *const space_group[] = {
        "fr","ru","pl","uk","fi","sv","cs","hu","sk","nb","nn","no", NULL };
    gboolean cd = intl_lang_in(locale, comma_decimal);
    if (intl_lang_in(locale, space_group)) {
        *group = "\xc2\xa0";
        *decimal = cd ? "," : ".";
        return;
    }
    *group = cd ? "." : ",";
    *decimal = cd ? "," : ".";
}

static const char *
intl_currency_symbol(const char *code)
{
    if (!code) return "$";
    static const struct { const char *c, *s; } tab[] = {
        {"USD","$"},{"CAD","CA$"},{"AUD","A$"},{"NZD","NZ$"},{"EUR","\xe2\x82\xac"},
        {"GBP","\xc2\xa3"},{"JPY","\xc2\xa5"},{"CNY","CN\xc2\xa5"},
        {"INR","\xe2\x82\xb9"},{"KRW","\xe2\x82\xa9"},{"RUB","\xe2\x82\xbd"},
        {"BRL","R$"},{"ZAR","R"},{"MXN","MX$"},{"CHF","CHF\xc2\xa0"},
        {"SEK","kr"},{"NOK","kr"},{"DKK","kr"},{"HKD","HK$"},
    };
    for (gsize i = 0; i < G_N_ELEMENTS(tab); i++)
        if (!g_ascii_strcasecmp(code, tab[i].c)) return tab[i].s;
    return code;
}

static JSValue
intl_nf_ctor(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv)
{
    char *locale = intl_arg_locale(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED);
    JSValueConst o = argc >= 2 ? argv[1] : JS_UNDEFINED;
    JSValue obj = intl_new(ctx, this_val, "NumberFormat");
    char *style = intl_opt_str(ctx, o, "style");
    char *currency = intl_opt_str(ctx, o, "currency");
    char *currencyDisplay = intl_opt_str(ctx, o, "currencyDisplay");
    char *unit = intl_opt_str(ctx, o, "unit");
    char *unitDisplay = intl_opt_str(ctx, o, "unitDisplay");
    char *notation = intl_opt_str(ctx, o, "notation");
    char *signDisplay = intl_opt_str(ctx, o, "signDisplay");

    intl_hide(ctx, obj, "_locale", intl_str(ctx, locale));
    intl_hide(ctx, obj, "_style", intl_str(ctx, style ? style : "decimal"));
    if (currency) intl_hide(ctx, obj, "_currency", intl_str(ctx, currency));
    intl_hide(ctx, obj, "_currencyDisplay",
              intl_str(ctx, currencyDisplay ? currencyDisplay : "symbol"));
    if (unit) intl_hide(ctx, obj, "_unit", intl_str(ctx, unit));
    intl_hide(ctx, obj, "_unitDisplay",
              intl_str(ctx, unitDisplay ? unitDisplay : "short"));
    intl_hide(ctx, obj, "_notation",
              intl_str(ctx, notation ? notation : "standard"));
    intl_hide(ctx, obj, "_signDisplay",
              intl_str(ctx, signDisplay ? signDisplay : "auto"));

    double d;
    int minfd = -1, maxfd = -1, minid = 1;
    gboolean range_err = FALSE;
    if (intl_opt_num(ctx, o, "minimumFractionDigits", &d)) {
        if (!(d >= 0 && d <= 100)) range_err = TRUE;
        minfd = (int)d;
    }
    if (intl_opt_num(ctx, o, "maximumFractionDigits", &d)) {
        if (!(d >= 0 && d <= 100)) range_err = TRUE;
        maxfd = (int)d;
    }
    if (intl_opt_num(ctx, o, "minimumIntegerDigits", &d)) {
        if (!(d >= 1 && d <= 21)) range_err = TRUE;
        minid = (int)d;
    }
    intl_hide(ctx, obj, "_minfd", JS_NewInt32(ctx, minfd));
    intl_hide(ctx, obj, "_maxfd", JS_NewInt32(ctx, maxfd));
    intl_hide(ctx, obj, "_minid",
              JS_NewInt32(ctx, minid < 1 ? 1 : minid > 21 ? 21 : minid));
    intl_hide(ctx, obj, "_grouping",
              JS_NewBool(ctx, intl_opt_bool(ctx, o, "useGrouping", 1)));
    intl_bind_bound(ctx, obj, "format", intl_nf_format_b, 1, obj);

    g_free(locale); g_free(style); g_free(currency); g_free(currencyDisplay);
    g_free(unit); g_free(unitDisplay); g_free(notation); g_free(signDisplay);
    if (range_err) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowRangeError(ctx,
            "Intl.NumberFormat digit option is out of range");
    }
    return obj;
}

static void
intl_nf_group_into(GString *out, const char *digits, const char *sep)
{
    int len = (int)strlen(digits);
    for (int i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) g_string_append(out, sep);
        g_string_append_c(out, digits[i]);
    }
}

static JSValue
intl_nf_parts(JSContext *ctx, JSValueConst this_val, double num)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t n = 0;
    char *style = intl_hget_str(ctx, this_val, "_style");
    char *locale = intl_hget_str(ctx, this_val, "_locale");
    char *notation = intl_hget_str(ctx, this_val, "_notation");
    char *signDisplay = intl_hget_str(ctx, this_val, "_signDisplay");
    int minfd = intl_hget_int(ctx, this_val, "_minfd", -1);
    int maxfd = intl_hget_int(ctx, this_val, "_maxfd", -1);
    int minid = intl_hget_int(ctx, this_val, "_minid", 1);
    int grouping = intl_hget_int(ctx, this_val, "_grouping", 1);
    gboolean is_currency = style && !strcmp(style, "currency");
    gboolean is_percent = style && !strcmp(style, "percent");
    gboolean is_unit = style && !strcmp(style, "unit");
    gboolean compact = notation && !strcmp(notation, "compact");

    if (isnan(num)) {
        JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, "nan", "NaN"));
        goto done;
    }
    int neg = num < 0 || (num == 0 && 1.0 / num < 0);
    double a = fabs(num);
    if (is_percent) a *= 100.0;
    if (!isfinite(a)) {
        if (neg) JS_SetPropertyUint32(ctx, arr, n++,
                                      intl_part(ctx, "minusSign", "-"));
        JS_SetPropertyUint32(ctx, arr, n++,
                             intl_part(ctx, "infinity", "\xe2\x88\x9e"));
        goto done;
    }

    const char *suffix = "";
    if (compact && a >= 1000.0) {
        static const struct { double v; const char *s; } u[] = {
            {1e12,"T"},{1e9,"B"},{1e6,"M"},{1e3,"K"} };
        for (gsize i = 0; i < G_N_ELEMENTS(u); i++)
            if (a >= u[i].v) { a /= u[i].v; suffix = u[i].s; break; }
    }

    if (minfd < 0) minfd = is_currency ? 2 : 0;
    if (maxfd < 0) maxfd = is_currency ? 2
                        : is_percent ? 0
                        : compact ? (minfd > 1 ? minfd : 1) : 3;
    if (maxfd < minfd) maxfd = minfd;

    char buf[64];
    double scale = pow(10.0, maxfd);
    double scaled = a * scale;
    if (isfinite(scaled) && scaled - floor(scaled) == 0.5)
        g_snprintf(buf, sizeof buf, "%.*f", maxfd, (floor(scaled) + 1.0) / scale);
    else
        g_snprintf(buf, sizeof buf, "%.*f", maxfd, a);
    char *dot = strchr(buf, '.');
    char intpart[80], fracpart[32];
    if (dot) {
        g_strlcpy(intpart, buf, MIN((gsize)(dot - buf) + 1, sizeof intpart));
        g_strlcpy(fracpart, dot + 1, sizeof fracpart);
    } else {
        g_strlcpy(intpart, buf, sizeof intpart);
        fracpart[0] = '\0';
    }
    int fraclen = (int)strlen(fracpart);
    while (fraclen > minfd && fracpart[fraclen - 1] == '0')
        fracpart[--fraclen] = '\0';

    while ((int)strlen(intpart) < minid && strlen(intpart) + 1 < sizeof intpart) {
        memmove(intpart + 1, intpart, strlen(intpart) + 1);
        intpart[0] = '0';
    }

    const char *gsep, *dsep;
    intl_nf_separators(locale, &gsep, &dsep);

    if (neg)
        JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, "minusSign", "-"));
    else if (signDisplay && (!strcmp(signDisplay, "always") ||
                             !strcmp(signDisplay, "exceptZero")) && a != 0)
        JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, "plusSign", "+"));

    if (is_currency) {
        char *cur = intl_hget_str(ctx, this_val, "_currency");
        char *disp = intl_hget_str(ctx, this_val, "_currencyDisplay");
        const char *sym = (disp && !strcmp(disp, "code")) ? (cur ? cur : "")
                        : intl_currency_symbol(cur ? cur : "USD");
        JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, "currency", sym));
        g_free(cur); g_free(disp);
    }

    GString *ig = g_string_new(NULL);
    if (grouping && !compact)
        intl_nf_group_into(ig, intpart, gsep);
    else
        g_string_append(ig, intpart);
    JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, "integer", ig->str));
    g_string_free(ig, TRUE);

    if (fracpart[0]) {
        JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, "decimal", dsep));
        JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, "fraction", fracpart));
    }
    if (suffix[0])
        JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, "compact", suffix));
    if (is_percent) {
        JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, "literal", ""));
        JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, "percentSign", "%"));
    }
    if (is_unit) {
        char *unit = intl_hget_str(ctx, this_val, "_unit");
        JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, "literal", "\xc2\xa0"));
        JS_SetPropertyUint32(ctx, arr, n++,
                             intl_part(ctx, "unit", unit ? unit : ""));
        g_free(unit);
    }

done:
    g_free(style); g_free(locale); g_free(notation); g_free(signDisplay);
    return arr;
}

static JSValue
intl_join_parts(JSContext *ctx, JSValue parts)
{
    GString *s = g_string_new(NULL);
    JSValue lv = JS_GetPropertyStr(ctx, parts, "length");
    uint32_t len = 0; JS_ToUint32(ctx, &len, lv); JS_FreeValue(ctx, lv);
    for (uint32_t i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, parts, i);
        JSValue v = JS_GetPropertyStr(ctx, e, "value");
        const char *cs = JS_ToCString(ctx, v);
        if (cs) { g_string_append(s, cs); JS_FreeCString(ctx, cs); }
        JS_FreeValue(ctx, v); JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, parts);
    JSValue out = JS_NewStringLen(ctx, s->str, s->len);
    g_string_free(s, TRUE);
    return out;
}

static JSValue
intl_nf_format(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    double num = 0;
    if (argc >= 1) JS_ToFloat64(ctx, &num, argv[0]);
    return intl_join_parts(ctx, intl_nf_parts(ctx, this_val, num));
}

static JSValue
intl_nf_formatToParts(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    double num = 0;
    if (argc >= 1) JS_ToFloat64(ctx, &num, argv[0]);
    return intl_nf_parts(ctx, this_val, num);
}

static JSValue
intl_nf_formatRange(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    double a = 0, b = 0;
    if (argc >= 1) JS_ToFloat64(ctx, &a, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &b, argv[1]);
    JSValue sa = intl_join_parts(ctx, intl_nf_parts(ctx, this_val, a));
    JSValue sb = intl_join_parts(ctx, intl_nf_parts(ctx, this_val, b));
    const char *ca = JS_ToCString(ctx, sa);
    const char *cb = JS_ToCString(ctx, sb);
    char *joined = g_strdup_printf("%s\xe2\x80\x93%s", ca ? ca : "", cb ? cb : "");
    if (ca) JS_FreeCString(ctx, ca);
    if (cb) JS_FreeCString(ctx, cb);
    JS_FreeValue(ctx, sa); JS_FreeValue(ctx, sb);
    JSValue out = JS_NewString(ctx, joined);
    g_free(joined);
    return out;
}

static JSValue
intl_nf_resolved(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    char *loc = intl_hget_str(ctx, this_val, "_locale");
    char *style = intl_hget_str(ctx, this_val, "_style");
    char *notation = intl_hget_str(ctx, this_val, "_notation");
    char *cur = intl_hget_str(ctx, this_val, "_currency");
    JS_SetPropertyStr(ctx, o, "locale", intl_str(ctx, loc ? loc : "en-US"));
    JS_SetPropertyStr(ctx, o, "numberingSystem", intl_str(ctx, "latn"));
    JS_SetPropertyStr(ctx, o, "style", intl_str(ctx, style ? style : "decimal"));
    JS_SetPropertyStr(ctx, o, "notation", intl_str(ctx, notation ? notation : "standard"));
    if (cur) JS_SetPropertyStr(ctx, o, "currency", intl_str(ctx, cur));
    JS_SetPropertyStr(ctx, o, "useGrouping",
                      JS_NewBool(ctx, intl_hget_int(ctx, this_val, "_grouping", 1)));
    int minfd = intl_hget_int(ctx, this_val, "_minfd", -1);
    int maxfd = intl_hget_int(ctx, this_val, "_maxfd", -1);
    gboolean is_currency = style && !strcmp(style, "currency");
    if (minfd < 0) minfd = is_currency ? 2 : 0;
    if (maxfd < 0) maxfd = is_currency ? 2 : (style && !strcmp(style, "percent") ? 0 : 3);
    JS_SetPropertyStr(ctx, o, "minimumIntegerDigits",
                      JS_NewInt32(ctx, intl_hget_int(ctx, this_val, "_minid", 1)));
    JS_SetPropertyStr(ctx, o, "minimumFractionDigits", JS_NewInt32(ctx, minfd));
    JS_SetPropertyStr(ctx, o, "maximumFractionDigits", JS_NewInt32(ctx, maxfd));
    g_free(loc); g_free(style); g_free(notation); g_free(cur);
    return o;
}

/* ---- Intl.DateTimeFormat ----------------------------------------------- */

static const char *const intl_months[] = {
    "January","February","March","April","May","June","July","August",
    "September","October","November","December" };
static const char *const intl_days[] = {
    "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday" };

static JSValue
intl_dtf_ctor(JSContext *ctx, JSValueConst this_val,
              int argc, JSValueConst *argv)
{
    char *locale = intl_arg_locale(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED);
    JSValueConst o = argc >= 2 ? argv[1] : JS_UNDEFINED;
    JSValue obj = intl_new(ctx, this_val, "DateTimeFormat");
    intl_hide(ctx, obj, "_locale", intl_str(ctx, locale));

    JSValue opts = JS_NewObject(ctx);
    char *dateStyle = intl_opt_str(ctx, o, "dateStyle");
    char *timeStyle = intl_opt_str(ctx, o, "timeStyle");
    char *tz = intl_opt_str(ctx, o, "timeZone");
    intl_hide(ctx, obj, "_tz", intl_str(ctx, tz ? tz : "local"));

    if (dateStyle) {
        if (!strcmp(dateStyle, "full")) {
            JS_SetPropertyStr(ctx, opts, "weekday", intl_str(ctx, "long"));
            JS_SetPropertyStr(ctx, opts, "year", intl_str(ctx, "numeric"));
            JS_SetPropertyStr(ctx, opts, "month", intl_str(ctx, "long"));
            JS_SetPropertyStr(ctx, opts, "day", intl_str(ctx, "numeric"));
        } else if (!strcmp(dateStyle, "long")) {
            JS_SetPropertyStr(ctx, opts, "year", intl_str(ctx, "numeric"));
            JS_SetPropertyStr(ctx, opts, "month", intl_str(ctx, "long"));
            JS_SetPropertyStr(ctx, opts, "day", intl_str(ctx, "numeric"));
        } else if (!strcmp(dateStyle, "medium")) {
            JS_SetPropertyStr(ctx, opts, "year", intl_str(ctx, "numeric"));
            JS_SetPropertyStr(ctx, opts, "month", intl_str(ctx, "short"));
            JS_SetPropertyStr(ctx, opts, "day", intl_str(ctx, "numeric"));
        } else {
            JS_SetPropertyStr(ctx, opts, "year", intl_str(ctx, "2-digit"));
            JS_SetPropertyStr(ctx, opts, "month", intl_str(ctx, "numeric"));
            JS_SetPropertyStr(ctx, opts, "day", intl_str(ctx, "numeric"));
        }
    }
    if (timeStyle) {
        JS_SetPropertyStr(ctx, opts, "hour", intl_str(ctx, "numeric"));
        JS_SetPropertyStr(ctx, opts, "minute", intl_str(ctx, "2-digit"));
        if (strcmp(timeStyle, "short"))
            JS_SetPropertyStr(ctx, opts, "second", intl_str(ctx, "2-digit"));
    }
    static const char *const keys[] = {
        "weekday","era","year","month","day","hour","minute","second",
        "hour12","hourCycle","dayPeriod" };
    for (gsize i = 0; i < G_N_ELEMENTS(keys); i++) {
        if (intl_opt_present(ctx, o, keys[i])) {
            JSValue v = JS_GetPropertyStr(ctx, o, keys[i]);
            JS_SetPropertyStr(ctx, opts, keys[i], v);
        }
    }
    gboolean any = FALSE;
    static const char *const datetime_keys[] = {
        "weekday","year","month","day","hour","minute","second" };
    for (gsize i = 0; i < G_N_ELEMENTS(datetime_keys); i++) {
        JSValue v = JS_GetPropertyStr(ctx, opts, datetime_keys[i]);
        if (!JS_IsUndefined(v)) any = TRUE;
        JS_FreeValue(ctx, v);
    }
    if (!any) {
        JS_SetPropertyStr(ctx, opts, "year", intl_str(ctx, "numeric"));
        JS_SetPropertyStr(ctx, opts, "month", intl_str(ctx, "numeric"));
        JS_SetPropertyStr(ctx, opts, "day", intl_str(ctx, "numeric"));
    }
    intl_hide(ctx, obj, "_opts", opts);
    intl_bind_bound(ctx, obj, "format", intl_dtf_format_b, 1, obj);
    g_free(locale); g_free(dateStyle); g_free(timeStyle); g_free(tz);
    return obj;
}

static JSValue
intl_dtf_parts_core(JSContext *ctx, JSValueConst opts, const char *locale,
                    const char *tz, double ms)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t n = 0;
    if (isnan(ms)) {
        JS_SetPropertyUint32(ctx, arr, n++,
                             intl_part(ctx, "literal", "Invalid Date"));
        return arr;
    }
    time_t secs = (time_t)floor(ms / 1000.0);
    struct tm tmv;
    gboolean utc = tz && !strcmp(tz, "UTC");
    gboolean ok = utc ? intl_gmtime(&secs, &tmv) : intl_localtime(&secs, &tmv);
    if (!ok) {
        JS_SetPropertyUint32(ctx, arr, n++,
                             intl_part(ctx, "literal", "Invalid Date"));
        return arr;
    }

    int Y = tmv.tm_year + 1900, Mo = tmv.tm_mon, D = tmv.tm_mday,
        Wd = tmv.tm_wday, H = tmv.tm_hour, Mi = tmv.tm_min, S = tmv.tm_sec;

    char *weekday = intl_opt_str(ctx, opts, "weekday");
    char *month = intl_opt_str(ctx, opts, "month");
    char *day = intl_opt_str(ctx, opts, "day");
    char *year = intl_opt_str(ctx, opts, "year");
    char *hour = intl_opt_str(ctx, opts, "hour");
    char *minute = intl_opt_str(ctx, opts, "minute");
    char *second = intl_opt_str(ctx, opts, "second");
    char *hourCycle = intl_opt_str(ctx, opts, "hourCycle");
    int hour12 = intl_opt_bool(ctx, opts, "hour12", -1);

    char buf[32];
    GPtrArray *dp = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *dpt = g_ptr_array_new_with_free_func(g_free);

    if (weekday) {
        const char *nm = intl_days[((Wd % 7) + 7) % 7];
        char *v = !strcmp(weekday, "narrow") ? g_strndup(nm, 1)
                : !strcmp(weekday, "short") ? g_strndup(nm, 3)
                : g_strdup(nm);
        g_ptr_array_add(dp, g_strconcat("weekday\x01", v, NULL));
        g_free(v);
    }
    if (month) {
        const char *nm = intl_months[((Mo % 12) + 12) % 12];
        char *v;
        if (!strcmp(month, "long")) v = g_strdup(nm);
        else if (!strcmp(month, "short")) v = g_strndup(nm, 3);
        else if (!strcmp(month, "narrow")) v = g_strndup(nm, 1);
        else if (!strcmp(month, "2-digit")) v = g_strdup_printf("%02d", Mo + 1);
        else v = g_strdup_printf("%d", Mo + 1);
        g_ptr_array_add(dp, g_strconcat("month\x01", v, NULL));
        g_free(v);
    }
    if (day) {
        char *v = !strcmp(day, "2-digit") ? g_strdup_printf("%02d", D)
                                          : g_strdup_printf("%d", D);
        g_ptr_array_add(dp, g_strconcat("day\x01", v, NULL));
        g_free(v);
    }
    if (year) {
        char *v = !strcmp(year, "2-digit") ? g_strdup_printf("%02d", (Y % 100 + 100) % 100)
                                           : g_strdup_printf("%d", Y);
        g_ptr_array_add(dp, g_strconcat("year\x01", v, NULL));
        g_free(v);
    }

    gboolean h12 = hour12 >= 0 ? hour12
        : hourCycle ? (!strcmp(hourCycle, "h11") || !strcmp(hourCycle, "h12")) : TRUE;
    if (hour || minute || second) {
        int hh = H; const char *ap = NULL;
        if (h12) { ap = H < 12 ? "AM" : "PM"; hh = H % 12; if (hh == 0) hh = 12; }
        if (hour) {
            char *v = !strcmp(hour, "2-digit") ? g_strdup_printf("%02d", hh)
                                               : g_strdup_printf("%d", hh);
            g_ptr_array_add(dpt, g_strconcat("hour\x01", v, NULL)); g_free(v);
        }
        if (minute) {
            char *v = !strcmp(minute, "2-digit") ? g_strdup_printf("%02d", Mi)
                                                 : g_strdup_printf("%d", Mi);
            g_ptr_array_add(dpt, g_strconcat("minute\x01", v, NULL)); g_free(v);
        }
        if (second) {
            char *v = !strcmp(second, "2-digit") ? g_strdup_printf("%02d", S)
                                                 : g_strdup_printf("%d", S);
            g_ptr_array_add(dpt, g_strconcat("second\x01", v, NULL)); g_free(v);
        }
        if (ap) g_ptr_array_add(dpt, g_strconcat("dayPeriod\x01", ap, NULL));
    }

    gboolean numericDate = month && day && !weekday &&
        (!strcmp(month, "numeric") || !strcmp(month, "2-digit")) &&
        (!strcmp(day, "numeric") || !strcmp(day, "2-digit"));

    char lang[16]; intl_lang_subtag(locale, lang, sizeof lang);
    static const char *const dmy[] = {
        "de","nb","nn","no","da","fi","fr","es","it","nl","pt","ru","pl","sv", NULL };
    gboolean dmy_order = FALSE;
    for (int i = 0; dmy[i]; i++) if (!strcmp(lang, dmy[i])) { dmy_order = TRUE; break; }

    GPtrArray *ordered = g_ptr_array_new();
    if (numericDate) {
        const char *sep = (!strcmp(lang, "de") || !strcmp(lang, "nb") ||
                           !strcmp(lang, "nn") || !strcmp(lang, "no") ||
                           !strcmp(lang, "da") || !strcmp(lang, "fi")) ? "." : "/";
        const char *order[3];
        if (dmy_order) { order[0] = "day"; order[1] = "month"; order[2] = "year"; }
        else { order[0] = "month"; order[1] = "day"; order[2] = "year"; }
        gboolean first = TRUE;
        for (int k = 0; k < 3; k++) {
            for (guint j = 0; j < dp->len; j++) {
                char *entry = g_ptr_array_index(dp, j);
                if (g_str_has_prefix(entry, order[k])) {
                    if (!first) g_ptr_array_add(ordered, g_strconcat("literal\x01", sep, NULL));
                    g_ptr_array_add(ordered, g_strdup(entry));
                    first = FALSE;
                }
            }
        }
    } else {
        for (guint j = 0; j < dp->len; j++) {
            char *entry = g_ptr_array_index(dp, j);
            if (ordered->len) {
                char *prev = g_ptr_array_index(ordered, ordered->len - 1);
                const char *lit = g_str_has_prefix(prev, "weekday") ? ", "
                                : g_str_has_prefix(entry, "year") ? ", " : " ";
                g_ptr_array_add(ordered, g_strconcat("literal\x01", lit, NULL));
            }
            g_ptr_array_add(ordered, g_strdup(entry));
        }
    }
    if (dpt->len) {
        if (ordered->len)
            g_ptr_array_add(ordered, g_strconcat("literal\x01", ", ", NULL));
        for (guint j = 0; j < dpt->len; j++) {
            char *entry = g_ptr_array_index(dpt, j);
            if (j > 0) {
                const char *sepc = g_str_has_prefix(entry, "dayPeriod") ? " " : ":";
                g_ptr_array_add(ordered, g_strconcat("literal\x01", sepc, NULL));
            }
            g_ptr_array_add(ordered, g_strdup(entry));
        }
    }

    for (guint j = 0; j < ordered->len; j++) {
        char *entry = g_ptr_array_index(ordered, j);
        char *sep = strchr(entry, '\x01');
        if (!sep) { g_free(entry); continue; }
        *sep = '\0';
        JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, entry, sep + 1));
        g_free(entry);
    }
    g_ptr_array_free(ordered, TRUE);
    g_ptr_array_free(dp, TRUE);
    g_ptr_array_free(dpt, TRUE);
    g_free(weekday); g_free(month); g_free(day); g_free(year);
    g_free(hour); g_free(minute); g_free(second); g_free(hourCycle);
    (void)buf;
    return arr;
}

static double
intl_to_ms(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 1 || JS_IsUndefined(argv[0]))
        return (double)(g_get_real_time() / 1000);
    double ms;
    if (JS_ToFloat64(ctx, &ms, argv[0]) != 0) return NAN;
    return ms;
}

static JSValue
intl_dtf_parts(JSContext *ctx, JSValueConst this_val, double ms)
{
    JSValue opts = JS_GetPropertyStr(ctx, this_val, "_opts");
    char *locale = intl_hget_str(ctx, this_val, "_locale");
    char *tz = intl_hget_str(ctx, this_val, "_tz");
    JSValue arr = intl_dtf_parts_core(ctx, opts, locale ? locale : "en-US",
                                      tz ? tz : "local", ms);
    JS_FreeValue(ctx, opts);
    g_free(locale); g_free(tz);
    return arr;
}

static JSValue
intl_dtf_format(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv)
{
    return intl_join_parts(ctx, intl_dtf_parts(ctx, this_val,
                                               intl_to_ms(ctx, argc, argv)));
}

static JSValue
intl_dtf_formatToParts(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    return intl_dtf_parts(ctx, this_val, intl_to_ms(ctx, argc, argv));
}

static JSValue
intl_dtf_formatRange(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    double a = NAN, b = NAN;
    if (argc >= 1) JS_ToFloat64(ctx, &a, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &b, argv[1]); else b = a;
    JSValue sa = intl_join_parts(ctx, intl_dtf_parts(ctx, this_val, a));
    JSValue sb = intl_join_parts(ctx, intl_dtf_parts(ctx, this_val, b));
    const char *ca = JS_ToCString(ctx, sa), *cb = JS_ToCString(ctx, sb);
    char *joined = g_strdup_printf("%s\xe2\x80\x93%s", ca ? ca : "", cb ? cb : "");
    if (ca) JS_FreeCString(ctx, ca);
    if (cb) JS_FreeCString(ctx, cb);
    JS_FreeValue(ctx, sa); JS_FreeValue(ctx, sb);
    JSValue out = JS_NewString(ctx, joined);
    g_free(joined);
    return out;
}

static const char *
intl_local_tz_id(void)
{
    static char *cached;
    static gboolean tried;
    if (!tried) {
        tried = TRUE;
#ifdef _WIN32
        DYNAMIC_TIME_ZONE_INFORMATION info = {0};
        if (GetDynamicTimeZoneInformation(&info) != TIME_ZONE_ID_INVALID) {
            char *key = g_utf16_to_utf8((const gunichar2 *)info.TimeZoneKeyName,
                                        -1, NULL, NULL, NULL);
            wchar_t locale_name[LOCALE_NAME_MAX_LENGTH] = {0};
            GetUserDefaultLocaleName(locale_name, G_N_ELEMENTS(locale_name));
            char *locale = g_utf16_to_utf8((const gunichar2 *)locale_name,
                                           -1, NULL, NULL, NULL);
            const char *region = locale ? strrchr(locale, '-') : NULL;
            if (key && !strcmp(key, "W. Europe Standard Time") && region) {
                static const struct { const char *region; const char *zone; } west[] = {
                    {"NO","Europe/Oslo"},{"SE","Europe/Stockholm"},
                    {"DK","Europe/Copenhagen"},{"DE","Europe/Berlin"},
                    {"NL","Europe/Amsterdam"},{"BE","Europe/Brussels"},
                    {"AT","Europe/Vienna"},{"CH","Europe/Zurich"},
                    {"IT","Europe/Rome"},{"ES","Europe/Madrid"},
                    {NULL,NULL}
                };
                for (int i = 0; west[i].region; i++)
                    if (!g_ascii_strcasecmp(region + 1, west[i].region)) {
                        cached = g_strdup(west[i].zone);
                        break;
                    }
            }
            static const struct { const char *windows; const char *iana; } zones[] = {
                {"UTC","UTC"},
                {"GMT Standard Time","Europe/London"},
                {"W. Europe Standard Time","Europe/Berlin"},
                {"Romance Standard Time","Europe/Paris"},
                {"Central Europe Standard Time","Europe/Budapest"},
                {"Central European Standard Time","Europe/Warsaw"},
                {"FLE Standard Time","Europe/Kyiv"},
                {"E. Europe Standard Time","Europe/Chisinau"},
                {"Turkey Standard Time","Europe/Istanbul"},
                {"Russian Standard Time","Europe/Moscow"},
                {"Israel Standard Time","Asia/Jerusalem"},
                {"Arabian Standard Time","Asia/Dubai"},
                {"India Standard Time","Asia/Kolkata"},
                {"China Standard Time","Asia/Shanghai"},
                {"Tokyo Standard Time","Asia/Tokyo"},
                {"Korea Standard Time","Asia/Seoul"},
                {"AUS Eastern Standard Time","Australia/Sydney"},
                {"New Zealand Standard Time","Pacific/Auckland"},
                {"Pacific Standard Time","America/Los_Angeles"},
                {"Mountain Standard Time","America/Denver"},
                {"Central Standard Time","America/Chicago"},
                {"Eastern Standard Time","America/New_York"},
                {"Atlantic Standard Time","America/Halifax"},
                {"SA Eastern Standard Time","America/Sao_Paulo"},
                {"Argentina Standard Time","America/Argentina/Buenos_Aires"},
                {"South Africa Standard Time","Africa/Johannesburg"},
                {NULL,NULL}
            };
            if (!cached && key)
                for (int i = 0; zones[i].windows; i++)
                    if (!strcmp(key, zones[i].windows)) {
                        cached = g_strdup(zones[i].iana);
                        break;
                    }
            g_free(locale);
            g_free(key);
        }
#endif
        GTimeZone *z = g_time_zone_new_local();
        if (z) {
            const char *id = g_time_zone_get_identifier(z);
            if (!cached && id && *id && strcmp(id, "UTC") != 0 &&
                id[0] != '+' && id[0] != '-' && strchr(id, '/'))
                cached = g_strdup(id);
            g_time_zone_unref(z);
        }
        if (!cached) {
            const char *env = g_getenv("TZ");
            if (env && *env && strchr(env, '/')) cached = g_strdup(env);
        }
        if (!cached) cached = g_strdup("UTC");
    }
    return cached;
}

static JSValue
intl_dtf_resolved(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    char *loc = intl_hget_str(ctx, this_val, "_locale");
    char *tz = intl_hget_str(ctx, this_val, "_tz");
    JS_SetPropertyStr(ctx, o, "locale", intl_str(ctx, loc ? loc : "en-US"));
    JS_SetPropertyStr(ctx, o, "calendar", intl_str(ctx, "gregory"));
    JS_SetPropertyStr(ctx, o, "numberingSystem", intl_str(ctx, "latn"));
    JS_SetPropertyStr(ctx, o, "timeZone",
                      intl_str(ctx, tz && strcmp(tz, "local")
                                        ? tz : intl_local_tz_id()));
    JSValue opts = JS_GetPropertyStr(ctx, this_val, "_opts");
    static const char *const keys[] = {
        "weekday","year","month","day","hour","minute","second" };
    for (gsize i = 0; i < G_N_ELEMENTS(keys); i++) {
        JSValue v = JS_GetPropertyStr(ctx, opts, keys[i]);
        if (!JS_IsUndefined(v)) JS_SetPropertyStr(ctx, o, keys[i], v);
        else JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, opts);
    g_free(loc); g_free(tz);
    return o;
}

/* ---- Intl.PluralRules --------------------------------------------------- */

static JSValue
intl_pr_ctor(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv)
{
    char *locale = intl_arg_locale(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED);
    JSValueConst o = argc >= 2 ? argv[1] : JS_UNDEFINED;
    JSValue obj = intl_new(ctx, this_val, "PluralRules");
    char *type = intl_opt_str(ctx, o, "type");
    intl_hide(ctx, obj, "_locale", intl_str(ctx, locale));
    intl_hide(ctx, obj, "_type", intl_str(ctx, type ? type : "cardinal"));
    g_free(locale); g_free(type);
    return obj;
}

static const char *
intl_plural_category(const char *locale, double n)
{
    char lang[16]; intl_lang_subtag(locale, lang, sizeof lang);
    double i = floor(fabs(n));
    int v = 0;
    long mod10 = (long)i % 10, mod100 = (long)i % 100;

    static const char *const french[] = { "fr","ff","kab", NULL };
    static const char *const slavic[] = { "ru","uk","be", NULL };
    static const char *const westslav[] = { "cs","sk", NULL };
    static const char *const polish[] = { "pl", NULL };
    static const char *const arabic[] = { "ar", NULL };
    static const char *const nonecat[] = { "ja","zh","ko","th","vi","id","ms",
                                           "lo","my","km","fa", NULL };

    for (int k = 0; nonecat[k]; k++) if (!strcmp(lang, nonecat[k])) return "other";
    for (int k = 0; french[k]; k++)
        if (!strcmp(lang, french[k])) return (i == 0 || i == 1) ? "one" : "other";
    for (int k = 0; arabic[k]; k++) if (!strcmp(lang, arabic[k])) {
        if (n == 0) return "zero";
        if (n == 1) return "one";
        if (n == 2) return "two";
        if (mod100 >= 3 && mod100 <= 10) return "few";
        if (mod100 >= 11 && mod100 <= 99) return "many";
        return "other";
    }
    for (int k = 0; polish[k]; k++) if (!strcmp(lang, polish[k])) {
        if (i == 1 && v == 0) return "one";
        if (v == 0 && mod10 >= 2 && mod10 <= 4 && !(mod100 >= 12 && mod100 <= 14)) return "few";
        if (v == 0 && (mod10 <= 1 || (mod10 >= 5 && mod10 <= 9) ||
                       (mod100 >= 12 && mod100 <= 14))) return "many";
        return "other";
    }
    for (int k = 0; slavic[k]; k++) if (!strcmp(lang, slavic[k])) {
        if (v == 0 && mod10 == 1 && mod100 != 11) return "one";
        if (v == 0 && mod10 >= 2 && mod10 <= 4 && !(mod100 >= 12 && mod100 <= 14)) return "few";
        if (v == 0 && (mod10 == 0 || (mod10 >= 5 && mod10 <= 9) ||
                       (mod100 >= 11 && mod100 <= 14))) return "many";
        return "other";
    }
    for (int k = 0; westslav[k]; k++) if (!strcmp(lang, westslav[k])) {
        if (i == 1 && v == 0) return "one";
        if (i >= 2 && i <= 4 && v == 0) return "few";
        if (v != 0) return "many";
        return "other";
    }
    return n == 1 ? "one" : "other";
}

static JSValue
intl_pr_select(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    double n = 0;
    if (argc >= 1) JS_ToFloat64(ctx, &n, argv[0]);
    char *loc = intl_hget_str(ctx, this_val, "_locale");
    const char *cat = intl_plural_category(loc ? loc : "en", n);
    g_free(loc);
    return intl_str(ctx, cat);
}

static JSValue
intl_pr_selectRange(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    double b = 1;
    if (argc >= 2) JS_ToFloat64(ctx, &b, argv[1]);
    char *loc = intl_hget_str(ctx, this_val, "_locale");
    const char *cat = intl_plural_category(loc ? loc : "en", b);
    g_free(loc);
    return intl_str(ctx, cat);
}

static JSValue
intl_pr_resolved(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    char *loc = intl_hget_str(ctx, this_val, "_locale");
    char *type = intl_hget_str(ctx, this_val, "_type");
    JS_SetPropertyStr(ctx, o, "locale", intl_str(ctx, loc ? loc : "en-US"));
    JS_SetPropertyStr(ctx, o, "type", intl_str(ctx, type ? type : "cardinal"));
    JSValue cats = JS_NewArray(ctx);
    const char *probe = loc ? loc : "en";
    const char *seen[6]; int ns = 0;
    double samples[] = { 0,1,2,3,5,11,21,100 };
    for (gsize s = 0; s < G_N_ELEMENTS(samples); s++) {
        const char *c = intl_plural_category(probe, samples[s]);
        gboolean dup = FALSE;
        for (int k = 0; k < ns; k++) if (!strcmp(seen[k], c)) dup = TRUE;
        if (!dup && ns < 6) { seen[ns++] = c; }
    }
    for (int k = 0; k < ns; k++)
        JS_SetPropertyUint32(ctx, cats, (uint32_t)k, intl_str(ctx, seen[k]));
    JS_SetPropertyStr(ctx, o, "pluralCategories", cats);
    g_free(loc); g_free(type);
    return o;
}

/* ---- Intl.ListFormat ---------------------------------------------------- */

static JSValue
intl_lf_ctor(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv)
{
    char *locale = intl_arg_locale(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED);
    JSValueConst o = argc >= 2 ? argv[1] : JS_UNDEFINED;
    JSValue obj = intl_new(ctx, this_val, "ListFormat");
    char *type = intl_opt_str(ctx, o, "type");
    char *style = intl_opt_str(ctx, o, "style");
    intl_hide(ctx, obj, "_locale", intl_str(ctx, locale));
    intl_hide(ctx, obj, "_type", intl_str(ctx, type ? type : "conjunction"));
    intl_hide(ctx, obj, "_style", intl_str(ctx, style ? style : "long"));
    g_free(locale); g_free(type); g_free(style);
    return obj;
}

static JSValue
intl_lf_parts(JSContext *ctx, JSValueConst this_val, JSValueConst list)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t out = 0;
    char *type = intl_hget_str(ctx, this_val, "_type");
    JSValue lv = JS_GetPropertyStr(ctx, list, "length");
    uint32_t len = 0; JS_ToUint32(ctx, &len, lv); JS_FreeValue(ctx, lv);
    if (len > (1u << 20)) len = 1u << 20;
    char **items = g_new0(char *, (gsize)len + 1);
    for (uint32_t i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, i);
        const char *s = JS_ToCString(ctx, e);
        items[i] = g_strdup(s ? s : "");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
    }
    gboolean is_unit = type && !strcmp(type, "unit");
    const char *word = (type && !strcmp(type, "disjunction")) ? "or" : "and";
    for (uint32_t i = 0; i < len; i++) {
        JS_SetPropertyUint32(ctx, arr, out++, intl_part(ctx, "element", items[i]));
        if (i < len - 1) {
            const char *lit;
            if (is_unit) lit = ", ";
            else if (i == len - 2)
                lit = NULL;
            else lit = ", ";
            if (is_unit || lit) {
                JS_SetPropertyUint32(ctx, arr, out++, intl_part(ctx, "literal", lit));
            } else {
                char *l = (len == 2) ? g_strdup_printf(" %s ", word)
                                     : g_strdup_printf(", %s ", word);
                JS_SetPropertyUint32(ctx, arr, out++, intl_part(ctx, "literal", l));
                g_free(l);
            }
        }
    }
    for (uint32_t i = 0; i < len; i++) g_free(items[i]);
    g_free(items);
    g_free(type);
    return arr;
}

static JSValue
intl_lf_format(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    JSValueConst list = argc >= 1 ? argv[0] : JS_UNDEFINED;
    if (!JS_IsObject(list)) return intl_str(ctx, "");
    return intl_join_parts(ctx, intl_lf_parts(ctx, this_val, list));
}

static JSValue
intl_lf_formatToParts(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    JSValueConst list = argc >= 1 ? argv[0] : JS_UNDEFINED;
    if (!JS_IsObject(list)) return JS_NewArray(ctx);
    return intl_lf_parts(ctx, this_val, list);
}

static JSValue
intl_lf_resolved(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    char *loc = intl_hget_str(ctx, this_val, "_locale");
    char *type = intl_hget_str(ctx, this_val, "_type");
    char *style = intl_hget_str(ctx, this_val, "_style");
    JS_SetPropertyStr(ctx, o, "locale", intl_str(ctx, loc ? loc : "en-US"));
    JS_SetPropertyStr(ctx, o, "type", intl_str(ctx, type ? type : "conjunction"));
    JS_SetPropertyStr(ctx, o, "style", intl_str(ctx, style ? style : "long"));
    g_free(loc); g_free(type); g_free(style);
    return o;
}

/* ---- Intl.RelativeTimeFormat ------------------------------------------- */

static JSValue
intl_rtf_ctor(JSContext *ctx, JSValueConst this_val,
              int argc, JSValueConst *argv)
{
    char *locale = intl_arg_locale(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED);
    JSValueConst o = argc >= 2 ? argv[1] : JS_UNDEFINED;
    JSValue obj = intl_new(ctx, this_val, "RelativeTimeFormat");
    char *numeric = intl_opt_str(ctx, o, "numeric");
    char *style = intl_opt_str(ctx, o, "style");
    intl_hide(ctx, obj, "_locale", intl_str(ctx, locale));
    intl_hide(ctx, obj, "_numeric", intl_str(ctx, numeric ? numeric : "always"));
    intl_hide(ctx, obj, "_style", intl_str(ctx, style ? style : "long"));
    g_free(locale); g_free(numeric); g_free(style);
    return obj;
}

static char *
intl_rtf_unit(JSContext *ctx, JSValueConst v)
{
    const char *s = JS_ToCString(ctx, v);
    char *u = g_ascii_strdown(s ? s : "", -1);
    if (s) JS_FreeCString(ctx, s);
    size_t l = strlen(u);
    if (l > 0 && u[l - 1] == 's') u[l - 1] = '\0';
    return u;
}

static const char *
intl_rtf_auto(const char *unit, int v)
{
    struct { const char *u; int v; const char *s; } tab[] = {
        {"second",0,"now"},
        {"day",-1,"yesterday"},{"day",0,"today"},{"day",1,"tomorrow"},
        {"week",-1,"last week"},{"week",0,"this week"},{"week",1,"next week"},
        {"month",-1,"last month"},{"month",0,"this month"},{"month",1,"next month"},
        {"quarter",-1,"last quarter"},{"quarter",0,"this quarter"},{"quarter",1,"next quarter"},
        {"year",-1,"last year"},{"year",0,"this year"},{"year",1,"next year"},
    };
    for (gsize i = 0; i < G_N_ELEMENTS(tab); i++)
        if (!strcmp(tab[i].u, unit) && tab[i].v == v) return tab[i].s;
    return NULL;
}

static JSValue
intl_rtf_format(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv)
{
    double val = 0;
    if (argc >= 1) JS_ToFloat64(ctx, &val, argv[0]);
    char *unit = intl_rtf_unit(ctx, argc >= 2 ? argv[1] : JS_UNDEFINED);
    char *numeric = intl_hget_str(ctx, this_val, "_numeric");
    if (numeric && !strcmp(numeric, "auto")) {
        const char *a = intl_rtf_auto(unit, (int)val);
        if (a) { g_free(unit); g_free(numeric); return intl_str(ctx, a); }
    }
    double abs_val = fabs(val);
    char *out = g_strdup_printf("%s%g %s%s%s",
                                val >= 0 ? "in " : "", abs_val, unit,
                                abs_val != 1 ? "s" : "", val < 0 ? " ago" : "");
    JSValue r = JS_NewString(ctx, out);
    g_free(out); g_free(unit); g_free(numeric);
    return r;
}

static JSValue
intl_rtf_formatToParts(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    double val = 0;
    if (argc >= 1) JS_ToFloat64(ctx, &val, argv[0]);
    char *unit = intl_rtf_unit(ctx, argc >= 2 ? argv[1] : JS_UNDEFINED);
    char *numeric = intl_hget_str(ctx, this_val, "_numeric");
    JSValue arr = JS_NewArray(ctx);
    uint32_t n = 0;
    if (numeric && !strcmp(numeric, "auto")) {
        const char *a = intl_rtf_auto(unit, (int)val);
        if (a) {
            JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, "literal", a));
            g_free(unit); g_free(numeric);
            return arr;
        }
    }
    double abs_val = fabs(val);
    if (val >= 0) JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, "literal", "in "));
    char *iv = g_strdup_printf("%g", abs_val);
    JSValue ip = intl_part(ctx, "integer", iv);
    JS_SetPropertyStr(ctx, ip, "unit", intl_str(ctx, unit));
    JS_SetPropertyUint32(ctx, arr, n++, ip);
    g_free(iv);
    char *tail = g_strdup_printf(" %s%s%s", unit, abs_val != 1 ? "s" : "",
                                 val < 0 ? " ago" : "");
    JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, "literal", tail));
    g_free(tail); g_free(unit); g_free(numeric);
    return arr;
}

static JSValue
intl_rtf_resolved(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    char *loc = intl_hget_str(ctx, this_val, "_locale");
    char *numeric = intl_hget_str(ctx, this_val, "_numeric");
    char *style = intl_hget_str(ctx, this_val, "_style");
    JS_SetPropertyStr(ctx, o, "locale", intl_str(ctx, loc ? loc : "en-US"));
    JS_SetPropertyStr(ctx, o, "numeric", intl_str(ctx, numeric ? numeric : "always"));
    JS_SetPropertyStr(ctx, o, "style", intl_str(ctx, style ? style : "long"));
    JS_SetPropertyStr(ctx, o, "numberingSystem", intl_str(ctx, "latn"));
    g_free(loc); g_free(numeric); g_free(style);
    return o;
}

/* ---- Intl.DisplayNames -------------------------------------------------- */

static const char *
intl_region_name(const char *code)
{
    static const struct { const char *c, *n; } tab[] = {
        {"US","United States"},{"GB","United Kingdom"},{"FR","France"},
        {"DE","Germany"},{"ES","Spain"},{"IT","Italy"},{"NL","Netherlands"},
        {"NO","Norway"},{"SE","Sweden"},{"DK","Denmark"},{"FI","Finland"},
        {"JP","Japan"},{"CN","China"},{"KR","South Korea"},{"IN","India"},
        {"BR","Brazil"},{"CA","Canada"},{"AU","Australia"},{"RU","Russia"},
        {"MX","Mexico"},{"PT","Portugal"},{"PL","Poland"},{"CH","Switzerland"},
        {"BE","Belgium"},{"AT","Austria"},{"IE","Ireland"},{"NZ","New Zealand"},
        {"ZA","South Africa"},{"AR","Argentina"},{"GR","Greece"},{"TR","Turkey"},
    };
    for (gsize i = 0; i < G_N_ELEMENTS(tab); i++)
        if (!g_ascii_strcasecmp(code, tab[i].c)) return tab[i].n;
    return NULL;
}

static const char *
intl_language_name(const char *code)
{
    char base[16]; intl_lang_subtag(code, base, sizeof base);
    static const struct { const char *c, *n; } tab[] = {
        {"en","English"},{"fr","French"},{"de","German"},{"es","Spanish"},
        {"it","Italian"},{"nl","Dutch"},{"no","Norwegian"},{"nb","Norwegian Bokmål"},
        {"nn","Norwegian Nynorsk"},{"sv","Swedish"},{"da","Danish"},{"fi","Finnish"},
        {"ja","Japanese"},{"zh","Chinese"},{"ko","Korean"},{"ru","Russian"},
        {"pt","Portuguese"},{"pl","Polish"},{"ar","Arabic"},{"hi","Hindi"},
        {"tr","Turkish"},{"el","Greek"},{"cs","Czech"},{"uk","Ukrainian"},
        {"he","Hebrew"},{"th","Thai"},{"vi","Vietnamese"},{"id","Indonesian"},
    };
    for (gsize i = 0; i < G_N_ELEMENTS(tab); i++)
        if (!strcmp(base, tab[i].c)) return tab[i].n;
    return NULL;
}

static const char *
intl_script_name(const char *code)
{
    static const struct { const char *c, *n; } tab[] = {
        {"Latn","Latin"},{"Cyrl","Cyrillic"},{"Grek","Greek"},{"Arab","Arabic"},
        {"Hans","Simplified Han"},{"Hant","Traditional Han"},{"Jpan","Japanese"},
        {"Kore","Korean"},{"Hebr","Hebrew"},{"Deva","Devanagari"},{"Thai","Thai"},
    };
    for (gsize i = 0; i < G_N_ELEMENTS(tab); i++)
        if (!g_ascii_strcasecmp(code, tab[i].c)) return tab[i].n;
    return NULL;
}

static JSValue
intl_dn_ctor(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv)
{
    char *locale = intl_arg_locale(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED);
    JSValueConst o = argc >= 2 ? argv[1] : JS_UNDEFINED;
    JSValue obj = intl_new(ctx, this_val, "DisplayNames");
    char *type = intl_opt_str(ctx, o, "type");
    char *style = intl_opt_str(ctx, o, "style");
    char *fallback = intl_opt_str(ctx, o, "fallback");
    intl_hide(ctx, obj, "_locale", intl_str(ctx, locale));
    intl_hide(ctx, obj, "_type", intl_str(ctx, type ? type : "language"));
    intl_hide(ctx, obj, "_style", intl_str(ctx, style ? style : "long"));
    intl_hide(ctx, obj, "_fallback", intl_str(ctx, fallback ? fallback : "code"));
    g_free(locale); g_free(type); g_free(style); g_free(fallback);
    return obj;
}

static JSValue
intl_dn_of(JSContext *ctx, JSValueConst this_val,
           int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    const char *code = JS_ToCString(ctx, argv[0]);
    if (!code) return JS_UNDEFINED;
    char *type = intl_hget_str(ctx, this_val, "_type");
    char *fallback = intl_hget_str(ctx, this_val, "_fallback");
    const char *name = NULL;
    if (type && !strcmp(type, "region")) name = intl_region_name(code);
    else if (type && !strcmp(type, "script")) name = intl_script_name(code);
    else if (type && !strcmp(type, "currency")) name = NULL;
    else name = intl_language_name(code);

    JSValue out;
    if (name) out = intl_str(ctx, name);
    else if (fallback && !strcmp(fallback, "none")) out = JS_UNDEFINED;
    else out = intl_str(ctx, code);
    JS_FreeCString(ctx, code);
    g_free(type); g_free(fallback);
    return out;
}

static JSValue
intl_dn_resolved(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    char *loc = intl_hget_str(ctx, this_val, "_locale");
    char *type = intl_hget_str(ctx, this_val, "_type");
    char *style = intl_hget_str(ctx, this_val, "_style");
    char *fb = intl_hget_str(ctx, this_val, "_fallback");
    JS_SetPropertyStr(ctx, o, "locale", intl_str(ctx, loc ? loc : "en-US"));
    JS_SetPropertyStr(ctx, o, "type", intl_str(ctx, type ? type : "language"));
    JS_SetPropertyStr(ctx, o, "style", intl_str(ctx, style ? style : "long"));
    JS_SetPropertyStr(ctx, o, "fallback", intl_str(ctx, fb ? fb : "code"));
    g_free(loc); g_free(type); g_free(style); g_free(fb);
    return o;
}

/* ---- Intl.DurationFormat ------------------------------------------------ */

static JSValue
intl_df_ctor(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv)
{
    char *locale = intl_arg_locale(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED);
    JSValueConst o = argc >= 2 ? argv[1] : JS_UNDEFINED;
    JSValue obj = intl_new(ctx, this_val, "DurationFormat");
    char *style = intl_opt_str(ctx, o, "style");
    intl_hide(ctx, obj, "_locale", intl_str(ctx, locale));
    intl_hide(ctx, obj, "_style", intl_str(ctx, style ? style : "short"));
    g_free(locale); g_free(style);
    return obj;
}

static const struct { const char *field; const char *unit; } intl_dur_units[] = {
    {"years","year"},{"months","month"},{"weeks","week"},{"days","day"},
    {"hours","hour"},{"minutes","minute"},{"seconds","second"},
    {"milliseconds","millisecond"},{"microseconds","microsecond"},
    {"nanoseconds","nanosecond"},
};

static JSValue
intl_df_parts(JSContext *ctx, JSValueConst this_val, JSValueConst dur)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t n = 0;
    char *style = intl_hget_str(ctx, this_val, "_style");
    gboolean narrow = style && !strcmp(style, "narrow");
    gboolean digital = style && !strcmp(style, "digital");
    gboolean lng = style && !strcmp(style, "long");
    GPtrArray *segs = g_ptr_array_new_with_free_func(g_free);
    for (gsize i = 0; i < G_N_ELEMENTS(intl_dur_units); i++) {
        JSValue v = JS_GetPropertyStr(ctx, dur, intl_dur_units[i].field);
        if (JS_IsUndefined(v)) { JS_FreeValue(ctx, v); continue; }
        double d = 0; JS_ToFloat64(ctx, &d, v); JS_FreeValue(ctx, v);
        if (d == 0) continue;
        const char *u = intl_dur_units[i].unit;
        char *piece;
        if (narrow) {
            const char *abbr = !strcmp(u, "year") ? "y" : !strcmp(u, "month") ? "mo"
                : !strcmp(u, "week") ? "w" : !strcmp(u, "day") ? "d"
                : !strcmp(u, "hour") ? "h" : !strcmp(u, "minute") ? "m"
                : !strcmp(u, "second") ? "s" : !strcmp(u, "millisecond") ? "ms"
                : !strcmp(u, "microsecond") ? "\xc2\xb5s" : "ns";
            piece = g_strdup_printf("%g%s", d, abbr);
        } else if (lng) {
            piece = g_strdup_printf("%g %s%s", d, u, d != 1 ? "s" : "");
        } else {
            const char *abbr = !strcmp(u, "year") ? "yr" : !strcmp(u, "month") ? "mth"
                : !strcmp(u, "week") ? "wk" : !strcmp(u, "day") ? "day"
                : !strcmp(u, "hour") ? "hr" : !strcmp(u, "minute") ? "min"
                : !strcmp(u, "second") ? "sec" : !strcmp(u, "millisecond") ? "ms"
                : !strcmp(u, "microsecond") ? "\xc2\xb5s" : "ns";
            piece = g_strdup_printf("%g %s", d, abbr);
        }
        g_ptr_array_add(segs, piece);
    }
    const char *sep = (narrow || digital) ? " " : ", ";
    for (guint i = 0; i < segs->len; i++) {
        if (i > 0) JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, "literal", sep));
        JS_SetPropertyUint32(ctx, arr, n++,
                             intl_part(ctx, "element", g_ptr_array_index(segs, i)));
    }
    if (segs->len == 0)
        JS_SetPropertyUint32(ctx, arr, n++, intl_part(ctx, "element", "0 sec"));
    g_ptr_array_free(segs, TRUE);
    g_free(style);
    return arr;
}

static JSValue
intl_df_format(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv)
{
    JSValueConst dur = argc >= 1 ? argv[0] : JS_UNDEFINED;
    if (!JS_IsObject(dur)) return intl_str(ctx, "");
    return intl_join_parts(ctx, intl_df_parts(ctx, this_val, dur));
}

static JSValue
intl_df_formatToParts(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    JSValueConst dur = argc >= 1 ? argv[0] : JS_UNDEFINED;
    if (!JS_IsObject(dur)) return JS_NewArray(ctx);
    return intl_df_parts(ctx, this_val, dur);
}

static JSValue
intl_df_resolved(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    char *loc = intl_hget_str(ctx, this_val, "_locale");
    char *style = intl_hget_str(ctx, this_val, "_style");
    JS_SetPropertyStr(ctx, o, "locale", intl_str(ctx, loc ? loc : "en-US"));
    JS_SetPropertyStr(ctx, o, "style", intl_str(ctx, style ? style : "short"));
    JS_SetPropertyStr(ctx, o, "numberingSystem", intl_str(ctx, "latn"));
    g_free(loc); g_free(style);
    return o;
}

/* ---- Intl.Segmenter ----------------------------------------------------- */

static JSValue
intl_seg_ctor(JSContext *ctx, JSValueConst this_val,
              int argc, JSValueConst *argv)
{
    char *locale = intl_arg_locale(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED);
    JSValueConst o = argc >= 2 ? argv[1] : JS_UNDEFINED;
    JSValue obj = intl_new(ctx, this_val, "Segmenter");
    char *gran = intl_opt_str(ctx, o, "granularity");
    intl_hide(ctx, obj, "_locale", intl_str(ctx, locale));
    intl_hide(ctx, obj, "_granularity", intl_str(ctx, gran ? gran : "grapheme"));
    g_free(locale); g_free(gran);
    return obj;
}

static JSValue
intl_seg_containing(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    int32_t idx = 0;
    if (argc >= 1) JS_ToInt32(ctx, &idx, argv[0]);
    JSValue lv = JS_GetPropertyStr(ctx, this_val, "length");
    uint32_t len = 0; JS_ToUint32(ctx, &len, lv); JS_FreeValue(ctx, lv);
    for (uint32_t i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, this_val, i);
        JSValue si = JS_GetPropertyStr(ctx, e, "index");
        JSValue sg = JS_GetPropertyStr(ctx, e, "segment");
        int32_t start = 0; JS_ToInt32(ctx, &start, si);
        const char *seg = JS_ToCString(ctx, sg);
        int32_t slen = seg ? (int32_t)g_utf8_strlen(seg, -1) : 0;
        if (seg) JS_FreeCString(ctx, seg);
        JS_FreeValue(ctx, si); JS_FreeValue(ctx, sg);
        if (idx >= start && idx < start + slen) return e;
        JS_FreeValue(ctx, e);
    }
    return JS_UNDEFINED;
}

static JSValue
intl_seg_segment(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    const char *input = JS_ToCString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED);
    if (!input) return JS_EXCEPTION;
    char *gran = intl_hget_str(ctx, this_val, "_granularity");
    gboolean words = gran && !strcmp(gran, "word");
    gboolean sentences = gran && !strcmp(gran, "sentence");
    char *locale = intl_hget_str(ctx, this_val, "_locale");
    PangoLanguage *lang = (locale && *locale)
        ? pango_language_from_string(locale) : NULL;

    glong nchars = g_utf8_strlen(input, -1);
    JSValue arr = JS_NewArray(ctx);
    uint32_t out = 0;

    if (nchars > 0) {
        PangoLogAttr *attrs = g_new0(PangoLogAttr, nchars + 1);
        pango_get_log_attrs(input, (int)strlen(input), -1, lang,
                            attrs, (int)(nchars + 1));
        glong seg_start = 0;
        const char *p = input;
        const char *seg_ptr = input;
        for (glong i = 1; i <= nchars; i++) {
            p = g_utf8_next_char(p);
            gboolean boundary;
            if (i == nchars) boundary = TRUE;
            else if (words) boundary = attrs[i].is_word_start || attrs[i].is_word_end;
            else if (sentences) boundary = attrs[i].is_sentence_boundary;
            else boundary = attrs[i].is_cursor_position;
            if (boundary) {
                JSValue seg = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, seg, "segment",
                                  JS_NewStringLen(ctx, seg_ptr, (size_t)(p - seg_ptr)));
                JS_SetPropertyStr(ctx, seg, "index", JS_NewInt32(ctx, (int32_t)seg_start));
                JS_SetPropertyStr(ctx, seg, "input", intl_str(ctx, input));
                if (words) {
                    gunichar c0 = g_utf8_get_char(seg_ptr);
                    JS_SetPropertyStr(ctx, seg, "isWordLike",
                                      JS_NewBool(ctx, g_unichar_isalnum(c0)));
                }
                JS_SetPropertyUint32(ctx, arr, out++, seg);
                seg_ptr = p;
                seg_start = i;
            }
        }
        g_free(attrs);
    }
    intl_bind(ctx, arr, "containing", intl_seg_containing, 1);
    JS_FreeCString(ctx, input);
    g_free(gran);
    g_free(locale);
    return arr;
}

/* ---- toLocale* prototype hooks ----------------------------------------- */

static JSValue
intl_number_toLocaleString(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    double num = 0;
    JS_ToFloat64(ctx, &num, this_val);
    JSValue glob = JS_GetGlobalObject(ctx);
    JSValue intl = JS_GetPropertyStr(ctx, glob, "Intl");
    JSValue ctor = JS_GetPropertyStr(ctx, intl, "NumberFormat");
    JSValueConst args[2] = {
        argc >= 1 ? argv[0] : JS_UNDEFINED,
        argc >= 2 ? argv[1] : JS_UNDEFINED,
    };
    JSValue fmt = JS_CallConstructor(ctx, ctor, 2, args);
    JSValue r = intl_join_parts(ctx, intl_nf_parts(ctx, fmt, num));
    JS_FreeValue(ctx, fmt); JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, intl); JS_FreeValue(ctx, glob);
    return r;
}

static JSValue
intl_date_format_with(JSContext *ctx, JSValueConst this_val,
                      JSValueConst locales, JSValueConst options)
{
    double ms;
    if (JS_ToFloat64(ctx, &ms, this_val) != 0) ms = NAN;
    JSValue glob = JS_GetGlobalObject(ctx);
    JSValue intl = JS_GetPropertyStr(ctx, glob, "Intl");
    JSValue ctor = JS_GetPropertyStr(ctx, intl, "DateTimeFormat");
    JSValueConst args[2] = { locales, options };
    JSValue fmt = JS_CallConstructor(ctx, ctor, 2, args);
    JSValue r = intl_join_parts(ctx, intl_dtf_parts(ctx, fmt, ms));
    JS_FreeValue(ctx, fmt); JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, intl); JS_FreeValue(ctx, glob);
    return r;
}

static JSValue
intl_date_toLocaleString(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    JSValueConst locales = argc >= 1 ? argv[0] : JS_UNDEFINED;
    JSValue options;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        options = JS_DupValue(ctx, argv[1]);
    } else {
        options = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, options, "year", intl_str(ctx, "numeric"));
        JS_SetPropertyStr(ctx, options, "month", intl_str(ctx, "numeric"));
        JS_SetPropertyStr(ctx, options, "day", intl_str(ctx, "numeric"));
        JS_SetPropertyStr(ctx, options, "hour", intl_str(ctx, "numeric"));
        JS_SetPropertyStr(ctx, options, "minute", intl_str(ctx, "2-digit"));
        JS_SetPropertyStr(ctx, options, "second", intl_str(ctx, "2-digit"));
    }
    JSValue r = intl_date_format_with(ctx, this_val, locales, options);
    JS_FreeValue(ctx, options);
    return r;
}

static JSValue
intl_date_toLocaleDateString(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    JSValueConst locales = argc >= 1 ? argv[0] : JS_UNDEFINED;
    JSValue options;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        options = JS_DupValue(ctx, argv[1]);
    } else {
        options = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, options, "year", intl_str(ctx, "numeric"));
        JS_SetPropertyStr(ctx, options, "month", intl_str(ctx, "numeric"));
        JS_SetPropertyStr(ctx, options, "day", intl_str(ctx, "numeric"));
    }
    JSValue r = intl_date_format_with(ctx, this_val, locales, options);
    JS_FreeValue(ctx, options);
    return r;
}

static JSValue
intl_date_toLocaleTimeString(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    JSValueConst locales = argc >= 1 ? argv[0] : JS_UNDEFINED;
    JSValue options;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        options = JS_DupValue(ctx, argv[1]);
    } else {
        options = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, options, "hour", intl_str(ctx, "numeric"));
        JS_SetPropertyStr(ctx, options, "minute", intl_str(ctx, "2-digit"));
        JS_SetPropertyStr(ctx, options, "second", intl_str(ctx, "2-digit"));
    }
    JSValue r = intl_date_format_with(ctx, this_val, locales, options);
    JS_FreeValue(ctx, options);
    return r;
}

static JSValue
intl_string_localeCompare(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    const char *a = JS_ToCString(ctx, this_val);
    const char *b = argc >= 1 ? JS_ToCString(ctx, argv[0]) : NULL;
    char *sens = NULL;
    int numeric = 0;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        sens = intl_opt_str(ctx, argv[2], "sensitivity");
        numeric = intl_opt_bool(ctx, argv[2], "numeric", 0);
    }
    int r;
    if (numeric) {
        r = intl_numeric_compare(a ? a : "", b ? b : "");
    } else {
        char *ka = intl_collation_key(a ? a : "", sens ? sens : "variant");
        char *kb = intl_collation_key(b ? b : "", sens ? sens : "variant");
        r = strcmp(ka, kb);
        if (r == 0) r = g_utf8_collate(a ? a : "", b ? b : "");
        r = r < 0 ? -1 : r > 0 ? 1 : 0;
        g_free(ka); g_free(kb);
    }
    g_free(sens);
    if (a) JS_FreeCString(ctx, a);
    if (b) JS_FreeCString(ctx, b);
    return JS_NewInt32(ctx, r);
}

static void
intl_bind_bound(JSContext *ctx, JSValueConst obj, const char *name,
                JSCFunctionData *fn, int argc, JSValueConst instance)
{
    JSValue f = JS_NewCFunctionData(ctx, fn, argc, 0, 1, &instance);
    JS_DefinePropertyValueStr(ctx, obj, name, f,
                              JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
}

static JSValue
intl_collator_compare_b(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv, int magic, JSValueConst *data)
{
    (void)this_val; (void)magic;
    return intl_collator_compare(ctx, data[0], argc, argv);
}

static JSValue
intl_nf_format_b(JSContext *ctx, JSValueConst this_val, int argc,
                 JSValueConst *argv, int magic, JSValueConst *data)
{
    (void)this_val; (void)magic;
    return intl_nf_format(ctx, data[0], argc, argv);
}

static JSValue
intl_dtf_format_b(JSContext *ctx, JSValueConst this_val, int argc,
                  JSValueConst *argv, int magic, JSValueConst *data)
{
    (void)this_val; (void)magic;
    return intl_dtf_format(ctx, data[0], argc, argv);
}

static void
intl_install_proto_hook(JSContext *ctx, JSValueConst global, const char *ctor,
                        const char *method, JSCFunction *fn, int argc)
{
    JSValue c = JS_GetPropertyStr(ctx, global, ctor);
    JSValue proto = JS_GetPropertyStr(ctx, c, "prototype");
    if (JS_IsObject(proto))
        intl_bind(ctx, proto, method, fn, argc);
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, c);
}

/* ---- install ------------------------------------------------------------ */

void
ns_js_intl_install(JSContext *ctx, JSValueConst global)
{
    JSAtom atom = JS_NewAtom(ctx, "Intl");
    int has = JS_HasProperty(ctx, global, atom);
    JS_FreeAtom(ctx, atom);
    if (has > 0) return;

    JSValue intl = JS_NewObject(ctx);

    static const intl_method collator_m[] = {
        { "compare", intl_collator_compare, 2 },
        { "resolvedOptions", intl_collator_resolved, 0 },
    };
    static const intl_method nf_m[] = {
        { "format", intl_nf_format, 1 },
        { "formatToParts", intl_nf_formatToParts, 1 },
        { "formatRange", intl_nf_formatRange, 2 },
        { "formatRangeToParts", intl_nf_formatToParts, 2 },
        { "resolvedOptions", intl_nf_resolved, 0 },
    };
    static const intl_method dtf_m[] = {
        { "format", intl_dtf_format, 1 },
        { "formatToParts", intl_dtf_formatToParts, 1 },
        { "formatRange", intl_dtf_formatRange, 2 },
        { "formatRangeToParts", intl_dtf_formatToParts, 2 },
        { "resolvedOptions", intl_dtf_resolved, 0 },
    };
    static const intl_method pr_m[] = {
        { "select", intl_pr_select, 1 },
        { "selectRange", intl_pr_selectRange, 2 },
        { "resolvedOptions", intl_pr_resolved, 0 },
    };
    static const intl_method lf_m[] = {
        { "format", intl_lf_format, 1 },
        { "formatToParts", intl_lf_formatToParts, 1 },
        { "resolvedOptions", intl_lf_resolved, 0 },
    };
    static const intl_method rtf_m[] = {
        { "format", intl_rtf_format, 2 },
        { "formatToParts", intl_rtf_formatToParts, 2 },
        { "resolvedOptions", intl_rtf_resolved, 0 },
    };
    static const intl_method dn_m[] = {
        { "of", intl_dn_of, 1 },
        { "resolvedOptions", intl_dn_resolved, 0 },
    };
    static const intl_method df_m[] = {
        { "format", intl_df_format, 1 },
        { "formatToParts", intl_df_formatToParts, 1 },
        { "resolvedOptions", intl_df_resolved, 0 },
    };
    static const intl_method seg_m[] = {
        { "segment", intl_seg_segment, 1 },
    };
    static const intl_method locale_m[] = {
        { "toString", intl_locale_toString, 0 },
        { "maximize", intl_locale_identity, 0 },
        { "minimize", intl_locale_identity, 0 },
    };

    JSValue c;
    c = intl_register(ctx, intl, "Collator", intl_collator_ctor, 0,
                      collator_m, G_N_ELEMENTS(collator_m)); JS_FreeValue(ctx, c);
    c = intl_register(ctx, intl, "NumberFormat", intl_nf_ctor, 0,
                      nf_m, G_N_ELEMENTS(nf_m)); JS_FreeValue(ctx, c);
    c = intl_register(ctx, intl, "DateTimeFormat", intl_dtf_ctor, 0,
                      dtf_m, G_N_ELEMENTS(dtf_m)); JS_FreeValue(ctx, c);
    c = intl_register(ctx, intl, "PluralRules", intl_pr_ctor, 0,
                      pr_m, G_N_ELEMENTS(pr_m)); JS_FreeValue(ctx, c);
    c = intl_register(ctx, intl, "ListFormat", intl_lf_ctor, 0,
                      lf_m, G_N_ELEMENTS(lf_m)); JS_FreeValue(ctx, c);
    c = intl_register(ctx, intl, "RelativeTimeFormat", intl_rtf_ctor, 0,
                      rtf_m, G_N_ELEMENTS(rtf_m)); JS_FreeValue(ctx, c);
    c = intl_register(ctx, intl, "DisplayNames", intl_dn_ctor, 0,
                      dn_m, G_N_ELEMENTS(dn_m)); JS_FreeValue(ctx, c);
    c = intl_register(ctx, intl, "DurationFormat", intl_df_ctor, 0,
                      df_m, G_N_ELEMENTS(df_m)); JS_FreeValue(ctx, c);
    c = intl_register(ctx, intl, "Segmenter", intl_seg_ctor, 0,
                      seg_m, G_N_ELEMENTS(seg_m)); JS_FreeValue(ctx, c);
    c = intl_register(ctx, intl, "Locale", intl_locale_ctor, 1,
                      locale_m, G_N_ELEMENTS(locale_m)); JS_FreeValue(ctx, c);

    intl_bind(ctx, intl, "getCanonicalLocales", intl_getCanonicalLocales, 1);
    intl_bind(ctx, intl, "supportedValuesOf", intl_supportedValuesOf, 1);

    JS_SetPropertyStr(ctx, global, "Intl", intl);

    intl_install_proto_hook(ctx, global, "Number", "toLocaleString",
                            intl_number_toLocaleString, 0);
    intl_install_proto_hook(ctx, global, "Date", "toLocaleString",
                            intl_date_toLocaleString, 0);
    intl_install_proto_hook(ctx, global, "Date", "toLocaleDateString",
                            intl_date_toLocaleDateString, 0);
    intl_install_proto_hook(ctx, global, "Date", "toLocaleTimeString",
                            intl_date_toLocaleTimeString, 0);
    intl_install_proto_hook(ctx, global, "String", "localeCompare",
                            intl_string_localeCompare, 1);
}
