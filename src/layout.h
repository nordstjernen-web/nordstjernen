/* Nordstjernen — layout tree API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

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
    ND_BOX_TABLE,
    ND_BOX_TABLE_ROW,
    ND_BOX_TABLE_CELL,
    ND_BOX_VIDEO,
} nd_box_kind;

const char *nd_box_kind_name(nd_box_kind k);

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
    char *target;
    const nd_node *dom;
} nd_link_range;

typedef enum nd_inline_attr_kind {
    ND_INLINE_BOLD,
    ND_INLINE_ITALIC,
    ND_INLINE_MONOSPACE,
    ND_INLINE_UNDERLINE,
    ND_INLINE_STRIKETHROUGH,
    ND_INLINE_INPUT_FIELD,
    ND_INLINE_INPUT_FIELD_FOCUSED,
    ND_INLINE_BUTTON,
    ND_INLINE_FONT_SIZE,
    ND_INLINE_COLOR,
    ND_INLINE_FONT_FAMILY,
    ND_INLINE_BG_COLOR,
    ND_INLINE_SUPERSCRIPT,
    ND_INLINE_SUBSCRIPT,
    ND_INLINE_SMALL_CAPS,
    ND_INLINE_CARET,
} nd_inline_attr_kind;

typedef struct nd_inline_attr {
    nd_inline_attr_kind kind;
    gsize start;
    gsize len;
    double font_size_px;
    guint8 r, g, b, a;
    const char *family;
    const nd_node *dom;
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
    char  *bg_image_src;
    void  *bg_image;
    char  *video_src;
    char  *video_poster;
    void  *video;

    int colspan;
    int rowspan;

    struct nd_box *parent;
    struct nd_box *first_child;
    struct nd_box *last_child;
    struct nd_box *next_sibling;
} nd_box;

void nd_box_free(nd_box *box);

struct nd_image_cache;
nd_box *nd_layout_build(const nd_node *doc, GHashTable *styles,
                        double viewport_width,
                        const nd_node *focused_input,
                        gsize focused_caret_byte,
                        struct nd_image_cache *image_cache,
                        const char *base_url);

void nd_layout_collect_images(const nd_box *root, GPtrArray *out_boxes);
void nd_layout_collect_videos(const nd_box *root, GPtrArray *out_boxes);

void nd_box_content_extent(const nd_box *root, double *out_w, double *out_h);

GString *nd_box_dump(const nd_box *root);

const char *nd_box_hit_link(const nd_box *root, double x, double y);
const nd_link_range *nd_box_hit_link_range(const nd_box *root, double x, double y);

const nd_box *nd_box_find_by_id(const nd_box *root, const char *id);

const nd_box *nd_box_hit_test(const nd_box *root, double x, double y);

const nd_node *nd_box_hit_form_dom(const nd_box *root, double x, double y);

guint nd_box_count_matches(const nd_box *root, const char *needle);

const nd_box *nd_box_first_match_below(const nd_box *root,
                                       const char *needle,
                                       double y_threshold);

G_END_DECLS

#endif
