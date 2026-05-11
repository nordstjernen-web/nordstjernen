/* Nordstjernen — Cairo paint. */

#include "paint.h"

#include <gdk/gdk.h>
#include <pango/pangocairo.h>
#include <string.h>

#include "css.h"
#include "image.h"

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

    PangoAttrList *attrs = pango_attr_list_new();
    if (b->links) {
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
    }
    if (b->attrs) {
        for (guint i = 0; i < b->attrs->len; i++) {
            const nd_inline_attr *r = &g_array_index(b->attrs, nd_inline_attr, i);
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
                a = pango_attr_background_new(0xffff, 0xffff, 0xffff); break;
            case ND_INLINE_BUTTON:
                a = pango_attr_background_new(0xe6e6, 0xe6e6, 0xe6e6); break;
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

    const nd_css_value *ta = s ? s->values[ND_CSS_TEXT_ALIGN] : NULL;
    if (keyword_is(ta, "center"))
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    else if (keyword_is(ta, "right"))
        pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
    else
        pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);

    if (b->attrs) {
        for (guint i = 0; i < b->attrs->len; i++) {
            const nd_inline_attr *r = &g_array_index(b->attrs, nd_inline_attr, i);
            if (r->kind != ND_INLINE_INPUT_FIELD && r->kind != ND_INLINE_BUTTON)
                continue;
            PangoRectangle r0, r1;
            pango_layout_index_to_pos(layout, (int)r->start, &r0);
            pango_layout_index_to_pos(layout, (int)(r->start + r->len - 1), &r1);
            double x0 = b->x + (double)r0.x / PANGO_SCALE - 4;
            double y0 = b->y + (double)r0.y / PANGO_SCALE - 2;
            double x1 = b->x + (double)(r1.x + r1.width) / PANGO_SCALE + 4;
            double y1 = b->y + (double)(r0.y + r0.height) / PANGO_SCALE + 2;
            if (x1 < x0) { double t = x0; x0 = x1; x1 = t; }
            cairo_save(cr);
            if (r->kind == ND_INLINE_BUTTON) {
                cairo_pattern_t *grad = cairo_pattern_create_linear(0, y0, 0, y1);
                cairo_pattern_add_color_stop_rgb(grad, 0.0, 0.95, 0.95, 0.95);
                cairo_pattern_add_color_stop_rgb(grad, 1.0, 0.78, 0.78, 0.78);
                cairo_set_source(cr, grad);
                cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
                cairo_fill(cr);
                cairo_pattern_destroy(grad);
                cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
                cairo_set_line_width(cr, 1.0);
                cairo_move_to(cr, x0 + 0.5, y0 + 0.5);
                cairo_line_to(cr, x1 - 0.5, y0 + 0.5);
                cairo_stroke(cr);
                cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
                cairo_set_line_width(cr, 1.0);
                cairo_rectangle(cr, x0 + 0.5, y0 + 0.5, x1 - x0 - 1, y1 - y0 - 1);
                cairo_stroke(cr);
            } else {
                cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
                cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
                cairo_fill(cr);
                cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
                cairo_set_line_width(cr, 1.0);
                cairo_rectangle(cr, x0 + 0.5, y0 + 0.5, x1 - x0 - 1, y1 - y0 - 1);
                cairo_stroke(cr);
                cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
                cairo_move_to(cr, x0 + 0.5, y1 - 0.5);
                cairo_line_to(cr, x1 - 0.5, y1 - 0.5);
                cairo_stroke(cr);
            }
            cairo_restore(cr);
        }
    }

    cairo_save(cr);
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    cairo_move_to(cr, b->x, b->y);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    g_object_unref(layout);
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
        gsize stride = (gsize)iw * 4;
        guchar *pixels = g_new0(guchar, stride * (gsize)ih);
        gdk_texture_download(img->texture, pixels, stride);
        cairo_surface_t *surf = cairo_image_surface_create_for_data(
            pixels, CAIRO_FORMAT_ARGB32, iw, ih, (int)stride);
        cairo_translate(cr, b->x, b->y);
        cairo_scale(cr, b->content_width / iw, b->content_height / ih);
        cairo_set_source_surface(cr, surf, 0, 0);
        cairo_paint(cr);
        cairo_surface_destroy(surf);
        g_free(pixels);
    } else {
        cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
        cairo_rectangle(cr, b->x, b->y, b->content_width, b->content_height);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static void
paint_marker(cairo_t *cr, const nd_box *b)
{
    if (!b->dom || !b->dom->name || strcmp(b->dom->name, "li") != 0) return;
    const nd_node *parent = b->dom->parent;
    if (!parent || !parent->name) return;
    const nd_style *s = b->style;
    double font_size = length_or(s ? s->values[ND_CSS_FONT_SIZE] : NULL, 16);
    double cy = b->y + b->margin.top + b->padding.top + font_size * 0.7;
    double cx = b->x + b->margin.left + b->padding.left - font_size * 0.8;
    rgba color = rgba_of(s ? s->values[ND_CSS_COLOR] : NULL, 0.1, 0.1, 0.1, 1);
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    if (strcmp(parent->name, "ol") == 0) {
        int n = 1;
        for (const nd_node *p = b->dom->prev_sibling; p; p = p->prev_sibling)
            if (p->kind == ND_NODE_ELEMENT && p->name && strcmp(p->name, "li") == 0) n++;
        char buf[16];
        g_snprintf(buf, sizeof buf, "%d.", n);
        cairo_move_to(cr, cx - font_size * 0.5, cy);
        cairo_set_font_size(cr, font_size);
        cairo_show_text(cr, buf);
    } else {
        cairo_arc(cr, cx, cy - font_size * 0.32, font_size * 0.18, 0, 2 * G_PI);
        cairo_fill(cr);
    }
}

static void
paint_hr(cairo_t *cr, const nd_box *b)
{
    if (!b->dom || !b->dom->name || strcmp(b->dom->name, "hr") != 0) return;
    double y = b->y + b->margin.top + 4;
    double x0 = b->x + b->margin.left;
    double x1 = x0 + b->content_width;
    cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x0, y);
    cairo_line_to(cr, x1, y);
    cairo_stroke(cr);
}

static void
paint_walk(cairo_t *cr, const nd_box *b, const char *highlight)
{
    if (!b) return;
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
    for (const nd_box *c = b->first_child; c; c = c->next_sibling)
        paint_walk(cr, c, highlight);
}

void
nd_paint(cairo_t *cr, const nd_box *root, const char *highlight_query)
{
    cairo_save(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_restore(cr);
    paint_walk(cr, root, highlight_query);
}
