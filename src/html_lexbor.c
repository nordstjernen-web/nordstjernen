/* Nordstjernen — lexbor-backed HTML parser. */

#include "html.h"

#include <string.h>

#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>

static nd_node *lxb_to_nd(lxb_dom_node_t *node);

static void
lxb_attach_children(nd_node *parent, lxb_dom_node_t *first)
{
    for (lxb_dom_node_t *c = first; c; c = c->next) {
        nd_node *nn = lxb_to_nd(c);
        if (nn) nd_node_append_child(parent, nn);
    }
}

static char *
lxb_strdup_lower(const lxb_char_t *data, size_t len)
{
    if (!data || len == 0) return g_strdup("unknown");
    return g_ascii_strdown((const char *)data, (gssize)len);
}

static char *
lxb_strdup_n(const lxb_char_t *data, size_t len)
{
    if (!data) return g_strdup("");
    return g_strndup((const char *)data, len);
}

static void
lxb_copy_attributes(lxb_dom_element_t *el, nd_node *out)
{
    lxb_dom_attr_t *attr = lxb_dom_element_first_attribute(el);
    while (attr) {
        size_t klen = 0, vlen = 0;
        const lxb_char_t *k = lxb_dom_attr_qualified_name(attr, &klen);
        const lxb_char_t *v = lxb_dom_attr_value(attr, &vlen);
        if (k && klen > 0) {
            char *kk = lxb_strdup_n(k, klen);
            char *vv = v ? lxb_strdup_n(v, vlen) : g_strdup("");
            nd_element_set_attr(out, kk, vv);
            g_free(kk);
            g_free(vv);
        }
        attr = lxb_dom_element_next_attribute(attr);
    }
}

static nd_node *
lxb_to_nd(lxb_dom_node_t *node)
{
    if (!node) return NULL;
    switch (node->type) {
    case LXB_DOM_NODE_TYPE_DOCUMENT: {
        nd_node *doc = nd_node_new_document();
        lxb_attach_children(doc, node->first_child);
        return doc;
    }
    case LXB_DOM_NODE_TYPE_ELEMENT: {
        lxb_dom_element_t *el = lxb_dom_interface_element(node);
        size_t nlen = 0;
        const lxb_char_t *name = lxb_dom_element_qualified_name(el, &nlen);
        char *lower = lxb_strdup_lower(name, nlen);
        nd_node *out = nd_node_new_element(lower);
        lxb_copy_attributes(el, out);
        lxb_attach_children(out, node->first_child);
        return out;
    }
    case LXB_DOM_NODE_TYPE_TEXT:
    case LXB_DOM_NODE_TYPE_CDATA_SECTION: {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(node);
        return nd_node_new_text(lxb_strdup_n(cd->data.data, cd->data.length));
    }
    case LXB_DOM_NODE_TYPE_COMMENT: {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(node);
        return nd_node_new_comment(lxb_strdup_n(cd->data.data, cd->data.length));
    }
    default:
        return NULL;
    }
}

nd_node *
nd_html_parse_lexbor(const char *input, gssize len)
{
    if (!input) return NULL;
    size_t n = (len < 0) ? strlen(input) : (size_t)len;
    lxb_html_document_t *doc = lxb_html_document_create();
    if (!doc) return NULL;
    lxb_status_t status = lxb_html_document_parse(doc,
                                                  (const lxb_char_t *)input, n);
    if (status != LXB_STATUS_OK) {
        lxb_html_document_destroy(doc);
        return NULL;
    }
    nd_node *root = lxb_to_nd(lxb_dom_interface_node(doc));
    lxb_html_document_destroy(doc);
    return root;
}
