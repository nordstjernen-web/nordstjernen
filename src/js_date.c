/* Nordstjernen — native Temporal date/time API over QuickJS, ICU-free. */

#include "js_date.h"
#include "js_classid.h"

#include <math.h>
#include <string.h>
#include <time.h>
#include <glib.h>

#include "datetime.h"

enum {
    TK_INSTANT, TK_PLAINDATE, TK_PLAINTIME, TK_PLAINDATETIME,
    TK_PLAINYEARMONTH, TK_PLAINMONTHDAY, TK_ZONEDDATETIME, TK_DURATION,
};

typedef struct {
    int kind;
    int64_t epoch_sec;
    int32_t nanos;
    int year, month, day;
    int hour, minute, second, ms, us, ns;
    int64_t dur[10];
    char *tz;
} ns_temporal;

static JSClassID ns_temporal_class_id;

static void
ns_temporal_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_temporal *t = JS_GetOpaque(val, ns_temporal_class_id);
    if (!t) return;
    g_free(t->tz);
    g_free(t);
}

static JSClassDef ns_temporal_class = {
    "Temporal", .finalizer = ns_temporal_finalizer,
};

static ns_temporal *
tmp_this(JSContext *ctx, JSValueConst v, int kind)
{
    ns_temporal *t = JS_GetOpaque(v, ns_temporal_class_id);
    if (!t || (kind >= 0 && t->kind != kind)) {
        JS_ThrowTypeError(ctx, "invalid Temporal receiver");
        return NULL;
    }
    return t;
}

static ns_temporal *
tmp_alloc(JSContext *ctx, JSValueConst this_val, int kind, JSValue *out)
{
    JSValue proto = JS_GetPropertyStr(ctx, this_val, "prototype");
    JSValue obj = JS_IsObject(proto)
        ? JS_NewObjectProtoClass(ctx, proto, ns_temporal_class_id)
        : JS_NewObjectClass(ctx, ns_temporal_class_id);
    JS_FreeValue(ctx, proto);
    ns_temporal *t = g_new0(ns_temporal, 1);
    t->kind = kind;
    JS_SetOpaque(obj, t);
    *out = obj;
    return t;
}

static JSValue
tmp_make(JSContext *ctx, JSValueConst global_or_proto, int kind,
         ns_temporal **out)
{
    JSValue obj = JS_NewObjectProtoClass(ctx, global_or_proto,
                                         ns_temporal_class_id);
    ns_temporal *t = g_new0(ns_temporal, 1);
    t->kind = kind;
    JS_SetOpaque(obj, t);
    *out = t;
    return obj;
}

static int
tmp_int_prop(JSContext *ctx, JSValueConst o, const char *key, int dflt)
{
    JSValue v = JS_GetPropertyStr(ctx, o, key);
    int r = dflt;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        int32_t t; if (JS_ToInt32(ctx, &t, v) == 0) r = t;
    }
    JS_FreeValue(ctx, v);
    return r;
}

static gboolean
tmp_has_prop(JSContext *ctx, JSValueConst o, const char *key)
{
    if (!JS_IsObject(o)) return FALSE;
    JSValue v = JS_GetPropertyStr(ctx, o, key);
    gboolean p = !JS_IsUndefined(v);
    JS_FreeValue(ctx, v);
    return p;
}

static void
tmp_def(JSContext *ctx, JSValueConst obj, const char *key, JSValue val)
{
    JS_DefinePropertyValueStr(ctx, obj, key, val, JS_PROP_ENUMERABLE);
}

static const char *
tmp_month_code(int m, char *buf)
{
    g_snprintf(buf, 8, "M%02d", m);
    return buf;
}

static int
tmp_iso_dow(int y, int m, int d)
{
    long days = ns_dt_days_from_civil(y, m, d);
    int dow = (int)ns_dt_floormod(days + 4, 7);
    return dow == 0 ? 7 : dow;
}

static int
tmp_day_of_year(int y, int m, int d)
{
    return (int)(ns_dt_days_from_civil(y, m, d) -
                 ns_dt_days_from_civil(y, 1, 1)) + 1;
}

static int
tmp_days_in_year(int y)
{
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
}

static int
tmp_week_of_year(int y, int m, int d)
{
    long ord = ns_dt_days_from_civil(y, m, d);
    long w1 = ns_dt_iso_week1_monday(y);
    if (ord < w1) { y -= 1; w1 = ns_dt_iso_week1_monday(y); }
    long next = ns_dt_iso_week1_monday(y + 1);
    if (ord >= next) return 1;
    return (int)((ord - w1) / 7) + 1;
}

/* ---- field population --------------------------------------------------- */

static void
tmp_fill_date(JSContext *ctx, JSValueConst obj, ns_temporal *t)
{
    char mc[8];
    tmp_def(ctx, obj, "year", JS_NewInt32(ctx, t->year));
    tmp_def(ctx, obj, "month", JS_NewInt32(ctx, t->month));
    tmp_def(ctx, obj, "monthCode", JS_NewString(ctx, tmp_month_code(t->month, mc)));
    tmp_def(ctx, obj, "day", JS_NewInt32(ctx, t->day));
    tmp_def(ctx, obj, "calendarId", JS_NewString(ctx, "iso8601"));
    tmp_def(ctx, obj, "dayOfWeek", JS_NewInt32(ctx, tmp_iso_dow(t->year, t->month, t->day)));
    tmp_def(ctx, obj, "dayOfYear", JS_NewInt32(ctx, tmp_day_of_year(t->year, t->month, t->day)));
    tmp_def(ctx, obj, "weekOfYear", JS_NewInt32(ctx, tmp_week_of_year(t->year, t->month, t->day)));
    tmp_def(ctx, obj, "daysInWeek", JS_NewInt32(ctx, 7));
    tmp_def(ctx, obj, "daysInMonth", JS_NewInt32(ctx, ns_dt_days_in_month(t->year, t->month)));
    tmp_def(ctx, obj, "daysInYear", JS_NewInt32(ctx, tmp_days_in_year(t->year)));
    tmp_def(ctx, obj, "monthsInYear", JS_NewInt32(ctx, 12));
    tmp_def(ctx, obj, "inLeapYear", JS_NewBool(ctx, tmp_days_in_year(t->year) == 366));
}

static void
tmp_fill_time(JSContext *ctx, JSValueConst obj, ns_temporal *t)
{
    tmp_def(ctx, obj, "hour", JS_NewInt32(ctx, t->hour));
    tmp_def(ctx, obj, "minute", JS_NewInt32(ctx, t->minute));
    tmp_def(ctx, obj, "second", JS_NewInt32(ctx, t->second));
    tmp_def(ctx, obj, "millisecond", JS_NewInt32(ctx, t->ms));
    tmp_def(ctx, obj, "microsecond", JS_NewInt32(ctx, t->us));
    tmp_def(ctx, obj, "nanosecond", JS_NewInt32(ctx, t->ns));
}

