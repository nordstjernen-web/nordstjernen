/*
 * Nordstjernen — dom.h
 *
 * Minimal DOM. Not a faithful WHATWG implementation; just enough
 * structure for a layout engine to walk.
 *
 * Memory model: nodes own their children. Detaching a node and freeing
 * it frees the whole subtree. Strings (tag names, text, attribute
 * keys/values) are owned by the node and freed with the node.
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
    char *name;   /* lowercased */
    char *value;  /* may be empty string but not NULL */
    struct nd_attr *next;
} nd_attr;

typedef struct nd_node {
    nd_node_kind kind;
    /* For ELEMENT: lowercased tag name. For DOCTYPE: doctype name.
     * NULL otherwise. */
    char *name;
    /* For TEXT and COMMENT: payload. NULL otherwise. */
    char *text;
    /* For ELEMENT: linked list of attributes; NULL otherwise. */
    nd_attr *attrs;

    struct nd_node *parent;
    struct nd_node *first_child;
    struct nd_node *last_child;
    struct nd_node *prev_sibling;
    struct nd_node *next_sibling;
} nd_node;

/* Construction. The constructors take ownership of the strings they
 * accept (use g_strdup at the call site if needed). */
nd_node *nd_node_new_document(void);
nd_node *nd_node_new_doctype(char *name);
nd_node *nd_node_new_element(char *name);
nd_node *nd_node_new_text(char *text);
nd_node *nd_node_new_comment(char *text);

/* Free a node and all its descendants. Safe on NULL. */
void nd_node_free(nd_node *node);

/* Append child to parent's children list. Detaches child from any
 * existing parent first. */
void nd_node_append_child(nd_node *parent, nd_node *child);

/* Attribute manipulation. Adding overwrites an existing value with the
 * same name. Both name and value are duplicated. */
void        nd_element_set_attr(nd_node *el, const char *name, const char *value);
const char *nd_element_get_attr(const nd_node *el, const char *name);

/* Debug dump: indented tree, one node per line. Caller owns the
 * returned GString. */
GString *nd_node_dump(const nd_node *node);

G_END_DECLS

#endif /* ND_DOM_H */
