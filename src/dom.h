/* Nordstjernen — DOM data structure API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_DOM_H
#define NS_DOM_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum ns_node_kind {
    NS_NODE_DOCUMENT,
    NS_NODE_DOCTYPE,
    NS_NODE_ELEMENT,
    NS_NODE_TEXT,
    NS_NODE_COMMENT,
} ns_node_kind;

#define NS_ATTR_OWN_NAME   (1u << 0)
#define NS_ATTR_OWN_VALUE  (1u << 1)
#define NS_ATTR_NAME_LOWER (1u << 2)

#define NS_SHADOW_ATTR     "data-nd-shadow-root"
#define NS_HOST_SCOPE_ATTR "data-nd-host"
#define NS_CUSTOM_VALIDITY_ATTR "data-nd-custom-validity"
#define NS_MEDIA_SRC_ATTR "data-nd-media-src"
#define NS_MEDIA_POSTER_ATTR "data-nd-media-poster"
#define NS_MEDIA_STREAM_ATTR "data-nd-media-stream"

typedef struct ns_attr {
    char *name;
    char *value;
    char *namespace_uri;
    char *prefix;
    char *local_name;
    struct ns_attr *next;
    guint value_len;
    guint8 flags;
} ns_attr;

typedef struct ns_node ns_node;

typedef void (*ns_node_invalidator)(ns_node *self);

#define NS_NODE_OWN_NAME      (1u << 0)
#define NS_NODE_OWN_TEXT      (1u << 1)
#define NS_NODE_FRAGMENT      (1u << 2)
#define NS_NODE_IMG_LOAD_FIRED (1u << 3)
#define NS_NODE_TEMPLATE_CONTENT (1u << 4)
#define NS_NODE_QUIRKS         (1u << 5)
#define NS_NODE_LIMITED_QUIRKS (1u << 6)
#define NS_NODE_SVG_NS         (1u << 7)
#define NS_NODE_LINK_LOAD_FIRED (1u << 8)
#define NS_NODE_FOREIGN_NS     (1u << 9)
#define NS_NODE_CDATA          (1u << 10)
#define NS_NODE_PI             (1u << 11)
#define NS_NODE_KEEP_CASE      (1u << 12)
#define NS_NODE_NOT_PARSER_INSERTED (1u << 13)
#define NS_NODE_XML_DOC        (1u << 14)

struct ns_node {
    ns_node_kind kind;

    char *name;

    char *text;

    ns_attr *attrs;

    struct ns_node *parent;
    struct ns_node *first_child;
    struct ns_node *last_child;
    struct ns_node *prev_sibling;
    struct ns_node *next_sibling;

    void               *js_wrapper;
    ns_node_invalidator js_invalidate;

    void  *backing;
    void (*backing_free)(void *);

    GHashTable *id_index;
    GHashTable *class_index;
    GHashTable *tag_index;

    void *class_set;

    guint64 attr_bloom;

    guint32 attr_gen;

    guint16 flags;

    /* 1-based source line/column of this element's content in the parsed
       document (0 = unknown). Used so inline <script> stack traces report
       document-relative positions like a real browser. */
    int src_line;
    int src_col;

    struct ns_node *tpl_content;
};

ns_node *ns_node_new_document(void);
ns_node *ns_node_new_element(char *name);
ns_node *ns_node_new_text(char *text);
ns_node *ns_node_new_comment(char *text);

void ns_node_set_name_borrow(ns_node *n, const char *name);
void ns_node_set_name_owned(ns_node *n, char *name);
void ns_node_set_text_borrow(ns_node *n, const char *text);
void ns_node_replace_text_owned(ns_node *n, char *text);
void ns_node_own_strings_deep(ns_node *n);
void ns_element_append_attr_borrow(ns_node *el, const char *name, const char *value);
void ns_node_attach_backing(ns_node *root, void *backing, void (*destroy)(void *));

void ns_node_free(ns_node *node);

void ns_node_append_child(ns_node *parent, ns_node *child);
void ns_node_remove(ns_node *child);

void        ns_element_set_attr(ns_node *el, const char *name, const char *value);
void        ns_element_set_attr_len(ns_node *el, const char *name,
                                    const char *value, gssize len);
char       *ns_value_dup_len(const char *value, gsize len);
void        ns_element_set_attr_ns(ns_node *el, const char *namespace_uri,
                                   const char *prefix, const char *local_name,
                                   const char *name, const char *value);
void        ns_element_remove_attr(ns_node *el, const char *name);
void        ns_element_remove_attr_ns(ns_node *el, const char *namespace_uri,
                                      const char *local_name);
gboolean    ns_attr_name_is_internal(const char *name);

ns_node    *ns_node_clone(const ns_node *src, gboolean deep);
ns_node    *ns_template_content_get(ns_node *tpl);
const char *ns_element_get_attr(const ns_node *el, const char *name);
const char *ns_element_get_attr_len(const ns_node *el, const char *name,
                                    gsize *out_len);
const ns_attr *ns_element_find_attr_ns(const ns_node *el,
                                       const char *namespace_uri,
                                       const char *local_name);