static void
tmp_fill(JSContext *ctx, JSValueConst obj, ns_temporal *t)
{
    char mc[8];
    if (t->year < -NS_DT_MAX_YEAR) t->year = -NS_DT_MAX_YEAR;
    if (t->year > NS_DT_MAX_YEAR) t->year = NS_DT_MAX_YEAR;
    switch (t->kind) {
    case TK_PLAINDATE:
        tmp_fill_date(ctx, obj, t);
        break;
    case TK_PLAINTIME:
        tmp_fill_time(ctx, obj, t);
        break;
    case TK_PLAINDATETIME:
        tmp_fill_date(ctx, obj, t);
        tmp_fill_time(ctx, obj, t);
        break;
    case TK_PLAINYEARMONTH:
        tmp_def(ctx, obj, "year", JS_NewInt32(ctx, t->year));
        tmp_def(ctx, obj, "month", JS_NewInt32(ctx, t->month));
        tmp_def(ctx, obj, "monthCode", JS_NewString(ctx, tmp_month_code(t->month, mc)));
        tmp_def(ctx, obj, "calendarId", JS_NewString(ctx, "iso8601"));
        tmp_def(ctx, obj, "daysInMonth", JS_NewInt32(ctx, ns_dt_days_in_month(t->year, t->month)));
        tmp_def(ctx, obj, "daysInYear", JS_NewInt32(ctx, tmp_days_in_year(t->year)));
        tmp_def(ctx, obj, "monthsInYear", JS_NewInt32(ctx, 12));
        tmp_def(ctx, obj, "inLeapYear", JS_NewBool(ctx, tmp_days_in_year(t->year) == 366));
        break;
    case TK_PLAINMONTHDAY:
        tmp_def(ctx, obj, "monthCode", JS_NewString(ctx, tmp_month_code(t->month, mc)));
        tmp_def(ctx, obj, "day", JS_NewInt32(ctx, t->day));
        tmp_def(ctx, obj, "calendarId", JS_NewString(ctx, "iso8601"));
        break;
    case TK_INSTANT:
        tmp_def(ctx, obj, "epochMilliseconds",
                JS_NewInt64(ctx, t->epoch_sec * 1000 + t->nanos / 1000000));
        tmp_def(ctx, obj, "epochNanoseconds",
                JS_NewBigInt64(ctx, t->epoch_sec * 1000000000LL + t->nanos));
        break;
    case TK_ZONEDDATETIME:
        tmp_def(ctx, obj, "timeZoneId", JS_NewString(ctx, t->tz ? t->tz : "UTC"));
        tmp_def(ctx, obj, "calendarId", JS_NewString(ctx, "iso8601"));
        tmp_def(ctx, obj, "epochMilliseconds",
                JS_NewInt64(ctx, t->epoch_sec * 1000 + t->nanos / 1000000));
        tmp_def(ctx, obj, "epochNanoseconds",
                JS_NewBigInt64(ctx, t->epoch_sec * 1000000000LL + t->nanos));
        break;
    case TK_DURATION: {
        static const char *const names[] = {
            "years","months","weeks","days","hours","minutes",
            "seconds","milliseconds","microseconds","nanoseconds" };
        int sign = 0;
        for (int i = 0; i < 10; i++) {
            tmp_def(ctx, obj, names[i], JS_NewInt64(ctx, t->dur[i]));
            if (sign == 0 && t->dur[i] != 0) sign = t->dur[i] > 0 ? 1 : -1;
        }
        tmp_def(ctx, obj, "sign", JS_NewInt32(ctx, sign));
        tmp_def(ctx, obj, "blank", JS_NewBool(ctx, sign == 0));
        break;
    }
    }
}

/* ---- ISO breakdown for Instant/Zoned ----------------------------------- */

static void
tmp_breakdown(int64_t epoch_sec, int *y, int *mo, int *d,
              int *h, int *mi, int *s)
{
    long days = (long)((epoch_sec >= 0 ? epoch_sec : epoch_sec - 86399) / 86400);
    long rem = (long)(epoch_sec - (int64_t)days * 86400);
    ns_dt_civil_from_days(days, y, mo, d);
    *h = (int)(rem / 3600);
    *mi = (int)((rem % 3600) / 60);
    *s = (int)(rem % 60);
}

static int64_t
tmp_epoch_of(int y, int mo, int d, int h, int mi, int s)
{
    return (int64_t)ns_dt_days_from_civil(y, mo, d) * 86400 +
           h * 3600 + mi * 60 + s;
}

/* ---- toString helpers --------------------------------------------------- */

static void
tmp_append_frac(GString *str, int ms, int us, int ns)
{
    int64_t frac = (int64_t)ms * 1000000 + (int64_t)us * 1000 + ns;
    if (frac == 0) return;
    char buf[16];
    g_snprintf(buf, sizeof buf, "%09" G_GINT64_FORMAT, frac);
    int len = 9;
    while (len > 0 && buf[len - 1] == '0') len--;
    g_string_append_c(str, '.');
    g_string_append_len(str, buf, len);
}

/* ---- parsing ------------------------------------------------------------ */

static gboolean
tmp_parse_datetime(const char *s, int *y, int *mo, int *d,
                   int *h, int *mi, int *sec, int *ms, int *us, int *ns,
                   int *off_min, gboolean *has_off, gboolean *has_time)
{
    *h = *mi = *sec = *ms = *us = *ns = 0;
    *off_min = 0; *has_off = FALSE; *has_time = FALSE;
    const char *p = ns_dt_rd_date(s, y, mo, d);
    if (!p) return FALSE;
    if (*p == 'T' || *p == 't' || *p == ' ') {
        p++;
        int hh, mm, ss = 0;
        const char *q = ns_dt_rd_digits(p, 2, 2, &hh);
        if (!q || *q != ':') return TRUE;
        q++;
        q = ns_dt_rd_digits(q, 2, 2, &mm);
        if (!q) return FALSE;
        *h = hh; *mi = mm; *has_time = TRUE;
        p = q;
        if (*p == ':') {
            p++;
            p = ns_dt_rd_digits(p, 2, 2, &ss);
            if (!p) return FALSE;
            *sec = ss;
            if (*p == '.' || *p == ',') {
                p++;
                int v = 0, c = 0;
                while (*p >= '0' && *p <= '9' && c < 9) {
                    v = v * 10 + (*p - '0'); p++; c++;
                }
                while (c < 9) { v *= 10; c++; }
                while (*p >= '0' && *p <= '9') p++;
                *ms = v / 1000000;
                *us = (v / 1000) % 1000;
                *ns = v % 1000;
            }
        }
    }
    if (*p == 'Z' || *p == 'z') { *has_off = TRUE; *off_min = 0; }
    else if (*p == '+' || *p == '-') {
        int sgn = (*p == '-') ? -1 : 1; p++;
        int oh, om = 0;
        const char *q = ns_dt_rd_digits(p, 2, 2, &oh);
        if (q) {
            if (*q == ':') q++;
            ns_dt_rd_digits(q, 2, 2, &om);
            *off_min = sgn * (oh * 60 + om);
            *has_off = TRUE;
        }
    }
    return TRUE;
}

/* ---- Instant ------------------------------------------------------------ */

static JSValue
tmp_instant_from(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "Instant.from requires an argument");
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_INSTANT, &out);
    if (JS_IsString(argv[0])) {
        const char *s = JS_ToCString(ctx, argv[0]);
        int y, mo, d, h, mi, se, ms, us, ns, off; gboolean ho, ht;
        gboolean ok = s && tmp_parse_datetime(s, &y, &mo, &d, &h, &mi, &se,
                                              &ms, &us, &ns, &off, &ho, &ht);
        if (s) JS_FreeCString(ctx, s);
        if (!ok) {
            JS_FreeValue(ctx, out);
            return JS_ThrowRangeError(ctx, "invalid Instant string");
        }
        t->epoch_sec = tmp_epoch_of(y, mo, d, h, mi, se) - off * 60;
        t->nanos = ms * 1000000 + us * 1000 + ns;
    } else {
        ns_temporal *src = JS_GetOpaque(argv[0], ns_temporal_class_id);
        if (src && (src->kind == TK_INSTANT || src->kind == TK_ZONEDDATETIME)) {
            t->epoch_sec = src->epoch_sec; t->nanos = src->nanos;
        }
    }
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_instant_fromEpochMilliseconds(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    int64_t ms = 0;
    if (argc >= 1) JS_ToInt64(ctx, &ms, argv[0]);
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_INSTANT, &out);
    t->epoch_sec = (ms >= 0 ? ms : ms - 999) / 1000;
    t->nanos = (int32_t)((ms - t->epoch_sec * 1000) * 1000000);
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_instant_fromEpochNanoseconds(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    int64_t ns = 0;
    if (argc >= 1) JS_ToBigInt64(ctx, &ns, argv[0]);
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_INSTANT, &out);
    t->epoch_sec = (ns >= 0 ? ns : ns - 999999999) / 1000000000LL;
    t->nanos = (int32_t)(ns - t->epoch_sec * 1000000000LL);
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_instant_ctor(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv)
{
    int64_t ns = 0;
    if (argc >= 1) JS_ToBigInt64(ctx, &ns, argv[0]);
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_INSTANT, &out);
    t->epoch_sec = (ns >= 0 ? ns : ns - 999999999) / 1000000000LL;
    t->nanos = (int32_t)(ns - t->epoch_sec * 1000000000LL);
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_instant_toString(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_temporal *t = tmp_this(ctx, this_val, TK_INSTANT);
    if (!t) return JS_EXCEPTION;
    int y, mo, d, h, mi, s;
    tmp_breakdown(t->epoch_sec, &y, &mo, &d, &h, &mi, &s);
    GString *str = g_string_new(NULL);
    g_string_append_printf(str, "%04d-%02d-%02dT%02d:%02d:%02d", y, mo, d, h, mi, s);
    tmp_append_frac(str, t->nanos / 1000000, (t->nanos / 1000) % 1000, t->nanos % 1000);
    g_string_append_c(str, 'Z');
    JSValue r = JS_NewStringLen(ctx, str->str, str->len);
    g_string_free(str, TRUE);
    return r;
}

