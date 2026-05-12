/* Nordstjernen — Cairo paint. */

#include "paint.h"

#include <gdk/gdk.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>

#include "css.h"
#include "image.h"

typedef struct rgba {
    double r, g, b, a;
} rgba;

static gboolean g_caret_visible = TRUE;

void
nd_paint_set_caret_visible(gboolean visible)
{
    g_caret_visible = visible;
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

static double
box_border_radius(const nd_box *b)
{
    const nd_style *s = b ? b->style : NULL;
    if (!s) return 0;
    const nd_css_value *v = s->values[ND_CSS_BORDER_RADIUS];
    if (!v || v->kind != ND_CSS_V_LENGTH) return 0;
    double r = v->u.length.v;
    if (r < 0) r = 0;
    return r;
}

static void
rounded_rect_path(cairo_t *cr, double x, double y, double w, double h, double r)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { cairo_rectangle(cr, x, y, w, h); return; }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -G_PI_2,  0);
    cairo_arc(cr, x + w - r, y + h - r, r,  0,       G_PI_2);
    cairo_arc(cr, x + r,     y + h - r, r,  G_PI_2,  G_PI);
    cairo_arc(cr, x + r,     y + r,     r,  G_PI,    1.5 * G_PI);
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
    double radius = box_border_radius(b);
    rgba bg = rgba_of(s ? s->values[ND_CSS_BACKGROUND_COLOR] : NULL,
                      0, 0, 0, 0);
    if (bg.a > 0) {
        cairo_set_source_rgba(cr, bg.r, bg.g, bg.b, bg.a);
        rounded_rect_path(cr, border_x, border_y, border_w, border_h, radius);
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
    const nd_css_value *fw = s ? s->values[ND_CSS_FONT_WEIGHT] : NULL;
    if (fw && fw->kind == ND_CSS_V_KEYWORD && fw->u.keyword) {
        const char *k = fw->u.keyword;
        int weight = 0;
        if (strcmp(k, "bold") == 0 || strcmp(k, "bolder") == 0) weight = PANGO_WEIGHT_BOLD;
        else if (g_ascii_isdigit(k[0])) {
            int n = atoi(k);
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
                a = pango_attr_background_new(0xffff, 0xffff, 0xffff); break;
            case ND_INLINE_BUTTON:
                a = pango_attr_background_new(0xe6e6, 0xe6e6, 0xe6e6); break;
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
                gboolean focused = r->kind == ND_INLINE_INPUT_FIELD_FOCUSED;
                cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
                cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
                cairo_fill(cr);
                if (focused) cairo_set_source_rgb(cr, 0.13, 0.36, 0.80);
                else         cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
                cairo_set_line_width(cr, focused ? 2.0 : 1.0);
                cairo_rectangle(cr, x0 + 0.5, y0 + 0.5, x1 - x0 - 1, y1 - y0 - 1);
                cairo_stroke(cr);
                if (!focused) {
                    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
                    cairo_move_to(cr, x0 + 0.5, y1 - 0.5);
                    cairo_line_to(cr, x1 - 0.5, y1 - 0.5);
                    cairo_stroke(cr);
                }
            }
            cairo_restore(cr);
        }
    }

    cairo_save(cr);
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    cairo_move_to(cr, b->x, b->y);
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
            double cy = b->y + (double)pos.y / PANGO_SCALE;
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
        if (start_attr) start = atoi(start_attr);
        int n = start;
        for (const nd_node *p = b->dom->prev_sibling; p; p = p->prev_sibling)
            if (p->kind == ND_NODE_ELEMENT && p->name && strcmp(p->name, "li") == 0) n++;
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
        cairo_rectangle(cr, cx - sz/2, cy - font_size * 0.32 - sz/2, sz, sz);
        cairo_fill(cr);
    } else if (style_kw && strcmp(style_kw, "circle") == 0) {
        cairo_arc(cr, cx, cy - font_size * 0.32, font_size * 0.18, 0, 2 * G_PI);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
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
    for (const nd_box *c = b->first_child; c; c = c->next_sibling)
        paint_walk(cr, c, highlight);
    if (grouped) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, op);
    }
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
