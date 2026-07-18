/* Nordstjernen — CSS engine API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_CSS_H
#define NS_CSS_H

#include <glib.h>

#include "dom.h"
#include "mat4.h"

G_BEGIN_DECLS

typedef enum ns_css_prop {
    NS_CSS_DISPLAY,
    NS_CSS_COLOR,
    NS_CSS_BACKGROUND_COLOR,
    NS_CSS_FONT_SIZE,
    NS_CSS_FONT_WEIGHT,
    NS_CSS_FONT_STYLE,
    NS_CSS_FONT_STRETCH,
    NS_CSS_FONT_KERNING,
    NS_CSS_FONT_VARIANT_LIGATURES,
    NS_CSS_FONT_FEATURE_SETTINGS,
    NS_CSS_FONT_VARIATION_SETTINGS,
    NS_CSS_FONT_FAMILY,
    NS_CSS_TEXT_ALIGN,
    NS_CSS_MARGIN_TOP,
    NS_CSS_MARGIN_RIGHT,
    NS_CSS_MARGIN_BOTTOM,
    NS_CSS_MARGIN_LEFT,
    NS_CSS_PADDING_TOP,
    NS_CSS_PADDING_RIGHT,
    NS_CSS_PADDING_BOTTOM,
    NS_CSS_PADDING_LEFT,
    NS_CSS_BORDER_TOP_WIDTH,
    NS_CSS_BORDER_RIGHT_WIDTH,
    NS_CSS_BORDER_BOTTOM_WIDTH,
    NS_CSS_BORDER_LEFT_WIDTH,
    NS_CSS_BORDER_TOP_COLOR,
    NS_CSS_BORDER_RIGHT_COLOR,
    NS_CSS_BORDER_BOTTOM_COLOR,
    NS_CSS_BORDER_LEFT_COLOR,
    NS_CSS_BORDER_TOP_STYLE,
    NS_CSS_BORDER_RIGHT_STYLE,
    NS_CSS_BORDER_BOTTOM_STYLE,
    NS_CSS_BORDER_LEFT_STYLE,
    NS_CSS_WIDTH,
    NS_CSS_HEIGHT,
    NS_CSS_MAX_WIDTH,
    NS_CSS_MAX_HEIGHT,
    NS_CSS_MIN_WIDTH,
    NS_CSS_MIN_HEIGHT,
    NS_CSS_LINE_HEIGHT,
    NS_CSS_TEXT_DECORATION,
    NS_CSS_POSITION,
    NS_CSS_TOP,
    NS_CSS_RIGHT,
    NS_CSS_BOTTOM,
    NS_CSS_LEFT,
    NS_CSS_Z_INDEX,
    NS_CSS_OPACITY,
    NS_CSS_CURSOR,
    NS_CSS_POINTER_EVENTS,
    NS_CSS_LETTER_SPACING,
    NS_CSS_WORD_SPACING,
    NS_CSS_WHITE_SPACE,
    NS_CSS_BOX_SIZING,
    NS_CSS_TEXT_INDENT,
    NS_CSS_TEXT_TRANSFORM,
    NS_CSS_LIST_STYLE_TYPE,
    NS_CSS_VERTICAL_ALIGN,
    NS_CSS_VISIBILITY,
    NS_CSS_OVERFLOW,
    NS_CSS_FONT_VARIANT,
    NS_CSS_BORDER_RADIUS,
    NS_CSS_BORDER_TOP_LEFT_RADIUS,
    NS_CSS_BORDER_TOP_RIGHT_RADIUS,
    NS_CSS_BORDER_BOTTOM_RIGHT_RADIUS,
    NS_CSS_BORDER_BOTTOM_LEFT_RADIUS,
    NS_CSS_FLEX_DIRECTION,
    NS_CSS_FLEX_WRAP,
    NS_CSS_JUSTIFY_CONTENT,
    NS_CSS_ALIGN_ITEMS,
    NS_CSS_ALIGN_SELF,
    NS_CSS_GAP,
    NS_CSS_ROW_GAP,
    NS_CSS_COLUMN_GAP,
    NS_CSS_FLEX_GROW,
    NS_CSS_FLEX_SHRINK,
    NS_CSS_FLEX_BASIS,
    NS_CSS_ORDER,
    NS_CSS_FLOAT,
    NS_CSS_CLEAR,
    NS_CSS_BOX_SHADOW,
    NS_CSS_OUTLINE_WIDTH,
    NS_CSS_OUTLINE_STYLE,
    NS_CSS_OUTLINE_COLOR,
    NS_CSS_OUTLINE_OFFSET,
    NS_CSS_BACKGROUND_IMAGE,
    NS_CSS_BACKGROUND_REPEAT,
    NS_CSS_BACKGROUND_POSITION_X,
    NS_CSS_BACKGROUND_POSITION_Y,
    NS_CSS_BACKGROUND_SIZE,
    NS_CSS_BACKGROUND_CLIP,
    NS_CSS_BACKGROUND_ORIGIN,
    NS_CSS_SCROLLBAR_WIDTH,
    NS_CSS_SCROLLBAR_COLOR,
    NS_CSS_IMAGE_RENDERING,
    NS_CSS_CONTENT,
    NS_CSS_GRID_TEMPLATE_COLUMNS,
    NS_CSS_GRID_TEMPLATE_ROWS,
    NS_CSS_GRID_TEMPLATE_AREAS,
    NS_CSS_GRID_COLUMN,
    NS_CSS_GRID_ROW,
    NS_CSS_GRID_COLUMN_START,
    NS_CSS_GRID_COLUMN_END,
    NS_CSS_GRID_ROW_START,
    NS_CSS_GRID_ROW_END,
    NS_CSS_GRID_AREA,
    NS_CSS_GRID_AUTO_ROWS,
    NS_CSS_GRID_AUTO_COLUMNS,
    NS_CSS_GRID_AUTO_FLOW,
    NS_CSS_TRANSFORM,
    NS_CSS_TRANSFORM_ORIGIN,
    NS_CSS_TRANSITION,
    NS_CSS_ANIMATION,
    NS_CSS_ASPECT_RATIO,
    NS_CSS_TEXT_SHADOW,
    NS_CSS_OVERFLOW_WRAP,
    NS_CSS_WORD_BREAK,
    NS_CSS_HYPHENS,
    NS_CSS_TEXT_OVERFLOW,
    NS_CSS_TEXT_DECORATION_COLOR,
    NS_CSS_TEXT_DECORATION_STYLE,
    NS_CSS_LIST_STYLE_POSITION,
    NS_CSS_LIST_STYLE_IMAGE,
    NS_CSS_USER_SELECT,
    NS_CSS_QUOTES,
    NS_CSS_COLUMN_COUNT,
    NS_CSS_COLUMN_WIDTH,
    NS_CSS_COLUMN_RULE_WIDTH,
    NS_CSS_COLUMN_RULE_STYLE,
    NS_CSS_COLUMN_RULE_COLOR,
    NS_CSS_FILTER,
    NS_CSS_CLIP_PATH,
    NS_CSS_MIX_BLEND_MODE,
    NS_CSS_ACCENT_COLOR,
    NS_CSS_COUNTER_RESET,
    NS_CSS_COUNTER_INCREMENT,
    NS_CSS_LINE_CLAMP,
    NS_CSS_OBJECT_FIT,
    NS_CSS_OBJECT_POSITION_X,
    NS_CSS_OBJECT_POSITION_Y,
    NS_CSS_MASK_IMAGE,
    NS_CSS_OVERFLOW_X,
    NS_CSS_OVERFLOW_Y,
    NS_CSS_APPEARANCE,
    NS_CSS_TABLE_LAYOUT,
    NS_CSS_CAPTION_SIDE,
    NS_CSS_BORDER_COLLAPSE,
    NS_CSS_BORDER_SPACING,
    NS_CSS_CONTAINER_TYPE,
    NS_CSS_CONTAINER_NAME,
    NS_CSS_CARET_COLOR,
    NS_CSS_TAB_SIZE,
    NS_CSS_JUSTIFY_ITEMS,
    NS_CSS_JUSTIFY_SELF,
    NS_CSS_ALIGN_CONTENT,
    NS_CSS_DIRECTION,
    NS_CSS_UNICODE_BIDI,
    NS_CSS_TRANSLATE,
    NS_CSS_ROTATE,
    NS_CSS_SCALE,
    NS_CSS_PERSPECTIVE,
    NS_CSS_PERSPECTIVE_ORIGIN,
    NS_CSS_TRANSFORM_STYLE,
    NS_CSS_BACKFACE_VISIBILITY,
    NS_CSS_ANIMATION_PLAY_STATE,
    NS_CSS_CLIP,
    NS_CSS_CONTENT_VISIBILITY,
    NS_CSS_WRITING_MODE,
    NS_CSS_TEXT_ORIENTATION,
    NS_CSS_TRANSITION_DELAY,
    NS_CSS_TRANSITION_DURATION,
    NS_CSS_ANIMATION_DELAY,
    NS_CSS_ANIMATION_DURATION,
    NS_CSS_ORPHANS,
    NS_CSS_WIDOWS,
    NS_CSS_MAX_LINES,
    NS_CSS_HYPHENATE_LIMIT_LINES,
    NS_CSS_COLUMN_SPAN,
    NS_CSS_PROP_COUNT,
} ns_css_prop;

int         ns_css_prop_id(const char *name);
gboolean    ns_css_declaration_valid(int prop, const char *text);

typedef enum ns_css_value_kind {
    NS_CSS_V_KEYWORD,
    NS_CSS_V_LENGTH,
    NS_CSS_V_SIZE,
    NS_CSS_V_COLOR,
    NS_CSS_V_CALC,
    NS_CSS_V_SHADOW,
    NS_CSS_V_GRADIENT,
    NS_CSS_V_TRACKS,
    NS_CSS_V_URL,
    NS_CSS_V_TRANSFORM,
    NS_CSS_V_AREAS,
    NS_CSS_V_ANIM,
    NS_CSS_V_RECT,
} ns_css_value_kind;

typedef enum ns_css_timing_kind {
    NS_CSS_TIMING_LINEAR,
    NS_CSS_TIMING_EASE,
    NS_CSS_TIMING_EASE_IN,
    NS_CSS_TIMING_EASE_OUT,
    NS_CSS_TIMING_EASE_IN_OUT,
    NS_CSS_TIMING_STEPS,
    NS_CSS_TIMING_CUBIC,
} ns_css_timing_kind;

typedef enum ns_css_step_pos {
    NS_CSS_STEP_JUMP_END,
    NS_CSS_STEP_JUMP_START,
    NS_CSS_STEP_JUMP_NONE,
    NS_CSS_STEP_JUMP_BOTH,
} ns_css_step_pos;

typedef struct ns_css_timing {
    ns_css_timing_kind kind;
    int                steps;
    ns_css_step_pos    step_pos;
    double             cb[4];
} ns_css_timing;

typedef enum ns_css_anim_target {
    NS_CSS_ANIM_TARGET_NONE,
    NS_CSS_ANIM_TARGET_ALL,
    NS_CSS_ANIM_TARGET_OPACITY,
    NS_CSS_ANIM_TARGET_TRANSFORM,
    NS_CSS_ANIM_TARGET_COLOR,
    NS_CSS_ANIM_TARGET_BG_COLOR,
    NS_CSS_ANIM_TARGET_OTHER,
} ns_css_anim_target;

typedef enum ns_css_anim_direction {
    NS_CSS_ANIM_DIR_NORMAL,
    NS_CSS_ANIM_DIR_REVERSE,
    NS_CSS_ANIM_DIR_ALTERNATE,
    NS_CSS_ANIM_DIR_ALTERNATE_REVERSE,
} ns_css_anim_direction;

typedef enum ns_css_anim_fill {
    NS_CSS_ANIM_FILL_NONE,
    NS_CSS_ANIM_FILL_FORWARDS,
    NS_CSS_ANIM_FILL_BACKWARDS,
    NS_CSS_ANIM_FILL_BOTH,
} ns_css_anim_fill;

typedef struct ns_css_anim_entry {
    ns_css_anim_target target;
    char         *name;
    double        duration_ms;
    double        delay_ms;
    ns_css_timing timing;
    int           iter_count;
    ns_css_anim_direction direction;
    ns_css_anim_fill      fill;
    gboolean      paused;
} ns_css_anim_entry;

#define NS_CSS_ANIM_ENTRIES_MAX 4

typedef struct ns_css_anim_list {
    int n;
    ns_css_anim_entry entries[NS_CSS_ANIM_ENTRIES_MAX];
} ns_css_anim_list;

typedef enum ns_css_transform_op_kind {
    NS_CSS_TFN_TRANSLATE,
    NS_CSS_TFN_ROTATE,
    NS_CSS_TFN_SCALE,
    NS_CSS_TFN_SKEW,
    NS_CSS_TFN_MATRIX,
    NS_CSS_TFN_MATRIX3D,
    NS_CSS_TFN_ROTATE3D,
    NS_CSS_TFN_PERSPECTIVE,
} ns_css_transform_op_kind;

typedef struct ns_css_transform_op {
    ns_css_transform_op_kind kind;
    double a, b, c, d, e, f;
    double m3d[16];
    gboolean a_is_percent, b_is_percent;
    gboolean e_is_percent, f_is_percent;
} ns_css_transform_op;

#define NS_CSS_TRANSFORM_OPS_MAX 8

typedef struct ns_css_transform {
    int n_ops;
    ns_css_transform_op ops[NS_CSS_TRANSFORM_OPS_MAX];
} ns_css_transform;

gboolean ns_css_transform_is_3d(const ns_css_transform *tf);

typedef enum ns_css_track_kind {
    NS_CSS_TRACK_PX,
    NS_CSS_TRACK_PERCENT,
    NS_CSS_TRACK_FR,
    NS_CSS_TRACK_AUTO,
} ns_css_track_kind;

#define NS_CSS_TRACKS_MAX 24

typedef struct ns_css_track {
    ns_css_track_kind kind;
    double v;
    ns_css_track_kind min_kind;
    double min_v;
    gboolean has_min;
} ns_css_track;

typedef enum ns_css_auto_repeat {
    NS_CSS_AUTO_REPEAT_NONE,
    NS_CSS_AUTO_REPEAT_FIT,
    NS_CSS_AUTO_REPEAT_FILL,
} ns_css_auto_repeat;

typedef struct ns_css_tracks {
    int n;
    ns_css_track tracks[NS_CSS_TRACKS_MAX];
    ns_css_auto_repeat auto_repeat;
    int auto_repeat_start;
    int auto_repeat_count;
    gboolean subgrid;
} ns_css_tracks;

typedef struct ns_css_area_rect {
    char *name;
    int r0, r1;
    int c0, c1;
} ns_css_area_rect;

#define NS_CSS_AREAS_MAX 32

typedef struct ns_css_areas {
    int n_rows;
    int n_cols;
    int n_rects;
    ns_css_area_rect rects[NS_CSS_AREAS_MAX];
} ns_css_areas;

#define NS_CSS_GRADIENT_STOPS_MAX 6

typedef struct ns_css_shadow {
    double x, y, blur, spread;
    guint8 r, g, b, a;
    gboolean inset;
} ns_css_shadow;

#define NS_CSS_SHADOWS_MAX 8

typedef struct ns_css_shadow_list {
    int n;
    ns_css_shadow s[NS_CSS_SHADOWS_MAX];
} ns_css_shadow_list;

typedef struct ns_css_gradient_stop {
    guint8 r, g, b, a;
    double pos;
    gboolean has_pos;
    gboolean pos_is_px;
} ns_css_gradient_stop;

typedef struct ns_css_gradient {
    int angle_deg;
    int n_stops;
    gboolean radial;
    gboolean conic;
    gboolean repeating;
    int from_deg;
    double center_x, center_y;
    gboolean has_center;
    ns_css_gradient_stop stops[NS_CSS_GRADIENT_STOPS_MAX];
} ns_css_gradient;

typedef enum ns_css_unit {
    NS_CSS_UNIT_PX,
    NS_CSS_UNIT_EM,
    NS_CSS_UNIT_REM,
    NS_CSS_UNIT_PERCENT,
    NS_CSS_UNIT_NUMBER,
    NS_CSS_UNIT_VW,
    NS_CSS_UNIT_VH,
    NS_CSS_UNIT_VMIN,
    NS_CSS_UNIT_VMAX,
    NS_CSS_UNIT_CQW,
    NS_CSS_UNIT_CQH,
    NS_CSS_UNIT_CQMIN,
    NS_CSS_UNIT_CQMAX,
    NS_CSS_UNIT_EX,
    NS_CSS_UNIT_CH,
    NS_CSS_UNIT_CAP,
    NS_CSS_UNIT_IC,
} ns_css_unit;

void     ns_css_set_viewport(double vw_px, double vh_px);
double   ns_css_viewport_w(void);
double   ns_css_viewport_h(void);
double   ns_css_container_w(void);
double   ns_css_container_h(void);

typedef enum ns_css_color_scheme {
    NS_CSS_COLOR_SCHEME_LIGHT,
    NS_CSS_COLOR_SCHEME_DARK,
} ns_css_color_scheme;

typedef enum ns_css_reduced_motion {
    NS_CSS_REDUCED_MOTION_NO_PREFERENCE,
    NS_CSS_REDUCED_MOTION_REDUCE,
} ns_css_reduced_motion;

ns_css_reduced_motion ns_css_get_reduced_motion(void);

typedef struct ns_css_value {
    ns_css_value_kind kind;
    int ref;
    union {
        char *keyword;
        struct { double v; ns_css_unit unit; } length;
        struct { double w, h; ns_css_unit w_unit, h_unit; gboolean w_auto, h_auto; } size;
        struct { guint8 r, g, b, a; } color;
        struct { double pct; double px; double em; double rem; } calc;
        ns_css_shadow_list shadow;
        ns_css_gradient  gradient;
        ns_css_tracks    tracks;
        char            *url;
        ns_css_transform transform;
        ns_css_areas     areas;
        ns_css_anim_list anim;
        struct { double v[4]; ns_css_unit unit[4]; gboolean is_auto[4]; } rect;
    } u;
    struct ns_css_value *next_layer;
} ns_css_value;

int                 ns_css_font_stretch_rank(const ns_css_value *v);
const ns_css_value *ns_css_value_layer(const ns_css_value *head, int index);
int                 ns_css_value_layer_count(const ns_css_value *head);

double   ns_css_length_or(const ns_css_value *v, double fallback);
gboolean ns_css_keyword_is(const ns_css_value *v, const char *kw);
char    *ns_css_font_family_for_pango(const char *css_family);
void     ns_css_set_font_available_cb(gboolean (*cb)(const char *family));
int      ns_css_font_weight_number(const ns_css_value *v, int fallback);

typedef struct ns_css_font_metrics {
    double ex_px;
    double ch_px;
    double cap_px;
    double ic_px;
} ns_css_font_metrics;

void ns_css_set_font_metrics_cb(
    void (*cb)(const char *family, double size_px, int weight,
               gboolean italic, ns_css_font_metrics *out));

typedef enum ns_css_attr_op {
    NS_CSS_ATTR_PRESENT,
    NS_CSS_ATTR_EQ,
    NS_CSS_ATTR_PREFIX,
    NS_CSS_ATTR_SUFFIX,
    NS_CSS_ATTR_SUBSTR,
    NS_CSS_ATTR_WORD,
    NS_CSS_ATTR_HYPHEN,
} ns_css_attr_op;

typedef struct ns_css_attr_pred {
    char *name;
    ns_css_attr_op op;
    char *value;
    gboolean case_insensitive;
    gboolean case_sensitive;
    guint64 name_bit;
} ns_css_attr_pred;

typedef enum ns_css_pseudo {
    NS_CSS_PC_FIRST_CHILD,
    NS_CSS_PC_LAST_CHILD,
    NS_CSS_PC_ONLY_CHILD,
    NS_CSS_PC_ONLY_OF_TYPE,
    NS_CSS_PC_FIRST_OF_TYPE,
    NS_CSS_PC_LAST_OF_TYPE,
    NS_CSS_PC_EMPTY,
    NS_CSS_PC_ROOT,
    NS_CSS_PC_CHECKED,
    NS_CSS_PC_DISABLED,
    NS_CSS_PC_ENABLED,
    NS_CSS_PC_REQUIRED,
    NS_CSS_PC_OPTIONAL,
    NS_CSS_PC_VALID,
    NS_CSS_PC_INVALID,
    NS_CSS_PC_IN_RANGE,
    NS_CSS_PC_OUT_OF_RANGE,
    NS_CSS_PC_DEFAULT,
    NS_CSS_PC_INDETERMINATE,
    NS_CSS_PC_NTH_CHILD,
    NS_CSS_PC_NTH_LAST_CHILD,
    NS_CSS_PC_NTH_OF_TYPE,
    NS_CSS_PC_NTH_LAST_OF_TYPE,
    NS_CSS_PC_LINK,
    NS_CSS_PC_VISITED,
    NS_CSS_PC_ANY_LINK,
    NS_CSS_PC_HOVER,
    NS_CSS_PC_ACTIVE,
    NS_CSS_PC_FOCUS,
    NS_CSS_PC_FOCUS_WITHIN,
    NS_CSS_PC_TARGET,
    NS_CSS_PC_TARGET_WITHIN,
    NS_CSS_PC_DEFINED,
    NS_CSS_PC_SCOPE,
    NS_CSS_PC_PLACEHOLDER_SHOWN,
    NS_CSS_PC_READ_ONLY,
    NS_CSS_PC_READ_WRITE,
    NS_CSS_PC_BLANK,
    NS_CSS_PC_LANG,
    NS_CSS_PC_DIR,
    NS_CSS_PC_OPEN,
    NS_CSS_PC_POPOVER_OPEN,
    NS_CSS_PC_MODAL,
    NS_CSS_PC_HEADING,
} ns_css_pseudo;

typedef struct ns_css_pseudo_pred {
    ns_css_pseudo kind;
    int a, b;
    char *arg;
    GPtrArray *of_group;
} ns_css_pseudo_pred;

typedef struct ns_css_simple {
    char *type;
    char *id;
    GPtrArray *classes;
    GArray    *class_lens;
    GArray    *attrs;
    GArray    *pseudos;
    GPtrArray *matches_any;
    GPtrArray *matches_none;
    GPtrArray *has_groups;
    gboolean   never_match;
    gboolean   ns_none;
} ns_css_simple;

typedef enum ns_css_comb {
    NS_CSS_COMB_NONE,
    NS_CSS_COMB_DESCENDANT,
    NS_CSS_COMB_CHILD,
    NS_CSS_COMB_ADJACENT,
    NS_CSS_COMB_SIBLING,
} ns_css_comb;

typedef enum ns_css_pseudo_element {
    NS_CSS_PE_NONE,
    NS_CSS_PE_BEFORE,
    NS_CSS_PE_AFTER,
    NS_CSS_PE_FIRST_LETTER,
    NS_CSS_PE_FIRST_LINE,
    NS_CSS_PE_SELECTION,
    NS_CSS_PE_MARKER,
    NS_CSS_PE_BACKDROP,
    NS_CSS_PE_PLACEHOLDER,
} ns_css_pseudo_element;

typedef struct ns_css_selector {

    GPtrArray *compounds;
    GArray    *combinators;

    ns_css_pseudo_element pseudo_element;

    int spec_a, spec_b, spec_c;
} ns_css_selector;

GPtrArray *ns_css_parse_selector_list(const char *text);
GPtrArray *ns_css_parse_selector_list_checked(const char *text,
                                              gboolean *out_valid);

gboolean   ns_css_supports_selector(const char *text);

const ns_node *ns_css_set_match_scope(const ns_node *scope);

gboolean   ns_css_selector_matches(const ns_css_selector *sel, const ns_node *el);
const char *ns_css_node_dir(const ns_node *el);

gboolean   ns_css_media_query_matches(const char *query);

double     ns_css_sizes_resolve(const char *sizes);

void       ns_css_register_defined_element(const char *tag);
void       ns_css_clear_defined_elements(void);

char *ns_inline_style_get(const char *style_text, const char *prop_name);
char *ns_inline_style_set(const char *style_text, const char *prop_name, const char *value);
gboolean ns_inline_value_strip_important(char *value);

typedef struct ns_css_decl {
    ns_css_prop prop;
    ns_css_value *value;
    gboolean important;
} ns_css_decl;

typedef struct ns_css_pending_decl {
    char     *pname;
    char     *raw_vtext;
    gboolean  important;
} ns_css_pending_decl;

typedef struct ns_css_rule {
    GPtrArray  *selectors;
    GArray     *decls;
    GHashTable *vars;
    GHashTable *var_important;
    GArray     *pending;
    char       *layer_name;
    char       *container_condition;
    GPtrArray  *scopes;
    int         source_order;
    guint       pe_mask;
} ns_css_rule;

typedef struct ns_css_import {
    char *url;
    char *layer_name;
    char *media;
} ns_css_import;

typedef struct ns_css_font_face {
    char *family;
    char *src_url;
} ns_css_font_face;

typedef struct ns_css_property_rule {
    char *name;
    char *initial_value;
    gboolean inherits;
    gboolean has_initial;
} ns_css_property_rule;

typedef struct ns_css_keyframe_stop {
    double pct;
    double opacity;
    gboolean has_opacity;
    ns_css_transform transform;
    gboolean has_transform;
    guint8 color[4];
    gboolean has_color;
    guint8 bg_color[4];
    gboolean has_bg_color;
    char *raw_props;
} ns_css_keyframe_stop;

typedef struct ns_css_keyframes {
    char *name;
    int n_stops;
    ns_css_keyframe_stop *stops;
} ns_css_keyframes;

struct ns_css_rule_index;

typedef struct ns_css_stylesheet {
    GPtrArray *rules;
    GArray    *imports;
    GPtrArray *layer_names;
    GHashTable *layers;
    GArray    *font_faces;
    GArray    *keyframes;
    GArray    *property_rules;
    gboolean   has_container_rules;
    gboolean   has_hover_rules;
    gboolean   has_active_rules;
    gboolean   cached;
    guint      pseudo_mask;
    struct ns_css_rule_index *index;
} ns_css_stylesheet;

gboolean ns_css_stylesheet_has_container_rules(const ns_css_stylesheet *sh);
gboolean ns_css_stylesheet_has_hover_rules(const ns_css_stylesheet *sh);
gboolean ns_css_stylesheet_has_active_rules(const ns_css_stylesheet *sh);

#define NS_CSS_IMPORT_MAX_DEPTH 8

ns_css_stylesheet *ns_css_stylesheet_parse(const char *text, gssize len);
ns_css_stylesheet *ns_css_stylesheet_from_style_element_cached(ns_node *style);
char              *ns_css_style_element_text(ns_node *style);
ns_css_stylesheet *ns_css_merged_styles_cached(const char *css, gssize len);
ns_css_stylesheet *ns_css_stylesheet_parse_url_cached(const char *url,
                                                      const char *css,
                                                      gssize len);
void               ns_css_style_element_cache_begin(void);
void               ns_css_relayout_enter(void);
void               ns_css_relayout_leave(void);
void               ns_css_stylesheet_resolve_urls(ns_css_stylesheet *s,
                                                  const char *base_url);
void               ns_css_stylesheet_free(ns_css_stylesheet *s);
void               ns_css_stylesheet_force_layer(ns_css_stylesheet *s,
                                                 const char *layer_name);

typedef struct ns_style {
    ns_css_value *values[NS_CSS_PROP_COUNT];
    struct ns_style *before;
    struct ns_style *after;
    struct ns_style *first_letter;
    struct ns_style *first_line;
    struct ns_style *placeholder;
    struct ns_style *selection;
    struct ns_style *marker;
    struct ns_style *backdrop;
    guint share_id;
    int   ref;
    struct ns_var_map *vars;
} ns_style;

int ns_css_writing_mode(const ns_style *s);
int ns_css_text_orientation(const ns_style *s);

const char *ns_var_map_lookup(const struct ns_var_map *m, const char *name);

void ns_css_style_effective_transform(const ns_style *st,
                                      const ns_css_transform *transform_override,
                                      ns_css_transform *out);
void ns_css_transform_to_mat4(const ns_css_transform *tf,
                              double bw, double bh, ns_mat4 *out);

ns_css_keyframes *ns_css_keyframes_resolve(const ns_css_keyframes *kf,
                                           const struct ns_var_map *vars);
void ns_css_keyframes_resolved_free(ns_css_keyframes *kf);

void ns_css_append_unescaped(GString *out, const char **pp);

GHashTable *ns_css_compute(ns_node                 *doc,
                           const ns_css_stylesheet *const *author_sheets,
                           gsize                     n_sheets);

void ns_css_mark_restyle_dirty(ns_node *parent);
void ns_css_mark_childlist_dirty(ns_node *parent, ns_node *added);
void ns_css_mark_attr_dirty(ns_node *target, const char *name,
                            const char *old_value);
gboolean ns_css_attr_may_affect_style(const ns_node *target, const char *name);
void ns_css_restyle_invalidate(void);
void ns_css_set_render_zoom(double zoom);

void ns_css_set_container_map(GHashTable *map);
void ns_css_set_container_dims(double inline_px, double block_px);
GHashTable *ns_css_container_map_new(void);
void ns_css_container_map_add(GHashTable *map, const void *node,
                              const char *type_kw, const char *name_kw,
                              double w, double h);

void ns_css_set_target_fragment(const char *fragment);

const ns_node *ns_css_set_focus_node(const ns_node *node);
const ns_node *ns_css_set_hover_node(const ns_node *node);
const ns_node *ns_css_set_active_node(const ns_node *node);

void ns_css_mark_visited(const char *abs_url);
void ns_css_set_doc_base(const char *base_url);
void ns_css_set_doc_language(const char *lang);

const char *ns_style_keyword(const ns_style *s, ns_css_prop p);

int ns_css_used_column_count(const ns_style *s, double avail_w,
                             double *out_gap);

char *ns_css_value_serialize(const ns_css_value *v);
char *ns_css_individual_transform_serialize(const ns_css_value *v, int prop);
char *ns_css_math_canonical(const char *value);
char *ns_css_transform_canonical(const char *value);
char *ns_css_display_canonical(const char *value);
char *ns_css_display_blockify(const char *d);
char *ns_css_specified_canonical(const char *prop, const char *value);
char *ns_css_time_specified(const char *value);
char *ns_css_time_computed(const char *value);

gboolean ns_css_parse_color(const char *s, guint8 *r, guint8 *g, guint8 *b,
                            guint8 *a);

G_END_DECLS

#endif
