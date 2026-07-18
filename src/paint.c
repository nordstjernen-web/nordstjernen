/* Nordstjernen — Cairo paint.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "paint.h"

static int g_dbg_paint_x = -2, g_dbg_paint_y = -2;

#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "css.h"
#include "dom.h"
#include "spellcheck.h"
#include "font.h"
#include "image.h"
#include "mathml.h"
#include "selection.h"
#include "video.h"

typedef struct rgba {
    double r, g, b, a;
} rgba;

static gboolean       g_caret_visible = TRUE;
static int            g_paint_no_cull;
static GPtrArray     *g_paint_deferred_list;

static int            g_paint_defer_depth;
static const ns_box  *g_paint_flush_box;
static ns_js         *g_paint_js;
static ns_anim       *g_paint_anim;
static gboolean       g_search_case_sensitive;
static const ns_box  *g_search_active_box;

static cairo_surface_t *texture_surface_cached(ns_texture *tex,
                                               const char *filter_kw);

static PangoLayout *
paint_create_layout(void)
{
    static PangoContext *cached_ctx;
    if (!cached_ctx) {
        PangoFontMap *fm = pango_cairo_font_map_get_default();
        cached_ctx = pango_font_map_create_context(fm);
        cairo_font_options_t *fo = cairo_font_options_create();
        const cairo_font_options_t *base =
            pango_cairo_context_get_font_options(cached_ctx);
        if (base) cairo_font_options_merge(fo, base);
        cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
        cairo_font_options_set_subpixel_order(fo, CAIRO_SUBPIXEL_ORDER_DEFAULT);
        pango_cairo_context_set_font_options(cached_ctx, fo);
        cairo_font_options_destroy(fo);
    }
    return pango_layout_new(cached_ctx);
}

static PangoWeight
pango_weight_from_css(int weight)
{
    if (weight <= 100) return PANGO_WEIGHT_THIN;
    if (weight <= 200) return PANGO_WEIGHT_ULTRALIGHT;
    if (weight <= 300) return PANGO_WEIGHT_LIGHT;
    if (weight <= 400) return PANGO_WEIGHT_NORMAL;
    if (weight <= 500) return PANGO_WEIGHT_MEDIUM;
    if (weight <= 600) return PANGO_WEIGHT_SEMIBOLD;
    if (weight <= 700) return PANGO_WEIGHT_BOLD;
    if (weight <= 800) return PANGO_WEIGHT_ULTRABOLD;
    if (weight <= 900) return PANGO_WEIGHT_HEAVY;
    return (PangoWeight)weight;
}

static PangoStretch
pango_stretch_from_css(int rank)
{
    static const PangoStretch map[] = {
        PANGO_STRETCH_ULTRA_CONDENSED,
        PANGO_STRETCH_EXTRA_CONDENSED,
        PANGO_STRETCH_CONDENSED,
        PANGO_STRETCH_SEMI_CONDENSED,
        PANGO_STRETCH_NORMAL,
        PANGO_STRETCH_SEMI_EXPANDED,
        PANGO_STRETCH_EXPANDED,
        PANGO_STRETCH_EXTRA_EXPANDED,
        PANGO_STRETCH_ULTRA_EXPANDED,
    };
    if (rank < 0) rank = 0;
    if (rank > 8) rank = 8;
    return map[rank];
}

void
ns_paint_set_search(gboolean case_sensitive, const ns_box *active)
{
    g_search_case_sensitive = case_sensitive;
    g_search_active_box = active;
}

void
ns_paint_set_js(ns_js *js)
{
    g_paint_js = js;
}

void
ns_paint_set_anim(ns_anim *anim)
{
    g_paint_anim = anim;
}

static rgba
rgba_of(const ns_css_value *v, double dr, double dg, double db, double da)
{
    rgba c = { dr, dg, db, da };
    if (!v || v->kind != NS_CSS_V_COLOR) return c;
    c.r = v->u.color.r / 255.0;
    c.g = v->u.color.g / 255.0;
    c.b = v->u.color.b / 255.0;
    c.a = v->u.color.a / 255.0;
    return c;
}

static rgba
rgba_anim(const ns_box *b, ns_css_anim_target which,
          const ns_css_value *v, double dr, double dg, double db, double da)
{
    if (b && b->dom && g_paint_anim) {
        guint8 c[4];
        if (ns_anim_get_color(g_paint_anim, b->dom, which, c)) {
            rgba r = { c[0] / 255.0, c[1] / 255.0, c[2] / 255.0, c[3] / 255.0 };
            return r;
        }
    }
    return rgba_of(v, dr, dg, db, da);
}

static inline void
set_source_rgba(cairo_t *cr, rgba c)
{
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

static gboolean
overflow_kw_clips(const char *ov)
{
    return ov && (g_ascii_strcasecmp(ov, "hidden") == 0 ||
                  g_ascii_strcasecmp(ov, "clip")   == 0 ||
                  g_ascii_strcasecmp(ov, "auto")   == 0 ||
                  g_ascii_strcasecmp(ov, "scroll") == 0);
}

#define length_or ns_css_length_or

#define keyword_is ns_css_keyword_is

static double
bg_size_px(double v, ns_css_unit unit, double basis)
{
    switch (unit) {
    case NS_CSS_UNIT_PERCENT: return v * basis / 100.0;
    case NS_CSS_UNIT_EM:
    case NS_CSS_UNIT_REM:     return v * 16.0;
    case NS_CSS_UNIT_VW:      return v * ns_css_viewport_w() / 100.0;
    case NS_CSS_UNIT_VH:      return v * ns_css_viewport_h() / 100.0;
    case NS_CSS_UNIT_VMIN: {
        double m = MIN(ns_css_viewport_w(), ns_css_viewport_h());
        return v * m / 100.0;
    }
    case NS_CSS_UNIT_VMAX: {
        double m = MAX(ns_css_viewport_w(), ns_css_viewport_h());
        return v * m / 100.0;
    }
    case NS_CSS_UNIT_NUMBER:
    case NS_CSS_UNIT_PX:
    default:                  return v;
    }
}

typedef struct corner_radii {
    double tl, tr, br, bl;
} corner_radii;

static double
positive_length(const ns_css_value *v)
{
    if (!v || v->kind != NS_CSS_V_LENGTH) return -1;
    double r = v->u.length.v;
    return r > 0 ? r : 0;
}

static corner_radii
style_border_radii(const ns_style *s)
{
    corner_radii c = {0};
    if (!s) return c;
    double base = positive_length(s->values[NS_CSS_BORDER_RADIUS]);
    if (base < 0) base = 0;
    double tl = positive_length(s->values[NS_CSS_BORDER_TOP_LEFT_RADIUS]);
    double tr = positive_length(s->values[NS_CSS_BORDER_TOP_RIGHT_RADIUS]);
    double br = positive_length(s->values[NS_CSS_BORDER_BOTTOM_RIGHT_RADIUS]);
    double bl = positive_length(s->values[NS_CSS_BORDER_BOTTOM_LEFT_RADIUS]);
    c.tl = tl >= 0 ? tl : base;
    c.tr = tr >= 0 ? tr : base;
    c.br = br >= 0 ? br : base;
    c.bl = bl >= 0 ? bl : base;
    return c;
}

static corner_radii
box_border_radii(const ns_box *b)
{
    return style_border_radii(b ? b->style : NULL);
}

static gboolean
corner_radii_zero(corner_radii c)
{
    return c.tl <= 0 && c.tr <= 0 && c.br <= 0 && c.bl <= 0;
}

static void
rounded_rect_path(cairo_t *cr, double x, double y, double w, double h,
                  corner_radii c)
{
    double half_w = w / 2.0;
    double half_h = h / 2.0;
    if (c.tl > half_w) c.tl = half_w;
    if (c.tr > half_w) c.tr = half_w;
    if (c.br > half_w) c.br = half_w;
    if (c.bl > half_w) c.bl = half_w;
    if (c.tl > half_h) c.tl = half_h;
    if (c.tr > half_h) c.tr = half_h;
    if (c.br > half_h) c.br = half_h;
    if (c.bl > half_h) c.bl = half_h;
    if (corner_radii_zero(c)) { cairo_rectangle(cr, x, y, w, h); return; }
    cairo_new_sub_path(cr);
    if (c.tr > 0) cairo_arc(cr, x + w - c.tr, y + c.tr,     c.tr, -G_PI_2,  0);
    else          cairo_move_to(cr, x + w, y);
    if (c.br > 0) cairo_arc(cr, x + w - c.br, y + h - c.br, c.br,  0,       G_PI_2);
    else          cairo_line_to(cr, x + w, y + h);
    if (c.bl > 0) cairo_arc(cr, x + c.bl,     y + h - c.bl, c.bl,  G_PI_2,  G_PI);
    else          cairo_line_to(cr, x, y + h);
    if (c.tl > 0) cairo_arc(cr, x + c.tl,     y + c.tl,     c.tl,  G_PI,    1.5 * G_PI);
    else          cairo_line_to(cr, x, y);
    cairo_close_path(cr);
}

static void
fill_outer_shadow(cairo_t *cr, double ox, double oy, double ow, double oh,
                  double ix, double iy, double iw, double ih, corner_radii radii)
{
    rounded_rect_path(cr, ox, oy, ow, oh, radii);
    rounded_rect_path(cr, ix, iy, iw, ih, radii);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_fill(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
}

static void box_blur_argb(guchar *data, int stride, int w, int h, int radius);

static void
paint_blurred_box_shadow(cairo_t *cr, double sx, double sy, double sw, double sh_h,
                         corner_radii radii, double blur,
                         double br, double bg, double bb, double ba,
                         double clip_x, double clip_y, double clip_w, double clip_h,
                         corner_radii clip_radii)
{
    int radius = (int)(blur * 0.5 + 0.5);
    if (radius < 1) radius = 1;
    if (radius > 256) radius = 256;
    int pad = radius * 3 + 2;
    int isw = (int)ceil(sw), ish = (int)ceil(sh_h);
    if (isw < 1) isw = 1;
    if (ish < 1) ish = 1;
    int surf_w = isw + pad * 2, surf_h = ish + pad * 2;
    if (surf_w > 8192 || surf_h > 8192) return;
    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, surf_w, surf_h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return;
    }
    cairo_t *scr = cairo_create(surf);
    rounded_rect_path(scr, pad, pad, sw, sh_h, radii);
    cairo_set_source_rgba(scr, br, bg, bb, ba);
    cairo_fill(scr);
    cairo_destroy(scr);
    cairo_surface_flush(surf);
    guchar *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    box_blur_argb(data, stride, surf_w, surf_h, radius);
    box_blur_argb(data, stride, surf_w, surf_h, radius);
    box_blur_argb(data, stride, surf_w, surf_h, radius);
    cairo_surface_mark_dirty(surf);

    cairo_save(cr);
    cairo_new_path(cr);
    cairo_rectangle(cr, sx - pad, sy - pad, surf_w, surf_h);
    rounded_rect_path(cr, clip_x, clip_y, clip_w, clip_h, clip_radii);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_clip(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    cairo_set_source_surface(cr, surf, sx - pad, sy - pad);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(surf);
}

static gboolean
style_side_visible(const ns_style *s, ns_css_prop wp, ns_css_prop sp)
{
    if (!s) return FALSE;
    double w = length_or(s->values[wp], 0);
    if (w <= 0) return FALSE;
    const ns_css_value *st = s->values[sp];
    return !keyword_is(st, "none") && !keyword_is(st, "hidden");
}

static gboolean
style_has_inline_box_paint(const ns_style *s)
{
    if (!s) return FALSE;
    const ns_css_value *bg = s->values[NS_CSS_BACKGROUND_COLOR];
    if (bg && bg->kind == NS_CSS_V_COLOR && bg->u.color.a > 0)
        return TRUE;
    if (s->values[NS_CSS_BOX_SHADOW] &&
        s->values[NS_CSS_BOX_SHADOW]->kind == NS_CSS_V_SHADOW &&
        s->values[NS_CSS_BOX_SHADOW]->u.shadow.n > 0)
        return TRUE;
    if (s->values[NS_CSS_BACKGROUND_IMAGE] &&
        (s->values[NS_CSS_BACKGROUND_IMAGE]->kind == NS_CSS_V_URL ||
         s->values[NS_CSS_BACKGROUND_IMAGE]->kind == NS_CSS_V_GRADIENT))
        return TRUE;
    if (style_side_visible(s, NS_CSS_BORDER_TOP_WIDTH, NS_CSS_BORDER_TOP_STYLE) ||
        style_side_visible(s, NS_CSS_BORDER_RIGHT_WIDTH, NS_CSS_BORDER_RIGHT_STYLE) ||
        style_side_visible(s, NS_CSS_BORDER_BOTTOM_WIDTH, NS_CSS_BORDER_BOTTOM_STYLE) ||
        style_side_visible(s, NS_CSS_BORDER_LEFT_WIDTH, NS_CSS_BORDER_LEFT_STYLE))
        return TRUE;
    return FALSE;
}

static gboolean
style_uniform_solid_border(const ns_style *s, double *out_w, rgba *out_color)
{
    if (!s) return FALSE;
    const ns_css_prop widths[4] = {
        NS_CSS_BORDER_TOP_WIDTH,
        NS_CSS_BORDER_RIGHT_WIDTH,
        NS_CSS_BORDER_BOTTOM_WIDTH,
        NS_CSS_BORDER_LEFT_WIDTH,
    };
    const ns_css_prop styles[4] = {
        NS_CSS_BORDER_TOP_STYLE,
        NS_CSS_BORDER_RIGHT_STYLE,
        NS_CSS_BORDER_BOTTOM_STYLE,
        NS_CSS_BORDER_LEFT_STYLE,
    };
    const ns_css_prop colors[4] = {
        NS_CSS_BORDER_TOP_COLOR,
        NS_CSS_BORDER_RIGHT_COLOR,
        NS_CSS_BORDER_BOTTOM_COLOR,
        NS_CSS_BORDER_LEFT_COLOR,
    };
    double bw = 0;
    rgba bc = {0};
    for (int i = 0; i < 4; i++) {
        if (!style_side_visible(s, widths[i], styles[i])) return FALSE;
        const ns_css_value *st = s->values[styles[i]];
        if (st && st->kind == NS_CSS_V_KEYWORD && st->u.keyword &&
            strcmp(st->u.keyword, "solid") != 0)
            return FALSE;
        double w = length_or(s->values[widths[i]], 0);
        rgba c = rgba_of(s->values[colors[i]] ? s->values[colors[i]]
                                              : s->values[NS_CSS_COLOR], 0, 0, 0, 1);
        if (i == 0) {
            bw = w;
            bc = c;
        } else {
            if (fabs(w - bw) > 0.01) return FALSE;
            if (fabs(c.r - bc.r) > 0.001 ||
                fabs(c.g - bc.g) > 0.001 ||
                fabs(c.b - bc.b) > 0.001 ||
                fabs(c.a - bc.a) > 0.001)
                return FALSE;
        }
    }
    if (out_w) *out_w = bw;
    if (out_color) *out_color = bc;
    return bw > 0;
}

static double
inline_control_dim_px(const ns_css_value *v, double font_size, double basis)
{
    if (!v) return 0;
    if (v->kind == NS_CSS_V_CALC) {
        double out = v->u.calc.px;
        if (basis > 0) out += v->u.calc.pct * basis / 100.0;
        return out > 0 ? out : 0;
    }
    if (v->kind != NS_CSS_V_LENGTH) return 0;
    switch (v->u.length.unit) {
    case NS_CSS_UNIT_PX:
    case NS_CSS_UNIT_NUMBER:
        return v->u.length.v;
    case NS_CSS_UNIT_EM:
        return v->u.length.v * font_size;
    case NS_CSS_UNIT_REM:
        return v->u.length.v * 16.0;
    case NS_CSS_UNIT_PERCENT:
        return basis > 0 ? v->u.length.v * basis / 100.0 : 0;
    case NS_CSS_UNIT_VW:
        return v->u.length.v * ns_css_viewport_w() / 100.0;
    case NS_CSS_UNIT_VH:
        return v->u.length.v * ns_css_viewport_h() / 100.0;
    case NS_CSS_UNIT_VMIN:
        return v->u.length.v * MIN(ns_css_viewport_w(), ns_css_viewport_h()) / 100.0;
    case NS_CSS_UNIT_VMAX:
        return v->u.length.v * MAX(ns_css_viewport_w(), ns_css_viewport_h()) / 100.0;
    case NS_CSS_UNIT_CQW:
        return v->u.length.v * (ns_css_container_w() > 0 ? ns_css_container_w() : ns_css_viewport_w()) / 100.0;
    case NS_CSS_UNIT_CQH:
        return v->u.length.v * (ns_css_container_h() > 0 ? ns_css_container_h() : ns_css_viewport_h()) / 100.0;
    case NS_CSS_UNIT_CQMIN: {
        double cw = ns_css_container_w() > 0 ? ns_css_container_w() : ns_css_viewport_w();
        double ch = ns_css_container_h() > 0 ? ns_css_container_h() : ns_css_viewport_h();
        return v->u.length.v * MIN(cw, ch) / 100.0;
    }
    case NS_CSS_UNIT_CQMAX: {
        double cw = ns_css_container_w() > 0 ? ns_css_container_w() : ns_css_viewport_w();
        double ch = ns_css_container_h() > 0 ? ns_css_container_h() : ns_css_viewport_h();
        return v->u.length.v * MAX(cw, ch) / 100.0;
    }
    case NS_CSS_UNIT_EX:
    case NS_CSS_UNIT_CH:
        return v->u.length.v * font_size * 0.5;
    case NS_CSS_UNIT_CAP:
        return v->u.length.v * font_size * 0.7;
    case NS_CSS_UNIT_IC:
        return v->u.length.v * font_size;
    }
    return 0;
}

static double
inline_control_dim_px_clamped(const ns_style *s, ns_css_prop value_prop,
                              ns_css_prop min_prop, ns_css_prop max_prop,
                              double font_size, double basis)
{
    if (!s) return 0;
    double out = inline_control_dim_px(s->values[value_prop], font_size, basis);
    double mn = inline_control_dim_px(s->values[min_prop], font_size, basis);
    double mx = inline_control_dim_px(s->values[max_prop], font_size, basis);
    if (mn > 0 && out > 0 && out < mn) out = mn;
    if (mx > 0 && out > mx) out = mx;
    return out;
}

static double
inline_control_css_width(const ns_inline_attr *r, const ns_box *b)
{
    if (!r || !r->style) return r && r->box_w > 0 ? r->box_w : 0;
    double fs = length_or(r->style->values[NS_CSS_FONT_SIZE], 16);
    double w = inline_control_dim_px_clamped(r->style, NS_CSS_WIDTH,
                                             NS_CSS_MIN_WIDTH, NS_CSS_MAX_WIDTH,
                                             fs, b ? b->content_width : 0);
    if (w > 0) w += ns_control_css_extra_w(r->dom, r->style);
    return w > 0 ? w : r->box_w;
}

static double
inline_control_css_min_width(const ns_inline_attr *r, const ns_box *b)
{
    if (!r || !r->style) return 0;
    double fs = length_or(r->style->values[NS_CSS_FONT_SIZE], 16);
    double mn = inline_control_dim_px(r->style->values[NS_CSS_MIN_WIDTH], fs,
                                      b ? b->content_width : 0);
    if (mn > 0) mn += ns_control_css_extra_w(r->dom, r->style);
    return mn;
}

static void
paint_inline_box_shadow(cairo_t *cr, const ns_style *s, double x, double y,
                        double w, double h, corner_radii radii)
{
    if (!s || !s->values[NS_CSS_BOX_SHADOW] ||
        s->values[NS_CSS_BOX_SHADOW]->kind != NS_CSS_V_SHADOW)
        return;
    const ns_css_shadow_list *sl = &s->values[NS_CSS_BOX_SHADOW]->u.shadow;
    for (int si = sl->n - 1; si >= 0; si--) {
        const ns_css_shadow *sh = &sl->s[si];
        if (sh->inset) continue;
        double sx = x + sh->x - sh->spread;
        double sy = y + sh->y - sh->spread;
        double sw = w + sh->spread * 2;
        double sh_h = h + sh->spread * 2;
        int blur = (int)sh->blur;
        if (blur > 0) {
            int steps = blur > 12 ? 12 : blur;
            if (steps < 1) steps = 1;
            for (int i = steps; i >= 1; i--) {
                double t = (double)i / steps;
                double pad = sh->blur * t;
                double alpha = (sh->a / 255.0) * (1.0 - t) * 0.7;
                cairo_set_source_rgba(cr,
                    sh->r / 255.0, sh->g / 255.0, sh->b / 255.0, alpha);
                fill_outer_shadow(cr, sx - pad, sy - pad,
                                  sw + pad * 2, sh_h + pad * 2,
                                  x, y, w, h, radii);
            }
        } else {
            cairo_set_source_rgba(cr,
                sh->r / 255.0, sh->g / 255.0, sh->b / 255.0,
                sh->a / 255.0);
            fill_outer_shadow(cr, sx, sy, sw, sh_h, x, y, w, h, radii);
        }
    }
}

static gboolean
style_pixelated(const ns_style *s)
{
    const ns_css_value *ir = s ? s->values[NS_CSS_IMAGE_RENDERING] : NULL;
    return ir && ir->kind == NS_CSS_V_KEYWORD && ir->u.keyword &&
           (strcmp(ir->u.keyword, "pixelated") == 0 ||
            strcmp(ir->u.keyword, "crisp-edges") == 0);
}

static void
paint_bg_image_core(cairo_t *cr, ns_image *img,
                    const ns_css_value *rep_v, const ns_css_value *sz,
                    const ns_css_value *px, const ns_css_value *py,
                    gboolean pixelated,
                    double x, double y, double w, double h,
                    double clip_x, double clip_y, double clip_w, double clip_h,
                    corner_radii radii)
{
    if (!img || !img->loaded || !img->texture) return;
    int iw = ns_texture_get_width(img->texture);
    int ih = ns_texture_get_height(img->texture);
    if (iw <= 0 || ih <= 0) return;
    gboolean tile_x = TRUE, tile_y = TRUE;
    const char *repeat = (rep_v && rep_v->kind == NS_CSS_V_KEYWORD)
                         ? rep_v->u.keyword : NULL;
    if (repeat) {
        if (strcmp(repeat, "no-repeat") == 0) { tile_x = tile_y = FALSE; }
        else if (strcmp(repeat, "repeat-x") == 0) { tile_y = FALSE; }
        else if (strcmp(repeat, "repeat-y") == 0) { tile_x = FALSE; }
    }
    double draw_w = iw, draw_h = ih;
    if (sz && sz->kind == NS_CSS_V_KEYWORD && sz->u.keyword) {
        if (strcmp(sz->u.keyword, "cover") == 0) {
            double sx = w / (double)iw;
            double sy = h / (double)ih;
            double sc = sx > sy ? sx : sy;
            draw_w = iw * sc;
            draw_h = ih * sc;
        } else if (strcmp(sz->u.keyword, "contain") == 0) {
            double sx = w / (double)iw;
            double sy = h / (double)ih;
            double sc = sx < sy ? sx : sy;
            draw_w = iw * sc;
            draw_h = ih * sc;
        }
    } else if (sz && sz->kind == NS_CSS_V_LENGTH) {
        draw_w = bg_size_px(sz->u.length.v, sz->u.length.unit, w);
        draw_h = draw_w * ((double)ih / (double)iw);
    } else if (sz && sz->kind == NS_CSS_V_SIZE) {
        gboolean wa = sz->u.size.w_auto;
        gboolean ha = sz->u.size.h_auto;
        if (!wa) draw_w = bg_size_px(sz->u.size.w, sz->u.size.w_unit, w);
        if (!ha) draw_h = bg_size_px(sz->u.size.h, sz->u.size.h_unit, h);
        if (wa && !ha) draw_w = draw_h * ((double)iw / (double)ih);
        else if (!wa && ha) draw_h = draw_w * ((double)ih / (double)iw);
        else if (wa && ha) { draw_w = iw; draw_h = ih; }
    }
    if (draw_w < 1) draw_w = 1;
    if (draw_h < 1) draw_h = 1;
    double off_x = 0, off_y = 0;
    if (px && px->kind == NS_CSS_V_LENGTH) {
        if (px->u.length.unit == NS_CSS_UNIT_PERCENT)
            off_x = (w - draw_w) * (px->u.length.v / 100.0);
        else
            off_x = px->u.length.v;
    } else if (px && px->kind == NS_CSS_V_CALC) {
        off_x = (w - draw_w) * (px->u.calc.pct / 100.0) + px->u.calc.px;
    }
    if (py && py->kind == NS_CSS_V_LENGTH) {
        if (py->u.length.unit == NS_CSS_UNIT_PERCENT)
            off_y = (h - draw_h) * (py->u.length.v / 100.0);
        else
            off_y = py->u.length.v;
    } else if (py && py->kind == NS_CSS_V_CALC) {
        off_y = (h - draw_h) * (py->u.calc.pct / 100.0) + py->u.calc.px;
    }
    cairo_surface_t *surf = texture_surface_cached(img->texture, NULL);
    if (!surf) return;
    cairo_save(cr);
    rounded_rect_path(cr, clip_x, clip_y, clip_w, clip_h, radii);
    cairo_clip(cr);
    cairo_pattern_t *pat = cairo_pattern_create_for_surface(surf);
    cairo_pattern_set_extend(pat,
        (tile_x || tile_y) ? CAIRO_EXTEND_REPEAT : CAIRO_EXTEND_NONE);
    if (pixelated)
        cairo_pattern_set_filter(pat, CAIRO_FILTER_NEAREST);
    cairo_matrix_t m;
    cairo_matrix_init_identity(&m);
    cairo_matrix_scale(&m, (double)iw / draw_w, (double)ih / draw_h);
    cairo_matrix_translate(&m, -(x + off_x), -(y + off_y));
    cairo_pattern_set_matrix(pat, &m);
    cairo_set_source(cr, pat);
    if (tile_x && tile_y) {
        cairo_paint(cr);
    } else if (!tile_x && !tile_y) {
        cairo_rectangle(cr, x + off_x, y + off_y, draw_w, draw_h);
        cairo_fill(cr);
    } else if (tile_x) {
        cairo_rectangle(cr, x, y + off_y, w, draw_h);
        cairo_fill(cr);
    } else {
        cairo_rectangle(cr, x + off_x, y, draw_w, h);
        cairo_fill(cr);
    }
    cairo_pattern_destroy(pat);
    cairo_restore(cr);
}

static void
conic_color_at(const ns_css_gradient *gr, double frac,
               double *r, double *g, double *b, double *a)
{
    double pos = frac + gr->from_deg / 360.0;
    while (pos < 0) pos += 1.0;
    while (pos >= 1.0) pos -= 1.0;
    if (gr->repeating) {
        double cper = gr->stops[gr->n_stops - 1].pos;
        if (cper > 0) pos = fmod(pos, cper);
    }
    int lo = 0;
    while (lo + 1 < gr->n_stops && gr->stops[lo + 1].pos < pos) lo++;
    int hi = lo + 1;
    if (hi >= gr->n_stops) hi = gr->n_stops - 1;
    double t = 0.0;
    if (gr->stops[hi].pos > gr->stops[lo].pos)
        t = (pos - gr->stops[lo].pos) /
            (gr->stops[hi].pos - gr->stops[lo].pos);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    *r = (gr->stops[lo].r * (1 - t) + gr->stops[hi].r * t) / 255.0;
    *g = (gr->stops[lo].g * (1 - t) + gr->stops[hi].g * t) / 255.0;
    *b = (gr->stops[lo].b * (1 - t) + gr->stops[hi].b * t) / 255.0;
    *a = (gr->stops[lo].a * (1 - t) + gr->stops[hi].a * t) / 255.0;
}

static void
paint_bg_gradient_core(cairo_t *cr, const ns_css_gradient *gr,
                       double border_x, double border_y,
                       double border_w, double border_h,
                       double clip_x, double clip_y,
                       double clip_w, double clip_h,
                       corner_radii radii)
{
    double cx = border_x + gr->center_x * border_w;
    double cy = border_y + gr->center_y * border_h;
    if (gr->conic && gr->n_stops > 0) {
        enum { CONIC_BASE = 24, CONIC_CAP = 96 };
        double bnd[CONIC_CAP];
        int nb = 0;
        for (int k = 0; k <= CONIC_BASE; k++)
            bnd[nb++] = k / (double)CONIC_BASE;
        double off = gr->from_deg / 360.0;
        for (int s = 0; s < gr->n_stops && nb < CONIC_CAP; s++) {
            double rel_pos = gr->stops[s].pos - off;
            rel_pos -= floor(rel_pos);
            bnd[nb++] = rel_pos;
            if (nb < CONIC_CAP) bnd[nb++] = rel_pos;
        }
        for (int i = 1; i < nb; i++) {
            double key = bnd[i];
            int j = i - 1;
            while (j >= 0 && bnd[j] > key) { bnd[j + 1] = bnd[j]; j--; }
            bnd[j + 1] = key;
        }
        double r_outer = sqrt(border_w * border_w + border_h * border_h) /
                         cos(G_PI / CONIC_BASE);
        cairo_save(cr);
        rounded_rect_path(cr, clip_x, clip_y, clip_w, clip_h, radii);
        cairo_clip(cr);
        cairo_pattern_t *mesh = cairo_pattern_create_mesh();
        for (int i = 0; i + 1 < nb; i++) {
            double f1 = bnd[i], f2 = bnd[i + 1];
            double span = f2 - f1;
            if (span < 1e-6) continue;
            double eps = span * 1e-3;
            double a1 = f1 * 2 * G_PI - G_PI / 2;
            double a2 = f2 * 2 * G_PI - G_PI / 2;
            double r1, g1, b1, al1, r2, g2, b2, al2;
            conic_color_at(gr, f1 + eps, &r1, &g1, &b1, &al1);
            conic_color_at(gr, f2 - eps, &r2, &g2, &b2, &al2);
            cairo_mesh_pattern_begin_patch(mesh);
            cairo_mesh_pattern_move_to(mesh, cx, cy);
            cairo_mesh_pattern_line_to(mesh, cx + r_outer * cos(a1),
                                       cy + r_outer * sin(a1));
            cairo_mesh_pattern_line_to(mesh, cx + r_outer * cos(a2),
                                       cy + r_outer * sin(a2));
            cairo_mesh_pattern_line_to(mesh, cx, cy);
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 0, r1, g1, b1, al1);
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 1, r1, g1, b1, al1);
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 2, r2, g2, b2, al2);
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 3, r2, g2, b2, al2);
            cairo_mesh_pattern_end_patch(mesh);
        }
        cairo_set_source(cr, mesh);
        cairo_paint(cr);
        cairo_pattern_destroy(mesh);
        cairo_restore(cr);
    } else {
        cairo_pattern_t *pat;
        double dxh = 0, dyh = 0, r_outer = 1, line_len;
        if (gr->radial) {
            double corners[4][2] = {
                { border_x, border_y },
                { border_x + border_w, border_y },
                { border_x, border_y + border_h },
                { border_x + border_w, border_y + border_h },
            };
            r_outer = 1;
            for (int k = 0; k < 4; k++) {
                double ddx = corners[k][0] - cx, ddy = corners[k][1] - cy;
                double dd = sqrt(ddx * ddx + ddy * ddy);
                if (dd > r_outer) r_outer = dd;
            }
            line_len = r_outer;
        } else {
            double rad = gr->angle_deg * G_PI / 180.0;
            double dx = sin(rad), dy = -cos(rad);
            double half = (fabs(dx) * border_w + fabs(dy) * border_h) / 2.0;
            dxh = dx * half;
            dyh = dy * half;
            line_len = 2.0 * half;
        }
        if (line_len <= 0) line_len = 1;
        double frac[NS_CSS_GRADIENT_STOPS_MAX];
        for (int i = 0; i < gr->n_stops; i++)
            frac[i] = gr->stops[i].pos_is_px
                ? gr->stops[i].pos / line_len : gr->stops[i].pos;
        double period = (gr->repeating && gr->n_stops > 0)
            ? frac[gr->n_stops - 1] : 1.0;
        if (period <= 0) period = 1.0;
        if (gr->radial) {
            double r_tile = r_outer * period;
            if (r_tile <= 0) r_tile = 1;
            pat = cairo_pattern_create_radial(cx, cy, 0, cx, cy, r_tile);
        } else {
            double x0 = cx - dxh, y0 = cy - dyh;
            double x1 = cx + dxh, y1 = cy + dyh;
            pat = cairo_pattern_create_linear(
                x0, y0,
                x0 + (x1 - x0) * period, y0 + (y1 - y0) * period);
        }
        for (int i = 0; i < gr->n_stops; i++) {
            const ns_css_gradient_stop *st = &gr->stops[i];
            cairo_pattern_add_color_stop_rgba(pat, frac[i] / period,
                st->r / 255.0, st->g / 255.0, st->b / 255.0, st->a / 255.0);
        }
        if (gr->repeating)
            cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
        cairo_save(cr);
        rounded_rect_path(cr, clip_x, clip_y, clip_w, clip_h, radii);
        cairo_clip(cr);
        cairo_set_source(cr, pat);
        cairo_paint(cr);
        cairo_pattern_destroy(pat);
        cairo_restore(cr);
    }
}

static void
paint_inline_background_image(cairo_t *cr, const ns_inline_attr *r,
                              const ns_style *s, double x, double y,
                              double w, double h, corner_radii radii)
{
    ns_image *img = r && r->bg_image ? r->bg_image : NULL;
    if (!img) return;
    paint_bg_image_core(cr, img,
        s ? s->values[NS_CSS_BACKGROUND_REPEAT] : NULL,
        s ? s->values[NS_CSS_BACKGROUND_SIZE] : NULL,
        s ? s->values[NS_CSS_BACKGROUND_POSITION_X] : NULL,
        s ? s->values[NS_CSS_BACKGROUND_POSITION_Y] : NULL,
        style_pixelated(s),
        x, y, w, h, x, y, w, h, radii);
}

static void
paint_inline_css_chrome(cairo_t *cr, const ns_inline_attr *r, double x, double y,
                        double w, double h)
{
    const ns_style *s = r ? r->style : NULL;
    if (!style_has_inline_box_paint(s) || w <= 0 || h <= 0) return;
    corner_radii radii = style_border_radii(s);
    cairo_save(cr);
    paint_inline_box_shadow(cr, s, x, y, w, h, radii);
    rgba bg = rgba_of(s->values[NS_CSS_BACKGROUND_COLOR], 0, 0, 0, 0);
    if (bg.a > 0) {
        set_source_rgba(cr, bg);
        rounded_rect_path(cr, x, y, w, h, radii);
        cairo_fill(cr);
    }
    paint_inline_background_image(cr, r, s, x, y, w, h, radii);
    double uniform_bw = 0;
    rgba uniform_color = {0};
    if (!corner_radii_zero(radii) &&
        style_uniform_solid_border(s, &uniform_bw, &uniform_color)) {
        set_source_rgba(cr, uniform_color);
        cairo_set_line_width(cr, uniform_bw);
        rounded_rect_path(cr, x + uniform_bw / 2.0, y + uniform_bw / 2.0,
                          w - uniform_bw, h - uniform_bw, radii);
        cairo_stroke(cr);
        cairo_restore(cr);
        return;
    }
    const struct {
        ns_css_prop width;
        ns_css_prop style;
        ns_css_prop color;
        double x1, y1, x2, y2;
    } sides[4] = {
        { NS_CSS_BORDER_TOP_WIDTH, NS_CSS_BORDER_TOP_STYLE,
          NS_CSS_BORDER_TOP_COLOR, x, y, x + w, y },
        { NS_CSS_BORDER_RIGHT_WIDTH, NS_CSS_BORDER_RIGHT_STYLE,
          NS_CSS_BORDER_RIGHT_COLOR, x + w, y, x + w, y + h },
        { NS_CSS_BORDER_BOTTOM_WIDTH, NS_CSS_BORDER_BOTTOM_STYLE,
          NS_CSS_BORDER_BOTTOM_COLOR, x, y + h, x + w, y + h },
        { NS_CSS_BORDER_LEFT_WIDTH, NS_CSS_BORDER_LEFT_STYLE,
          NS_CSS_BORDER_LEFT_COLOR, x, y, x, y + h },
    };
    for (int i = 0; i < 4; i++) {
        double bw = length_or(s->values[sides[i].width], 0);
        if (bw <= 0 || !style_side_visible(s, sides[i].width, sides[i].style))
            continue;
        rgba c = rgba_of(s->values[sides[i].color] ? s->values[sides[i].color]
                                                   : s->values[NS_CSS_COLOR], 0, 0, 0, 1);
        set_source_rgba(cr, c);
        cairo_set_line_width(cr, bw);
        cairo_move_to(cr, sides[i].x1, sides[i].y1);
        cairo_line_to(cr, sides[i].x2, sides[i].y2);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static void
paint_block(cairo_t *cr, const ns_box *b)
{
    double border_x = b->x + b->margin.left;
    double border_y = b->y + b->margin.top;
    double border_w = b->content_width + b->padding.left + b->padding.right +
                      b->border.left + b->border.right;
    double border_h = b->content_height + b->padding.top + b->padding.bottom +
                      b->border.top + b->border.bottom;

    if (border_w <= 0 || border_h <= 0) return;

    const ns_style *s = b->style;
    corner_radii radii = box_border_radii(b);

    double clip_x = border_x, clip_y = border_y;
    double clip_w = border_w, clip_h = border_h;
    {
        const ns_css_value *bcl = s ? s->values[NS_CSS_BACKGROUND_CLIP] : NULL;
        const char *bk = (bcl && bcl->kind == NS_CSS_V_KEYWORD) ? bcl->u.keyword : NULL;
        if (bk && strcmp(bk, "padding-box") == 0) {
            clip_x += b->border.left; clip_y += b->border.top;
            clip_w -= b->border.left + b->border.right;
            clip_h -= b->border.top + b->border.bottom;
        } else if (bk && strcmp(bk, "content-box") == 0) {
            clip_x += b->border.left + b->padding.left;
            clip_y += b->border.top + b->padding.top;
            clip_w -= b->border.left + b->border.right + b->padding.left + b->padding.right;
            clip_h -= b->border.top + b->border.bottom + b->padding.top + b->padding.bottom;
        }
        if (clip_w < 0) clip_w = 0;
        if (clip_h < 0) clip_h = 0;
    }

    double pos_x = border_x + b->border.left;
    double pos_y = border_y + b->border.top;
    double pos_w = border_w - b->border.left - b->border.right;
    double pos_h = border_h - b->border.top - b->border.bottom;
    {
        const ns_css_value *bor = s ? s->values[NS_CSS_BACKGROUND_ORIGIN] : NULL;
        const char *ok = (bor && bor->kind == NS_CSS_V_KEYWORD) ? bor->u.keyword : NULL;
        if (ok && strcmp(ok, "border-box") == 0) {
            pos_x = border_x; pos_y = border_y;
            pos_w = border_w; pos_h = border_h;
        } else if (ok && strcmp(ok, "content-box") == 0) {
            pos_x += b->padding.left;
            pos_y += b->padding.top;
            pos_w -= b->padding.left + b->padding.right;
            pos_h -= b->padding.top + b->padding.bottom;
        }
        if (pos_w < 1) pos_w = 1;
        if (pos_h < 1) pos_h = 1;
    }

    if (s && s->values[NS_CSS_BOX_SHADOW] &&
        s->values[NS_CSS_BOX_SHADOW]->kind == NS_CSS_V_SHADOW) {
        const ns_css_shadow_list *sl = &s->values[NS_CSS_BOX_SHADOW]->u.shadow;
        for (int si = sl->n - 1; si >= 0; si--) {
            const ns_css_shadow *sh = &sl->s[si];
            if (sh->inset) continue;
            double sx = border_x + sh->x - sh->spread;
            double sy = border_y + sh->y - sh->spread;
            double sw = border_w + sh->spread * 2;
            double sh_h = border_h + sh->spread * 2;
            cairo_save(cr);
            if (sh->blur > 0) {
                paint_blurred_box_shadow(cr, sx, sy, sw, sh_h, radii, sh->blur,
                    sh->r / 255.0, sh->g / 255.0, sh->b / 255.0, sh->a / 255.0,
                    border_x, border_y, border_w, border_h, radii);
            } else {
                cairo_set_source_rgba(cr,
                    sh->r / 255.0, sh->g / 255.0, sh->b / 255.0,
                    sh->a / 255.0);
                fill_outer_shadow(cr, sx, sy, sw, sh_h,
                                  border_x, border_y, border_w, border_h,
                                  radii);
            }
            cairo_restore(cr);
        }
    }

    gboolean has_mask = s && s->values[NS_CSS_MASK_IMAGE] &&
                        s->values[NS_CSS_MASK_IMAGE]->kind == NS_CSS_V_URL;
    rgba bg = rgba_anim(b, NS_CSS_ANIM_TARGET_BG_COLOR,
                        s ? s->values[NS_CSS_BACKGROUND_COLOR] : NULL,
                        0, 0, 0, 0);
    if (bg.a > 0 && !has_mask) {
        set_source_rgba(cr, bg);
        rounded_rect_path(cr, clip_x, clip_y, clip_w, clip_h, radii);
        cairo_fill(cr);
    }

    gboolean masked_fill = FALSE;
    if (bg.a > 0 && has_mask && b->media && b->media->bg_image) {
        ns_image *mimg = b->media->bg_image;
        int iw = mimg->loaded && mimg->texture
                 ? ns_texture_get_width(mimg->texture) : 0;
        int ih = mimg->loaded && mimg->texture
                 ? ns_texture_get_height(mimg->texture) : 0;
        if (iw > 0 && ih > 0 && clip_w > 0 && clip_h > 0) {
            double sc = MIN(clip_w / iw, clip_h / ih);
            double draw_w = iw * sc, draw_h = ih * sc;
            double off_x = (clip_w - draw_w) / 2.0;
            double off_y = (clip_h - draw_h) / 2.0;
            cairo_surface_t *surf = texture_surface_cached(mimg->texture, NULL);
            if (surf) {
                cairo_save(cr);
                rounded_rect_path(cr, clip_x, clip_y, clip_w, clip_h, radii);
                cairo_clip(cr);
                set_source_rgba(cr, bg);
                cairo_pattern_t *mp = cairo_pattern_create_for_surface(surf);
                cairo_matrix_t mm;
                cairo_matrix_init_identity(&mm);
                cairo_matrix_scale(&mm, (double)iw / draw_w,
                                   (double)ih / draw_h);
                cairo_matrix_translate(&mm, -(clip_x + off_x),
                                       -(clip_y + off_y));
                cairo_pattern_set_matrix(mp, &mm);
                cairo_mask(cr, mp);
                cairo_pattern_destroy(mp);
                cairo_restore(cr);
                masked_fill = TRUE;
            }
        }
    }

    const ns_css_value *bg_head = s ? s->values[NS_CSS_BACKGROUND_IMAGE] : NULL;
    gboolean bg_has_url = FALSE;
    for (const ns_css_value *l = bg_head; l; l = l->next_layer)
        if (l->kind == NS_CSS_V_URL) { bg_has_url = TRUE; break; }

    if (b->media && b->media->bg_image && !bg_has_url && !masked_fill) {
        paint_bg_image_core(cr, b->media->bg_image,
            s ? s->values[NS_CSS_BACKGROUND_REPEAT] : NULL,
            s ? s->values[NS_CSS_BACKGROUND_SIZE] : NULL,
            s ? s->values[NS_CSS_BACKGROUND_POSITION_X] : NULL,
            s ? s->values[NS_CSS_BACKGROUND_POSITION_Y] : NULL,
            style_pixelated(s),
            pos_x, pos_y, pos_w, pos_h,
            clip_x, clip_y, clip_w, clip_h, radii);
    }

    int n_bg_layers = ns_css_value_layer_count(bg_head);
    for (int li = n_bg_layers - 1; li >= 0; li--) {
        const ns_css_value *lv = ns_css_value_layer(bg_head, li);
        if (lv->kind == NS_CSS_V_GRADIENT) {
            paint_bg_gradient_core(cr, &lv->u.gradient,
                border_x, border_y, border_w, border_h,
                clip_x, clip_y, clip_w, clip_h, radii);
            continue;
        }
        if (lv->kind != NS_CSS_V_URL) continue;
        ns_image *img = NULL;
        if (b->media && b->media->bg_layer_images &&
            li < (int)b->media->bg_layer_images->len)
            img = g_ptr_array_index(b->media->bg_layer_images, li);
        else if (b->media)
            img = b->media->bg_image;
        if (!img) continue;
        paint_bg_image_core(cr, img,
            ns_css_value_layer(s->values[NS_CSS_BACKGROUND_REPEAT], li),
            ns_css_value_layer(s->values[NS_CSS_BACKGROUND_SIZE], li),
            ns_css_value_layer(s->values[NS_CSS_BACKGROUND_POSITION_X], li),
            ns_css_value_layer(s->values[NS_CSS_BACKGROUND_POSITION_Y], li),
            style_pixelated(s),
            pos_x, pos_y, pos_w, pos_h,
            clip_x, clip_y, clip_w, clip_h, radii);
    }

    if (s && s->values[NS_CSS_BOX_SHADOW] &&
        s->values[NS_CSS_BOX_SHADOW]->kind == NS_CSS_V_SHADOW) {
        const ns_css_shadow_list *sl = &s->values[NS_CSS_BOX_SHADOW]->u.shadow;
        for (int si = sl->n - 1; si >= 0; si--) {
            const ns_css_shadow *sh = &sl->s[si];
            if (!sh->inset) continue;
            cairo_save(cr);
            rounded_rect_path(cr, border_x, border_y, border_w, border_h, radii);
            cairo_clip(cr);
            cairo_set_source_rgba(cr,
                sh->r / 255.0, sh->g / 255.0, sh->b / 255.0, sh->a / 255.0);
            cairo_set_line_width(cr, sh->blur > 0 ? sh->blur : 4);
            cairo_translate(cr, sh->x, sh->y);
            rounded_rect_path(cr, border_x, border_y, border_w, border_h, radii);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    }

    if (s) {
        double uniform_bw = 0;
        rgba uniform_color = {0};
        gboolean drew_uniform = FALSE;
        if (!corner_radii_zero(radii) &&
            style_uniform_solid_border(s, &uniform_bw, &uniform_color)) {
            set_source_rgba(cr, uniform_color);
            cairo_set_line_width(cr, uniform_bw);
            rounded_rect_path(cr,
                              border_x + uniform_bw / 2.0,
                              border_y + uniform_bw / 2.0,
                              border_w - uniform_bw,
                              border_h - uniform_bw,
                              radii);
            cairo_stroke(cr);
            drew_uniform = TRUE;
        }
        const struct {
            double w;
            const ns_css_value *col;
            const ns_css_value *style;
            double x1, y1, x2, y2;
        } sides[4] = {
            { b->border.top,
              s->values[NS_CSS_BORDER_TOP_COLOR],
              s->values[NS_CSS_BORDER_TOP_STYLE],
              border_x, border_y,
              border_x + border_w, border_y },
            { b->border.right,
              s->values[NS_CSS_BORDER_RIGHT_COLOR],
              s->values[NS_CSS_BORDER_RIGHT_STYLE],
              border_x + border_w, border_y,
              border_x + border_w, border_y + border_h },
            { b->border.bottom,
              s->values[NS_CSS_BORDER_BOTTOM_COLOR],
              s->values[NS_CSS_BORDER_BOTTOM_STYLE],
              border_x, border_y + border_h,
              border_x + border_w, border_y + border_h },
            { b->border.left,
              s->values[NS_CSS_BORDER_LEFT_COLOR],
              s->values[NS_CSS_BORDER_LEFT_STYLE],
              border_x, border_y,
              border_x, border_y + border_h },
        };
        for (int i = 0; !drew_uniform && i < 4; i++) {
            if (sides[i].w <= 0) continue;
            const ns_css_value *bs = sides[i].style;
            if (!bs || bs->kind != NS_CSS_V_KEYWORD || !bs->u.keyword ||
                strcmp(bs->u.keyword, "none") == 0 ||
                strcmp(bs->u.keyword, "hidden") == 0)
                continue;
            rgba c = rgba_of(sides[i].col ? sides[i].col
                                          : (s ? s->values[NS_CSS_COLOR] : NULL),
                             0, 0, 0, 1);
            set_source_rgba(cr, c);
            cairo_set_line_width(cr, sides[i].w);
            cairo_save(cr);
            if (strcmp(bs->u.keyword, "dashed") == 0) {
                double dashes[] = { sides[i].w * 3, sides[i].w * 2 };
                cairo_set_dash(cr, dashes, 2, 0);
            } else if (strcmp(bs->u.keyword, "dotted") == 0) {
                double dashes[] = { sides[i].w, sides[i].w };
                cairo_set_dash(cr, dashes, 2, 0);
            }
            double x1 = sides[i].x1, y1 = sides[i].y1;
            double x2 = sides[i].x2, y2 = sides[i].y2;
            if (sides[i].w < 1.5) {
                if (x1 == x2) { x1 = floor(x1) + 0.5; x2 = x1; }
                if (y1 == y2) { y1 = floor(y1) + 0.5; y2 = y1; }
            }
            cairo_move_to(cr, x1, y1);
            cairo_line_to(cr, x2, y2);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
        double ow = length_or(s->values[NS_CSS_OUTLINE_WIDTH], 0);
        const ns_css_value *ostyle = s->values[NS_CSS_OUTLINE_STYLE];
        gboolean ostyle_drawable = ostyle && ostyle->kind == NS_CSS_V_KEYWORD &&
            ostyle->u.keyword && strcmp(ostyle->u.keyword, "none") != 0 &&
            strcmp(ostyle->u.keyword, "hidden") != 0;
        if (ow > 0 && ostyle_drawable) {
            double off = length_or(s->values[NS_CSS_OUTLINE_OFFSET], 0);
            rgba oc = rgba_of(s->values[NS_CSS_OUTLINE_COLOR], 0, 0, 0, 1);
            cairo_save(cr);
            set_source_rgba(cr, oc);
            cairo_set_line_width(cr, ow);
            if (strcmp(ostyle->u.keyword, "dashed") == 0) {
                double dashes[] = { ow * 3, ow * 2 };
                cairo_set_dash(cr, dashes, 2, 0);
            } else if (strcmp(ostyle->u.keyword, "dotted") == 0) {
                double dashes[] = { ow, ow };
                cairo_set_dash(cr, dashes, 2, 0);
            }
            cairo_rectangle(cr,
                border_x - off - ow / 2.0,
                border_y - off - ow / 2.0,
                border_w + (off + ow / 2.0) * 2,
                border_h + (off + ow / 2.0) * 2);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    }
}

static const ns_style *
inherited_style(const ns_box *b)
{
    for (const ns_box *p = b->parent; p; p = p->parent)
        if (p->style) return p->style;
    return NULL;
}

static void
attr_insert_range(PangoAttrList *attrs, PangoAttribute *a,
                  gsize start, gsize len)
{
    if (!a) return;
    a->start_index = (guint)start;
    a->end_index   = (guint)(start + len);
    pango_attr_list_insert(attrs, a);
}

static gsize
find_ci_substring(const char *hay, gsize hay_len,
                  const char *needle, gsize needle_len,
                  gsize start)
{
    if (needle_len == 0 || start >= hay_len) return (gsize)-1;
    for (gsize i = start; i + needle_len <= hay_len; i++) {
        gboolean match = g_search_case_sensitive
            ? (strncmp(hay + i, needle, needle_len) == 0)
            : (g_ascii_strncasecmp(hay + i, needle, needle_len) == 0);
        if (match)
            return i;
    }
    return (gsize)-1;
}

static const char *
nearest_node_attr(const ns_node *n, const char *attr)
{
    for (const ns_node *p = n; p; p = p->parent) {
        if (p->kind != NS_NODE_ELEMENT) continue;
        const char *v = ns_element_get_attr(p, attr);
        if (v && *v) return v;
    }
    return NULL;
}

PangoWrapMode
ns_paint_wrap_mode_for(const ns_style *style)
{
    if (style) {
        const ns_css_value *wb = style->values[NS_CSS_WORD_BREAK];
        if (wb && wb->kind == NS_CSS_V_KEYWORD && wb->u.keyword) {
            if (strcmp(wb->u.keyword, "break-all") == 0)
                return PANGO_WRAP_CHAR;
            if (strcmp(wb->u.keyword, "keep-all") == 0)
                return PANGO_WRAP_WORD;
        }
        const ns_css_value *ow = style->values[NS_CSS_OVERFLOW_WRAP];
        if (ow && ow->kind == NS_CSS_V_KEYWORD && ow->u.keyword) {
            if (strcmp(ow->u.keyword, "normal") == 0)
                return PANGO_WRAP_WORD;
            if (strcmp(ow->u.keyword, "break-word") == 0 ||
                strcmp(ow->u.keyword, "anywhere") == 0)
                return PANGO_WRAP_WORD_CHAR;
        }
    }
    return PANGO_WRAP_WORD;
}

static gboolean
ns_style_is_nowrap(const ns_style *style)
{
    if (!style) return FALSE;
    const ns_css_value *ws = style->values[NS_CSS_WHITE_SPACE];
    return ws && ws->kind == NS_CSS_V_KEYWORD && ws->u.keyword &&
           (strcmp(ws->u.keyword, "nowrap") == 0 ||
            strcmp(ws->u.keyword, "pre") == 0);
}

double
ns_paint_css_line_height_px(const ns_style *s)
{
    if (!s) return -1;
    const ns_css_value *lh = s->values[NS_CSS_LINE_HEIGHT];
    if (!lh || lh->kind != NS_CSS_V_LENGTH) return -1;
    double font_size = length_or(s->values[NS_CSS_FONT_SIZE], 16);
    switch (lh->u.length.unit) {
    case NS_CSS_UNIT_PX:      return lh->u.length.v;
    case NS_CSS_UNIT_NUMBER:
    case NS_CSS_UNIT_EM:      return lh->u.length.v * font_size;
    case NS_CSS_UNIT_PERCENT: return lh->u.length.v / 100.0 * font_size;
    default:                  return -1;
    }
}

void
ns_paint_apply_css_line_spacing(PangoLayout *layout, const ns_style *s)
{
    double lh_px = ns_paint_css_line_height_px(s);
    if (!layout || lh_px <= 0) return;
    PangoContext *ctx = pango_layout_get_context(layout);
    const PangoFontDescription *fd = pango_layout_get_font_description(layout);
    PangoFontMetrics *fm = pango_context_get_metrics(ctx, fd, NULL);
    if (!fm) return;
    double natural = (pango_font_metrics_get_ascent(fm) +
                      pango_font_metrics_get_descent(fm)) / (double)PANGO_SCALE;
    pango_font_metrics_unref(fm);
    if (natural <= 0) return;
    pango_layout_set_line_spacing(layout, (float)(lh_px / natural));
}

void
ns_paint_apply_i18n(PangoLayout *layout, PangoAttrList *attrs,
                    const ns_box *b)
{
    if (!b) return;
    const ns_node  *dn = b->dom;
    const ns_style *st = b->style;
    for (const ns_box *p = b->parent; (!dn || !st) && p; p = p->parent) {
        if (!dn && p->dom)   dn = p->dom;
        if (!st && p->style) st = p->style;
    }
    if (!dn && !st) return;
    if (attrs) {
        gboolean auto_hyphens =
            st && keyword_is(st->values[NS_CSS_HYPHENS], "auto");
        PangoAttribute *ih = pango_attr_insert_hyphens_new(auto_hyphens);
        ih->start_index = 0;
        ih->end_index   = G_MAXUINT;
        pango_attr_list_insert(attrs, ih);
    }
    const char *lang = dn ? nearest_node_attr(dn, "lang") : NULL;
    if (!lang && dn) lang = nearest_node_attr(dn, "xml:lang");
    if (lang && attrs) {
        PangoAttribute *a = pango_attr_language_new(
            pango_language_from_string(lang));
        a->start_index = 0;
        a->end_index   = G_MAXUINT;
        pango_attr_list_insert(attrs, a);
    }
    const char *dir = dn ? nearest_node_attr(dn, "dir") : NULL;
    PangoDirection bd = PANGO_DIRECTION_NEUTRAL;
    if (dir) {
        if (g_ascii_strcasecmp(dir, "rtl") == 0) bd = PANGO_DIRECTION_RTL;
        else if (g_ascii_strcasecmp(dir, "ltr") == 0) bd = PANGO_DIRECTION_LTR;
    }
    if (bd == PANGO_DIRECTION_NEUTRAL && st &&
        keyword_is(st->values[NS_CSS_DIRECTION], "rtl"))
        bd = PANGO_DIRECTION_RTL;
    if (bd != PANGO_DIRECTION_NEUTRAL && layout) {
        pango_layout_set_auto_dir(layout, FALSE);
        pango_context_set_base_dir(pango_layout_get_context(layout), bd);
    }
}

static void
append_font_feature(GString *out, const char *feature)
{
    if (!feature || !*feature) return;
    if (out->len > 0) g_string_append(out, ", ");
    g_string_append(out, feature);
}

static void
append_font_kerning_features(GString *out, const ns_css_value *v)
{
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return;
    if (strcmp(v->u.keyword, "none") == 0)
        append_font_feature(out, "kern=0");
    else if (strcmp(v->u.keyword, "normal") == 0 ||
             strcmp(v->u.keyword, "auto") == 0)
        append_font_feature(out, "kern=1");
}

static void
append_font_ligature_token(GString *out, const char *token)
{
    if (strcmp(token, "none") == 0) {
        append_font_feature(out, "liga=0");
        append_font_feature(out, "clig=0");
        append_font_feature(out, "dlig=0");
        append_font_feature(out, "hlig=0");
        append_font_feature(out, "calt=0");
    } else if (strcmp(token, "normal") == 0) {
        append_font_feature(out, "liga=1");
        append_font_feature(out, "clig=1");
        append_font_feature(out, "dlig=0");
        append_font_feature(out, "hlig=0");
        append_font_feature(out, "calt=1");
    } else if (strcmp(token, "common-ligatures") == 0) {
        append_font_feature(out, "liga=1");
        append_font_feature(out, "clig=1");
    } else if (strcmp(token, "no-common-ligatures") == 0) {
        append_font_feature(out, "liga=0");
        append_font_feature(out, "clig=0");
    } else if (strcmp(token, "discretionary-ligatures") == 0) {
        append_font_feature(out, "dlig=1");
    } else if (strcmp(token, "no-discretionary-ligatures") == 0) {
        append_font_feature(out, "dlig=0");
    } else if (strcmp(token, "historical-ligatures") == 0) {
        append_font_feature(out, "hlig=1");
    } else if (strcmp(token, "no-historical-ligatures") == 0) {
        append_font_feature(out, "hlig=0");
    } else if (strcmp(token, "contextual") == 0) {
        append_font_feature(out, "calt=1");
    } else if (strcmp(token, "no-contextual") == 0) {
        append_font_feature(out, "calt=0");
    }
}

static void
append_font_ligature_features(GString *out, const char *ligatures)
{
    if (!ligatures || !*ligatures) return;
    char **tokens = g_strsplit_set(ligatures, " \t\r\n\f", -1);
    for (int i = 0; tokens[i]; i++) {
        if (*tokens[i])
            append_font_ligature_token(out, tokens[i]);
    }
    g_strfreev(tokens);
}

static const char *
paint_font_feature_skip_ws(const char *p)
{
    while (*p && g_ascii_isspace((unsigned char)*p)) p++;
    return p;
}

static gboolean
paint_font_feature_read_tag(const char **pp, char tag[5])
{
    const char *p = paint_font_feature_skip_ws(*pp);
    if (*p != '"' && *p != '\'') return FALSE;
    char quote = *p++;
    const char *s = p;
    while (*p && *p != quote) p++;
    if (*p != quote || p - s != 4) return FALSE;
    memcpy(tag, s, 4);
    tag[4] = '\0';
    *pp = p + 1;
    return TRUE;
}

static gboolean
paint_font_feature_read_value(const char **pp, int *out)
{
    const char *p = paint_font_feature_skip_ws(*pp);
    *out = 1;
    if (!*p || *p == ',') {
        *pp = p;
        return TRUE;
    }
    if (g_ascii_isalpha((unsigned char)*p)) {
        const char *s = p;
        while (g_ascii_isalpha((unsigned char)*p) || *p == '-') p++;
        char *kw = g_ascii_strdown(s, (gssize)(p - s));
        if (strcmp(kw, "on") == 0) *out = 1;
        else if (strcmp(kw, "off") == 0) *out = 0;
        else {
            g_free(kw);
            return FALSE;
        }
        g_free(kw);
        *pp = paint_font_feature_skip_ws(p);
        return TRUE;
    }
    if (!g_ascii_isdigit((unsigned char)*p)) return FALSE;
    char *endp = NULL;
    gint64 v = g_ascii_strtoll(p, &endp, 10);
    if (!endp || endp == p) return FALSE;
    if (v < 0) v = 0;
    if (v > G_MAXINT) v = G_MAXINT;
    *out = (int)v;
    *pp = paint_font_feature_skip_ws(endp);
    return TRUE;
}

static void
append_font_feature_settings_features(GString *out, const char *settings)
{
    if (!settings || !*settings || strcmp(settings, "normal") == 0) return;
    const char *p = paint_font_feature_skip_ws(settings);
    while (*p) {
        char tag[5];
        int value = 1;
        if (!paint_font_feature_read_tag(&p, tag)) return;
        if (!paint_font_feature_read_value(&p, &value)) return;
        char feature[32];
        g_snprintf(feature, sizeof(feature), "%s=%d", tag, value);
        append_font_feature(out, feature);
        if (*p != ',') break;
        p = paint_font_feature_skip_ws(p + 1);
    }
}

static gboolean
paint_font_variation_read_value(const char **pp, char value[32])
{
    const char *p = paint_font_feature_skip_ws(*pp);
    if (!*p || *p == ',') return FALSE;
    char *endp = NULL;
    double v = g_ascii_strtod(p, &endp);
    if (!endp || endp == p || !isfinite(v)) return FALSE;
    g_ascii_formatd(value, 32, "%.8g", v);
    *pp = paint_font_feature_skip_ws(endp);
    return TRUE;
}

static char *
paint_font_variations_from_css(const char *settings)
{
    if (!settings || !*settings) return NULL;
    if (strcmp(settings, "normal") == 0) return g_strdup("");
    GString *out = g_string_new(NULL);
    const char *p = paint_font_feature_skip_ws(settings);
    while (*p) {
        char tag[5];
        char value[32];
        if (!paint_font_feature_read_tag(&p, tag) ||
            !paint_font_variation_read_value(&p, value)) {
            g_string_free(out, TRUE);
            return NULL;
        }
        if (out->len > 0) g_string_append_c(out, ',');
        g_string_append_printf(out, "%s=%s", tag, value);
        if (*p != ',') break;
        p = paint_font_feature_skip_ws(p + 1);
    }
    if (out->len == 0) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

PangoAttribute *
ns_paint_font_features_attr_from_values(int kerning, const char *ligatures,
                                        const char *settings)
{
    GString *s = g_string_new(NULL);
    if (kerning == 0)
        append_font_feature(s, "kern=0");
    else if (kerning > 0)
        append_font_feature(s, "kern=1");
    append_font_ligature_features(s, ligatures);
    append_font_feature_settings_features(s, settings);
    if (s->len == 0) {
        g_string_free(s, TRUE);
        return NULL;
    }
    char *features = g_string_free(s, FALSE);
    PangoAttribute *a = pango_attr_font_features_new(features);
    g_free(features);
    return a;
}

PangoAttribute *
ns_paint_font_variations_attr_from_values(const char *settings)
{
    char *variations = paint_font_variations_from_css(settings);
    if (!variations) return NULL;
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_variations(desc, variations);
    PangoAttribute *a = pango_attr_font_desc_new(desc);
    pango_font_description_free(desc);
    g_free(variations);
    return a;
}

void
ns_paint_apply_font_features(PangoAttrList *attrs, const ns_style *s,
                             guint start, guint end)
{
    if (!attrs || !s) return;
    GString *features = g_string_new(NULL);
    append_font_kerning_features(features, s->values[NS_CSS_FONT_KERNING]);
    const ns_css_value *lig = s->values[NS_CSS_FONT_VARIANT_LIGATURES];
    if (lig && lig->kind == NS_CSS_V_KEYWORD)
        append_font_ligature_features(features, lig->u.keyword);
    const ns_css_value *settings = s->values[NS_CSS_FONT_FEATURE_SETTINGS];
    if (settings && settings->kind == NS_CSS_V_KEYWORD)
        append_font_feature_settings_features(features, settings->u.keyword);
    if (features->len == 0) {
        g_string_free(features, TRUE);
        return;
    }
    char *str = g_string_free(features, FALSE);
    PangoAttribute *a = pango_attr_font_features_new(str);
    g_free(str);
    a->start_index = start;
    a->end_index = end;
    pango_attr_list_insert(attrs, a);
}

static gboolean
ns_paint_font_available(const char *family)
{
    static GHashTable *avail;
    static guint cached_serial;
    if (!family || !*family) return TRUE;
    PangoFontMap *fm = pango_cairo_font_map_get_default();
    guint serial = fm ? pango_font_map_get_serial(fm) : 0;
    if (!avail || serial != cached_serial) {
        if (avail) g_hash_table_remove_all(avail);
        else avail = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        cached_serial = serial;
        if (fm) {
            PangoFontFamily **fams = NULL;
            int n = 0;
            pango_font_map_list_families(fm, &fams, &n);
            for (int i = 0; i < n; i++) {
                const char *nm = pango_font_family_get_name(fams[i]);
                if (nm) g_hash_table_add(avail, g_ascii_strdown(nm, -1));
            }
            g_free(fams);
        }
    }
    char *lo = g_ascii_strdown(family, -1);
    gboolean has = g_hash_table_contains(avail, lo);
    g_free(lo);
    if (!has) has = ns_font_family_loaded(family);
    return has;
}

static void
ns_paint_font_metrics(const char *family, double size_px, int weight,
                      gboolean italic, ns_css_font_metrics *out)
{
    if (size_px <= 0) return;
    PangoLayout *l = paint_create_layout();
    if (!l) return;
    PangoFontDescription *fd = pango_font_description_new();
    char *pango_family = family ? ns_css_font_family_for_pango(family) : NULL;
    if (pango_family && *pango_family)
        pango_font_description_set_family(fd, pango_family);
    g_free(pango_family);
    if (weight > 0) pango_font_description_set_weight(fd, (PangoWeight)weight);
    if (italic) pango_font_description_set_style(fd, PANGO_STYLE_ITALIC);
    pango_font_description_set_absolute_size(fd, size_px * PANGO_SCALE);
    pango_layout_set_font_description(l, fd);

    PangoRectangle ink;
    pango_layout_set_text(l, "x", -1);
    pango_layout_get_pixel_extents(l, &ink, NULL);
    if (ink.height > 0) out->ex_px = ink.height;

    pango_layout_set_text(l, "H", -1);
    pango_layout_get_pixel_extents(l, &ink, NULL);
    if (ink.height > 0) out->cap_px = ink.height;

    int w = 0;
    pango_layout_set_text(l, "0", -1);
    pango_layout_get_pixel_size(l, &w, NULL);
    if (w > 0) out->ch_px = w;

    w = 0;
    pango_layout_set_text(l, "\xe6\xb0\xb4", -1);
    pango_layout_get_pixel_size(l, &w, NULL);
    if (w > 0) out->ic_px = w;

    pango_font_description_free(fd);
    g_object_unref(l);
}

void
ns_paint_register_font_oracle(void)
{
    ns_css_set_font_available_cb(ns_paint_font_available);
    ns_css_set_font_metrics_cb(ns_paint_font_metrics);
}

void
ns_paint_apply_inline_font(PangoLayout *layout, const ns_style *s)
{
    PangoFontDescription *desc = pango_font_description_new();
    double font_size = length_or(s ? s->values[NS_CSS_FONT_SIZE] : NULL, 16);
    const char *family = "sans-serif";
    const ns_css_value *fam = s ? s->values[NS_CSS_FONT_FAMILY] : NULL;
    if (fam && fam->kind == NS_CSS_V_KEYWORD) family = fam->u.keyword;
    char *pango_family = ns_css_font_family_for_pango(family);
    pango_font_description_set_family(desc, pango_family);
    g_free(pango_family);
    pango_font_description_set_absolute_size(desc, font_size * PANGO_SCALE);
    const ns_css_value *fw = s ? s->values[NS_CSS_FONT_WEIGHT] : NULL;
    int font_weight = ns_css_font_weight_number(fw, -1);
    if (font_weight > 0)
        pango_font_description_set_weight(desc, pango_weight_from_css(font_weight));
    pango_font_description_set_stretch(desc,
        pango_stretch_from_css(ns_css_font_stretch_rank(
            s ? s->values[NS_CSS_FONT_STRETCH] : NULL)));
    if (keyword_is(s ? s->values[NS_CSS_FONT_STYLE] : NULL, "italic"))
        pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
    else if (keyword_is(s ? s->values[NS_CSS_FONT_STYLE] : NULL, "oblique"))
        pango_font_description_set_style(desc, PANGO_STYLE_OBLIQUE);
    char *variations = paint_font_variations_from_css(
        ns_style_keyword(s, NS_CSS_FONT_VARIATION_SETTINGS));
    if (variations) {
        pango_font_description_set_variations(desc, variations);
        g_free(variations);
    }
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    const ns_css_value *tsv = s ? s->values[NS_CSS_TAB_SIZE] : NULL;
    if (tsv && tsv->kind == NS_CSS_V_LENGTH && tsv->u.length.v > 0) {
        double tab_w;
        if (tsv->u.length.unit == NS_CSS_UNIT_NUMBER) {
            PangoContext *ctx = pango_layout_get_context(layout);
            PangoLayout *probe = pango_layout_new(ctx);
            pango_layout_set_font_description(probe,
                pango_layout_get_font_description(layout));
            pango_layout_set_text(probe, " ", 1);
            int sw = 0, sh = 0;
            pango_layout_get_size(probe, &sw, &sh);
            g_object_unref(probe);
            tab_w = tsv->u.length.v * (sw / (double)PANGO_SCALE);
        } else {
            tab_w = tsv->u.length.v;
        }
        if (tab_w > 0) {
            int n = 32;
            PangoTabArray *tabs = pango_tab_array_new(n, TRUE);
            for (int i = 0; i < n; i++)
                pango_tab_array_set_tab(tabs, i, PANGO_TAB_LEFT,
                                        (int)((i + 1) * tab_w + 0.5));
            pango_layout_set_tabs(layout, tabs);
            pango_tab_array_free(tabs);
        }
    }
}

static void
apply_text_align(PangoLayout *layout, const ns_style *s)
{
    const ns_css_value *ta = s ? s->values[NS_CSS_TEXT_ALIGN] : NULL;
    gboolean rtl = pango_context_get_base_dir(
        pango_layout_get_context(layout)) == PANGO_DIRECTION_RTL;
    if (keyword_is(ta, "center"))
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    else if (keyword_is(ta, "right") ||
             (keyword_is(ta, "end") && !rtl) ||
             (keyword_is(ta, "start") && rtl) ||
             (!ta && rtl))
        pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
    else if (keyword_is(ta, "justify"))
        pango_layout_set_justify(layout, TRUE);
    else
        pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
}

static void
apply_nowrap_align_width(PangoLayout *layout, const ns_box *b)
{
    if (pango_layout_get_width(layout) >= 0) return;
    PangoAlignment al = pango_layout_get_alignment(layout);
    if (al == PANGO_ALIGN_LEFT) return;
    int pw, ph;
    pango_layout_get_pixel_size(layout, &pw, &ph);
    if (pw <= b->content_width)
        pango_layout_set_width(layout, (int)(b->content_width * PANGO_SCALE));
}

static void paint_walk(cairo_t *cr, const ns_box *b, const char *highlight);

static gboolean
inline_has_form_controls(const ns_box *b)
{
    if (!b || !b->attrs) return FALSE;
    for (guint i = 0; i < b->attrs->len; i++) {
        const ns_inline_attr *r =
            &g_array_index(b->attrs, ns_inline_attr, i);
        if (r->kind == NS_INLINE_INPUT_FIELD ||
            r->kind == NS_INLINE_INPUT_FIELD_FOCUSED ||
            r->kind == NS_INLINE_BUTTON)
            return TRUE;
    }
    return FALSE;
}

double
ns_paint_inline_y_offset_for_layout(const ns_box *b, PangoLayout *layout)
{
    if (!b || !layout) return 0;
    int ph;
    pango_layout_get_pixel_size(layout, NULL, &ph);
    double y_offset = (b->content_height - (double)ph) * 0.5;
    if (inline_has_form_controls(b)) y_offset = 0;
    if (y_offset < 0 &&
        ns_paint_css_line_height_px(inherited_style(b)) <= 0)
        y_offset = 0;
    return y_offset;
}

static void
apply_first_line_attrs(PangoAttrList *attrs, const ns_style *fl,
                       guint start, guint end)
{
    if (!fl || end <= start) return;
    guint len = end - start;
    const ns_css_value *fs = fl->values[NS_CSS_FONT_SIZE];
    if (fs && fs->kind == NS_CSS_V_LENGTH && fs->u.length.unit == NS_CSS_UNIT_PX)
        attr_insert_range(attrs,
            pango_attr_size_new_absolute((int)(fs->u.length.v * PANGO_SCALE)),
            start, len);
    const ns_css_value *col = fl->values[NS_CSS_COLOR];
    if (col && col->kind == NS_CSS_V_COLOR) {
        attr_insert_range(attrs,
            pango_attr_foreground_new((guint16)(col->u.color.r * 0x101),
                                      (guint16)(col->u.color.g * 0x101),
                                      (guint16)(col->u.color.b * 0x101)),
            start, len);
        if (col->u.color.a < 255)
            attr_insert_range(attrs,
                pango_attr_foreground_alpha_new(
                    col->u.color.a ? (guint16)(col->u.color.a * 0x101) : 1),
                start, len);
    }
    const ns_css_value *bg = fl->values[NS_CSS_BACKGROUND_COLOR];
    if (bg && bg->kind == NS_CSS_V_COLOR)
        attr_insert_range(attrs,
            pango_attr_background_new((guint16)(bg->u.color.r * 0x101),
                                      (guint16)(bg->u.color.g * 0x101),
                                      (guint16)(bg->u.color.b * 0x101)),
            start, len);
    int fw = ns_css_font_weight_number(fl->values[NS_CSS_FONT_WEIGHT], -1);
    if (fw > 0)
        attr_insert_range(attrs, pango_attr_weight_new(pango_weight_from_css(fw)),
                          start, len);
    if (fl->values[NS_CSS_FONT_STRETCH])
        attr_insert_range(attrs,
            pango_attr_stretch_new(pango_stretch_from_css(
                ns_css_font_stretch_rank(fl->values[NS_CSS_FONT_STRETCH]))),
            start, len);
    if (keyword_is(fl->values[NS_CSS_FONT_STYLE], "italic") ||
        keyword_is(fl->values[NS_CSS_FONT_STYLE], "oblique"))
        attr_insert_range(attrs, pango_attr_style_new(PANGO_STYLE_ITALIC),
                          start, len);
    const ns_css_value *ff = fl->values[NS_CSS_FONT_FAMILY];
    if (ff && ff->kind == NS_CSS_V_KEYWORD && ff->u.keyword) {
        char *pf = ns_css_font_family_for_pango(ff->u.keyword);
        attr_insert_range(attrs, pango_attr_family_new(pf), start, len);
        g_free(pf);
    }
    if (keyword_is(fl->values[NS_CSS_FONT_VARIANT], "small-caps"))
        attr_insert_range(attrs, pango_attr_variant_new(PANGO_VARIANT_SMALL_CAPS),
                          start, len);
    ns_paint_apply_font_features(attrs, fl, start, end);
    attr_insert_range(attrs,
        ns_paint_font_variations_attr_from_values(
            ns_style_keyword(fl, NS_CSS_FONT_VARIATION_SETTINGS)),
        start, len);
    const ns_css_value *td = fl->values[NS_CSS_TEXT_DECORATION];
    if (td && td->kind == NS_CSS_V_KEYWORD && td->u.keyword &&
        strstr(td->u.keyword, "underline") && !strstr(td->u.keyword, "none"))
        attr_insert_range(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE),
                          start, len);
}

static const char *
underline_dash_style(const ns_inline_attr *r, const ns_style *s)
{
    const ns_css_value *dv = NULL;
    if (r->style && r->style->values[NS_CSS_TEXT_DECORATION_STYLE])
        dv = r->style->values[NS_CSS_TEXT_DECORATION_STYLE];
    else if (s && s->values[NS_CSS_TEXT_DECORATION_STYLE])
        dv = s->values[NS_CSS_TEXT_DECORATION_STYLE];
    if (!dv || dv->kind != NS_CSS_V_KEYWORD || !dv->u.keyword)
        return NULL;
    if (strcmp(dv->u.keyword, "dotted") == 0 ||
        strcmp(dv->u.keyword, "dashed") == 0)
        return dv->u.keyword;
    return NULL;
}

static void
paint_inline_dashed_decorations(cairo_t *cr, const ns_box *b, PangoLayout *layout,
                               double text_x, double y_origin,
                               const ns_style *s, rgba base)
{
    if (!b->attrs)
        return;
    gboolean any = FALSE;
    for (guint i = 0; i < b->attrs->len; i++) {
        const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, i);
        if ((r->kind == NS_INLINE_UNDERLINE ||
             r->kind == NS_INLINE_STRIKETHROUGH ||
             r->kind == NS_INLINE_OVERLINE) && underline_dash_style(r, s)) {
            any = TRUE;
            break;
        }
    }
    if (!any)
        return;
    PangoLayoutIter *iter = pango_layout_get_iter(layout);
    if (!iter)
        return;
    do {
        PangoLayoutLine *line = pango_layout_iter_get_line_readonly(iter);
        if (!line)
            continue;
        double base_y = y_origin +
            (double)pango_layout_iter_get_baseline(iter) / PANGO_SCALE;
        int line_start = line->start_index;
        int line_end = line->start_index + line->length;
        for (guint i = 0; i < b->attrs->len; i++) {
            const ns_inline_attr *r =
                &g_array_index(b->attrs, ns_inline_attr, i);
            if (r->kind != NS_INLINE_UNDERLINE &&
                r->kind != NS_INLINE_STRIKETHROUGH &&
                r->kind != NS_INLINE_OVERLINE)
                continue;
            const char *kw = underline_dash_style(r, s);
            if (!kw)
                continue;
            gboolean dotted = strcmp(kw, "dotted") == 0;
            int rstart = (int)r->start, rend = (int)(r->start + r->len);
            int seg0 = rstart > line_start ? rstart : line_start;
            int seg1 = rend < line_end ? rend : line_end;
            if (seg0 >= seg1)
                continue;
            int xa = 0, xb = 0;
            pango_layout_line_index_to_x(line, seg0, FALSE, &xa);
            pango_layout_line_index_to_x(line, seg1, FALSE, &xb);
            double x0 = text_x + (double)(xa < xb ? xa : xb) / PANGO_SCALE;
            double x1 = text_x + (double)(xa < xb ? xb : xa) / PANGO_SCALE;
            double em = r->font_size_px > 0 ? r->font_size_px : 16.0;
            double thick = em / 16.0;
            if (thick < 1.0)
                thick = 1.0;
            double uy;
            if (r->kind == NS_INLINE_STRIKETHROUGH)
                uy = base_y - em * 0.28;
            else if (r->kind == NS_INLINE_OVERLINE)
                uy = base_y - em * 0.78;
            else
                uy = base_y + thick * 1.5;
            const ns_css_value *dc =
                r->style ? r->style->values[NS_CSS_TEXT_DECORATION_COLOR] : NULL;
            cairo_save(cr);
            if (dc && dc->kind == NS_CSS_V_COLOR)
                cairo_set_source_rgba(cr, dc->u.color.r / 255.0,
                                      dc->u.color.g / 255.0,
                                      dc->u.color.b / 255.0,
                                      dc->u.color.a / 255.0);
            else
                cairo_set_source_rgba(cr, base.r, base.g, base.b, base.a);
            cairo_set_line_width(cr, thick);
            double on = dotted ? thick : thick * 3.0;
            double off = dotted ? thick * 1.6 : thick * 2.5;
            double dash[2] = { on, off };
            cairo_set_dash(cr, dash, 2, 0);
            cairo_set_line_cap(cr, dotted ? CAIRO_LINE_CAP_ROUND
                                          : CAIRO_LINE_CAP_BUTT);
            cairo_move_to(cr, x0, uy);
            cairo_line_to(cr, x1, uy);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    } while (pango_layout_iter_next_line(iter));
    pango_layout_iter_free(iter);
}

static void
ns_box_blur_a8(unsigned char *data, int w, int h, int stride, int radius)
{
    if (radius < 1 || w <= 0 || h <= 0) return;
    int win = 2 * radius + 1;
    unsigned char *tmp = g_malloc((gsize)stride * h);
    for (int y = 0; y < h; y++) {
        unsigned char *row = data + (gsize)y * stride;
        unsigned char *out = tmp + (gsize)y * stride;
        int sum = 0;
        for (int k = -radius; k <= radius; k++) {
            int xi = k < 0 ? 0 : (k >= w ? w - 1 : k);
            sum += row[xi];
        }
        for (int x = 0; x < w; x++) {
            out[x] = (unsigned char)(sum / win);
            int xo = x - radius; xo = xo < 0 ? 0 : (xo >= w ? w - 1 : xo);
            int xi = x + radius + 1; xi = xi < 0 ? 0 : (xi >= w ? w - 1 : xi);
            sum += row[xi] - row[xo];
        }
    }
    for (int x = 0; x < w; x++) {
        int sum = 0;
        for (int k = -radius; k <= radius; k++) {
            int yi = k < 0 ? 0 : (k >= h ? h - 1 : k);
            sum += tmp[(gsize)yi * stride + x];
        }
        for (int y = 0; y < h; y++) {
            data[(gsize)y * stride + x] = (unsigned char)(sum / win);
            int yo = y - radius; yo = yo < 0 ? 0 : (yo >= h ? h - 1 : yo);
            int yi = y + radius + 1; yi = yi < 0 ? 0 : (yi >= h ? h - 1 : yi);
            sum += tmp[(gsize)yi * stride + x] - tmp[(gsize)yo * stride + x];
        }
    }
    g_free(tmp);
}

static void
paint_text_shadow_layer(cairo_t *cr, PangoLayout *layout, double x, double y,
                        const ns_css_shadow *sh)
{
    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(layout, &lw, &lh);
    if (lw <= 0 || lh <= 0) return;

    int blur = (int)(sh->blur + 0.5);
    if (blur < 0) blur = 0;

    double ds = blur / 3.0;
    if (ds < 1.0) ds = 1.0;
    if (ds > 4.0) ds = 4.0;
    int blur_s = (int)(blur / ds + 0.5);
    if (blur > 0 && blur_s < 1) blur_s = 1;
    int pad = blur_s * 3 + 2;
    int mw = (int)ceil(lw / ds) + 2 * pad;
    int mh = (int)ceil(lh / ds) + 2 * pad;

    if (mw > 4096 || mh > 4096) {
        cairo_save(cr);
        cairo_set_source_rgba(cr, sh->r / 255.0, sh->g / 255.0,
                              sh->b / 255.0, sh->a / 255.0);
        cairo_move_to(cr, x + sh->x, y + sh->y);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
        return;
    }

    cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8, mw, mh);
    if (cairo_surface_status(mask) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(mask);
        return;
    }
    cairo_t *mcr = cairo_create(mask);
    cairo_scale(mcr, 1.0 / ds, 1.0 / ds);
    cairo_move_to(mcr, pad * ds, pad * ds);
    pango_cairo_show_layout(mcr, layout);
    cairo_destroy(mcr);
    cairo_surface_flush(mask);

    if (blur > 0) {
        unsigned char *data = cairo_image_surface_get_data(mask);
        int stride = cairo_image_surface_get_stride(mask);
        ns_box_blur_a8(data, mw, mh, stride, blur_s);
        ns_box_blur_a8(data, mw, mh, stride, blur_s);
        cairo_surface_mark_dirty(mask);
    }

    double ox = x + sh->x - pad * ds;
    double oy = y + sh->y - pad * ds;
    cairo_save(cr);
    cairo_set_source_rgba(cr, sh->r / 255.0, sh->g / 255.0,
                          sh->b / 255.0, sh->a / 255.0);
    if (ds == 1.0) {
        cairo_mask_surface(cr, mask, ox, oy);
    } else {
        cairo_pattern_t *mp = cairo_pattern_create_for_surface(mask);
        cairo_matrix_t m;
        cairo_matrix_init_scale(&m, 1.0 / ds, 1.0 / ds);
        cairo_matrix_translate(&m, -ox, -oy);
        cairo_pattern_set_matrix(mp, &m);
        cairo_mask(cr, mp);
        cairo_pattern_destroy(mp);
    }
    cairo_restore(cr);
    cairo_surface_destroy(mask);
}

static void
spell_underline_range(PangoAttrList *attrs, const char *t,
                      gsize rstart, gsize rend)
{
    const char *p = t + rstart;
    const char *end = t + rend;
    while (p < end) {
        if (!g_unichar_isalpha(g_utf8_get_char(p))) {
            p = g_utf8_next_char(p);
            continue;
        }
        const char *start = p;
        const char *q = p;
        int alpha = 0;
        while (q < end) {
            gunichar c = g_utf8_get_char(q);
            const char *nx = g_utf8_next_char(q);
            if (g_unichar_isalpha(c)) { alpha++; q = nx; continue; }
            if ((c == '\'' || c == 0x2019) && nx < end &&
                g_unichar_isalpha(g_utf8_get_char(nx))) { q = nx; continue; }
            break;
        }
        gsize bstart = (gsize)(start - t);
        gsize blen = (gsize)(q - start);
        if (alpha >= 2 && !ns_spell_word_ok(start, (gssize)blen, NULL)) {
            PangoAttribute *u = pango_attr_underline_new(PANGO_UNDERLINE_ERROR);
            u->start_index = (guint)bstart;
            u->end_index = (guint)(bstart + blen);
            pango_attr_list_insert(attrs, u);
            PangoAttribute *col = pango_attr_underline_color_new(0xffff, 0x1000, 0x1000);
            col->start_index = (guint)bstart;
            col->end_index = (guint)(bstart + blen);
            pango_attr_list_insert(attrs, col);
        }
        p = q;
    }
}

static void
paint_spell_underlines(PangoAttrList *attrs, const ns_box *b)
{
    if (!attrs || !b || !b->text) return;
    if (!ns_spell_available()) return;
    gsize tlen = strlen(b->text);
    gboolean had_range = FALSE;
    if (b->attrs) {
        for (guint i = 0; i < b->attrs->len; i++) {
            const ns_inline_attr *a =
                &g_array_index(b->attrs, ns_inline_attr, i);
            if (a->kind != NS_INLINE_SPELLCHECK) continue;
            gsize s = a->start, e = a->start + a->len;
            if (e > tlen) e = tlen;
            if (s < e) {
                spell_underline_range(attrs, b->text, s, e);
                had_range = TRUE;
            }
        }
    }
    if (had_range) return;
    const ns_node *owner = NULL;
    for (const ns_box *bx = b; bx && !owner; bx = bx->parent)
        owner = bx->dom;
    if (owner && ns_node_spellcheck_host(owner))
        spell_underline_range(attrs, b->text, 0, tlen);
}

void
ns_paint_drop_box_cache(ns_box *box)
{
    if (box && box->paint_layout) {
        g_object_unref(box->paint_layout);
        box->paint_layout = NULL;
    }
}

static PangoLayout *
paint_inline_make_layout(const ns_box *b, const ns_style *s,
                         const char *highlight)
{
    PangoLayout *layout = paint_create_layout();
    ns_paint_apply_inline_font(layout, s);

    if (ns_style_is_nowrap(s) &&
        !keyword_is(s ? s->values[NS_CSS_TEXT_OVERFLOW] : NULL, "ellipsis"))
        pango_layout_set_width(layout, -1);
    else
        pango_layout_set_width(layout, (int)(b->content_width * PANGO_SCALE));
    pango_layout_set_wrap(layout, ns_paint_wrap_mode_for(s));
    if (!(b->inline_atomics && b->inline_atomics->len > 0))
        ns_paint_apply_css_line_spacing(layout, s);
    {
        double ti = ns_text_indent_px(s, b->content_width);
        if (ti > 0)
            pango_layout_set_indent(layout, (int)(ti * PANGO_SCALE));
    }
    if (keyword_is(s ? s->values[NS_CSS_TEXT_OVERFLOW] : NULL, "ellipsis"))
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    {
        const ns_css_value *lc = s ? s->values[NS_CSS_LINE_CLAMP] : NULL;
        if (lc && lc->kind == NS_CSS_V_LENGTH && lc->u.length.v >= 1) {
            pango_layout_set_height(layout, -(int)lc->u.length.v);
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        }
    }
    pango_layout_set_text(layout, b->text, -1);

    PangoAttrList *attrs = pango_attr_list_new();
    ns_paint_apply_i18n(layout, attrs, b);
    ns_paint_apply_font_features(attrs, s, 0, G_MAXUINT);
    ns_inline_apply_atomic_shapes(attrs, b);
    double ls_px = 0, ws_px = 0;
    if (s && s->values[NS_CSS_LETTER_SPACING] &&
        s->values[NS_CSS_LETTER_SPACING]->kind == NS_CSS_V_LENGTH &&
        s->values[NS_CSS_LETTER_SPACING]->u.length.unit == NS_CSS_UNIT_PX)
        ls_px = s->values[NS_CSS_LETTER_SPACING]->u.length.v;
    if (s && s->values[NS_CSS_WORD_SPACING] &&
        s->values[NS_CSS_WORD_SPACING]->kind == NS_CSS_V_LENGTH &&
        s->values[NS_CSS_WORD_SPACING]->u.length.unit == NS_CSS_UNIT_PX)
        ws_px = s->values[NS_CSS_WORD_SPACING]->u.length.v;
    if (ls_px != 0) {
        PangoAttribute *ls = pango_attr_letter_spacing_new(
            (int)(ls_px * PANGO_SCALE));
        ls->start_index = 0;
        ls->end_index = G_MAXUINT;
        pango_attr_list_insert(attrs, ls);
    }
    if (ws_px != 0) {
        int per_space = (int)((ls_px + ws_px) * PANGO_SCALE);
        for (const char *p = b->text; *p; p++) {
            if (*p == ' ') {
                gsize idx = (gsize)(p - b->text);
                PangoAttribute *a = pango_attr_letter_spacing_new(per_space);
                a->start_index = (guint)idx;
                a->end_index = (guint)(idx + 1);
                pango_attr_list_insert(attrs, a);
            }
        }
    }
    if (b->attrs) {
        for (gint ii = (gint)b->attrs->len - 1; ii >= 0; ii--) {
            const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, (guint)ii);
            PangoAttribute *a = NULL;
            switch (r->kind) {
            case NS_INLINE_BOLD:
                a = pango_attr_weight_new(PANGO_WEIGHT_BOLD); break;
            case NS_INLINE_FONT_WEIGHT:
                a = pango_attr_weight_new(pango_weight_from_css(r->font_weight)); break;
            case NS_INLINE_FONT_STRETCH:
                a = pango_attr_stretch_new(
                    pango_stretch_from_css(r->font_stretch)); break;
            case NS_INLINE_FONT_FEATURES:
                a = ns_paint_font_features_attr_from_values(
                    r->font_kerning, r->font_ligatures, r->font_features); break;
            case NS_INLINE_FONT_VARIATIONS:
                a = ns_paint_font_variations_attr_from_values(
                    r->font_variations); break;
            case NS_INLINE_ITALIC:
                a = pango_attr_style_new(PANGO_STYLE_ITALIC); break;
            case NS_INLINE_MONOSPACE:
                a = pango_attr_family_new("monospace"); break;
            case NS_INLINE_UNDERLINE: {
                if (underline_dash_style(r, s))
                    break;
                PangoUnderline ul = PANGO_UNDERLINE_SINGLE;
                if (s && s->values[NS_CSS_TEXT_DECORATION_STYLE] &&
                    s->values[NS_CSS_TEXT_DECORATION_STYLE]->kind == NS_CSS_V_KEYWORD &&
                    s->values[NS_CSS_TEXT_DECORATION_STYLE]->u.keyword) {
                    const char *kw = s->values[NS_CSS_TEXT_DECORATION_STYLE]->u.keyword;
                    if (strcmp(kw, "double") == 0) ul = PANGO_UNDERLINE_DOUBLE;
                    else if (strcmp(kw, "wavy") == 0) ul = PANGO_UNDERLINE_ERROR;
                }
                a = pango_attr_underline_new(ul);
                if (s && s->values[NS_CSS_TEXT_DECORATION_COLOR] &&
                    s->values[NS_CSS_TEXT_DECORATION_COLOR]->kind == NS_CSS_V_COLOR) {
                    const ns_css_value *cv =
                        s->values[NS_CSS_TEXT_DECORATION_COLOR];
                    PangoAttribute *cc = pango_attr_underline_color_new(
                        (guint16)(cv->u.color.r * 0x101),
                        (guint16)(cv->u.color.g * 0x101),
                        (guint16)(cv->u.color.b * 0x101));
                    attr_insert_range(attrs, cc, r->start, r->len);
                }
                break;
            }
            case NS_INLINE_OVERLINE:
                if (underline_dash_style(r, s))
                    break;
                a = pango_attr_overline_new(PANGO_OVERLINE_SINGLE);
                if (s && s->values[NS_CSS_TEXT_DECORATION_COLOR] &&
                    s->values[NS_CSS_TEXT_DECORATION_COLOR]->kind == NS_CSS_V_COLOR) {
                    const ns_css_value *cv =
                        s->values[NS_CSS_TEXT_DECORATION_COLOR];
                    PangoAttribute *cc = pango_attr_overline_color_new(
                        (guint16)(cv->u.color.r * 0x101),
                        (guint16)(cv->u.color.g * 0x101),
                        (guint16)(cv->u.color.b * 0x101));
                    attr_insert_range(attrs, cc, r->start, r->len);
                }
                break;
            case NS_INLINE_STRIKETHROUGH:
                if (underline_dash_style(r, s))
                    break;
                a = pango_attr_strikethrough_new(TRUE);
                if (s && s->values[NS_CSS_TEXT_DECORATION_COLOR] &&
                    s->values[NS_CSS_TEXT_DECORATION_COLOR]->kind == NS_CSS_V_COLOR) {
                    const ns_css_value *cv =
                        s->values[NS_CSS_TEXT_DECORATION_COLOR];
                    PangoAttribute *cc = pango_attr_strikethrough_color_new(
                        (guint16)(cv->u.color.r * 0x101),
                        (guint16)(cv->u.color.g * 0x101),
                        (guint16)(cv->u.color.b * 0x101));
                    attr_insert_range(attrs, cc, r->start, r->len);
                }
                break;
            case NS_INLINE_INPUT_FIELD:
            case NS_INLINE_INPUT_FIELD_FOCUSED:
            case NS_INLINE_BUTTON: {
                gboolean ta = r->dom && r->dom->name &&
                              strcmp(r->dom->name, "textarea") == 0;
                if (!ta)
                    attr_insert_range(attrs,
                        pango_attr_allow_breaks_new(FALSE),
                        r->start, r->len);
                break;
            }
            case NS_INLINE_CHECKBOX:
            case NS_INLINE_CHECKBOX_CHECKED:
            case NS_INLINE_RADIO:
            case NS_INLINE_RADIO_CHECKED:
                attr_insert_range(attrs,
                    pango_attr_foreground_alpha_new(1),
                    r->start, r->len);
                break;
            case NS_INLINE_PROGRESS:
            case NS_INLINE_METER:
                break;
            case NS_INLINE_FONT_SIZE:
                a = pango_attr_size_new_absolute(
                    (int)(r->font_size_px * PANGO_SCALE));
                break;
            case NS_INLINE_COLOR:
                a = pango_attr_foreground_new(
                    (guint16)(r->r * 0x101),
                    (guint16)(r->g * 0x101),
                    (guint16)(r->b * 0x101));
                if (r->a < 255)
                    attr_insert_range(attrs,
                        pango_attr_foreground_alpha_new(
                            r->a ? (guint16)(r->a * 0x101) : 1),
                        r->start, r->len);
                break;
            case NS_INLINE_BG_COLOR:
                a = pango_attr_background_new(
                    (guint16)(r->r * 0x101),
                    (guint16)(r->g * 0x101),
                    (guint16)(r->b * 0x101));
                break;
            case NS_INLINE_FONT_FAMILY:
                if (r->family) {
                    char *pango_family = ns_css_font_family_for_pango(r->family);
                    a = pango_attr_family_new(pango_family);
                    g_free(pango_family);
                }
                break;
            case NS_INLINE_SUPERSCRIPT:
                attr_insert_range(attrs, pango_attr_rise_new(4000),
                                  r->start, r->len);
                a = pango_attr_scale_new(0.75);
                break;
            case NS_INLINE_SUBSCRIPT:
                attr_insert_range(attrs, pango_attr_rise_new(-3000),
                                  r->start, r->len);
                a = pango_attr_scale_new(0.75);
                break;
            case NS_INLINE_SMALL_CAPS:
                a = pango_attr_variant_new(PANGO_VARIANT_SMALL_CAPS);
                break;
            case NS_INLINE_SELECTION:
                attr_insert_range(attrs,
                    pango_attr_background_new(0xb400, 0xd500, 0xfe00),
                    r->start, r->len);
                attr_insert_range(attrs,
                    pango_attr_foreground_new(0x0000, 0x0000, 0x0000),
                    r->start, r->len);
                break;
            case NS_INLINE_CARET:
            case NS_INLINE_ELEMENT:
            case NS_INLINE_SPELLCHECK:
                break;
            }
            attr_insert_range(attrs, a, r->start, r->len);
        }
    }
    if (highlight && *highlight) {
        gsize text_len = strlen(b->text);
        gsize needle_len = strlen(highlight);
        gsize pos = 0;
        gboolean is_active = (b == g_search_active_box);
        guint16 br = 0xffff;
        guint16 bg = is_active ? 0xff00 : 0xee00;
        guint16 bb = is_active ? 0x6600 : 0xb000;
        while ((pos = find_ci_substring(b->text, text_len,
                                        highlight, needle_len, pos)) != (gsize)-1) {
            attr_insert_range(attrs,
                pango_attr_background_new(br, bg, bb),
                pos, needle_len);
            pos += needle_len > 0 ? needle_len : 1;
        }
    }
    if (s && s->first_line && b->parent && b->parent->first_child == b) {
        PangoLayoutLine *line0 = pango_layout_get_line_readonly(layout, 0);
        if (line0 && line0->length > 0)
            apply_first_line_attrs(attrs, s->first_line,
                                   (guint)line0->start_index,
                                   (guint)(line0->start_index + line0->length));
    }
    paint_spell_underlines(attrs, b);
    ns_inline_layout_set_attrs(layout, attrs, b);
    pango_attr_list_unref(attrs);

    apply_text_align(layout, s);
    apply_nowrap_align_width(layout, b);
    const ns_css_value *ta = s ? s->values[NS_CSS_TEXT_ALIGN] : NULL;
    if (keyword_is(ta, "justify"))
        pango_layout_set_justify(layout, TRUE);
    return layout;
}

static void
paint_inline(cairo_t *cr, const ns_box *b, const char *highlight)
{
    if (!b->text || !*b->text) return;
    const ns_style *s = inherited_style(b);
    rgba color = rgba_anim(b, NS_CSS_ANIM_TARGET_COLOR,
                           s ? s->values[NS_CSS_COLOR] : NULL,
                           0.07, 0.07, 0.07, 1);

    if (b->vertical_wm) {
        if (b->text_orient == 1) {
            char *stacked = ns_vertical_stack_text(b->text);
            PangoLayout *layout = paint_inline_make_layout(b, s, highlight);
            pango_layout_set_attributes(layout, NULL);
            pango_layout_set_width(layout, -1);
            pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
            pango_layout_set_text(layout, stacked, -1);
            g_free(stacked);
            cairo_save(cr);
            set_source_rgba(cr, color);
            cairo_move_to(cr, b->x, b->y);
            pango_cairo_show_layout(cr, layout);
            cairo_restore(cr);
            g_object_unref(layout);
            return;
        }
        PangoLayout *layout = paint_inline_make_layout(b, s, highlight);
        pango_layout_set_width(layout, -1);
        set_source_rgba(cr, color);
        PangoLayoutIter *it = pango_layout_get_iter(layout);
        double acc = 0;
        do {
            PangoLayoutLine *line = pango_layout_iter_get_line_readonly(it);
            PangoRectangle logical;
            pango_layout_iter_get_line_extents(it, NULL, &logical);
            int baseline = pango_layout_iter_get_baseline(it);
            double line_h = (double)logical.height / PANGO_SCALE;
            double ascent = (double)(baseline - logical.y) / PANGO_SCALE;
            double col_left = (b->vertical_wm == 2)
                ? b->x + acc
                : b->x + b->content_width - acc - line_h;
            cairo_save(cr);
            cairo_translate(cr, col_left + line_h, b->y);
            cairo_rotate(cr, G_PI / 2.0);
            cairo_move_to(cr, 0, ascent);
            pango_cairo_show_layout_line(cr, line);
            cairo_restore(cr);
            acc += line_h;
        } while (pango_layout_iter_next_line(it));
        pango_layout_iter_free(it);
        g_object_unref(layout);
        return;
    }

    double text_x = b->x;
    {
        double ti = ns_text_indent_px(s, b->content_width);
        if (ti < 0) text_x += ti;
    }
    gboolean layout_cacheable = !(highlight && *highlight);
    PangoLayout *layout = (layout_cacheable && b->paint_layout)
        ? (PangoLayout *)g_object_ref(b->paint_layout)
        : paint_inline_make_layout(b, s, highlight);
    if (layout_cacheable && !b->paint_layout)
        ((ns_box *)b)->paint_layout = (PangoLayout *)g_object_ref(layout);
    double y_offset = ns_paint_inline_y_offset_for_layout(b, layout);
    double y_origin = b->y + y_offset;

    if (b->attrs) {
        double opt_minx = 1e9, opt_maxx = -1e9, opt_miny = 1e9, opt_maxy = -1e9;
        int opt_count = 0, opt_nsel = 0;
        struct { double x0, y0, x1, y1; } opt_sel[64];
        for (guint i = 0; i < b->attrs->len; i++) {
            const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, i);
            if (r->kind != NS_INLINE_INPUT_FIELD &&
                r->kind != NS_INLINE_INPUT_FIELD_FOCUSED &&
                r->kind != NS_INLINE_BUTTON)
                continue;
            PangoRectangle r0, r1;
            pango_layout_index_to_pos(layout, (int)r->start, &r0);
            pango_layout_index_to_pos(layout,
                (int)(r->len > 0 ? r->start + r->len - 1 : r->start), &r1);
            if (r->dom && r->dom->name && strcmp(r->dom->name, "option") == 0) {
                double rx0 = text_x + (double)r0.x / PANGO_SCALE;
                double rx1 = text_x + (double)(r1.x + r1.width) / PANGO_SCALE;
                double ry0 = y_origin + (double)r0.y / PANGO_SCALE;
                double ry1 = y_origin + (double)(r0.y + r0.height) / PANGO_SCALE;
                if (rx0 < opt_minx) opt_minx = rx0;
                if (rx1 > opt_maxx) opt_maxx = rx1;
                if (ry0 < opt_miny) opt_miny = ry0;
                if (ry1 > opt_maxy) opt_maxy = ry1;
                opt_count++;
                if (ns_element_get_attr(r->dom, "selected") && opt_nsel < 64) {
                    opt_sel[opt_nsel].x0 = rx0; opt_sel[opt_nsel].y0 = ry0;
                    opt_sel[opt_nsel].x1 = rx1; opt_sel[opt_nsel].y1 = ry1;
                    opt_nsel++;
                }
                continue;
            }
            double bleed_x = r->box_w > 0 || r->box_h > 0 ? 0 : 10;
            double bleed_y = r->box_w > 0 || r->box_h > 0 ? 0 : 5;
            double x0 = text_x + (double)r0.x / PANGO_SCALE - bleed_x;
            double y0 = y_origin + (double)r0.y / PANGO_SCALE - bleed_y;
            double x1 = text_x + (double)(r1.x + r1.width) / PANGO_SCALE + bleed_x;
            double y1 = y_origin + (double)(r0.y + r0.height) / PANGO_SCALE + bleed_y;
            double css_w = inline_control_css_width(r, b);
            if ((r->kind == NS_INLINE_INPUT_FIELD ||
                 r->kind == NS_INLINE_INPUT_FIELD_FOCUSED) && r->dom) {
                const char *type = ns_element_get_attr(r->dom, "type");
                gboolean text_like = !type || !*type ||
                    g_ascii_strcasecmp(type, "text") == 0 ||
                    g_ascii_strcasecmp(type, "search") == 0 ||
                    g_ascii_strcasecmp(type, "email") == 0 ||
                    g_ascii_strcasecmp(type, "url") == 0 ||
                    g_ascii_strcasecmp(type, "tel") == 0 ||
                    g_ascii_strcasecmp(type, "number") == 0 ||
                    g_ascii_strcasecmp(type, "password") == 0;
                if (text_like && r->box_w <= 0) {
                    const char *sz = ns_element_get_attr(r->dom, "size");
                    int n = sz ? ns_parse_int(sz, 20, 4, 80) : 20;
                    PangoContext *pctx = pango_layout_get_context(layout);
                    const PangoFontDescription *fd =
                        pango_layout_get_font_description(layout);
                    if (!fd) fd = pango_context_get_font_description(pctx);
                    PangoFontMetrics *fm =
                        pango_context_get_metrics(pctx, fd, NULL);
                    int aw = pango_font_metrics_get_approximate_char_width(fm);
                    pango_font_metrics_unref(fm);
                    double cell = (double)aw / PANGO_SCALE;
                    double want_w = cell * (double)n + 20.0;
                    double cur_w = x1 - x0;
                    if (want_w > cur_w) x1 = x0 + want_w;
                }
            }
            gboolean is_textarea = r->dom && r->dom->name &&
                                   strcmp(r->dom->name, "textarea") == 0;
            if (css_w <= 0 && r->kind == NS_INLINE_BUTTON) {
                double mnw = inline_control_css_min_width(r, b);
                if (mnw > 0 && x1 - x0 < mnw) {
                    double cx = (x0 + x1) / 2.0;
                    x0 = cx - mnw / 2.0;
                    x1 = cx + mnw / 2.0;
                }
            }
            if (css_w > 0) {
                x0 = text_x + (double)r0.x / PANGO_SCALE;
                x1 = x0 + css_w;
            } else if ((r->kind == NS_INLINE_INPUT_FIELD ||
                        r->kind == NS_INLINE_INPUT_FIELD_FOCUSED) &&
                       b->content_width > 0 && b->parent &&
                       b->parent->dom == r->dom) {
                gsize tlen = b->text ? strlen(b->text) : 0;
                if (r->start <= 3 && r->start + r->len >= tlen) {
                    double fill_x1 = text_x + b->content_width;
                    if (fill_x1 > x1) x1 = fill_x1;
                }
            }
            if (r->box_h > 0) {
                if (is_textarea) {
                    y1 = y0 + r->box_h;
                    double text_bottom = y_origin +
                        (double)(r1.y + r1.height) / PANGO_SCALE + 3.0;
                    if (text_bottom > y1) y1 = text_bottom;
                } else {
                    double cy = (y0 + y1) / 2.0;
                    y0 = cy - r->box_h / 2.0;
                    y1 = cy + r->box_h / 2.0;
                }
            }
            if (x1 < x0) { double t = x0; x0 = x1; x1 = t; }
            const ns_box *field_box = NULL;
            for (const ns_box *p = b; p; p = p->parent)
                if (p->dom) { field_box = p; break; }
            gboolean block_chrome = field_box &&
                                    field_box->dom == r->dom &&
                                    style_has_inline_box_paint(r->style);
            gboolean draw_native_chrome = r->native_chrome && !block_chrome;
            if (r->kind == NS_INLINE_BUTTON && r->dom &&
                ns_element_get_attr(r->dom, "class") &&
                r->box_w <= 0 && r->box_h <= 0)
                draw_native_chrome = FALSE;
            if (!draw_native_chrome && !block_chrome)
                paint_inline_css_chrome(cr, r, x0, y0, x1 - x0, y1 - y0);
            if (draw_native_chrome) {
                cairo_save(cr);
                if (r->kind == NS_INLINE_BUTTON)
                    cairo_set_source_rgb(cr, 0.902, 0.902, 0.902);
                else
                    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
                cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
                cairo_fill(cr);
                cairo_set_source_rgb(cr, 0.722, 0.722, 0.722);
                cairo_set_line_width(cr, 1.0);
                cairo_rectangle(cr, x0 + 0.5, y0 + 0.5,
                                x1 - x0 - 1, y1 - y0 - 1);
                cairo_stroke(cr);
                cairo_restore(cr);
            }
            if (r->kind == NS_INLINE_INPUT_FIELD_FOCUSED &&
                draw_native_chrome) {
                cairo_save(cr);
                cairo_set_source_rgb(cr, 0.13, 0.36, 0.80);
                cairo_set_line_width(cr, 2.0);
                cairo_rectangle(cr, x0 + 0.5, y0 + 0.5,
                                x1 - x0 - 1, y1 - y0 - 1);
                cairo_stroke(cr);
                cairo_restore(cr);
            }
        }
        if (opt_count > 0 && opt_maxx > opt_minx) {
            double px = opt_minx - 6.0;
            double pw = (opt_maxx - opt_minx) + 12.0;
            double py = opt_miny;
            double ph = opt_maxy - opt_miny;
            corner_radii pr = { 3, 3, 3, 3 };
            cairo_save(cr);
            rounded_rect_path(cr, px + 0.5, py + 1.5, pw, ph, pr);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
            cairo_fill(cr);
            rounded_rect_path(cr, px, py, pw, ph, pr);
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            cairo_fill(cr);
            for (int k = 0; k < opt_nsel; k++) {
                cairo_rectangle(cr, px, opt_sel[k].y0, pw,
                                opt_sel[k].y1 - opt_sel[k].y0);
                cairo_set_source_rgb(cr, 0.816, 0.886, 0.988);
                cairo_fill(cr);
            }
            rounded_rect_path(cr, px + 0.5, py + 0.5, pw - 1, ph - 1, pr);
            cairo_set_source_rgb(cr, 0.70, 0.72, 0.75);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    }

    if (s && s->values[NS_CSS_TEXT_SHADOW] &&
        s->values[NS_CSS_TEXT_SHADOW]->kind == NS_CSS_V_SHADOW) {
        const ns_css_shadow_list *sl = &s->values[NS_CSS_TEXT_SHADOW]->u.shadow;
        for (int si = sl->n - 1; si >= 0; si--)
            paint_text_shadow_layer(cr, layout, text_x, y_origin, &sl->s[si]);
    }

    if (g_dbg_paint_x >= 0 && b->text) {
        double px0 = b->x, py0 = b->y;
        double px1 = b->x + b->content_width, py1 = b->y + b->content_height;
        cairo_user_to_device(cr, &px0, &py0);
        cairo_user_to_device(cr, &px1, &py1);
        if (g_dbg_paint_x >= px0 && g_dbg_paint_x <= px1 &&
            g_dbg_paint_y >= py0 && g_dbg_paint_y <= py1) {
            double cx0, cy0, cx1, cy1;
            cairo_clip_extents(cr, &cx0, &cy0, &cx1, &cy1);
            g_printerr("[paint-at] TEXT \"%.30s\" rgba(%.2f,%.2f,%.2f,%.2f) "
                       "at %.0f,%.0f clip=%.0f,%.0f..%.0f,%.0f grp=%d\n",
                       b->text, color.r, color.g, color.b, color.a,
                       text_x, y_origin, cx0, cy0, cx1, cy1,
                       cairo_get_group_target(cr) != cairo_get_target(cr));
        }
    }
    cairo_save(cr);
    set_source_rgba(cr, color);
    cairo_move_to(cr, text_x, y_origin);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    paint_inline_dashed_decorations(cr, b, layout, text_x, y_origin, s, color);

    if (b->attrs) {
        for (guint i = 0; i < b->attrs->len; i++) {
            const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, i);
            if (r->kind != NS_INLINE_CARET) continue;
            if (!g_caret_visible) continue;
            if (b->text && r->start >= strlen(b->text)) continue;
            PangoRectangle pos;
            pango_layout_index_to_pos(layout, (int)r->start, &pos);
            double cx = text_x + (double)pos.x / PANGO_SCALE;
            double cy = y_origin + (double)pos.y / PANGO_SCALE;
            double ch = (double)pos.height / PANGO_SCALE;
            if (ch < 1.0) ch = 14.0;
            cairo_save(cr);
            const ns_style *cstyle = s;
            for (guint j = 0; j < b->attrs->len; j++) {
                const ns_inline_attr *f = &g_array_index(b->attrs, ns_inline_attr, j);
                if ((f->kind == NS_INLINE_INPUT_FIELD ||
                     f->kind == NS_INLINE_INPUT_FIELD_FOCUSED) && f->style &&
                    f->start <= r->start && r->start <= f->start + f->len) {
                    cstyle = f->style;
                    break;
                }
            }
            const ns_css_value *cc = cstyle ? cstyle->values[NS_CSS_CARET_COLOR] : NULL;
            const ns_css_value *tc = cstyle ? cstyle->values[NS_CSS_COLOR] : NULL;
            if (cc && cc->kind == NS_CSS_V_COLOR)
                cairo_set_source_rgb(cr, cc->u.color.r / 255.0,
                                     cc->u.color.g / 255.0, cc->u.color.b / 255.0);
            else if (tc && tc->kind == NS_CSS_V_COLOR)
                cairo_set_source_rgb(cr, tc->u.color.r / 255.0,
                                     tc->u.color.g / 255.0, tc->u.color.b / 255.0);
            else
                cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
            cairo_set_line_width(cr, 1.5);
            cairo_move_to(cr, cx + 0.5, cy);
            cairo_line_to(cr, cx + 0.5, cy + ch);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    }

    if (b->attrs) {
        double font_size = length_or(s ? s->values[NS_CSS_FONT_SIZE] : NULL, 16);
        const ns_css_value *ac = s ? s->values[NS_CSS_ACCENT_COLOR] : NULL;
        rgba accent = rgba_of(
            (ac && ac->kind == NS_CSS_V_COLOR) ? ac : NULL,
            0.13, 0.36, 0.80, 1);
        for (guint i = 0; i < b->attrs->len; i++) {
            const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, i);
            if (r->kind != NS_INLINE_CHECKBOX &&
                r->kind != NS_INLINE_CHECKBOX_CHECKED &&
                r->kind != NS_INLINE_RADIO &&
                r->kind != NS_INLINE_RADIO_CHECKED)
                continue;
            PangoRectangle r0, r1;
            pango_layout_index_to_pos(layout, (int)r->start, &r0);
            pango_layout_index_to_pos(layout,
                (int)(r->len > 0 ? r->start + r->len - 1 : r->start), &r1);
            double gx0 = text_x + (double)r0.x / PANGO_SCALE;
            double gy0 = y_origin + (double)r0.y / PANGO_SCALE;
            double gx1 = text_x + (double)(r1.x + r1.width) / PANGO_SCALE;
            double gy1 = y_origin + (double)(r0.y + r0.height) / PANGO_SCALE;
            if (gx1 < gx0) { double t = gx0; gx0 = gx1; gx1 = t; }
            double side = font_size * 0.82;
            if (r->box_w > 0 || r->box_h > 0) {
                double bw = r->box_w > 0 ? r->box_w : r->box_h;
                double bh = r->box_h > 0 ? r->box_h : r->box_w;
                side = bw < bh ? bw : bh;
            }
            double bx = gx0 + ((gx1 - gx0) - side) / 2.0;
            double by = gy0 + ((gy1 - gy0) - side) / 2.0;
            gboolean radio = (r->kind == NS_INLINE_RADIO ||
                              r->kind == NS_INLINE_RADIO_CHECKED);
            gboolean checked = (r->kind == NS_INLINE_CHECKBOX_CHECKED ||
                                r->kind == NS_INLINE_RADIO_CHECKED);
            cairo_save(cr);
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            if (radio) {
                cairo_new_sub_path(cr);
                cairo_arc(cr, bx + side / 2.0, by + side / 2.0,
                          side / 2.0, 0, 2 * G_PI);
            } else {
                cairo_rectangle(cr, bx, by, side, side);
            }
            cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
            if (checked) {
                cairo_set_source_rgba(cr, accent.r, accent.g, accent.b, accent.a);
                if (radio) {
                    double rdot = side * 0.30;
                    cairo_new_sub_path(cr);
                    cairo_arc(cr, bx + side / 2.0, by + side / 2.0,
                              rdot, 0, 2 * G_PI);
                    cairo_fill(cr);
                } else {
                    cairo_rectangle(cr, bx, by, side, side);
                    cairo_fill(cr);
                    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
                    cairo_set_line_width(cr, side * 0.18);
                    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                    cairo_move_to(cr, bx + side * 0.20, by + side * 0.55);
                    cairo_line_to(cr, bx + side * 0.42, by + side * 0.78);
                    cairo_line_to(cr, bx + side * 0.80, by + side * 0.28);
                    cairo_stroke(cr);
                }
            }
            cairo_restore(cr);
        }
    }

    if (b->attrs) {
        const ns_css_value *ac = s ? s->values[NS_CSS_ACCENT_COLOR] : NULL;
        rgba accent = rgba_of(
            (ac && ac->kind == NS_CSS_V_COLOR) ? ac : NULL,
            0.13, 0.36, 0.80, 1);
        for (guint i = 0; i < b->attrs->len; i++) {
            const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, i);
            if (r->kind != NS_INLINE_PROGRESS &&
                r->kind != NS_INLINE_METER) continue;
            PangoRectangle r0, r1;
            pango_layout_index_to_pos(layout, (int)r->start, &r0);
            pango_layout_index_to_pos(layout,
                (int)(r->len > 0 ? r->start + r->len - 1 : r->start), &r1);
            double gx0 = text_x + (double)r0.x / PANGO_SCALE;
            double gy0 = y_origin + (double)r0.y / PANGO_SCALE;
            double gx1 = text_x + (double)(r1.x + r1.width) / PANGO_SCALE;
            double gy1 = y_origin + (double)(r0.y + r0.height) / PANGO_SCALE;
            if (gx1 < gx0) { double t = gx0; gx0 = gx1; gx1 = t; }
            double pad_x = 2;
            double bx = gx0 + pad_x;
            double bw = gx1 - gx0 - pad_x * 2;
            if (bw < 4) bw = 4;
            double bh = (gy1 - gy0) * 0.55;
            if (bh < 6) bh = 6;
            double by = gy0 + ((gy1 - gy0) - bh) / 2.0;
            double radius = bh / 2.0;
            cairo_save(cr);
            cairo_new_sub_path(cr);
            cairo_arc(cr, bx + radius,      by + radius, radius,  G_PI / 2,  3 * G_PI / 2);
            cairo_arc(cr, bx + bw - radius, by + radius, radius, -G_PI / 2,      G_PI / 2);
            cairo_close_path(cr);
            cairo_set_source_rgb(cr, 0.88, 0.88, 0.90);
            cairo_fill_preserve(cr);
            cairo_clip(cr);
            double frac = r->font_size_px;
            if (frac > 1) frac = 1;
            rgba fill = accent;
            if (r->kind == NS_INLINE_METER && r->a)
                fill = (rgba){ r->r / 255.0, r->g / 255.0,
                               r->b / 255.0, r->a / 255.0 };
            cairo_set_source_rgba(cr, fill.r, fill.g, fill.b, fill.a);
            if (r->kind == NS_INLINE_PROGRESS && frac < 0) {
                double iw = bw * 0.35;
                double ix = bx + (bw - iw) / 2.0;
                cairo_rectangle(cr, ix, by, iw, bh);
            } else {
                if (frac < 0) frac = 0;
                cairo_rectangle(cr, bx, by, bw * frac, bh);
            }
            cairo_fill(cr);
            cairo_restore(cr);
        }
    }

    if (b->inline_atomics) {
        for (guint i = 0; i < b->inline_atomics->len; i++) {
            const ns_inline_atomic *a =
                &g_array_index(b->inline_atomics, ns_inline_atomic, i);
            if (!a->box) continue;
            PangoRectangle pos;
            pango_layout_index_to_pos(layout, (int)a->byte_off, &pos);
            double sx = text_x + (double)pos.x / PANGO_SCALE;
            double sy = b->y + (double)pos.y / PANGO_SCALE;
            cairo_save(cr);
            cairo_translate(cr, sx - a->box->x, sy - a->box->y);
            g_paint_no_cull++;
            const ns_box *saved_flush = g_paint_flush_box;
            g_paint_flush_box = a->box;
            paint_walk(cr, a->box, highlight);
            g_paint_flush_box = saved_flush;
            g_paint_no_cull--;
            cairo_restore(cr);
        }
    }

    g_object_unref(layout);
}

PangoLayout *
ns_paint_build_inline_layout(cairo_t *cr, const ns_box *b)
{
    (void)cr;
    if (!b || !b->text) return NULL;
    const ns_style *s = inherited_style(b);

    PangoLayout *layout = paint_create_layout();
    ns_paint_apply_inline_font(layout, s);
    if (ns_style_is_nowrap(s) &&
        !keyword_is(s ? s->values[NS_CSS_TEXT_OVERFLOW] : NULL, "ellipsis"))
        pango_layout_set_width(layout, -1);
    else
        pango_layout_set_width(layout, (int)(b->content_width * PANGO_SCALE));
    pango_layout_set_wrap(layout, ns_paint_wrap_mode_for(s));
    if (!(b->inline_atomics && b->inline_atomics->len > 0))
        ns_paint_apply_css_line_spacing(layout, s);
    {
        double ti = ns_text_indent_px(s, b->content_width);
        if (ti > 0) pango_layout_set_indent(layout, (int)(ti * PANGO_SCALE));
    }
    if (keyword_is(s ? s->values[NS_CSS_TEXT_OVERFLOW] : NULL, "ellipsis"))
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    {
        const ns_css_value *lc = s ? s->values[NS_CSS_LINE_CLAMP] : NULL;
        if (lc && lc->kind == NS_CSS_V_LENGTH && lc->u.length.v >= 1) {
            pango_layout_set_height(layout, -(int)lc->u.length.v);
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        }
    }
    pango_layout_set_text(layout, b->text, -1);

    PangoAttrList *attrs = pango_attr_list_new();
    ns_paint_apply_i18n(layout, attrs, b);
    ns_paint_apply_font_features(attrs, s, 0, G_MAXUINT);
    ns_inline_apply_atomic_shapes(attrs, b);
    if (b->attrs) {
        for (gint ii = (gint)b->attrs->len - 1; ii >= 0; ii--) {
            const ns_inline_attr *r = &g_array_index(b->attrs, ns_inline_attr, (guint)ii);
            PangoAttribute *a = NULL;
            switch (r->kind) {
            case NS_INLINE_BOLD:      a = pango_attr_weight_new(PANGO_WEIGHT_BOLD); break;
            case NS_INLINE_FONT_WEIGHT:
                a = pango_attr_weight_new(pango_weight_from_css(r->font_weight)); break;
            case NS_INLINE_FONT_STRETCH:
                a = pango_attr_stretch_new(
                    pango_stretch_from_css(r->font_stretch)); break;
            case NS_INLINE_FONT_FEATURES:
                a = ns_paint_font_features_attr_from_values(
                    r->font_kerning, r->font_ligatures, r->font_features); break;
            case NS_INLINE_FONT_VARIATIONS:
                a = ns_paint_font_variations_attr_from_values(
                    r->font_variations); break;
            case NS_INLINE_ITALIC:    a = pango_attr_style_new(PANGO_STYLE_ITALIC); break;
            case NS_INLINE_MONOSPACE: a = pango_attr_family_new("monospace"); break;
            case NS_INLINE_FONT_SIZE:
                a = pango_attr_size_new_absolute((int)(r->font_size_px * PANGO_SCALE));
                break;
            case NS_INLINE_FONT_FAMILY:
                if (r->family) {
                    char *pango_family = ns_css_font_family_for_pango(r->family);
                    a = pango_attr_family_new(pango_family);
                    g_free(pango_family);
                }
                break;
            case NS_INLINE_SUPERSCRIPT:
            case NS_INLINE_SUBSCRIPT:
                a = pango_attr_scale_new(0.75); break;
            case NS_INLINE_SMALL_CAPS:
                a = pango_attr_variant_new(PANGO_VARIANT_SMALL_CAPS); break;
            default: break;
            }
            attr_insert_range(attrs, a, r->start, r->len);
        }
    }
    ns_inline_layout_set_attrs(layout, attrs, b);
    pango_attr_list_unref(attrs);

    apply_text_align(layout, s);
    apply_nowrap_align_width(layout, b);
    return layout;
}

gboolean
ns_paint_inline_xy_to_byte(const ns_box *b, double rel_x, double rel_y,
                           gsize *out_byte)
{
    if (!b || !b->text || !*b->text) return FALSE;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t *cr = cairo_create(surf);
    PangoLayout *layout = ns_paint_build_inline_layout(cr, b);
    if (!layout) {
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
        return FALSE;
    }

    int index = 0, trailing = 0;
    double y_offset = ns_paint_inline_y_offset_for_layout(b, layout);
    double layout_y = rel_y - y_offset;
    if (layout_y < 0) layout_y = 0;
    pango_layout_xy_to_index(layout, (int)(rel_x * PANGO_SCALE),
                             (int)(layout_y * PANGO_SCALE),
                             &index, &trailing);
    if (out_byte) {
        gsize tlen = strlen(b->text);
        gsize bi = (gsize)index <= tlen ? (gsize)index : tlen;
        const char *p = g_utf8_offset_to_pointer(b->text + bi, trailing);
        gsize off = (gsize)(p - b->text);
        *out_byte = off <= tlen ? off : tlen;
    }

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return TRUE;
}

gboolean
ns_paint_inline_range_extents(const ns_box *b, gsize start, gsize len,
                              double *out_x, double *out_y,
                              double *out_w, double *out_h)
{
    if (!b || !b->text || !*b->text || len == 0) return FALSE;
    gsize text_len = strlen(b->text);
    if (start >= text_len) return FALSE;
    if (start + len > text_len) len = text_len - start;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t *cr = cairo_create(surf);
    PangoLayout *layout = ns_paint_build_inline_layout(cr, b);
    if (!layout) {
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
        return FALSE;
    }
    double y_offset = ns_paint_inline_y_offset_for_layout(b, layout);
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    gboolean any = FALSE;
    PangoLayoutIter *it = pango_layout_get_iter(layout);
    do {
        PangoLayoutLine *line = pango_layout_iter_get_line_readonly(it);
        gsize line_start = (gsize)line->start_index;
        gsize line_end = line_start + (gsize)line->length;
        gsize lo = start > line_start ? start : line_start;
        gsize hi = start + len < line_end ? start + len : line_end;
        if (lo >= hi) continue;
        PangoRectangle p0, p1, lrect;
        pango_layout_index_to_pos(layout, (int)lo, &p0);
        int hi_idx = (int)hi - 1;
        if (hi_idx < (int)lo) hi_idx = (int)lo;
        pango_layout_index_to_pos(layout, hi_idx, &p1);
        if (p0.width < 0) { p0.x += p0.width; p0.width = -p0.width; }
        if (p1.width < 0) { p1.x += p1.width; p1.width = -p1.width; }
        pango_layout_iter_get_line_extents(it, NULL, &lrect);
        double seg_x0 = (double)MIN(p0.x, p1.x) / PANGO_SCALE;
        double seg_x1 = (double)MAX(p0.x + p0.width, p1.x + p1.width)
                        / PANGO_SCALE;
        double seg_y0 = (double)lrect.y / PANGO_SCALE;
        double seg_y1 = (double)(lrect.y + lrect.height) / PANGO_SCALE;
        if (!any) {
            x0 = seg_x0; x1 = seg_x1; y0 = seg_y0; y1 = seg_y1;
            any = TRUE;
        } else {
            if (seg_x0 < x0) x0 = seg_x0;
            if (seg_x1 > x1) x1 = seg_x1;
            if (seg_y0 < y0) y0 = seg_y0;
            if (seg_y1 > y1) y1 = seg_y1;
        }
    } while (pango_layout_iter_next_line(it));
    pango_layout_iter_free(it);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    if (!any) return FALSE;
    if (out_x) *out_x = x0;
    if (out_y) *out_y = y0 + y_offset;
    if (out_w) *out_w = x1 - x0;
    if (out_h) *out_h = y1 - y0;
    return TRUE;
}

static double
parse_filter_amount(const char *p, const char **out_end)
{
    while (*p && (*p == ' ' || *p == '\t')) p++;
    char *endp = NULL;
    double v = g_ascii_strtod(p, &endp);
    if (!endp || endp == p) {
        if (out_end) *out_end = p;
        return -1;
    }
    if (*endp == '%') {
        v /= 100.0;
        endp++;
    }
    if (out_end) *out_end = endp;
    return v;
}

static int
clamp_i(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void
box_blur_argb(guchar *data, int stride, int w, int h, int radius)
{
    if (radius < 1 || w < 1 || h < 1) return;
    int win = radius * 2 + 1;
    guchar *tmp = g_malloc0((gsize)stride * h);
    for (int y = 0; y < h; y++) {
        guchar *s = data + y * stride;
        guchar *d = tmp + y * stride;
        for (int c = 0; c < 4; c++) {
            int sum = 0;
            for (int i = -radius; i <= radius; i++)
                sum += s[clamp_i(i, 0, w - 1) * 4 + c];
            for (int x = 0; x < w; x++) {
                d[x * 4 + c] = (guchar)(sum / win);
                sum += s[clamp_i(x + radius + 1, 0, w - 1) * 4 + c]
                     - s[clamp_i(x - radius, 0, w - 1) * 4 + c];
            }
        }
    }
    for (int x = 0; x < w; x++) {
        for (int c = 0; c < 4; c++) {
            int sum = 0;
            for (int i = -radius; i <= radius; i++)
                sum += tmp[clamp_i(i, 0, h - 1) * stride + x * 4 + c];
            for (int y = 0; y < h; y++) {
                data[y * stride + x * 4 + c] = (guchar)(sum / win);
                sum += tmp[clamp_i(y + radius + 1, 0, h - 1) * stride + x * 4 + c]
                     - tmp[clamp_i(y - radius, 0, h - 1) * stride + x * 4 + c];
            }
        }
    }
    g_free(tmp);
}

typedef struct image_drop_shadow {
    double x, y, blur;
    rgba color;
} image_drop_shadow;

static gboolean
filter_name_is(const char *name, gsize nlen, const char *want)
{
    gsize want_len = strlen(want);
    return nlen == want_len &&
           g_ascii_strncasecmp(name, want, want_len) == 0;
}

static const char *
filter_function_next(const char *p, const char **name_out, gsize *name_len_out,
                     const char **body_out, const char **body_end_out)
{
    while (*p && g_ascii_isspace((unsigned char)*p)) p++;
    if (!*p) return NULL;
    const char *name = p;
    while (*p && (g_ascii_isalpha((unsigned char)*p) || *p == '-')) p++;
    gsize name_len = (gsize)(p - name);
    while (*p && g_ascii_isspace((unsigned char)*p)) p++;
    if (!name_len || *p != '(') return NULL;
    p++;
    const char *body = p;
    int depth = 1;
    while (*p) {
        if (*p == '(') {
            depth++;
        } else if (*p == ')') {
            depth--;
            if (depth == 0) {
                if (name_out) *name_out = name;
                if (name_len_out) *name_len_out = name_len;
                if (body_out) *body_out = body;
                if (body_end_out) *body_end_out = p;
                return p + 1;
            }
        }
        p++;
    }
    return NULL;
}

static gboolean
filter_has_bitmap_effect(const char *filter)
{
    if (!filter) return FALSE;
    const char *p = filter;
    while (*p) {
        const char *name = NULL;
        gsize nlen = 0;
        const char *next = filter_function_next(p, &name, &nlen, NULL, NULL);
        if (!next) break;
        if (filter_name_is(name, nlen, "grayscale") ||
            filter_name_is(name, nlen, "sepia") ||
            filter_name_is(name, nlen, "invert") ||
            filter_name_is(name, nlen, "brightness") ||
            filter_name_is(name, nlen, "contrast") ||
            filter_name_is(name, nlen, "saturate") ||
            filter_name_is(name, nlen, "blur"))
            return TRUE;
        p = next;
    }
    return FALSE;
}

static gboolean
parse_filter_length_px(const char *s, double *out)
{
    while (*s && g_ascii_isspace((unsigned char)*s)) s++;
    char *endp = NULL;
    double v = g_ascii_strtod(s, &endp);
    if (!endp || endp == s) return FALSE;
    while (*endp && g_ascii_isspace((unsigned char)*endp)) endp++;
    if (*endp == '\0' || g_ascii_strcasecmp(endp, "px") == 0) {
        *out = v;
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "em") == 0 ||
        g_ascii_strcasecmp(endp, "rem") == 0 ||
        g_ascii_strcasecmp(endp, "lh") == 0 ||
        g_ascii_strcasecmp(endp, "rlh") == 0) {
        *out = v * 16.0;
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "pt") == 0) {
        *out = v * (96.0 / 72.0);
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "pc") == 0) {
        *out = v * 16.0;
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "cm") == 0) {
        *out = v * (96.0 / 2.54);
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "mm") == 0) {
        *out = v * (96.0 / 25.4);
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "q") == 0) {
        *out = v * (96.0 / 101.6);
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "in") == 0) {
        *out = v * 96.0;
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "vw") == 0 ||
        g_ascii_strcasecmp(endp, "dvw") == 0 ||
        g_ascii_strcasecmp(endp, "svw") == 0 ||
        g_ascii_strcasecmp(endp, "lvw") == 0) {
        *out = v * ns_css_viewport_w() / 100.0;
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "vh") == 0 ||
        g_ascii_strcasecmp(endp, "dvh") == 0 ||
        g_ascii_strcasecmp(endp, "svh") == 0 ||
        g_ascii_strcasecmp(endp, "lvh") == 0) {
        *out = v * ns_css_viewport_h() / 100.0;
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "vmin") == 0 ||
        g_ascii_strcasecmp(endp, "dvmin") == 0 ||
        g_ascii_strcasecmp(endp, "svmin") == 0 ||
        g_ascii_strcasecmp(endp, "lvmin") == 0) {
        *out = v * MIN(ns_css_viewport_w(), ns_css_viewport_h()) / 100.0;
        return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "vmax") == 0 ||
        g_ascii_strcasecmp(endp, "dvmax") == 0 ||
        g_ascii_strcasecmp(endp, "svmax") == 0 ||
        g_ascii_strcasecmp(endp, "lvmax") == 0) {
        *out = v * MAX(ns_css_viewport_w(), ns_css_viewport_h()) / 100.0;
        return TRUE;
    }
    return FALSE;
}

static int
filter_split_ws(const char *start, const char *end, char *tokens[], int max)
{
    int n = 0;
    int depth = 0;
    const char *tok = NULL;
    for (const char *p = start; p <= end; p++) {
        gboolean done = p == end;
        char c = done ? '\0' : *p;
        if (!done && !tok && !g_ascii_isspace((unsigned char)c))
            tok = p;
        if (!done && c == '(') depth++;
        else if (!done && c == ')' && depth > 0) depth--;
        if ((done || (g_ascii_isspace((unsigned char)c) && depth == 0)) && tok) {
            if (n < max)
                tokens[n++] = g_strndup(tok, (gsize)(p - tok));
            tok = NULL;
        }
    }
    return n;
}

static gboolean
parse_filter_shadow_body(const char *body, const char *body_end,
                         rgba current_color, image_drop_shadow *out)
{
    char *tokens[8] = {0};
    double lengths[3] = {0};
    int n_lengths = 0;
    rgba color = current_color;
    int n = filter_split_ws(body, body_end, tokens, G_N_ELEMENTS(tokens));
    gboolean ok = TRUE;
    for (int i = 0; i < n; i++) {
        double len = 0;
        guint8 r, g, b, a;
        char *token = g_strstrip(tokens[i]);
        if (parse_filter_length_px(token, &len)) {
            if (n_lengths < 3) lengths[n_lengths++] = len;
            else ok = FALSE;
        } else if (g_ascii_strcasecmp(token, "currentcolor") == 0) {
            color = current_color;
        } else if (ns_css_parse_color(token, &r, &g, &b, &a)) {
            color.r = r / 255.0;
            color.g = g / 255.0;
            color.b = b / 255.0;
            color.a = a / 255.0;
        } else {
            ok = FALSE;
        }
    }
    for (int i = 0; i < n; i++) g_free(tokens[i]);
    if (!ok || n_lengths < 2) return FALSE;
    out->x = lengths[0];
    out->y = lengths[1];
    out->blur = n_lengths >= 3 && lengths[2] > 0 ? lengths[2] : 0;
    out->color = color;
    return TRUE;
}

static int
parse_filter_drop_shadows(const char *filter, rgba current_color,
                          image_drop_shadow shadows[], int max)
{
    if (!filter || max <= 0) return 0;
    int n = 0;
    const char *p = filter;
    while (*p && n < max) {
        const char *name = NULL;
        const char *body = NULL;
        const char *body_end = NULL;
        gsize nlen = 0;
        const char *next =
            filter_function_next(p, &name, &nlen, &body, &body_end);
        if (!next) break;
        if (filter_name_is(name, nlen, "drop-shadow") &&
            parse_filter_shadow_body(body, body_end, current_color, &shadows[n]))
            n++;
        p = next;
    }
    return n;
}

static cairo_surface_t *
drop_shadow_surface(cairo_surface_t *src, int iw, int ih, int cw, int ch,
                    double ox, double oy, double sx, double sy,
                    image_drop_shadow shadow, int *pad_out)
{
    if (cw <= 0 || ch <= 0 || sx <= 0 || sy <= 0) return NULL;
    if (cw > 4096) cw = 4096;
    if (ch > 4096) ch = 4096;
    int radius = shadow.blur > 0 ? (int)(shadow.blur + 0.5) : 0;
    if (radius > 512) radius = 512;
    int pad = radius * 2 + 2;
    int sw = cw + pad * 2;
    int sh = ch + pad * 2;
    cairo_surface_t *shadow_surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
    if (cairo_surface_status(shadow_surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(shadow_surf);
        return NULL;
    }

    cairo_t *s_cr = cairo_create(shadow_surf);
    cairo_set_operator(s_cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(s_cr);
    cairo_set_operator(s_cr, CAIRO_OPERATOR_OVER);
    cairo_rectangle(s_cr, pad, pad, cw, ch);
    cairo_clip(s_cr);
    cairo_translate(s_cr, pad + ox, pad + oy);
    cairo_scale(s_cr, sx, sy);
    cairo_set_source_surface(s_cr, src, 0, 0);
    cairo_paint(s_cr);
    cairo_destroy(s_cr);

    cairo_surface_flush(shadow_surf);
    guchar *data = cairo_image_surface_get_data(shadow_surf);
    int stride = cairo_image_surface_get_stride(shadow_surf);
    for (int y = 0; y < sh; y++) {
        guchar *row = data + y * stride;
        for (int x = 0; x < sw; x++) {
            guchar *px = row + x * 4;
            double a = px[3] / 255.0 * shadow.color.a;
            px[0] = (guchar)(shadow.color.b * a * 255.0 + 0.5);
            px[1] = (guchar)(shadow.color.g * a * 255.0 + 0.5);
            px[2] = (guchar)(shadow.color.r * a * 255.0 + 0.5);
            px[3] = (guchar)(a * 255.0 + 0.5);
        }
    }
    if (radius > 0)
        box_blur_argb(data, stride, sw, sh, radius);
    cairo_surface_mark_dirty(shadow_surf);
    if (pad_out) *pad_out = pad;
    (void)iw;
    (void)ih;
    return shadow_surf;
}

static void
paint_texture_drop_shadows(cairo_t *cr, cairo_surface_t *surf, const ns_box *b,
                           int iw, int ih, double sx, double sy,
                           double ox, double oy, const char *filter_kw)
{
    const ns_style *st = b ? b->style : NULL;
    rgba current = rgba_of(st ? st->values[NS_CSS_COLOR] : NULL, 0, 0, 0, 1);
    image_drop_shadow shadows[4];
    int n = parse_filter_drop_shadows(filter_kw, current, shadows,
                                      G_N_ELEMENTS(shadows));
    if (n <= 0) return;
    int cw = MAX(1, (int)ceil(b->content_width));
    int ch = MAX(1, (int)ceil(b->content_height));
    for (int i = 0; i < n; i++) {
        int pad = 0;
        cairo_surface_t *shadow =
            drop_shadow_surface(surf, iw, ih, cw, ch, ox, oy, sx, sy,
                                shadows[i], &pad);
        if (!shadow) continue;
        cairo_save(cr);
        cairo_set_source_surface(cr, shadow,
                                 b->x + shadows[i].x - pad,
                                 b->y + shadows[i].y - pad);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(shadow);
    }
}

static void
apply_image_filter(guchar *data, int stride, int w, int h, const char *filter)
{
    if (!filter || !*filter) return;
    typedef struct { int op; double amount; } fop;
    fop ops[16];
    int n_ops = 0;
    double blur_radius = 0;
    const char *q = filter;
    while (*q && n_ops < 16) {
        int op = 0;
        const char *name = NULL;
        const char *body = NULL;
        gsize nlen = 0;
        const char *next = filter_function_next(q, &name, &nlen, &body, NULL);
        if (!next) break;
        double amt = parse_filter_amount(body, NULL);
        q = next;
        if (filter_name_is(name, nlen, "grayscale")) op = 1;
        else if (filter_name_is(name, nlen, "sepia")) op = 2;
        else if (filter_name_is(name, nlen, "invert")) op = 3;
        else if (filter_name_is(name, nlen, "brightness")) op = 4;
        else if (filter_name_is(name, nlen, "contrast")) op = 5;
        else if (filter_name_is(name, nlen, "saturate")) op = 6;
        else if (filter_name_is(name, nlen, "blur")) {
            if (amt >= 0 && amt > blur_radius) blur_radius = amt;
        }
        if (op && amt >= 0) {
            ops[n_ops].op = op;
            ops[n_ops].amount = amt;
            n_ops++;
        }
    }
    if (n_ops == 0 && blur_radius <= 0) return;
    for (int y = 0; y < h; y++) {
        guchar *row = data + y * stride;
        for (int x = 0; x < w; x++) {
            guchar *px = row + x * 4;
            double a = px[3] / 255.0;
            double b = px[0] / 255.0;
            double g = px[1] / 255.0;
            double r = px[2] / 255.0;
            if (a > 0.0001) { r /= a; g /= a; b /= a; }
            for (int oi = 0; oi < n_ops; oi++) {
                double t = ops[oi].amount;
                switch (ops[oi].op) {
                case 1: {
                    double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                    r = r * (1 - t) + lum * t;
                    g = g * (1 - t) + lum * t;
                    b = b * (1 - t) + lum * t;
                    break;
                }
                case 2: {
                    double sr = r * 0.393 + g * 0.769 + b * 0.189;
                    double sg = r * 0.349 + g * 0.686 + b * 0.168;
                    double sb = r * 0.272 + g * 0.534 + b * 0.131;
                    r = r * (1 - t) + sr * t;
                    g = g * (1 - t) + sg * t;
                    b = b * (1 - t) + sb * t;
                    break;
                }
                case 3: {
                    r = r * (1 - t) + (1 - r) * t;
                    g = g * (1 - t) + (1 - g) * t;
                    b = b * (1 - t) + (1 - b) * t;
                    break;
                }
                case 4: r *= t; g *= t; b *= t; break;
                case 5:
                    r = (r - 0.5) * t + 0.5;
                    g = (g - 0.5) * t + 0.5;
                    b = (b - 0.5) * t + 0.5;
                    break;
                case 6: {
                    double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                    r = lum + (r - lum) * t;
                    g = lum + (g - lum) * t;
                    b = lum + (b - lum) * t;
                    break;
                }
                }
            }
            if (r < 0) r = 0;
            if (r > 1) r = 1;
            if (g < 0) g = 0;
            if (g > 1) g = 1;
            if (b < 0) b = 0;
            if (b > 1) b = 1;
            px[0] = (guchar)(b * a * 255.0 + 0.5);
            px[1] = (guchar)(g * a * 255.0 + 0.5);
            px[2] = (guchar)(r * a * 255.0 + 0.5);
        }
    }
    if (blur_radius > 0) {
        int r = (int)(blur_radius + 0.5);
        if (r > 512) r = 512;
        box_blur_argb(data, stride, w, h, r);
    }
}

static gboolean
apply_box_content_clip(cairo_t *cr, const ns_box *b)
{
    if (!b) return FALSE;
    gboolean clipped = FALSE;
    if (isnan(b->x) || isnan(b->y) ||
        isnan(b->content_width) || isnan(b->content_height))
        return FALSE;
    corner_radii radii = box_border_radii(b);
    if (!corner_radii_zero(radii)) {
        rounded_rect_path(cr, b->x, b->y,
                          b->content_width, b->content_height, radii);
        cairo_clip(cr);
        clipped = TRUE;
    }
    const ns_style *st = b->style;
    if (!st || !st->values[NS_CSS_CLIP_PATH]) return clipped;
    if (st->values[NS_CSS_CLIP_PATH]->kind != NS_CSS_V_KEYWORD) return clipped;
    const char *cp = st->values[NS_CSS_CLIP_PATH]->u.keyword;
    if (!cp || !*cp || strcmp(cp, "none") == 0) return clipped;
    double w = b->content_width;
    double h = b->content_height;
    double cx = b->x + w / 2.0;
    double cy = b->y + h / 2.0;
    if (g_ascii_strncasecmp(cp, "circle", 6) == 0) {
        double r = (w < h ? w : h) / 2.0;
        const char *paren = strchr(cp, '(');
        if (paren) {
            paren++;
            while (*paren == ' ' || *paren == '\t') paren++;
            if (*paren && *paren != ')') {
                char *endp = NULL;
                double rv = g_ascii_strtod(paren, &endp);
                if (endp && endp != paren && rv > 0)
                    r = (*endp == '%') ? rv / 100.0 * ((w < h ? w : h) / 2.0) : rv;
            }
        }
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
        cairo_clip(cr);
        clipped = TRUE;
    } else if (g_ascii_strncasecmp(cp, "ellipse", 7) == 0) {
        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_scale(cr, w / 2.0, h / 2.0);
        cairo_arc(cr, 0, 0, 1.0, 0, 2 * G_PI);
        cairo_restore(cr);
        cairo_clip(cr);
        clipped = TRUE;
    } else if (g_ascii_strncasecmp(cp, "polygon", 7) == 0) {
        const char *paren = strchr(cp, '(');
        if (!paren) return clipped;
        paren++;
        const char *end = strrchr(paren, ')');
        if (!end) return clipped;
        char *body = g_strndup(paren, (gsize)(end - paren));
        gchar **verts = g_strsplit(body, ",", -1);
        gboolean first = TRUE;
        cairo_new_sub_path(cr);
        for (int i = 0; verts[i]; i++) {
            char *coords = g_strstrip(verts[i]);
            if (!*coords) continue;
            char *endp1 = NULL;
            double xv = g_ascii_strtod(coords, &endp1);
            if (!endp1 || endp1 == coords) continue;
            gboolean xpct = (*endp1 == '%');
            if (xpct) endp1++;
            while (*endp1 == ' ' || *endp1 == '\t') endp1++;
            char *endp2 = NULL;
            double yv = g_ascii_strtod(endp1, &endp2);
            if (!endp2 || endp2 == endp1) continue;
            gboolean ypct = (*endp2 == '%');
            double px = xpct ? b->x + xv / 100.0 * w : b->x + xv;
            double py = ypct ? b->y + yv / 100.0 * h : b->y + yv;
            if (first) { cairo_move_to(cr, px, py); first = FALSE; }
            else       cairo_line_to(cr, px, py);
        }
        g_strfreev(verts);
        g_free(body);
        if (!first) {
            cairo_close_path(cr);
            cairo_clip(cr);
            clipped = TRUE;
        }
    } else if (g_ascii_strncasecmp(cp, "inset", 5) == 0) {
        const char *paren = strchr(cp, '(');
        if (paren) {
            paren++;
            double pad = 0;
            char *endp = NULL;
            double pv = g_ascii_strtod(paren, &endp);
            if (endp && endp != paren) {
                pad = (*endp == '%') ? pv / 100.0 * ((w < h ? w : h)) : pv;
                cairo_rectangle(cr, b->x + pad, b->y + pad,
                                w - 2 * pad, h - 2 * pad);
                cairo_clip(cr);
                clipped = TRUE;
            }
        }
    }
    return clipped;
}

static cairo_surface_t *
texture_surface_cached(ns_texture *tex, const char *filter_kw)
{
    int iw = ns_texture_get_width(tex);
    int ih = ns_texture_get_height(tex);
    if (iw <= 0 || ih <= 0) return NULL;
    cairo_surface_t *surf = ns_texture_get_user_data(tex);
    if (surf && !filter_kw) return surf;
    surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    guchar *dst = cairo_image_surface_get_data(surf);
    int dst_stride = cairo_image_surface_get_stride(surf);
    ns_texture_download(tex, dst, (gsize)dst_stride);
    if (filter_kw) {
        apply_image_filter(dst, dst_stride, iw, ih, filter_kw);
    }
    cairo_surface_mark_dirty(surf);
    if (!filter_kw)
        ns_texture_set_user_data(tex, surf,
                                 (GDestroyNotify)cairo_surface_destroy);
    return surf;
}

static double
object_position_offset(const ns_style *st, ns_css_prop prop,
                       double box_size, double object_size)
{
    double delta = box_size - object_size;
    const ns_css_value *v = st ? st->values[prop] : NULL;
    if (v && v->kind == NS_CSS_V_LENGTH) {
        if (v->u.length.unit == NS_CSS_UNIT_PERCENT)
            return delta * v->u.length.v / 100.0;
        return bg_size_px(v->u.length.v, v->u.length.unit, box_size);
    }
    return delta * 0.5;
}

static gboolean
paint_texture(cairo_t *cr, const ns_box *b, ns_texture *tex)
{
    int iw = ns_texture_get_width(tex);
    int ih = ns_texture_get_height(tex);
    if (iw <= 0 || ih <= 0) return FALSE;
    if (b->content_width <= 0 || b->content_height <= 0) return FALSE;
    const ns_style *st = b->style;
    const char *filter_kw = NULL;
    if (st && st->values[NS_CSS_FILTER] &&
        st->values[NS_CSS_FILTER]->kind == NS_CSS_V_KEYWORD &&
        st->values[NS_CSS_FILTER]->u.keyword) {
        filter_kw = st->values[NS_CSS_FILTER]->u.keyword;
    }
    const char *surface_filter =
        filter_has_bitmap_effect(filter_kw) ? filter_kw : NULL;
    cairo_surface_t *surf = texture_surface_cached(tex, surface_filter);
    if (!surf) return FALSE;
    double cw = b->content_width, ch = b->content_height;
    double sx = cw / iw, sy = ch / ih;
    const char *fit = (st && st->values[NS_CSS_OBJECT_FIT] &&
                       st->values[NS_CSS_OBJECT_FIT]->kind == NS_CSS_V_KEYWORD)
                      ? st->values[NS_CSS_OBJECT_FIT]->u.keyword : NULL;
    double ox = 0, oy = 0;
    if (fit && strcmp(fit, "fill") != 0) {
        double s;
        if (strcmp(fit, "contain") == 0)         s = MIN(sx, sy);
        else if (strcmp(fit, "cover") == 0)      s = MAX(sx, sy);
        else if (strcmp(fit, "none") == 0)       s = 1.0;
        else if (strcmp(fit, "scale-down") == 0) s = MIN(1.0, MIN(sx, sy));
        else                                     s = -1;
        if (s > 0) {
            sx = sy = s;
            ox = object_position_offset(st, NS_CSS_OBJECT_POSITION_X,
                                        cw, iw * s);
            oy = object_position_offset(st, NS_CSS_OBJECT_POSITION_Y,
                                        ch, ih * s);
        }
    }
    paint_texture_drop_shadows(cr, surf, b, iw, ih, sx, sy, ox, oy, filter_kw);
    apply_box_content_clip(cr, b);
    cairo_rectangle(cr, b->x, b->y, cw, ch);
    cairo_clip(cr);
    cairo_translate(cr, b->x + ox, b->y + oy);
    cairo_scale(cr, sx, sy);
    cairo_set_source_surface(cr, surf, 0, 0);
    ns_video *pv = b->media ? (ns_video *)b->media->video : NULL;
    if (pv && pv->playing && pv->frame_texture == tex)
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
    const ns_css_value *ir = st ? st->values[NS_CSS_IMAGE_RENDERING] : NULL;
    if (ir && ir->kind == NS_CSS_V_KEYWORD && ir->u.keyword &&
        (strcmp(ir->u.keyword, "pixelated") == 0 ||
         strcmp(ir->u.keyword, "crisp-edges") == 0))
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    if (surface_filter) cairo_surface_destroy(surf);
    return TRUE;
}

static void
paint_failed_image(cairo_t *cr, const ns_box *b)
{
    double w = b->content_width;
    double h = b->content_height;
    if (w <= 0 || h <= 0) return;
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, b->x, b->y, w, h);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    double s = MIN(10.0, MIN(w, h) - 2.0);
    if (s < 4.0) return;
    double x = b->x + 3.0;
    double y = b->y + 3.0;
    if (x + s > b->x + w) x = b->x + MAX(0.0, w - s - 1.0);
    if (y + s > b->y + h) y = b->y + MAX(0.0, h - s - 1.0);
    cairo_set_source_rgb(cr, 0.82, 0.0, 0.0);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, x, y);
    cairo_line_to(cr, x + s, y + s);
    cairo_move_to(cr, x + s, y);
    cairo_line_to(cr, x, y + s);
    cairo_stroke(cr);
}

static void
paint_math(cairo_t *cr, const ns_box *b)
{
    if (!b->dom) return;
    const ns_style *s = b->style;
    double fpx = length_or(s ? s->values[NS_CSS_FONT_SIZE] : NULL, 16);
    double r = 0, g = 0, bl = 0, a = 1;
    const ns_css_value *col = s ? s->values[NS_CSS_COLOR] : NULL;
    if (col && col->kind == NS_CSS_V_COLOR) {
        r = col->u.color.r / 255.0;
        g = col->u.color.g / 255.0;
        bl = col->u.color.b / 255.0;
        a = col->u.color.a / 255.0;
    }
    double ox = b->x + b->margin.left + b->border.left + b->padding.left;
    double oy = b->y + b->margin.top + b->border.top + b->padding.top;
    ns_math_paint(cr, b->dom, ox, oy, fpx, r, g, bl, a);
}

static void
paint_image(cairo_t *cr, const ns_box *b)
{
    if (ns_node_is_element_named(b->dom, "canvas")) return;
    const ns_image *img = NULL;
    if (b->dom && g_paint_js)
        img = ns_js_image_for_node(g_paint_js, b->dom);
    if (!img && b->media)
        img = b->media->image;
    cairo_save(cr);
    if (img && img->loaded && img->texture) {
        paint_texture(cr, b, img->texture);
    } else if (img && img->failed) {
        if (b->content_width > 24 || b->content_height > 24)
            paint_failed_image(cr, b);
    } else {
        if (b->content_width <= 24 && b->content_height <= 24) {
            cairo_restore(cr);
            return;
        }
        const ns_style *s = b->style;
        rgba bg = rgba_anim(b, NS_CSS_ANIM_TARGET_BG_COLOR,
                            s ? s->values[NS_CSS_BACKGROUND_COLOR] : NULL,
                            0, 0, 0, 0);
        gboolean has_bg = bg.a > 0;
        if (!has_bg) {
            cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
            cairo_rectangle(cr, b->x, b->y, b->content_width, b->content_height);
            cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        }
        const char *alt = b->dom ? ns_element_get_attr(b->dom, "alt") : NULL;
        if (alt && *alt && b->content_width > 24 && b->content_height > 16) {
            PangoLayout *layout = paint_create_layout();
            pango_layout_set_text(layout, alt, -1);
            pango_layout_set_width(layout,
                (int)((b->content_width - 8) * PANGO_SCALE));
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
            int pw, ph;
            pango_layout_get_pixel_size(layout, &pw, &ph);
            cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
            cairo_move_to(cr,
                          b->x + 4,
                          b->y + (b->content_height - ph) / 2);
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);
        }
    }
    cairo_restore(cr);
}

static void
paint_video_caption(cairo_t *cr, const ns_box *b, const char *text)
{
    char **lines = g_strsplit(text, "\n", -1);
    guint nl = g_strv_length(lines);
    if (nl == 0) { g_strfreev(lines); return; }

    int fs = (int)(b->content_height * 0.07 + 0.5);
    if (fs < 11) fs = 11;
    if (fs > 26) fs = 26;
    PangoFontDescription *fd = pango_font_description_from_string("sans");
    pango_font_description_set_size(fd, fs * PANGO_SCALE);
    pango_font_description_set_weight(fd, PANGO_WEIGHT_MEDIUM);

    PangoLayout **lays = g_new0(PangoLayout *, nl);
    int *lw = g_new0(int, nl);
    int *lh = g_new0(int, nl);
    double pad = 3.0, gap = 1.0, total = 0;
    for (guint i = 0; i < nl; i++) {
        PangoLayout *layout = paint_create_layout();
        pango_layout_set_font_description(layout, fd);
        pango_layout_set_text(layout, lines[i][0] ? lines[i] : " ", -1);
        pango_layout_get_pixel_size(layout, &lw[i], &lh[i]);
        lays[i] = layout;
        total += lh[i] + 2 * pad + (i ? gap : 0);
    }
    double y = b->y + b->content_height - b->content_height * 0.05 - total;
    if (y < b->y + 2) y = b->y + 2;
    cairo_save(cr);
    for (guint i = 0; i < nl; i++) {
        double bw = lw[i] + 2 * pad;
        double bx = b->x + (b->content_width - bw) / 2.0;
        if (bx < b->x) bx = b->x;
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
        cairo_rectangle(cr, bx, y, bw, lh[i] + 2 * pad);
        cairo_fill(cr);
        cairo_move_to(cr, bx + pad, y + pad);
        cairo_set_source_rgb(cr, 1, 1, 1);
        pango_cairo_show_layout(cr, lays[i]);
        y += lh[i] + 2 * pad + gap;
        g_object_unref(lays[i]);
    }
    cairo_restore(cr);
    pango_font_description_free(fd);
    g_free(lays);
    g_free(lw);
    g_free(lh);
    g_strfreev(lines);
}

static void
paint_video(cairo_t *cr, const ns_box *b)
{
    if (b->media && b->media->video_audio_src && !b->media->video_src) {
        double x = b->x, y = b->y, w = b->content_width, h = b->content_height;
        if (!(w > 0) || !(h > 0)) return;
        cairo_save(cr);
        corner_radii radii = { 4, 4, 4, 4 };
        rounded_rect_path(cr, x, y, w, h, radii);
        cairo_set_source_rgb(cr, 0.96, 0.97, 0.98);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.55, 0.58, 0.62);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        double cy = y + h / 2.0;
        double play_x = x + 13.0;
        double play_r = h * 0.28;
        if (play_r > 9) play_r = 9;
        if (play_r < 5) play_r = 5;
        cairo_arc(cr, play_x, cy, play_r, 0, 2 * G_PI);
        cairo_set_source_rgb(cr, 0.20, 0.23, 0.26);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_move_to(cr, play_x - play_r * 0.28, cy - play_r * 0.45);
        cairo_line_to(cr, play_x + play_r * 0.45, cy);
        cairo_line_to(cr, play_x - play_r * 0.28, cy + play_r * 0.45);
        cairo_close_path(cr);
        cairo_fill(cr);

        const char *dur = b->dom ? ns_element_get_attr(b->dom, "data-durationhint") : NULL;
        char dtext[32] = "";
        if (dur && *dur) {
            char *end = NULL;
            double sec_d = g_ascii_strtod(dur, &end);
            if (end != dur && sec_d >= 0) {
                int sec = (int)(sec_d + 0.5);
                g_snprintf(dtext, sizeof dtext, "%d:%02d", sec / 60, sec % 60);
            }
        }
        double text_w = 0;
        if (dtext[0]) {
            PangoLayout *layout = paint_create_layout();
            PangoFontDescription *fd = pango_font_description_from_string("sans 9");
            pango_layout_set_font_description(layout, fd);
            pango_layout_set_text(layout, dtext, -1);
            int tw = 0, th = 0;
            pango_layout_get_pixel_size(layout, &tw, &th);
            text_w = tw + 10;
            cairo_move_to(cr, x + w - tw - 8, y + (h - th) / 2.0);
            cairo_set_source_rgb(cr, 0.18, 0.20, 0.23);
            pango_cairo_show_layout(cr, layout);
            pango_font_description_free(fd);
            g_object_unref(layout);
        }

        double tx0 = x + 31.0;
        double tx1 = x + w - (dtext[0] ? text_w + 8 : 12);
        if (tx1 > tx0 + 12) {
            cairo_set_source_rgb(cr, 0.72, 0.74, 0.77);
            cairo_set_line_width(cr, 3.0);
            cairo_move_to(cr, tx0, cy);
            cairo_line_to(cr, tx1, cy);
            cairo_stroke(cr);
            cairo_arc(cr, tx0, cy, 3.5, 0, 2 * G_PI);
            cairo_set_source_rgb(cr, 0.20, 0.23, 0.26);
            cairo_fill(cr);
        }
        cairo_restore(cr);
        return;
    }
    ns_video *v = b->media ? b->media->video : NULL;
    ns_texture *tex = v ? (v->frame_texture ? v->frame_texture
                                            : v->poster_texture)
                        : NULL;
    if (v) {
        double dx0 = b->x, dy0 = b->y;
        double dx1 = b->x + b->content_width;
        double dy1 = b->y + b->content_height;
        cairo_user_to_device(cr, &dx0, &dy0);
        cairo_user_to_device(cr, &dx1, &dy1);
        ns_video_note_paint_rect(v, dx0, dy0, dx1 - dx0, dy1 - dy0);
    }
    ns_image *bgimg = b->media ? b->media->bg_image : NULL;
    gboolean bg_painted = bgimg && bgimg->loaded && bgimg->texture;
    if (!bg_painted && b->media && b->media->bg_layer_images) {
        for (guint li = 0; li < b->media->bg_layer_images->len; li++) {
            ns_image *limg = g_ptr_array_index(b->media->bg_layer_images, li);
            if (limg && limg->loaded && limg->texture) {
                bg_painted = TRUE;
                break;
            }
        }
    }
    gboolean punched = ns_video_helper_composited(v);
    if (v && g_getenv("NS_DBG_COMPOSITE")) {
        static gint64 plast;
        gint64 pn = g_get_monotonic_time();
        if (pn - plast > 1000000) {
            plast = pn;
            g_printerr("[hole] punched=%d opened=%d tex=%d box=%.0f,%.0f %.0fx%.0f\n",
                       punched, v->video_opened ? 1 : 0, tex ? 1 : 0,
                       b->x, b->y, b->content_width, b->content_height);
        }
    }
    cairo_save(cr);
    if (punched) {
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_rectangle(cr, b->x, b->y, b->content_width, b->content_height);
        cairo_fill(cr);
    } else if (tex) {
        paint_texture(cr, b, tex);
    } else if (!bg_painted) {
        gboolean ambient = b->dom &&
            ns_element_get_attr(b->dom, "autoplay") &&
            ns_element_get_attr(b->dom, "muted") &&
            !ns_element_get_attr(b->dom, "controls");
        if (!ambient) {
            cairo_set_source_rgb(cr, 0.10, 0.10, 0.10);
            cairo_rectangle(cr, b->x, b->y,
                            b->content_width, b->content_height);
            cairo_fill(cr);
        }
    }
    cairo_restore(cr);

    const char *cue = ns_video_active_cue_text(v);
    if (cue && *cue && b->content_width > 24 && b->content_height > 24)
        paint_video_caption(cr, b, cue);
}

static void
roman_numeral(int n, gboolean upper, char *out, gsize sz)
{
    static const int vals[]  = {1000,900,500,400,100,90,50,40,10,9,5,4,1};
    static const char *upr[] = {"M","CM","D","CD","C","XC","L","XL","X","IX","V","IV","I"};
    static const char *lwr[] = {"m","cm","d","cd","c","xc","l","xl","x","ix","v","iv","i"};
    GString *s = g_string_new(NULL);
    if (n < 1 || n > 3999) {
        g_snprintf(out, sz, "%d", n);
        g_string_free(s, TRUE);
        return;
    }
    for (int i = 0; i < (int)(sizeof vals / sizeof vals[0]); i++) {
        while (n >= vals[i]) {
            g_string_append(s, upper ? upr[i] : lwr[i]);
            n -= vals[i];
        }
    }
    g_strlcpy(out, s->str, sz);
    g_string_free(s, TRUE);
}

static void
alpha_label(int n, gboolean upper, char *out, gsize sz)
{
    if (sz == 0) return;
    if (n < 1) { g_strlcpy(out, "?", sz); return; }
    char buf[16];
    int p = 0;
    while (n > 0 && p < (int)sizeof buf - 1) {
        n--;
        buf[p++] = (char)((upper ? 'A' : 'a') + (n % 26));
        n /= 26;
    }
    buf[p] = '\0';
    int len = p < (int)sz - 1 ? p : (int)sz - 1;
    for (int i = 0; i < len; i++) out[i] = buf[p - 1 - i];
    out[len] = '\0';
}

static const char *
ordered_marker_kind(const char *style_kw)
{
    static const char *const kinds[] = {
        "decimal",
        "decimal-leading-zero",
        "upper-alpha", "lower-alpha",
        "upper-latin", "lower-latin",
        "upper-roman", "lower-roman",
        "lower-greek",
    };
    if (!style_kw) return NULL;
    for (size_t i = 0; i < G_N_ELEMENTS(kinds); i++)
        if (strcmp(style_kw, kinds[i]) == 0) return kinds[i];
    return NULL;
}

static const char *
ordered_kind_from_type_attr(const char *type_attr)
{
    if (!type_attr || !*type_attr) return NULL;
    switch (type_attr[0]) {
    case 'A': return "upper-alpha";
    case 'a': return "lower-alpha";
    case 'I': return "upper-roman";
    case 'i': return "lower-roman";
    default:  return "decimal";
    }
}

static void format_ordered_label(const char *kind, int n,
                                 char *out, gsize out_sz);

static int
list_item_count(const ns_node *parent)
{
    int total = 0;
    for (const ns_node *p = parent ? parent->first_child : NULL;
         p; p = p->next_sibling) {
        if (ns_node_is_element_named(p, "li")) total++;
    }
    return total;
}

static int
list_item_ordinal(const ns_node *li)
{
    const ns_node *parent = li ? li->parent : NULL;
    if (!parent || !parent->name) return 1;
    const char *start_attr = ns_element_get_attr(parent, "start");
    int start = start_attr
        ? ns_parse_int(start_attr, 1, -1000000, 1000000)
        : 1;
    gboolean reversed = ns_element_get_attr(parent, "reversed") != NULL;
    int current = reversed && !start_attr ? list_item_count(parent) : start;
    for (const ns_node *p = parent->first_child; p; p = p->next_sibling) {
        if (!ns_node_is_element_named(p, "li")) continue;
        const char *val = ns_element_get_attr(p, "value");
        int ordinal = val ? ns_parse_int(val, current, -1000000, 1000000)
                          : current;
        if (p == li) return ordinal;
        current = ordinal + (reversed ? -1 : 1);
    }
    return current;
}

static const char *
marker_default_kind(const ns_node *li, const char *style_kw)
{
    if (style_kw && ordered_marker_kind(style_kw)) return style_kw;
    const ns_node *parent = li ? li->parent : NULL;
    if (parent && parent->name && strcmp(parent->name, "ol") == 0)
        return ordered_kind_from_type_attr(ns_element_get_attr(parent, "type"));
    return style_kw;
}

static void
marker_append_escaped_string(GString *out, const char *start, gsize len)
{
    char *raw = g_strndup(start, len);
    for (const char *r = raw; *r; )
        ns_css_append_unescaped(out, &r);
    g_free(raw);
}

static char *
marker_counter_text(const char *body, const ns_node *li, const char *style_kw)
{
    const char *p = body;
    while (*p && g_ascii_isspace(*p)) p++;
    const char *name_s = p;
    while (*p && *p != ',' && *p != ')' && !g_ascii_isspace(*p)) p++;
    if ((gsize)(p - name_s) != 9 ||
        g_ascii_strncasecmp(name_s, "list-item", 9) != 0)
        return g_strdup("");
    while (*p && g_ascii_isspace(*p)) p++;
    char *style = NULL;
    if (*p == ',') {
        p++;
        while (*p && g_ascii_isspace(*p)) p++;
        const char *style_s = p;
        while (*p && *p != ')' && !g_ascii_isspace(*p)) p++;
        if (p != style_s) style = g_ascii_strdown(style_s, p - style_s);
    }
    char buf[32];
    format_ordered_label(style ? style : marker_default_kind(li, style_kw),
                         list_item_ordinal(li), buf, sizeof buf);
    g_free(style);
    return g_strdup(buf);
}

static char *
marker_resolve_content_text(const char *raw, const ns_node *li,
                            const char *style_kw)
{
    gboolean has_func = strchr(raw, '(') != NULL;
    gboolean has_string = strchr(raw, '"') || strchr(raw, '\'');
    if (!has_func && !has_string) return g_strdup(raw);
    GString *out = g_string_new(NULL);
    const char *p = raw;
    while (*p) {
        while (*p && g_ascii_isspace(*p)) p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            const char *start = p;
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) { p += 2; continue; }
                p++;
            }
            marker_append_escaped_string(out, start, p - start);
            if (*p == q) p++;
        } else if (g_str_has_prefix(p, "attr(")) {
            p += 5;
            while (*p && g_ascii_isspace(*p)) p++;
            const char *start = p;
            while (*p && *p != ')' && *p != ',' && !g_ascii_isspace(*p)) p++;
            if (li && p != start) {
                char *attr_name = g_strndup(start, p - start);
                const char *val = ns_element_get_attr(li, attr_name);
                if (val) g_string_append(out, val);
                g_free(attr_name);
            }
            while (*p && *p != ')') p++;
            if (*p == ')') p++;
        } else if (g_str_has_prefix(p, "counter(")) {
            p += 8;
            const char *body_s = p;
            int depth = 1;
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') { depth--; if (depth == 0) break; }
                p++;
            }
            char *body = g_strndup(body_s, p - body_s);
            if (*p == ')') p++;
            char *sub = marker_counter_text(body, li, style_kw);
            if (sub) g_string_append(out, sub);
            g_free(sub);
            g_free(body);
        } else {
            const char *start = p;
            while (*p && !g_ascii_isspace(*p) && *p != '"' && *p != '\'') p++;
            g_string_append_len(out, start, p - start);
        }
    }
    return g_string_free(out, FALSE);
}

static char *
marker_custom_text(const ns_node *li, const ns_style *li_style,
                   const char *style_kw, gboolean *suppress_default)
{
    if (suppress_default) *suppress_default = FALSE;
    const ns_style *ms = li_style ? li_style->marker : NULL;
    if (!ms) return NULL;
    if (keyword_is(ms->values[NS_CSS_DISPLAY], "none")) {
        if (suppress_default) *suppress_default = TRUE;
        return NULL;
    }
    const ns_css_value *cv = ms->values[NS_CSS_CONTENT];
    if (!cv || cv->kind != NS_CSS_V_KEYWORD || !cv->u.keyword) return NULL;
    const char *raw = cv->u.keyword;
    if (strcmp(raw, "normal") == 0) return NULL;
    if (strcmp(raw, "none") == 0) {
        if (suppress_default) *suppress_default = TRUE;
        return NULL;
    }
    if (strcmp(raw, "open-quote") == 0) return g_strdup("\xe2\x80\x9c");
    if (strcmp(raw, "close-quote") == 0) return g_strdup("\xe2\x80\x9d");
    if (strcmp(raw, "no-open-quote") == 0 ||
        strcmp(raw, "no-close-quote") == 0) {
        if (suppress_default) *suppress_default = TRUE;
        return NULL;
    }
    return marker_resolve_content_text(raw, li, style_kw);
}

static void
paint_marker_label(cairo_t *cr, const char *label,
                   double content_x, double baseline_y, double font_size)
{
    if (!label || !*label) return;
    cairo_set_font_size(cr, font_size);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, label, &ext);
    double x = content_x - font_size * 0.35 - ext.width - ext.x_bearing;
    cairo_move_to(cr, x, baseline_y);
    cairo_show_text(cr, label);
}

static void
greek_label(int n, char *out, gsize out_sz)
{
    static const gunichar greek[24] = {
        0x3B1, 0x3B2, 0x3B3, 0x3B4, 0x3B5, 0x3B6, 0x3B7, 0x3B8,
        0x3B9, 0x3BA, 0x3BB, 0x3BC, 0x3BD, 0x3BE, 0x3BF, 0x3C0,
        0x3C1, 0x3C3, 0x3C4, 0x3C5, 0x3C6, 0x3C7, 0x3C8, 0x3C9,
    };
    if (n < 1) { g_snprintf(out, out_sz, "%d", n); return; }
    gunichar buf[16];
    int len = 0, v = n;
    while (v > 0 && len < 16) { v--; buf[len++] = greek[v % 24]; v /= 24; }
    GString *s = g_string_new(NULL);
    for (int i = len - 1; i >= 0; i--)
        g_string_append_unichar(s, buf[i]);
    g_strlcpy(out, s->str, out_sz);
    g_string_free(s, TRUE);
}

static void
format_ordered_label(const char *kind, int n, char *out, gsize out_sz)
{
    if (kind) {
        if (strcmp(kind, "lower-greek") == 0) {
            greek_label(n, out, out_sz); return;
        }
        if (strcmp(kind, "upper-alpha") == 0 || strcmp(kind, "upper-latin") == 0) {
            alpha_label(n, TRUE, out, out_sz); return;
        }
        if (strcmp(kind, "lower-alpha") == 0 || strcmp(kind, "lower-latin") == 0) {
            alpha_label(n, FALSE, out, out_sz); return;
        }
        if (strcmp(kind, "upper-roman") == 0) {
            roman_numeral(n, TRUE, out, out_sz); return;
        }
        if (strcmp(kind, "lower-roman") == 0) {
            roman_numeral(n, FALSE, out, out_sz); return;
        }
        if (strcmp(kind, "decimal-leading-zero") == 0) {
            g_snprintf(out, out_sz, "%02d", n); return;
        }
    }
    g_snprintf(out, out_sz, "%d", n);
}

gboolean
ns_paint_li_is_inside(const ns_style *li_style)
{
    if (!li_style) return FALSE;
    const ns_css_value *v = li_style->values[NS_CSS_LIST_STYLE_POSITION];
    return v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
           strcmp(v->u.keyword, "inside") == 0;
}

gboolean
ns_paint_li_marker_text(const ns_node *li, const ns_style *li_style,
                        char *out, gsize out_sz)
{
    if (!li || !li->name || strcmp(li->name, "li") != 0) return FALSE;
    if (out_sz < 8) return FALSE;
    const ns_node *parent = li->parent;
    if (!parent || !parent->name) return FALSE;
    const ns_css_value *lst = li_style
        ? li_style->values[NS_CSS_LIST_STYLE_TYPE] : NULL;
    const char *style_kw = NULL;
    if (lst && lst->kind == NS_CSS_V_KEYWORD) style_kw = lst->u.keyword;
    gboolean suppress_default = FALSE;
    char *custom = marker_custom_text(li, li_style, style_kw,
                                      &suppress_default);
    if (custom || suppress_default) {
        g_strlcpy(out, custom ? custom : "", out_sz);
        g_free(custom);
        return TRUE;
    }
    if (style_kw && strcmp(style_kw, "none") == 0) {
        out[0] = '\0';
        return TRUE;
    }
    gboolean ordered = strcmp(parent->name, "ol") == 0 ||
                       ordered_marker_kind(style_kw) != NULL;
    if (ordered) {
        int n = list_item_ordinal(li);
        const char *kind = marker_default_kind(li, style_kw);
        char buf[32];
        format_ordered_label(kind, n, buf, sizeof buf);
        g_snprintf(out, out_sz, "%s. ", buf);
        return TRUE;
    }
    const char *glyph = "\xe2\x80\xa2";
    if (style_kw && strcmp(style_kw, "square") == 0) glyph = "\xe2\x96\xaa";
    else if (style_kw && strcmp(style_kw, "circle") == 0) glyph = "\xe2\x97\x8b";
    g_snprintf(out, out_sz, "%s ", glyph);
    return TRUE;
}

static void
paint_marker(cairo_t *cr, const ns_box *b)
{
    if (!b->dom || !b->dom->name || strcmp(b->dom->name, "li") != 0) return;
    const ns_node *parent = b->dom->parent;
    if (!parent || !parent->name) return;
    const ns_style *s = b->style;
    if (ns_paint_li_is_inside(s)) return;
    const ns_css_value *lst = s ? s->values[NS_CSS_LIST_STYLE_TYPE] : NULL;
    const char *style_kw = NULL;
    if (lst && lst->kind == NS_CSS_V_KEYWORD) style_kw = lst->u.keyword;

    gboolean suppress_default = FALSE;
    char *custom = marker_custom_text(b->dom, s, style_kw,
                                      &suppress_default);
    if (suppress_default) {
        g_free(custom);
        return;
    }
    ns_image *marker_img = NULL;
    if (!custom && b->media && b->media->marker_image &&
        s && s->values[NS_CSS_LIST_STYLE_IMAGE] &&
        s->values[NS_CSS_LIST_STYLE_IMAGE]->kind == NS_CSS_V_URL) {
        ns_image *mi = b->media->marker_image;
        if (mi->loaded && mi->texture &&
            ns_texture_get_width(mi->texture) > 0 &&
            ns_texture_get_height(mi->texture) > 0)
            marker_img = mi;
    }
    if (style_kw && strcmp(style_kw, "none") == 0 && !custom && !marker_img)
        return;

    const ns_style *ms = s ? s->marker : NULL;

    double font_size = length_or(s ? s->values[NS_CSS_FONT_SIZE] : NULL, 16);
    if (ms && ms->values[NS_CSS_FONT_SIZE])
        font_size = length_or(ms->values[NS_CSS_FONT_SIZE], font_size);
    double cy = b->y + b->margin.top + b->padding.top + font_size * 0.7;
    double content_x = b->x + b->margin.left + b->padding.left;
    double cx = content_x - font_size * 0.8;

    if (marker_img) {
        int iw = ns_texture_get_width(marker_img->texture);
        int ih = ns_texture_get_height(marker_img->texture);
        double dw = iw, dh = ih;
        double cap = font_size;
        if (dh > cap) { dw *= cap / dh; dh = cap; }
        cairo_surface_t *surf = texture_surface_cached(marker_img->texture,
                                                       NULL);
        if (surf) {
            double dx = content_x - font_size * 0.35 - dw;
            double dy = cy - dh + font_size * 0.15;
            cairo_save(cr);
            cairo_translate(cr, dx, dy);
            cairo_scale(cr, dw / iw, dh / ih);
            cairo_set_source_surface(cr, surf, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
            return;
        }
    }
    const ns_css_value *cval = (ms && ms->values[NS_CSS_COLOR])
        ? ms->values[NS_CSS_COLOR]
        : (s ? s->values[NS_CSS_COLOR] : NULL);
    rgba color = rgba_of(cval, 0.1, 0.1, 0.1, 1);
    set_source_rgba(cr, color);

    gboolean ordered = strcmp(parent->name, "ol") == 0 ||
                       ordered_marker_kind(style_kw) != NULL;

    if (custom) {
        paint_marker_label(cr, custom, content_x, cy, font_size);
        g_free(custom);
    } else if (ordered) {
        int n = list_item_ordinal(b->dom);
        const char *kind = marker_default_kind(b->dom, style_kw);
        char buf[32];
        format_ordered_label(kind, n, buf, sizeof buf);
        char with_dot[40];
        g_snprintf(with_dot, sizeof with_dot, "%s.", buf);
        paint_marker_label(cr, with_dot, content_x, cy, font_size);
    } else if (style_kw && strcmp(style_kw, "square") == 0) {
        double sz = font_size * 0.32;
        cairo_new_path(cr);
        cairo_rectangle(cr, cx - sz/2, cy - font_size * 0.32 - sz/2, sz, sz);
        cairo_fill(cr);
    } else if (style_kw && strcmp(style_kw, "circle") == 0) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy - font_size * 0.32, font_size * 0.18, 0, 2 * G_PI);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    } else {
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy - font_size * 0.32, font_size * 0.18, 0, 2 * G_PI);
        cairo_fill(cr);
    }
}

static void
paint_hr(cairo_t *cr, const ns_box *b)
{
    if (!b->dom || !b->dom->name || strcmp(b->dom->name, "hr") != 0) return;
    if (b->border.top > 0 || b->border.bottom > 0 ||
        b->border.left > 0 || b->border.right > 0) return;
    double h = 1.0;
    const ns_style *s = b->style;
    if (s && s->values[NS_CSS_HEIGHT] &&
        s->values[NS_CSS_HEIGHT]->kind == NS_CSS_V_LENGTH) {
        double hv = s->values[NS_CSS_HEIGHT]->u.length.v;
        if (hv > 0) h = hv;
    }
    if (h > 24) h = 24;
    double y = b->y + b->margin.top + 4;
    double x0 = b->x + b->margin.left;
    double x1 = x0 + b->content_width;
    rgba color = rgba_of(s ? s->values[NS_CSS_COLOR] : NULL, 0.65, 0.65, 0.65, 1);
    set_source_rgba(cr, color);
    if (h <= 1.5) {
        cairo_set_line_width(cr, h);
        cairo_move_to(cr, x0, y);
        cairo_line_to(cr, x1, y);
        cairo_stroke(cr);
    } else {
        cairo_rectangle(cr, x0, y, x1 - x0, h);
        cairo_fill(cr);
    }
}

static gboolean
box_is_hidden(const ns_box *b)
{
    const ns_style *s = b ? b->style : NULL;
    if (!s) return FALSE;
    const ns_css_value *v = s->values[NS_CSS_VISIBILITY];
    if (v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
        (strcmp(v->u.keyword, "hidden") == 0 ||
         strcmp(v->u.keyword, "collapse") == 0))
        return TRUE;
    const ns_css_value *cv = s->values[NS_CSS_CONTENT_VISIBILITY];
    return cv && cv->kind == NS_CSS_V_KEYWORD && cv->u.keyword &&
           strcmp(cv->u.keyword, "hidden") == 0;
}

static double
box_opacity(const ns_box *b)
{
    if (b && g_paint_anim) {
        double anim_o;
        if (ns_anim_get_opacity(g_paint_anim, b->dom, &anim_o)) {
            if (anim_o < 0) anim_o = 0;
            if (anim_o > 1) anim_o = 1;
            return anim_o;
        }
    }
    const ns_style *s = b ? b->style : NULL;
    if (!s) return 1.0;
    const ns_css_value *v = s->values[NS_CSS_OPACITY];
    if (!v) return 1.0;
    if (v->kind == NS_CSS_V_LENGTH) {
        double o = v->u.length.v;
        if (o < 0) o = 0;
        if (o > 1) o = 1;
        return o;
    }
    return 1.0;
}

static gboolean
box_is_positioned(const ns_box *b)
{
    const ns_style *s = b ? b->style : NULL;
    if (!s) return FALSE;
    const ns_css_value *v = s->values[NS_CSS_POSITION];
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return FALSE;
    const char *kw = v->u.keyword;
    return strcmp(kw, "relative") == 0 || strcmp(kw, "absolute") == 0 ||
           strcmp(kw, "fixed") == 0    || strcmp(kw, "sticky") == 0;
}

static gboolean
box_clip_hides(const ns_box *b)
{
    const ns_style *s = b ? b->style : NULL;
    if (!s) return FALSE;
    const ns_css_value *cv = s->values[NS_CSS_CLIP];
    if (!cv || cv->kind != NS_CSS_V_RECT) return FALSE;
    const ns_css_value *pv = s->values[NS_CSS_POSITION];
    if (!pv || pv->kind != NS_CSS_V_KEYWORD || !pv->u.keyword) return FALSE;
    if (strcmp(pv->u.keyword, "absolute") != 0 &&
        strcmp(pv->u.keyword, "fixed") != 0) return FALSE;
    double bw = b->content_width + b->padding.left + b->padding.right +
                b->border.left + b->border.right;
    double bh = b->content_height + b->padding.top + b->padding.bottom +
                b->border.top + b->border.bottom;
    double top    = cv->u.rect.is_auto[0] ? 0  : cv->u.rect.v[0];
    double right  = cv->u.rect.is_auto[1] ? bw : cv->u.rect.v[1];
    double bottom = cv->u.rect.is_auto[2] ? bh : cv->u.rect.v[2];
    double left   = cv->u.rect.is_auto[3] ? 0  : cv->u.rect.v[3];
    return (right - left) <= 0 || (bottom - top) <= 0;
}

static int
box_z_index(const ns_box *b)
{
    const ns_style *s = b ? b->style : NULL;
    if (!s) return 0;
    const ns_css_value *v = s->values[NS_CSS_Z_INDEX];
    if (!v || v->kind != NS_CSS_V_LENGTH) return 0;
    return (int)v->u.length.v;
}

typedef struct paint_entry {
    const ns_box *box;
    int key;
    guint order;
} paint_entry;

static int
paint_entry_cmp(const void *a, const void *b)
{
    const paint_entry *pa = a;
    const paint_entry *pb = b;
    if (pa->key != pb->key) return pa->key < pb->key ? -1 : 1;
    if (pa->order != pb->order) return pa->order < pb->order ? -1 : 1;
    return 0;
}

static gboolean
box_defers_to_positioned_layer(const ns_box *b)
{
    return box_is_positioned(b) && box_z_index(b) >= 0;
}

static int
dom_tree_order_cmp(const ns_node *a, const ns_node *b)
{
    if (!a || !b || a == b) return 0;
    const ns_node *pa[128], *pb[128];
    int na = 0, nb = 0;
    for (const ns_node *n = a; n && na < 128; n = n->parent) pa[na++] = n;
    for (const ns_node *n = b; n && nb < 128; n = n->parent) pb[nb++] = n;
    if (na >= 128 || nb >= 128) return 0;
    int ia = na - 1, ib = nb - 1;
    while (ia >= 0 && ib >= 0 && pa[ia] == pb[ib]) { ia--; ib--; }
    if (ia < 0) return -1;
    if (ib < 0) return 1;
    if (pa[ia]->parent != pb[ib]->parent) return 0;
    for (const ns_node *s = pa[ia]->next_sibling; s; s = s->next_sibling)
        if (s == pb[ib]) return -1;
    return 1;
}

typedef struct deferred_capture {
    const ns_box *box;
    double dev_x, dev_y;
} deferred_capture;

typedef struct deferred_entry {
    const ns_box *box;
    guint idx;
} deferred_entry;

static int
deferred_entry_cmp(const void *va, const void *vb)
{
    const deferred_entry *a = va;
    const deferred_entry *b = vb;
    if (!a->box || !b->box)
        return a->idx < b->idx ? -1 : a->idx > b->idx ? 1 : 0;
    int za = box_z_index(a->box), zb = box_z_index(b->box);
    if (za != zb) return za < zb ? -1 : 1;
    int c = dom_tree_order_cmp(a->box->dom, b->box->dom);
    if (c) return c;
    return a->idx < b->idx ? -1 : a->idx > b->idx ? 1 : 0;
}

static void
paint_flush_deferred(cairo_t *cr, GPtrArray *list, const char *highlight)
{
    if (!list || list->len == 0) return;
    deferred_entry entries_buf[32];
    deferred_entry *entries = list->len <= G_N_ELEMENTS(entries_buf)
        ? entries_buf : g_new(deferred_entry, list->len);
    for (guint i = 0; i < list->len; i++) {
        const deferred_capture *cap = g_ptr_array_index(list, i);
        entries[i].box = cap->box;
        entries[i].idx = i;
    }
    qsort(entries, list->len, sizeof(deferred_entry), deferred_entry_cmp);
    const ns_box *saved_flush = g_paint_flush_box;
    for (guint i = 0; i < list->len; i++) {
        const deferred_capture *cap = NULL;
        for (guint j = 0; j < list->len; j++) {
            const deferred_capture *c2 = g_ptr_array_index(list, j);
            if (c2->box == entries[i].box) { cap = c2; break; }
        }
        double cur_x = 0, cur_y = 0;
        cairo_user_to_device(cr, &cur_x, &cur_y);
        double dx = cap ? cap->dev_x - cur_x : 0;
        double dy = cap ? cap->dev_y - cur_y : 0;
        if (isnan(dx) || isnan(dy)) dx = dy = 0;
        cairo_save(cr);
        if (dx != 0 || dy != 0) cairo_translate(cr, dx, dy);
        g_paint_flush_box = entries[i].box;
        if (g_dbg_paint_x >= 0 && entries[i].box->dom) {
            double gx0, gy0, gx1, gy1;
            cairo_clip_extents(cr, &gx0, &gy0, &gx1, &gy1);
            g_printerr("[flush-one] <%s#%s y=%.0f h=%.0f> d=%.0f,%.0f "
                       "clip=%.0f,%.0f..%.0f,%.0f\n",
                       entries[i].box->dom->name ? entries[i].box->dom->name
                                                 : "?",
                       ns_element_get_attr(entries[i].box->dom, "id")
                           ? ns_element_get_attr(entries[i].box->dom, "id")
                           : "",
                       entries[i].box->y, entries[i].box->content_height,
                       dx, dy, gx0, gy0, gx1, gy1);
        }
        paint_walk(cr, entries[i].box, highlight);
        cairo_restore(cr);
    }
    g_paint_flush_box = saved_flush;
    if (entries != entries_buf) g_free(entries);
}

static gboolean
sticky_length(const ns_css_value *v, double *out)
{
    if (!v || v->kind != NS_CSS_V_LENGTH) return FALSE;
    if (v->u.length.unit != NS_CSS_UNIT_PX &&
        v->u.length.unit != NS_CSS_UNIT_NUMBER) return FALSE;
    *out = v->u.length.v;
    return TRUE;
}

static gboolean g_paint_have_viewport;
static double g_paint_vp_x0, g_paint_vp_y0, g_paint_vp_x1, g_paint_vp_y1;

static void
compute_sticky_offset(const ns_box *b, cairo_t *cr,
                      double *out_dx, double *out_dy)
{
    *out_dx = 0;
    *out_dy = 0;
    if (!b || !b->style) return;
    if (!keyword_is(b->style->values[NS_CSS_POSITION], "sticky")) return;

    double clip_x1, clip_y1, clip_x2, clip_y2;
    cairo_clip_extents(cr, &clip_x1, &clip_y1, &clip_x2, &clip_y2);
    if (g_paint_have_viewport) {
        clip_x1 = g_paint_vp_x0;
        clip_y1 = g_paint_vp_y0;
        clip_x2 = g_paint_vp_x1;
        clip_y2 = g_paint_vp_y1;
    }

    double box_top = b->y;
    double box_h = b->margin.top + b->border.top + b->padding.top +
                   b->content_height +
                   b->padding.bottom + b->border.bottom + b->margin.bottom;
    double box_left = b->x;
    double box_w = b->margin.left + b->border.left + b->padding.left +
                   b->content_width +
                   b->padding.right + b->border.right + b->margin.right;

    double cb_top, cb_bot, cb_left, cb_right;
    const ns_box *p = b->parent;
    if (p) {
        cb_left = p->x + p->margin.left + p->border.left + p->padding.left;
        cb_top  = p->y + p->margin.top  + p->border.top  + p->padding.top;
        cb_right = cb_left + p->content_width;
        cb_bot   = cb_top  + p->content_height;
    } else {
        cb_left = clip_x1; cb_top = 0;
        cb_right = clip_x2; cb_bot = G_MAXDOUBLE / 2;
    }

    double tval = 0, bval = 0, lval = 0, rval = 0;
    gboolean has_top    = sticky_length(b->style->values[NS_CSS_TOP],    &tval);
    gboolean has_bot    = sticky_length(b->style->values[NS_CSS_BOTTOM], &bval);
    gboolean has_left   = sticky_length(b->style->values[NS_CSS_LEFT],   &lval);
    gboolean has_right  = sticky_length(b->style->values[NS_CSS_RIGHT],  &rval);

    if (has_top) {
        double target = clip_y1 + tval;
        if (box_top < target) {
            double want = target - box_top;
            double cap  = cb_bot - (box_top + box_h);
            if (cap < 0) cap = 0;
            *out_dy = want < cap ? want : cap;
        }
    }
    if (has_bot && *out_dy == 0) {
        double target = clip_y2 - bval;
        double box_bot = box_top + box_h;
        if (box_bot > target) {
            double want = target - box_bot;
            double cap  = cb_top - box_top;
            if (cap > 0) cap = 0;
            *out_dy = want > cap ? want : cap;
        }
    }
    if (has_left) {
        double target = clip_x1 + lval;
        if (box_left < target) {
            double want = target - box_left;
            double cap  = cb_right - (box_left + box_w);
            if (cap < 0) cap = 0;
            *out_dx = want < cap ? want : cap;
        }
    }
    if (has_right && *out_dx == 0) {
        double target = clip_x2 - rval;
        double box_right = box_left + box_w;
        if (box_right > target) {
            double want = target - box_right;
            double cap  = cb_left - box_left;
            if (cap > 0) cap = 0;
            *out_dx = want > cap ? want : cap;
        }
    }
}

static cairo_operator_t
blend_mode_operator(const ns_style *s)
{
    if (!s || !s->values[NS_CSS_MIX_BLEND_MODE]) return CAIRO_OPERATOR_OVER;
    const ns_css_value *v = s->values[NS_CSS_MIX_BLEND_MODE];
    if (v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return CAIRO_OPERATOR_OVER;
    const char *k = v->u.keyword;
    if (strcmp(k, "multiply")    == 0) return CAIRO_OPERATOR_MULTIPLY;
    if (strcmp(k, "screen")      == 0) return CAIRO_OPERATOR_SCREEN;
    if (strcmp(k, "overlay")     == 0) return CAIRO_OPERATOR_OVERLAY;
    if (strcmp(k, "darken")      == 0) return CAIRO_OPERATOR_DARKEN;
    if (strcmp(k, "lighten")     == 0) return CAIRO_OPERATOR_LIGHTEN;
    if (strcmp(k, "color-dodge") == 0) return CAIRO_OPERATOR_COLOR_DODGE;
    if (strcmp(k, "color-burn")  == 0) return CAIRO_OPERATOR_COLOR_BURN;
    if (strcmp(k, "hard-light")  == 0) return CAIRO_OPERATOR_HARD_LIGHT;
    if (strcmp(k, "soft-light")  == 0) return CAIRO_OPERATOR_SOFT_LIGHT;
    if (strcmp(k, "difference")  == 0) return CAIRO_OPERATOR_DIFFERENCE;
    if (strcmp(k, "exclusion")   == 0) return CAIRO_OPERATOR_EXCLUSION;
    if (strcmp(k, "hue")         == 0) return CAIRO_OPERATOR_HSL_HUE;
    if (strcmp(k, "saturation")  == 0) return CAIRO_OPERATOR_HSL_SATURATION;
    if (strcmp(k, "color")       == 0) return CAIRO_OPERATOR_HSL_COLOR;
    if (strcmp(k, "luminosity")  == 0) return CAIRO_OPERATOR_HSL_LUMINOSITY;
    return CAIRO_OPERATOR_OVER;
}

static const ns_box *g_paint_skip_box;
static ns_paint_stats g_paint_stats;
static gboolean g_paint_collect_stats;
static gboolean g_paint_have_clip;
static double g_paint_clip_y0, g_paint_clip_y1;
static double g_paint_cull_margin = 400.0;

static cairo_pattern_t *
mask_gradient_pattern(const ns_css_gradient *gr,
                      double bx, double by, double bw, double bh)
{
    if (!gr || gr->conic || gr->n_stops < 1) return NULL;
    double cx = bx + gr->center_x * bw;
    double cy = by + gr->center_y * bh;
    double dxh = 0, dyh = 0, r_outer = 1, line_len;
    if (gr->radial) {
        double corners[4][2] = {
            { bx, by }, { bx + bw, by }, { bx, by + bh }, { bx + bw, by + bh },
        };
        r_outer = 1;
        for (int k = 0; k < 4; k++) {
            double ddx = corners[k][0] - cx, ddy = corners[k][1] - cy;
            double dd = sqrt(ddx * ddx + ddy * ddy);
            if (dd > r_outer) r_outer = dd;
        }
        line_len = r_outer;
    } else {
        double rad = gr->angle_deg * G_PI / 180.0;
        double dx = sin(rad), dy = -cos(rad);
        double half = (fabs(dx) * bw + fabs(dy) * bh) / 2.0;
        dxh = dx * half; dyh = dy * half;
        line_len = 2.0 * half;
    }
    if (line_len <= 0) line_len = 1;
    double frac[NS_CSS_GRADIENT_STOPS_MAX];
    for (int i = 0; i < gr->n_stops; i++)
        frac[i] = gr->stops[i].pos_is_px ? gr->stops[i].pos / line_len
                                         : gr->stops[i].pos;
    double period = (gr->repeating && gr->n_stops > 0) ? frac[gr->n_stops - 1] : 1.0;
    if (period <= 0) period = 1.0;
    cairo_pattern_t *pat;
    if (gr->radial) {
        double r_tile = r_outer * period;
        if (r_tile <= 0) r_tile = 1;
        pat = cairo_pattern_create_radial(cx, cy, 0, cx, cy, r_tile);
    } else {
        double x0 = cx - dxh, y0 = cy - dyh;
        pat = cairo_pattern_create_linear(x0, y0,
            x0 + 2.0 * dxh * period, y0 + 2.0 * dyh * period);
    }
    for (int i = 0; i < gr->n_stops; i++) {
        const ns_css_gradient_stop *st = &gr->stops[i];
        cairo_pattern_add_color_stop_rgba(pat, frac[i] / period,
            st->r / 255.0, st->g / 255.0, st->b / 255.0, st->a / 255.0);
    }
    if (gr->repeating) cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
    return pat;
}


static void
paint_cache_clip(cairo_t *cr)
{
    double x0, x1;
    cairo_clip_extents(cr, &x0, &g_paint_clip_y0, &x1, &g_paint_clip_y1);
    g_paint_have_clip = TRUE;
}

static const ns_box *g_paint_tex_root;

typedef struct ns_quad3 {
    const ns_box *box;
    ns_mat4 m;
    double bx, by, bw, bh;
    double depth;
    guint seq;
    gboolean own_only;
} ns_quad3;

typedef struct quad_tex_entry {
    cairo_surface_t *surf;
    int tw, th;
} quad_tex_entry;

static GHashTable *g_paint_3d_tex;

static void
quad_tex_entry_free(gpointer p)
{
    quad_tex_entry *e = p;
    cairo_surface_destroy(e->surf);
    g_free(e);
}

static void
box_border_rect(const ns_box *b, double *bx, double *by, double *bw, double *bh)
{
    *bx = b->x + b->margin.left;
    *by = b->y + b->margin.top;
    *bw = b->content_width + b->padding.left + b->padding.right +
          b->border.left + b->border.right;
    *bh = b->content_height + b->padding.top + b->padding.bottom +
          b->border.top + b->border.bottom;
}

static gboolean
box_preserve3d(const ns_box *b)
{
    if (!b->style || !b->style->values[NS_CSS_TRANSFORM_STYLE]) return FALSE;
    const char *kw = ns_style_keyword(b->style, NS_CSS_TRANSFORM_STYLE);
    return kw && strcmp(kw, "preserve-3d") == 0;
}

static double
box_perspective_px(const ns_box *b)
{
    const ns_css_value *v =
        b->style ? b->style->values[NS_CSS_PERSPECTIVE] : NULL;
    if (v && v->kind == NS_CSS_V_LENGTH && v->u.length.v > 0)
        return v->u.length.v;
    return 0;
}

static gboolean
box_establishes_3d(const ns_box *b)
{
    if (box_perspective_px(b) > 0) return TRUE;
    if (!box_preserve3d(b)) return FALSE;
    return !(b->parent && box_preserve3d(b->parent));
}

static gboolean
box_has_own_decor(const ns_box *b)
{
    const ns_style *st = b->style;
    if (!st) return FALSE;
    const ns_css_value *bg = st->values[NS_CSS_BACKGROUND_COLOR];
    if (bg && bg->kind == NS_CSS_V_COLOR && bg->u.color.a > 0) return TRUE;
    const ns_css_value *bi = st->values[NS_CSS_BACKGROUND_IMAGE];
    if (bi && (bi->kind == NS_CSS_V_URL || bi->kind == NS_CSS_V_GRADIENT))
        return TRUE;
    if (b->border.left > 0 || b->border.right > 0 ||
        b->border.top > 0 || b->border.bottom > 0)
        return TRUE;
    const ns_css_value *ow = st->values[NS_CSS_OUTLINE_WIDTH];
    const ns_css_value *os = st->values[NS_CSS_OUTLINE_STYLE];
    if (ow && os && os->kind == NS_CSS_V_KEYWORD && os->u.keyword &&
        strcmp(os->u.keyword, "none") != 0 && length_or(ow, 0) > 0)
        return TRUE;
    return FALSE;
}

static gboolean
box_subtree_paints(const ns_box *b)
{
    if (box_is_hidden(b)) return FALSE;
    if (b->kind == NS_BOX_IMAGE || b->kind == NS_BOX_VIDEO ||
        b->kind == NS_BOX_INLINE || b->kind == NS_BOX_TEXT)
        return TRUE;
    if (box_has_own_decor(b)) return TRUE;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        if (box_subtree_paints(c)) return TRUE;
    return FALSE;
}

static void
box_transform_origin(const ns_box *b, double bx, double by, double bw,
                     double bh, ns_css_prop prop,
                     double *ox, double *oy, double *oz)
{
    *ox = bx + bw / 2.0;
    *oy = by + bh / 2.0;
    *oz = 0;
    const ns_css_value *origin = b->style ? b->style->values[prop] : NULL;
    if (origin && origin->kind == NS_CSS_V_TRANSFORM &&
        origin->u.transform.n_ops > 0) {
        const ns_css_transform_op *o = &origin->u.transform.ops[0];
        *ox = bx + (o->a_is_percent ? o->a / 100.0 * bw : o->a);
        *oy = by + (o->b_is_percent ? o->b / 100.0 * bh : o->b);
        *oz = o->c;
    }
}

static void
collect_3d_quads(const ns_box *b, const ns_mat4 *pm, GArray *quads)
{
    if (box_is_hidden(b) || b == g_paint_skip_box) return;
    double bx, by, bw, bh;
    box_border_rect(b, &bx, &by, &bw, &bh);
    ns_mat4 m = *pm;
    const ns_css_transform *anim_tf =
        g_paint_anim ? ns_anim_get_transform(g_paint_anim, b->dom) : NULL;
    ns_css_transform eff;
    eff.n_ops = 0;
    if (anim_tf ||
        (b->style && (b->style->values[NS_CSS_TRANSFORM] ||
                      b->style->values[NS_CSS_TRANSLATE] ||
                      b->style->values[NS_CSS_ROTATE] ||
                      b->style->values[NS_CSS_SCALE])))
        ns_css_style_effective_transform(b->style, anim_tf, &eff);
    if (eff.n_ops > 0) {
        double ox, oy, oz;
        box_transform_origin(b, bx, by, bw, bh, NS_CSS_TRANSFORM_ORIGIN,
                             &ox, &oy, &oz);
        ns_mat4 tm;
        ns_css_transform_to_mat4(&eff, bw, bh, &tm);
        ns_mat4_translate(&m, ox, oy, oz);
        ns_mat4_multiply(&m, &tm, &m);
        ns_mat4_translate(&m, -ox, -oy, -oz);
    }
    gboolean p3d = box_preserve3d(b);
    if (!p3d || !b->first_child) {
        if (box_subtree_paints(b)) {
            ns_quad3 q = { b, m, bx, by, bw, bh, 0, quads->len, FALSE };
            g_array_append_val(quads, q);
        }
        return;
    }
    if (box_has_own_decor(b)) {
        ns_quad3 q = { b, m, bx, by, bw, bh, 0, quads->len, TRUE };
        g_array_append_val(quads, q);
    }
    ns_mat4 cm = m;
    double d = box_perspective_px(b);
    if (d > 0) {
        double pox, poy, poz;
        box_transform_origin(b, bx, by, bw, bh, NS_CSS_PERSPECTIVE_ORIGIN,
                             &pox, &poy, &poz);
        ns_mat4_translate(&cm, pox, poy, 0);
        ns_mat4_perspective(&cm, d);
        ns_mat4_translate(&cm, -pox, -poy, 0);
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        collect_3d_quads(c, &cm, quads);
}

static void
paint_quad3(cairo_t *cr, const ns_quad3 *q, const char *highlight)
{
    if (q->bw < 0.5 || q->bh < 0.5) return;
    const double weps = 0.02;
    double px[4], py[4], pws[4];
    const double cxs[4] = { q->bx, q->bx + q->bw, q->bx + q->bw, q->bx };
    const double cys[4] = { q->by, q->by, q->by + q->bh, q->by + q->bh };
    int behind = 0;
    for (int i = 0; i < 4; i++) {
        double ox, oy, oz, ow;
        ns_mat4_apply(&q->m, cxs[i], cys[i], 0, &ox, &oy, &oz, &ow);
        pws[i] = ow;
        if (ow < weps) {
            behind++;
            px[i] = 0;
            py[i] = 0;
            continue;
        }
        px[i] = ox / ow;
        py[i] = oy / ow;
    }
    if (behind == 4) return;
    gboolean clipped = behind > 0;
    double clx0, cly0, clx1, cly1;
    cairo_clip_extents(cr, &clx0, &cly0, &clx1, &cly1);
    double minx = 0, maxx = 0, miny = 0, maxy = 0;
    if (!clipped) {
        double area2 = (px[1] - px[0]) * (py[3] - py[0]) -
                       (px[3] - px[0]) * (py[1] - py[0]);
        if (fabs(area2) < 0.01) return;
        if (area2 < 0 && q->box->style) {
            const char *bfv =
                ns_style_keyword(q->box->style, NS_CSS_BACKFACE_VISIBILITY);
            if (bfv && strcmp(bfv, "hidden") == 0) return;
        }
        minx = px[0]; maxx = px[0]; miny = py[0]; maxy = py[0];
        for (int i = 1; i < 4; i++) {
            minx = MIN(minx, px[i]);
            maxx = MAX(maxx, px[i]);
            miny = MIN(miny, py[i]);
            maxy = MAX(maxy, py[i]);
        }
        if (maxx < clx0 || minx > clx1 || maxy < cly0 || miny > cly1) return;
    } else {
        minx = clx0; maxx = clx1; miny = cly0; maxy = cly1;
    }

    double k = clipped ? 2.0
                       : MAX((maxx - minx) / MAX(q->bw, 1.0),
                             (maxy - miny) / MAX(q->bh, 1.0));
    k = CLAMP(k, 1.0, 3.0);
    double maxdim = MAX(q->bw, q->bh);
    if (maxdim > 0.0 && maxdim * k > 4096.0)
        k = 4096.0 / maxdim;
    int tw = (int)ceil(q->bw * k);
    int th = (int)ceil(q->bh * k);
    if (tw < 1 || th < 1) return;
    if (tw > 4096) { k *= 4096.0 / tw; tw = 4096; th = (int)ceil(q->bh * k); }
    if (th > 4096) { k *= 4096.0 / th; th = 4096; tw = (int)ceil(q->bw * k); }
    gboolean cacheable = q->box->dom && !q->own_only &&
                         !q->box->first_child &&
                         q->box->kind == NS_BOX_BLOCK &&
                         !(q->box->dom->name &&
                           strcmp(q->box->dom->name, "canvas") == 0);
    if (cacheable && g_paint_anim) {
        double anim_op;
        guint8 anim_col[4];
        if (ns_anim_get_opacity(g_paint_anim, q->box->dom, &anim_op) ||
            ns_anim_get_color(g_paint_anim, q->box->dom,
                              NS_CSS_ANIM_TARGET_COLOR, anim_col) ||
            ns_anim_get_color(g_paint_anim, q->box->dom,
                              NS_CSS_ANIM_TARGET_BG_COLOR, anim_col))
            cacheable = FALSE;
    }
    cairo_surface_t *tex = NULL;
    if (cacheable && g_paint_3d_tex) {
        quad_tex_entry *e =
            g_hash_table_lookup(g_paint_3d_tex, (gpointer)q->box);
        if (e && e->tw == tw && e->th == th)
            tex = cairo_surface_reference(e->surf);
    }
    if (!tex) {
        tex = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
        if (cairo_surface_status(tex) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(tex);
            return;
        }
        cairo_t *tcr = cairo_create(tex);
        cairo_scale(tcr, k, k);
        cairo_translate(tcr, -q->bx, -q->by);
        const ns_box *saved_root = g_paint_tex_root;
        const ns_box *saved_flush = g_paint_flush_box;
        g_paint_tex_root = q->box;
        g_paint_flush_box = q->box;
        g_paint_no_cull++;
        if (q->own_only)
            paint_block(tcr, q->box);
        else
            paint_walk(tcr, q->box, highlight);
        g_paint_no_cull--;
        g_paint_tex_root = saved_root;
        g_paint_flush_box = saved_flush;
        cairo_destroy(tcr);
        const ns_style *st = q->box->style;
        const char *filter_kw = st && st->values[NS_CSS_FILTER] &&
            st->values[NS_CSS_FILTER]->kind == NS_CSS_V_KEYWORD
            ? st->values[NS_CSS_FILTER]->u.keyword : NULL;
        if (filter_kw && filter_has_bitmap_effect(filter_kw)) {
            cairo_surface_flush(tex);
            apply_image_filter(cairo_image_surface_get_data(tex),
                               cairo_image_surface_get_stride(tex),
                               tw, th, filter_kw);
            cairo_surface_mark_dirty(tex);
        }
        if (cacheable) {
            if (!g_paint_3d_tex)
                g_paint_3d_tex =
                    g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                          NULL, quad_tex_entry_free);
            quad_tex_entry *e = g_new(quad_tex_entry, 1);
            e->surf = cairo_surface_reference(tex);
            e->tw = tw;
            e->th = th;
            g_hash_table_replace(g_paint_3d_tex, (gpointer)q->box, e);
        }
    }

    double wmin = pws[0], wmax = pws[0];
    for (int i = 1; i < 4; i++) {
        wmin = MIN(wmin, pws[i]);
        wmax = MAX(wmax, pws[i]);
    }
    int n = 1;
    if (clipped) {
        n = 32;
    } else if ((wmax - wmin) / MAX(wmin, 1e-9) > 0.02) {
        double dim = MAX(maxx - minx, maxy - miny);
        n = (int)(dim / 24.0);
        n = CLAMP(n, 2, 32);
    }
    int np = n + 1;
    double *gx = g_new(double, (gsize)np * np * 3);
    double *gy = gx + np * np;
    double *gw = gy + np * np;
    for (int j = 0; j <= n; j++)
        for (int i = 0; i <= n; i++) {
            double ox, oy, oz, ow;
            ns_mat4_apply(&q->m,
                          q->bx + q->bw * i / n,
                          q->by + q->bh * j / n, 0,
                          &ox, &oy, &oz, &ow);
            gw[j * np + i] = ow;
            if (ow < weps) {
                gx[j * np + i] = 0;
                gy[j * np + i] = 0;
            } else {
                gx[j * np + i] = ox / ow;
                gy[j * np + i] = oy / ow;
            }
        }
    cairo_pattern_t *pat = cairo_pattern_create_for_surface(tex);
    cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);
    cairo_pattern_set_filter(pat, CAIRO_FILTER_GOOD);
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            double cws[4] = { gw[j * np + i], gw[j * np + i + 1],
                              gw[(j + 1) * np + i + 1], gw[(j + 1) * np + i] };
            int cell_behind = 0;
            for (int c2 = 0; c2 < 4; c2++)
                if (cws[c2] < weps) cell_behind++;
            if (cell_behind == 4) continue;
            double sx[8], sy[8], ss[8], st[8];
            int nv;
            if (cell_behind == 0) {
                sx[0] = gx[j * np + i];       sy[0] = gy[j * np + i];
                sx[1] = gx[j * np + i + 1];   sy[1] = gy[j * np + i + 1];
                sx[2] = gx[(j + 1) * np + i + 1];
                sy[2] = gy[(j + 1) * np + i + 1];
                sx[3] = gx[(j + 1) * np + i]; sy[3] = gy[(j + 1) * np + i];
                ss[0] = 0; st[0] = 0; ss[1] = 1; st[1] = 0;
                ss[2] = 1; st[2] = 1; ss[3] = 0; st[3] = 1;
                nv = 4;
            } else {
                const double cs[4] = { 0, 1, 1, 0 };
                const double ct[4] = { 0, 0, 1, 1 };
                nv = 0;
                for (int c2 = 0; c2 < 4; c2++) {
                    int c3 = (c2 + 1) & 3;
                    gboolean in_a = cws[c2] >= weps;
                    gboolean in_b = cws[c3] >= weps;
                    if (in_a) {
                        ss[nv] = cs[c2];
                        st[nv] = ct[c2];
                        nv++;
                    }
                    if (in_a != in_b) {
                        double t = (weps - cws[c2]) / (cws[c3] - cws[c2]);
                        ss[nv] = cs[c2] + (cs[c3] - cs[c2]) * t;
                        st[nv] = ct[c2] + (ct[c3] - ct[c2]) * t;
                        nv++;
                    }
                }
                if (nv < 3) continue;
                for (int v = 0; v < nv; v++) {
                    double ox, oy, oz, ow;
                    ns_mat4_apply(&q->m,
                                  q->bx + q->bw * (i + ss[v]) / n,
                                  q->by + q->bh * (j + st[v]) / n, 0,
                                  &ox, &oy, &oz, &ow);
                    if (ow < weps * 0.5) { nv = 0; break; }
                    sx[v] = ox / ow;
                    sy[v] = oy / ow;
                }
                if (nv < 3) continue;
            }
            double d_s1 = ss[1] - ss[0], d_t1 = st[1] - st[0];
            double d_s2 = ss[2] - ss[0], d_t2 = st[2] - st[0];
            double pdet = d_s1 * d_t2 - d_s2 * d_t1;
            if (fabs(pdet) < 1e-9) continue;
            double ma = ((sx[1] - sx[0]) * d_t2 - (sx[2] - sx[0]) * d_t1) / pdet;
            double mc = ((sx[2] - sx[0]) * d_s1 - (sx[1] - sx[0]) * d_s2) / pdet;
            double mb = ((sy[1] - sy[0]) * d_t2 - (sy[2] - sy[0]) * d_t1) / pdet;
            double md = ((sy[2] - sy[0]) * d_s1 - (sy[1] - sy[0]) * d_s2) / pdet;
            double me = sx[0] - ma * ss[0] - mc * st[0];
            double mf = sy[0] - mb * ss[0] - md * st[0];
            double ccx = 0, ccy = 0;
            for (int v = 0; v < nv; v++) { ccx += sx[v]; ccy += sy[v]; }
            ccx /= nv;
            ccy /= nv;
            double pad = n > 1 ? 0.35 : 0.0;
            cairo_save(cr);
            cairo_new_path(cr);
            for (int v = 0; v < nv; v++) {
                double dx2 = sx[v] - ccx, dy2 = sy[v] - ccy;
                double dlen = sqrt(dx2 * dx2 + dy2 * dy2);
                double ex = sx[v], ey = sy[v];
                if (dlen > 1e-9) {
                    ex += dx2 / dlen * pad;
                    ey += dy2 / dlen * pad;
                }
                if (v == 0) cairo_move_to(cr, ex, ey);
                else        cairo_line_to(cr, ex, ey);
            }
            cairo_close_path(cr);
            cairo_clip(cr);
            cairo_matrix_t cm2;
            cairo_matrix_init(&cm2, ma, mb, mc, md, me, mf);
            cairo_transform(cr, &cm2);
            cairo_matrix_t pm2;
            cairo_matrix_init(&pm2, (double)tw / n, 0, 0, (double)th / n,
                              (double)tw * i / n, (double)th * j / n);
            cairo_pattern_set_matrix(pat, &pm2);
            cairo_set_source(cr, pat);
            cairo_rectangle(cr, -0.5, -0.5, 2.0, 2.0);
            cairo_fill(cr);
            cairo_restore(cr);
        }
    }
    cairo_pattern_destroy(pat);
    cairo_surface_destroy(tex);
    g_free(gx);
}

static int
quad3_cmp(gconstpointer pa, gconstpointer pb)
{
    const ns_quad3 *a = pa, *b = pb;
    if (a->depth < b->depth) return -1;
    if (a->depth > b->depth) return 1;
    if (a->seq < b->seq) return -1;
    if (a->seq > b->seq) return 1;
    return 0;
}

static GHashTable *g_paint_3d_reg;

static void
paint_3d_reg_value_free(gpointer v)
{
    g_array_free(v, TRUE);
}

void
ns_paint_3d_invalidate(void)
{
    if (g_paint_3d_reg) g_hash_table_remove_all(g_paint_3d_reg);
    if (g_paint_3d_tex) g_hash_table_remove_all(g_paint_3d_tex);
}

gboolean
ns_paint_3d_registered(const ns_box *b)
{
    if (g_paint_3d_reg && g_hash_table_contains(g_paint_3d_reg, (gpointer)b))
        return TRUE;
    return box_establishes_3d(b);
}

static void
paint_3d_register(const ns_box *root, const GArray *quads)
{
    if (!g_paint_3d_reg)
        g_paint_3d_reg = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                               NULL, paint_3d_reg_value_free);
    GArray *snap = g_array_sized_new(FALSE, FALSE, sizeof(ns_quad3),
                                     quads->len);
    g_array_append_vals(snap, quads->data, quads->len);
    g_hash_table_replace(g_paint_3d_reg, (gpointer)root, snap);
}

static gboolean
quad3_unproject(const ns_quad3 *q, double sx, double sy,
                double *u_out, double *v_out, double *depth_out)
{
    const double *m = q->m.m;
    double a = m[0] * q->bw, b = m[1] * q->bh;
    double c = m[0] * q->bx + m[1] * q->by + m[3];
    double d = m[4] * q->bw, e = m[5] * q->bh;
    double f = m[4] * q->bx + m[5] * q->by + m[7];
    double g = m[12] * q->bw, h = m[13] * q->bh;
    double k = m[12] * q->bx + m[13] * q->by + m[15];
    double i0 = e * k - f * h;
    double i1 = c * h - b * k;
    double i2 = b * f - c * e;
    double i3 = f * g - d * k;
    double i4 = a * k - c * g;
    double i5 = c * d - a * f;
    double i6 = d * h - e * g;
    double i7 = b * g - a * h;
    double i8 = a * e - b * d;
    double det = a * i0 + b * i3 + c * i6;
    if (fabs(det) < 1e-12) return FALSE;
    double tu = i0 * sx + i1 * sy + i2;
    double tv = i3 * sx + i4 * sy + i5;
    double tw = i6 * sx + i7 * sy + i8;
    if (fabs(tw) < 1e-12) return FALSE;
    double u = tu / tw, v = tv / tw;
    if (u < -0.002 || u > 1.002 || v < -0.002 || v > 1.002) return FALSE;
    double px = q->bx + u * q->bw, py = q->by + v * q->bh;
    double w = m[12] * px + m[13] * py + m[15];
    if (w < 1e-6) return FALSE;
    double z = m[8] * px + m[9] * py + m[11];
    *u_out = u;
    *v_out = v;
    *depth_out = z / w;
    return TRUE;
}

typedef struct quad_pick_cand {
    guint idx;
    double depth, u, v;
} quad_pick_cand;

static int
quad_pick_cand_cmp(gconstpointer pa, gconstpointer pb)
{
    const quad_pick_cand *a = pa, *b = pb;
    if (a->depth > b->depth) return -1;
    if (a->depth < b->depth) return 1;
    if (a->idx > b->idx) return -1;
    if (a->idx < b->idx) return 1;
    return 0;
}

static GArray *collect_root_quads(const ns_box *b);

const ns_box *
ns_paint_3d_pick(const ns_box *root3d, double x, double y)
{
    GArray *quads = g_paint_3d_reg
        ? g_hash_table_lookup(g_paint_3d_reg, (gpointer)root3d) : NULL;
    if (!quads) {
        GArray *fresh = collect_root_quads(root3d);
        paint_3d_register(root3d, fresh);
        g_array_free(fresh, TRUE);
        quads = g_hash_table_lookup(g_paint_3d_reg, (gpointer)root3d);
        if (!quads) return NULL;
    }
    GArray *cands = g_array_new(FALSE, FALSE, sizeof(quad_pick_cand));
    for (guint i = 0; i < quads->len; i++) {
        const ns_quad3 *q = &g_array_index(quads, ns_quad3, i);
        quad_pick_cand c = { i, 0, 0, 0 };
        if (quad3_unproject(q, x, y, &c.u, &c.v, &c.depth))
            g_array_append_val(cands, c);
    }
    g_array_sort(cands, quad_pick_cand_cmp);
    const ns_box *result = NULL;
    for (guint i = 0; i < cands->len && !result; i++) {
        const quad_pick_cand *c = &g_array_index(cands, quad_pick_cand, i);
        const ns_quad3 *q = &g_array_index(quads, ns_quad3, c->idx);
        double lx = q->bx + c->u * q->bw;
        double ly = q->by + c->v * q->bh;
        result = ns_box_hit_test(q->box, lx, ly);
    }
    g_array_free(cands, TRUE);
    return result;
}

static GArray *
collect_root_quads(const ns_box *b)
{
    GArray *quads = g_array_new(FALSE, FALSE, sizeof(ns_quad3));
    ns_mat4 root;
    ns_mat4_identity(&root);
    if (box_establishes_3d(b)) {
        double d = box_perspective_px(b);
        if (d > 0) {
            double bx, by, bw, bh;
            box_border_rect(b, &bx, &by, &bw, &bh);
            double pox, poy, poz;
            box_transform_origin(b, bx, by, bw, bh,
                                 NS_CSS_PERSPECTIVE_ORIGIN,
                                 &pox, &poy, &poz);
            ns_mat4_translate(&root, pox, poy, 0);
            ns_mat4_perspective(&root, d);
            ns_mat4_translate(&root, -pox, -poy, 0);
        }
        for (const ns_box *c = b->first_child; c; c = c->next_sibling)
            collect_3d_quads(c, &root, quads);
    } else {
        collect_3d_quads(b, &root, quads);
    }
    for (guint i = 0; i < quads->len; i++) {
        ns_quad3 *q = &g_array_index(quads, ns_quad3, i);
        double ox, oy, oz, ow;
        ns_mat4_apply(&q->m, q->bx + q->bw / 2.0, q->by + q->bh / 2.0, 0,
                      &ox, &oy, &oz, &ow);
        if (ow > 1e-6) {
            q->depth = oz / ow;
        } else {
            q->depth = -1e30;
            const double cxs[4] = { q->bx, q->bx + q->bw,
                                    q->bx + q->bw, q->bx };
            const double cys[4] = { q->by, q->by,
                                    q->by + q->bh, q->by + q->bh };
            for (int c = 0; c < 4; c++) {
                ns_mat4_apply(&q->m, cxs[c], cys[c], 0, &ox, &oy, &oz, &ow);
                if (ow > 1e-6 && oz / ow > q->depth) q->depth = oz / ow;
            }
        }
    }
    g_array_sort(quads, quad3_cmp);
    return quads;
}

static void
paint_3d_root(cairo_t *cr, const ns_box *b, const char *highlight)
{
    if (box_establishes_3d(b) &&
        (b->kind == NS_BOX_BLOCK || b->kind == NS_BOX_TABLE ||
         b->kind == NS_BOX_TABLE_CAPTION || b->kind == NS_BOX_TABLE_CELL))
        paint_block(cr, b);
    GArray *quads = collect_root_quads(b);
    paint_3d_register(b, quads);
    if (g_getenv("NS_3D_DEBUG")) {
        fprintf(stderr, "3d root <%s class=%s> %u quads\n",
                b->dom && b->dom->name ? b->dom->name : "?",
                b->dom ? (ns_element_get_attr(b->dom, "class") ?: "") : "",
                quads->len);
        for (guint i = 0; i < quads->len; i++) {
            const ns_quad3 *q = &g_array_index(quads, ns_quad3, i);
            double cxs[4] = { q->bx, q->bx + q->bw, q->bx + q->bw, q->bx };
            double cys[4] = { q->by, q->by, q->by + q->bh, q->by + q->bh };
            fprintf(stderr, "  quad <%s class=%s> rect %.0f,%.0f %gx%g depth %.2f corners",
                    q->box->dom && q->box->dom->name ? q->box->dom->name : "?",
                    q->box->dom ? (ns_element_get_attr(q->box->dom, "class") ?: "") : "",
                    q->bx, q->by, q->bw, q->bh, q->depth);
            for (int c = 0; c < 4; c++) {
                double ox, oy, oz, ow;
                ns_mat4_apply(&q->m, cxs[c], cys[c], 0, &ox, &oy, &oz, &ow);
                if (ow > 1e-6)
                    fprintf(stderr, " (%.1f,%.1f)", ox / ow, oy / ow);
                else
                    fprintf(stderr, " (w=%.3f!)", ow);
            }
            fprintf(stderr, "\n");
        }
    }
    g_paint_no_cull++;
    for (guint i = 0; i < quads->len; i++) {
        double w0 = 0, w1 = 0, w2 = 0, w3 = 0;
        if (g_dbg_paint_x >= 0)
            cairo_clip_extents(cr, &w0, &w1, &w2, &w3);
        paint_quad3(cr, &g_array_index(quads, ns_quad3, i), highlight);
        if (g_dbg_paint_x >= 0) {
            double v0, v1, v2, v3;
            cairo_clip_extents(cr, &v0, &v1, &v2, &v3);
            if (fabs(v0 - w0) > 0.5 || fabs(v1 - w1) > 0.5 ||
                fabs(v2 - w2) > 0.5 || fabs(v3 - w3) > 0.5) {
                const ns_quad3 *q = &g_array_index(quads, ns_quad3, i);
                g_printerr("[quad3-LEAK] <%s class=%s> rect %.0f,%.0f "
                           "%gx%g\n",
                           q->box->dom && q->box->dom->name
                               ? q->box->dom->name : "?",
                           q->box->dom
                               ? (ns_element_get_attr(q->box->dom, "class")
                                      ?: "")
                               : "",
                           q->bx, q->by, q->bw, q->bh);
            }
        }
    }
    g_paint_no_cull--;
    g_array_free(quads, TRUE);
}

static void
ns_dbg_paint_probe(cairo_t *cr, const ns_box *b)
{
    if (g_dbg_paint_x == -2) {
        const char *s = g_getenv("NS_DBG_PAINT_AT");
        g_dbg_paint_x = g_dbg_paint_y = -1;
        if (s) sscanf(s, "%d,%d", &g_dbg_paint_x, &g_dbg_paint_y);
    }
    if (g_dbg_paint_x < 0) return;
    if (isnan(b->x) || isnan(b->y) ||
        isnan(b->content_width) || isnan(b->content_height)) {
        GString *ch = g_string_new("[paint-NAN]");
        for (const ns_box *p2 = b; p2; p2 = p2->parent) {
            const char *nm = p2->dom && p2->dom->name ? p2->dom->name : "?";
            const char *id = p2->dom && p2->dom->kind == NS_NODE_ELEMENT
                           ? ns_element_get_attr(p2->dom, "id") : NULL;
            g_string_append_printf(ch, " <%s#%s%s>", nm, id ? id : "",
                                   isnan(p2->x) ? " NAN" : "");
        }
        g_printerr("%s\n", ch->str);
        g_string_free(ch, TRUE);
    }
    if (b->dom && b->dom->name &&
        strcmp(b->dom->name, "ytd-watch-metadata") == 0) {
        GString *chain = g_string_new("[paint-chain]");
        for (const ns_box *p2 = b; p2; p2 = p2->parent) {
            const char *nm = p2->dom && p2->dom->name ? p2->dom->name : "?";
            const char *id = p2->dom && p2->dom->kind == NS_NODE_ELEMENT
                           ? ns_element_get_attr(p2->dom, "id") : NULL;
            g_string_append_printf(chain, " <%s#%s y=%.0f h=%.0f>",
                                   nm, id ? id : "", p2->y,
                                   p2->content_height);
        }
        g_printerr("%s\n", chain->str);
        g_string_free(chain, TRUE);
    }
    double x0 = b->x, y0 = b->y;
    double x1 = b->x + b->content_width, y1 = b->y + b->content_height;
    cairo_user_to_device(cr, &x0, &y0);
    cairo_user_to_device(cr, &x1, &y1);
    if (isnan(x0) && !isnan(b->x)) {
        GString *ch = g_string_new("[paint-CTM-NAN]");
        for (const ns_box *p2 = b; p2; p2 = p2->parent) {
            const char *nm = p2->dom && p2->dom->name ? p2->dom->name : "?";
            const char *id = p2->dom && p2->dom->kind == NS_NODE_ELEMENT
                           ? ns_element_get_attr(p2->dom, "id") : NULL;
            g_string_append_printf(ch, " <%s#%s sx=%.0f sy=%.0f>",
                                   nm, id ? id : "",
                                   p2->scroll_x, p2->scroll_y);
        }
        g_printerr("%s\n", ch->str);
        g_string_free(ch, TRUE);
    }
    if (g_dbg_paint_x < x0 || g_dbg_paint_x > x1 ||
        g_dbg_paint_y < y0 || g_dbg_paint_y > y1)
        return;
    const ns_css_value *bgv =
        b->style ? b->style->values[NS_CSS_BACKGROUND_COLOR] : NULL;
    char bg[64] = "-";
    if (bgv && bgv->kind == NS_CSS_V_COLOR)
        g_snprintf(bg, sizeof bg, "rgba(%d,%d,%d,%d)",
                   (int)bgv->u.color.r, (int)bgv->u.color.g,
                   (int)bgv->u.color.b, (int)bgv->u.color.a);
    double kx0, ky0, kx1, ky1;
    cairo_clip_extents(cr, &kx0, &ky0, &kx1, &ky1);
    g_printerr("[paint-at] <%s> %.0f,%.0f %.0fx%.0f bg=%s clip=%.0f,%.0f..%.0f,%.0f\n",
               b->dom && b->dom->name ? b->dom->name : "?",
               x0, y0, x1 - x0, y1 - y0, bg, kx0, ky0, kx1, ky1);
}

static void
paint_walk(cairo_t *cr, const ns_box *b, const char *highlight)
{
    if (!b) return;
    if (g_paint_collect_stats) g_paint_stats.boxes_seen++;
    if (box_is_hidden(b)) {
        if (g_paint_collect_stats) g_paint_stats.hidden++;
        return;
    }
    ns_dbg_paint_probe(cr, b);
    if (box_clip_hides(b)) {
        if (g_paint_collect_stats) g_paint_stats.hidden++;
        return;
    }
    if (b == g_paint_skip_box) {
        if (g_paint_collect_stats) g_paint_stats.skipped_top++;
        return;
    }
    if (g_paint_defer_depth > 0 && b != g_paint_flush_box &&
        box_defers_to_positioned_layer(b)) {
        if (!g_paint_deferred_list)
            g_paint_deferred_list = g_ptr_array_new_with_free_func(g_free);
        deferred_capture *cap = g_new0(deferred_capture, 1);
        cap->box = b;
        cairo_user_to_device(cr, &cap->dev_x, &cap->dev_y);
        g_ptr_array_add(g_paint_deferred_list, cap);
        if (g_dbg_paint_x >= 0 && b->dom && b->dom->name)
            g_printerr("[paint-defer] <%s#%s> y=%.0f h=%.0f\n",
                       b->dom->name,
                       ns_element_get_attr(b->dom, "id")
                           ? ns_element_get_attr(b->dom, "id") : "",
                       b->y, b->content_height);
        return;
    }
    if (!g_paint_no_cull && g_paint_have_clip &&
        b->paint_bottom > b->paint_top) {
        if (b->paint_bottom < g_paint_clip_y0 - g_paint_cull_margin ||
            b->paint_top > g_paint_clip_y1 + g_paint_cull_margin) {
            if (g_paint_collect_stats) g_paint_stats.culled_bounds++;
            return;
        }
    }
    double dbg_e0 = 0, dbg_e1 = 0, dbg_e2 = 0, dbg_e3 = 0;
    if (g_dbg_paint_x >= 0)
        cairo_clip_extents(cr, &dbg_e0, &dbg_e1, &dbg_e2, &dbg_e3);
    const ns_style *style = b->style;
    double op = box_opacity(b);
    cairo_operator_t blend = blend_mode_operator(style);
    const ns_css_value *mask_v = style ? style->values[NS_CSS_MASK_IMAGE] : NULL;
    gboolean mask_grad = mask_v && mask_v->kind == NS_CSS_V_GRADIENT &&
                         !mask_v->u.gradient.conic;
    gboolean grouped = op < 0.999 || blend != CAIRO_OPERATOR_OVER || mask_grad;
    double sticky_dx = 0, sticky_dy = 0;
    compute_sticky_offset(b, cr, &sticky_dx, &sticky_dy);
    if (isnan(sticky_dx) || isnan(sticky_dy)) {
        if (g_dbg_paint_x >= 0)
            g_printerr("[paint-nan-guard] sticky <%s>\n",
                       b->dom && b->dom->name ? b->dom->name : "?");
        sticky_dx = sticky_dy = 0;
    }
    gboolean has_sticky = (sticky_dx != 0 || sticky_dy != 0);
    if (has_sticky) {
        cairo_save(cr);
        cairo_translate(cr, sticky_dx, sticky_dy);
    }
    const ns_css_transform *anim_tf =
        g_paint_anim ? ns_anim_get_transform(g_paint_anim, b->dom) : NULL;
    ns_css_transform eff_tf;
    eff_tf.n_ops = 0;
    if (anim_tf ||
        (style && (style->values[NS_CSS_TRANSFORM] ||
                   style->values[NS_CSS_TRANSLATE] ||
                   style->values[NS_CSS_ROTATE] ||
                   style->values[NS_CSS_SCALE])))
        ns_css_style_effective_transform(style, anim_tf, &eff_tf);
    gboolean has_transform = eff_tf.n_ops > 0;
    if (b == g_paint_tex_root) {
        has_transform = FALSE;
    } else if (box_establishes_3d(b) ||
               (has_transform && ns_css_transform_is_3d(&eff_tf))) {
        double u0 = 0, u1 = 0, u2 = 0, u3 = 0;
        if (g_dbg_paint_x >= 0)
            cairo_clip_extents(cr, &u0, &u1, &u2, &u3);
        paint_3d_root(cr, b, highlight);
        if (g_dbg_paint_x >= 0) {
            double z0, z1, z2, z3;
            cairo_clip_extents(cr, &z0, &z1, &z2, &z3);
            if (fabs(z0 - u0) > 0.5 || fabs(z1 - u1) > 0.5 ||
                fabs(z2 - u2) > 0.5 || fabs(z3 - u3) > 0.5)
                g_printerr("[3droot-LEAK] <%s class=%.60s>\n",
                           b->dom && b->dom->name ? b->dom->name : "?",
                           b->dom ? (ns_element_get_attr(b->dom, "class")
                                         ?: "")
                                  : "");
        }
        if (has_sticky) cairo_restore(cr);
        return;
    }

    gboolean box_offscreen = FALSE;
    if (!has_transform && !has_sticky && !g_paint_no_cull) {
        const ns_css_value *posv = style ? style->values[NS_CSS_POSITION] : NULL;
        gboolean is_fixed = posv && posv->kind == NS_CSS_V_KEYWORD &&
                            posv->u.keyword && strcmp(posv->u.keyword, "fixed") == 0;
        if (!is_fixed) {
            double by = b->y + b->margin.top;
            double bh = b->content_height + b->padding.top + b->padding.bottom +
                        b->border.top + b->border.bottom;
            if (g_paint_have_clip &&
                (by + bh < g_paint_clip_y0 - g_paint_cull_margin ||
                 by > g_paint_clip_y1 + g_paint_cull_margin))
                box_offscreen = TRUE;
        }
    }
    if (box_offscreen && g_paint_collect_stats) g_paint_stats.offscreen++;

    if (grouped) {
        if (g_paint_collect_stats) g_paint_stats.grouped++;
        cairo_push_group(cr);
    }
    if (has_transform) {
        cairo_save(cr);
        double bx, by, bw, bh;
        box_border_rect(b, &bx, &by, &bw, &bh);
        double ox, oy, oz;
        box_transform_origin(b, bx, by, bw, bh, NS_CSS_TRANSFORM_ORIGIN,
                             &ox, &oy, &oz);
        ns_mat4 m;
        ns_css_transform_to_mat4(&eff_tf, bw, bh, &m);
        cairo_matrix_t cm;
        cairo_matrix_init(&cm, m.m[0], m.m[4], m.m[1], m.m[5],
                          m.m[3], m.m[7]);
        if (isnan(m.m[0]) || isnan(m.m[4]) || isnan(m.m[1]) ||
            isnan(m.m[5]) || isnan(m.m[3]) || isnan(m.m[7]) ||
            isnan(ox) || isnan(oy)) {
            if (g_dbg_paint_x >= 0)
                g_printerr("[paint-nan-guard] transform <%s>\n",
                           b->dom && b->dom->name ? b->dom->name : "?");
        } else if (fabs(m.m[0] * m.m[5] - m.m[1] * m.m[4]) < 1e-6) {
            cairo_restore(cr);
            if (grouped)
                cairo_pattern_destroy(cairo_pop_group(cr));
            if (has_sticky) cairo_restore(cr);
            return;
        } else {
            cairo_translate(cr, ox, oy);
            cairo_transform(cr, &cm);
            cairo_translate(cr, -ox, -oy);
        }
    }
    gboolean has_path_clip = FALSE;
    if ((b->kind == NS_BOX_BLOCK || b->kind == NS_BOX_TABLE ||
         b->kind == NS_BOX_TABLE_CAPTION || b->kind == NS_BOX_TABLE_CELL) &&
        style && style->values[NS_CSS_CLIP_PATH] &&
        style->values[NS_CSS_CLIP_PATH]->kind == NS_CSS_V_KEYWORD &&
        style->values[NS_CSS_CLIP_PATH]->u.keyword &&
        strcmp(style->values[NS_CSS_CLIP_PATH]->u.keyword, "none") != 0) {
        cairo_save(cr);
        if (apply_box_content_clip(cr, b)) has_path_clip = TRUE;
        else                                cairo_restore(cr);
    }
    if (!box_offscreen) {
        double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        if (g_dbg_paint_x >= 0)
            cairo_clip_extents(cr, &s0, &s1, &s2, &s3);
        if (b->kind == NS_BOX_BLOCK || b->kind == NS_BOX_TABLE ||
            b->kind == NS_BOX_TABLE_CAPTION ||
            b->kind == NS_BOX_TABLE_ROW || b->kind == NS_BOX_TABLE_CELL ||
            b->kind == NS_BOX_IMAGE || b->kind == NS_BOX_VIDEO ||
            b->kind == NS_BOX_MATH) {
            if (g_paint_collect_stats) g_paint_stats.blocks++;
            paint_block(cr, b);
            if (g_dbg_paint_x >= 0) {
                double t0, t1, t2, t3;
                cairo_clip_extents(cr, &t0, &t1, &t2, &t3);
                if (fabs(t0 - s0) > 0.5 || fabs(t1 - s1) > 0.5 ||
                    fabs(t2 - s2) > 0.5 || fabs(t3 - s3) > 0.5)
                    g_printerr("[block-LEAK] <%s class=%.60s>\n",
                               b->dom && b->dom->name ? b->dom->name : "?",
                               b->dom
                                   ? (ns_element_get_attr(b->dom, "class")
                                          ?: "")
                                   : "");
            }
        }
        if (b->kind == NS_BOX_BLOCK) {
            paint_marker(cr, b);
            paint_hr(cr, b);
        }
        if (b->kind == NS_BOX_INLINE) {
            if (g_paint_collect_stats) g_paint_stats.inlines++;
            paint_inline(cr, b, highlight);
            if (g_dbg_paint_x >= 0) {
                double t0, t1, t2, t3;
                cairo_clip_extents(cr, &t0, &t1, &t2, &t3);
                if (fabs(t0 - s0) > 0.5 || fabs(t1 - s1) > 0.5 ||
                    fabs(t2 - s2) > 0.5 || fabs(t3 - s3) > 0.5)
                    g_printerr("[inline-LEAK] <%s> text=%.30s\n",
                               b->dom && b->dom->name ? b->dom->name : "?",
                               b->text ? b->text : "");
            }
        }
        if (b->kind == NS_BOX_IMAGE) {
            if (g_paint_collect_stats) g_paint_stats.images++;
            paint_image(cr, b);
        }
        if (b->kind == NS_BOX_VIDEO) {
            if (g_paint_collect_stats) g_paint_stats.videos++;
            paint_video(cr, b);
        }
        if (b->kind == NS_BOX_MATH)
            paint_math(cr, b);
    }
    if (ns_node_is_element_named(b->dom, "canvas") && g_paint_js) {
        if (g_paint_collect_stats) g_paint_stats.canvases++;
        cairo_surface_t *surf = ns_js_canvas_surface(g_paint_js, b->dom);
        if (surf) {
            int sw = cairo_image_surface_get_width(surf);
            int sh = cairo_image_surface_get_height(surf);
            if (sw > 0 && sh > 0) {
                double dx = b->x + b->margin.left + b->border.left + b->padding.left;
                double dy = b->y + b->margin.top  + b->border.top  + b->padding.top;
                double dw = b->content_width > 0 ? b->content_width : sw;
                double dh = b->content_height > 0 ? b->content_height : sh;
                cairo_save(cr);
                cairo_translate(cr, dx, dy);
                cairo_scale(cr, dw / sw, dh / sh);
                cairo_set_source_surface(cr, surf, 0, 0);
                cairo_paint(cr);
                cairo_restore(cr);
            }
        }
    }

    guint n_children = 0;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        n_children++;
    paint_entry entries_buf[64];
    paint_entry *entries = n_children <= G_N_ELEMENTS(entries_buf)
        ? entries_buf : g_new(paint_entry, n_children);
    guint order = 0;
    gboolean any_z = FALSE;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling) {
        paint_entry e;
        e.box = c;
        e.order = order++;
        if (box_is_positioned(c)) {
            e.key = box_z_index(c);
            if (e.key != 0) any_z = TRUE;
        } else {
            e.key = 0;
        }
        entries[e.order] = e;
    }
    if (any_z) {
        if (g_paint_collect_stats) {
            g_paint_stats.sorted_parents++;
            g_paint_stats.sorted_children += n_children;
        }
        qsort(entries, n_children, sizeof(paint_entry), paint_entry_cmp);
    }
    const char *ovx = b->style ? ns_style_keyword(b->style, NS_CSS_OVERFLOW_X) : NULL;
    const char *ovy = b->style ? ns_style_keyword(b->style, NS_CSS_OVERFLOW_Y) : NULL;
    const char *ovs = b->style ? ns_style_keyword(b->style, NS_CSS_OVERFLOW) : NULL;
    if (!ovx) ovx = ovs;
    if (!ovy) ovy = ovs;
    gboolean is_root = (b->parent == NULL) ||
                       (b->dom && b->dom->name &&
                        (strcmp(b->dom->name, "html") == 0 ||
                         strcmp(b->dom->name, "body") == 0));
    gboolean clip_overflow = !is_root &&
                             (overflow_kw_clips(ovx) || overflow_kw_clips(ovy));
    if (clip_overflow &&
        (b->kind == NS_BOX_BLOCK || b->kind == NS_BOX_TABLE_CAPTION ||
         b->kind == NS_BOX_TABLE_CELL)) {
        double px = b->x + b->margin.left + b->border.left;
        double py = b->y + b->margin.top  + b->border.top;
        double pw = b->content_width + b->padding.left + b->padding.right;
        double ph = b->content_height + b->padding.top + b->padding.bottom;
        if (pw < 0) pw = 0;
        if (ph < 0) ph = 0;
        if (isnan(px) || isnan(py) || isnan(pw) || isnan(ph)) {
            if (g_dbg_paint_x >= 0)
                g_printerr("[paint-nan-guard] overflow-clip <%s>\n",
                           b->dom && b->dom->name ? b->dom->name : "?");
            px = py = 0; pw = ph = 0;
        }
        const ns_style *bs = b->style;
        gboolean explicit_h = bs &&
            ((bs->values[NS_CSS_MAX_HEIGHT] &&
              (bs->values[NS_CSS_MAX_HEIGHT]->kind == NS_CSS_V_LENGTH ||
               bs->values[NS_CSS_MAX_HEIGHT]->kind == NS_CSS_V_CALC)) ||
             (bs->values[NS_CSS_HEIGHT] &&
              (bs->values[NS_CSS_HEIGHT]->kind == NS_CSS_V_LENGTH ||
               bs->values[NS_CSS_HEIGHT]->kind == NS_CSS_V_CALC)));
        gboolean explicit_w = bs &&
            ((bs->values[NS_CSS_MAX_WIDTH] &&
              (bs->values[NS_CSS_MAX_WIDTH]->kind == NS_CSS_V_LENGTH ||
               bs->values[NS_CSS_MAX_WIDTH]->kind == NS_CSS_V_CALC)) ||
             (bs->values[NS_CSS_WIDTH] &&
              (bs->values[NS_CSS_WIDTH]->kind == NS_CSS_V_LENGTH ||
               bs->values[NS_CSS_WIDTH]->kind == NS_CSS_V_CALC)));
        if ((pw > 0 || explicit_w) && (ph > 0 || explicit_h)) {
            cairo_save(cr);
            corner_radii ov_radii = box_border_radii(b);
            if (!corner_radii_zero(ov_radii))
                rounded_rect_path(cr, px, py, pw, ph, ov_radii);
            else
                cairo_rectangle(cr, px, py, pw, ph);
            cairo_clip(cr);
            if (g_dbg_paint_x >= 0) {
                double ex0, ey0, ex1, ey1;
                cairo_clip_extents(cr, &ex0, &ey0, &ex1, &ey1);
                g_printerr("[paint-clip%s] <%s#%s> rect %.0f,%.0f %.0fx%.0f"
                           " -> clip %.0f,%.0f..%.0f,%.0f\n",
                           (ey1 - ey0 < 1 || ex1 - ex0 < 1) ? "-EMPTY" : "",
                           b->dom && b->dom->name ? b->dom->name : "?",
                           b->dom ? (ns_element_get_attr(b->dom, "id")
                                     ? ns_element_get_attr(b->dom, "id") : "")
                                  : "",
                           px, py, pw, ph, ex0, ey0, ex1, ey1);
            }
            if ((b->scroll_x != 0 || b->scroll_y != 0) &&
                !isnan(b->scroll_x) && !isnan(b->scroll_y))
                cairo_translate(cr, -b->scroll_x, -b->scroll_y);
            if (g_paint_collect_stats) g_paint_stats.overflow_clips++;
        } else {
            clip_overflow = FALSE;
        }
    } else {
        clip_overflow = FALSE;
    }
    gboolean own_layer_scope = b->parent == NULL || grouped || has_transform ||
                               clip_overflow || has_path_clip ||
                               b == g_paint_flush_box;
    GPtrArray *saved_layer_list = NULL;
    if (own_layer_scope) {
        saved_layer_list = g_paint_deferred_list;
        g_paint_deferred_list = NULL;
        g_paint_defer_depth++;
    }
    if (has_transform || has_sticky) g_paint_no_cull++;
    for (guint i = 0; i < n_children; i++)
        paint_walk(cr, entries[i].box, highlight);
    if (has_transform || has_sticky) g_paint_no_cull--;
    GPtrArray *deferred_mine = NULL;
    if (own_layer_scope) {
        deferred_mine = g_paint_deferred_list;
        g_paint_deferred_list = saved_layer_list;
        g_paint_defer_depth--;
        if (deferred_mine && !clip_overflow && !has_path_clip) {
            if (g_dbg_paint_x >= 0) {
                double fx0, fy0, fx1, fy1;
                cairo_clip_extents(cr, &fx0, &fy0, &fx1, &fy1);
                g_printerr("[paint-flush] owner=<%s#%s> n=%u "
                           "clip=%.0f,%.0f..%.0f,%.0f\n",
                           b->dom && b->dom->name ? b->dom->name : "?",
                           b->dom && ns_element_get_attr(b->dom, "id")
                               ? ns_element_get_attr(b->dom, "id") : "",
                           deferred_mine->len, fx0, fy0, fx1, fy1);
            }
            if (has_transform || has_sticky) g_paint_no_cull++;
            paint_flush_deferred(cr, deferred_mine, highlight);
            if (has_transform || has_sticky) g_paint_no_cull--;
            g_ptr_array_free(deferred_mine, TRUE);
            deferred_mine = NULL;
        }
    }
    const char *sbw_kw = b->style && b->style->values[NS_CSS_SCROLLBAR_WIDTH] &&
        b->style->values[NS_CSS_SCROLLBAR_WIDTH]->kind == NS_CSS_V_KEYWORD
        ? b->style->values[NS_CSS_SCROLLBAR_WIDTH]->u.keyword : NULL;
    gboolean sb_hidden = sbw_kw && strcmp(sbw_kw, "none") == 0;
    double sb_size = (sbw_kw && strcmp(sbw_kw, "thin") == 0) ? 5.0 : 8.0;
    double th_r = 0, th_g = 0, th_b = 0, th_a = 0.40;
    double tk_r = 0, tk_g = 0, tk_b = 0, tk_a = 0.06;
    const char *sbc_kw = b->style && b->style->values[NS_CSS_SCROLLBAR_COLOR] &&
        b->style->values[NS_CSS_SCROLLBAR_COLOR]->kind == NS_CSS_V_KEYWORD
        ? b->style->values[NS_CSS_SCROLLBAR_COLOR]->u.keyword : NULL;
    if (sbc_kw && g_ascii_strcasecmp(sbc_kw, "auto") != 0) {
        char **ct = g_strsplit_set(sbc_kw, " \t", -1);
        int idx = 0;
        for (int i = 0; ct[i] && idx < 2; i++) {
            char *t = g_strstrip(ct[i]);
            guint8 r, g, bb, a;
            if (*t && ns_css_parse_color(t, &r, &g, &bb, &a)) {
                if (idx == 0) { th_r = r/255.0; th_g = g/255.0; th_b = bb/255.0; th_a = a/255.0; }
                else          { tk_r = r/255.0; tk_g = g/255.0; tk_b = bb/255.0; tk_a = a/255.0; }
                idx++;
            }
        }
        g_strfreev(ct);
    }
    if (clip_overflow && b->scrolls && !sb_hidden &&
        (b->scroll_max_x > 0 || b->scroll_max_y > 0)) {
        double px = b->x + b->margin.left + b->border.left;
        double py = b->y + b->margin.top  + b->border.top;
        double pw = b->content_width + b->padding.left + b->padding.right;
        double ph = b->content_height + b->padding.top + b->padding.bottom;
        if (b->scroll_x != 0 || b->scroll_y != 0)
            cairo_translate(cr, b->scroll_x, b->scroll_y);
        if (b->scroll_max_y > 0 && ph > 16) {
            double track_w = sb_size;
            double track_x = px + pw - track_w - 1.0;
            double track_y = py + 1.0;
            double track_h = ph - 2.0;
            double total_h = ph + b->scroll_max_y;
            double thumb_h = track_h * (ph / total_h);
            if (thumb_h < 16.0) thumb_h = 16.0;
            if (thumb_h > track_h) thumb_h = track_h;
            double thumb_y = track_y +
                (track_h - thumb_h) * (b->scroll_y / b->scroll_max_y);
            cairo_save(cr);
            cairo_set_source_rgba(cr, tk_r, tk_g, tk_b, tk_a);
            cairo_rectangle(cr, track_x, track_y, track_w, track_h);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, th_r, th_g, th_b, th_a);
            cairo_rectangle(cr, track_x + 1, thumb_y, track_w - 2, thumb_h);
            cairo_fill(cr);
            cairo_restore(cr);
        }
        if (b->scroll_max_x > 0 && pw > 16) {
            double track_h = sb_size;
            double track_x = px + 1.0;
            double track_y = py + ph - track_h - 1.0;
            double track_w = pw - 2.0 -
                (b->scroll_max_y > 0 ? sb_size : 0.0);
            double total_w = pw + b->scroll_max_x;
            double thumb_w = track_w * (pw / total_w);
            if (thumb_w < 16.0) thumb_w = 16.0;
            if (thumb_w > track_w) thumb_w = track_w;
            double thumb_x = track_x +
                (track_w - thumb_w) * (b->scroll_x / b->scroll_max_x);
            cairo_save(cr);
            cairo_set_source_rgba(cr, tk_r, tk_g, tk_b, tk_a);
            cairo_rectangle(cr, track_x, track_y, track_w, track_h);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, th_r, th_g, th_b, th_a);
            cairo_rectangle(cr, thumb_x, track_y + 1, thumb_w, track_h - 2);
            cairo_fill(cr);
            cairo_restore(cr);
        }
    }

    if (b->kind == NS_BOX_BLOCK && b->style && b->columns >= 2) {
        double col_gap = 16;
        int n_cols = b->columns;
        (void)ns_css_used_column_count(b->style, b->content_width, &col_gap);
        {
            double rule_w = length_or(b->style->values[NS_CSS_COLUMN_RULE_WIDTH], 0);
            const ns_css_value *rstyle =
                b->style->values[NS_CSS_COLUMN_RULE_STYLE];
            gboolean rdrawable = rstyle && rstyle->kind == NS_CSS_V_KEYWORD &&
                rstyle->u.keyword && strcmp(rstyle->u.keyword, "none") != 0 &&
                strcmp(rstyle->u.keyword, "hidden") != 0;
            if (rule_w > 0 && rdrawable) {
                rgba rc = rgba_of(b->style->values[NS_CSS_COLUMN_RULE_COLOR],
                                  0.50, 0.50, 0.50, 1.0);
                double inx = b->x + b->margin.left + b->border.left + b->padding.left;
                double iny = b->y + b->margin.top  + b->border.top  + b->padding.top;
                double cw = b->content_width;
                double col_w = (cw - col_gap * (n_cols - 1)) / n_cols;
                cairo_save(cr);
                set_source_rgba(cr, rc);
                cairo_set_line_width(cr, rule_w);
                if (strcmp(rstyle->u.keyword, "dashed") == 0) {
                    double dashes[] = { rule_w * 3, rule_w * 2 };
                    cairo_set_dash(cr, dashes, 2, 0);
                } else if (strcmp(rstyle->u.keyword, "dotted") == 0) {
                    double dashes[] = { rule_w, rule_w };
                    cairo_set_dash(cr, dashes, 2, 0);
                }
                for (int i = 0; i < n_cols - 1; i++) {
                    double rx = inx + col_w * (i + 1) + col_gap * i + col_gap / 2.0;
                    cairo_move_to(cr, rx, iny);
                    cairo_line_to(cr, rx, iny + b->content_height);
                    cairo_stroke(cr);
                }
                cairo_restore(cr);
            }
        }
    }
    if (clip_overflow) cairo_restore(cr);
    if (entries != entries_buf) g_free(entries);

    if (has_path_clip) cairo_restore(cr);
    if (deferred_mine) {
        if (g_dbg_paint_x >= 0) {
            double fx0, fy0, fx1, fy1;
            cairo_clip_extents(cr, &fx0, &fy0, &fx1, &fy1);
            g_printerr("[paint-flush-postclip] owner=<%s#%s> n=%u "
                       "clip=%.0f,%.0f..%.0f,%.0f\n",
                       b->dom && b->dom->name ? b->dom->name : "?",
                       b->dom && ns_element_get_attr(b->dom, "id")
                           ? ns_element_get_attr(b->dom, "id") : "",
                       deferred_mine->len, fx0, fy0, fx1, fy1);
        }
        if (has_transform || has_sticky) g_paint_no_cull++;
        paint_flush_deferred(cr, deferred_mine, highlight);
        if (has_transform || has_sticky) g_paint_no_cull--;
        g_ptr_array_free(deferred_mine, TRUE);
    }

    if (has_transform) cairo_restore(cr);

    if (grouped) {
        cairo_pop_group_to_source(cr);
        cairo_operator_t saved_op = cairo_get_operator(cr);
        if (blend != CAIRO_OPERATOR_OVER) cairo_set_operator(cr, blend);
        cairo_pattern_t *mp = NULL;
        if (mask_grad) {
            double bx = b->x + b->margin.left, by = b->y + b->margin.top;
            double bw = b->content_width + b->padding.left + b->padding.right +
                        b->border.left + b->border.right;
            double bh = b->content_height + b->padding.top + b->padding.bottom +
                        b->border.top + b->border.bottom;
            mp = mask_gradient_pattern(&mask_v->u.gradient, bx, by, bw, bh);
        }
        if (mp) {
            cairo_mask(cr, mp);
            cairo_pattern_destroy(mp);
        } else {
            cairo_paint_with_alpha(cr, op);
        }
        if (blend != CAIRO_OPERATOR_OVER) cairo_set_operator(cr, saved_op);
    }

    if (has_sticky) cairo_restore(cr);
    if (g_dbg_paint_x >= 0) {
        double q0, q1, q2, q3;
        cairo_clip_extents(cr, &q0, &q1, &q2, &q3);
        if (fabs(q0 - dbg_e0) > 0.5 || fabs(q1 - dbg_e1) > 0.5 ||
            fabs(q2 - dbg_e2) > 0.5 || fabs(q3 - dbg_e3) > 0.5)
            g_printerr("[clip-LEAK] <%s#%s class=%.70s kind=%d grp=%d "
                       "tf=%d ov=%d pc=%d st=%d> "
                       "entry=%.0f,%.0f..%.0f,%.0f exit=%.0f,%.0f..%.0f,%.0f\n",
                       b->dom && b->dom->name ? b->dom->name : "?",
                       b->dom && b->dom->kind == NS_NODE_ELEMENT &&
                       ns_element_get_attr(b->dom, "id")
                           ? ns_element_get_attr(b->dom, "id") : "",
                       b->dom && b->dom->kind == NS_NODE_ELEMENT
                           ? (ns_element_get_attr(b->dom, "class") ?: "")
                           : "",
                       (int)b->kind, grouped, has_transform, clip_overflow,
                       has_path_clip, has_sticky,
                       dbg_e0, dbg_e1, dbg_e2, dbg_e3, q0, q1, q2, q3);
    }
}

static const ns_box *
find_element_box_named(const ns_box *b, const char *name)
{
    if (!b) return NULL;
    if (b->dom && b->dom->kind == NS_NODE_ELEMENT && b->dom->name &&
        strcmp(b->dom->name, name) == 0)
        return b;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling) {
        const ns_box *hit = find_element_box_named(c, name);
        if (hit) return hit;
    }
    return NULL;
}

static gboolean
box_solid_background(const ns_box *b, rgba *out)
{
    const ns_style *s = b ? b->style : NULL;
    if (s && s->values[NS_CSS_BACKGROUND_COLOR] &&
        s->values[NS_CSS_BACKGROUND_COLOR]->kind == NS_CSS_V_COLOR &&
        s->values[NS_CSS_BACKGROUND_COLOR]->u.color.a > 0) {
        *out = rgba_of(s->values[NS_CSS_BACKGROUND_COLOR], 1, 1, 1, 1);
        return TRUE;
    }
    return FALSE;
}

static gboolean
canvas_background_of(const ns_box *root, rgba *out)
{
    if (!root) return FALSE;
    const ns_box *html = find_element_box_named(root, "html");
    if (box_solid_background(html, out)) return TRUE;
    const ns_box *body = find_element_box_named(root, "body");
    if (box_solid_background(body, out)) return TRUE;
    return FALSE;
}

static const ns_box *
find_box_for_node(const ns_box *b, const ns_node *node)
{
    if (!b) return NULL;
    if (b->dom == node) return b;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling) {
        const ns_box *hit = find_box_for_node(c, node);
        if (hit) return hit;
    }
    return NULL;
}

static const ns_box *
top_layer_box(const ns_box *root)
{
    const ns_node *modal = ns_dom_active_modal();
    if (!modal) return NULL;
    return find_box_for_node(root, modal);
}

static void
paint_top_layer(cairo_t *cr, const ns_box *root, const char *highlight)
{
    const ns_box *top = top_layer_box(root);
    if (!top) return;
    const ns_style *s = top->style;
    const ns_style *bd = s ? s->backdrop : NULL;
    if (bd && bd->values[NS_CSS_BACKGROUND_COLOR]) {
        rgba c = rgba_of(bd->values[NS_CSS_BACKGROUND_COLOR], 0, 0, 0, 0);
        double x1, y1, x2, y2;
        cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
        cairo_save(cr);
        set_source_rgba(cr, c);
        cairo_rectangle(cr, x1, y1, x2 - x1, y2 - y1);
        cairo_fill(cr);
        cairo_restore(cr);
    }
    const ns_box *saved = g_paint_skip_box;
    const ns_box *saved_flush = g_paint_flush_box;
    g_paint_skip_box = NULL;
    g_paint_flush_box = top;
    paint_walk(cr, top, highlight);
    g_paint_flush_box = saved_flush;
    g_paint_skip_box = saved;
}

void
ns_paint(cairo_t *cr, const ns_box *root, const char *highlight_query)
{
    rgba bg = { 254.0 / 255, 254.0 / 255, 254.0 / 255, 1 };
    canvas_background_of(root, &bg);
    cairo_save(cr);
    set_source_rgba(cr, bg);
    cairo_paint(cr);
    cairo_restore(cr);
    paint_cache_clip(cr);
    g_paint_skip_box = top_layer_box(root);
    paint_walk(cr, root, highlight_query);
    g_paint_skip_box = NULL;
    paint_top_layer(cr, root, highlight_query);
    g_paint_have_clip = FALSE;
}

void
ns_paint_with_selection(cairo_t *cr, const ns_box *root,
                        const char *highlight_query,
                        const struct ns_selection *sel)
{
    rgba bg = { 254.0 / 255, 254.0 / 255, 254.0 / 255, 1 };
    canvas_background_of(root, &bg);
    cairo_save(cr);
    set_source_rgba(cr, bg);
    cairo_paint(cr);
    cairo_restore(cr);
    paint_cache_clip(cr);
    g_paint_skip_box = top_layer_box(root);
    paint_walk(cr, root, highlight_query);
    g_paint_skip_box = NULL;
    paint_top_layer(cr, root, highlight_query);
    if (sel) ns_selection_paint(cr, root, sel);
    g_paint_have_clip = FALSE;
}