static int64_t
tmp_dur_time_ns(const ns_temporal *dur)
{
    return dur->dur[3] * 86400000000000LL +
           dur->dur[4] * 3600000000000LL +
           dur->dur[5] * 60000000000LL +
           dur->dur[6] * 1000000000LL +
           dur->dur[7] * 1000000LL +
           dur->dur[8] * 1000LL +
           dur->dur[9];
}

static JSValue
tmp_instant_add_impl(JSContext *ctx, JSValueConst this_val,
                     JSValueConst durv, int sign)
{
    ns_temporal *t = tmp_this(ctx, this_val, TK_INSTANT);
    if (!t) return JS_EXCEPTION;
    ns_temporal *dur = JS_GetOpaque(durv, ns_temporal_class_id);
    int64_t add_ns = 0;
    if (dur && dur->kind == TK_DURATION) add_ns = tmp_dur_time_ns(dur);
    else if (JS_IsObject(durv)) {
        add_ns = (int64_t)tmp_int_prop(ctx, durv, "hours", 0) * 3600000000000LL +
                 (int64_t)tmp_int_prop(ctx, durv, "minutes", 0) * 60000000000LL +
                 (int64_t)tmp_int_prop(ctx, durv, "seconds", 0) * 1000000000LL +
                 (int64_t)tmp_int_prop(ctx, durv, "milliseconds", 0) * 1000000LL +
                 (int64_t)tmp_int_prop(ctx, durv, "microseconds", 0) * 1000LL +
                 (int64_t)tmp_int_prop(ctx, durv, "nanoseconds", 0);
    }
    int64_t total = t->epoch_sec * 1000000000LL + t->nanos + sign * add_ns;
    JSValue gp = JS_GetPropertyStr(ctx, this_val, "constructor");
    JSValue proto = JS_GetPropertyStr(ctx, gp, "prototype");
    ns_temporal *nt; JSValue out = tmp_make(ctx, proto, TK_INSTANT, &nt);
    JS_FreeValue(ctx, proto); JS_FreeValue(ctx, gp);
    nt->epoch_sec = (total >= 0 ? total : total - 999999999) / 1000000000LL;
    nt->nanos = (int32_t)(total - nt->epoch_sec * 1000000000LL);
    tmp_fill(ctx, out, nt);
    return out;
}

static JSValue
tmp_instant_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return tmp_instant_add_impl(ctx, this_val, argc >= 1 ? argv[0] : JS_UNDEFINED, 1);
}

static JSValue
tmp_instant_subtract(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return tmp_instant_add_impl(ctx, this_val, argc >= 1 ? argv[0] : JS_UNDEFINED, -1);
}

/* ---- PlainDate / PlainDateTime / PlainTime ----------------------------- */

static void
tmp_set_date_fields(ns_temporal *t, int y, int mo, int d)
{
    if (mo < 1) mo = 1;
    if (mo > 12) mo = 12;
    int dim = ns_dt_days_in_month(y, mo);
    if (d < 1) d = 1;
    if (d > dim) d = dim;
    t->year = y; t->month = mo; t->day = d;
}

static JSValue
tmp_date_from(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_PLAINDATE, &out);
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char *s = JS_ToCString(ctx, argv[0]);
        int y = 1970, mo = 1, d = 1;
        gboolean ok = s && ns_dt_rd_date(s, &y, &mo, &d) != NULL;
        if (s) JS_FreeCString(ctx, s);
        if (!ok) {
            JS_FreeValue(ctx, out);
            return JS_ThrowRangeError(ctx, "invalid PlainDate string");
        }
        tmp_set_date_fields(t, y, mo, d);
    } else if (argc >= 1 && JS_IsObject(argv[0])) {
        ns_temporal *src = JS_GetOpaque(argv[0], ns_temporal_class_id);
        if (src && (src->kind == TK_PLAINDATE || src->kind == TK_PLAINDATETIME))
            tmp_set_date_fields(t, src->year, src->month, src->day);
        else
            tmp_set_date_fields(t,
                tmp_int_prop(ctx, argv[0], "year", 1970),
                tmp_int_prop(ctx, argv[0], "month", 1),
                tmp_int_prop(ctx, argv[0], "day", 1));
    }
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_date_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_PLAINDATE, &out);
    int y = argc >= 1 ? 0 : 1970, mo = 1, d = 1;
    if (argc >= 1) { int32_t v; JS_ToInt32(ctx, &v, argv[0]); y = v; }
    if (argc >= 2) { int32_t v; JS_ToInt32(ctx, &v, argv[1]); mo = v; }
    if (argc >= 3) { int32_t v; JS_ToInt32(ctx, &v, argv[2]); d = v; }
    tmp_set_date_fields(t, y, mo, d);
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_date_toString(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_temporal *t = tmp_this(ctx, this_val, TK_PLAINDATE);
    if (!t) return JS_EXCEPTION;
    char buf[32];
    g_snprintf(buf, sizeof buf, "%04d-%02d-%02d", t->year, t->month, t->day);
    return JS_NewString(ctx, buf);
}

static void
tmp_add_date(ns_temporal *t, int64_t y, int64_t mo, int64_t w, int64_t d, int sign)
{
    int ny = t->year + (int)(sign * y);
    long total_mo = (long)t->month - 1 + sign * mo;
    ny += (int)(total_mo >= 0 ? total_mo / 12 : (total_mo - 11) / 12);
    int nmo = (int)ns_dt_floormod(total_mo, 12) + 1;
    int dim = ns_dt_days_in_month(ny, nmo);
    int nd = t->day > dim ? dim : t->day;
    long days = ns_dt_days_from_civil(ny, nmo, nd) + sign * (w * 7 + d);
    ns_dt_civil_from_days(days, &t->year, &t->month, &t->day);
}

static void
tmp_read_dur(JSContext *ctx, JSValueConst durv, int64_t out[10])
{
    for (int i = 0; i < 10; i++) out[i] = 0;
    ns_temporal *dur = JS_GetOpaque(durv, ns_temporal_class_id);
    if (dur && dur->kind == TK_DURATION) {
        for (int i = 0; i < 10; i++) out[i] = dur->dur[i];
        return;
    }
    if (!JS_IsObject(durv)) return;
    static const char *const names[] = {
        "years","months","weeks","days","hours","minutes",
        "seconds","milliseconds","microseconds","nanoseconds" };
    for (int i = 0; i < 10; i++) out[i] = tmp_int_prop(ctx, durv, names[i], 0);
}

static JSValue
tmp_date_add_impl(JSContext *ctx, JSValueConst this_val, JSValueConst durv, int sign)
{
    ns_temporal *t = tmp_this(ctx, this_val, TK_PLAINDATE);
    if (!t) return JS_EXCEPTION;
    int64_t dur[10]; tmp_read_dur(ctx, durv, dur);
    JSValue gp = JS_GetPropertyStr(ctx, this_val, "constructor");
    JSValue proto = JS_GetPropertyStr(ctx, gp, "prototype");
    ns_temporal *nt; JSValue out = tmp_make(ctx, proto, TK_PLAINDATE, &nt);
    JS_FreeValue(ctx, proto); JS_FreeValue(ctx, gp);
    *nt = *t; nt->tz = NULL;
    tmp_add_date(nt, dur[0], dur[1], dur[2], dur[3], sign);
    tmp_fill(ctx, out, nt);
    return out;
}

static JSValue
tmp_date_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return tmp_date_add_impl(ctx, this_val, argc >= 1 ? argv[0] : JS_UNDEFINED, 1); }
static JSValue
tmp_date_subtract(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return tmp_date_add_impl(ctx, this_val, argc >= 1 ? argv[0] : JS_UNDEFINED, -1); }

static JSValue
tmp_date_with(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ns_temporal *t = tmp_this(ctx, this_val, TK_PLAINDATE);
    if (!t) return JS_EXCEPTION;
    JSValueConst f = argc >= 1 ? argv[0] : JS_UNDEFINED;
    JSValue gp = JS_GetPropertyStr(ctx, this_val, "constructor");
    JSValue proto = JS_GetPropertyStr(ctx, gp, "prototype");
    ns_temporal *nt; JSValue out = tmp_make(ctx, proto, TK_PLAINDATE, &nt);
    JS_FreeValue(ctx, proto); JS_FreeValue(ctx, gp);
    tmp_set_date_fields(nt,
        tmp_has_prop(ctx, f, "year") ? tmp_int_prop(ctx, f, "year", t->year) : t->year,
        tmp_has_prop(ctx, f, "month") ? tmp_int_prop(ctx, f, "month", t->month) : t->month,
        tmp_has_prop(ctx, f, "day") ? tmp_int_prop(ctx, f, "day", t->day) : t->day);
    tmp_fill(ctx, out, nt);
    return out;
}

