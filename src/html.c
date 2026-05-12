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
