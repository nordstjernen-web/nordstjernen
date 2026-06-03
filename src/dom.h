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

#define ND_ATTR_OWN_NAME  (1u << 0)
#define ND_ATTR_OWN_VALUE (1u << 1)

#define ND_SHADOW_ATTR     "data-nd-shadow-root"
#define ND_HOST_SCOPE_ATTR "data-nd-host"
#define ND_CUSTOM_VALIDITY_ATTR "data-nd-custom-validity"

typedef struct nd_attr {
    char *name;
    char *value;
    struct nd_attr *next;
    guint8 flags;
} nd_attr;

typedef struct nd_node nd_node;

typedef void (*nd_node_invalidator)(nd_node *self);

#define ND_NODE_OWN_NAME      (1u << 0)
#define ND_NODE_OWN_TEXT      (1u << 1)
#define ND_NODE_FRAGMENT      (1u << 2)
#define ND_NODE_IMG_LOAD_FIRED (1u << 3)
#define ND_NODE_TEMPLATE_CONTENT (1u << 4)
#define ND_NODE_QUIRKS         (1u << 5)
#define ND_NODE_LIMITED_QUIRKS (1u << 6)

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

    void  *backing;
    void (*backing_free)(void *);

    GHashTable *id_index;
    GHashTable *class_index;
    GHashTable *tag_index;

    void *class_set;

    guint8 flags;
};

nd_node *nd_node_new_document(void);
nd_node *nd_node_new_element(char *name);
nd_node *nd_node_new_text(char *text);
nd_node *nd_node_new_comment(char *text);

void nd_node_set_name_borrow(nd_node *n, const char *name);
void nd_node_set_text_borrow(nd_node *n, const char *text);
void nd_node_replace_text_owned(nd_node *n, char *text);
void nd_node_own_strings_deep(nd_node *n);
void nd_element_append_attr_borrow(nd_node *el, const char *name, const char *value);
void nd_node_attach_backing(nd_node *root, void *backing, void (*destroy)(void *));

void nd_node_free(nd_node *node);

void nd_node_append_child(nd_node *parent, nd_node *child);
void nd_node_remove(nd_node *child);

void        nd_element_set_attr(nd_node *el, const char *name, const char *value);
void        nd_element_remove_attr(nd_node *el, const char *name);

nd_node    *nd_node_clone(const nd_node *src, gboolean deep);
const char *nd_element_get_attr(const nd_node *el, const char *name);
gboolean    nd_node_has_class(const nd_node *el, const char *name, gsize len);
gboolean    nd_node_is_element_named(const nd_node *n, const char *tag);

const nd_node *nd_node_root(const nd_node *n);
nd_node    *nd_node_find_first_element(const nd_node *root, const char *tag);
nd_node    *nd_node_find_by_id(const nd_node *root, const char *id);

void        nd_doc_id_index_build(nd_node *doc);
void        nd_doc_id_index_register(nd_node *doc, const char *id, nd_node *node);
void        nd_doc_id_index_unregister(nd_node *doc, const char *id, const nd_node *node);
void        nd_doc_id_index_subtree_added(nd_node *doc, nd_node *root);
void        nd_doc_id_index_subtree_removed(nd_node *doc, nd_node *root);

void        nd_doc_class_index_build(nd_node *doc);
void        nd_doc_class_index_register(nd_node *doc, const char *class_attr, nd_node *node);
void        nd_doc_class_index_unregister(nd_node *doc, const char *class_attr, nd_node *node);
void        nd_doc_class_index_subtree_added(nd_node *doc, nd_node *root);
void        nd_doc_class_index_subtree_removed(nd_node *doc, nd_node *root);

void        nd_doc_tag_index_build(nd_node *doc);
void        nd_doc_tag_index_subtree_added(nd_node *doc, nd_node *root);
void        nd_doc_tag_index_subtree_removed(nd_node *doc, nd_node *root);
GPtrArray  *nd_doc_tag_index_lookup(const nd_node *doc, const char *tag);
const nd_node *nd_select_first_selected_option(const nd_node *select);
const nd_node *nd_select_chosen_option(const nd_node *select);
char       *nd_option_value_dup(const nd_node *option);
char       *nd_option_text_dup(const nd_node *option);
char       *nd_option_label_dup(const nd_node *option);
const nd_node *nd_form_owner(const nd_node *control, const nd_node *doc);
void        nd_form_reset_owned_controls(nd_node *form, nd_node *root,
                                         const nd_node *doc);
gboolean    nd_element_effectively_disabled(const nd_node *el);
gboolean    nd_element_supports_disabled(const nd_node *el);
gboolean    nd_element_effectively_inert(const nd_node *el);
void        nd_dom_set_active_modal(const nd_node *modal);
const nd_node *nd_dom_active_modal(void);
char       *nd_node_collect_text(const nd_node *root);
char       *nd_node_collect_all_text(const nd_node *root);

char       *nd_node_inner_html(const nd_node *root);
char       *nd_node_outer_html(const nd_node *node);

GString *nd_node_dump(const nd_node *node);

char *nd_image_map_resolve(const nd_node *doc, const char *usemap,
                           double lx, double ly, double iw, double ih,
                           const char **out_target);

int      nd_parse_int(const char *s, int dflt, int min_v, int max_v);
gboolean nd_input_type_has_number_value(const char *type);
gboolean nd_input_type_supports_readonly(const char *type);
gboolean nd_input_type_supports_text_constraints(const char *type);
gboolean nd_input_value_to_number(const char *type, const char *value, double *out);
gboolean nd_input_value_range_state(const nd_node *input, const char *value,
                                    gboolean *underflow, gboolean *overflow);
gboolean nd_input_value_step_mismatch(const nd_node *input, const char *value);
gboolean nd_form_control_value_missing(const nd_node *control,
                                       const char *value,
                                       const nd_node *doc);
gboolean nd_form_control_readonly_bars_validation(const nd_node *control);
gboolean nd_form_control_length_limits_apply(const nd_node *control);
gboolean nd_form_control_supports_required(const nd_node *control);
gboolean nd_input_email_value_valid(const nd_node *input, const char *value);
gboolean nd_node_is_numeric_input(const nd_node *control);
char    *nd_numeric_filter_insert(const char *insert, gsize len, gsize *out_len);

gboolean    nd_ce_attr_enables(const char *contenteditable);
gboolean    nd_node_is_text_input(const nd_node *n);
gboolean    nd_node_is_contenteditable_host(const nd_node *n);
gboolean    nd_node_is_editable(const nd_node *n);
const char *nd_node_editable_value(const nd_node *n);
void        nd_node_set_editable_value(nd_node *n, const char *value);
void        nd_node_flatten_editable(nd_node *n);

G_END_DECLS

#endif
