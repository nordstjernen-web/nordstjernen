/* Nordstjernen — CSS engine API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_CSS_H
#define ND_CSS_H

#include <glib.h>

#include "dom.h"

G_BEGIN_DECLS

typedef enum nd_css_prop {
    ND_CSS_DISPLAY,
    ND_CSS_COLOR,
    ND_CSS_BACKGROUND_COLOR,
    ND_CSS_FONT_SIZE,
    ND_CSS_FONT_WEIGHT,
    ND_CSS_FONT_STYLE,
    ND_CSS_FONT_FAMILY,
    ND_CSS_TEXT_ALIGN,
    ND_CSS_MARGIN_TOP,
    ND_CSS_MARGIN_RIGHT,
    ND_CSS_MARGIN_BOTTOM,
    ND_CSS_MARGIN_LEFT,
    ND_CSS_PADDING_TOP,
    ND_CSS_PADDING_RIGHT,
    ND_CSS_PADDING_BOTTOM,
    ND_CSS_PADDING_LEFT,
    ND_CSS_BORDER_TOP_WIDTH,
    ND_CSS_BORDER_RIGHT_WIDTH,
    ND_CSS_BORDER_BOTTOM_WIDTH,
    ND_CSS_BORDER_LEFT_WIDTH,
    ND_CSS_BORDER_TOP_COLOR,
    ND_CSS_BORDER_RIGHT_COLOR,
    ND_CSS_BORDER_BOTTOM_COLOR,
    ND_CSS_BORDER_LEFT_COLOR,
    ND_CSS_BORDER_TOP_STYLE,
    ND_CSS_BORDER_RIGHT_STYLE,
    ND_CSS_BORDER_BOTTOM_STYLE,
    ND_CSS_BORDER_LEFT_STYLE,
    ND_CSS_WIDTH,
    ND_CSS_HEIGHT,
    ND_CSS_MAX_WIDTH,
    ND_CSS_MAX_HEIGHT,
    ND_CSS_MIN_WIDTH,
    ND_CSS_MIN_HEIGHT,
    ND_CSS_LINE_HEIGHT,
    ND_CSS_TEXT_DECORATION,
    ND_CSS_POSITION,
    ND_CSS_TOP,
    ND_CSS_RIGHT,
    ND_CSS_BOTTOM,
    ND_CSS_LEFT,
    ND_CSS_Z_INDEX,
    ND_CSS_OPACITY,
    ND_CSS_CURSOR,
    ND_CSS_POINTER_EVENTS,
    ND_CSS_LETTER_SPACING,
    ND_CSS_WORD_SPACING,
    ND_CSS_WHITE_SPACE,
    ND_CSS_BOX_SIZING,
    ND_CSS_TEXT_INDENT,
    ND_CSS_TEXT_TRANSFORM,
    ND_CSS_LIST_STYLE_TYPE,
    ND_CSS_VERTICAL_ALIGN,
    ND_CSS_VISIBILITY,
    ND_CSS_OVERFLOW,
    ND_CSS_FONT_VARIANT,
    ND_CSS_BORDER_RADIUS,
    ND_CSS_BORDER_TOP_LEFT_RADIUS,
    ND_CSS_BORDER_TOP_RIGHT_RADIUS,
    ND_CSS_BORDER_BOTTOM_RIGHT_RADIUS,
    ND_CSS_BORDER_BOTTOM_LEFT_RADIUS,
    ND_CSS_FLEX_DIRECTION,
    ND_CSS_FLEX_WRAP,
    ND_CSS_JUSTIFY_CONTENT,
    ND_CSS_ALIGN_ITEMS,
    ND_CSS_ALIGN_SELF,
    ND_CSS_GAP,
    ND_CSS_ROW_GAP,
    ND_CSS_COLUMN_GAP,
    ND_CSS_FLEX_GROW,
    ND_CSS_FLEX_SHRINK,
    ND_CSS_FLEX_BASIS,
    ND_CSS_ORDER,
    ND_CSS_FLOAT,
    ND_CSS_CLEAR,
    ND_CSS_BOX_SHADOW,
    ND_CSS_OUTLINE_WIDTH,
    ND_CSS_OUTLINE_STYLE,
    ND_CSS_OUTLINE_COLOR,
    ND_CSS_OUTLINE_OFFSET,
    ND_CSS_BACKGROUND_IMAGE,
    ND_CSS_BACKGROUND_REPEAT,
    ND_CSS_BACKGROUND_POSITION_X,
    ND_CSS_BACKGROUND_POSITION_Y,
    ND_CSS_BACKGROUND_SIZE,
    ND_CSS_BACKGROUND_CLIP,
    ND_CSS_SCROLLBAR_WIDTH,
    ND_CSS_SCROLLBAR_COLOR,
    ND_CSS_IMAGE_RENDERING,
    ND_CSS_CONTENT,
    ND_CSS_GRID_TEMPLATE_COLUMNS,
    ND_CSS_GRID_TEMPLATE_ROWS,
    ND_CSS_GRID_TEMPLATE_AREAS,
    ND_CSS_GRID_COLUMN,
    ND_CSS_GRID_ROW,
    ND_CSS_GRID_COLUMN_START,
    ND_CSS_GRID_COLUMN_END,
    ND_CSS_GRID_ROW_START,
    ND_CSS_GRID_ROW_END,
    ND_CSS_GRID_AREA,
    ND_CSS_GRID_AUTO_ROWS,
    ND_CSS_TRANSFORM,
    ND_CSS_TRANSFORM_ORIGIN,
    ND_CSS_TRANSITION,
    ND_CSS_ANIMATION,
    ND_CSS_ASPECT_RATIO,
    ND_CSS_TEXT_SHADOW,
    ND_CSS_OVERFLOW_WRAP,
    ND_CSS_WORD_BREAK,
    ND_CSS_TEXT_OVERFLOW,
    ND_CSS_TEXT_DECORATION_COLOR,
    ND_CSS_TEXT_DECORATION_STYLE,
    ND_CSS_LIST_STYLE_POSITION,
    ND_CSS_COLUMN_COUNT,
    ND_CSS_COLUMN_WIDTH,
    ND_CSS_COLUMN_RULE_WIDTH,
    ND_CSS_COLUMN_RULE_STYLE,
    ND_CSS_COLUMN_RULE_COLOR,
    ND_CSS_FILTER,
    ND_CSS_CLIP_PATH,
    ND_CSS_MIX_BLEND_MODE,
    ND_CSS_ACCENT_COLOR,
    ND_CSS_COUNTER_RESET,
    ND_CSS_COUNTER_INCREMENT,
    ND_CSS_LINE_CLAMP,
    ND_CSS_OBJECT_FIT,
    ND_CSS_OBJECT_POSITION_X,
    ND_CSS_OBJECT_POSITION_Y,
    ND_CSS_MASK_IMAGE,
    ND_CSS_OVERFLOW_X,
    ND_CSS_OVERFLOW_Y,
    ND_CSS_APPEARANCE,
    ND_CSS_TABLE_LAYOUT,
    ND_CSS_CAPTION_SIDE,
    ND_CSS_BORDER_COLLAPSE,
    ND_CSS_BORDER_SPACING,
    ND_CSS_CONTAINER_TYPE,
    ND_CSS_CONTAINER_NAME,
    ND_CSS_CARET_COLOR,
    ND_CSS_TAB_SIZE,
    ND_CSS_JUSTIFY_ITEMS,
    ND_CSS_JUSTIFY_SELF,
    ND_CSS_ALIGN_CONTENT,
    ND_CSS_PROP_COUNT,
} nd_css_prop;

int         nd_css_prop_id(const char *name);

typedef enum nd_css_value_kind {
    ND_CSS_V_KEYWORD,
    ND_CSS_V_LENGTH,
    ND_CSS_V_SIZE,
    ND_CSS_V_COLOR,
    ND_CSS_V_CALC,
    ND_CSS_V_SHADOW,
    ND_CSS_V_GRADIENT,
    ND_CSS_V_TRACKS,
    ND_CSS_V_URL,
    ND_CSS_V_TRANSFORM,
    ND_CSS_V_AREAS,
    ND_CSS_V_ANIM,
} nd_css_value_kind;

typedef enum nd_css_timing_kind {
    ND_CSS_TIMING_LINEAR,
    ND_CSS_TIMING_EASE,
    ND_CSS_TIMING_EASE_IN,
    ND_CSS_TIMING_EASE_OUT,
    ND_CSS_TIMING_EASE_IN_OUT,
    ND_CSS_TIMING_STEPS,
    ND_CSS_TIMING_CUBIC,
} nd_css_timing_kind;

typedef enum nd_css_step_pos {
    ND_CSS_STEP_JUMP_END,
    ND_CSS_STEP_JUMP_START,
    ND_CSS_STEP_JUMP_NONE,
    ND_CSS_STEP_JUMP_BOTH,
} nd_css_step_pos;

typedef struct nd_css_timing {
    nd_css_timing_kind kind;
    int                steps;
    nd_css_step_pos    step_pos;
    double             cb[4];
} nd_css_timing;

typedef enum nd_css_anim_target {
    ND_CSS_ANIM_TARGET_NONE,
    ND_CSS_ANIM_TARGET_ALL,
    ND_CSS_ANIM_TARGET_OPACITY,
    ND_CSS_ANIM_TARGET_TRANSFORM,
    ND_CSS_ANIM_TARGET_COLOR,
    ND_CSS_ANIM_TARGET_BG_COLOR,
} nd_css_anim_target;

typedef enum nd_css_anim_direction {
    ND_CSS_ANIM_DIR_NORMAL,
    ND_CSS_ANIM_DIR_REVERSE,
    ND_CSS_ANIM_DIR_ALTERNATE,
    ND_CSS_ANIM_DIR_ALTERNATE_REVERSE,
} nd_css_anim_direction;

typedef enum nd_css_anim_fill {
    ND_CSS_ANIM_FILL_NONE,
    ND_CSS_ANIM_FILL_FORWARDS,
    ND_CSS_ANIM_FILL_BACKWARDS,
    ND_CSS_ANIM_FILL_BOTH,
} nd_css_anim_fill;

typedef struct nd_css_anim_entry {
    nd_css_anim_target target;
    char         *name;
    double        duration_ms;
    double        delay_ms;
    nd_css_timing timing;
    int           iter_count;
    nd_css_anim_direction direction;
    nd_css_anim_fill      fill;
} nd_css_anim_entry;

#define ND_CSS_ANIM_ENTRIES_MAX 4

typedef struct nd_css_anim_list {
    int n;
    nd_css_anim_entry entries[ND_CSS_ANIM_ENTRIES_MAX];
} nd_css_anim_list;

typedef enum nd_css_transform_op_kind {
    ND_CSS_TFN_TRANSLATE,
    ND_CSS_TFN_ROTATE,
    ND_CSS_TFN_SCALE,
    ND_CSS_TFN_SKEW,
    ND_CSS_TFN_MATRIX,
} nd_css_transform_op_kind;

typedef struct nd_css_transform_op {
    nd_css_transform_op_kind kind;
    double a, b, c, d, e, f;
    gboolean a_is_percent, b_is_percent;
    gboolean e_is_percent, f_is_percent;
} nd_css_transform_op;

#define ND_CSS_TRANSFORM_OPS_MAX 6

typedef struct nd_css_transform {
    int n_ops;
    nd_css_transform_op ops[ND_CSS_TRANSFORM_OPS_MAX];
} nd_css_transform;

typedef enum nd_css_track_kind {
    ND_CSS_TRACK_PX,
    ND_CSS_TRACK_PERCENT,
    ND_CSS_TRACK_FR,
    ND_CSS_TRACK_AUTO,
} nd_css_track_kind;

#define ND_CSS_TRACKS_MAX 24

typedef struct nd_css_track {
    nd_css_track_kind kind;
    double v;
    nd_css_track_kind min_kind;
    double min_v;
    gboolean has_min;
} nd_css_track;

typedef enum nd_css_auto_repeat {
    ND_CSS_AUTO_REPEAT_NONE,
    ND_CSS_AUTO_REPEAT_FIT,
    ND_CSS_AUTO_REPEAT_FILL,
} nd_css_auto_repeat;

typedef struct nd_css_tracks {
    int n;
    nd_css_track tracks[ND_CSS_TRACKS_MAX];
    nd_css_auto_repeat auto_repeat;
    int auto_repeat_start;
    int auto_repeat_count;
    gboolean subgrid;
} nd_css_tracks;

typedef struct nd_css_area_rect {
    char *name;
    int r0, r1;
    int c0, c1;
} nd_css_area_rect;

#define ND_CSS_AREAS_MAX 32

typedef struct nd_css_areas {
    int n_rows;
    int n_cols;
    int n_rects;
    nd_css_area_rect rects[ND_CSS_AREAS_MAX];
} nd_css_areas;

#define ND_CSS_GRADIENT_STOPS_MAX 6

typedef struct nd_css_shadow {
    double x, y, blur, spread;
    guint8 r, g, b, a;
    gboolean inset;
} nd_css_shadow;

#define ND_CSS_SHADOWS_MAX 8

typedef struct nd_css_shadow_list {
    int n;
    nd_css_shadow s[ND_CSS_SHADOWS_MAX];
} nd_css_shadow_list;

typedef struct nd_css_gradient_stop {
    guint8 r, g, b, a;
    double pos;
    gboolean has_pos;
    gboolean pos_is_px;
} nd_css_gradient_stop;

typedef struct nd_css_gradient {
    int angle_deg;
    int n_stops;
    gboolean radial;
    gboolean conic;
    gboolean repeating;
    int from_deg;
    double center_x, center_y;
    gboolean has_center;
    nd_css_gradient_stop stops[ND_CSS_GRADIENT_STOPS_MAX];
} nd_css_gradient;

typedef enum nd_css_unit {
    ND_CSS_UNIT_PX,
    ND_CSS_UNIT_EM,
    ND_CSS_UNIT_REM,
    ND_CSS_UNIT_PERCENT,
    ND_CSS_UNIT_NUMBER,
    ND_CSS_UNIT_VW,
    ND_CSS_UNIT_VH,
    ND_CSS_UNIT_VMIN,
    ND_CSS_UNIT_VMAX,
    ND_CSS_UNIT_CQW,
    ND_CSS_UNIT_CQH,
    ND_CSS_UNIT_CQMIN,
    ND_CSS_UNIT_CQMAX,
} nd_css_unit;

void     nd_css_set_viewport(double vw_px, double vh_px);
double   nd_css_viewport_w(void);
double   nd_css_viewport_h(void);
double   nd_css_container_w(void);
double   nd_css_container_h(void);

typedef enum nd_css_color_scheme {
    ND_CSS_COLOR_SCHEME_LIGHT,
    ND_CSS_COLOR_SCHEME_DARK,
} nd_css_color_scheme;

typedef enum nd_css_reduced_motion {
    ND_CSS_REDUCED_MOTION_NO_PREFERENCE,
    ND_CSS_REDUCED_MOTION_REDUCE,
} nd_css_reduced_motion;

void                 nd_css_set_color_scheme(nd_css_color_scheme s);
void                 nd_css_set_reduced_motion(nd_css_reduced_motion m);
nd_css_reduced_motion nd_css_get_reduced_motion(void);

typedef struct nd_css_value {
    nd_css_value_kind kind;
    int ref;
    union {
        char *keyword;
        struct { double v; nd_css_unit unit; } length;
        struct { double w, h; nd_css_unit w_unit, h_unit; gboolean w_auto, h_auto; } size;
        struct { guint8 r, g, b, a; } color;
        struct { double pct; double px; double em; double rem; } calc;
        nd_css_shadow_list shadow;
        nd_css_gradient  gradient;
        nd_css_tracks    tracks;
        char            *url;
        nd_css_transform transform;
        nd_css_areas     areas;
        nd_css_anim_list anim;
    } u;
} nd_css_value;

double   nd_css_length_or(const nd_css_value *v, double fallback);
gboolean nd_css_keyword_is(const nd_css_value *v, const char *kw);
char    *nd_css_font_family_for_pango(const char *css_family);
void     nd_css_set_font_available_cb(gboolean (*cb)(const char *family));
int      nd_css_font_weight_number(const nd_css_value *v, int fallback);

typedef enum nd_css_attr_op {
    ND_CSS_ATTR_PRESENT,
    ND_CSS_ATTR_EQ,
    ND_CSS_ATTR_PREFIX,
    ND_CSS_ATTR_SUFFIX,
    ND_CSS_ATTR_SUBSTR,
    ND_CSS_ATTR_WORD,
    ND_CSS_ATTR_HYPHEN,
} nd_css_attr_op;

typedef struct nd_css_attr_pred {
    char *name;
    nd_css_attr_op op;
    char *value;
    gboolean ci;
} nd_css_attr_pred;

typedef enum nd_css_pseudo {
    ND_CSS_PC_FIRST_CHILD,
    ND_CSS_PC_LAST_CHILD,
    ND_CSS_PC_ONLY_CHILD,
    ND_CSS_PC_ONLY_OF_TYPE,
    ND_CSS_PC_FIRST_OF_TYPE,
    ND_CSS_PC_LAST_OF_TYPE,
    ND_CSS_PC_EMPTY,
    ND_CSS_PC_ROOT,
    ND_CSS_PC_CHECKED,
    ND_CSS_PC_DISABLED,
    ND_CSS_PC_ENABLED,
    ND_CSS_PC_REQUIRED,
    ND_CSS_PC_OPTIONAL,
    ND_CSS_PC_VALID,
    ND_CSS_PC_INVALID,
    ND_CSS_PC_NTH_CHILD,
    ND_CSS_PC_NTH_LAST_CHILD,
    ND_CSS_PC_NTH_OF_TYPE,
    ND_CSS_PC_NTH_LAST_OF_TYPE,
    ND_CSS_PC_LINK,
    ND_CSS_PC_VISITED,
    ND_CSS_PC_ANY_LINK,
    ND_CSS_PC_HOVER,
    ND_CSS_PC_ACTIVE,
    ND_CSS_PC_FOCUS,
    ND_CSS_PC_FOCUS_WITHIN,
    ND_CSS_PC_TARGET,
    ND_CSS_PC_DEFINED,
    ND_CSS_PC_SCOPE,
    ND_CSS_PC_PLACEHOLDER_SHOWN,
    ND_CSS_PC_READ_ONLY,
    ND_CSS_PC_READ_WRITE,
    ND_CSS_PC_LANG,
    ND_CSS_PC_DIR,
    ND_CSS_PC_OPEN,
    ND_CSS_PC_POPOVER_OPEN,
} nd_css_pseudo;

typedef struct nd_css_pseudo_pred {
    nd_css_pseudo kind;
    int a, b;
    char *arg;
    GPtrArray *of_group;
} nd_css_pseudo_pred;

typedef struct nd_css_simple {
    char *type;
    char *id;
    GPtrArray *classes;
    GArray    *attrs;
    GArray    *pseudos;
    GPtrArray *matches_any;
    GPtrArray *matches_none;
    GPtrArray *has_groups;
    gboolean   never_match;
} nd_css_simple;

typedef enum nd_css_comb {
    ND_CSS_COMB_NONE,
    ND_CSS_COMB_DESCENDANT,
    ND_CSS_COMB_CHILD,
    ND_CSS_COMB_ADJACENT,
    ND_CSS_COMB_SIBLING,
} nd_css_comb;

typedef enum nd_css_pseudo_element {
    ND_CSS_PE_NONE,
    ND_CSS_PE_BEFORE,
    ND_CSS_PE_AFTER,
    ND_CSS_PE_FIRST_LETTER,
    ND_CSS_PE_FIRST_LINE,
    ND_CSS_PE_SELECTION,
    ND_CSS_PE_MARKER,
    ND_CSS_PE_BACKDROP,
    ND_CSS_PE_PLACEHOLDER,
} nd_css_pseudo_element;

typedef struct nd_css_selector {

    GPtrArray *compounds;
    GArray    *combinators;

    nd_css_pseudo_element pseudo_element;

    int spec_a, spec_b, spec_c;
} nd_css_selector;

GPtrArray *nd_css_parse_selector_list(const char *text);

const nd_node *nd_css_set_match_scope(const nd_node *scope);

void nd_collect_inline_stylesheets(nd_node *doc, GPtrArray *out);
char *nd_css_assign_iframe_scope(nd_node *node);
char *nd_css_scope_css(const char *css, gssize len, const char *scope_id);
gboolean   nd_css_selector_matches(const nd_css_selector *sel, const nd_node *el);

gboolean   nd_css_media_query_matches(const char *query);

double     nd_css_sizes_resolve(const char *sizes);

void       nd_css_register_defined_element(const char *tag);
void       nd_css_clear_defined_elements(void);

char *nd_inline_style_get(const char *style_text, const char *prop_name);
char *nd_inline_style_set(const char *style_text, const char *prop_name, const char *value);

typedef struct nd_css_decl {
    nd_css_prop prop;
    nd_css_value *value;
    gboolean important;
} nd_css_decl;

typedef struct nd_css_pending_decl {
    char     *pname;
    char     *raw_vtext;
    gboolean  important;
} nd_css_pending_decl;

typedef struct nd_css_rule {
    GPtrArray  *selectors;
    GArray     *decls;
    GHashTable *vars;
    GHashTable *var_important;
    GArray     *pending;
    char       *layer_name;
    char       *container_condition;
    GPtrArray  *scopes;
    int         source_order;
} nd_css_rule;

typedef struct nd_css_import {
    char *url;
    char *layer_name;
    char *media;
} nd_css_import;

typedef struct nd_css_font_face {
    char *family;
    char *src_url;
} nd_css_font_face;

typedef struct nd_css_property_rule {
    char *name;
    char *initial_value;
    gboolean inherits;
    gboolean has_initial;
} nd_css_property_rule;

typedef struct nd_css_keyframe_stop {
    double pct;
    double opacity;
    gboolean has_opacity;
    nd_css_transform transform;
    gboolean has_transform;
    guint8 color[4];
    gboolean has_color;
    guint8 bg_color[4];
    gboolean has_bg_color;
} nd_css_keyframe_stop;

typedef struct nd_css_keyframes {
    char *name;
    int n_stops;
    nd_css_keyframe_stop *stops;
} nd_css_keyframes;

struct nd_css_rule_index;

typedef struct nd_css_stylesheet {
    GPtrArray *rules;
    GArray    *imports;
    GPtrArray *layer_names;
    GHashTable *layers;
    GArray    *font_faces;
    GArray    *keyframes;
    GArray    *property_rules;
    gboolean   has_container_rules;
    guint      pseudo_mask;
    struct nd_css_rule_index *index;
} nd_css_stylesheet;

gboolean nd_css_stylesheet_has_container_rules(const nd_css_stylesheet *sh);

#define ND_CSS_IMPORT_MAX_DEPTH 8

nd_css_stylesheet *nd_css_stylesheet_parse(const char *text, gssize len);
void               nd_css_stylesheet_resolve_urls(nd_css_stylesheet *s,
                                                  const char *base_url);
void               nd_css_stylesheet_free(nd_css_stylesheet *s);
void               nd_css_stylesheet_force_layer(nd_css_stylesheet *s,
                                                 const char *layer_name);

typedef struct nd_style {
    nd_css_value *values[ND_CSS_PROP_COUNT];
    struct nd_style *before;
    struct nd_style *after;
    struct nd_style *first_letter;
    struct nd_style *first_line;
    struct nd_style *placeholder;
    struct nd_style *selection;
    struct nd_style *marker;
    struct nd_style *backdrop;
    GHashTable      *vars;
} nd_style;

GHashTable *nd_css_compute(nd_node                 *doc,
                           const nd_css_stylesheet *const *author_sheets,
                           gsize                     n_sheets);

void nd_css_set_container_map(GHashTable *map);
void nd_css_set_container_dims(double inline_px, double block_px);
GHashTable *nd_css_container_map_new(void);
void nd_css_container_map_add(GHashTable *map, const void *node,
                              const char *type_kw, const char *name_kw,
                              double w, double h);

void nd_css_set_target_fragment(const char *fragment);

const nd_node *nd_css_set_focus_node(const nd_node *node);

const char *nd_style_keyword(const nd_style *s, nd_css_prop p);

int nd_css_used_column_count(const nd_style *s, double avail_w,
                             double *out_gap);

char *nd_css_value_serialize(const nd_css_value *v);

gboolean nd_css_parse_color(const char *s, guint8 *r, guint8 *g, guint8 *b,
                            guint8 *a);

G_END_DECLS

#endif
