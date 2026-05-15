/* Nordstjernen — Cairo paint.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "paint.h"

#include <gdk/gdk.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>

#include "css.h"
#include "image.h"
#include "selection.h"
#include "video.h"

typedef struct rgba {
    double r, g, b, a;
} rgba;

static gboolean g_caret_visible = TRUE;
static nd_js   *g_paint_js;

void
nd_paint_set_caret_visible(gboolean visible)
{
    g_caret_visible = visible;
}

void
nd_paint_set_js(nd_js *js)
{
    g_paint_js = js;
}

static rgba
rgba_of(const nd_css_value *v, double dr, double dg, double db, double da)
{
    rgba c = { dr, dg, db, da };
    if (!v || v->kind != ND_CSS_V_COLOR) return c;
    c.r = v->u.color.r / 255.0;
    c.g = v->u.color.g / 255.0;
    c.b = v->u.color.b / 255.0;
    c.a = v->u.color.a / 255.0;
    return c;
}

#define length_or nd_css_length_or

#define keyword_is nd_css_keyword_is

typedef struct corner_radii {
    double tl, tr, br, bl;
} corner_radii;

static double
positive_length(const nd_css_value *v)
{
    if (!v || v->kind != ND_CSS_V_LENGTH) return -1;
    double r = v->u.length.v;
    return r > 0 ? r : 0;
}

static corner_radii
box_border_radii(const nd_box *b)
{
    corner_radii c = {0};
    const nd_style *s = b ? b->style : NULL;
    if (!s) return c;
    double base = positive_length(s->values[ND_CSS_BORDER_RADIUS]);
    if (base < 0) base = 0;
    double tl = positive_length(s->values[ND_CSS_BORDER_TOP_LEFT_RADIUS]);
    double tr = positive_length(s->values[ND_CSS_BORDER_TOP_RIGHT_RADIUS]);
    double br = positive_length(s->values[ND_CSS_BORDER_BOTTOM_RIGHT_RADIUS]);
    double bl = positive_length(s->values[ND_CSS_BORDER_BOTTOM_LEFT_RADIUS]);
    c.tl = tl >= 0 ? tl : base;
    c.tr = tr >= 0 ? tr : base;
    c.br = br >= 0 ? br : base;
    c.bl = bl >= 0 ? bl : base;
    return c;
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
paint_block(cairo_t *cr, const nd_box *b)
{
    double border_x = b->x + b->margin.left;
    double border_y = b->y + b->margin.top;
    double border_w = b->content_width + b->padding.left + b->padding.right +
                      b->border.left + b->border.right;
    double border_h = b->content_height + b->padding.top + b->padding.bottom +
                      b->border.top + b->border.bottom;

    if (border_w <= 0 || border_h <= 0) return;

    const nd_style *s = b->style;
    corner_radii radii = box_border_radii(b);

    if (s && s->values[ND_CSS_BOX_SHADOW] &&
        s->values[ND_CSS_BOX_SHADOW]->kind == ND_CSS_V_SHADOW) {
        const nd_css_shadow *sh = &s->values[ND_CSS_BOX_SHADOW]->u.shadow;
        if (!sh->inset) {
            double sx = border_x + sh->x - sh->spread;
            double sy = border_y + sh->y - sh->spread;
            double sw = border_w + sh->spread * 2;
            double sh_h = border_h + sh->spread * 2;
            cairo_save(cr);
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
                    rounded_rect_path(cr,
                        sx - pad, sy - pad, sw + pad * 2, sh_h + pad * 2,
                        radii);
                    cairo_fill(cr);
                }
            } else {
                cairo_set_source_rgba(cr,
                    sh->r / 255.0, sh->g / 255.0, sh->b / 255.0,
                    sh->a / 255.0);
                rounded_rect_path(cr, sx, sy, sw, sh_h, radii);
                cairo_fill(cr);
            }
            cairo_restore(cr);
        }
    }

    rgba bg = rgba_of(s ? s->values[ND_CSS_BACKGROUND_COLOR] : NULL,
                      0, 0, 0, 0);
    if (bg.a > 0) {
        cairo_set_source_rgba(cr, bg.r, bg.g, bg.b, bg.a);
        rounded_rect_path(cr, border_x, border_y, border_w, border_h, radii);
        cairo_fill(cr);
    }

    if (b->bg_image) {
        nd_image *img = b->bg_image;
        if (img->loaded && img->texture) {
            int iw = gdk_texture_get_width(img->texture);
            int ih = gdk_texture_get_height(img->texture);
            if (iw > 0 && ih > 0) {
                const char *repeat = s ? nd_style_keyword(s, ND_CSS_BACKGROUND_REPEAT) : NULL;
                gboolean tile_x = TRUE, tile_y = TRUE;
                if (repeat) {
                    if (strcmp(repeat, "no-repeat") == 0) { tile_x = tile_y = FALSE; }
                    else if (strcmp(repeat, "repeat-x") == 0) { tile_y = FALSE; }
                    else if (strcmp(repeat, "repeat-y") == 0) { tile_x = FALSE; }
                }
                double draw_w = iw, draw_h = ih;
                const nd_css_value *sz = s ? s->values[ND_CSS_BACKGROUND_SIZE] : NULL;
                if (sz && sz->kind == ND_CSS_V_KEYWORD && sz->u.keyword) {
                    if (strcmp(sz->u.keyword, "cover") == 0) {
                        double sx = border_w / (double)iw;
                        double sy = border_h / (double)ih;
                        double sc = sx > sy ? sx : sy;
                        draw_w = iw * sc; draw_h = ih * sc;
                    } else if (strcmp(sz->u.keyword, "contain") == 0) {
                        double sx = border_w / (double)iw;
                        double sy = border_h / (double)ih;
                        double sc = sx < sy ? sx : sy;
                        draw_w = iw * sc; draw_h = ih * sc;
                    }
                } else if (sz && sz->kind == ND_CSS_V_LENGTH) {
                    if (sz->u.length.unit == ND_CSS_UNIT_PERCENT)
                        draw_w = draw_h = (sz->u.length.v / 100.0) * border_w;
                    else
                        draw_w = draw_h = sz->u.length.v;
                }
                if (draw_w < 1) draw_w = 1;
                if (draw_h < 1) draw_h = 1;
                double off_x = 0, off_y = 0;
                const nd_css_value *px = s ? s->values[ND_CSS_BACKGROUND_POSITION_X] : NULL;
                const nd_css_value *py = s ? s->values[ND_CSS_BACKGROUND_POSITION_Y] : NULL;
                if (px && px->kind == ND_CSS_V_LENGTH) {
                    if (px->u.length.unit == ND_CSS_UNIT_PERCENT)
                        off_x = (border_w - draw_w) * (px->u.length.v / 100.0);
                    else
                        off_x = px->u.length.v;
                }
                if (py && py->kind == ND_CSS_V_LENGTH) {
                    if (py->u.length.unit == ND_CSS_UNIT_PERCENT)
                        off_y = (border_h - draw_h) * (py->u.length.v / 100.0);
                    else
                        off_y = py->u.length.v;
                }
                cairo_surface_t *surf = cairo_image_surface_create(
                    CAIRO_FORMAT_ARGB32, iw, ih);
                if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
                    guchar *dst = cairo_image_surface_get_data(surf);
                    int dst_stride = cairo_image_surface_get_stride(surf);
                    gdk_texture_download(img->texture, dst, (gsize)dst_stride);
                    cairo_surface_mark_dirty(surf);
                    cairo_save(cr);
                    rounded_rect_path(cr, border_x, border_y, border_w, border_h, radii);
                    cairo_clip(cr);
                    cairo_pattern_t *pat = cairo_pattern_create_for_surface(surf);
                    cairo_pattern_set_extend(pat,
                        (tile_x || tile_y) ? CAIRO_EXTEND_REPEAT : CAIRO_EXTEND_NONE);
                    double sx = draw_w / (double)iw;
                    double sy = draw_h / (double)ih;
                    cairo_matrix_t m;
                    cairo_matrix_init_identity(&m);
                    cairo_matrix_scale(&m, 1.0 / sx, 1.0 / sy);
                    cairo_matrix_translate(&m, -(border_x + off_x), -(border_y + off_y));
                    cairo_pattern_set_matrix(pat, &m);
                    cairo_set_source(cr, pat);
                    if (tile_x && tile_y) {
                        cairo_paint(cr);
                    } else if (!tile_x && !tile_y) {
                        cairo_rectangle(cr, border_x + off_x, border_y + off_y,
                                        draw_w, draw_h);
                        cairo_fill(cr);
                    } else if (tile_x) {
                        cairo_rectangle(cr, border_x, border_y + off_y,
                                        border_w, draw_h);
                        cairo_fill(cr);
                    } else {
                        cairo_rectangle(cr, border_x + off_x, border_y,
                                        draw_w, border_h);
                        cairo_fill(cr);
                    }
                    cairo_pattern_destroy(pat);
                    cairo_restore(cr);
                }
                cairo_surface_destroy(surf);
            }
        }
    }

    if (s && s->values[ND_CSS_BACKGROUND_IMAGE] &&
        s->values[ND_CSS_BACKGROUND_IMAGE]->kind == ND_CSS_V_GRADIENT) {
        const nd_css_gradient *gr = &s->values[ND_CSS_BACKGROUND_IMAGE]->u.gradient;
        double rad = gr->angle_deg * G_PI / 180.0;
        double dx = sin(rad), dy = -cos(rad);
        double cx = border_x + border_w / 2.0;
        double cy = border_y + border_h / 2.0;
        double half = (fabs(dx) * border_w + fabs(dy) * border_h) / 2.0;
        cairo_pattern_t *pat = cairo_pattern_create_linear(
            cx - dx * half, cy - dy * half,
            cx + dx * half, cy + dy * half);
        for (int i = 0; i < gr->n_stops; i++) {
            const nd_css_gradient_stop *st = &gr->stops[i];
            cairo_pattern_add_color_stop_rgba(pat, st->pos,
                st->r / 255.0, st->g / 255.0, st->b / 255.0, st->a / 255.0);
        }
        cairo_save(cr);
        rounded_rect_path(cr, border_x, border_y, border_w, border_h, radii);
        cairo_clip(cr);
        cairo_set_source(cr, pat);
        cairo_paint(cr);
        cairo_pattern_destroy(pat);
        cairo_restore(cr);
    }

    if (s && s->values[ND_CSS_BOX_SHADOW] &&
        s->values[ND_CSS_BOX_SHADOW]->kind == ND_CSS_V_SHADOW &&
        s->values[ND_CSS_BOX_SHADOW]->u.shadow.inset) {
        const nd_css_shadow *sh = &s->values[ND_CSS_BOX_SHADOW]->u.shadow;
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

    if (s) {
        const struct {
            double w;
            const nd_css_value *col;
            double x1, y1, x2, y2;
        } sides[4] = {
            { b->border.top,
              s->values[ND_CSS_BORDER_TOP_COLOR],
              border_x, border_y,
              border_x + border_w, border_y },
            { b->border.right,
              s->values[ND_CSS_BORDER_RIGHT_COLOR],
              border_x + border_w, border_y,
              border_x + border_w, border_y + border_h },
            { b->border.bottom,
              s->values[ND_CSS_BORDER_BOTTOM_COLOR],
              border_x, border_y + border_h,
              border_x + border_w, border_y + border_h },
            { b->border.left,
              s->values[ND_CSS_BORDER_LEFT_COLOR],
              border_x, border_y,
              border_x, border_y + border_h },
        };
        for (int i = 0; i < 4; i++) {
            if (sides[i].w <= 0) continue;
            rgba c = rgba_of(sides[i].col, 0, 0, 0, 1);
            cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
            cairo_set_line_width(cr, sides[i].w);
            double x1 = sides[i].x1, y1 = sides[i].y1;
            double x2 = sides[i].x2, y2 = sides[i].y2;
            if (sides[i].w < 1.5) {
                if (x1 == x2) { x1 = floor(x1) + 0.5; x2 = x1; }
                if (y1 == y2) { y1 = floor(y1) + 0.5; y2 = y1; }
            }
            cairo_move_to(cr, x1, y1);
            cairo_line_to(cr, x2, y2);
            cairo_stroke(cr);
        }
        double ow = length_or(s->values[ND_CSS_OUTLINE_WIDTH], 0);
        const nd_css_value *ostyle = s->values[ND_CSS_OUTLINE_STYLE];
        gboolean ostyle_drawable = ostyle && ostyle->kind == ND_CSS_V_KEYWORD &&
            ostyle->u.keyword && strcmp(ostyle->u.keyword, "none") != 0 &&
            strcmp(ostyle->u.keyword, "hidden") != 0;
        if (ow > 0 && ostyle_drawable) {
            double off = length_or(s->values[ND_CSS_OUTLINE_OFFSET], 0);
            rgba oc = rgba_of(s->values[ND_CSS_OUTLINE_COLOR], 0, 0, 0, 1);
            cairo_save(cr);
            cairo_set_source_rgba(cr, oc.r, oc.g, oc.b, oc.a);
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

static const nd_style *
inherited_style(const nd_box *b)
{
    for (const nd_box *p = b->parent; p; p = p->parent)
        if (p->style) return p->style;
    return NULL;
}

static gsize
ascii_case_strstr_pos(const char *hay, gsize hay_len,
                      const char *needle, gsize needle_len,
                      gsize start)
{
    if (needle_len == 0 || start >= hay_len) return (gsize)-1;
    for (gsize i = start; i + needle_len <= hay_len; i++) {
        if (g_ascii_strncasecmp(hay + i, needle, needle_len) == 0)
            return i;
    }
    return (gsize)-1;
}

static void
paint_inline(cairo_t *cr, const nd_box *b, const char *highlight)
{
    if (!b->text || !*b->text) return;
    const nd_style *s = inherited_style(b);
    double font_size = length_or(s ? s->values[ND_CSS_FONT_SIZE] : NULL, 16);
    rgba color = rgba_of(s ? s->values[ND_CSS_COLOR] : NULL, 0.07, 0.07, 0.07, 1);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();

    const char *family = "sans-serif";
    const nd_css_value *fam = s ? s->values[ND_CSS_FONT_FAMILY] : NULL;
    if (fam && fam->kind == ND_CSS_V_KEYWORD) family = fam->u.keyword;
    pango_font_description_set_family(desc, family);
    pango_font_description_set_absolute_size(desc, font_size * PANGO_SCALE);
    const nd_css_value *fw = s ? s->values[ND_CSS_FONT_WEIGHT] : NULL;
    if (fw && fw->kind == ND_CSS_V_KEYWORD && fw->u.keyword) {
        const char *k = fw->u.keyword;
        int weight = 0;
        if (strcmp(k, "bold") == 0 || strcmp(k, "bolder") == 0) weight = PANGO_WEIGHT_BOLD;
        else if (g_ascii_isdigit(k[0])) {
            int n = nd_parse_int(k, 0, 0, 1000);
            if (n >= 600) weight = PANGO_WEIGHT_BOLD;
            else if (n <= 300) weight = PANGO_WEIGHT_LIGHT;
        }
        if (weight) pango_font_description_set_weight(desc, weight);
    }
    if (keyword_is(s ? s->values[ND_CSS_FONT_STYLE] : NULL, "italic"))
        pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);

    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    pango_layout_set_width(layout, (int)(b->content_width * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_text(layout, b->text, -1);

    PangoAttrList *attrs = pango_attr_list_new();
    if (b->links) {
        for (guint i = 0; i < b->links->len; i++) {
            const nd_link_range *r = &g_array_index(b->links, nd_link_range, i);
            PangoAttribute *u = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
            u->start_index = (guint)r->start;
            u->end_index   = (guint)(r->start + r->len);
            pango_attr_list_insert(attrs, u);
            PangoAttribute *fg = pango_attr_foreground_new(0x1111, 0x6868, 0xcccc);
            fg->start_index = (guint)r->start;
            fg->end_index   = (guint)(r->start + r->len);
            pango_attr_list_insert(attrs, fg);
        }
    }
    if (b->attrs) {
        for (gint ii = (gint)b->attrs->len - 1; ii >= 0; ii--) {
            const nd_inline_attr *r = &g_array_index(b->attrs, nd_inline_attr, (guint)ii);
            PangoAttribute *a = NULL;
            switch (r->kind) {
            case ND_INLINE_BOLD:
                a = pango_attr_weight_new(PANGO_WEIGHT_BOLD); break;
            case ND_INLINE_ITALIC:
                a = pango_attr_style_new(PANGO_STYLE_ITALIC); break;
            case ND_INLINE_MONOSPACE:
                a = pango_attr_family_new("monospace"); break;
            case ND_INLINE_UNDERLINE:
                a = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE); break;
            case ND_INLINE_STRIKETHROUGH:
                a = pango_attr_strikethrough_new(TRUE); break;
            case ND_INLINE_INPUT_FIELD:
            case ND_INLINE_INPUT_FIELD_FOCUSED:
            case ND_INLINE_BUTTON:
                break;
            case ND_INLINE_FONT_SIZE:
                a = pango_attr_size_new_absolute(
                    (int)(r->font_size_px * PANGO_SCALE));
                break;
            case ND_INLINE_COLOR:
                a = pango_attr_foreground_new(
                    (guint16)(r->r * 0x101),
                    (guint16)(r->g * 0x101),
                    (guint16)(r->b * 0x101));
                break;
            case ND_INLINE_BG_COLOR:
                a = pango_attr_background_new(
                    (guint16)(r->r * 0x101),
                    (guint16)(r->g * 0x101),
                    (guint16)(r->b * 0x101));
                break;
            case ND_INLINE_FONT_FAMILY:
                if (r->family) a = pango_attr_family_new(r->family);
                break;
            case ND_INLINE_SUPERSCRIPT: {
                PangoAttribute *rise = pango_attr_rise_new(4000);
                rise->start_index = (guint)r->start;
                rise->end_index   = (guint)(r->start + r->len);
                pango_attr_list_insert(attrs, rise);
                a = pango_attr_scale_new(0.75);
                break;
            }
            case ND_INLINE_SUBSCRIPT: {
                PangoAttribute *rise = pango_attr_rise_new(-3000);
                rise->start_index = (guint)r->start;
                rise->end_index   = (guint)(r->start + r->len);
                pango_attr_list_insert(attrs, rise);
                a = pango_attr_scale_new(0.75);
                break;
            }
            case ND_INLINE_SMALL_CAPS:
                a = pango_attr_variant_new(PANGO_VARIANT_SMALL_CAPS);
                break;
            case ND_INLINE_CARET:
                break;
            }
            if (a) {
                a->start_index = (guint)r->start;
                a->end_index   = (guint)(r->start + r->len);
                pango_attr_list_insert(attrs, a);
            }
        }
    }
    if (highlight && *highlight && b->text) {
        gsize text_len = strlen(b->text);
        gsize needle_len = strlen(highlight);
        gsize pos = 0;
        while ((pos = ascii_case_strstr_pos(b->text, text_len,
                                            highlight, needle_len, pos)) != (gsize)-1) {
            PangoAttribute *bg = pango_attr_background_new(0xffff, 0xff00, 0x6600);
            bg->start_index = (guint)pos;
            bg->end_index   = (guint)(pos + needle_len);
            pango_attr_list_insert(attrs, bg);
            pos += needle_len > 0 ? needle_len : 1;
        }
    }
    pango_layout_set_attributes(layout, attrs);
    pango_attr_list_unref(attrs);

    int pw, ph;
    pango_layout_get_pixel_size(layout, &pw, &ph);
    double y_offset = (b->content_height - (double)ph) * 0.5;
    if (y_offset < 0) y_offset = 0;
    double y_origin = b->y + y_offset;

    const nd_css_value *ta = s ? s->values[ND_CSS_TEXT_ALIGN] : NULL;
    if (keyword_is(ta, "center"))
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    else if (keyword_is(ta, "right") || keyword_is(ta, "end"))
        pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
    else
        pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    if (keyword_is(ta, "justify"))
        pango_layout_set_justify(layout, TRUE);

    if (b->attrs) {
        for (guint i = 0; i < b->attrs->len; i++) {
            const nd_inline_attr *r = &g_array_index(b->attrs, nd_inline_attr, i);
            if (r->kind != ND_INLINE_INPUT_FIELD &&
                r->kind != ND_INLINE_INPUT_FIELD_FOCUSED &&
                r->kind != ND_INLINE_BUTTON)
                continue;
            PangoRectangle r0, r1;
            pango_layout_index_to_pos(layout, (int)r->start, &r0);
            pango_layout_index_to_pos(layout, (int)(r->start + r->len - 1), &r1);
            double x0 = b->x + (double)r0.x / PANGO_SCALE - 4;
            double y0 = y_origin + (double)r0.y / PANGO_SCALE - 2;
            double x1 = b->x + (double)(r1.x + r1.width) / PANGO_SCALE + 4;
            double y1 = y_origin + (double)(r0.y + r0.height) / PANGO_SCALE + 2;
            if (x1 < x0) { double t = x0; x0 = x1; x1 = t; }
            if (r->kind == ND_INLINE_INPUT_FIELD_FOCUSED) {
                cairo_save(cr);
                cairo_set_source_rgb(cr, 0.13, 0.36, 0.80);
                cairo_set_line_width(cr, 2.0);
                cairo_rectangle(cr, x0 + 0.5, y0 + 0.5,
                                x1 - x0 - 1, y1 - y0 - 1);
                cairo_stroke(cr);
                cairo_restore(cr);
            }
        }
    }

    cairo_save(cr);
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    cairo_move_to(cr, b->x, y_origin);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    if (b->attrs) {
        for (guint i = 0; i < b->attrs->len; i++) {
            const nd_inline_attr *r = &g_array_index(b->attrs, nd_inline_attr, i);
            if (r->kind != ND_INLINE_CARET) continue;
            if (!g_caret_visible) continue;
            if (b->text && r->start >= strlen(b->text)) continue;
            PangoRectangle pos;
            pango_layout_index_to_pos(layout, (int)r->start, &pos);
            double cx = b->x + (double)pos.x / PANGO_SCALE;
            double cy = y_origin + (double)pos.y / PANGO_SCALE;
            double ch = (double)pos.height / PANGO_SCALE;
            if (ch < 1.0) ch = 14.0;
            cairo_save(cr);
            cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
            cairo_set_line_width(cr, 1.5);
            cairo_move_to(cr, cx + 0.5, cy);
            cairo_line_to(cr, cx + 0.5, cy + ch);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    }

    g_object_unref(layout);
}

PangoLayout *
nd_paint_build_inline_layout(cairo_t *cr, const nd_box *b)
{
    if (!b || !b->text) return NULL;
    const nd_style *s = inherited_style(b);
    double font_size = length_or(s ? s->values[ND_CSS_FONT_SIZE] : NULL, 16);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    const char *family = "sans-serif";
    const nd_css_value *fam = s ? s->values[ND_CSS_FONT_FAMILY] : NULL;
    if (fam && fam->kind == ND_CSS_V_KEYWORD) family = fam->u.keyword;
    pango_font_description_set_family(desc, family);
    pango_font_description_set_absolute_size(desc, font_size * PANGO_SCALE);
    const nd_css_value *fw = s ? s->values[ND_CSS_FONT_WEIGHT] : NULL;
    if (fw && fw->kind == ND_CSS_V_KEYWORD && fw->u.keyword) {
        const char *k = fw->u.keyword;
        int weight = 0;
        if (strcmp(k, "bold") == 0 || strcmp(k, "bolder") == 0) weight = PANGO_WEIGHT_BOLD;
        else if (g_ascii_isdigit(k[0])) {
            int n = nd_parse_int(k, 0, 0, 1000);
            if (n >= 600) weight = PANGO_WEIGHT_BOLD;
            else if (n <= 300) weight = PANGO_WEIGHT_LIGHT;
        }
        if (weight) pango_font_description_set_weight(desc, weight);
    }
    if (keyword_is(s ? s->values[ND_CSS_FONT_STYLE] : NULL, "italic"))
        pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_width(layout, (int)(b->content_width * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_text(layout, b->text, -1);

    PangoAttrList *attrs = pango_attr_list_new();
    if (b->attrs) {
        for (gint ii = (gint)b->attrs->len - 1; ii >= 0; ii--) {
            const nd_inline_attr *r = &g_array_index(b->attrs, nd_inline_attr, (guint)ii);
            PangoAttribute *a = NULL;
            switch (r->kind) {
            case ND_INLINE_BOLD:      a = pango_attr_weight_new(PANGO_WEIGHT_BOLD); break;
            case ND_INLINE_ITALIC:    a = pango_attr_style_new(PANGO_STYLE_ITALIC); break;
            case ND_INLINE_MONOSPACE: a = pango_attr_family_new("monospace"); break;
            case ND_INLINE_FONT_SIZE:
                a = pango_attr_size_new_absolute((int)(r->font_size_px * PANGO_SCALE));
                break;
            case ND_INLINE_FONT_FAMILY:
                if (r->family) a = pango_attr_family_new(r->family);
                break;
            case ND_INLINE_SUPERSCRIPT:
            case ND_INLINE_SUBSCRIPT:
                a = pango_attr_scale_new(0.75); break;
            case ND_INLINE_SMALL_CAPS:
                a = pango_attr_variant_new(PANGO_VARIANT_SMALL_CAPS); break;
            default: break;
            }
            if (a) {
                a->start_index = (guint)r->start;
                a->end_index   = (guint)(r->start + r->len);
                pango_attr_list_insert(attrs, a);
            }
        }
    }
    pango_layout_set_attributes(layout, attrs);
    pango_attr_list_unref(attrs);

    const nd_css_value *ta = s ? s->values[ND_CSS_TEXT_ALIGN] : NULL;
    if (keyword_is(ta, "center"))
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    else if (keyword_is(ta, "right") || keyword_is(ta, "end"))
        pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
    else
        pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    return layout;
}

gboolean
nd_paint_inline_xy_to_byte(const nd_box *b, double rel_x, double rel_y,
                           gsize *out_byte)
{
    if (!b || !b->text || !*b->text) return FALSE;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t *cr = cairo_create(surf);
    PangoLayout *layout = nd_paint_build_inline_layout(cr, b);
    if (!layout) {
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
        return FALSE;
    }

    int index = 0, trailing = 0;
    pango_layout_xy_to_index(layout, (int)(rel_x * PANGO_SCALE),
                             (int)(rel_y * PANGO_SCALE),
                             &index, &trailing);
    if (out_byte) *out_byte = (gsize)index + (gsize)trailing;

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return TRUE;
}

static void
paint_image(cairo_t *cr, const nd_box *b)
{
    nd_image *img = b->image;
    cairo_save(cr);
    if (img && img->loaded && img->texture) {
        int iw = gdk_texture_get_width(img->texture);
        int ih = gdk_texture_get_height(img->texture);
        if (iw <= 0 || ih <= 0) { cairo_restore(cr); return; }
        cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
        if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(surf);
            cairo_restore(cr);
            return;
        }
        guchar *dst = cairo_image_surface_get_data(surf);
        int dst_stride = cairo_image_surface_get_stride(surf);
        gdk_texture_download(img->texture, dst, (gsize)dst_stride);
        cairo_surface_mark_dirty(surf);
        cairo_translate(cr, b->x, b->y);
        cairo_scale(cr, b->content_width / iw, b->content_height / ih);
        cairo_set_source_surface(cr, surf, 0, 0);
        cairo_paint(cr);
        cairo_surface_destroy(surf);
    } else {
        cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
        cairo_rectangle(cr, b->x, b->y, b->content_width, b->content_height);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        const char *alt = b->dom ? nd_element_get_attr(b->dom, "alt") : NULL;
        if (alt && *alt && b->content_width > 24 && b->content_height > 16) {
            PangoLayout *layout = pango_cairo_create_layout(cr);
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
paint_video(cairo_t *cr, const nd_box *b)
{
    nd_video *v = b->video;
    GdkTexture *tex = NULL;
    if (v) tex = v->frame_texture ? v->frame_texture : v->poster_texture;
    cairo_save(cr);
    if (tex) {
        int iw = gdk_texture_get_width(tex);
        int ih = gdk_texture_get_height(tex);
        if (iw > 0 && ih > 0) {
            cairo_surface_t *surf = cairo_image_surface_create(
                CAIRO_FORMAT_ARGB32, iw, ih);
            if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
                guchar *dst = cairo_image_surface_get_data(surf);
                int dst_stride = cairo_image_surface_get_stride(surf);
                gdk_texture_download(tex, dst, (gsize)dst_stride);
                cairo_surface_mark_dirty(surf);
                cairo_translate(cr, b->x, b->y);
                cairo_scale(cr, b->content_width / iw, b->content_height / ih);
                cairo_set_source_surface(cr, surf, 0, 0);
                cairo_paint(cr);
            }
            cairo_surface_destroy(surf);
        }
    } else {
        cairo_set_source_rgb(cr, 0.10, 0.10, 0.10);
        cairo_rectangle(cr, b->x, b->y, b->content_width, b->content_height);
        cairo_fill(cr);
        double cx = b->x + b->content_width  / 2;
        double cy = b->y + b->content_height / 2;
        double r  = b->content_height / 6;
        if (r > 4) {
            cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
            cairo_move_to(cr, cx - r * 0.6, cy - r);
            cairo_line_to(cr, cx + r,       cy);
            cairo_line_to(cr, cx - r * 0.6, cy + r);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
    }
    cairo_restore(cr);
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

static void
paint_marker(cairo_t *cr, const nd_box *b)
{
    if (!b->dom || !b->dom->name || strcmp(b->dom->name, "li") != 0) return;
    const nd_node *parent = b->dom->parent;
    if (!parent || !parent->name) return;
    const nd_style *s = b->style;
    const nd_css_value *lst = s ? s->values[ND_CSS_LIST_STYLE_TYPE] : NULL;
    const char *style_kw = NULL;
    if (lst && lst->kind == ND_CSS_V_KEYWORD) style_kw = lst->u.keyword;
    if (style_kw && strcmp(style_kw, "none") == 0) return;

    double font_size = length_or(s ? s->values[ND_CSS_FONT_SIZE] : NULL, 16);
    double cy = b->y + b->margin.top + b->padding.top + font_size * 0.7;
    double cx = b->x + b->margin.left + b->padding.left - font_size * 0.8;
    rgba color = rgba_of(s ? s->values[ND_CSS_COLOR] : NULL, 0.1, 0.1, 0.1, 1);
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);

    gboolean ordered = strcmp(parent->name, "ol") == 0;
    if (style_kw &&
        (strcmp(style_kw, "decimal") == 0 ||
         strcmp(style_kw, "upper-alpha") == 0 || strcmp(style_kw, "lower-alpha") == 0 ||
         strcmp(style_kw, "upper-latin") == 0 || strcmp(style_kw, "lower-latin") == 0 ||
         strcmp(style_kw, "upper-roman") == 0 || strcmp(style_kw, "lower-roman") == 0))
        ordered = TRUE;

    if (ordered) {
        int start = 1;
        const char *start_attr = nd_element_get_attr(parent, "start");
        if (start_attr) start = nd_parse_int(start_attr, 1, -1000000, 1000000);
        gboolean reversed = nd_element_get_attr(parent, "reversed") != NULL;
        const char *li_val = nd_element_get_attr(b->dom, "value");
        int n;
        if (li_val) {
            n = nd_parse_int(li_val, 1, -1000000, 1000000);
        } else if (reversed) {
            int total = 0;
            for (const nd_node *p = parent->first_child; p; p = p->next_sibling)
                if (p->kind == ND_NODE_ELEMENT && p->name &&
                    strcmp(p->name, "li") == 0) total++;
            n = start_attr ? start : total;
            for (const nd_node *p = b->dom->prev_sibling; p; p = p->prev_sibling)
                if (p->kind == ND_NODE_ELEMENT && p->name &&
                    strcmp(p->name, "li") == 0) n--;
        } else {
            n = start;
            for (const nd_node *p = b->dom->prev_sibling; p; p = p->prev_sibling)
                if (p->kind == ND_NODE_ELEMENT && p->name &&
                    strcmp(p->name, "li") == 0) n++;
        }
        const char *type_attr = nd_element_get_attr(parent, "type");
        const char *kind = style_kw;
        if (!kind && type_attr && *type_attr) {
            switch (type_attr[0]) {
                case 'A': kind = "upper-alpha"; break;
                case 'a': kind = "lower-alpha"; break;
                case 'I': kind = "upper-roman"; break;
                case 'i': kind = "lower-roman"; break;
                default:  kind = "decimal";     break;
            }
        }
        char buf[32];
        if (kind && (strcmp(kind, "upper-alpha") == 0 || strcmp(kind, "upper-latin") == 0))
            alpha_label(n, TRUE, buf, sizeof buf);
        else if (kind && (strcmp(kind, "lower-alpha") == 0 || strcmp(kind, "lower-latin") == 0))
            alpha_label(n, FALSE, buf, sizeof buf);
        else if (kind && strcmp(kind, "upper-roman") == 0)
            roman_numeral(n, TRUE, buf, sizeof buf);
        else if (kind && strcmp(kind, "lower-roman") == 0)
            roman_numeral(n, FALSE, buf, sizeof buf);
        else
            g_snprintf(buf, sizeof buf, "%d", n);
        char with_dot[40];
        g_snprintf(with_dot, sizeof with_dot, "%s.", buf);
        cairo_move_to(cr, cx - font_size * 0.5, cy);
        cairo_set_font_size(cr, font_size);
        cairo_show_text(cr, with_dot);
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
paint_hr(cairo_t *cr, const nd_box *b)
{
    if (!b->dom || !b->dom->name || strcmp(b->dom->name, "hr") != 0) return;
    if (b->border.top > 0 || b->border.bottom > 0 ||
        b->border.left > 0 || b->border.right > 0) return;
    double h = 1.0;
    const nd_style *s = b->style;
    if (s && s->values[ND_CSS_HEIGHT] &&
        s->values[ND_CSS_HEIGHT]->kind == ND_CSS_V_LENGTH) {
        double hv = s->values[ND_CSS_HEIGHT]->u.length.v;
        if (hv > 0) h = hv;
    }
    if (h > 24) h = 24;
    double y = b->y + b->margin.top + 4;
    double x0 = b->x + b->margin.left;
    double x1 = x0 + b->content_width;
    rgba color = rgba_of(s ? s->values[ND_CSS_COLOR] : NULL, 0.65, 0.65, 0.65, 1);
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
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
box_is_hidden(const nd_box *b)
{
    const nd_style *s = b ? b->style : NULL;
    if (!s) return FALSE;
    const nd_css_value *v = s->values[ND_CSS_VISIBILITY];
    return v && v->kind == ND_CSS_V_KEYWORD && v->u.keyword &&
           (strcmp(v->u.keyword, "hidden") == 0 ||
            strcmp(v->u.keyword, "collapse") == 0);
}

static double
box_opacity(const nd_box *b)
{
    const nd_style *s = b ? b->style : NULL;
    if (!s) return 1.0;
    const nd_css_value *v = s->values[ND_CSS_OPACITY];
    if (!v) return 1.0;
    if (v->kind == ND_CSS_V_LENGTH) {
        double o = v->u.length.v;
        if (o < 0) o = 0;
        if (o > 1) o = 1;
        return o;
    }
    return 1.0;
}

static gboolean
box_is_positioned(const nd_box *b)
{
    const nd_style *s = b ? b->style : NULL;
    if (!s) return FALSE;
    const nd_css_value *v = s->values[ND_CSS_POSITION];
    if (!v || v->kind != ND_CSS_V_KEYWORD || !v->u.keyword) return FALSE;
    const char *kw = v->u.keyword;
    return strcmp(kw, "relative") == 0 || strcmp(kw, "absolute") == 0 ||
           strcmp(kw, "fixed") == 0    || strcmp(kw, "sticky") == 0;
}

static int
box_z_index(const nd_box *b)
{
    const nd_style *s = b ? b->style : NULL;
    if (!s) return 0;
    const nd_css_value *v = s->values[ND_CSS_Z_INDEX];
    if (!v || v->kind != ND_CSS_V_LENGTH) return 0;
    return (int)v->u.length.v;
}

typedef struct paint_entry {
    const nd_box *box;
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

static void
paint_walk(cairo_t *cr, const nd_box *b, const char *highlight)
{
    if (!b) return;
    if (box_is_hidden(b)) return;
    double op = box_opacity(b);
    gboolean grouped = op < 0.999;
    if (grouped) cairo_push_group(cr);
    if (b->kind == ND_BOX_BLOCK || b->kind == ND_BOX_TABLE ||
        b->kind == ND_BOX_TABLE_ROW || b->kind == ND_BOX_TABLE_CELL) {
        paint_block(cr, b);
    }
    if (b->kind == ND_BOX_BLOCK) {
        paint_marker(cr, b);
        paint_hr(cr, b);
    }
    if (b->kind == ND_BOX_INLINE) paint_inline(cr, b, highlight);
    if (b->kind == ND_BOX_IMAGE)  paint_image(cr, b);
    if (b->kind == ND_BOX_VIDEO)  paint_video(cr, b);
    if (b->dom && b->dom->kind == ND_NODE_ELEMENT && b->dom->name &&
        strcmp(b->dom->name, "canvas") == 0 && g_paint_js) {
        cairo_surface_t *surf = nd_js_canvas_surface(g_paint_js, b->dom);
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

    GArray *entries = g_array_new(FALSE, FALSE, sizeof(paint_entry));
    guint order = 0;
    gboolean any_z = FALSE;
    for (const nd_box *c = b->first_child; c; c = c->next_sibling) {
        paint_entry e;
        e.box = c;
        e.order = order++;
        if (box_is_positioned(c)) {
            e.key = box_z_index(c);
            if (e.key != 0) any_z = TRUE;
        } else {
            e.key = 0;
        }
        g_array_append_val(entries, e);
    }
    if (any_z) g_array_sort(entries, paint_entry_cmp);
    for (guint i = 0; i < entries->len; i++) {
        const paint_entry *e = &g_array_index(entries, paint_entry, i);
        paint_walk(cr, e->box, highlight);
    }
    g_array_free(entries, TRUE);

    if (grouped) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, op);
    }
}

static gboolean
canvas_background_of(const nd_box *root, rgba *out)
{
    if (!root) return FALSE;
    for (const nd_box *c = root->first_child; c; c = c->next_sibling) {
        if (!c->dom || c->dom->kind != ND_NODE_ELEMENT || !c->dom->name) continue;
        if (strcmp(c->dom->name, "html") != 0) continue;
        const nd_style *hs = c->style;
        if (hs && hs->values[ND_CSS_BACKGROUND_COLOR] &&
            hs->values[ND_CSS_BACKGROUND_COLOR]->kind == ND_CSS_V_COLOR &&
            hs->values[ND_CSS_BACKGROUND_COLOR]->u.color.a > 0) {
            *out = rgba_of(hs->values[ND_CSS_BACKGROUND_COLOR], 1, 1, 1, 1);
            return TRUE;
        }
        for (const nd_box *b = c->first_child; b; b = b->next_sibling) {
            if (!b->dom || b->dom->kind != ND_NODE_ELEMENT || !b->dom->name) continue;
            if (strcmp(b->dom->name, "body") != 0) continue;
            const nd_style *bs = b->style;
            if (bs && bs->values[ND_CSS_BACKGROUND_COLOR] &&
                bs->values[ND_CSS_BACKGROUND_COLOR]->kind == ND_CSS_V_COLOR &&
                bs->values[ND_CSS_BACKGROUND_COLOR]->u.color.a > 0) {
                *out = rgba_of(bs->values[ND_CSS_BACKGROUND_COLOR], 1, 1, 1, 1);
                return TRUE;
            }
        }
    }
    return FALSE;
}

void
nd_paint(cairo_t *cr, const nd_box *root, const char *highlight_query)
{
    rgba bg = { 1, 1, 1, 1 };
    canvas_background_of(root, &bg);
    cairo_save(cr);
    cairo_set_source_rgba(cr, bg.r, bg.g, bg.b, bg.a);
    cairo_paint(cr);
    cairo_restore(cr);
    paint_walk(cr, root, highlight_query);
}

void
nd_paint_with_selection(cairo_t *cr, const nd_box *root,
                        const char *highlight_query,
                        const struct nd_selection *sel)
{
    rgba bg = { 1, 1, 1, 1 };
    canvas_background_of(root, &bg);
    cairo_save(cr);
    cairo_set_source_rgba(cr, bg.r, bg.g, bg.b, bg.a);
    cairo_paint(cr);
    cairo_restore(cr);
    paint_walk(cr, root, highlight_query);
    if (sel) nd_selection_paint(cr, root, sel);
}
