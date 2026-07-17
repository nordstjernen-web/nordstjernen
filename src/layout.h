/* Nordstjernen — layout tree API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_LAYOUT_H
#define NS_LAYOUT_H

#include <glib.h>

#include "css.h"
#include "dom.h"

G_BEGIN_DECLS

typedef enum ns_box_kind {
    NS_BOX_BLOCK,
    NS_BOX_INLINE,
    NS_BOX_TEXT,
    NS_BOX_IMAGE,
    NS_BOX_TABLE,
    NS_BOX_TABLE_CAPTION,
    NS_BOX_TABLE_ROW,
    NS_BOX_TABLE_CELL,
    NS_BOX_VIDEO,
    NS_BOX_MATH,
} ns_box_kind;

const char *ns_box_kind_name(ns_box_kind k);

typedef struct ns_edges {
    double top, right, bottom, left;
} ns_edges;

typedef struct ns_link_range {
    gsize start;
    gsize len;
    char *href;
    char *target;
    const ns_node *dom;
} ns_link_range;

typedef enum ns_inline_attr_kind {
    NS_INLINE_BOLD,
    NS_INLINE_ITALIC,
    NS_INLINE_MONOSPACE,
    NS_INLINE_UNDERLINE,
    NS_INLINE_OVERLINE,
    NS_INLINE_STRIKETHROUGH,
    NS_INLINE_INPUT_FIELD,
    NS_INLINE_INPUT_FIELD_FOCUSED,
    NS_INLINE_BUTTON,
    NS_INLINE_CHECKBOX,
    NS_INLINE_CHECKBOX_CHECKED,
    NS_INLINE_RADIO,
    NS_INLINE_RADIO_CHECKED,
    NS_INLINE_PROGRESS,
    NS_INLINE_METER,
    NS_INLINE_FONT_SIZE,
    NS_INLINE_FONT_WEIGHT,
    NS_INLINE_FONT_STRETCH,
    NS_INLINE_FONT_FEATURES,
    NS_INLINE_FONT_VARIATIONS,
    NS_INLINE_COLOR,
    NS_INLINE_FONT_FAMILY,
    NS_INLINE_BG_COLOR,
    NS_INLINE_SUPERSCRIPT,
    NS_INLINE_SUBSCRIPT,
    NS_INLINE_SMALL_CAPS,
    NS_INLINE_CARET,
    NS_INLINE_SELECTION,
    NS_INLINE_ELEMENT,
    NS_INLINE_SPELLCHECK,
} ns_inline_attr_kind;

typedef struct ns_inline_attr {
    ns_inline_attr_kind kind;
    gsize start;
    gsize len;
    double font_size_px;
    int font_weight;
    int font_stretch;
    int font_kerning;
    const char *font_ligatures;
    const char *font_features;
    const char *font_variations;
    double box_w, box_h;
    gboolean native_chrome;
    guint8 r, g, b, a;
    const char *family;
    const ns_node *dom;
    const ns_style *style;
    const char *bg_image_src;
    void *bg_image;
} ns_inline_attr;

typedef struct ns_inline_atomic {
    gsize byte_off;
    struct ns_box *box;
} ns_inline_atomic;

typedef struct ns_box_media {
    char  *image_src;
    void  *image;
    char  *bg_image_src;
    void  *bg_image;
    char  *marker_image_src;
    void  *marker_image;
    GPtrArray *bg_layer_srcs;
    GPtrArray *bg_layer_images;
    char  *video_src;
    char  *video_poster;
    char  *video_audio_src;
    void  *video;
    gboolean declared_image_size;
    gboolean placeholder_image_size;
    gboolean size_independent_of_image;
    gboolean intrinsic_ratio_only;
} ns_box_media;

typedef struct ns_box {
    ns_box_kind kind;
    const ns_node  *dom;
    const ns_style *style;

    double x, y;

    double content_width, content_height;
    double definite_height;
    double paint_top, paint_bottom;
    ns_edges margin, padding, border;

    double scroll_x, scroll_y;
    double scroll_max_x, scroll_max_y;
    gboolean scrolls;

    char *text;

    const ns_style *inline_layout_cache_style;
    double inline_layout_cache_width;
    double inline_layout_cache_height;
    gboolean inline_layout_cache_valid;
    int vertical_wm;
    int text_orient;
    const ns_style *inline_natural_cache_style;
    double inline_natural_cache_width;
    gboolean inline_natural_cache_valid;
    const ns_style *inline_min_cache_style;
    double inline_min_cache_width;
    gboolean inline_min_cache_valid;

    void *paint_layout;

    GArray *links;
    GArray *attrs;
    GArray *inline_atomics;
    GArray *table_col_hints;

    ns_box_media *media;

    int colspan;
    int rowspan;
    int columns;

    struct ns_box *parent;
    struct ns_box *first_child;
    struct ns_box *last_child;
    struct ns_box *next_sibling;
} ns_box;

struct _PangoAttrList;
struct _PangoLayout;
void ns_inline_apply_atomic_shapes(struct _PangoAttrList *list, const ns_box *box);
void ns_inline_layout_set_attrs(struct _PangoLayout *layout,
                                struct _PangoAttrList *list, const ns_box *box);
double ns_text_indent_px(const ns_style *s, double basis);
double ns_control_css_extra_w(const ns_node *dom, const ns_style *s);
double ns_control_css_extra_h(const ns_node *dom, const ns_style *s);

void ns_box_free(ns_box *box);

double ns_box_max_bottom(const ns_box *root, double seed);

void ns_paint_drop_box_cache(ns_box *box);

struct ns_image_cache;
ns_box *ns_layout_build(const ns_node *doc, GHashTable *styles,
                        double viewport_width,
                        const ns_node *focused_input,
                        gsize focused_caret_byte,
                        gsize focused_sel_anchor_byte,
                        struct ns_image_cache *image_cache,
                        const char *base_url);

void ns_layout_set_open_select(const ns_node *select);
void ns_layout_set_datalist_open(gboolean open);
char *ns_vertical_stack_text(const char *text);

void ns_layout_collect_images(const ns_box *root, GPtrArray *out_boxes);
void ns_layout_collect_videos(const ns_box *root, GPtrArray *out_boxes);


gboolean ns_box_tree_has_sticky(const ns_box *root);


const char *ns_box_hit_link(const ns_box *root, double x, double y);
const ns_link_range *ns_box_hit_link_range(const ns_box *root, double x, double y);

const ns_box *ns_box_find_by_id(const ns_box *root, const char *id);
const ns_box *ns_box_find_by_id_or_name(const ns_box *root, const char *frag);

const ns_box *ns_box_hit_test(const ns_box *root, double x, double y);

ns_box *ns_box_hit_scrollable(ns_box *root, double x, double y);
ns_box *ns_box_hit_scrollbar(ns_box *root, double x, double y,
                             double *lx, double *ly);

const ns_node *ns_box_hit_form_dom(const ns_box *root, double x, double y);

const ns_node *ns_box_hit_inline_dom(const ns_box *root, double x, double y);

gboolean ns_box_inline_rect_for_dom(const ns_box *root, const ns_node *target,
                                    double *x, double *y,
                                    double *w, double *h);

char *ns_img_chosen_url(const ns_node *n);

guint ns_box_count_matches(const ns_box *root, const char *needle,
                           gboolean case_sensitive);

const ns_box *ns_box_first_match_below(const ns_box *root,
                                       const char *needle,
                                       double y_threshold,
                                       gboolean case_sensitive);

const ns_box *ns_box_first_match_above(const ns_box *root,
                                       const char *needle,
                                       double y_threshold,
                                       gboolean case_sensitive);

guint ns_box_match_ordinal(const ns_box *root,
                           const char *needle,
                           const ns_box *target,
                           gboolean case_sensitive);

G_END_DECLS

#endif
