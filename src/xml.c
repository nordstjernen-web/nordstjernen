/* Nordstjernen — minimal namespaced XML / XHTML parser to the DOM.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "html.h"
#include "dom.h"

#include <glib.h>
#include <string.h>

#define XML_NS_XML   "http://www.w3.org/XML/1998/namespace"
#define XML_NS_XMLNS "http://www.w3.org/2000/xmlns/"
#define XML_NS_XHTML "http://www.w3.org/1999/xhtml"
#define XML_NS_SVG   "http://www.w3.org/2000/svg"

typedef struct {
    char *prefix;
    char *uri;
} xml_binding;

typedef struct {
    const char *p;
    const char *end;
    GPtrArray  *ns_stack;
    gboolean    ok;
} xml_parser;

static gboolean
xml_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void
xml_skip_space(xml_parser *xp)
{
    while (xp->p < xp->end && xml_is_space(*xp->p)) xp->p++;
}

static gboolean
xml_name_char(char c, gboolean first)
{
    if (c == ':' || c == '_') return TRUE;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return TRUE;
    if ((unsigned char)c >= 0x80) return TRUE;
    if (first) return FALSE;
    if (c == '-' || c == '.') return TRUE;
    if (c >= '0' && c <= '9') return TRUE;
    return FALSE;
}

static char *
xml_read_name(xml_parser *xp)
{
    const char *s = xp->p;
    if (xp->p >= xp->end || !xml_name_char(*xp->p, TRUE)) return NULL;
    xp->p++;
    while (xp->p < xp->end && xml_name_char(*xp->p, FALSE)) xp->p++;
    return g_strndup(s, (gsize)(xp->p - s));
}

static void
xml_append_codepoint(GString *out, guint64 cp)
{
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        g_string_append(out, "\xEF\xBF\xBD");
        return;
    }
    char buf[6];
    gint nb = g_unichar_to_utf8((gunichar)cp, buf);
    g_string_append_len(out, buf, nb);
}

static char *
xml_decode_text(const char *s, gsize len)
{
    GString *out = g_string_sized_new(len);
    const char *p = s, *end = s + len;
    while (p < end) {
        if (*p != '&') { g_string_append_c(out, *p++); continue; }
        const char *semi = memchr(p, ';', (gsize)(end - p));
        if (!semi) { g_string_append_c(out, *p++); continue; }
        gsize elen = (gsize)(semi - p - 1);
        const char *e = p + 1;
        if (elen == 0) { g_string_append_c(out, *p++); continue; }
        if (*e == '#') {
            guint64 cp = 0;
            if (elen >= 2 && (e[1] == 'x' || e[1] == 'X'))
                cp = g_ascii_strtoull(e + 2, NULL, 16);
            else
                cp = g_ascii_strtoull(e + 1, NULL, 10);
            xml_append_codepoint(out, cp);
            p = semi + 1;
            continue;
        }
        if (elen == 3 && strncmp(e, "amp", 3) == 0)       g_string_append_c(out, '&');
        else if (elen == 2 && strncmp(e, "lt", 2) == 0)   g_string_append_c(out, '<');
        else if (elen == 2 && strncmp(e, "gt", 2) == 0)   g_string_append_c(out, '>');
        else if (elen == 4 && strncmp(e, "quot", 4) == 0) g_string_append_c(out, '"');
        else if (elen == 4 && strncmp(e, "apos", 4) == 0) g_string_append_c(out, '\'');
        else if (elen == 4 && strncmp(e, "nbsp", 4) == 0) g_string_append(out, "\xC2\xA0");
        else { g_string_append_c(out, '&'); p++; continue; }
        p = semi + 1;
    }
    return g_string_free(out, FALSE);
}

static const char *
xml_lookup_prefix(xml_parser *xp, const char *prefix)
{
    if (prefix && strcmp(prefix, "xml") == 0)   return XML_NS_XML;
    if (prefix && strcmp(prefix, "xmlns") == 0) return XML_NS_XMLNS;
    for (guint i = xp->ns_stack->len; i > 0; i--) {
        xml_binding *b = g_ptr_array_index(xp->ns_stack, i - 1);
        if ((prefix == NULL && b->prefix == NULL) ||
            (prefix && b->prefix && strcmp(prefix, b->prefix) == 0))
            return b->uri;
    }
    return NULL;
}

static void
xml_push_binding(xml_parser *xp, const char *prefix, const char *uri)
{
    xml_binding *b = g_new0(xml_binding, 1);
    b->prefix = prefix && *prefix ? g_strdup(prefix) : NULL;
    b->uri = g_strdup(uri ? uri : "");
    g_ptr_array_add(xp->ns_stack, b);
}

static void
xml_binding_free(gpointer p)
{
    xml_binding *b = p;
    g_free(b->prefix);
    g_free(b->uri);
    g_free(b);
}

#define XML_MAX_DEPTH 256

static gboolean xml_parse_element(xml_parser *xp, ns_node *parent, int depth);

static void
xml_skip_misc_and_doctype(xml_parser *xp, ns_node *doc)
{
    for (;;) {
        xml_skip_space(xp);
        if (xp->end - xp->p < 2 || xp->p[0] != '<') return;
        char c1 = xp->p[1];
        if (c1 == '?') {
            const char *e = xp->p + 2;
            while (e + 1 < xp->end && !(e[0] == '?' && e[1] == '>')) e++;
            if (e + 1 >= xp->end) { xp->p = xp->end; return; }
            if (doc && !(xp->p[2] == 'x' && xp->p[3] == 'm' && xp->p[4] == 'l' &&
                         (e == xp->p + 5 || xml_is_space(xp->p[5])))) {
                const char *t = xp->p + 2;
                const char *tn = t;
                while (tn < e && !xml_is_space(*tn)) tn++;
                char *target = g_strndup(t, (gsize)(tn - t));
                const char *d = tn;
                while (d < e && xml_is_space(*d)) d++;
                ns_node *pi = ns_node_new_comment(g_strndup(d, (gsize)(e - d)));
                ns_node_set_name_owned(pi, target);
                pi->flags |= NS_NODE_PI;
                ns_node_append_child(doc, pi);
            }
            xp->p = e + 2;
            continue;
        }
        if (c1 == '!') {
            if (xp->end - xp->p >= 4 && strncmp(xp->p, "<!--", 4) == 0) {
                const char *e = xp->p + 4;
                while (e + 2 < xp->end && strncmp(e, "-->", 3) != 0) e++;
                if (e + 2 >= xp->end) { xp->p = xp->end; return; }
                if (doc)
                    ns_node_append_child(doc,
                        ns_node_new_comment(g_strndup(xp->p + 4,
                                                      (gsize)(e - (xp->p + 4)))));
                xp->p = e + 3;
                continue;
            }
            if (xp->end - xp->p >= 9 && g_ascii_strncasecmp(xp->p, "<!DOCTYPE", 9) == 0) {
                const char *e = xp->p + 9;
                int bracket = 0;
                while (e < xp->end) {
                    if (*e == '[') bracket++;
                    else if (*e == ']') { if (bracket > 0) bracket--; }
                    else if (*e == '>' && bracket == 0) break;
                    e++;
                }
                if (doc) {
                    const char *t = xp->p + 9;
                    while (t < e && xml_is_space(*t)) t++;
                    const char *tn = t;
                    while (tn < e && !xml_is_space(*tn) && *tn != '>' && *tn != '[') tn++;
                    if (tn > t) {
                        ns_node *dt = ns_node_new_element(NULL);
                        ns_node_set_name_owned(dt, g_strndup(t, (gsize)(tn - t)));
                        ns_element_set_attr(dt, "publicId", "");
                        ns_element_set_attr(dt, "systemId", "");
                        dt->kind = NS_NODE_DOCTYPE;
                        ns_node_append_child(doc, dt);
                    }
                }
                xp->p = (e < xp->end) ? e + 1 : xp->end;
                continue;
            }
        }
        return;
    }
}

static gboolean
xml_parse_attributes(xml_parser *xp,
                     GPtrArray *names, GPtrArray *values, guint *base)
{
    *base = xp->ns_stack->len;
    for (;;) {
        xml_skip_space(xp);
        if (xp->p >= xp->end) { xp->ok = FALSE; return FALSE; }
        if (*xp->p == '>' || *xp->p == '/') return TRUE;
        char *aname = xml_read_name(xp);
        if (!aname) { xp->ok = FALSE; return FALSE; }
        xml_skip_space(xp);
        if (xp->p >= xp->end || *xp->p != '=') { g_free(aname); xp->ok = FALSE; return FALSE; }
        xp->p++;
        xml_skip_space(xp);
        if (xp->p >= xp->end || (*xp->p != '"' && *xp->p != '\'')) {
            g_free(aname); xp->ok = FALSE; return FALSE;
        }
        char q = *xp->p++;
        const char *vs = xp->p;
        while (xp->p < xp->end && *xp->p != q) xp->p++;
        if (xp->p >= xp->end) { g_free(aname); xp->ok = FALSE; return FALSE; }
        char *aval = xml_decode_text(vs, (gsize)(xp->p - vs));
        xp->p++;

        if (strcmp(aname, "xmlns") == 0) {
            xml_push_binding(xp, NULL, aval);
        } else if (strncmp(aname, "xmlns:", 6) == 0) {
            xml_push_binding(xp, aname + 6, aval);
        }
        g_ptr_array_add(names, aname);
        g_ptr_array_add(values, aval);
    }
}

static void
xml_apply_namespace(ns_node *el, const char *qname, const char *uri)
{
    const char *colon = strchr(qname, ':');
    gboolean has_prefix = colon != NULL;
    el->flags |= NS_NODE_KEEP_CASE;
    if (uri && strcmp(uri, XML_NS_XHTML) == 0) {
        if (has_prefix) {
            char *prefix = g_strndup(qname, (gsize)(colon - qname));
            ns_element_set_attr(el, "data-nd-ns-prefix", prefix);
            g_free(prefix);
        }
        return;
    }
    if (uri && strcmp(uri, XML_NS_SVG) == 0)
        el->flags |= NS_NODE_SVG_NS;
    else
        el->flags |= NS_NODE_FOREIGN_NS;
    if (uri && *uri)
        ns_element_set_attr(el, "data-nd-ns-uri", uri);
}

static gboolean
xml_parse_element(xml_parser *xp, ns_node *parent, int depth)
{
    if (depth > XML_MAX_DEPTH) { xp->ok = FALSE; return FALSE; }
    xp->p++;
    char *qname = xml_read_name(xp);
    if (!qname) { xp->ok = FALSE; return FALSE; }

    GPtrArray *anames = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *avals  = g_ptr_array_new_with_free_func(g_free);
    guint base = 0;
    if (!xml_parse_attributes(xp, anames, avals, &base)) {
        g_free(qname);
        g_ptr_array_free(anames, TRUE);
        g_ptr_array_free(avals, TRUE);
        return FALSE;
    }

    const char *colon = strchr(qname, ':');
    char *prefix = colon ? g_strndup(qname, (gsize)(colon - qname)) : NULL;
    const char *el_uri = xml_lookup_prefix(xp, prefix);

    gboolean is_xhtml = el_uri && strcmp(el_uri, XML_NS_XHTML) == 0;
    char *stored = (is_xhtml && colon) ? g_strdup(colon + 1) : g_strdup(qname);
    ns_node *el = ns_node_new_element(stored);
    el->flags |= NS_NODE_NOT_PARSER_INSERTED;
    xml_apply_namespace(el, qname, el_uri);

    for (guint i = 0; i < anames->len; i++) {
        const char *an = g_ptr_array_index(anames, i);
        const char *av = g_ptr_array_index(avals, i);
        const char *acolon = strchr(an, ':');
        if (strcmp(an, "xmlns") == 0) {
            ns_element_set_attr_ns(el, XML_NS_XMLNS, NULL, "xmlns", "xmlns", av);
        } else if (acolon) {
            char *apfx = g_strndup(an, (gsize)(acolon - an));
            const char *auri = xml_lookup_prefix(xp, apfx);
            ns_element_set_attr_ns(el, auri, apfx, acolon + 1, an, av);
            g_free(apfx);
        } else {
            ns_element_set_attr_ns(el, NULL, NULL, an, an, av);
        }
    }
    g_ptr_array_free(anames, TRUE);
    g_ptr_array_free(avals, TRUE);
    g_free(prefix);

    ns_node_append_child(parent, el);

    xml_skip_space(xp);
    if (xp->p < xp->end && *xp->p == '/') {
        xp->p++;
        if (xp->p >= xp->end || *xp->p != '>') { xp->ok = FALSE; }
        else xp->p++;
        g_ptr_array_set_size(xp->ns_stack, base);
        g_free(qname);
        return xp->ok;
    }
    if (xp->p >= xp->end || *xp->p != '>') { xp->ok = FALSE; g_free(qname); return FALSE; }
    xp->p++;

    while (xp->ok && xp->p < xp->end) {
        if (*xp->p == '<') {
            if (xp->end - xp->p >= 2 && xp->p[1] == '/') {
                xp->p += 2;
                char *ename = xml_read_name(xp);
                xml_skip_space(xp);
                if (xp->p < xp->end && *xp->p == '>') xp->p++;
                else xp->ok = FALSE;
                if (!ename || strcmp(ename, qname) != 0) xp->ok = FALSE;
                g_free(ename);
                break;
            }
            if (xp->end - xp->p >= 4 && strncmp(xp->p, "<!--", 4) == 0) {
                const char *e = xp->p + 4;
                while (e + 2 < xp->end && strncmp(e, "-->", 3) != 0) e++;
                if (e + 2 >= xp->end) { xp->ok = FALSE; break; }
                ns_node_append_child(el,
                    ns_node_new_comment(g_strndup(xp->p + 4,
                                                  (gsize)(e - (xp->p + 4)))));
                xp->p = e + 3;
                continue;
            }
            if (xp->end - xp->p >= 9 && strncmp(xp->p, "<![CDATA[", 9) == 0) {
                const char *e = xp->p + 9;
                while (e + 2 < xp->end && strncmp(e, "]]>", 3) != 0) e++;
                if (e + 2 >= xp->end) { xp->ok = FALSE; break; }
                ns_node *t = ns_node_new_text(g_strndup(xp->p + 9,
                                                        (gsize)(e - (xp->p + 9))));
                t->flags |= NS_NODE_CDATA;
                ns_node_append_child(el, t);
                xp->p = e + 3;
                continue;
            }
            if (xp->end - xp->p >= 2 && xp->p[1] == '?') {
                const char *e = xp->p + 2;
                while (e + 1 < xp->end && !(e[0] == '?' && e[1] == '>')) e++;
                if (e + 1 >= xp->end) { xp->ok = FALSE; break; }
                const char *t = xp->p + 2;
                const char *tn = t;
                while (tn < e && !xml_is_space(*tn)) tn++;
                char *target = g_strndup(t, (gsize)(tn - t));
                const char *d = tn;
                while (d < e && xml_is_space(*d)) d++;
                ns_node *pi = ns_node_new_comment(g_strndup(d, (gsize)(e - d)));
                ns_node_set_name_owned(pi, target);
                pi->flags |= NS_NODE_PI;
                ns_node_append_child(el, pi);
                xp->p = e + 2;
                continue;
            }
            if (!xml_parse_element(xp, el, depth + 1)) break;
            continue;
        }
        const char *ts = xp->p;
        while (xp->p < xp->end && *xp->p != '<') xp->p++;
        char *txt = xml_decode_text(ts, (gsize)(xp->p - ts));
        ns_node_append_child(el, ns_node_new_text(txt));
    }

    g_ptr_array_set_size(xp->ns_stack, base);
    g_free(qname);
    return xp->ok;
}

static void
xml_offset_to_line_column(const char *input, gsize offset, int *line, int *column)
{
    int ln = 1, col = 1;
    for (gsize i = 0; i < offset; i++) {
        if (input[i] == '\n') { ln++; col = 1; }
        else col++;
    }
    if (line) *line = ln;
    if (column) *column = col;
}

ns_node *
ns_xml_parse_reporting(const char *input, gssize len, int *line, int *column)
{
    if (line) *line = 1;
    if (column) *column = 1;
    if (!input) return NULL;
    gsize n = (len < 0) ? strlen(input) : (gsize)len;
    xml_parser xp = {
        .p = input,
        .end = input + n,
        .ns_stack = g_ptr_array_new_with_free_func(xml_binding_free),
        .ok = TRUE,
    };
    if (n >= 3 && (unsigned char)xp.p[0] == 0xEF &&
        (unsigned char)xp.p[1] == 0xBB && (unsigned char)xp.p[2] == 0xBF)
        xp.p += 3;

    ns_node *doc = ns_node_new_document();
    xml_skip_misc_and_doctype(&xp, doc);

    ns_node *root = NULL;
    if (xp.p < xp.end && *xp.p == '<') {
        guint depth = xp.ns_stack->len;
        if (xml_parse_element(&xp, doc, 0))
            root = doc->last_child;
        (void)depth;
    } else {
        xp.ok = FALSE;
    }

    if (xp.ok)
        xml_skip_misc_and_doctype(&xp, doc);

    g_ptr_array_free(xp.ns_stack, TRUE);

    if (!xp.ok || !root) {
        xml_offset_to_line_column(input, (gsize)(xp.p - input), line, column);
        ns_node_free(doc);
        return NULL;
    }
    return doc;
}

ns_node *
ns_xml_parse(const char *input, gssize len)
{
    return ns_xml_parse_reporting(input, len, NULL, NULL);
}
