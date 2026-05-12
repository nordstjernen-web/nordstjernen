/* Nordstjernen — HTML parser (gumbo-parser backed). */

#include "html.h"

#include <gumbo.h>
#include <string.h>

gboolean
nd_html_is_void(const char *tag)
{
    if (!tag) return FALSE;
    static const char *const voids[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr",
        NULL,
    };
    for (int i = 0; voids[i]; i++)
        if (strcmp(tag, voids[i]) == 0)
            return TRUE;
    return FALSE;
}

static nd_node *gumbo_to_nd(const GumboNode *gn);

static void
gumbo_attach_children(nd_node *parent, const GumboVector *children)
{
    if (!children) return;
    for (unsigned i = 0; i < children->length; i++) {
        nd_node *c = gumbo_to_nd((const GumboNode *)children->data[i]);
        if (c) nd_node_append_child(parent, c);
    }
}

static char *
gumbo_tag_name_dup(const GumboElement *el)
{
    if (el->tag != GUMBO_TAG_UNKNOWN) {
        const char *t = gumbo_normalized_tagname(el->tag);
        if (t && *t) return g_ascii_strdown(t, -1);
    }
    if (el->original_tag.data && el->original_tag.length > 0) {
        GumboStringPiece sp = el->original_tag;
        gumbo_tag_from_original_text(&sp);
        if (sp.length > 0)
            return g_ascii_strdown(sp.data, (gssize)sp.length);
    }
    return g_strdup("unknown");
}

static nd_node *
gumbo_to_nd(const GumboNode *gn)
{
    if (!gn) return NULL;
    switch (gn->type) {
    case GUMBO_NODE_DOCUMENT: {
        nd_node *doc = nd_node_new_document();
        gumbo_attach_children(doc, &gn->v.document.children);
        return doc;
    }
    case GUMBO_NODE_ELEMENT: {
        char *name = gumbo_tag_name_dup(&gn->v.element);
        nd_node *el = nd_node_new_element(name);
        const GumboVector *attrs = &gn->v.element.attributes;
        for (unsigned i = 0; i < attrs->length; i++) {
            const GumboAttribute *a = (const GumboAttribute *)attrs->data[i];
            if (a->name && a->value)
                nd_element_set_attr(el, a->name, a->value);
        }
        gumbo_attach_children(el, &gn->v.element.children);
        return el;
    }
    case GUMBO_NODE_TEXT:
    case GUMBO_NODE_WHITESPACE:
    case GUMBO_NODE_CDATA:
        return nd_node_new_text(g_strdup(gn->v.text.text ? gn->v.text.text : ""));
    case GUMBO_NODE_COMMENT:
        return nd_node_new_comment(g_strdup(gn->v.text.text ? gn->v.text.text : ""));
    case GUMBO_NODE_TEMPLATE:
        return NULL;
    }
    return NULL;
}

nd_node *
nd_html_parse(const char *input, gssize len)
{
    if (!input) return NULL;
    gsize n = (len < 0) ? strlen(input) : (gsize)len;
    GumboOptions opts = kGumboDefaultOptions;
    GumboOutput *out = gumbo_parse_with_options(&opts, input, n);
    if (!out) return NULL;
    nd_node *root = gumbo_to_nd(out->document);
    gumbo_destroy_output(&opts, out);
    return root;
}

nd_node *
nd_html_parse_for_page(const char *input, gssize len)
{
    return nd_html_parse(input, len);
}

static nd_node *
find_child_named(nd_node *parent, const char *tag)
{
    if (!parent) return NULL;
    for (nd_node *c = parent->first_child; c; c = c->next_sibling)
        if (c->kind == ND_NODE_ELEMENT && c->name
            && g_ascii_strcasecmp(c->name, tag) == 0)
            return c;
    return NULL;
}

nd_node *
nd_html_parse_fragment(const char *input, gssize len)
{
    nd_node *doc = nd_html_parse(input, len);
    if (!doc) return NULL;
    nd_node *html_el = find_child_named(doc, "html");
    nd_node *body    = html_el ? find_child_named(html_el, "body") : NULL;
    if (!body) return doc;
    nd_node *frag = nd_node_new_document();
    nd_node *c = body->first_child;
    while (c) {
        nd_node *next = c->next_sibling;
        nd_node_remove(c);
        nd_node_append_child(frag, c);
        c = next;
    }
    nd_node_free(doc);
    return frag;
}

