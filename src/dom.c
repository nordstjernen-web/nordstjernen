/* Nordstjernen — DOM data structure.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "dom.h"

#include "datetime.h"

#include <errno.h>
#include <math.h>
#include <string.h>

static void ns_class_set_clear(ns_node *el);

static gboolean
ns_str_is_ascii_lower(const char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') return FALSE;
    return TRUE;
}

int
ns_parse_int(const char *s, int dflt, int min_v, int max_v)
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
    NS_FORM_INPUT_OTHER,
    NS_FORM_INPUT_NUMBER,
    NS_FORM_INPUT_RANGE,
    NS_FORM_INPUT_DATE,
    NS_FORM_INPUT_MONTH,
    NS_FORM_INPUT_WEEK,
    NS_FORM_INPUT_TIME,
    NS_FORM_INPUT_DATETIME,
} ns_form_input_kind;

static ns_form_input_kind
ns_form_input_kind_of_type(const char *type)
{
    if (!type) return NS_FORM_INPUT_OTHER;
    if (!g_ascii_strcasecmp(type, "number"))         return NS_FORM_INPUT_NUMBER;
    if (!g_ascii_strcasecmp(type, "range"))          return NS_FORM_INPUT_RANGE;
    if (!g_ascii_strcasecmp(type, "date"))           return NS_FORM_INPUT_DATE;
    if (!g_ascii_strcasecmp(type, "month"))          return NS_FORM_INPUT_MONTH;
    if (!g_ascii_strcasecmp(type, "week"))           return NS_FORM_INPUT_WEEK;
    if (!g_ascii_strcasecmp(type, "time"))           return NS_FORM_INPUT_TIME;
    if (!g_ascii_strcasecmp(type, "datetime-local")) return NS_FORM_INPUT_DATETIME;
    return NS_FORM_INPUT_OTHER;
}

gboolean
ns_input_type_has_number_value(const char *type)
{
    return ns_form_input_kind_of_type(type) != NS_FORM_INPUT_OTHER;
}

gboolean
ns_input_type_supports_readonly(const char *type)
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
ns_input_type_supports_text_constraints(const char *type)
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
ns_form_control_readonly_bars_validation(const ns_node *control)
{
    if (!control || control->kind != NS_NODE_ELEMENT || !control->name)
        return FALSE;
    if (!ns_element_get_attr(control, "readonly")) return FALSE;
    if (strcmp(control->name, "textarea") == 0) return TRUE;
    if (strcmp(control->name, "input") != 0) return FALSE;
    return ns_input_type_supports_readonly(ns_element_get_attr(control, "type"));
}

gboolean
ns_form_control_length_limits_apply(const ns_node *control)
{
    if (!control || control->kind != NS_NODE_ELEMENT || !control->name)
        return FALSE;
    if (strcmp(control->name, "textarea") == 0) return TRUE;
    if (strcmp(control->name, "input") != 0) return FALSE;
    return ns_input_type_supports_text_constraints(ns_element_get_attr(control, "type"));
}

gboolean
ns_form_control_supports_required(const ns_node *control)
{
    if (!control || control->kind != NS_NODE_ELEMENT || !control->name)
        return FALSE;
    if (strcmp(control->name, "textarea") == 0 ||
        strcmp(control->name, "select") == 0)
        return TRUE;
    if (strcmp(control->name, "input") != 0) return FALSE;
    const char *type = ns_element_get_attr(control, "type");
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
ns_form_parse_finite_double(const char *v, double *out)
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
ns_input_value_to_number(const char *type, const char *value, double *out)
{
    if (!value || !*value) return FALSE;
    ns_form_input_kind kind = ns_form_input_kind_of_type(type);
    int y, m, d, ms;
    switch (kind) {
    case NS_FORM_INPUT_NUMBER:
    case NS_FORM_INPUT_RANGE:
        return ns_form_parse_finite_double(value, out);
    case NS_FORM_INPUT_DATE: {
        const char *p = ns_dt_rd_date(value, &y, &m, &d);
        if (!p || *p != '\0') return FALSE;
        if (out) *out = (double)ns_dt_days_from_civil(y, m, d) * 86400000.0;
        return TRUE;
    }
    case NS_FORM_INPUT_MONTH: {
        const char *p = ns_dt_rd_digits(value, 4, 9, &y);
        if (!p || *p != '-') return FALSE;
        p++;
        p = ns_dt_rd_digits(p, 2, 2, &m);
        if (!p || *p != '\0' || y < 1 || y > NS_DT_MAX_YEAR ||
            m < 1 || m > 12) return FALSE;
        if (out) *out = (double)((y - 1970) * 12 + (m - 1));
        return TRUE;
    }
    case NS_FORM_INPUT_WEEK: {
        const char *p = ns_dt_rd_digits(value, 4, 9, &y);
        if (!p || *p != '-' || *(p + 1) != 'W') return FALSE;
        p += 2;
        int w;
        p = ns_dt_rd_digits(p, 2, 2, &w);
        if (!p || *p != '\0' || y < 1 || y > NS_DT_MAX_YEAR || w < 1 ||
            w > ns_dt_iso_weeks_in_year(y))
            return FALSE;
        long monday = ns_dt_iso_week1_monday(y) + (long)(w - 1) * 7;
        if (out) *out = (double)monday * 86400000.0;
        return TRUE;
    }
    case NS_FORM_INPUT_TIME: {
        const char *p = ns_dt_rd_time(value, &ms);
        if (!p || *p != '\0') return FALSE;
        if (out) *out = (double)ms;
        return TRUE;
    }
    case NS_FORM_INPUT_DATETIME: {
        const char *p = ns_dt_rd_date(value, &y, &m, &d);
        if (!p || (*p != 'T' && *p != ' ')) return FALSE;
        p++;
        p = ns_dt_rd_time(p, &ms);
        if (!p || *p != '\0') return FALSE;
        if (out) *out = (double)ns_dt_days_from_civil(y, m, d) * 86400000.0 + ms;
        return TRUE;
    }
    default:
        return FALSE;
    }
}

gboolean
ns_input_value_range_state(const ns_node *input, const char *value,
                           gboolean *underflow, gboolean *overflow)
{
    if (underflow) *underflow = FALSE;
    if (overflow) *overflow = FALSE;
    if (!input || !ns_node_is_element_named(input, "input")) return FALSE;
    const char *type = ns_element_get_attr(input, "type");
    double v;
    if (!ns_input_value_to_number(type, value, &v)) return FALSE;
    double bound;
    const char *min = ns_element_get_attr(input, "min");
    const char *max = ns_element_get_attr(input, "max");
    if (ns_input_value_to_number(type, min, &bound) && v < bound) {
        if (underflow) *underflow = TRUE;
    }
    if (ns_input_value_to_number(type, max, &bound) && v > bound) {
        if (overflow) *overflow = TRUE;
    }
    return TRUE;
}

static double
ns_form_step_scale(ns_form_input_kind kind)
{
    switch (kind) {
    case NS_FORM_INPUT_DATE:     return 86400000.0;
    case NS_FORM_INPUT_WEEK:     return 604800000.0;
    case NS_FORM_INPUT_TIME:
    case NS_FORM_INPUT_DATETIME: return 1000.0;
    default:                     return 1.0;
    }
}

static double
ns_form_default_step(ns_form_input_kind kind)
{
    switch (kind) {
    case NS_FORM_INPUT_TIME:
    case NS_FORM_INPUT_DATETIME:
        return 60.0;
    default:
        return 1.0;
    }
}

gboolean
ns_input_value_step_mismatch(const ns_node *input, const char *value)
{
    if (!input || !ns_node_is_element_named(input, "input")) return FALSE;
    const char *type = ns_element_get_attr(input, "type");
    ns_form_input_kind kind = ns_form_input_kind_of_type(type);
    if (kind == NS_FORM_INPUT_OTHER) return FALSE;
    const char *step_attr = ns_element_get_attr(input, "step");
    if (step_attr && g_ascii_strcasecmp(step_attr, "any") == 0)
        return FALSE;
    double v;
    if (!ns_input_value_to_number(type, value, &v)) return FALSE;
    double step_value = ns_form_default_step(kind);
    double parsed;
    if (ns_form_parse_finite_double(step_attr, &parsed) && parsed > 0)
        step_value = parsed;
    double step = step_value * ns_form_step_scale(kind);
    if (step <= 0 || !isfinite(step)) return FALSE;
    double base = kind == NS_FORM_INPUT_WEEK ? -259200000.0 : 0.0;
    if (ns_input_value_to_number(type, ns_element_get_attr(input, "min"), &parsed))
        base = parsed;
    double q = (v - base) / step;
    double nearest = round(q);
    double scale = fabs(q) > 1.0 ? fabs(q) : 1.0;
    return fabs(q - nearest) > 1e-7 * scale;
}

static gboolean
ns_input_type_is(const ns_node *node, const char *want)
{
    if (!ns_node_is_element_named(node, "input")) return FALSE;
    const char *type = ns_element_get_attr(node, "type");
    return type && g_ascii_strcasecmp(type, want) == 0;
}

static gboolean
ns_radio_group_has_checked(const ns_node *scan, const ns_node *doc,
                           const ns_node *owner,
                           const char *name, int depth)
{
    if (!scan || depth >= 512) return FALSE;
    if (ns_input_type_is(scan, "radio")) {
        const char *scan_name = ns_element_get_attr(scan, "name");
        if (!scan_name) scan_name = "";
        if (strcmp(scan_name, name) == 0 &&
            ns_form_owner(scan, doc) == owner &&
            ns_input_is_checked(scan))
            return TRUE;
    }
    for (const ns_node *c = scan->first_child; c; c = c->next_sibling)
        if (ns_radio_group_has_checked(c, doc, owner, name, depth + 1))
            return TRUE;
    return FALSE;
}

gboolean
ns_form_control_value_missing(const ns_node *control, const char *value,
                              const ns_node *doc)
{
    if (!control || control->kind != NS_NODE_ELEMENT || !control->name)
        return FALSE;
    if (ns_input_type_is(control, "checkbox"))
        return !ns_input_is_checked(control);
    if (ns_input_type_is(control, "radio")) {
        const char *name = ns_element_get_attr(control, "name");
        if (!name) name = "";
        const ns_node *root = doc ? doc : ns_node_root(control);
        const ns_node *owner = ns_form_owner(control, root);
        return !ns_radio_group_has_checked(root ? root : control,
                                           root, owner, name, 0);
    }
    return !value || !*value;
}

static gboolean
ns_email_token_valid(const char *start, gsize len)
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
ns_input_email_value_valid(const ns_node *input, const char *value)
{
    if (!value || !*value) return TRUE;
    if (!input || !ns_element_get_attr(input, "multiple"))
        return ns_email_token_valid(value, strlen(value));
    const char *p = value;
    while (TRUE) {
        const char *comma = strchr(p, ',');
        gsize len = comma ? (gsize)(comma - p) : strlen(p);
        if (!ns_email_token_valid(p, len)) return FALSE;
        if (!comma) return TRUE;
        p = comma + 1;
    }
}

gboolean
ns_ce_attr_enables(const char *ce)
{
    return ce && (!*ce ||
                  g_ascii_strcasecmp(ce, "true") == 0 ||
                  g_ascii_strcasecmp(ce, "plaintext-only") == 0);
}

gboolean
ns_node_is_text_input(const ns_node *n)
{
    if (!n || n->kind != NS_NODE_ELEMENT || !n->name) return FALSE;
    if (strcmp(n->name, "textarea") == 0) return TRUE;
    if (strcmp(n->name, "input") != 0) return FALSE;
    const char *type = ns_element_get_attr(n, "type");
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
ns_node_is_contenteditable_host(const ns_node *n)
{
    if (!n || n->kind != NS_NODE_ELEMENT || !n->name) return FALSE;
    if (strcmp(n->name, "input") == 0 || strcmp(n->name, "textarea") == 0)
        return FALSE;
    return ns_ce_attr_enables(ns_element_get_attr(n, "contenteditable"));
}

gboolean
ns_node_is_editable(const ns_node *n)
{
    return ns_node_is_text_input(n) || ns_node_is_contenteditable_host(n);
}

gboolean
ns_node_spellcheck_used(const ns_node *n)
{
    for (const ns_node *cur = n; cur; cur = cur->parent) {
        if (cur->kind != NS_NODE_ELEMENT) continue;
        const char *v = ns_element_get_attr(cur, "spellcheck");
        if (!v) continue;
        if (!g_ascii_strcasecmp(v, "false")) return FALSE;
        if (!*v || !g_ascii_strcasecmp(v, "true")) return TRUE;
    }
    return TRUE;
}

const ns_node *
ns_node_spellcheck_host(const ns_node *n)
{
    for (const ns_node *cur = n; cur; cur = cur->parent)
        if (ns_node_is_editable(cur))
            return ns_node_spellcheck_used(cur) ? cur : NULL;
    return NULL;
}

gboolean
ns_input_value_is_dirty_mode(const ns_node *n)
{
    if (!n || !n->name || strcmp(n->name, "input") != 0) return FALSE;
    const char *type = ns_element_get_attr(n, "type");
    if (!type) return TRUE;
    return !(g_ascii_strcasecmp(type, "checkbox") == 0 ||
             g_ascii_strcasecmp(type, "radio")    == 0 ||
             g_ascii_strcasecmp(type, "submit")   == 0 ||
             g_ascii_strcasecmp(type, "reset")    == 0 ||
             g_ascii_strcasecmp(type, "button")   == 0 ||
             g_ascii_strcasecmp(type, "image")    == 0 ||
             g_ascii_strcasecmp(type, "file")     == 0 ||
             g_ascii_strcasecmp(type, "hidden")   == 0);
}

const char *
ns_input_used_value(const ns_node *n)
{
    if (!n) return NULL;
    if (ns_input_value_is_dirty_mode(n)) {
        const char *dirty = ns_element_get_attr(n, "data-nd-value");
        if (dirty) return dirty;
    }
    return ns_element_get_attr(n, "value");
}

gboolean
ns_input_is_checked(const ns_node *n)
{
    if (!n) return FALSE;
    const char *dirty = ns_element_get_attr(n, "data-nd-checked");
    if (dirty) return strcmp(dirty, "1") == 0;
    return ns_element_get_attr(n, "checked") != NULL;
}

const char *
ns_node_editable_value(const ns_node *n)
{
    if (!n) return "";
    if ((n->name && strcmp(n->name, "textarea") == 0) ||
        ns_node_is_contenteditable_host(n)) {
        for (const ns_node *c = n->first_child; c; c = c->next_sibling)
            if (c->kind == NS_NODE_TEXT && c->text)
                return c->text;
        return "";
    }
    const char *v = ns_input_used_value(n);
    return v ? v : "";
}

void
ns_node_set_editable_value(ns_node *n, const char *value)
{
    if (!n) return;
    if ((n->name && strcmp(n->name, "textarea") == 0) ||
        ns_node_is_contenteditable_host(n)) {
        ns_node *doc = (ns_node *)ns_node_root(n);
        for (ns_node *c = n->first_child; c; ) {
            ns_node *next = c->next_sibling;
            ns_node_remove(c);
            if (doc && doc != c) {
                ns_doc_id_index_subtree_removed(doc, c);
                ns_doc_class_index_subtree_removed(doc, c);
                ns_doc_tag_index_subtree_removed(doc, c);
            }
            ns_node_free(c);
            c = next;
        }
        ns_node_append_child(n, ns_node_new_text(g_strdup(value ? value : "")));
    } else if (ns_input_value_is_dirty_mode(n)) {
        ns_element_set_attr(n, "data-nd-value", value ? value : "");
    } else {
        ns_element_set_attr(n, "value", value ? value : "");
    }
}

void
ns_node_flatten_editable(ns_node *n)
{
    if (!ns_node_is_contenteditable_host(n)) return;
    char *txt = ns_node_collect_text(n);
    ns_node_set_editable_value(n, txt ? txt : "");
    g_free(txt);
}

gboolean
ns_node_is_numeric_input(const ns_node *control)
{
    if (!control || control->kind != NS_NODE_ELEMENT || !control->name)
        return FALSE;
    if (strcmp(control->name, "input") != 0) return FALSE;
    const char *type = ns_element_get_attr(control, "type");
    return type && g_ascii_strcasecmp(type, "number") == 0;
}

char *
ns_numeric_filter_insert(const char *insert, gsize len, gsize *out_len)
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


static ns_node *
ns_node_new(ns_node_kind kind)
{
    ns_node *n = g_new0(ns_node, 1);
    n->kind = kind;
    return n;
}

ns_node *
ns_node_new_document(void)
{
    return ns_node_new(NS_NODE_DOCUMENT);
}

ns_node *
ns_node_new_element(char *name)
{
    ns_node *n = ns_node_new(NS_NODE_ELEMENT);
    n->name = name;
    n->flags |= NS_NODE_OWN_NAME;
    return n;
}

ns_node *
ns_node_new_text(char *text)
{
    ns_node *n = ns_node_new(NS_NODE_TEXT);
    n->text = text;
    n->flags |= NS_NODE_OWN_TEXT;
    return n;
}

ns_node *
ns_node_new_comment(char *text)
{
    ns_node *n = ns_node_new(NS_NODE_COMMENT);
    n->text = text;
    n->flags |= NS_NODE_OWN_TEXT;
    return n;
}

void
ns_node_set_name_borrow(ns_node *n, const char *name)
{
    if (!n) return;
    if (n->flags & NS_NODE_OWN_NAME)
        g_free(n->name);
    n->name = (char *)name;
    n->flags &= ~NS_NODE_OWN_NAME;
}

void
ns_node_set_name_owned(ns_node *n, char *name)
{
    if (!n) {
        g_free(name);
        return;
    }
    if (n->flags & NS_NODE_OWN_NAME)
        g_free(n->name);
    n->name = name;
    n->flags |= NS_NODE_OWN_NAME;
}

void
ns_node_set_text_borrow(ns_node *n, const char *text)
{
    if (!n) return;
    if (n->flags & NS_NODE_OWN_TEXT)
        g_free(n->text);
    n->text = (char *)text;
    n->flags &= ~NS_NODE_OWN_TEXT;
}

void
ns_node_replace_text_owned(ns_node *n, char *text)
{
    if (!n) {
        g_free(text);
        return;
    }
    if (n->flags & NS_NODE_OWN_TEXT)
        g_free(n->text);
    n->text = text;
    n->flags |= NS_NODE_OWN_TEXT;
}

static void
ns_node_own_strings_one(ns_node *n)
{
    ns_class_set_clear(n);
    if (n->name && !(n->flags & NS_NODE_OWN_NAME)) {
        n->name = g_strdup(n->name);
        n->flags |= NS_NODE_OWN_NAME;
    }
    if (n->text && !(n->flags & NS_NODE_OWN_TEXT)) {
        n->text = g_strdup(n->text);
        n->flags |= NS_NODE_OWN_TEXT;
    }
    for (ns_attr *a = n->attrs; a; a = a->next) {
        if (a->name && !(a->flags & NS_ATTR_OWN_NAME)) {
            a->name = g_strdup(a->name);
            a->flags |= NS_ATTR_OWN_NAME;
        }
        if (a->value && !(a->flags & NS_ATTR_OWN_VALUE)) {
            a->value = ns_value_dup_len(a->value, a->value_len);
            a->flags |= NS_ATTR_OWN_VALUE;
        }
    }
}

void
ns_node_own_strings_deep(ns_node *n)
{
    if (!n) return;
    GPtrArray *stack = g_ptr_array_new();
    g_ptr_array_add(stack, n);
    while (stack->len > 0) {
        ns_node *cur = g_ptr_array_index(stack, stack->len - 1);
        g_ptr_array_set_size(stack, stack->len - 1);
        ns_node_own_strings_one(cur);
        if (cur->tpl_content)
            g_ptr_array_add(stack, cur->tpl_content);
        for (ns_node *c = cur->first_child; c; c = c->next_sibling)
            g_ptr_array_add(stack, c);
    }
    g_ptr_array_free(stack, TRUE);
}

void
ns_element_append_attr_borrow(ns_node *el, const char *name, const char *value)
{
    if (!el || el->kind != NS_NODE_ELEMENT || !name) return;
    if (el->class_set) ns_class_set_clear(el);
    el->attr_bloom = 0;
    el->attr_gen++;
    ns_attr *a = g_new0(ns_attr, 1);
    a->name  = (char *)name;
    a->value = (char *)(value ? value : "");
    a->value_len = value ? (guint)strlen(value) : 0;
    a->flags = ns_str_is_ascii_lower(name) ? NS_ATTR_NAME_LOWER : 0;
    ns_attr *tail = NULL;
    for (ns_attr *cur = el->attrs; cur; cur = cur->next) tail = cur;
    if (tail) tail->next = a;
    else      el->attrs = a;
}

void
ns_node_attach_backing(ns_node *root, void *backing, void (*destroy)(void *))
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
ns_attr_free_one(ns_attr *a)
{
    if (!a) return;
    if (a->flags & NS_ATTR_OWN_NAME)  g_free(a->name);
    if (a->flags & NS_ATTR_OWN_VALUE) g_free(a->value);
    g_free(a->namespace_uri);
    g_free(a->prefix);
    g_free(a->local_name);
    g_free(a);
}

static void
ns_attr_free(ns_attr *a)
{
    while (a) {
        ns_attr *next = a->next;
        ns_attr_free_one(a);
        a = next;
    }
}

typedef struct ns_class_set {
    guint n;
    struct { const char *p; guint len; } tok[];
} ns_class_set;

static ns_class_set g_nd_empty_class_set;

static inline gboolean
ns_clsset_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static ns_class_set *
ns_class_set_build(const char *cls)
{
    guint n = 0;
    for (const char *s = cls; *s; ) {
        while (*s && ns_clsset_ws(*s)) s++;
        if (!*s) break;
        while (*s && !ns_clsset_ws(*s)) s++;
        n++;
    }
    if (n == 0) return &g_nd_empty_class_set;
    ns_class_set *cs = g_malloc(sizeof *cs + (gsize)n * sizeof cs->tok[0]);
    cs->n = 0;
    for (const char *s = cls; *s; ) {
        while (*s && ns_clsset_ws(*s)) s++;
        if (!*s) break;
        const char *t = s;
        while (*s && !ns_clsset_ws(*s)) s++;
        cs->tok[cs->n].p = t;
        cs->tok[cs->n].len = (guint)(s - t);
        cs->n++;
    }
    return cs;
}

static void
ns_class_set_clear(ns_node *el)
{
    if (el->class_set && el->class_set != &g_nd_empty_class_set)
        g_free(el->class_set);
    el->class_set = NULL;
}

gboolean
ns_node_has_class(const ns_node *el, const char *name, gsize len)
{
    if (!el || el->kind != NS_NODE_ELEMENT) return FALSE;
    ns_class_set *cs = el->class_set;
    if (!cs) {
        const char *cls = ns_element_get_attr(el, "class");
        cs = (cls && *cls) ? ns_class_set_build(cls) : &g_nd_empty_class_set;
        ((ns_node *)el)->class_set = cs;
    }
    for (guint i = 0; i < cs->n; i++)
        if (cs->tok[i].len == len && memcmp(cs->tok[i].p, name, len) == 0)
            return TRUE;
    return FALSE;
}

void
ns_node_free(ns_node *node)
{
    if (!node)
        return;

    GPtrArray *stack = g_ptr_array_new();
    g_ptr_array_add(stack, node);

    while (stack->len > 0) {
        ns_node *cur = g_ptr_array_index(stack, stack->len - 1);
        if (cur->tpl_content) {
            ns_node *tc = cur->tpl_content;
            cur->tpl_content = NULL;
            g_ptr_array_add(stack, tc);
            continue;
        }
        if (cur->first_child) {
            ns_node *c = cur->first_child;
            cur->first_child = NULL;
            while (c) {
                ns_node *next = c->next_sibling;
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
        if (cur->flags & NS_NODE_OWN_NAME) g_free(cur->name);
        if (cur->flags & NS_NODE_OWN_TEXT) g_free(cur->text);
        ns_class_set_clear(cur);
        ns_attr_free(cur->attrs);
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
ns_node_detach(ns_node *child)
{
    ns_node *p = child->parent;
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
ns_node_append_child(ns_node *parent, ns_node *child)
{
    g_return_if_fail(parent != NULL);
    g_return_if_fail(child != NULL);

    ns_node_detach(child);
    child->parent = parent;
    child->prev_sibling = parent->last_child;
    if (parent->last_child)
        parent->last_child->next_sibling = child;
    else
        parent->first_child = child;
    parent->last_child = child;
}

void
ns_node_remove(ns_node *child)
{
    ns_node_detach(child);
}

gboolean
ns_attr_name_is_internal(const char *name)
{
    return name && g_ascii_strncasecmp(name, "data-nd-", 8) == 0;
}

const char *
ns_attr_local_name(const ns_attr *attr)
{
    if (!attr) return "";
    return attr->local_name ? attr->local_name : (attr->name ? attr->name : "");
}

static const char *
ns_attr_normalize_namespace(const char *namespace_uri)
{
    return (namespace_uri && *namespace_uri) ? namespace_uri : NULL;
}

static gboolean
ns_attr_namespace_equal(const char *a, const char *b)
{
    a = ns_attr_normalize_namespace(a);
    b = ns_attr_normalize_namespace(b);
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

static gboolean
ns_attr_matches_ns(const ns_attr *attr, const char *namespace_uri,
                   const char *local_name)
{
    if (!attr || !local_name) return FALSE;
    return ns_attr_namespace_equal(attr->namespace_uri, namespace_uri) &&
           strcmp(ns_attr_local_name(attr), local_name) == 0;
}

char *
ns_value_dup_len(const char *value, gsize len)
{
    char *v = g_malloc(len + 1);
    if (len && value) memcpy(v, value, len);
    v[len] = '\0';
    return v;
}

void
ns_element_set_attr_len(ns_node *el, const char *name,
                        const char *value, gssize len)
{
    g_return_if_fail(el != NULL);
    g_return_if_fail(el->kind == NS_NODE_ELEMENT);
    g_return_if_fail(name != NULL);

    gsize vlen = len < 0 ? (value ? strlen(value) : 0) : (gsize)len;
    if (el->class_set && g_ascii_strcasecmp(name, "class") == 0)
        ns_class_set_clear(el);
    el->attr_bloom = 0;
    el->attr_gen++;

    ns_attr *tail = NULL;
    for (ns_attr *a = el->attrs; a; a = a->next) {
        if (g_ascii_strcasecmp(a->name, name) == 0) {
            if (a->flags & NS_ATTR_OWN_VALUE) g_free(a->value);
            a->value = ns_value_dup_len(value, vlen);
            a->value_len = (guint)vlen;
            a->flags |= NS_ATTR_OWN_VALUE;
            return;
        }
        tail = a;
    }
    ns_attr *a = g_new0(ns_attr, 1);
    a->name = g_strdup(name);
    a->value = ns_value_dup_len(value, vlen);
    a->value_len = (guint)vlen;
    a->flags = NS_ATTR_OWN_NAME | NS_ATTR_OWN_VALUE |
               (ns_str_is_ascii_lower(name) ? NS_ATTR_NAME_LOWER : 0);
    a->next = NULL;
    if (tail) tail->next = a;
    else      el->attrs = a;
}

void
ns_element_set_attr(ns_node *el, const char *name, const char *value)
{
    ns_element_set_attr_len(el, name, value, -1);
}

void
ns_element_set_attr_ns(ns_node *el, const char *namespace_uri,
                       const char *prefix, const char *local_name,
                       const char *name, const char *value)
{
    g_return_if_fail(el != NULL);
    g_return_if_fail(el->kind == NS_NODE_ELEMENT);
    g_return_if_fail(local_name != NULL);

    const char *ns = ns_attr_normalize_namespace(namespace_uri);
    const char *pfx = prefix && *prefix ? prefix : NULL;
    const char *qualified = name && *name ? name : local_name;

    if (el->class_set && !ns && g_ascii_strcasecmp(local_name, "class") == 0)
        ns_class_set_clear(el);
    el->attr_bloom = 0;
    el->attr_gen++;

    gsize vlen = value ? strlen(value) : 0;
    ns_attr *tail = NULL;
    for (ns_attr *a = el->attrs; a; a = a->next) {
        if (ns_attr_matches_ns(a, ns, local_name)) {
            if (a->flags & NS_ATTR_OWN_VALUE) g_free(a->value);
            a->value = ns_value_dup_len(value, vlen);
            a->value_len = (guint)vlen;
            a->flags |= NS_ATTR_OWN_VALUE;
            return;
        }
        tail = a;
    }

    ns_attr *a = g_new0(ns_attr, 1);
    a->name = g_strdup(qualified);
    a->value = ns_value_dup_len(value, vlen);
    a->value_len = (guint)vlen;
    a->namespace_uri = ns ? g_strdup(ns) : NULL;
    a->prefix = pfx ? g_strdup(pfx) : NULL;
    a->local_name = g_strdup(local_name);
    a->flags = NS_ATTR_OWN_NAME | NS_ATTR_OWN_VALUE |
               (ns_str_is_ascii_lower(qualified) ? NS_ATTR_NAME_LOWER : 0);
    if (tail) tail->next = a;
    else      el->attrs = a;
}

void
ns_element_remove_attr(ns_node *el, const char *name)
{
    if (!el || el->kind != NS_NODE_ELEMENT || !name) return;
    if (el->class_set && g_ascii_strcasecmp(name, "class") == 0)
        ns_class_set_clear(el);
    el->attr_bloom = 0;
    el->attr_gen++;
    ns_attr **link = &el->attrs;
    while (*link) {
        if (g_ascii_strcasecmp((*link)->name, name) == 0) {
            ns_attr *dead = *link;
            *link = dead->next;
            ns_attr_free_one(dead);
            return;
        }
        link = &(*link)->next;
    }
}

void
ns_element_remove_attr_ns(ns_node *el, const char *namespace_uri,
                          const char *local_name)
{
    if (!el || el->kind != NS_NODE_ELEMENT || !local_name) return;
    const char *ns = ns_attr_normalize_namespace(namespace_uri);
    if (el->class_set && !ns && g_ascii_strcasecmp(local_name, "class") == 0)
        ns_class_set_clear(el);
    el->attr_bloom = 0;
    el->attr_gen++;
    ns_attr **link = &el->attrs;
    while (*link) {
        if (ns_attr_matches_ns(*link, ns, local_name)) {
            ns_attr *dead = *link;
            *link = dead->next;
            ns_attr_free_one(dead);
            return;
        }
        link = &(*link)->next;
    }
}

#define NS_DOM_MAX_DEPTH 512

static ns_node *
ns_node_clone_depth(const ns_node *src, gboolean deep, int depth)
{
    if (!src || depth >= NS_DOM_MAX_DEPTH) return NULL;
    ns_node *out = NULL;
    switch (src->kind) {
    case NS_NODE_ELEMENT:
        out = ns_node_new_element(src->name ? g_strdup(src->name) : g_strdup(""));
        out->flags |= src->flags & (NS_NODE_SVG_NS | NS_NODE_FOREIGN_NS |
                                    NS_NODE_KEEP_CASE);
        for (const ns_attr *a = src->attrs; a; a = a->next)
            ns_element_set_attr_ns(out, a->namespace_uri, a->prefix,
                                   ns_attr_local_name(a), a->name,
                                   a->value ? a->value : "");
        break;
    case NS_NODE_TEXT:
        out = ns_node_new_text(g_strdup(src->text ? src->text : ""));
        break;
    case NS_NODE_DOCTYPE:
        out = ns_node_new_element(src->name ? g_strdup(src->name) : g_strdup(""));
        for (const ns_attr *a = src->attrs; a; a = a->next)
            ns_element_set_attr_ns(out, a->namespace_uri, a->prefix,
                                   ns_attr_local_name(a), a->name,
                                   a->value ? a->value : "");
        out->kind = NS_NODE_DOCTYPE;
        break;
    case NS_NODE_DOCUMENT:
    case NS_NODE_COMMENT:
        out = ns_node_new(src->kind);
        if (src->text) {
            out->text = g_strdup(src->text);
            out->flags |= NS_NODE_OWN_TEXT;
        }
        if (src->name) {
            out->name = g_strdup(src->name);
            out->flags |= NS_NODE_OWN_NAME;
        }
        for (const ns_attr *a = src->attrs; a; a = a->next)
            ns_element_set_attr_ns(out, a->namespace_uri, a->prefix,
                                   ns_attr_local_name(a), a->name,
                                   a->value ? a->value : "");
        break;
    }
    if (out) out->flags |= src->flags & (NS_NODE_FRAGMENT | NS_NODE_CDATA |
                                         NS_NODE_PI);
    if (deep && out) {
        for (const ns_node *c = src->first_child; c; c = c->next_sibling) {
            ns_node *cc = ns_node_clone_depth(c, TRUE, depth + 1);
            if (cc) ns_node_append_child(out, cc);
        }
        if (src->tpl_content)
            out->tpl_content = ns_node_clone_depth(src->tpl_content, TRUE,
                                                   depth + 1);
    }
    return out;
}

ns_node *
ns_node_clone(const ns_node *src, gboolean deep)
{
    return ns_node_clone_depth(src, deep, 0);
}

ns_node *
ns_template_content_get(ns_node *tpl)
{
    if (!tpl) return NULL;
    if (!tpl->tpl_content) {
        ns_node *frag = ns_node_new_document();
        if (!frag) return NULL;
        frag->flags |= NS_NODE_FRAGMENT | NS_NODE_TEMPLATE_CONTENT;
        tpl->tpl_content = frag;
    }
    return tpl->tpl_content;
}

guint64
ns_attr_name_bloom_bit(const char *name)
{
    guint32 h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return (guint64)1 << (h & 63);
}

guint64
ns_node_attr_bloom(const ns_node *el)
{
    if (el->attr_bloom) return el->attr_bloom;
    guint64 b = 0;
    for (const ns_attr *a = el->attrs; a; a = a->next)
        if (a->name) b |= ns_attr_name_bloom_bit(a->name);
    ((ns_node *)el)->attr_bloom = b;
    return b;
}

static const ns_attr *
ns_element_find_attr(const ns_node *el, const char *name)
{
    if (!el || el->kind != NS_NODE_ELEMENT || !name)
        return NULL;
    if (el->flags & NS_NODE_SVG_NS) {
        for (const ns_attr *a = el->attrs; a; a = a->next)
            if (a->name && strcmp(a->name, name) == 0)
                return a;
        return NULL;
    }
    if (ns_str_is_ascii_lower(name)) {
        char c0 = name[0];
        for (const ns_attr *a = el->attrs; a; a = a->next) {
            const char *an = a->name;
            if (!an) continue;
            if (a->flags & NS_ATTR_NAME_LOWER) {
                if (an[0] == c0 && strcmp(an, name) == 0) return a;
            } else if (g_ascii_strcasecmp(an, name) == 0) {
                return a;
            }
        }
        return NULL;
    }
    for (const ns_attr *a = el->attrs; a; a = a->next) {
        if (g_ascii_strcasecmp(a->name, name) == 0)
            return a;
    }
    return NULL;
}

const char *
ns_element_get_attr(const ns_node *el, const char *name)
{
    const ns_attr *a = ns_element_find_attr(el, name);
    return a ? a->value : NULL;
}

const char *
ns_element_get_attr_len(const ns_node *el, const char *name, gsize *out_len)
{
    const ns_attr *a = ns_element_find_attr(el, name);
    if (!a) { if (out_len) *out_len = 0; return NULL; }
    if (out_len) *out_len = a->value_len;
    return a->value;
}

const ns_attr *
ns_element_find_attr_ns(const ns_node *el, const char *namespace_uri,
                        const char *local_name)
{
    if (!el || el->kind != NS_NODE_ELEMENT || !local_name)
        return NULL;
    const char *ns = ns_attr_normalize_namespace(namespace_uri);
    for (const ns_attr *a = el->attrs; a; a = a->next)
        if (ns_attr_matches_ns(a, ns, local_name))
            return a;
    return NULL;
}

gboolean
ns_node_is_element_named(const ns_node *n, const char *tag)
{
    return n && n->kind == NS_NODE_ELEMENT && n->name && tag &&
           strcmp(n->name, tag) == 0;
}

const ns_node *
ns_node_root(const ns_node *n)
{
    if (!n) return NULL;
    while (n->parent) n = n->parent;
    return n;
}

static ns_node *
ns_node_find_first_element_depth(const ns_node *root, const char *tag, int depth)
{
    if (!root || !tag || depth >= NS_DOM_MAX_DEPTH) return NULL;
    if (ns_node_is_element_named(root, tag))
        return (ns_node *)root;
    for (const ns_node *c = root->first_child; c; c = c->next_sibling) {
        ns_node *m = ns_node_find_first_element_depth(c, tag, depth + 1);
        if (m) return m;
    }
    return NULL;
}

ns_node *
ns_node_find_first_element(const ns_node *root, const char *tag)
{
    if (root && tag && *tag && root->tag_index) {
        GPtrArray *list = ns_doc_tag_index_lookup(root, tag);
        if (list && list->len > 0) return g_ptr_array_index(list, 0);
        return NULL;
    }
    return ns_node_find_first_element_depth(root, tag, 0);
}

static gboolean ns_node_contains(const ns_node *ancestor, const ns_node *node);

static ns_node *
ns_node_find_by_id_depth(const ns_node *root, const char *id, int depth)
{
    if (!root || !id || depth >= NS_DOM_MAX_DEPTH) return NULL;
    if (root->kind == NS_NODE_ELEMENT) {
        const char *eid = ns_element_get_attr(root, "id");
        if (eid && strcmp(eid, id) == 0) return (ns_node *)root;
    }
    if (ns_node_is_element_named(root, "template")) return NULL;
    for (const ns_node *c = root->first_child; c; c = c->next_sibling) {
        ns_node *m = ns_node_find_by_id_depth(c, id, depth + 1);
        if (m) return m;
    }
    return NULL;
}

static void
ns_doc_id_index_register_subtree(GHashTable *map, ns_node *n, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n->kind == NS_NODE_ELEMENT) {
        const char *eid = ns_element_get_attr(n, "id");
        if (eid && *eid && !g_hash_table_contains(map, eid))
            g_hash_table_insert(map, g_strdup(eid), n);
    }
    if (ns_node_is_element_named(n, "template")) return;
    for (ns_node *c = n->first_child; c; c = c->next_sibling)
        ns_doc_id_index_register_subtree(map, c, depth + 1);
}

void
ns_doc_id_index_build(ns_node *doc)
{
    if (!doc) return;
    if (doc->id_index) {
        g_hash_table_remove_all(doc->id_index);
    } else {
        doc->id_index = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
    }
    ns_doc_id_index_register_subtree(doc->id_index, doc, 0);
}

void
ns_doc_id_index_register(ns_node *doc, const char *id, ns_node *node)
{
    if (!doc || !doc->id_index || !id || !*id || !node) return;
    if (g_hash_table_contains(doc->id_index, id)) return;
    g_hash_table_insert(doc->id_index, g_strdup(id), node);
}

void
ns_doc_id_index_unregister(ns_node *doc, const char *id, const ns_node *node)
{
    if (!doc || !doc->id_index || !id || !*id) return;
    gpointer cur = g_hash_table_lookup(doc->id_index, id);
    if (cur == node) g_hash_table_remove(doc->id_index, id);
}

static void
ns_doc_id_index_add_subtree(ns_node *doc, ns_node *n, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n->kind == NS_NODE_ELEMENT) {
        const char *eid = ns_element_get_attr(n, "id");
        if (eid && *eid && !g_hash_table_contains(doc->id_index, eid))
            g_hash_table_insert(doc->id_index, g_strdup(eid), n);
    }
    if (ns_node_is_element_named(n, "template")) return;
    for (ns_node *c = n->first_child; c; c = c->next_sibling)
        ns_doc_id_index_add_subtree(doc, c, depth + 1);
}

static void
ns_doc_id_index_remove_subtree(ns_node *doc, ns_node *n, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n->kind == NS_NODE_ELEMENT) {
        const char *eid = ns_element_get_attr(n, "id");
        if (eid && *eid) {
            gpointer cur = g_hash_table_lookup(doc->id_index, eid);
            if (cur == n) g_hash_table_remove(doc->id_index, eid);
        }
    }
    if (ns_node_is_element_named(n, "template")) return;
    for (ns_node *c = n->first_child; c; c = c->next_sibling)
        ns_doc_id_index_remove_subtree(doc, c, depth + 1);
}

void
ns_doc_id_index_subtree_added(ns_node *doc, ns_node *root)
{
    if (!doc || !doc->id_index || !root) return;
    ns_doc_id_index_add_subtree(doc, root, 0);
}

void
ns_doc_id_index_subtree_removed(ns_node *doc, ns_node *root)
{
    if (!doc || !doc->id_index || !root) return;
    ns_doc_id_index_remove_subtree(doc, root, 0);
}

static void
ns_class_array_destroy(gpointer p)
{
    g_ptr_array_free((GPtrArray *)p, TRUE);
}

static int
ns_node_document_order_cmp(const ns_node *a, const ns_node *b)
{
    if (a == b) return 0;
    int da = 0, db = 0;
    for (const ns_node *p = a; p; p = p->parent) da++;
    for (const ns_node *p = b; p; p = p->parent) db++;
    const ns_node *ca = a, *cb = b;
    int x = da, y = db;
    while (x > y) { ca = ca->parent; x--; }
    while (y > x) { cb = cb->parent; y--; }
    if (ca == cb) return da < db ? -1 : 1;
    while (ca->parent != cb->parent) {
        ca = ca->parent;
        cb = cb->parent;
    }
    const ns_node *par = ca->parent;
    if (par) {
        if (ca == par->first_child || cb == par->last_child) return -1;
        if (cb == par->first_child || ca == par->last_child) return 1;
    }
    const ns_node *fwd = ca->next_sibling;
    const ns_node *back = ca->prev_sibling;
    while (fwd || back) {
        if (fwd == cb) return -1;
        if (back == cb) return 1;
        if (fwd) fwd = fwd->next_sibling;
        if (back) back = back->prev_sibling;
    }
    return 0;
}

static gboolean g_doc_index_building;

static void
ns_doc_index_ordered_insert(GPtrArray *arr, ns_node *node)
{
    if (g_doc_index_building) {
        if (arr->len == 0 ||
            g_ptr_array_index(arr, arr->len - 1) != node)
            g_ptr_array_add(arr, node);
        return;
    }
    if (arr->len == 0 ||
        ns_node_document_order_cmp(node, g_ptr_array_index(arr, arr->len - 1)) > 0) {
        g_ptr_array_add(arr, node);
        return;
    }
    guint lo = 0, hi = arr->len;
    while (lo < hi) {
        guint mid = (lo + hi) / 2;
        int c = ns_node_document_order_cmp(node,
                                           g_ptr_array_index(arr, mid));
        if (c == 0)
            return;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    g_ptr_array_insert(arr, (gint)lo, node);
}

static void
ns_doc_class_index_add_token(GHashTable *map, const char *tok, gsize tok_len,
                             ns_node *node)
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
        ns_doc_index_ordered_insert(arr, node);
    } else {
        arr = g_ptr_array_new();
        g_ptr_array_add(arr, node);
        gchar *owned = (key == stack) ? g_strndup(tok, tok_len) : key;
        g_hash_table_insert(map, owned, arr);
    }
}

static void
ns_doc_class_index_remove_token(GHashTable *map, const char *tok, gsize tok_len,
                                ns_node *node)
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
ns_class_is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

void
ns_doc_class_index_register(ns_node *doc, const char *class_attr, ns_node *node)
{
    if (!doc || !doc->class_index || !class_attr || !node) return;
    const char *p = class_attr;
    while (*p) {
        while (*p && ns_class_is_ws(*p)) p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && !ns_class_is_ws(*p)) p++;
        ns_doc_class_index_add_token(doc->class_index, tok, (gsize)(p - tok), node);
    }
}

void
ns_doc_class_index_unregister(ns_node *doc, const char *class_attr, ns_node *node)
{
    if (!doc || !doc->class_index || !class_attr || !node) return;
    const char *p = class_attr;
    while (*p) {
        while (*p && ns_class_is_ws(*p)) p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && !ns_class_is_ws(*p)) p++;
        ns_doc_class_index_remove_token(doc->class_index, tok, (gsize)(p - tok), node);
    }
}

static void
ns_doc_class_index_add_subtree(ns_node *doc, ns_node *n, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n->kind == NS_NODE_ELEMENT) {
        const char *cls = ns_element_get_attr(n, "class");
        if (cls && *cls) ns_doc_class_index_register(doc, cls, n);
    }
    if (ns_node_is_element_named(n, "template")) return;
    for (ns_node *c = n->first_child; c; c = c->next_sibling)
        ns_doc_class_index_add_subtree(doc, c, depth + 1);
}

static void
ns_doc_class_index_remove_subtree(ns_node *doc, ns_node *n, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n->kind == NS_NODE_ELEMENT) {
        const char *cls = ns_element_get_attr(n, "class");
        if (cls && *cls) ns_doc_class_index_unregister(doc, cls, n);
    }
    if (ns_node_is_element_named(n, "template")) return;
    for (ns_node *c = n->first_child; c; c = c->next_sibling)
        ns_doc_class_index_remove_subtree(doc, c, depth + 1);
}

void
ns_doc_class_index_build(ns_node *doc)
{
    if (!doc) return;
    if (doc->class_index) {
        g_hash_table_remove_all(doc->class_index);
    } else {
        doc->class_index = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, ns_class_array_destroy);
    }
    g_doc_index_building = TRUE;
    ns_doc_class_index_add_subtree(doc, doc, 0);
    g_doc_index_building = FALSE;
}

void
ns_doc_class_index_subtree_added(ns_node *doc, ns_node *root)
{
    if (!doc || !doc->class_index || !root) return;
    ns_doc_class_index_add_subtree(doc, root, 0);
}

void
ns_doc_class_index_subtree_removed(ns_node *doc, ns_node *root)
{
    if (!doc || !doc->class_index || !root) return;
    ns_doc_class_index_remove_subtree(doc, root, 0);
}

static void
ns_doc_tag_index_add_single(GHashTable *map, const char *tag, ns_node *node)
{
    if (!tag || !*tag) return;
    gboolean is_lower = ns_str_is_ascii_lower(tag);
    GPtrArray *arr = is_lower
        ? g_hash_table_lookup(map, tag)
        : NULL;
    if (!arr && !is_lower) {
        gchar *probe = g_ascii_strdown(tag, -1);
        arr = g_hash_table_lookup(map, probe);
        g_free(probe);
    }
    if (arr) {
        ns_doc_index_ordered_insert(arr, node);
        return;
    }
    arr = g_ptr_array_new();
    g_ptr_array_add(arr, node);
    g_hash_table_insert(map,
        is_lower ? g_strdup(tag) : g_ascii_strdown(tag, -1), arr);
}

static void
ns_doc_tag_index_remove_single(GHashTable *map, const char *tag, ns_node *node)
{
    if (!tag || !*tag) return;
    GPtrArray *arr;
    if (ns_str_is_ascii_lower(tag)) {
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
ns_doc_tag_index_add_subtree(ns_node *doc, ns_node *n, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n->kind == NS_NODE_ELEMENT && n->name)
        ns_doc_tag_index_add_single(doc->tag_index, n->name, n);
    if (ns_node_is_element_named(n, "template")) return;
    for (ns_node *c = n->first_child; c; c = c->next_sibling)
        ns_doc_tag_index_add_subtree(doc, c, depth + 1);
}

static void
ns_doc_tag_index_remove_subtree(ns_node *doc, ns_node *n, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n->kind == NS_NODE_ELEMENT && n->name)
        ns_doc_tag_index_remove_single(doc->tag_index, n->name, n);
    if (ns_node_is_element_named(n, "template")) return;
    for (ns_node *c = n->first_child; c; c = c->next_sibling)
        ns_doc_tag_index_remove_subtree(doc, c, depth + 1);
}

void
ns_doc_tag_index_build(ns_node *doc)
{
    if (!doc) return;
    if (doc->tag_index) {
        g_hash_table_remove_all(doc->tag_index);
    } else {
        doc->tag_index = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, ns_class_array_destroy);
    }
    g_doc_index_building = TRUE;
    ns_doc_tag_index_add_subtree(doc, doc, 0);
    g_doc_index_building = FALSE;
}

void
ns_doc_tag_index_subtree_added(ns_node *doc, ns_node *root)
{
    if (!doc || !doc->tag_index || !root) return;
    ns_doc_tag_index_add_subtree(doc, root, 0);
}

void
ns_doc_tag_index_subtree_removed(ns_node *doc, ns_node *root)
{
    if (!doc || !doc->tag_index || !root) return;
    ns_doc_tag_index_remove_subtree(doc, root, 0);
}

GPtrArray *
ns_doc_tag_index_lookup(const ns_node *doc, const char *tag)
{
    if (!doc || !doc->tag_index || !tag || !*tag) return NULL;
    if (ns_str_is_ascii_lower(tag))
        return g_hash_table_lookup(doc->tag_index, tag);
    gchar *key = g_ascii_strdown(tag, -1);
    GPtrArray *arr = g_hash_table_lookup(doc->tag_index, key);
    g_free(key);
    return arr;
}

ns_node *
ns_node_find_by_id(const ns_node *root, const char *id)
{
    if (!root || !id || !*id) return NULL;
    if (root->id_index) {
        ns_node *hit = g_hash_table_lookup(root->id_index, id);
        if (hit) {
            const char *hid = ns_element_get_attr(hit, "id");
            if (hid && strcmp(hid, id) == 0 && ns_node_contains(root, hit))
                return hit;
        }
        ns_node *found = ns_node_find_by_id_depth(root, id, 0);
        if (found)
            g_hash_table_replace(root->id_index, g_strdup(id), found);
        else
            g_hash_table_remove(root->id_index, id);
        return found;
    }
    return ns_node_find_by_id_depth(root, id, 0);
}

static ns_node *
ns_node_find_anchor_name_depth(const ns_node *root, const char *name, int depth)
{
    if (!root || !name || depth >= NS_DOM_MAX_DEPTH) return NULL;
    if (ns_node_is_element_named(root, "a")) {
        const char *n = ns_element_get_attr(root, "name");
        if (n && strcmp(n, name) == 0) return (ns_node *)root;
    }
    if (ns_node_is_element_named(root, "template")) return NULL;
    for (const ns_node *c = root->first_child; c; c = c->next_sibling) {
        ns_node *m = ns_node_find_anchor_name_depth(c, name, depth + 1);
        if (m) return m;
    }
    return NULL;
}

ns_node *
ns_node_find_fragment_target(const ns_node *root, const char *frag)
{
    if (!root || !frag || !*frag) return NULL;
    ns_node *by_id = ns_node_find_by_id(root, frag);
    if (by_id) return by_id;
    return ns_node_find_anchor_name_depth(root, frag, 0);
}

gboolean
ns_element_hidden_until_found(const ns_node *el)
{
    if (!el || el->kind != NS_NODE_ELEMENT) return FALSE;
    const char *hidden = ns_element_get_attr(el, "hidden");
    return hidden && g_ascii_strcasecmp(hidden, "until-found") == 0;
}

gboolean
ns_details_fragment_needs_open(const ns_node *details, const ns_node *target)
{
    if (!ns_node_is_element_named(details, "details") ||
        !target || target == details ||
        ns_element_get_attr(details, "open"))
        return FALSE;
    const ns_node *child = target;
    while (child && child->parent != details) child = child->parent;
    if (!child) return FALSE;
    const ns_node *summary = NULL;
    for (const ns_node *c = details->first_child; c; c = c->next_sibling) {
        if (ns_node_is_element_named(c, "summary")) {
            summary = c;
            break;
        }
    }
    return !summary || child != summary;
}

static void
collect_descendant_text_skip_script(const ns_node *n, GString *out, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_TEXT) {
            if (c->text) g_string_append(out, c->text);
        } else if (c->kind == NS_NODE_ELEMENT && c->name &&
                   g_ascii_strcasecmp(c->name, "script") == 0) {
            continue;
        } else {
            collect_descendant_text_skip_script(c, out, depth + 1);
        }
    }
}

static char *
ns_node_collect_descendant_text_skip_script(const ns_node *root)
{
    GString *out = g_string_new(NULL);
    collect_descendant_text_skip_script(root, out, 0);
    return g_string_free(out, FALSE);
}

static char *
ns_strip_and_collapse_ascii_ws(const char *s)
{
    if (!s) return g_strdup("");
    GString *out = g_string_new(NULL);
    gboolean in_ws = TRUE;
    const char *end = s + strlen(s);
    for (const char *p = s; *p; ) {
        gunichar ch = g_utf8_get_char(p);
        const char *next = g_utf8_next_char(p);
        if (next > end) next = end;
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
ns_option_text_dup(const ns_node *option)
{
    if (!option) return g_strdup("");
    g_autofree char *raw = ns_node_collect_descendant_text_skip_script(option);
    return ns_strip_and_collapse_ascii_ws(raw);
}

char *
ns_option_label_dup(const ns_node *option)
{
    if (!option) return g_strdup("");
    const char *lbl = ns_element_get_attr(option, "label");
    if (lbl) return g_strdup(lbl);
    return ns_option_text_dup(option);
}

char *
ns_option_value_dup(const ns_node *option)
{
    if (!option) return g_strdup("");
    const char *v = ns_element_get_attr(option, "value");
    if (v) return g_strdup(v);
    return ns_option_text_dup(option);
}

const ns_node *
ns_select_first_selected_option(const ns_node *select)
{
    if (!select) return NULL;
    for (const ns_node *c = select->first_child; c; c = c->next_sibling) {
        if (ns_node_is_element_named(c, "optgroup")) {
            for (const ns_node *cc = c->first_child; cc; cc = cc->next_sibling) {
                if (ns_node_is_element_named(cc, "option"))
                    if (ns_element_get_attr(cc, "selected")) return cc;
            }
        } else if (ns_node_is_element_named(c, "option")) {
            if (ns_element_get_attr(c, "selected")) return c;
        }
    }
    return NULL;
}

const ns_node *
ns_select_chosen_option(const ns_node *select)
{
    if (!select) return NULL;
    const ns_node *selected = ns_select_first_selected_option(select);
    if (selected) return selected;
    if (ns_element_get_attr(select, "data-nd-noselect")) return NULL;
    if (ns_element_get_attr(select, "multiple")) return NULL;
    const char *size = ns_element_get_attr(select, "size");
    if (size) {
        char *end = NULL;
        long v = strtol(size, &end, 10);
        if (end != size && v > 1) return NULL;
    }
    const ns_node *first = NULL;
    for (const ns_node *c = select->first_child; c && !first; c = c->next_sibling) {
        if (ns_node_is_element_named(c, "optgroup")) {
            if (ns_element_get_attr(c, "disabled")) continue;
            for (const ns_node *cc = c->first_child; cc; cc = cc->next_sibling) {
                if (ns_node_is_element_named(cc, "option") &&
                    !ns_element_get_attr(cc, "disabled")) {
                    first = cc;
                    break;
                }
            }
        } else if (ns_node_is_element_named(c, "option") &&
                   !ns_element_get_attr(c, "disabled")) {
            first = c;
        }
    }
    return first;
}

const ns_node *
ns_form_owner(const ns_node *control, const ns_node *doc)
{
    if (!control || control->kind != NS_NODE_ELEMENT) return NULL;
    if (!doc) doc = ns_node_root(control);
    const char *form_id = ns_element_get_attr(control, "form");
    if (form_id) {
        if (*form_id && doc) {
            ns_node *owner = ns_node_find_by_id(doc, form_id);
            if (ns_node_is_element_named(owner, "form")) return owner;
        }
        return NULL;
    }
    for (const ns_node *p = control->parent; p; p = p->parent)
        if (ns_node_is_element_named(p, "form")) return p;
    return NULL;
}

static void
ns_form_reset_control(ns_node *n)
{
    if (!n || n->kind != NS_NODE_ELEMENT || !n->name) return;
    if (strcmp(n->name, "input") == 0 ||
        strcmp(n->name, "textarea") == 0) {
        const char *type = ns_element_get_attr(n, "type");
        if (type && (g_ascii_strcasecmp(type, "checkbox") == 0 ||
                     g_ascii_strcasecmp(type, "radio") == 0)) {
            ns_element_remove_attr(n, "data-nd-checked");
        }
        ns_element_remove_attr(n, "data-nd-value");
    } else if (strcmp(n->name, "select") == 0) {
        ns_element_remove_attr(n, "data-nd-noselect");
        for (ns_node *o = n->first_child; o; o = o->next_sibling) {
            if (ns_node_is_element_named(o, "option"))
                ns_element_remove_attr(o, "selected");
        }
    }
}

static void
ns_form_reset_walk(ns_node *form, ns_node *scan, const ns_node *doc, int depth)
{
    if (!form || !scan || depth >= 512) return;
    if (scan->kind == NS_NODE_ELEMENT && scan->name &&
        ns_form_owner(scan, doc) == form)
        ns_form_reset_control(scan);
    for (ns_node *c = scan->first_child; c; c = c->next_sibling)
        ns_form_reset_walk(form, c, doc, depth + 1);
}

void
ns_form_reset_owned_controls(ns_node *form, ns_node *root, const ns_node *doc)
{
    ns_form_reset_walk(form, root ? root : form, doc ? doc : form, 0);
}

static gboolean
ns_node_contains(const ns_node *ancestor, const ns_node *node)
{
    for (const ns_node *p = node; p; p = p->parent)
        if (p == ancestor) return TRUE;
    return FALSE;
}

static const ns_node *
ns_fieldset_first_legend(const ns_node *fieldset)
{
    if (!fieldset) return NULL;
    for (const ns_node *c = fieldset->first_child; c; c = c->next_sibling)
        if (ns_node_is_element_named(c, "legend")) return c;
    return NULL;
}

gboolean
ns_element_supports_disabled(const ns_node *el)
{
    if (!el || el->kind != NS_NODE_ELEMENT || !el->name) return FALSE;
    return strcmp(el->name, "button") == 0 ||
           strcmp(el->name, "fieldset") == 0 ||
           strcmp(el->name, "input") == 0 ||
           strcmp(el->name, "optgroup") == 0 ||
           strcmp(el->name, "option") == 0 ||
           strcmp(el->name, "select") == 0 ||
           strcmp(el->name, "textarea") == 0;
}

gboolean
ns_element_effectively_disabled(const ns_node *el)
{
    if (!ns_element_supports_disabled(el)) return FALSE;
    if (ns_element_get_attr(el, "disabled")) return TRUE;
    for (const ns_node *p = el->parent; p; p = p->parent) {
        if (ns_node_is_element_named(el, "option") &&
            ns_node_is_element_named(p, "optgroup") &&
            ns_element_get_attr(p, "disabled"))
            return TRUE;
        if (!ns_node_is_element_named(p, "fieldset") ||
            !ns_element_get_attr(p, "disabled"))
            continue;
        const ns_node *legend = ns_fieldset_first_legend(p);
        if (legend && ns_node_contains(legend, el)) continue;
        return TRUE;
    }
    return FALSE;
}

static const ns_node *g_active_modal;

void
ns_dom_set_active_modal(const ns_node *modal)
{
    g_active_modal = modal;
}

const ns_node *
ns_dom_active_modal(void)
{
    return g_active_modal;
}

gboolean
ns_element_effectively_inert(const ns_node *el)
{
    if (!el || el->kind != NS_NODE_ELEMENT) return FALSE;
    for (const ns_node *p = el; p; p = p->parent)
        if (p->kind == NS_NODE_ELEMENT && ns_element_get_attr(p, "inert"))
            return TRUE;
    if (g_active_modal && el != g_active_modal &&
        !ns_node_contains(g_active_modal, el))
        return TRUE;
    return FALSE;
}

static void
collect_text(const ns_node *n, GString *out, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n->kind == NS_NODE_TEXT) {
        if (n->text) g_string_append(out, n->text);
        return;
    }
    if (n->kind == NS_NODE_ELEMENT && n->name &&
        (strcmp(n->name, "style")    == 0 ||
         strcmp(n->name, "script")   == 0 ||
         strcmp(n->name, "noscript") == 0 ||
         strcmp(n->name, "template") == 0))
        return;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        collect_text(c, out, depth + 1);
}

char *
ns_node_collect_text(const ns_node *root)
{
    GString *out = g_string_new(NULL);
    collect_text(root, out, 0);
    return g_string_free(out, FALSE);
}

static void
collect_all_text(const ns_node *n, GString *out, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n->kind == NS_NODE_TEXT) {
        if (n->text) g_string_append(out, n->text);
        return;
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        collect_all_text(c, out, depth + 1);
}

char *
ns_node_collect_all_text(const ns_node *root)
{
    GString *out = g_string_new(NULL);
    collect_all_text(root, out, 0);
    return g_string_free(out, FALSE);
}

#include "html.h"
#define is_void_tag ns_html_is_void

static gboolean
serialize_pre_like(const char *name)
{
    return name &&
           (g_ascii_strcasecmp(name, "pre") == 0 ||
            g_ascii_strcasecmp(name, "textarea") == 0 ||
            g_ascii_strcasecmp(name, "listing") == 0);
}

static const ns_node *
ns_serialize_shadow_child(const ns_node *host)
{
    if (!host) return NULL;
    for (const ns_node *c = host->first_child; c; c = c->next_sibling)
        if (c->kind == NS_NODE_ELEMENT &&
            ns_element_get_attr(c, NS_SHADOW_ATTR))
            return c;
    return NULL;
}

static gboolean
ns_shadow_root_included(const ns_node *sr, const ns_html_ser_opts *opts)
{
    if (!opts || !sr) return FALSE;
    for (int i = 0; i < opts->n_roots; i++)
        if (opts->roots[i] == sr) return TRUE;
    return opts->include_serializable &&
           ns_element_get_attr(sr, "data-nd-shadow-serializable") != NULL;
}

static void serialize_node_opts(const ns_node *n, GString *out,
                                gboolean include_self, int depth,
                                const ns_html_ser_opts *opts);

static void
serialize_shadow_template(const ns_node *sr, GString *out, int depth,
                          const ns_html_ser_opts *opts)
{
    const char *mode = ns_element_get_attr(sr, NS_SHADOW_ATTR);
    g_string_append(out, "<template shadowrootmode=\"");
    g_string_append(out, mode ? mode : "open");
    g_string_append_c(out, '"');
    if (ns_element_get_attr(sr, "data-nd-shadow-delegates"))
        g_string_append(out, " shadowrootdelegatesfocus=\"\"");
    if (ns_element_get_attr(sr, "data-nd-shadow-serializable"))
        g_string_append(out, " shadowrootserializable=\"\"");
    if (ns_element_get_attr(sr, "data-nd-shadow-clonable"))
        g_string_append(out, " shadowrootclonable=\"\"");
    g_string_append_c(out, '>');
    for (const ns_node *c = sr->first_child; c; c = c->next_sibling)
        serialize_node_opts(c, out, TRUE, depth + 1, opts);
    g_string_append(out, "</template>");
}

static void
serialize_node_opts(const ns_node *n, GString *out, gboolean include_self,
                    int depth, const ns_html_ser_opts *opts)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n->kind == NS_NODE_TEXT) {
        ns_html_escape_append(out, n->text, FALSE);
        return;
    }
    if (n->kind == NS_NODE_COMMENT) {
        g_string_append(out, "<!--");
        g_string_append(out, n->text ? n->text : "");
        g_string_append(out, "-->");
        return;
    }
    if (n->kind == NS_NODE_DOCTYPE) {
        g_string_append_printf(out, "<!DOCTYPE %s>", n->name ? n->name : "");
        return;
    }
    gboolean raw_text = n->kind == NS_NODE_ELEMENT && n->name &&
                        ns_html_is_raw_text(n->name);
    if (n->kind == NS_NODE_ELEMENT && include_self) {
        g_string_append_c(out, '<');
        g_string_append(out, n->name ? n->name : "");
        for (const ns_attr *a = n->attrs; a; a = a->next) {
            if (ns_attr_name_is_internal(a->name)) continue;
            g_string_append_c(out, ' ');
            g_string_append(out, a->name);
            g_string_append(out, "=\"");
            ns_html_escape_append(out, a->value, TRUE);
            g_string_append_c(out, '"');
        }
        g_string_append_c(out, '>');
        if (is_void_tag(n->name)) return;
        if (serialize_pre_like(n->name) && n->first_child &&
            n->first_child->kind == NS_NODE_TEXT && n->first_child->text &&
            n->first_child->text[0] == '\n')
            g_string_append_c(out, '\n');
    }
    if (n->tpl_content)
        for (const ns_node *c = n->tpl_content->first_child; c;
             c = c->next_sibling)
            serialize_node_opts(c, out, TRUE, depth + 1, opts);
    const ns_node *shadow = ns_serialize_shadow_child(n);
    if (shadow && ns_shadow_root_included(shadow, opts))
        serialize_shadow_template(shadow, out, depth, opts);
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c == shadow)
            continue;
        if (raw_text && c->kind == NS_NODE_TEXT)
            g_string_append(out, c->text ? c->text : "");
        else
            serialize_node_opts(c, out, TRUE, depth + 1, opts);
    }
    if (n->kind == NS_NODE_ELEMENT && include_self) {
        g_string_append(out, "</");
        g_string_append(out, n->name ? n->name : "");
        g_string_append_c(out, '>');
    }
}

static void
serialize_node(const ns_node *n, GString *out, gboolean include_self, int depth)
{
    serialize_node_opts(n, out, include_self, depth, NULL);
}

char *
ns_node_inner_html(const ns_node *root)
{
    GString *out = g_string_new(NULL);
    gboolean raw_text = root && root->kind == NS_NODE_ELEMENT && root->name &&
                        ns_html_is_raw_text(root->name);
    if (root && root->tpl_content)
        root = root->tpl_content;
    const ns_node *shadow = ns_serialize_shadow_child(root);
    if (root)
        for (const ns_node *c = root->first_child; c; c = c->next_sibling) {
            if (c == shadow) continue;
            if (raw_text && c->kind == NS_NODE_TEXT)
                g_string_append(out, c->text ? c->text : "");
            else
                serialize_node(c, out, TRUE, 0);
        }
    return g_string_free(out, FALSE);
}

char *
ns_node_get_html(const ns_node *root, const ns_html_ser_opts *opts)
{
    GString *out = g_string_new(NULL);
    gboolean raw_text = root && root->kind == NS_NODE_ELEMENT && root->name &&
                        ns_html_is_raw_text(root->name);
    if (root && root->tpl_content)
        root = root->tpl_content;
    const ns_node *shadow = ns_serialize_shadow_child(root);
    if (shadow && ns_shadow_root_included(shadow, opts))
        serialize_shadow_template(shadow, out, 0, opts);
    if (root)
        for (const ns_node *c = root->first_child; c; c = c->next_sibling) {
            if (c == shadow) continue;
            if (raw_text && c->kind == NS_NODE_TEXT)
                g_string_append(out, c->text ? c->text : "");
            else
                serialize_node_opts(c, out, TRUE, 0, opts);
        }
    return g_string_free(out, FALSE);
}

char *
ns_node_outer_html(const ns_node *node)
{
    GString *out = g_string_new(NULL);
    if (node) serialize_node(node, out, TRUE, 0);
    return g_string_free(out, FALSE);
}

#define NS_HTML_NAMESPACE "http://www.w3.org/1999/xhtml"

static const char *
ns_xml_element_ns(const ns_node *n)
{
    if (n->flags & NS_NODE_SVG_NS) return "http://www.w3.org/2000/svg";
    if (n->flags & NS_NODE_FOREIGN_NS) {
        const char *u = ns_element_get_attr(n, "data-nd-ns-uri");
        return u ? u : "";
    }
    return NS_HTML_NAMESPACE;
}

static void
ns_xml_escape_append(GString *out, const char *s, gboolean attr)
{
    for (; s && *s; s++) {
        switch (*s) {
        case '&': g_string_append(out, "&amp;"); break;
        case '<': g_string_append(out, "&lt;"); break;
        case '>': g_string_append(out, "&gt;"); break;
        case '"':
            if (attr) g_string_append(out, "&quot;");
            else g_string_append_c(out, '"');
            break;
        default:  g_string_append_c(out, *s);
        }
    }
}

static void
xml_serialize_node(const ns_node *n, GString *out, const char *parent_ns,
                   int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return;
    if (n->kind == NS_NODE_TEXT) {
        ns_xml_escape_append(out, n->text, FALSE);
        return;
    }
    if (n->kind == NS_NODE_COMMENT) {
        g_string_append(out, "<!--");
        g_string_append(out, n->text ? n->text : "");
        g_string_append(out, "-->");
        return;
    }
    if (n->kind != NS_NODE_ELEMENT) return;

    const char *ns = ns_xml_element_ns(n);
    const char *prefix = ns_element_get_attr(n, "data-nd-ns-prefix");
    g_string_append_c(out, '<');
    if (prefix && *prefix) {
        g_string_append(out, prefix);
        g_string_append_c(out, ':');
    }
    g_string_append(out, n->name ? n->name : "");
    if (ns && (!parent_ns || strcmp(ns, parent_ns) != 0)) {
        g_string_append(out, prefix && *prefix ? " xmlns:" : " xmlns");
        if (prefix && *prefix) g_string_append(out, prefix);
        g_string_append(out, "=\"");
        ns_xml_escape_append(out, ns, TRUE);
        g_string_append_c(out, '"');
    }
    for (const ns_attr *a = n->attrs; a; a = a->next) {
        if (ns_attr_name_is_internal(a->name)) continue;
        g_string_append_c(out, ' ');
        g_string_append(out, a->name);
        g_string_append(out, "=\"");
        ns_xml_escape_append(out, a->value, TRUE);
        g_string_append_c(out, '"');
    }
    gboolean html = strcmp(ns, NS_HTML_NAMESPACE) == 0;
    if (!n->first_child) {
        if (html && !is_void_tag(n->name)) {
            g_string_append(out, "></");
            if (prefix && *prefix) {
                g_string_append(out, prefix);
                g_string_append_c(out, ':');
            }
            g_string_append(out, n->name ? n->name : "");
            g_string_append_c(out, '>');
        } else {
            g_string_append(out, " />");
        }
        return;
    }
    g_string_append_c(out, '>');
    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        xml_serialize_node(c, out, ns, depth + 1);
    g_string_append(out, "</");
    if (prefix && *prefix) {
        g_string_append(out, prefix);
        g_string_append_c(out, ':');
    }
    g_string_append(out, n->name ? n->name : "");
    g_string_append_c(out, '>');
}

char *
ns_node_xml_outer_html(const ns_node *node)
{
    GString *out = g_string_new(NULL);
    if (node) xml_serialize_node(node, out, NULL, 0);
    return g_string_free(out, FALSE);
}

static void
ns_dump_text(GString *out, const char *s, gsize max)
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
ns_dump_node(GString *out, const ns_node *n, int depth)
{
    if (depth >= NS_DOM_MAX_DEPTH) return;
    for (int i = 0; i < depth; i++)
        g_string_append(out, "  ");

    switch (n->kind) {
    case NS_NODE_DOCUMENT:
        g_string_append(out, "#document\n");
        break;
    case NS_NODE_DOCTYPE:
        g_string_append_printf(out, "<!DOCTYPE %s>\n", n->name ? n->name : "");
        break;
    case NS_NODE_ELEMENT:
        g_string_append_printf(out, "<%s", n->name ? n->name : "?");
        for (const ns_attr *a = n->attrs; a; a = a->next) {
            g_string_append_printf(out, " %s=\"", a->name);
            ns_dump_text(out, a->value, 0);
            g_string_append_c(out, '"');
        }
        g_string_append(out, ">\n");
        break;
    case NS_NODE_TEXT:
        g_string_append(out, "\"");
        ns_dump_text(out, n->text, 120);
        g_string_append(out, "\"\n");
        break;
    case NS_NODE_COMMENT:
        g_string_append(out, "<!--");
        ns_dump_text(out, n->text, 120);
        g_string_append(out, "-->\n");
        break;
    }

    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        ns_dump_node(out, c, depth + 1);
}

GString *
ns_node_dump(const ns_node *node)
{
    GString *out = g_string_new(NULL);
    if (node)
        ns_dump_node(out, node, 0);
    return out;
}

static int
ns_map_parse_coords(const char *s, double *out, int max)
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
ns_map_point_in_poly(const double *pts, int npairs, double x, double y)
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
ns_area_hit(const char *shape, const char *coords, double lx, double ly,
            double iw, double ih)
{
    gboolean is_circle = shape && g_ascii_strncasecmp(shape, "circ", 4) == 0;
    gboolean is_poly   = shape && g_ascii_strncasecmp(shape, "poly", 4) == 0;
    gboolean is_default = shape && g_ascii_strcasecmp(shape, "default") == 0;
    if (is_default)
        return lx >= 0 && ly >= 0 && lx <= iw && ly <= ih;
    double c[64];
    int n = coords ? ns_map_parse_coords(coords, c, 64) : 0;
    if (is_circle) {
        if (n < 3) return FALSE;
        double dx = lx - c[0], dy = ly - c[1];
        return dx * dx + dy * dy <= c[2] * c[2];
    }
    if (is_poly) {
        if (n < 6) return FALSE;
        return ns_map_point_in_poly(c, n / 2, lx, ly);
    }
    if (n < 4) return FALSE;
    double x1 = MIN(c[0], c[2]), x2 = MAX(c[0], c[2]);
    double y1 = MIN(c[1], c[3]), y2 = MAX(c[1], c[3]);
    return lx >= x1 && lx <= x2 && ly >= y1 && ly <= y2;
}

static const ns_node *
ns_find_map(const ns_node *n, const char *name, int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return NULL;
    if (ns_node_is_element_named(n, "map")) {
        const char *mn = ns_element_get_attr(n, "name");
        const char *mid = ns_element_get_attr(n, "id");
        if ((mn && strcmp(mn, name) == 0) || (mid && strcmp(mid, name) == 0))
            return n;
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        const ns_node *r = ns_find_map(c, name, depth + 1);
        if (r) return r;
    }
    return NULL;
}

static const ns_node *
ns_map_first_area(const ns_node *n, double lx, double ly, double iw, double ih,
                  int depth)
{
    if (!n || depth >= NS_DOM_MAX_DEPTH) return NULL;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (ns_node_is_element_named(c, "area") &&
            ns_area_hit(ns_element_get_attr(c, "shape"),
                        ns_element_get_attr(c, "coords"), lx, ly, iw, ih))
            return c;
        const ns_node *r = ns_map_first_area(c, lx, ly, iw, ih, depth + 1);
        if (r) return r;
    }
    return NULL;
}

char *
ns_image_map_resolve(const ns_node *doc, const char *usemap,
                     double lx, double ly, double iw, double ih,
                     const char **out_target)
{
    if (out_target) *out_target = NULL;
    if (!doc || !usemap || lx < 0 || ly < 0) return NULL;
    const char *name = usemap[0] == '#' ? usemap + 1 : usemap;
    if (!*name) return NULL;
    const ns_node *map = ns_find_map(doc, name, 0);
    if (!map) return NULL;
    const ns_node *area = ns_map_first_area(map, lx, ly, iw, ih, 0);
    if (!area) return NULL;
    const char *href = ns_element_get_attr(area, "href");
    if (!href || !*href) return NULL;
    if (out_target) *out_target = ns_element_get_attr(area, "target");
    return g_strdup(href);
}
