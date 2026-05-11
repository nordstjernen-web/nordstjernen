/* Nordstjernen — layout tree API. */

#ifndef ND_LAYOUT_H
#define ND_LAYOUT_H

#include <glib.h>

#include "css.h"
#include "dom.h"

G_BEGIN_DECLS

typedef enum nd_box_kind {
    ND_BOX_BLOCK,
    ND_BOX_INLINE,
    ND_BOX_TEXT,
    ND_BOX_IMAGE,
} nd_box_kind;

typedef struct nd_edges {
    double top, right, bottom, left;
} nd_edges;

typedef struct nd_line {
    double y;
    double height;
    int    char_count;
    char  *text;
} nd_line;

typedef struct nd_link_range {
    gsize start;
    gsize len;
    char *href;
} nd_link_range;

typedef enum nd_inline_attr_kind {
    ND_INLINE_BOLD,
    ND_INLINE_ITALIC,
    ND_INLINE_MONOSPACE,
    ND_INLINE_UNDERLINE,
    ND_INLINE_STRIKETHROUGH,
} nd_inline_attr_kind;

typedef struct nd_inline_attr {
    nd_inline_attr_kind kind;
    gsize start;
    gsize len;
} nd_inline_attr;

typedef struct nd_box {
    nd_box_kind kind;
    const nd_node  *dom;
    const nd_style *style;

    double x, y;

    double content_width, content_height;
    nd_edges margin, padding, border;

    char *text;

    GArray *lines;
    GArray *links;
    GArray *attrs;

    char  *image_src;
    void  *image;

    struct nd_box *parent;
    struct nd_box *first_child;
    struct nd_box *last_child;
    struct nd_box *next_sibling;
} nd_box;

void nd_box_free(nd_box *box);

nd_box *nd_layout_build(const nd_node *doc, GHashTable *styles,
                        double viewport_width);

void nd_layout_collect_images(const nd_box *root, GPtrArray *out_boxes);

GString *nd_box_dump(const nd_box *root);

const char *nd_box_hit_link(const nd_box *root, double x, double y);

G_END_DECLS

#endif