static int
tmp_cmp_date(const ns_temporal *a, const ns_temporal *b)
{
    long da = ns_dt_days_from_civil(a->year, a->month, a->day);
    long db = ns_dt_days_from_civil(b->year, b->month, b->day);
    return da < db ? -1 : da > db ? 1 : 0;
}

static JSValue
tmp_date_until_impl(JSContext *ctx, JSValueConst this_val, JSValueConst other, int sign)
{
    ns_temporal *t = tmp_this(ctx, this_val, TK_PLAINDATE);
    ns_temporal *o = JS_GetOpaque(other, ns_temporal_class_id);
    if (!t) return JS_EXCEPTION;
    if (!o || o->kind != TK_PLAINDATE)
        return JS_ThrowTypeError(ctx, "expected a PlainDate");
    long days = ns_dt_days_from_civil(o->year, o->month, o->day) -
                ns_dt_days_from_civil(t->year, t->month, t->day);
    JSValue glob = JS_GetGlobalObject(ctx);
    JSValue temporal = JS_GetPropertyStr(ctx, glob, "Temporal");
    JSValue dctor = JS_GetPropertyStr(ctx, temporal, "Duration");
    JSValue proto = JS_GetPropertyStr(ctx, dctor, "prototype");
    ns_temporal *nt; JSValue out = tmp_make(ctx, proto, TK_DURATION, &nt);
    nt->dur[3] = sign * days;
    tmp_fill(ctx, out, nt);
    JS_FreeValue(ctx, proto); JS_FreeValue(ctx, dctor);
    JS_FreeValue(ctx, temporal); JS_FreeValue(ctx, glob);
    return out;
}

static JSValue
tmp_date_until(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return tmp_date_until_impl(ctx, this_val, argc >= 1 ? argv[0] : JS_UNDEFINED, 1); }
static JSValue
tmp_date_since(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return tmp_date_until_impl(ctx, this_val, argc >= 1 ? argv[0] : JS_UNDEFINED, -1); }

static JSValue
tmp_date_equals(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ns_temporal *t = tmp_this(ctx, this_val, TK_PLAINDATE);
    if (!t) return JS_EXCEPTION;
    ns_temporal *o = argc >= 1 ? JS_GetOpaque(argv[0], ns_temporal_class_id) : NULL;
    gboolean eq = o && o->kind == TK_PLAINDATE && tmp_cmp_date(t, o) == 0;
    return JS_NewBool(ctx, eq);
}

static JSValue
tmp_date_compare(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    ns_temporal *a = argc >= 1 ? JS_GetOpaque(argv[0], ns_temporal_class_id) : NULL;
    ns_temporal *b = argc >= 2 ? JS_GetOpaque(argv[1], ns_temporal_class_id) : NULL;
    if (!a || !b) return JS_ThrowTypeError(ctx, "Temporal.PlainDate.compare needs two dates");
    return JS_NewInt32(ctx, tmp_cmp_date(a, b));
}

static JSValue
tmp_date_toPlainDateTime(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ns_temporal *t = tmp_this(ctx, this_val, TK_PLAINDATE);
    if (!t) return JS_EXCEPTION;
    ns_temporal *time = argc >= 1 ? JS_GetOpaque(argv[0], ns_temporal_class_id) : NULL;
    JSValue glob = JS_GetGlobalObject(ctx);
    JSValue temporal = JS_GetPropertyStr(ctx, glob, "Temporal");
    JSValue ctor = JS_GetPropertyStr(ctx, temporal, "PlainDateTime");
    JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    ns_temporal *nt; JSValue out = tmp_make(ctx, proto, TK_PLAINDATETIME, &nt);
    nt->year = t->year; nt->month = t->month; nt->day = t->day;
    if (time && time->kind == TK_PLAINTIME) {
        nt->hour = time->hour; nt->minute = time->minute; nt->second = time->second;
        nt->ms = time->ms; nt->us = time->us; nt->ns = time->ns;
    }
    tmp_fill(ctx, out, nt);
    JS_FreeValue(ctx, proto); JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, temporal); JS_FreeValue(ctx, glob);
    return out;
}

/* ---- PlainTime ---------------------------------------------------------- */

static void
tmp_norm_time(ns_temporal *t)
{
    int64_t ns = (int64_t)t->hour * 3600000000000LL +
                 (int64_t)t->minute * 60000000000LL +
                 (int64_t)t->second * 1000000000LL +
                 (int64_t)t->ms * 1000000LL + (int64_t)t->us * 1000LL + t->ns;
    ns = ((ns % 86400000000000LL) + 86400000000000LL) % 86400000000000LL;
    t->hour = (int)(ns / 3600000000000LL); ns %= 3600000000000LL;
    t->minute = (int)(ns / 60000000000LL); ns %= 60000000000LL;
    t->second = (int)(ns / 1000000000LL); ns %= 1000000000LL;
    t->ms = (int)(ns / 1000000LL); ns %= 1000000LL;
    t->us = (int)(ns / 1000LL); t->ns = (int)(ns % 1000LL);
}

static JSValue
tmp_time_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_PLAINTIME, &out);
    int vals[6] = { 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < argc && i < 6; i++) { int32_t v; JS_ToInt32(ctx, &v, argv[i]); vals[i] = v; }
    t->hour = vals[0]; t->minute = vals[1]; t->second = vals[2];
    t->ms = vals[3]; t->us = vals[4]; t->ns = vals[5];
    tmp_norm_time(t);
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_time_from(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_PLAINTIME, &out);
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char *s = JS_ToCString(ctx, argv[0]);
        int ms = 0;
        gboolean ok = s && ns_dt_rd_time(s, &ms) != NULL;
        if (s) JS_FreeCString(ctx, s);
        if (!ok) {
            JS_FreeValue(ctx, out);
            return JS_ThrowRangeError(ctx, "invalid PlainTime string");
        }
        t->hour = ms / 3600000; t->minute = (ms / 60000) % 60;
        t->second = (ms / 1000) % 60; t->ms = ms % 1000;
    } else if (argc >= 1 && JS_IsObject(argv[0])) {
        ns_temporal *src = JS_GetOpaque(argv[0], ns_temporal_class_id);
        if (src && (src->kind == TK_PLAINTIME || src->kind == TK_PLAINDATETIME)) {
            t->hour = src->hour; t->minute = src->minute; t->second = src->second;
            t->ms = src->ms; t->us = src->us; t->ns = src->ns;
        } else {
            t->hour = tmp_int_prop(ctx, argv[0], "hour", 0);
            t->minute = tmp_int_prop(ctx, argv[0], "minute", 0);
            t->second = tmp_int_prop(ctx, argv[0], "second", 0);
            t->ms = tmp_int_prop(ctx, argv[0], "millisecond", 0);
            t->us = tmp_int_prop(ctx, argv[0], "microsecond", 0);
            t->ns = tmp_int_prop(ctx, argv[0], "nanosecond", 0);
        }
    }
    tmp_norm_time(t);
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_time_toString(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_temporal *t = tmp_this(ctx, this_val, TK_PLAINTIME);
    if (!t) return JS_EXCEPTION;
    GString *str = g_string_new(NULL);
    g_string_append_printf(str, "%02d:%02d:%02d", t->hour, t->minute, t->second);
    tmp_append_frac(str, t->ms, t->us, t->ns);
    JSValue r = JS_NewStringLen(ctx, str->str, str->len);
    g_string_free(str, TRUE);
    return r;
}