const char *ns_attr_local_name(const ns_attr *attr);
guint64     ns_attr_name_bloom_bit(const char *name);
guint64     ns_node_attr_bloom(const ns_node *el);
gboolean    ns_node_has_class(const ns_node *el, const char *name, gsize len);
gboolean    ns_node_is_element_named(const ns_node *n, const char *tag);

const ns_node *ns_node_root(const ns_node *n);
ns_node    *ns_node_find_first_element(const ns_node *root, const char *tag);
ns_node    *ns_node_find_by_id(const ns_node *root, const char *id);
ns_node    *ns_node_find_fragment_target(const ns_node *root, const char *frag);
gboolean    ns_element_hidden_until_found(const ns_node *el);
gboolean    ns_details_fragment_needs_open(const ns_node *details,
                                           const ns_node *target);

void        ns_doc_id_index_build(ns_node *doc);
void        ns_doc_id_index_register(ns_node *doc, const char *id, ns_node *node);
void        ns_doc_id_index_unregister(ns_node *doc, const char *id, const ns_node *node);
void        ns_doc_id_index_subtree_added(ns_node *doc, ns_node *root);
void        ns_doc_id_index_subtree_removed(ns_node *doc, ns_node *root);

void        ns_doc_class_index_build(ns_node *doc);
void        ns_doc_class_index_register(ns_node *doc, const char *class_attr, ns_node *node);
void        ns_doc_class_index_unregister(ns_node *doc, const char *class_attr, ns_node *node);
void        ns_doc_class_index_subtree_added(ns_node *doc, ns_node *root);
void        ns_doc_class_index_subtree_removed(ns_node *doc, ns_node *root);

void        ns_doc_tag_index_build(ns_node *doc);
void        ns_doc_tag_index_subtree_added(ns_node *doc, ns_node *root);
void        ns_doc_tag_index_subtree_removed(ns_node *doc, ns_node *root);
GPtrArray  *ns_doc_tag_index_lookup(const ns_node *doc, const char *tag);
const ns_node *ns_select_first_selected_option(const ns_node *select);
const ns_node *ns_select_chosen_option(const ns_node *select);
char       *ns_option_value_dup(const ns_node *option);
char       *ns_option_text_dup(const ns_node *option);
char       *ns_option_label_dup(const ns_node *option);
const ns_node *ns_form_owner(const ns_node *control, const ns_node *doc);
void        ns_form_reset_owned_controls(ns_node *form, ns_node *root,
                                         const ns_node *doc);
gboolean    ns_element_effectively_disabled(const ns_node *el);
gboolean    ns_element_supports_disabled(const ns_node *el);
gboolean    ns_element_effectively_inert(const ns_node *el);
void        ns_dom_set_active_modal(const ns_node *modal);
const ns_node *ns_dom_active_modal(void);
char       *ns_node_collect_text(const ns_node *root);
char       *ns_node_collect_all_text(const ns_node *root);

char       *ns_node_inner_html(const ns_node *root);
char       *ns_node_outer_html(const ns_node *node);
char       *ns_node_xml_outer_html(const ns_node *node);

typedef struct {
    gboolean           include_serializable;
    const ns_node    **roots;
    int                n_roots;
} ns_html_ser_opts;

char       *ns_node_get_html(const ns_node *root, const ns_html_ser_opts *opts);

GString *ns_node_dump(const ns_node *node);

char *ns_image_map_resolve(const ns_node *doc, const char *usemap,
                           double lx, double ly, double iw, double ih,
                           const char **out_target);

int      ns_parse_int(const char *s, int dflt, int min_v, int max_v);
gboolean ns_input_type_has_number_value(const char *type);
gboolean ns_input_type_supports_readonly(const char *type);
gboolean ns_input_type_supports_text_constraints(const char *type);
gboolean ns_input_value_to_number(const char *type, const char *value, double *out);
gboolean ns_input_value_range_state(const ns_node *input, const char *value,
                                    gboolean *underflow, gboolean *overflow);
gboolean ns_input_value_step_mismatch(const ns_node *input, const char *value);
gboolean ns_form_control_value_missing(const ns_node *control,
                                       const char *value,
                                       const ns_node *doc);
gboolean ns_form_control_readonly_bars_validation(const ns_node *control);
gboolean ns_form_control_length_limits_apply(const ns_node *control);
gboolean ns_form_control_supports_required(const ns_node *control);
gboolean ns_input_email_value_valid(const ns_node *input, const char *value);
gboolean ns_node_is_numeric_input(const ns_node *control);
char    *ns_numeric_filter_insert(const char *insert, gsize len, gsize *out_len);

gboolean    ns_ce_attr_enables(const char *contenteditable);
gboolean    ns_node_is_text_input(const ns_node *n);
gboolean    ns_node_is_contenteditable_host(const ns_node *n);
gboolean    ns_node_is_editable(const ns_node *n);
gboolean    ns_node_spellcheck_used(const ns_node *n);
const ns_node *ns_node_spellcheck_host(const ns_node *n);
const char *ns_node_editable_value(const ns_node *n);
const char *ns_input_used_value(const ns_node *n);
gboolean    ns_input_is_checked(const ns_node *n);
gboolean    ns_input_value_is_dirty_mode(const ns_node *n);
void        ns_node_set_editable_value(ns_node *n, const char *value);
void        ns_node_flatten_editable(ns_node *n);

G_END_DECLS

#endif