static char *
extract_http_charset(const char *content_type)
{
    if (!content_type) return NULL;
    const char *s = strstr(content_type, "charset=");
    if (!s) s = strstr(content_type, "charset =");
    if (!s) return NULL;
    s += strlen("charset");
    while (*s == ' ') s++;
    if (*s == '=') s++;
    while (*s == ' ' || *s == '"' || *s == '\'') s++;
    const char *e = s;
    while (*e && *e != ';' && *e != ' ' && *e != '"' && *e != '\'' &&
           *e != '\r' && *e != '\n')
        e++;
    return e == s ? NULL : g_strndup(s, (gsize)(e - s));
}

static char *
sniff_meta_charset(const char *body, gsize len)
{
    if (!body) return NULL;
    gsize scan = len < 2048 ? len : 2048;
    GString *lower = g_string_new(NULL);
    for (gsize i = 0; i < scan; i++) {
        char c = body[i];
        g_string_append_c(lower, (c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    char *result = NULL;
    const char *p = strstr(lower->str, "charset=");
    if (p) {
        p += 8;
        while (*p == ' ' || *p == '"' || *p == '\'') p++;
        const char *q = p;
        while (*q && *q != '"' && *q != '\'' && *q != ' ' && *q != '/' &&
               *q != '>' && *q != ';' && *q != '\r' && *q != '\n')
            q++;
        if (q > p) result = g_strndup(p, (gsize)(q - p));
    }
    g_string_free(lower, TRUE);
    return result;
}

static const char *
detect_bom(const char *body, gsize len, gsize *skip)
{
    if (!body || len < 2) return NULL;
    const guint8 *p = (const guint8 *)body;
    if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        *skip = 3; return "UTF-8";
    }
    if (len >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0xFE && p[3] == 0xFF) {
        *skip = 4; return "UTF-32BE";
    }
    if (len >= 4 && p[0] == 0xFF && p[1] == 0xFE && p[2] == 0 && p[3] == 0) {
        *skip = 4; return "UTF-32LE";
    }
    if (p[0] == 0xFE && p[1] == 0xFF) { *skip = 2; return "UTF-16BE"; }
    if (p[0] == 0xFF && p[1] == 0xFE) { *skip = 2; return "UTF-16LE"; }
    return NULL;
}

char *
nd_html_decode_body(const char *body, gsize len, const char *content_type)
{
    if (!body || len == 0) return g_strdup("");

    gsize bom_skip = 0;
    const char *bom_charset = detect_bom(body, len, &bom_skip);
    if (bom_charset) {
        body += bom_skip;
        len  -= bom_skip;
        if (strcmp(bom_charset, "UTF-8") == 0 &&
            g_utf8_validate(body, (gssize)len, NULL))
            return g_strndup(body, len);
        GError *err = NULL;
        char *out = g_convert(body, (gssize)len, "UTF-8", bom_charset,
                              NULL, NULL, &err);
        if (out) return out;
        g_clear_error(&err);
    }

    char *charset = extract_http_charset(content_type);
    if (!charset) charset = sniff_meta_charset(body, len);

    if (charset) {
        char *upper = g_ascii_strup(charset, -1);
        gboolean is_utf8 = strcmp(upper, "UTF-8") == 0 ||
                           strcmp(upper, "UTF8") == 0 ||
                           strcmp(upper, "US-ASCII") == 0 ||
                           strcmp(upper, "ASCII") == 0;
        g_free(upper);
        if (is_utf8) {
            g_free(charset);
            if (g_utf8_validate(body, (gssize)len, NULL))
                return g_strndup(body, len);
        } else {
            GError *err = NULL;
            char *out = g_convert(body, (gssize)len, "UTF-8", charset,
                                  NULL, NULL, &err);
            g_free(charset);
            if (out) return out;
            g_clear_error(&err);
        }
    } else if (g_utf8_validate(body, (gssize)len, NULL)) {
        return g_strndup(body, len);
    }

    GError *err = NULL;
    char *out = g_convert(body, (gssize)len, "UTF-8", "ISO-8859-1",
                          NULL, NULL, &err);
    if (out) return out;
    g_clear_error(&err);
    return g_strdup("(unable to decode response body)\n");
}