static JSValue
tmp_time_add_impl(JSContext *ctx, JSValueConst this_val, JSValueConst durv, int sign)
{
    ns_temporal *t = tmp_this(ctx, this_val, TK_PLAINTIME);
    if (!t) return JS_EXCEPTION;
    int64_t dur[10]; tmp_read_dur(ctx, durv, dur);
    JSValue gp = JS_GetPropertyStr(ctx, this_val, "constructor");
    JSValue proto = JS_GetPropertyStr(ctx, gp, "prototype");
    ns_temporal *nt; JSValue out = tmp_make(ctx, proto, TK_PLAINTIME, &nt);
    JS_FreeValue(ctx, proto); JS_FreeValue(ctx, gp);
    *nt = *t; nt->tz = NULL;
    nt->hour += (int)(sign * dur[4]);
    nt->minute += (int)(sign * dur[5]);
    nt->second += (int)(sign * dur[6]);
    nt->ms += (int)(sign * dur[7]);
    nt->us += (int)(sign * dur[8]);
    nt->ns += (int)(sign * dur[9]);
    tmp_norm_time(nt);
    tmp_fill(ctx, out, nt);
    return out;
}

static JSValue
tmp_time_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return tmp_time_add_impl(ctx, this_val, argc >= 1 ? argv[0] : JS_UNDEFINED, 1); }
static JSValue
tmp_time_subtract(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return tmp_time_add_impl(ctx, this_val, argc >= 1 ? argv[0] : JS_UNDEFINED, -1); }

/* ---- PlainDateTime ------------------------------------------------------ */

static JSValue
tmp_datetime_from(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_PLAINDATETIME, &out);
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char *s = JS_ToCString(ctx, argv[0]);
        int y, mo, d, h, mi, se, ms, us, ns, off; gboolean ho, ht;
        gboolean ok = s && tmp_parse_datetime(s, &y, &mo, &d, &h, &mi, &se,
                                              &ms, &us, &ns, &off, &ho, &ht);
        if (s) JS_FreeCString(ctx, s);
        if (!ok) {
            JS_FreeValue(ctx, out);
            return JS_ThrowRangeError(ctx, "invalid PlainDateTime string");
        }
        tmp_set_date_fields(t, y, mo, d);
        t->hour = h; t->minute = mi; t->second = se; t->ms = ms; t->us = us; t->ns = ns;
    } else if (argc >= 1 && JS_IsObject(argv[0])) {
        tmp_set_date_fields(t,
            tmp_int_prop(ctx, argv[0], "year", 1970),
            tmp_int_prop(ctx, argv[0], "month", 1),
            tmp_int_prop(ctx, argv[0], "day", 1));
        t->hour = tmp_int_prop(ctx, argv[0], "hour", 0);
        t->minute = tmp_int_prop(ctx, argv[0], "minute", 0);
        t->second = tmp_int_prop(ctx, argv[0], "second", 0);
        t->ms = tmp_int_prop(ctx, argv[0], "millisecond", 0);
        t->us = tmp_int_prop(ctx, argv[0], "microsecond", 0);
        t->ns = tmp_int_prop(ctx, argv[0], "nanosecond", 0);
    }
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_datetime_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_PLAINDATETIME, &out);
    int vals[9] = { 1970, 1, 1, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < argc && i < 9; i++) { int32_t v; JS_ToInt32(ctx, &v, argv[i]); vals[i] = v; }
    tmp_set_date_fields(t, vals[0], vals[1], vals[2]);
    t->hour = vals[3]; t->minute = vals[4]; t->second = vals[5];
    t->ms = vals[6]; t->us = vals[7]; t->ns = vals[8];
    tmp_norm_time(t);
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_datetime_toString(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_temporal *t = tmp_this(ctx, this_val, TK_PLAINDATETIME);
    if (!t) return JS_EXCEPTION;
    GString *str = g_string_new(NULL);
    g_string_append_printf(str, "%04d-%02d-%02dT%02d:%02d:%02d",
                           t->year, t->month, t->day, t->hour, t->minute, t->second);
    tmp_append_frac(str, t->ms, t->us, t->ns);
    JSValue r = JS_NewStringLen(ctx, str->str, str->len);
    g_string_free(str, TRUE);
    return r;
}

static JSValue
tmp_datetime_add_impl(JSContext *ctx, JSValueConst this_val, JSValueConst durv, int sign)
{
    ns_temporal *t = tmp_this(ctx, this_val, TK_PLAINDATETIME);
    if (!t) return JS_EXCEPTION;
    int64_t dur[10]; tmp_read_dur(ctx, durv, dur);
    JSValue gp = JS_GetPropertyStr(ctx, this_val, "constructor");
    JSValue proto = JS_GetPropertyStr(ctx, gp, "prototype");
    ns_temporal *nt; JSValue out = tmp_make(ctx, proto, TK_PLAINDATETIME, &nt);
    JS_FreeValue(ctx, proto); JS_FreeValue(ctx, gp);
    *nt = *t; nt->tz = NULL;
    int64_t tns = (int64_t)nt->hour * 3600000000000LL + (int64_t)nt->minute * 60000000000LL +
                  (int64_t)nt->second * 1000000000LL + (int64_t)nt->ms * 1000000LL +
                  (int64_t)nt->us * 1000LL + nt->ns;
    tns += sign * (dur[4] * 3600000000000LL + dur[5] * 60000000000LL +
                   dur[6] * 1000000000LL + dur[7] * 1000000LL + dur[8] * 1000LL + dur[9]);
    int64_t carry = tns >= 0 ? tns / 86400000000000LL : (tns - 86399999999999LL) / 86400000000000LL;
    tns -= carry * 86400000000000LL;
    nt->hour = (int)(tns / 3600000000000LL); tns %= 3600000000000LL;
    nt->minute = (int)(tns / 60000000000LL); tns %= 60000000000LL;
    nt->second = (int)(tns / 1000000000LL); tns %= 1000000000LL;
    nt->ms = (int)(tns / 1000000LL); tns %= 1000000LL;
    nt->us = (int)(tns / 1000LL); nt->ns = (int)(tns % 1000LL);
    int ny = nt->year + (int)(sign * dur[0]);
    long total_mo = (long)nt->month - 1 + sign * dur[1];
    ny += (int)(total_mo >= 0 ? total_mo / 12 : (total_mo - 11) / 12);
    int nmo = (int)ns_dt_floormod(total_mo, 12) + 1;
    int dim = ns_dt_days_in_month(ny, nmo);
    int nd = nt->day > dim ? dim : nt->day;
    long days = ns_dt_days_from_civil(ny, nmo, nd) +
                sign * (dur[2] * 7 + dur[3]) + carry;
    ns_dt_civil_from_days(days, &nt->year, &nt->month, &nt->day);
    tmp_fill(ctx, out, nt);
    return out;
}

static JSValue
tmp_datetime_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return tmp_datetime_add_impl(ctx, this_val, argc >= 1 ? argv[0] : JS_UNDEFINED, 1); }
static JSValue
tmp_datetime_subtract(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return tmp_datetime_add_impl(ctx, this_val, argc >= 1 ? argv[0] : JS_UNDEFINED, -1); }

static JSValue
tmp_datetime_toPlainDate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_temporal *t = tmp_this(ctx, this_val, TK_PLAINDATETIME);
    if (!t) return JS_EXCEPTION;
    JSValue glob = JS_GetGlobalObject(ctx);
    JSValue temporal = JS_GetPropertyStr(ctx, glob, "Temporal");
    JSValue ctor = JS_GetPropertyStr(ctx, temporal, "PlainDate");
    JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    ns_temporal *nt; JSValue out = tmp_make(ctx, proto, TK_PLAINDATE, &nt);
    nt->year = t->year; nt->month = t->month; nt->day = t->day;
    tmp_fill(ctx, out, nt);
    JS_FreeValue(ctx, proto); JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, temporal); JS_FreeValue(ctx, glob);
    return out;
}

static JSValue
tmp_datetime_toPlainTime(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_temporal *t = tmp_this(ctx, this_val, TK_PLAINDATETIME);
    if (!t) return JS_EXCEPTION;
    JSValue glob = JS_GetGlobalObject(ctx);
    JSValue temporal = JS_GetPropertyStr(ctx, glob, "Temporal");
    JSValue ctor = JS_GetPropertyStr(ctx, temporal, "PlainTime");
    JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    ns_temporal *nt; JSValue out = tmp_make(ctx, proto, TK_PLAINTIME, &nt);
    nt->hour = t->hour; nt->minute = t->minute; nt->second = t->second;
    nt->ms = t->ms; nt->us = t->us; nt->ns = t->ns;
    tmp_fill(ctx, out, nt);
    JS_FreeValue(ctx, proto); JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, temporal); JS_FreeValue(ctx, glob);
    return out;
}

/* ---- PlainYearMonth / PlainMonthDay ------------------------------------ */

