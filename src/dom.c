/* Nordstjernen — DOM data structure.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "dom.h"

#include <errno.h>
#include <string.h>

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
nd_node_new_doctype(char *name)
{
    nd_node *n = nd_node_new(ND_NODE_DOCTYPE);
    n->name = name;
    return n;
}

nd_node *
nd_node_new_element(char *name)
{
    nd_node *n = nd_node_new(ND_NODE_ELEMENT);
    n->name = name;
    return n;
}

nd_node *
nd_node_new_text(char *text)
{
    nd_node *n = nd_node_new(ND_NODE_TEXT);
    n->text = text;
    return n;
}

nd_node *
nd_node_new_comment(char *text)
{
    nd_node *n = nd_node_new(ND_NODE_COMMENT);
    n->text = text;
    return n;
}

static void
nd_attr_free(nd_attr *a)
{
    while (a) {
        nd_attr *next = a->next;
        g_free(a->name);
        g_free(a->value);
        g_free(a);
        a = next;
    }
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
        g_free(cur->name);
        g_free(cur->text);
        nd_attr_free(cur->attrs);
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

    for (nd_attr *a = el->attrs; a; a = a->next) {
        if (g_ascii_strcasecmp(a->name, name) == 0) {
            g_free(a->value);
            a->value = g_strdup(value ? value : "");
            return;
        }
    }
    nd_attr *a = g_new0(nd_attr, 1);
    a->name = g_strdup(name);
    a->value = g_strdup(value ? value : "");
    a->next = NULL;
    if (!el->attrs) {
        el->attrs = a;
    } else {
        nd_attr *tail = el->attrs;
        while (tail->next)
            tail = tail->next;
        tail->next = a;
    }
}

void
nd_element_remove_attr(nd_node *el, const char *name)
{
    if (!el || el->kind != ND_NODE_ELEMENT || !name) return;
    nd_attr **link = &el->attrs;
    while (*link) {
        if (g_ascii_strcasecmp((*link)->name, name) == 0) {
            nd_attr *dead = *link;
            *link = dead->next;
            g_free(dead->name);
            g_free(dead->value);
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
        if (src->text) out->text = g_strdup(src->text);
        if (src->name) out->name = g_strdup(src->name);
        break;
    }
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
    for (const nd_node *c = root->first_child; c; c = c->next_sibling) {
        nd_node *m = nd_node_find_by_id_depth(c, id, depth + 1);
        if (m) return m;
    }
    return NULL;
}

nd_node *
nd_node_find_by_id(const nd_node *root, const char *id)
{
    return nd_node_find_by_id_depth(root, id, 0);
}

char *
nd_option_value_dup(const nd_node *option)
{
    if (!option) return g_strdup("");
    const char *v = nd_element_get_attr(option, "value");
    if (v) return g_strdup(v);
    return nd_node_collect_text(option);
}

const nd_node *
nd_select_chosen_option(const nd_node *select)
{
    if (!select) return NULL;
    const nd_node *first = NULL;
    for (const nd_node *c = select->first_child; c; c = c->next_sibling) {
        if (nd_node_is_element_named(c, "optgroup")) {
            for (const nd_node *cc = c->first_child; cc; cc = cc->next_sibling) {
                if (nd_node_is_element_named(cc, "option")) {
                    if (!first) first = cc;
                    if (nd_element_get_attr(cc, "selected")) return cc;
                }
            }
        } else if (nd_node_is_element_named(c, "option")) {
            if (!first) first = c;
            if (nd_element_get_attr(c, "selected")) return c;
        }
    }
    return first;
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
append_attr_escaped(GString *out, const char *s)
{
    for (const char *p = s ? s : ""; *p; p++) {
        switch (*p) {
        case '&':  g_string_append(out, "&amp;");  break;
        case '<':  g_string_append(out, "&lt;");   break;
        case '>':  g_string_append(out, "&gt;");   break;
        case '"':  g_string_append(out, "&quot;"); break;
        default:   g_string_append_c(out, *p);     break;
        }
    }
}

static void
append_text_escaped(GString *out, const char *s)
{
    for (const char *p = s ? s : ""; *p; p++) {
        switch (*p) {
        case '&':  g_string_append(out, "&amp;");  break;
        case '<':  g_string_append(out, "&lt;");   break;
        case '>':  g_string_append(out, "&gt;");   break;
        default:   g_string_append_c(out, *p);     break;
        }
    }
}

#include "html.h"
#define is_void_tag nd_html_is_void

static void
serialize_node(const nd_node *n, GString *out, gboolean include_self, int depth)
{
    if (!n || depth >= ND_DOM_MAX_DEPTH) return;
    if (n->kind == ND_NODE_TEXT) {
        append_text_escaped(out, n->text);
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
            append_attr_escaped(out, a->value);
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
    if (root)
        for (const nd_node *c = root->first_child; c; c = c->next_sibling)
            serialize_node(c, out, TRUE, 0);
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
