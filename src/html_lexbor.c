/* Nordstjernen — lexbor-backed HTML parser.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "html.h"

#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>
#include <lexbor/html/interfaces/template_element.h>

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
    nd_attr *head = NULL, *tail = NULL;
    lxb_dom_attr_t *attr = lxb_dom_element_first_attribute(el);
    while (attr) {
        size_t klen = 0, vlen = 0;
        const lxb_char_t *k = lxb_dom_attr_qualified_name(attr, &klen);
        const lxb_char_t *v = lxb_dom_attr_value(attr, &vlen);
        if (k && klen > 0) {
            nd_attr *a = g_new0(nd_attr, 1);
            a->name  = g_strndup((const char *)k, klen);
            a->value = v ? g_strndup((const char *)v, vlen) : g_strdup("");
            if (tail) tail->next = a;
            else      head = a;
            tail = a;
        }
        attr = lxb_dom_element_next_attribute(attr);
    }
    out->attrs = head;
}

static nd_node *
lxb_node_convert(lxb_dom_node_t *src)
{
    switch (src->type) {
    case LXB_DOM_NODE_TYPE_DOCUMENT:
    case LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT:
        return nd_node_new_document();
    case LXB_DOM_NODE_TYPE_ELEMENT: {
        lxb_dom_element_t *el = lxb_dom_interface_element(src);
        size_t nlen = 0;
        const lxb_char_t *name = lxb_dom_element_qualified_name(el, &nlen);
        char *lower = lxb_strdup_lower(name, nlen);
        nd_node *out = nd_node_new_element(lower);
        lxb_copy_attributes(el, out);
        return out;
    }
    case LXB_DOM_NODE_TYPE_TEXT:
    case LXB_DOM_NODE_TYPE_CDATA_SECTION: {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(src);
        return nd_node_new_text(lxb_strdup_n(cd->data.data, cd->data.length));
    }
    case LXB_DOM_NODE_TYPE_COMMENT: {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(src);
        return nd_node_new_comment(lxb_strdup_n(cd->data.data, cd->data.length));
    }
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE:
    case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
    case LXB_DOM_NODE_TYPE_ATTRIBUTE:
    case LXB_DOM_NODE_TYPE_ENTITY_REFERENCE:
    case LXB_DOM_NODE_TYPE_ENTITY:
    case LXB_DOM_NODE_TYPE_NOTATION:
    default:
        return NULL;
    }
}

static lxb_dom_node_t *
lxb_template_content_first_child(lxb_dom_node_t *src)
{
    if (src->type != LXB_DOM_NODE_TYPE_ELEMENT) return NULL;
    if (src->local_name != LXB_TAG_TEMPLATE) return NULL;
    lxb_html_template_element_t *tpl = lxb_html_interface_template(src);
    if (!tpl || !tpl->content) return NULL;
    return tpl->content->node.first_child;
}

typedef struct lxb_walk_frame {
    lxb_dom_node_t *src_child;
    nd_node        *nd_parent;
} lxb_walk_frame;

static void
lxb_walk_push(GArray *stack, lxb_dom_node_t *child, nd_node *parent)
{
    if (!child || !parent) return;
    lxb_walk_frame fr = { .src_child = child, .nd_parent = parent };
    g_array_append_val(stack, fr);
}

static void
lxb_walk_into(lxb_dom_node_t *src_root, nd_node *nd_root)
{
    GArray *stack = g_array_new(FALSE, FALSE, sizeof(lxb_walk_frame));
    lxb_walk_push(stack, src_root->first_child, nd_root);
    lxb_walk_push(stack, lxb_template_content_first_child(src_root), nd_root);
    while (stack->len > 0) {
        lxb_walk_frame fr = g_array_index(stack, lxb_walk_frame, stack->len - 1);
        g_array_set_size(stack, stack->len - 1);
        lxb_dom_node_t *src = fr.src_child;
        nd_node *parent = fr.nd_parent;
        while (src) {
            lxb_dom_node_t *next = src->next;
            nd_node *converted = lxb_node_convert(src);
            if (converted) {
                nd_node_append_child(parent, converted);
                lxb_dom_node_t *kids = src->first_child;
                lxb_dom_node_t *tpl_kids = lxb_template_content_first_child(src);
                if (next) lxb_walk_push(stack, next, parent);
                if (tpl_kids) lxb_walk_push(stack, tpl_kids, converted);
                if (kids) {
                    src = kids;
                    parent = converted;
                    continue;
                }
            } else if (next) {
                src = next;
                continue;
            }
            src = NULL;
        }
    }
    g_array_free(stack, TRUE);
}

static nd_node *
lxb_to_nd_root(lxb_dom_node_t *root)
{
    if (!root) return NULL;
    nd_node *out = lxb_node_convert(root);
    if (!out) return NULL;
    lxb_walk_into(root, out);
    return out;
}

nd_node *
nd_html_parse_lexbor(const char *input, gssize len)
{
    if (!input) return NULL;
    size_t n = (len < 0) ? strlen(input) : (size_t)len;
    lxb_html_document_t *doc = lxb_html_document_create();
    if (!doc) return NULL;
    lxb_html_document_dom_opt_set(doc, LXB_DOM_DOCUMENT_OPT_WO_EVENTS);
    lxb_status_t status = lxb_html_document_parse(doc,
                                                  (const lxb_char_t *)input, n);
    if (status != LXB_STATUS_OK) {
        lxb_html_document_destroy(doc);
        return NULL;
    }
    nd_node *root = lxb_to_nd_root(lxb_dom_interface_node(doc));
    lxb_html_document_destroy(doc);
    return root;
}

static lxb_tag_id_t
lxb_tag_id_from_name(lxb_html_document_t *doc, const char *name)
{
    if (!name || !*name) return LXB_TAG_BODY;
    lexbor_hash_t *hash = doc->dom_document.tags;
    const lxb_tag_data_t *data = lxb_tag_data_by_name(hash,
        (const lxb_char_t *)name, strlen(name));
    if (!data) return LXB_TAG_BODY;
    return data->tag_id;
}

nd_node *
nd_html_parse_fragment_lexbor(const char *context_tag,
                              const char *input, gssize len)
{
    if (!input) return NULL;
    size_t n = (len < 0) ? strlen(input) : (size_t)len;
    lxb_html_parser_t *parser = lxb_html_parser_create();
    if (!parser || lxb_html_parser_init(parser) != LXB_STATUS_OK) {
        if (parser) lxb_html_parser_destroy(parser);
        return NULL;
    }
    lxb_html_parser_dom_opt_set(parser, LXB_DOM_DOCUMENT_OPT_WO_EVENTS);
    lxb_html_document_t *doc = lxb_html_document_create();
    if (!doc) {
        lxb_html_parser_destroy(parser);
        return NULL;
    }
    lxb_tag_id_t tag_id = lxb_tag_id_from_name(doc, context_tag);
    lxb_dom_node_t *frag = lxb_html_parse_fragment_by_tag_id(
        parser, doc, tag_id, LXB_NS_HTML,
        (const lxb_char_t *)input, n);
    if (!frag) {
        lxb_html_parser_destroy(parser);
        lxb_html_document_destroy(doc);
        return NULL;
    }
    nd_node *out = nd_node_new_document();
    lxb_walk_into(frag, out);
    lxb_html_parser_destroy(parser);
    lxb_html_document_destroy(doc);
    return out;
}