static JSValue
tmp_yearmonth_from(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_PLAINYEARMONTH, &out);
    t->year = 1970; t->month = 1; t->day = 1;
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s) { int y, m; if (sscanf(s, "%d-%d", &y, &m) >= 2) { t->year = y; t->month = m; } JS_FreeCString(ctx, s); }
    } else if (argc >= 1 && JS_IsObject(argv[0])) {
        t->year = tmp_int_prop(ctx, argv[0], "year", 1970);
        t->month = tmp_int_prop(ctx, argv[0], "month", 1);
    }
    if (t->month < 1) t->month = 1;
    if (t->month > 12) t->month = 12;
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_yearmonth_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_PLAINYEARMONTH, &out);
    int y = 1970, m = 1;
    if (argc >= 1) { int32_t v; JS_ToInt32(ctx, &v, argv[0]); y = v; }
    if (argc >= 2) { int32_t v; JS_ToInt32(ctx, &v, argv[1]); m = v; }
    t->year = y; t->month = m < 1 ? 1 : m > 12 ? 12 : m; t->day = 1;
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_yearmonth_toString(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_temporal *t = tmp_this(ctx, this_val, TK_PLAINYEARMONTH);
    if (!t) return JS_EXCEPTION;
    char buf[16];
    g_snprintf(buf, sizeof buf, "%04d-%02d", t->year, t->month);
    return JS_NewString(ctx, buf);
}

static JSValue
tmp_monthday_from(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_PLAINMONTHDAY, &out);
    t->year = 1972; t->month = 1; t->day = 1;
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s) {
            int m, d;
            const char *p = s;
            if (p[0] == '-' && p[1] == '-') p += 2;
            if (sscanf(p, "%d-%d", &m, &d) >= 2) { t->month = m; t->day = d; }
            JS_FreeCString(ctx, s);
        }
    } else if (argc >= 1 && JS_IsObject(argv[0])) {
        t->month = tmp_int_prop(ctx, argv[0], "month", 1);
        t->day = tmp_int_prop(ctx, argv[0], "day", 1);
    }
    if (t->month < 1) t->month = 1;
    if (t->month > 12) t->month = 12;
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_monthday_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_PLAINMONTHDAY, &out);
    int m = 1, d = 1;
    if (argc >= 1) { int32_t v; JS_ToInt32(ctx, &v, argv[0]); m = v; }
    if (argc >= 2) { int32_t v; JS_ToInt32(ctx, &v, argv[1]); d = v; }
    t->year = 1972; t->month = m < 1 ? 1 : m > 12 ? 12 : m; t->day = d;
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_monthday_toString(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_temporal *t = tmp_this(ctx, this_val, TK_PLAINMONTHDAY);
    if (!t) return JS_EXCEPTION;
    char buf[16];
    g_snprintf(buf, sizeof buf, "%02d-%02d", t->month, t->day);
    return JS_NewString(ctx, buf);
}

/* ---- ZonedDateTime ------------------------------------------------------ */

static JSValue
tmp_zoned_from(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_ZONEDDATETIME, &out);
    t->tz = g_strdup("UTC");
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char *s = JS_ToCString(ctx, argv[0]);
        int y, mo, d, h, mi, se, ms, us, ns, off; gboolean ho, ht;
        if (s && tmp_parse_datetime(s, &y, &mo, &d, &h, &mi, &se, &ms, &us, &ns, &off, &ho, &ht)) {
            t->epoch_sec = tmp_epoch_of(y, mo, d, h, mi, se) - off * 60;
            t->nanos = ms * 1000000 + us * 1000 + ns;
            const char *br = s ? strchr(s, '[') : NULL;
            if (br) {
                const char *end = strchr(br, ']');
                if (end && end > br + 1) {
                    g_free(t->tz);
                    t->tz = g_strndup(br + 1, (gsize)(end - br - 1));
                }
            }
        }
        if (s) JS_FreeCString(ctx, s);
    }
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_zoned_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    int64_t ns = 0;
    if (argc >= 1) JS_ToBigInt64(ctx, &ns, argv[0]);
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_ZONEDDATETIME, &out);
    t->epoch_sec = (ns >= 0 ? ns : ns - 999999999) / 1000000000LL;
    t->nanos = (int32_t)(ns - t->epoch_sec * 1000000000LL);
    if (argc >= 2 && JS_IsString(argv[1])) {
        const char *z = JS_ToCString(ctx, argv[1]);
        t->tz = g_strdup(z ? z : "UTC");
        if (z) JS_FreeCString(ctx, z);
    } else {
        t->tz = g_strdup("UTC");
    }
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_zoned_toString(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_temporal *t = tmp_this(ctx, this_val, TK_ZONEDDATETIME);
    if (!t) return JS_EXCEPTION;
    int y, mo, d, h, mi, s;
    tmp_breakdown(t->epoch_sec, &y, &mo, &d, &h, &mi, &s);
    GString *str = g_string_new(NULL);
    g_string_append_printf(str, "%04d-%02d-%02dT%02d:%02d:%02d", y, mo, d, h, mi, s);
    tmp_append_frac(str, t->nanos / 1000000, (t->nanos / 1000) % 1000, t->nanos % 1000);
    g_string_append_printf(str, "+00:00[%s]", t->tz ? t->tz : "UTC");
    JSValue r = JS_NewStringLen(ctx, str->str, str->len);
    g_string_free(str, TRUE);
    return r;
}

static JSValue
tmp_zoned_toInstant(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_temporal *t = tmp_this(ctx, this_val, TK_ZONEDDATETIME);
    if (!t) return JS_EXCEPTION;
    JSValue glob = JS_GetGlobalObject(ctx);
    JSValue temporal = JS_GetPropertyStr(ctx, glob, "Temporal");
    JSValue ctor = JS_GetPropertyStr(ctx, temporal, "Instant");
    JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    ns_temporal *nt; JSValue out = tmp_make(ctx, proto, TK_INSTANT, &nt);
    nt->epoch_sec = t->epoch_sec; nt->nanos = t->nanos;
    tmp_fill(ctx, out, nt);
    JS_FreeValue(ctx, proto); JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, temporal); JS_FreeValue(ctx, glob);
    return out;
}

static JSValue
tmp_zoned_toPlainDateTime(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_temporal *t = tmp_this(ctx, this_val, TK_ZONEDDATETIME);
    if (!t) return JS_EXCEPTION;
    int y, mo, d, h, mi, s;
    tmp_breakdown(t->epoch_sec, &y, &mo, &d, &h, &mi, &s);
    JSValue glob = JS_GetGlobalObject(ctx);
    JSValue temporal = JS_GetPropertyStr(ctx, glob, "Temporal");
    JSValue ctor = JS_GetPropertyStr(ctx, temporal, "PlainDateTime");
    JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    ns_temporal *nt; JSValue out = tmp_make(ctx, proto, TK_PLAINDATETIME, &nt);
    nt->year = y; nt->month = mo; nt->day = d;
    nt->hour = h; nt->minute = mi; nt->second = s;
    nt->ms = t->nanos / 1000000; nt->us = (t->nanos / 1000) % 1000; nt->ns = t->nanos % 1000;
    tmp_fill(ctx, out, nt);
    JS_FreeValue(ctx, proto); JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, temporal); JS_FreeValue(ctx, glob);
    return out;
}

/* ---- Duration ----------------------------------------------------------- */

static JSValue
tmp_duration_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_DURATION, &out);
    for (int i = 0; i < argc && i < 10; i++) {
        int64_t v; JS_ToInt64(ctx, &v, argv[i]); t->dur[i] = v;
    }
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_duration_from(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue out; ns_temporal *t = tmp_alloc(ctx, this_val, TK_DURATION, &out);
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s) {
            int sign = 1;
            const char *p = s;
            if (*p == '-' || *p == '+') { if (*p == '-') sign = -1; p++; }
            if (*p == 'P' || *p == 'p') {
                p++;
                gboolean in_time = FALSE;
                while (*p) {
                    if (*p == 'T' || *p == 't') { in_time = TRUE; p++; continue; }
                    char *end;
                    double val = g_ascii_strtod(p, &end);
                    if (end == p) break;
                    char unit = *end;
                    if (!unit) break;
                    p = end + 1;
                    int64_t iv = (int64_t)(sign * val);
                    if (!in_time) {
                        if (unit == 'Y' || unit == 'y') t->dur[0] = iv;
                        else if (unit == 'M' || unit == 'm') t->dur[1] = iv;
                        else if (unit == 'W' || unit == 'w') t->dur[2] = iv;
                        else if (unit == 'D' || unit == 'd') t->dur[3] = iv;
                    } else {
                        if (unit == 'H' || unit == 'h') t->dur[4] = iv;
                        else if (unit == 'M' || unit == 'm') t->dur[5] = iv;
                        else if (unit == 'S' || unit == 's') {
                            t->dur[6] = iv;
                            double frac = sign * (val - (int64_t)val);
                            t->dur[7] = (int64_t)(frac * 1000);
                            t->dur[8] = (int64_t)(frac * 1000000) % 1000;
                            t->dur[9] = (int64_t)(frac * 1000000000) % 1000;
                        }
                    }
                }
            }
            JS_FreeCString(ctx, s);
        }
    } else if (argc >= 1 && JS_IsObject(argv[0])) {
        static const char *const names[] = {
            "years","months","weeks","days","hours","minutes",
            "seconds","milliseconds","microseconds","nanoseconds" };
        for (int i = 0; i < 10; i++) t->dur[i] = tmp_int_prop(ctx, argv[0], names[i], 0);
    }
    tmp_fill(ctx, out, t);
    return out;
}

