/* Nordstjernen — DOM data structure.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "dom.h"

#include "datetime.h"

#include <errno.h>
#include <math.h>
#include <string.h>

static void nd_class_set_clear(nd_node *el);

static gboolean
nd_str_is_ascii_lower(const char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') return FALSE;
    return TRUE;
}

int
nd_parse_int(const char *s, int dflt, int min_v, int max_v)
{
    if (!s || !*s) return dflt;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return dflt;
    errno = 0;
    char *end = NULL;
    gint64 v = g_ascii_strtoll(s, &end, 10);
    if (end == s) return dflt;
    if (errno == ERANGE) v = (v < 0) ? min_v : max_v;
    if (v < (gint64)min_v) v = min_v;
    if (v > (gint64)max_v) v = max_v;
    return (int)v;
}

typedef enum {
    ND_FORM_INPUT_OTHER,
    ND_FORM_INPUT_NUMBER,
    ND_FORM_INPUT_RANGE,
    ND_FORM_INPUT_DATE,
    ND_FORM_INPUT_MONTH,
    ND_FORM_INPUT_WEEK,
    ND_FORM_INPUT_TIME,
    ND_FORM_INPUT_DATETIME,
} nd_form_input_kind;

static nd_form_input_kind
nd_form_input_kind_of_type(const char *type)
{
    if (!type) return ND_FORM_INPUT_OTHER;
    if (!g_ascii_strcasecmp(type, "number"))         return ND_FORM_INPUT_NUMBER;
    if (!g_ascii_strcasecmp(type, "range"))          return ND_FORM_INPUT_RANGE;
    if (!g_ascii_strcasecmp(type, "date"))           return ND_FORM_INPUT_DATE;
    if (!g_ascii_strcasecmp(type, "month"))          return ND_FORM_INPUT_MONTH;
    if (!g_ascii_strcasecmp(type, "week"))           return ND_FORM_INPUT_WEEK;
    if (!g_ascii_strcasecmp(type, "time"))           return ND_FORM_INPUT_TIME;
    if (!g_ascii_strcasecmp(type, "datetime-local")) return ND_FORM_INPUT_DATETIME;
    return ND_FORM_INPUT_OTHER;
}

gboolean
nd_input_type_has_number_value(const char *type)
{
    return nd_form_input_kind_of_type(type) != ND_FORM_INPUT_OTHER;
}

gboolean
nd_input_type_supports_readonly(const char *type)
{
    if (!type || !*type) return TRUE;
    return g_ascii_strcasecmp(type, "text") == 0 ||
           g_ascii_strcasecmp(type, "search") == 0 ||
           g_ascii_strcasecmp(type, "url") == 0 ||
           g_ascii_strcasecmp(type, "tel") == 0 ||
           g_ascii_strcasecmp(type, "email") == 0 ||
           g_ascii_strcasecmp(type, "password") == 0 ||
           g_ascii_strcasecmp(type, "date") == 0 ||
           g_ascii_strcasecmp(type, "month") == 0 ||
           g_ascii_strcasecmp(type, "week") == 0 ||
           g_ascii_strcasecmp(type, "time") == 0 ||
           g_ascii_strcasecmp(type, "datetime-local") == 0 ||
           g_ascii_strcasecmp(type, "number") == 0;
}

gboolean
nd_input_type_supports_text_constraints(const char *type)
{
    if (!type || !*type) return TRUE;
    return g_ascii_strcasecmp(type, "text") == 0 ||
           g_ascii_strcasecmp(type, "search") == 0 ||
           g_ascii_strcasecmp(type, "url") == 0 ||
           g_ascii_strcasecmp(type, "tel") == 0 ||
           g_ascii_strcasecmp(type, "email") == 0 ||
           g_ascii_strcasecmp(type, "password") == 0;
}

gboolean
nd_form_control_readonly_bars_validation(const nd_node *control)
{
    if (!control || control->kind != ND_NODE_ELEMENT || !control->name)
        return FALSE;
    if (!nd_element_get_attr(control, "readonly")) return FALSE;
    if (strcmp(control->name, "textarea") == 0) return TRUE;
    if (strcmp(control->name, "input") != 0) return FALSE;
    return nd_input_type_supports_readonly(nd_element_get_attr(control, "type"));
}

gboolean
nd_form_control_length_limits_apply(const nd_node *control)
{
    if (!control || control->kind != ND_NODE_ELEMENT || !control->name)
        return FALSE;
    if (strcmp(control->name, "textarea") == 0) return TRUE;
    if (strcmp(control->name, "input") != 0) return FALSE;
    return nd_input_type_supports_text_constraints(nd_element_get_attr(control, "type"));
}

gboolean
nd_form_control_supports_required(const nd_node *control)
{
    if (!control || control->kind != ND_NODE_ELEMENT || !control->name)
        return FALSE;
    if (strcmp(control->name, "textarea") == 0 ||
        strcmp(control->name, "select") == 0)
        return TRUE;
    if (strcmp(control->name, "input") != 0) return FALSE;
    const char *type = nd_element_get_attr(control, "type");
    if (!type || !*type) return TRUE;
    return g_ascii_strcasecmp(type, "hidden") != 0 &&
           g_ascii_strcasecmp(type, "range") != 0 &&
           g_ascii_strcasecmp(type, "color") != 0 &&
           g_ascii_strcasecmp(type, "submit") != 0 &&
           g_ascii_strcasecmp(type, "image") != 0 &&
           g_ascii_strcasecmp(type, "reset") != 0 &&
           g_ascii_strcasecmp(type, "button") != 0;
}

static gboolean
nd_form_parse_finite_double(const char *v, double *out)
{
    if (!v || !*v) return FALSE;
    char *end = NULL;
    double d = g_ascii_strtod(v, &end);
    if (!end || end == v) return FALSE;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0' || !isfinite(d)) return FALSE;
    if (out) *out = d;
    return TRUE;
}


gboolean
nd_input_value_to_number(const char *type, const char *value, double *out)
{
    if (!value || !*value) return FALSE;
    nd_form_input_kind kind = nd_form_input_kind_of_type(type);
    int y, m, d, ms;
    switch (kind) {
    case ND_FORM_INPUT_NUMBER:
    case ND_FORM_INPUT_RANGE:
        return nd_form_parse_finite_double(value, out);
    case ND_FORM_INPUT_DATE: {
        const char *p = nd_dt_rd_date(value, &y, &m, &d);
        if (!p || *p != '\0') return FALSE;
        if (out) *out = (double)nd_dt_days_from_civil(y, m, d) * 86400000.0;
        return TRUE;
    }
    case ND_FORM_INPUT_MONTH: {
        const char *p = nd_dt_rd_digits(value, 4, 9, &y);
        if (!p || *p != '-') return FALSE;
        p++;
        p = nd_dt_rd_digits(p, 2, 2, &m);
        if (!p || *p != '\0' || y < 1 || m < 1 || m > 12) return FALSE;
        if (out) *out = (double)((y - 1970) * 12 + (m - 1));
        return TRUE;
    }
    case ND_FORM_INPUT_WEEK: {
        const char *p = nd_dt_rd_digits(value, 4, 9, &y);
        if (!p || *p != '-' || *(p + 1) != 'W') return FALSE;
        p += 2;
        int w;
        p = nd_dt_rd_digits(p, 2, 2, &w);
        if (!p || *p != '\0' || y < 1 || w < 1 ||
            w > nd_dt_iso_weeks_in_year(y))
            return FALSE;
        long monday = nd_dt_iso_week1_monday(y) + (long)(w - 1) * 7;
        if (out) *out = (double)monday * 86400000.0;
        return TRUE;
    }
    case ND_FORM_INPUT_TIME: {
        const char *p = nd_dt_rd_time(value, &ms);
        if (!p || *p != '\0') return FALSE;
        if (out) *out = (double)ms;
        return TRUE;
    }
    case ND_FORM_INPUT_DATETIME: {
        const char *p = nd_dt_rd_date(value, &y, &m, &d);
        if (!p || (*p != 'T' && *p != ' ')) return FALSE;
        p++;
        p = nd_dt_rd_time(p, &ms);
        if (!p || *p != '\0') return FALSE;
        if (out) *out = (double)nd_dt_days_from_civil(y, m, d) * 86400000.0 + ms;
        return TRUE;
    }
    default:
        return FALSE;
    }
}

gboolean
nd_input_value_range_state(const nd_node *input, const char *value,
                           gboolean *underflow, gboolean *overflow)
{
    if (underflow) *underflow = FALSE;
    if (overflow) *overflow = FALSE;
    if (!input || !nd_node_is_element_named(input, "input")) return FALSE;
    const char *type = nd_element_get_attr(input, "type");
    double v;
    if (!nd_input_value_to_number(type, value, &v)) return FALSE;
    double bound;
    const char *min = nd_element_get_attr(input, "min");
    const char *max = nd_element_get_attr(input, "max");
    if (nd_input_value_to_number(type, min, &bound) && v < bound) {
        if (underflow) *underflow = TRUE;
    }
    if (nd_input_value_to_number(type, max, &bound) && v > bound) {
        if (overflow) *overflow = TRUE;
    }
    return TRUE;
}

static double
nd_form_step_scale(nd_form_input_kind kind)
{
    switch (kind) {
    case ND_FORM_INPUT_DATE:     return 86400000.0;
    case ND_FORM_INPUT_WEEK:     return 604800000.0;
    case ND_FORM_INPUT_TIME:
    case ND_FORM_INPUT_DATETIME: return 1000.0;
    default:                     return 1.0;
    }
}

static double
nd_form_default_step(nd_form_input_kind kind)
{
    switch (kind) {
    case ND_FORM_INPUT_TIME:
    case ND_FORM_INPUT_DATETIME:
        return 60.0;
    default:
        return 1.0;
    }
}

gboolean
nd_input_value_step_mismatch(const nd_node *input, const char *value)
{
    if (!input || !nd_node_is_element_named(input, "input")) return FALSE;
    const char *type = nd_element_get_attr(input, "type");
    nd_form_input_kind kind = nd_form_input_kind_of_type(type);
    if (kind == ND_FORM_INPUT_OTHER) return FALSE;
    const char *step_attr = nd_element_get_attr(input, "step");
    if (step_attr && g_ascii_strcasecmp(step_attr, "any") == 0)
        return FALSE;
    double v;
    if (!nd_input_value_to_number(type, value, &v)) return FALSE;
    double step_value = nd_form_default_step(kind);
    double parsed;
    if (nd_form_parse_finite_double(step_attr, &parsed) && parsed > 0)
        step_value = parsed;
    double step = step_value * nd_form_step_scale(kind);
    if (step <= 0 || !isfinite(step)) return FALSE;
    double base = kind == ND_FORM_INPUT_WEEK ? -259200000.0 : 0.0;
    if (nd_input_value_to_number(type, nd_element_get_attr(input, "min"), &parsed))
        base = parsed;
    double q = (v - base) / step;
    double nearest = round(q);
    double scale = fabs(q) > 1.0 ? fabs(q) : 1.0;
    return fabs(q - nearest) > 1e-7 * scale;
}

static gboolean
nd_input_type_is(const nd_node *node, const char *want)
{
    if (!nd_node_is_element_named(node, "input")) return FALSE;
    const char *type = nd_element_get_attr(node, "type");
    return type && g_ascii_strcasecmp(type, want) == 0;
}

static gboolean
nd_radio_group_has_checked(const nd_node *scan, const nd_node *doc,
                           const nd_node *owner,
                           const char *name, int depth)
{
    if (!scan || depth >= 512) return FALSE;
    if (nd_input_type_is(scan, "radio")) {
        const char *scan_name = nd_element_get_attr(scan, "name");
        if (!scan_name) scan_name = "";
        if (strcmp(scan_name, name) == 0 &&
            nd_form_owner(scan, doc) == owner &&
            nd_element_get_attr(scan, "checked"))
            return TRUE;
    }
    for (const nd_node *c = scan->first_child; c; c = c->next_sibling)
        if (nd_radio_group_has_checked(c, doc, owner, name, depth + 1))
            return TRUE;
    return FALSE;
}

gboolean
nd_form_control_value_missing(const nd_node *control, const char *value,
                              const nd_node *doc)
{
    if (!control || control->kind != ND_NODE_ELEMENT || !control->name)
        return FALSE;
    if (nd_input_type_is(control, "checkbox"))
        return nd_element_get_attr(control, "checked") == NULL;
    if (nd_input_type_is(control, "radio")) {
        const char *name = nd_element_get_attr(control, "name");
        if (!name) name = "";
        const nd_node *root = doc ? doc : nd_node_root(control);
        const nd_node *owner = nd_form_owner(control, root);
        return !nd_radio_group_has_checked(root ? root : control,
                                           root, owner, name, 0);
    }
    return !value || !*value;
}

static gboolean
nd_email_token_valid(const char *start, gsize len)
{
    char *token = g_strndup(start, len);
    char *trimmed = g_strstrip(token);
    gboolean ok = FALSE;
    if (*trimmed) {
        for (const char *p = trimmed; *p; p++) {
            if (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n' ||
                *p == '\r' || *p == '\f') {
                g_free(token);
                return FALSE;
            }
        }
        const char *at = strchr(trimmed, '@');
        const char *dot = at ? strchr(at + 1, '.') : NULL;
        ok = at && at != trimmed && !strchr(at + 1, '@') &&
             dot && dot != at + 1 && *(dot + 1) != '\0';
    }
    g_free(token);
    return ok;
}

gboolean
nd_input_email_value_valid(const nd_node *input, const char *value)
{
    if (!value || !*value) return TRUE;
    if (!input || !nd_element_get_attr(input, "multiple"))
        return nd_email_token_valid(value, strlen(value));
    const char *p = value;
    while (TRUE) {
        const char *comma = strchr(p, ',');
        gsize len = comma ? (gsize)(comma - p) : strlen(p);
        if (!nd_email_token_valid(p, len)) return FALSE;
        if (!comma) return TRUE;
        p = comma + 1;
    }
}

gboolean
nd_ce_attr_enables(const char *ce)
{
    return ce && (!*ce ||
                  g_ascii_strcasecmp(ce, "true") == 0 ||
                  g_ascii_strcasecmp(ce, "plaintext-only") == 0);
}

gboolean
nd_node_is_text_input(const nd_node *n)
{
    if (!n || n->kind != ND_NODE_ELEMENT || !n->name) return FALSE;
    if (strcmp(n->name, "textarea") == 0) return TRUE;
    if (strcmp(n->name, "input") != 0) return FALSE;
    const char *type = nd_element_get_attr(n, "type");
    if (!type || !*type) return TRUE;
    return g_ascii_strcasecmp(type, "text")     == 0 ||
           g_ascii_strcasecmp(type, "search")   == 0 ||
           g_ascii_strcasecmp(type, "email")    == 0 ||
           g_ascii_strcasecmp(type, "url")      == 0 ||
           g_ascii_strcasecmp(type, "tel")      == 0 ||
           g_ascii_strcasecmp(type, "number")   == 0 ||
           g_ascii_strcasecmp(type, "password") == 0;
}

gboolean
nd_node_is_contenteditable_host(const nd_node *n)
{
    if (!n || n->kind != ND_NODE_ELEMENT || !n->name) return FALSE;
    if (strcmp(n->name, "input") == 0 || strcmp(n->name, "textarea") == 0)
        return FALSE;
    return nd_ce_attr_enables(nd_element_get_attr(n, "contenteditable"));
}

gboolean
nd_node_is_editable(const nd_node *n)
{
    return nd_node_is_text_input(n) || nd_node_is_contenteditable_host(n);
}

const char *
nd_node_editable_value(const nd_node *n)
{
    if (!n) return "";
    if ((n->name && strcmp(n->name, "textarea") == 0) ||
        nd_node_is_contenteditable_host(n)) {
        for (const nd_node *c = n->first_child; c; c = c->next_sibling)
            if (c->kind == ND_NODE_TEXT && c->text)
                return c->text;
        return "";
    }
    const char *v = nd_element_get_attr(n, "value");
    return v ? v : "";
}

void
nd_node_set_editable_value(nd_node *n, const char *value)
{
    if (!n) return;
    if ((n->name && strcmp(n->name, "textarea") == 0) ||
        nd_node_is_contenteditable_host(n)) {
        for (nd_node *c = n->first_child; c; ) {
            nd_node *next = c->next_sibling;
            nd_node_remove(c);
            nd_node_free(c);
            c = next;
        }
        nd_node_append_child(n, nd_node_new_text(g_strdup(value ? value : "")));
    } else {
        nd_element_set_attr(n, "value", value ? value : "");
    }
}

void
nd_node_flatten_editable(nd_node *n)
{
    if (!nd_node_is_contenteditable_host(n)) return;
    char *txt = nd_node_collect_text(n);
    nd_node_set_editable_value(n, txt ? txt : "");
    g_free(txt);
}

gboolean
nd_node_is_numeric_input(const nd_node *control)
{
    if (!control || control->kind != ND_NODE_ELEMENT || !control->name)
        return FALSE;
    if (strcmp(control->name, "input") != 0) return FALSE;
    const char *type = nd_element_get_attr(control, "type");
    return type && g_ascii_strcasecmp(type, "number") == 0;
}

char *
nd_numeric_filter_insert(const char *insert, gsize len, gsize *out_len)
{
    GString *s = g_string_sized_new(len);
    for (gsize i = 0; insert && i < len; i++) {
        char c = insert[i];
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' ||
            c == 'e' || c == 'E')
            g_string_append_c(s, c);
    }
    if (out_len) *out_len = s->len;
    return g_string_free(s, FALSE);
}


static nd_node *
nd_node_new(nd_node_kind kind)
{
    nd_node *n = g_new0(nd_node, 1);
    n->kind = kind;
    return n;
}

nd_node *
nd_node_new_document(void)
{
    return nd_node_new(ND_NODE_DOCUMENT);
}

nd_node *
nd_node_new_element(char *name)
{
    nd_node *n = nd_node_new(ND_NODE_ELEMENT);
    n->name = name;
    n->flags |= ND_NODE_OWN_NAME;
    return n;
}

nd_node *
nd_node_new_text(char *text)
{
    nd_node *n = nd_node_new(ND_NODE_TEXT);
    n->text = text;
    n->flags |= ND_NODE_OWN_TEXT;
    return n;
}

nd_node *
nd_node_new_comment(char *text)
{
    nd_node *n = nd_node_new(ND_NODE_COMMENT);
    n->text = text;
    n->flags |= ND_NODE_OWN_TEXT;
    return n;
}

void
nd_node_set_name_borrow(nd_node *n, const char *name)
{
    if (!n) return;
    if (n->flags & ND_NODE_OWN_NAME)
        g_free(n->name);
    n->name = (char *)name;
    n->flags &= ~ND_NODE_OWN_NAME;
}

void
nd_node_set_text_borrow(nd_node *n, const char *text)
{
    if (!n) return;
    if (n->flags & ND_NODE_OWN_TEXT)
        g_free(n->text);
    n->text = (char *)text;
    n->flags &= ~ND_NODE_OWN_TEXT;
}

void
nd_node_replace_text_owned(nd_node *n, char *text)
{
    if (!n) {
        g_free(text);
        return;
    }
    if (n->flags & ND_NODE_OWN_TEXT)
        g_free(n->text);
    n->text = text;
    n->flags |= ND_NODE_OWN_TEXT;
}

void
nd_node_own_strings_deep(nd_node *n)
{
    if (!n) return;
    nd_class_set_clear(n);
    if (n->name && !(n->flags & ND_NODE_OWN_NAME)) {
        n->name = g_strdup(n->name);
        n->flags |= ND_NODE_OWN_NAME;
    }
    if (n->text && !(n->flags & ND_NODE_OWN_TEXT)) {
        n->text = g_strdup(n->text);
        n->flags |= ND_NODE_OWN_TEXT;
    }
    for (nd_attr *a = n->attrs; a; a = a->next) {
        if (a->name && !(a->flags & ND_ATTR_OWN_NAME)) {
            a->name = g_strdup(a->name);
            a->flags |= ND_ATTR_OWN_NAME;
        }
        if (a->value && !(a->flags & ND_ATTR_OWN_VALUE)) {
            a->value = g_strdup(a->value);
            a->flags |= ND_ATTR_OWN_VALUE;
        }
    }
    for (nd_node *c = n->first_child; c; c = c->next_sibling)
        nd_node_own_strings_deep(c);
}

void
nd_element_append_attr_borrow(nd_node *el, const char *name, const char *value)
{
    if (!el || el->kind != ND_NODE_ELEMENT || !name) return;
    if (el->class_set) nd_class_set_clear(el);
    nd_attr *a = g_new0(nd_attr, 1);
    a->name  = (char *)name;
    a->value = (char *)(value ? value : "");
    a->flags = 0;
    nd_attr *tail = NULL;
    for (nd_attr *cur = el->attrs; cur; cur = cur->next) tail = cur;
    if (tail) tail->next = a;
    else      el->attrs = a;
}

void
nd_node_attach_backing(nd_node *root, void *backing, void (*destroy)(void *))
{
    if (!root) {
        if (backing && destroy) destroy(backing);
        return;
    }
    if (root->backing && root->backing_free)
        root->backing_free(root->backing);
    root->backing = backing;
    root->backing_free = destroy;
}

static void
nd_attr_free(nd_attr *a)
{
    while (a) {
        nd_attr *next = a->next;
        if (a->flags & ND_ATTR_OWN_NAME)  g_free(a->name);
        if (a->flags & ND_ATTR_OWN_VALUE) g_free(a->value);
        g_free(a);
        a = next;
    }
}

typedef struct nd_class_set {
    guint n;
    struct { const char *p; guint len; } tok[];
} nd_class_set;

static nd_class_set g_nd_empty_class_set;

static inline gboolean
nd_clsset_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static nd_class_set *
nd_class_set_build(const char *cls)
{
    guint n = 0;
    for (const char *s = cls; *s; ) {
        while (*s && nd_clsset_ws(*s)) s++;
        if (!*s) break;
        while (*s && !nd_clsset_ws(*s)) s++;
        n++;
    }
    if (n == 0) return &g_nd_empty_class_set;
    nd_class_set *cs = g_malloc(sizeof *cs + (gsize)n * sizeof cs->tok[0]);
    cs->n = 0;
    for (const char *s = cls; *s; ) {
        while (*s && nd_clsset_ws(*s)) s++;
        if (!*s) break;
        const char *t = s;
        while (*s && !nd_clsset_ws(*s)) s++;
        cs->tok[cs->n].p = t;
        cs->tok[cs->n].len = (guint)(s - t);
        cs->n++;
    }
    return cs;
}

static void
nd_class_set_clear(nd_node *el)
{
    if (el->class_set && el->class_set != &g_nd_empty_class_set)
        g_free(el->class_set);
    el->class_set = NULL;
}

gboolean
nd_node_has_class(const nd_node *el, const char *name, gsize len)
{
    if (!el || el->kind != ND_NODE_ELEMENT) return FALSE;
    nd_class_set *cs = el->class_set;
    if (!cs) {
        const char *cls = nd_element_get_attr(el, "class");
        cs = (cls && *cls) ? nd_class_set_build(cls) : &g_nd_empty_class_set;
        ((nd_node *)el)->class_set = cs;
    }
    for (guint i = 0; i < cs->n; i++)
        if (cs->tok[i].len == len && memcmp(cs->tok[i].p, name, len) == 0)
            return TRUE;
    return FALSE;
}

void
nd_node_free(nd_node *node)
{
    if (!node)
        return;

    GPtrArray *stack = g_ptr_array_new();
    g_ptr_array_add(stack, node);

    while (stack->len > 0) {
        nd_node *cur = g_ptr_array_index(stack, stack->len - 1);
        if (cur->first_child) {
            nd_node *c = cur->first_child;
            cur->first_child = NULL;
            while (c) {
                nd_node *next = c->next_sibling;
                c->next_sibling = NULL;
                c->parent = NULL;
                g_ptr_array_add(stack, c);
                c = next;
            }
            continue;
        }
        g_ptr_array_set_size(stack, stack->len - 1);
        if (cur->js_invalidate)
            cur->js_invalidate(cur);
        if (cur->flags & ND_NODE_OWN_NAME) g_free(cur->name);
        if (cur->flags & ND_NODE_OWN_TEXT) g_free(cur->text);
        nd_class_set_clear(cur);
        nd_attr_free(cur->attrs);
        if (cur->backing && cur->backing_free)
            cur->backing_free(cur->backing);
        if (cur->id_index) {
            g_hash_table_destroy(cur->id_index);
            cur->id_index = NULL;
        }
        if (cur->class_index) {
            g_hash_table_destroy(cur->class_index);
            cur->class_index = NULL;
        }
        if (cur->tag_index) {
            g_hash_table_destroy(cur->tag_index);
            cur->tag_index = NULL;
        }
        g_free(cur);
    }
    g_ptr_array_free(stack, TRUE);
}

static void
nd_node_detach(nd_node *child)
{
    nd_node *p = child->parent;
    if (!p)
        return;
    if (child->prev_sibling)
        child->prev_sibling->next_sibling = child->next_sibling;
    else
        p->first_child = child->next_sibling;
    if (child->next_sibling)
        child->next_sibling->prev_sibling = child->prev_sibling;
    else
        p->last_child = child->prev_sibling;
    child->parent = NULL;
    child->prev_sibling = NULL;
    child->next_sibling = NULL;
}

void
nd_node_append_child(nd_node *parent, nd_node *child)
{
    g_return_if_fail(parent != NULL);
    g_return_if_fail(child != NULL);

    nd_node_detach(child);
    child->parent = parent;
    child->prev_sibling = parent->last_child;
    if (parent->last_child)
        parent->last_child->next_sibling = child;
    else
        parent->first_child = child;
    parent->last_child = child;
}

void
nd_node_remove(nd_node *child)
{
    nd_node_detach(child);
}

void
nd_element_set_attr(nd_node *el, const char *name, const char *value)
{
    g_return_if_fail(el != NULL);
    g_return_if_fail(el->kind == ND_NODE_ELEMENT);
    g_return_if_fail(name != NULL);

    if (el->class_set && g_ascii_strcasecmp(name, "class") == 0)
        nd_class_set_clear(el);

    nd_attr *tail = NULL;
    for (nd_attr *a = el->attrs; a; a = a->next) {
        if (g_ascii_strcasecmp(a->name, name) == 0) {
            if (a->flags & ND_ATTR_OWN_VALUE) g_free(a->value);
            a->value = g_strdup(value ? value : "");
            a->flags |= ND_ATTR_OWN_VALUE;
            return;
        }
        tail = a;
    }
    nd_attr *a = g_new0(nd_attr, 1);
    a->name = g_strdup(name);
    a->value = g_strdup(value ? value : "");
    a->flags = ND_ATTR_OWN_NAME | ND_ATTR_OWN_VALUE;
    a->next = NULL;
    if (tail) tail->next = a;
    else      el->attrs = a;
}

void
nd_element_remove_attr(nd_node *el, const char *name)
{
    if (!el || el->kind != ND_NODE_ELEMENT || !name) return;
    if (el->class_set && g_ascii_strcasecmp(name, "class") == 0)
        nd_class_set_clear(el);
    nd_attr **link = &el->attrs;
    while (*link) {
        if (g_ascii_strcasecmp((*link)->name, name) == 0) {
            nd_attr *dead = *link;
            *link = dead->next;
            if (dead->flags & ND_ATTR_OWN_NAME)  g_free(dead->name);
            if (dead->flags & ND_ATTR_OWN_VALUE) g_free(dead->value);
            g_free(dead);
            return;
        }
        link = &(*link)->next;
    }
}

#define ND_DOM_MAX_DEPTH 512

static nd_node *
nd_node_clone_depth(const nd_node *src, gboolean deep, int depth)
{
    if (!src || depth >= ND_DOM_MAX_DEPTH) return NULL;
    nd_node *out = NULL;
    switch (src->kind) {
    case ND_NODE_ELEMENT:
        out = nd_node_new_element(src->name ? g_strdup(src->name) : g_strdup(""));
        for (const nd_attr *a = src->attrs; a; a = a->next)
            nd_element_set_attr(out, a->name ? a->name : "",
                                a->value ? a->value : "");
        break;
    case ND_NODE_TEXT:
        out = nd_node_new_text(g_strdup(src->text ? src->text : ""));
        break;
    case ND_NODE_DOCUMENT:
    case ND_NODE_DOCTYPE:
    case ND_NODE_COMMENT:
        out = nd_node_new(src->kind);
        if (src->text) {
            out->text = g_strdup(src->text);
            out->flags |= ND_NODE_OWN_TEXT;
        }
        if (src->name) {
            out->name = g_strdup(src->name);
            out->flags |= ND_NODE_OWN_NAME;
        }
        break;
    }
    if (out) out->flags |= src->flags & ND_NODE_FRAGMENT;
    if (deep && out)
        for (const nd_node *c = src->first_child; c; c = c->next_sibling)
            nd_node_append_child(out, nd_node_clone_depth(c, TRUE, depth + 1));
    return out;
}

nd_node *
nd_node_clone(const nd_node *src, gboolean deep)
{
    return nd_node_clone_depth(src, deep, 0);
}

const char *
nd_element_get_attr(const nd_node *el, const char *name)
{
    if (!el || el->kind != ND_NODE_ELEMENT || !name)
        return NULL;
    if (nd_str_is_ascii_lower(name)) {
        char c0 = name[0];
        for (const nd_attr *a = el->attrs; a; a = a->next) {
            const char *an = a->name;
            if (!an) continue;
            char ac = an[0];
            if (ac == c0) {
                if (strcmp(an, name) == 0) return a->value;
                continue;
            }
            if (ac >= 'A' && ac <= 'Z' && (ac + 32) == c0) {
                if (g_ascii_strcasecmp(an, name) == 0) return a->value;
            }
        }
        return NULL;
    }
    for (const nd_attr *a = el->attrs; a; a = a->next) {
        if (g_ascii_strcasecmp(a->name, name) == 0)
            return a->value;
    }
    return NULL;
}

gboolean
nd_node_is_element_named(const nd_node *n, const char *tag)
{
    return n && n->kind == ND_NODE_ELEMENT && n->name && tag &&
           strcmp(n->name, tag) == 0;
}

const nd_node *
nd_node_root(const nd_node *n)
{
    if (!n) return NULL;
    while (n->parent) n = n->parent;
    return n;
}

static nd_node *
nd_node_find_first_element_depth(const nd_node *root, const char *tag, int depth)
{
    if (!root || !tag || depth >= ND_DOM_MAX_DEPTH) return NULL;
    if (nd_node_is_element_named(root, tag))
        return (nd_node *)root;
    for (const nd_node *c = root->first_child; c; c = c->next_sibling) {
        nd_node *m = nd_node_find_first_element_depth(c, tag, depth + 1);
        if (m) return m;
    }
    return NULL;
}

nd_node *
nd_node_find_first_element(const nd_node *root, const char *tag)
{
    if (root && tag && *tag && root->tag_index) {
        GPtrArray *list = nd_doc_tag_index_lookup(root, tag);
        if (list && list->len > 0) return g_ptr_array_index(list, 0);
        return NULL;
    }
    return nd_node_find_first_element_depth(root, tag, 0);
}

static nd_node *
nd_node_find_by_id_depth(const nd_node *root, const char *id, int depth)
{
    if (!root || !id || depth >= ND_DOM_MAX_DEPTH) return NULL;
    if (root->kind == ND_NODE_ELEMENT) {
        const char *eid = nd_element_get_attr(root, "id");
        if (eid && strcmp(eid, id) == 0) return (nd_node *)root;
    }
    if (nd_node_is_element_named(root, "template")) return NULL;
    for (const nd_node *c = root->first_child; c; c = c->next_sibling) {
        nd_node *m = nd_node_find_by_id_depth(c, id, depth + 1);
        if (m) return m;
    }
    return NULL;
}

static void
nd_doc_id_index_register_subtree(GHashTable *map, nd_node *n, int depth)
{
    if (!n || depth >= ND_DOM_MAX_DEPTH) return;
    if (n->kind == ND_NODE_ELEMENT) {
        const char *eid = nd_element_get_attr(n, "id");
        if (eid && *eid && !g_hash_table_contains(map, eid))
            g_hash_table_insert(map, g_strdup(eid), n);
    }
    if (nd_node_is_element_named(n, "template")) return;
    for (nd_node *c = n->first_child; c; c = c->next_sibling)
        nd_doc_id_index_register_subtree(map, c, depth + 1);
}

void
nd_doc_id_index_build(nd_node *doc)
{
    if (!doc) return;
    if (doc->id_index) {
        g_hash_table_remove_all(doc->id_index);
    } else {
        doc->id_index = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
    }
    nd_doc_id_index_register_subtree(doc->id_index, doc, 0);
}

void
nd_doc_id_index_register(nd_node *doc, const char *id, nd_node *node)
{
    if (!doc || !doc->id_index || !id || !*id || !node) return;
    if (g_hash_table_contains(doc->id_index, id)) return;
    g_hash_table_insert(doc->id_index, g_strdup(id), node);
}

void
nd_doc_id_index_unregister(nd_node *doc, const char *id, const nd_node *node)
{
    if (!doc || !doc->id_index || !id || !*id) return;
    gpointer cur = g_hash_table_lookup(doc->id_index, id);
    if (cur == node) g_hash_table_remove(doc->id_index, id);
}

static void
nd_doc_id_index_add_subtree(nd_node *doc, nd_node *n, int depth)
{
    if (!n || depth >= ND_DOM_MAX_DEPTH) return;
    if (n->kind == ND_NODE_ELEMENT) {
        const char *eid = nd_element_get_attr(n, "id");
        if (eid && *eid && !g_hash_table_contains(doc->id_index, eid))
            g_hash_table_insert(doc->id_index, g_strdup(eid), n);
    }
    if (nd_node_is_element_named(n, "template")) return;
    for (nd_node *c = n->first_child; c; c = c->next_sibling)
        nd_doc_id_index_add_subtree(doc, c, depth + 1);
}

static void
nd_doc_id_index_remove_subtree(nd_node *doc, nd_node *n, int depth)
{
    if (!n || depth >= ND_DOM_MAX_DEPTH) return;
    if (n->kind == ND_NODE_ELEMENT) {
        const char *eid = nd_element_get_attr(n, "id");
        if (eid && *eid) {
            gpointer cur = g_hash_table_lookup(doc->id_index, eid);
            if (cur == n) g_hash_table_remove(doc->id_index, eid);
        }
    }
    if (nd_node_is_element_named(n, "template")) return;
    for (nd_node *c = n->first_child; c; c = c->next_sibling)
        nd_doc_id_index_remove_subtree(doc, c, depth + 1);
}

void
nd_doc_id_index_subtree_added(nd_node *doc, nd_node *root)
{
    if (!doc || !doc->id_index || !root) return;
    nd_doc_id_index_add_subtree(doc, root, 0);
}

void
nd_doc_id_index_subtree_removed(nd_node *doc, nd_node *root)
{
    if (!doc || !doc->id_index || !root) return;
    nd_doc_id_index_remove_subtree(doc, root, 0);
}

static void
nd_class_array_destroy(gpointer p)
{
    g_ptr_array_free((GPtrArray *)p, TRUE);
}

static void
nd_doc_class_index_add_token(GHashTable *map, const char *tok, gsize tok_len,
                             nd_node *node)
{
    if (tok_len == 0) return;
    char stack[96];
    gchar *key;
    if (tok_len < sizeof(stack)) {
        memcpy(stack, tok, tok_len);
        stack[tok_len] = '\0';
        key = stack;
    } else {
        key = g_strndup(tok, tok_len);
    }
    GPtrArray *arr = g_hash_table_lookup(map, key);
    if (arr) {
        if (key != stack) g_free(key);
        for (guint k = 0; k < arr->len; k++)
            if (g_ptr_array_index(arr, k) == node) return;
        g_ptr_array_add(arr, node);
    } else {
        arr = g_ptr_array_new();
        g_ptr_array_add(arr, node);
        gchar *owned = (key == stack) ? g_strndup(tok, tok_len) : key;
        g_hash_table_insert(map, owned, arr);
    }
}

static void
nd_doc_class_index_remove_token(GHashTable *map, const char *tok, gsize tok_len,
                                nd_node *node)
{
    if (tok_len == 0) return;
    char stack[96];
    gchar *key;
    if (tok_len < sizeof(stack)) {
        memcpy(stack, tok, tok_len);
        stack[tok_len] = '\0';
        key = stack;
    } else {
        key = g_strndup(tok, tok_len);
    }
    GPtrArray *arr = g_hash_table_lookup(map, key);
    if (key != stack) g_free(key);
    if (!arr) return;
    for (guint k = 0; k < arr->len; k++) {
        if (g_ptr_array_index(arr, k) == node) {
            g_ptr_array_remove_index(arr, k);
            break;
        }
    }
}

static gboolean
nd_class_is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

void
nd_doc_class_index_register(nd_node *doc, const char *class_attr, nd_node *node)
{
    if (!doc || !doc->class_index || !class_attr || !node) return;
    const char *p = class_attr;
    while (*p) {
        while (*p && nd_class_is_ws(*p)) p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && !nd_class_is_ws(*p)) p++;
        nd_doc_class_index_add_token(doc->class_index, tok, (gsize)(p - tok), node);
    }
}

void
nd_doc_class_index_unregister(nd_node *doc, const char *class_attr, nd_node *node)
{
    if (!doc || !doc->class_index || !class_attr || !node) return;
    const char *p = class_attr;
    while (*p) {
        while (*p && nd_class_is_ws(*p)) p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && !nd_class_is_ws(*p)) p++;
        nd_doc_class_index_remove_token(doc->class_index, tok, (gsize)(p - tok), node);
    }
}

static void
nd_doc_class_index_add_subtree(nd_node *doc, nd_node *n, int depth)
{
    if (!n || depth >= ND_DOM_MAX_DEPTH) return;
    if (n->kind == ND_NODE_ELEMENT) {
        const char *cls = nd_element_get_attr(n, "class");
        if (cls && *cls) nd_doc_class_index_register(doc, cls, n);
    }
    if (nd_node_is_element_named(n, "template")) return;
    for (nd_node *c = n->first_child; c; c = c->next_sibling)
        nd_doc_class_index_add_subtree(doc, c, depth + 1);
}

static void
nd_doc_class_index_remove_subtree(nd_node *doc, nd_node *n, int depth)
{
    if (!n || depth >= ND_DOM_MAX_DEPTH) return;
    if (n->kind == ND_NODE_ELEMENT) {
        const char *cls = nd_element_get_attr(n, "class");
        if (cls && *cls) nd_doc_class_index_unregister(doc, cls, n);
    }
    if (nd_node_is_element_named(n, "template")) return;
    for (nd_node *c = n->first_child; c; c = c->next_sibling)
        nd_doc_class_index_remove_subtree(doc, c, depth + 1);
}

void
nd_doc_class_index_build(nd_node *doc)
{
    if (!doc) return;
    if (doc->class_index) {
        g_hash_table_remove_all(doc->class_index);
    } else {
        doc->class_index = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, nd_class_array_destroy);
    }
    nd_doc_class_index_add_subtree(doc, doc, 0);
}

void
nd_doc_class_index_subtree_added(nd_node *doc, nd_node *root)
{
    if (!doc || !doc->class_index || !root) return;
    nd_doc_class_index_add_subtree(doc, root, 0);
}

void
nd_doc_class_index_subtree_removed(nd_node *doc, nd_node *root)
{
    if (!doc || !doc->class_index || !root) return;
    nd_doc_class_index_remove_subtree(doc, root, 0);
}

static void
nd_doc_tag_index_add_single(GHashTable *map, const char *tag, nd_node *node)
{
    if (!tag || !*tag) return;
    gboolean is_lower = nd_str_is_ascii_lower(tag);
    GPtrArray *arr = is_lower
        ? g_hash_table_lookup(map, tag)
        : NULL;
    if (!arr && !is_lower) {
        gchar *probe = g_ascii_strdown(tag, -1);
        arr = g_hash_table_lookup(map, probe);
        g_free(probe);
    }
    if (arr) {
        for (guint k = 0; k < arr->len; k++)
            if (g_ptr_array_index(arr, k) == node) return;
        g_ptr_array_add(arr, node);
        return;
    }
    arr = g_ptr_array_new();
    g_ptr_array_add(arr, node);
    g_hash_table_insert(map,
        is_lower ? g_strdup(tag) : g_ascii_strdown(tag, -1), arr);
}

static void
nd_doc_tag_index_remove_single(GHashTable *map, const char *tag, nd_node *node)
{
    if (!tag || !*tag) return;
    GPtrArray *arr;
    if (nd_str_is_ascii_lower(tag)) {
        arr = g_hash_table_lookup(map, tag);
    } else {
        gchar *key = g_ascii_strdown(tag, -1);
        arr = g_hash_table_lookup(map, key);
        g_free(key);
    }
    if (!arr) return;
    for (guint k = 0; k < arr->len; k++) {
        if (g_ptr_array_index(arr, k) == node) {
            g_ptr_array_remove_index(arr, k);
            break;
        }
    }
}

static void
nd_doc_tag_index_add_subtree(nd_node *doc, nd_node *n, int depth)
{
    if (!n || depth >= ND_DOM_MAX_DEPTH) return;
    if (n->kind == ND_NODE_ELEMENT && n->name)
        nd_doc_tag_index_add_single(doc->tag_index, n->name, n);
    if (nd_node_is_element_named(n, "template")) return;
    for (nd_node *c = n->first_child; c; c = c->next_sibling)
        nd_doc_tag_index_add_subtree(doc, c, depth + 1);
}

static void
nd_doc_tag_index_remove_subtree(nd_node *doc, nd_node *n, int depth)
{
    if (!n || depth >= ND_DOM_MAX_DEPTH) return;
    if (n->kind == ND_NODE_ELEMENT && n->name)
        nd_doc_tag_index_remove_single(doc->tag_index, n->name, n);
    if (nd_node_is_element_named(n, "template")) return;
    for (nd_node *c = n->first_child; c; c = c->next_sibling)
        nd_doc_tag_index_remove_subtree(doc, c, depth + 1);
}

void
nd_doc_tag_index_build(nd_node *doc)
{
    if (!doc) return;
    if (doc->tag_index) {
        g_hash_table_remove_all(doc->tag_index);
    } else {
        doc->tag_index = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, nd_class_array_destroy);
    }
    nd_doc_tag_index_add_subtree(doc, doc, 0);
}

void
nd_doc_tag_index_subtree_added(nd_node *doc, nd_node *root)
{
    if (!doc || !doc->tag_index || !root) return;
    nd_doc_tag_index_add_subtree(doc, root, 0);
}

void
nd_doc_tag_index_subtree_removed(nd_node *doc, nd_node *root)
{
    if (!doc || !doc->tag_index || !root) return;
    nd_doc_tag_index_remove_subtree(doc, root, 0);
}

GPtrArray *
nd_doc_tag_index_lookup(const nd_node *doc, const char *tag)
{
    if (!doc || !doc->tag_index || !tag || !*tag) return NULL;
    if (nd_str_is_ascii_lower(tag))
        return g_hash_table_lookup(doc->tag_index, tag);
    gchar *key = g_ascii_strdown(tag, -1);
    GPtrArray *arr = g_hash_table_lookup(doc->tag_index, key);
    g_free(key);
    return arr;
}

nd_node *
nd_node_find_by_id(const nd_node *root, const char *id)
{
    if (!root || !id || !*id) return NULL;
    if (root->id_index) {
        nd_node *hit = g_hash_table_lookup(root->id_index, id);
        if (hit) return hit;
        return NULL;
    }
    return nd_node_find_by_id_depth(root, id, 0);
}

static void
collect_descendant_text_skip_script(const nd_node *n, GString *out, int depth)
{
    if (!n || depth >= ND_DOM_MAX_DEPTH) return;
    for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == ND_NODE_TEXT) {
            if (c->text) g_string_append(out, c->text);
        } else if (c->kind == ND_NODE_ELEMENT && c->name &&
                   g_ascii_strcasecmp(c->name, "script") == 0) {
            continue;
        } else {
            collect_descendant_text_skip_script(c, out, depth + 1);
        }
    }
}

static char *
nd_node_collect_descendant_text_skip_script(const nd_node *root)
{
    GString *out = g_string_new(NULL);
    collect_descendant_text_skip_script(root, out, 0);
    return g_string_free(out, FALSE);
}

static char *
nd_strip_and_collapse_ascii_ws(const char *s)
{
    if (!s) return g_strdup("");
    GString *out = g_string_new(NULL);
    gboolean in_ws = TRUE;
    for (const char *p = s; *p; ) {
        gunichar ch = g_utf8_get_char(p);
        const char *next = g_utf8_next_char(p);
        gboolean is_ws = (ch == 0x09 || ch == 0x0A || ch == 0x0C ||
                          ch == 0x0D || ch == 0x20);
        if (is_ws) {
            if (!in_ws) { g_string_append_c(out, ' '); in_ws = TRUE; }
        } else {
            g_string_append_len(out, p, next - p);
            in_ws = FALSE;
        }
        p = next;
    }
    if (out->len > 0 && out->str[out->len - 1] == ' ')
        g_string_truncate(out, out->len - 1);
    return g_string_free(out, FALSE);
}

char *
nd_option_text_dup(const nd_node *option)
{
    if (!option) return g_strdup("");
    g_autofree char *raw = nd_node_collect_descendant_text_skip_script(option);
    return nd_strip_and_collapse_ascii_ws(raw);
}

char *
nd_option_label_dup(const nd_node *option)
{
    if (!option) return g_strdup("");
    const char *lbl = nd_element_get_attr(option, "label");
    if (lbl) return g_strdup(lbl);
    return nd_option_text_dup(option);
}

char *
nd_option_value_dup(const nd_node *option)
{
    if (!option) return g_strdup("");
    const char *v = nd_element_get_attr(option, "value");
    if (v) return g_strdup(v);
    return nd_option_text_dup(option);
}

const nd_node *
nd_select_first_selected_option(const nd_node *select)
{
    if (!select) return NULL;
    for (const nd_node *c = select->first_child; c; c = c->next_sibling) {
        if (nd_node_is_element_named(c, "optgroup")) {
            for (const nd_node *cc = c->first_child; cc; cc = cc->next_sibling) {
                if (nd_node_is_element_named(cc, "option"))
                    if (nd_element_get_attr(cc, "selected")) return cc;
            }
        } else if (nd_node_is_element_named(c, "option")) {
            if (nd_element_get_attr(c, "selected")) return c;
        }
    }
    return NULL;
}

const nd_node *
nd_select_chosen_option(const nd_node *select)
{
    if (!select) return NULL;
    const nd_node *selected = nd_select_first_selected_option(select);
    if (selected) return selected;
    const nd_node *first = NULL;
    for (const nd_node *c = select->first_child; c && !first; c = c->next_sibling) {
        if (nd_node_is_element_named(c, "optgroup")) {
            for (const nd_node *cc = c->first_child; cc; cc = cc->next_sibling) {
                if (nd_node_is_element_named(cc, "option")) {
                    first = cc;
                    break;
                }
            }
        } else if (nd_node_is_element_named(c, "option")) {
            first = c;
        }
    }
    return first;
}

const nd_node *
nd_form_owner(const nd_node *control, const nd_node *doc)
{
    if (!control || control->kind != ND_NODE_ELEMENT) return NULL;
    if (!doc) doc = nd_node_root(control);
    const char *form_id = nd_element_get_attr(control, "form");
    if (form_id) {
        if (*form_id && doc) {
            nd_node *owner = nd_node_find_by_id(doc, form_id);
            if (nd_node_is_element_named(owner, "form")) return owner;
        }
        return NULL;
    }
    for (const nd_node *p = control->parent; p; p = p->parent)
        if (nd_node_is_element_named(p, "form")) return p;
    return NULL;
}

static void
nd_form_reset_control(nd_node *n)
{
    if (!n || n->kind != ND_NODE_ELEMENT || !n->name) return;
    if (strcmp(n->name, "input") == 0 ||
        strcmp(n->name, "textarea") == 0) {
        const char *type = nd_element_get_attr(n, "type");
        if (type && (g_ascii_strcasecmp(type, "checkbox") == 0 ||
                     g_ascii_strcasecmp(type, "radio") == 0)) {
            if (nd_element_get_attr(n, "defaultChecked"))
                nd_element_set_attr(n, "checked", "");
            else
                nd_element_remove_attr(n, "checked");
        }
    } else if (strcmp(n->name, "select") == 0) {
        for (nd_node *o = n->first_child; o; o = o->next_sibling) {
            if (nd_node_is_element_named(o, "option"))
                nd_element_remove_attr(o, "selected");
        }
    }
}

static void
nd_form_reset_walk(nd_node *form, nd_node *scan, const nd_node *doc, int depth)
{
    if (!form || !scan || depth >= 512) return;
    if (scan->kind == ND_NODE_ELEMENT && scan->name &&
        nd_form_owner(scan, doc) == form)
        nd_form_reset_control(scan);
    for (nd_node *c = scan->first_child; c; c = c->next_sibling)
        nd_form_reset_walk(form, c, doc, depth + 1);
}

void
nd_form_reset_owned_controls(nd_node *form, nd_node *root, const nd_node *doc)
{
    nd_form_reset_walk(form, root ? root : form, doc ? doc : form, 0);
}

static gboolean
nd_node_contains(const nd_node *ancestor, const nd_node *node)
{
    for (const nd_node *p = node; p; p = p->parent)
        if (p == ancestor) return TRUE;
    return FALSE;
}

static const nd_node *
nd_fieldset_first_legend(const nd_node *fieldset)
{
    if (!fieldset) return NULL;
    for (const nd_node *c = fieldset->first_child; c; c = c->next_sibling)
        if (nd_node_is_element_named(c, "legend")) return c;
    return NULL;
}

gboolean
nd_element_supports_disabled(const nd_node *el)
{
    if (!el || el->kind != ND_NODE_ELEMENT || !el->name) return FALSE;
    return strcmp(el->name, "button") == 0 ||
           strcmp(el->name, "fieldset") == 0 ||
           strcmp(el->name, "input") == 0 ||
           strcmp(el->name, "optgroup") == 0 ||
           strcmp(el->name, "option") == 0 ||
           strcmp(el->name, "select") == 0 ||
           strcmp(el->name, "textarea") == 0;
}

gboolean
nd_element_effectively_disabled(const nd_node *el)
{
    if (!nd_element_supports_disabled(el)) return FALSE;
    if (nd_element_get_attr(el, "disabled")) return TRUE;
    for (const nd_node *p = el->parent; p; p = p->parent) {
        if (nd_node_is_element_named(el, "option") &&
            nd_node_is_element_named(p, "optgroup") &&
            nd_element_get_attr(p, "disabled"))
            return TRUE;
        if (!nd_node_is_element_named(p, "fieldset") ||
            !nd_element_get_attr(p, "disabled"))
            continue;
        const nd_node *legend = nd_fieldset_first_legend(p);
        if (legend && nd_node_contains(legend, el)) continue;
        return TRUE;
    }
    return FALSE;
}

static const nd_node *g_active_modal;

void
nd_dom_set_active_modal(const nd_node *modal)
{
    g_active_modal = modal;
}

const nd_node *
nd_dom_active_modal(void)
{
    return g_active_modal;
}

gboolean
nd_element_effectively_inert(const nd_node *el)
{
    if (!el || el->kind != ND_NODE_ELEMENT) return FALSE;
    for (const nd_node *p = el; p; p = p->parent)
        if (p->kind == ND_NODE_ELEMENT && nd_element_get_attr(p, "inert"))
            return TRUE;
    if (g_active_modal && el != g_active_modal &&
        !nd_node_contains(g_active_modal, el))
        return TRUE;
    return FALSE;
}

static void
collect_text(const nd_node *n, GString *out, int depth)
{
    if (!n || depth >= ND_DOM_MAX_DEPTH) return;
    if (n->kind == ND_NODE_TEXT) {
        if (n->text) g_string_append(out, n->text);
        return;
    }
    if (n->kind == ND_NODE_ELEMENT && n->name &&
        (strcmp(n->name, "style")    == 0 ||
         strcmp(n->name, "script")   == 0 ||
         strcmp(n->name, "noscript") == 0 ||
         strcmp(n->name, "template") == 0))
        return;
    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        collect_text(c, out, depth + 1);
}

char *
nd_node_collect_text(const nd_node *root)
{
    GString *out = g_string_new(NULL);
    collect_text(root, out, 0);
    return g_string_free(out, FALSE);
}

static void
collect_all_text(const nd_node *n, GString *out, int depth)
{
    if (!n || depth >= ND_DOM_MAX_DEPTH) return;
    if (n->kind == ND_NODE_TEXT) {
        if (n->text) g_string_append(out, n->text);
        return;
    }
    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        collect_all_text(c, out, depth + 1);
}

char *
nd_node_collect_all_text(const nd_node *root)
{
    GString *out = g_string_new(NULL);
    collect_all_text(root, out, 0);
    return g_string_free(out, FALSE);
}

#include "html.h"
#define is_void_tag nd_html_is_void

static void
serialize_node(const nd_node *n, GString *out, gboolean include_self, int depth)
{
    if (!n || depth >= ND_DOM_MAX_DEPTH) return;
    if (n->kind == ND_NODE_TEXT) {
        nd_html_escape_append(out, n->text, FALSE);
        return;
    }
    if (n->kind == ND_NODE_COMMENT) {
        g_string_append(out, "<!--");
        g_string_append(out, n->text ? n->text : "");
        g_string_append(out, "-->");
        return;
    }
    if (n->kind == ND_NODE_DOCTYPE) {
        g_string_append_printf(out, "<!DOCTYPE %s>", n->name ? n->name : "");
        return;
    }
    gboolean raw_text = n->kind == ND_NODE_ELEMENT && n->name &&
                        (g_ascii_strcasecmp(n->name, "script") == 0 ||
                         g_ascii_strcasecmp(n->name, "style") == 0);
    if (n->kind == ND_NODE_ELEMENT && include_self) {
        g_string_append_c(out, '<');
        g_string_append(out, n->name ? n->name : "");
        for (const nd_attr *a = n->attrs; a; a = a->next) {
            g_string_append_c(out, ' ');
            g_string_append(out, a->name);
            g_string_append(out, "=\"");
            nd_html_escape_append(out, a->value, TRUE);
            g_string_append_c(out, '"');
        }
        g_string_append_c(out, '>');
        if (is_void_tag(n->name)) return;
    }
    for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
        if (raw_text && c->kind == ND_NODE_TEXT)
            g_string_append(out, c->text ? c->text : "");
        else
            serialize_node(c, out, TRUE, depth + 1);
    }
    if (n->kind == ND_NODE_ELEMENT && include_self) {
        g_string_append(out, "</");
        g_string_append(out, n->name ? n->name : "");
        g_string_append_c(out, '>');
    }
}

char *
nd_node_inner_html(const nd_node *root)
{
    GString *out = g_string_new(NULL);
    gboolean raw_text = root && root->kind == ND_NODE_ELEMENT && root->name &&
                        (g_ascii_strcasecmp(root->name, "script") == 0 ||
                         g_ascii_strcasecmp(root->name, "style") == 0);
    if (root)
        for (const nd_node *c = root->first_child; c; c = c->next_sibling) {
            if (raw_text && c->kind == ND_NODE_TEXT)
                g_string_append(out, c->text ? c->text : "");
            else
                serialize_node(c, out, TRUE, 0);
        }
    return g_string_free(out, FALSE);
}

char *
nd_node_outer_html(const nd_node *node)
{
    GString *out = g_string_new(NULL);
    if (node) serialize_node(node, out, TRUE, 0);
    return g_string_free(out, FALSE);
}

static void
nd_dump_text(GString *out, const char *s, gsize max)
{
    if (!s) return;
    gsize len = strlen(s);
    gboolean truncated = (max > 0 && len > max);
    if (truncated) len = max;
    for (gsize i = 0; i < len; i++) {
        guchar c = (guchar)s[i];
        if (c == '\\')      g_string_append(out, "\\\\");
        else if (c == '\n') g_string_append(out, "\\n");
        else if (c == '\r') g_string_append(out, "\\r");
        else if (c == '\t') g_string_append(out, "\\t");
        else if (c < 0x20)  g_string_append_printf(out, "\\x%02x", c);
        else                g_string_append_c(out, (char)c);
    }
    if (truncated)
        g_string_append(out, "…");
}

static void
nd_dump_node(GString *out, const nd_node *n, int depth)
{
    if (depth >= ND_DOM_MAX_DEPTH) return;
    for (int i = 0; i < depth; i++)
        g_string_append(out, "  ");

    switch (n->kind) {
    case ND_NODE_DOCUMENT:
        g_string_append(out, "#document\n");
        break;
    case ND_NODE_DOCTYPE:
        g_string_append_printf(out, "<!DOCTYPE %s>\n", n->name ? n->name : "");
        break;
    case ND_NODE_ELEMENT:
        g_string_append_printf(out, "<%s", n->name ? n->name : "?");
        for (const nd_attr *a = n->attrs; a; a = a->next) {
            g_string_append_printf(out, " %s=\"", a->name);
            nd_dump_text(out, a->value, 0);
            g_string_append_c(out, '"');
        }
        g_string_append(out, ">\n");
        break;
    case ND_NODE_TEXT:
        g_string_append(out, "\"");
        nd_dump_text(out, n->text, 120);
        g_string_append(out, "\"\n");
        break;
    case ND_NODE_COMMENT:
        g_string_append(out, "<!--");
        nd_dump_text(out, n->text, 120);
        g_string_append(out, "-->\n");
        break;
    }

    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        nd_dump_node(out, c, depth + 1);
}

GString *
nd_node_dump(const nd_node *node)
{
    GString *out = g_string_new(NULL);
    if (node)
        nd_dump_node(out, node, 0);
    return out;
}

static int
nd_map_parse_coords(const char *s, double *out, int max)
{
    int n = 0;
    const char *p = s;
    while (p && *p && n < max) {
        while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (!*p) break;
        char *e = NULL;
        double v = g_ascii_strtod(p, &e);
        if (e == p) break;
        out[n++] = v;
        p = e;
    }
    return n;
}

static gboolean
nd_map_point_in_poly(const double *pts, int npairs, double x, double y)
{
    gboolean in = FALSE;
    for (int i = 0, j = npairs - 1; i < npairs; j = i++) {
        double xi = pts[2 * i], yi = pts[2 * i + 1];
        double xj = pts[2 * j], yj = pts[2 * j + 1];
        if (((yi > y) != (yj > y)) &&
            (x < (xj - xi) * (y - yi) / (yj - yi) + xi))
            in = !in;
    }
    return in;
}

static gboolean
nd_area_hit(const char *shape, const char *coords, double lx, double ly,
            double iw, double ih)
{
    gboolean is_circle = shape && g_ascii_strncasecmp(shape, "circ", 4) == 0;
    gboolean is_poly   = shape && g_ascii_strncasecmp(shape, "poly", 4) == 0;
    gboolean is_default = shape && g_ascii_strcasecmp(shape, "default") == 0;
    if (is_default)
        return lx >= 0 && ly >= 0 && lx <= iw && ly <= ih;
    double c[64];
    int n = coords ? nd_map_parse_coords(coords, c, 64) : 0;
    if (is_circle) {
        if (n < 3) return FALSE;
        double dx = lx - c[0], dy = ly - c[1];
        return dx * dx + dy * dy <= c[2] * c[2];
    }
    if (is_poly) {
        if (n < 6) return FALSE;
        return nd_map_point_in_poly(c, n / 2, lx, ly);
    }
    if (n < 4) return FALSE;
    double x1 = MIN(c[0], c[2]), x2 = MAX(c[0], c[2]);
    double y1 = MIN(c[1], c[3]), y2 = MAX(c[1], c[3]);
    return lx >= x1 && lx <= x2 && ly >= y1 && ly <= y2;
}

static const nd_node *
nd_find_map(const nd_node *n, const char *name)
{
    if (!n) return NULL;
    if (nd_node_is_element_named(n, "map")) {
        const char *mn = nd_element_get_attr(n, "name");
        const char *mid = nd_element_get_attr(n, "id");
        if ((mn && strcmp(mn, name) == 0) || (mid && strcmp(mid, name) == 0))
            return n;
    }
    for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
        const nd_node *r = nd_find_map(c, name);
        if (r) return r;
    }
    return NULL;
}

static const nd_node *
nd_map_first_area(const nd_node *n, double lx, double ly, double iw, double ih)
{
    for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
        if (nd_node_is_element_named(c, "area") &&
            nd_area_hit(nd_element_get_attr(c, "shape"),
                        nd_element_get_attr(c, "coords"), lx, ly, iw, ih))
            return c;
        const nd_node *r = nd_map_first_area(c, lx, ly, iw, ih);
        if (r) return r;
    }
    return NULL;
}

char *
nd_image_map_resolve(const nd_node *doc, const char *usemap,
                     double lx, double ly, double iw, double ih,
                     const char **out_target)
{
    if (out_target) *out_target = NULL;
    if (!doc || !usemap || lx < 0 || ly < 0) return NULL;
    const char *name = usemap[0] == '#' ? usemap + 1 : usemap;
    if (!*name) return NULL;
    const nd_node *map = nd_find_map(doc, name);
    if (!map) return NULL;
    const nd_node *area = nd_map_first_area(map, lx, ly, iw, ih);
    if (!area) return NULL;
    const char *href = nd_element_get_attr(area, "href");
    if (!href || !*href) return NULL;
    if (out_target) *out_target = nd_element_get_attr(area, "target");
    return g_strdup(href);
}
