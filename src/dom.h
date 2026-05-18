/* Nordstjernen — DOM data structure API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_DOM_H
#define ND_DOM_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum nd_node_kind {
    ND_NODE_DOCUMENT,
    ND_NODE_DOCTYPE,
    ND_NODE_ELEMENT,
    ND_NODE_TEXT,
    ND_NODE_COMMENT,
} nd_node_kind;

typedef struct nd_attr {
    char *name;
    char *value;
    struct nd_attr *next;
} nd_attr;

typedef struct nd_node nd_node;

typedef void (*nd_node_invalidator)(nd_node *self);

struct nd_node {
    nd_node_kind kind;

    char *name;

    char *text;

    nd_attr *attrs;

    struct nd_node *parent;
    struct nd_node *first_child;
    struct nd_node *last_child;
    struct nd_node *prev_sibling;
    struct nd_node *next_sibling;

    void               *js_wrapper;
    nd_node_invalidator js_invalidate;
};

nd_node *nd_node_new_document(void);
nd_node *nd_node_new_doctype(char *name);
nd_node *nd_node_new_element(char *name);
nd_node *nd_node_new_text(char *text);
nd_node *nd_node_new_comment(char *text);

void nd_node_free(nd_node *node);

void nd_node_append_child(nd_node *parent, nd_node *child);
void nd_node_remove(nd_node *child);

void        nd_element_set_attr(nd_node *el, const char *name, const char *value);
void        nd_element_remove_attr(nd_node *el, const char *name);

nd_node    *nd_node_clone(const nd_node *src, gboolean deep);
const char *nd_element_get_attr(const nd_node *el, const char *name);
gboolean    nd_node_is_element_named(const nd_node *n, const char *tag);

nd_node    *nd_node_find_first_element(const nd_node *root, const char *tag);
nd_node    *nd_node_find_by_id(const nd_node *root, const char *id);
const nd_node *nd_select_chosen_option(const nd_node *select);
char       *nd_option_value_dup(const nd_node *option);
char       *nd_node_collect_text(const nd_node *root);

char       *nd_node_inner_html(const nd_node *root);
char       *nd_node_outer_html(const nd_node *node);

GString *nd_node_dump(const nd_node *node);

int      nd_parse_int(const char *s, int dflt, int min_v, int max_v);

G_END_DECLS

#endif