static JSValue
tmp_duration_toString(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_temporal *t = tmp_this(ctx, this_val, TK_DURATION);
    if (!t) return JS_EXCEPTION;
    int sign = 0;
    for (int i = 0; i < 10; i++) if (t->dur[i] != 0) { sign = t->dur[i] > 0 ? 1 : -1; break; }
    GString *str = g_string_new(NULL);
    if (sign < 0) g_string_append_c(str, '-');
    g_string_append_c(str, 'P');
    int64_t a[10];
    for (int i = 0; i < 10; i++) a[i] = t->dur[i] < 0 ? -t->dur[i] : t->dur[i];
    if (a[0]) g_string_append_printf(str, "%" G_GINT64_FORMAT "Y", a[0]);
    if (a[1]) g_string_append_printf(str, "%" G_GINT64_FORMAT "M", a[1]);
    if (a[2]) g_string_append_printf(str, "%" G_GINT64_FORMAT "W", a[2]);
    if (a[3]) g_string_append_printf(str, "%" G_GINT64_FORMAT "D", a[3]);
    int64_t frac_ns = a[7] * 1000000 + a[8] * 1000 + a[9];
    if (a[4] || a[5] || a[6] || frac_ns) {
        g_string_append_c(str, 'T');
        if (a[4]) g_string_append_printf(str, "%" G_GINT64_FORMAT "H", a[4]);
        if (a[5]) g_string_append_printf(str, "%" G_GINT64_FORMAT "M", a[5]);
        if (a[6] || frac_ns) {
            g_string_append_printf(str, "%" G_GINT64_FORMAT, a[6]);
            if (frac_ns) {
                char fb[16];
                g_snprintf(fb, sizeof fb, "%09" G_GINT64_FORMAT, frac_ns);
                int len = 9; while (len > 0 && fb[len - 1] == '0') len--;
                g_string_append_c(str, '.');
                g_string_append_len(str, fb, len);
            }
            g_string_append_c(str, 'S');
        }
    }
    if (str->len == 1) g_string_append(str, "T0S");
    JSValue r = JS_NewStringLen(ctx, str->str, str->len);
    g_string_free(str, TRUE);
    return r;
}

static JSValue
tmp_duration_arith(JSContext *ctx, JSValueConst this_val, JSValueConst other, int sign)
{
    ns_temporal *t = tmp_this(ctx, this_val, TK_DURATION);
    if (!t) return JS_EXCEPTION;
    int64_t o[10]; tmp_read_dur(ctx, other, o);
    JSValue gp = JS_GetPropertyStr(ctx, this_val, "constructor");
    JSValue proto = JS_GetPropertyStr(ctx, gp, "prototype");
    ns_temporal *nt; JSValue out = tmp_make(ctx, proto, TK_DURATION, &nt);
    JS_FreeValue(ctx, proto); JS_FreeValue(ctx, gp);
    for (int i = 0; i < 10; i++) nt->dur[i] = t->dur[i] + sign * o[i];
    tmp_fill(ctx, out, nt);
    return out;
}

static JSValue
tmp_duration_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return tmp_duration_arith(ctx, this_val, argc >= 1 ? argv[0] : JS_UNDEFINED, 1); }
static JSValue
tmp_duration_subtract(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return tmp_duration_arith(ctx, this_val, argc >= 1 ? argv[0] : JS_UNDEFINED, -1); }

static JSValue
tmp_duration_unary(JSContext *ctx, JSValueConst this_val, int abs)
{
    ns_temporal *t = tmp_this(ctx, this_val, TK_DURATION);
    if (!t) return JS_EXCEPTION;
    JSValue gp = JS_GetPropertyStr(ctx, this_val, "constructor");
    JSValue proto = JS_GetPropertyStr(ctx, gp, "prototype");
    ns_temporal *nt; JSValue out = tmp_make(ctx, proto, TK_DURATION, &nt);
    JS_FreeValue(ctx, proto); JS_FreeValue(ctx, gp);
    for (int i = 0; i < 10; i++)
        nt->dur[i] = abs ? (t->dur[i] < 0 ? -t->dur[i] : t->dur[i]) : -t->dur[i];
    tmp_fill(ctx, out, nt);
    return out;
}

static JSValue
tmp_duration_negated(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ (void)argc; (void)argv; return tmp_duration_unary(ctx, this_val, 0); }
static JSValue
tmp_duration_abs(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ (void)argc; (void)argv; return tmp_duration_unary(ctx, this_val, 1); }

/* ---- Temporal.Now ------------------------------------------------------- */

static JSValue
tmp_now_make_dt(JSContext *ctx, const char *ctor_name, int kind)
{
    gint64 us = g_get_real_time();
    int64_t epoch_sec = us / 1000000;
    int32_t nanos = (int32_t)((us % 1000000) * 1000);
    JSValue glob = JS_GetGlobalObject(ctx);
    JSValue temporal = JS_GetPropertyStr(ctx, glob, "Temporal");
    JSValue ctor = JS_GetPropertyStr(ctx, temporal, ctor_name);
    JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    ns_temporal *t; JSValue out = tmp_make(ctx, proto, kind, &t);
    if (kind == TK_INSTANT) { t->epoch_sec = epoch_sec; t->nanos = nanos; }
    else if (kind == TK_ZONEDDATETIME) {
        t->epoch_sec = epoch_sec; t->nanos = nanos; t->tz = g_strdup("UTC");
    } else {
        int y, mo, d, h, mi, s;
        tmp_breakdown(epoch_sec, &y, &mo, &d, &h, &mi, &s);
        t->year = y; t->month = mo; t->day = d;
        t->hour = h; t->minute = mi; t->second = s;
        t->ms = nanos / 1000000; t->us = (nanos / 1000) % 1000; t->ns = nanos % 1000;
    }
    tmp_fill(ctx, out, t);
    JS_FreeValue(ctx, proto); JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, temporal); JS_FreeValue(ctx, glob);
    return out;
}

static JSValue tmp_now_instant(JSContext *ctx, JSValueConst t, int c, JSValueConst *a)
{ (void)t; (void)c; (void)a; return tmp_now_make_dt(ctx, "Instant", TK_INSTANT); }
static JSValue tmp_now_zdt(JSContext *ctx, JSValueConst t, int c, JSValueConst *a)
{ (void)t; (void)c; (void)a; return tmp_now_make_dt(ctx, "ZonedDateTime", TK_ZONEDDATETIME); }
static JSValue tmp_now_pdt(JSContext *ctx, JSValueConst t, int c, JSValueConst *a)
{ (void)t; (void)c; (void)a; return tmp_now_make_dt(ctx, "PlainDateTime", TK_PLAINDATETIME); }
static JSValue tmp_now_pd(JSContext *ctx, JSValueConst t, int c, JSValueConst *a)
{ (void)t; (void)c; (void)a; return tmp_now_make_dt(ctx, "PlainDate", TK_PLAINDATE); }
static JSValue tmp_now_pt(JSContext *ctx, JSValueConst t, int c, JSValueConst *a)
{ (void)t; (void)c; (void)a; return tmp_now_make_dt(ctx, "PlainTime", TK_PLAINTIME); }
static JSValue tmp_now_tz(JSContext *ctx, JSValueConst t, int c, JSValueConst *a)
{ (void)t; (void)c; (void)a; return JS_NewString(ctx, "UTC"); }

