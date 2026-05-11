/* Nordstjernen — Cairo paint. */

#include "paint.h"

#include <pango/pangocairo.h>
#include <string.h>

#include "css.h"

typedef struct rgba {
    double r, g, b, a;
} rgba;

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

static double
length_or(const nd_css_value *v, double fallback)
{
    if (!v) return fallback;
    if (v->kind == ND_CSS_V_LENGTH) return v->u.length.v;
    return fallback;
}

static gboolean
keyword_is(const nd_css_value *v, const char *kw)
{
    return v && v->kind == ND_CSS_V_KEYWORD && kw &&
           strcmp(v->u.keyword, kw) == 0;
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
    rgba bg = rgba_of(s ? s->values[ND_CSS_BACKGROUND_COLOR] : NULL,
                      0, 0, 0, 0);
    if (bg.a > 0) {
        cairo_set_source_rgba(cr, bg.r, bg.g, bg.b, bg.a);
        cairo_rectangle(cr, border_x, border_y, border_w, border_h);
        cairo_fill(cr);
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
            cairo_move_to(cr, sides[i].x1, sides[i].y1);
            cairo_line_to(cr, sides[i].x2, sides[i].y2);
            cairo_stroke(cr);
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

static void
paint_inline(cairo_t *cr, const nd_box *b)
{
    if (!b->text || !*b->text) return;
    const nd_style *s = inherited_style(b);
    double font_size = length_or(s ? s->values[ND_CSS_FONT_SIZE] : NULL, 16);
    rgba color = rgba_of(s ? s->values[ND_CSS_COLOR] : NULL, 0.07, 0.07, 0.07, 1);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();

    const char *family = "serif";
    const nd_css_value *fam = s ? s->values[ND_CSS_FONT_FAMILY] : NULL;
    if (fam && fam->kind == ND_CSS_V_KEYWORD) family = fam->u.keyword;
    pango_font_description_set_family(desc, family);
    pango_font_description_set_absolute_size(desc, font_size * PANGO_SCALE);
    if (keyword_is(s ? s->values[ND_CSS_FONT_WEIGHT] : NULL, "bold"))
        pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    if (keyword_is(s ? s->values[ND_CSS_FONT_STYLE] : NULL, "italic"))
        pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);

    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    pango_layout_set_width(layout, (int)(b->content_width * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_text(layout, b->text, -1);

    if (b->links && b->links->len > 0) {
        PangoAttrList *attrs = pango_attr_list_new();
        for (guint i = 0; i < b->links->len; i++) {
            const nd_link_range *r = &g_array_index(b->links, nd_link_range, i);
            PangoAttribute *u = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
            u->start_index = (guint)r->start;
            u->end_index   = (guint)(r->start + r->len);
            pango_attr_list_insert(attrs, u);
            PangoAttribute *fg = pango_attr_foreground_new(0x0645, 0xad00, 0xad00);
            fg->start_index = (guint)r->start;
            fg->end_index   = (guint)(r->start + r->len);
            pango_attr_list_insert(attrs, fg);
        }
        pango_layout_set_attributes(layout, attrs);
        pango_attr_list_unref(attrs);
    }

    const nd_css_value *ta = s ? s->values[ND_CSS_TEXT_ALIGN] : NULL;
    if (keyword_is(ta, "center"))
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    else if (keyword_is(ta, "right"))
        pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
    else
        pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);

    cairo_save(cr);
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    cairo_move_to(cr, b->x, b->y);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    g_object_unref(layout);
}

static void
paint_walk(cairo_t *cr, const nd_box *b)
{
    if (!b) return;
    if (b->kind == ND_BOX_BLOCK)  paint_block(cr, b);
    if (b->kind == ND_BOX_INLINE) paint_inline(cr, b);
    for (const nd_box *c = b->first_child; c; c = c->next_sibling)
        paint_walk(cr, c);
}

void
nd_paint(cairo_t *cr, const nd_box *root)
{
    cairo_save(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_restore(cr);
    paint_walk(cr, root);
}