/* ---- registration ------------------------------------------------------- */

typedef struct { const char *name; JSCFunction *fn; int argc; } tmp_method;

static void
tmp_bind(JSContext *ctx, JSValueConst obj, const char *name, JSCFunction *fn, int argc)
{
    JS_SetPropertyStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, argc));
}

static JSValue
tmp_make_toJSON(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue ts = JS_GetPropertyStr(ctx, this_val, "toString");
    JSValue r = JS_Call(ctx, ts, this_val, 0, NULL);
    JS_FreeValue(ctx, ts);
    (void)argc; (void)argv;
    return r;
}

static JSValue
tmp_register(JSContext *ctx, JSValueConst temporal, const char *name,
             JSCFunction *ctor, int ctor_argc,
             const tmp_method *methods, int n_methods,
             const tmp_method *statics, int n_statics)
{
    JSValue func = JS_NewCFunction2(ctx, ctor, name, ctor_argc,
                                    JS_CFUNC_constructor_or_func, 0);
    JSValue proto = JS_NewObject(ctx);
    for (int i = 0; i < n_methods; i++)
        tmp_bind(ctx, proto, methods[i].name, methods[i].fn, methods[i].argc);
    tmp_bind(ctx, proto, "toJSON", tmp_make_toJSON, 0);
    JS_SetConstructor(ctx, func, proto);
    JS_FreeValue(ctx, proto);
    for (int i = 0; i < n_statics; i++)
        tmp_bind(ctx, func, statics[i].name, statics[i].fn, statics[i].argc);
    JS_SetPropertyStr(ctx, temporal, name, JS_DupValue(ctx, func));
    return func;
}

void
ns_js_temporal_install(JSContext *ctx, JSValueConst global)
{
    JSAtom atom = JS_NewAtom(ctx, "Temporal");
    int has = JS_HasProperty(ctx, global, atom);
    JS_FreeAtom(ctx, atom);
    if (has > 0) {
        JSValue existing = JS_GetPropertyStr(ctx, global, "Temporal");
        gboolean real = JS_IsObject(existing) &&
            JS_HasProperty(ctx, existing,
                           (atom = JS_NewAtom(ctx, "Now")));
        JS_FreeAtom(ctx, atom);
        JS_FreeValue(ctx, existing);
        if (real) return;
    }

    ns_new_class_id(&ns_temporal_class_id);
    if (!JS_IsRegisteredClass(JS_GetRuntime(ctx), ns_temporal_class_id))
        JS_NewClass(JS_GetRuntime(ctx), ns_temporal_class_id, &ns_temporal_class);

    JSValue temporal = JS_NewObject(ctx);

    static const tmp_method instant_m[] = {
        { "toString", tmp_instant_toString, 0 },
        { "add", tmp_instant_add, 1 },
        { "subtract", tmp_instant_subtract, 1 },
    };
    static const tmp_method instant_s[] = {
        { "from", tmp_instant_from, 1 },
        { "fromEpochMilliseconds", tmp_instant_fromEpochMilliseconds, 1 },
        { "fromEpochNanoseconds", tmp_instant_fromEpochNanoseconds, 1 },
    };
    JSValue c = tmp_register(ctx, temporal, "Instant", tmp_instant_ctor, 1,
                             instant_m, G_N_ELEMENTS(instant_m),
                             instant_s, G_N_ELEMENTS(instant_s));
    JS_FreeValue(ctx, c);

    static const tmp_method date_m[] = {
        { "toString", tmp_date_toString, 0 },
        { "add", tmp_date_add, 1 },
        { "subtract", tmp_date_subtract, 1 },
        { "with", tmp_date_with, 1 },
        { "until", tmp_date_until, 1 },
        { "since", tmp_date_since, 1 },
        { "equals", tmp_date_equals, 1 },
        { "toPlainDateTime", tmp_date_toPlainDateTime, 1 },
    };
    static const tmp_method date_s[] = {
        { "from", tmp_date_from, 1 },
        { "compare", tmp_date_compare, 2 },
    };
    c = tmp_register(ctx, temporal, "PlainDate", tmp_date_ctor, 3,
                     date_m, G_N_ELEMENTS(date_m), date_s, G_N_ELEMENTS(date_s));
    JS_FreeValue(ctx, c);

    static const tmp_method time_m[] = {
        { "toString", tmp_time_toString, 0 },
        { "add", tmp_time_add, 1 },
        { "subtract", tmp_time_subtract, 1 },
    };
    static const tmp_method time_s[] = {
        { "from", tmp_time_from, 1 },
    };
    c = tmp_register(ctx, temporal, "PlainTime", tmp_time_ctor, 0,
                     time_m, G_N_ELEMENTS(time_m), time_s, G_N_ELEMENTS(time_s));
    JS_FreeValue(ctx, c);

    static const tmp_method datetime_m[] = {
        { "toString", tmp_datetime_toString, 0 },
        { "add", tmp_datetime_add, 1 },
        { "subtract", tmp_datetime_subtract, 1 },
        { "toPlainDate", tmp_datetime_toPlainDate, 0 },
        { "toPlainTime", tmp_datetime_toPlainTime, 0 },
    };
    static const tmp_method datetime_s[] = {
        { "from", tmp_datetime_from, 1 },
    };
    c = tmp_register(ctx, temporal, "PlainDateTime", tmp_datetime_ctor, 3,
                     datetime_m, G_N_ELEMENTS(datetime_m),
                     datetime_s, G_N_ELEMENTS(datetime_s));
    JS_FreeValue(ctx, c);

    static const tmp_method ym_m[] = {
        { "toString", tmp_yearmonth_toString, 0 },
    };
    static const tmp_method ym_s[] = {
        { "from", tmp_yearmonth_from, 1 },
    };
    c = tmp_register(ctx, temporal, "PlainYearMonth", tmp_yearmonth_ctor, 2,
                     ym_m, G_N_ELEMENTS(ym_m), ym_s, G_N_ELEMENTS(ym_s));
    JS_FreeValue(ctx, c);

    static const tmp_method md_m[] = {
        { "toString", tmp_monthday_toString, 0 },
    };
    static const tmp_method md_s[] = {
        { "from", tmp_monthday_from, 1 },
    };
    c = tmp_register(ctx, temporal, "PlainMonthDay", tmp_monthday_ctor, 2,
                     md_m, G_N_ELEMENTS(md_m), md_s, G_N_ELEMENTS(md_s));
    JS_FreeValue(ctx, c);

    static const tmp_method zoned_m[] = {
        { "toString", tmp_zoned_toString, 0 },
        { "toInstant", tmp_zoned_toInstant, 0 },
        { "toPlainDateTime", tmp_zoned_toPlainDateTime, 0 },
    };
    static const tmp_method zoned_s[] = {
        { "from", tmp_zoned_from, 1 },
    };
    c = tmp_register(ctx, temporal, "ZonedDateTime", tmp_zoned_ctor, 2,
                     zoned_m, G_N_ELEMENTS(zoned_m), zoned_s, G_N_ELEMENTS(zoned_s));
    JS_FreeValue(ctx, c);

    static const tmp_method dur_m[] = {
        { "toString", tmp_duration_toString, 0 },
        { "add", tmp_duration_add, 1 },
        { "subtract", tmp_duration_subtract, 1 },
        { "negated", tmp_duration_negated, 0 },
        { "abs", tmp_duration_abs, 0 },
    };
    static const tmp_method dur_s[] = {
        { "from", tmp_duration_from, 1 },
    };
    c = tmp_register(ctx, temporal, "Duration", tmp_duration_ctor, 10,
                     dur_m, G_N_ELEMENTS(dur_m), dur_s, G_N_ELEMENTS(dur_s));
    JS_FreeValue(ctx, c);

    JSValue now = JS_NewObject(ctx);
    tmp_bind(ctx, now, "instant", tmp_now_instant, 0);
    tmp_bind(ctx, now, "zonedDateTimeISO", tmp_now_zdt, 0);
    tmp_bind(ctx, now, "plainDateTimeISO", tmp_now_pdt, 0);
    tmp_bind(ctx, now, "plainDateISO", tmp_now_pd, 0);
    tmp_bind(ctx, now, "plainTimeISO", tmp_now_pt, 0);
    tmp_bind(ctx, now, "timeZoneId", tmp_now_tz, 0);
    JS_SetPropertyStr(ctx, temporal, "Now", now);

    JS_SetPropertyStr(ctx, global, "Temporal", temporal);
}
