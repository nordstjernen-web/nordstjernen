/* Nordstjernen — block layout. */

#include "layout.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>

static double
length_or(const nd_css_value *v, double fallback)
{
    if (!v) return fallback;
    if (v->kind == ND_CSS_V_LENGTH &&
        (v->u.length.unit == ND_CSS_UNIT_PX ||
         v->u.length.unit == ND_CSS_UNIT_NUMBER))
        return v->u.length.v;
    if (v->kind == ND_CSS_V_CALC)
        return v->u.calc.px;
    return fallback;
}

static double
length_resolve(const nd_css_value *v, double basis, double fallback)
{
    if (!v) return fallback;
    if (v->kind == ND_CSS_V_CALC)
        return v->u.calc.pct / 100.0 * basis + v->u.calc.px;
    if (v->kind != ND_CSS_V_LENGTH) return fallback;
    if (v->u.length.unit == ND_CSS_UNIT_PX ||
        v->u.length.unit == ND_CSS_UNIT_NUMBER) return v->u.length.v;
    if (v->u.length.unit == ND_CSS_UNIT_PERCENT)
        return v->u.length.v * basis / 100.0;
    return fallback;
}

static gboolean
length_is_auto(const nd_css_value *v)
{
    return v && v->kind == ND_CSS_V_KEYWORD && v->u.keyword &&
           strcmp(v->u.keyword, "auto") == 0;
}

static gboolean
is_keyword(const nd_css_value *v, const char *kw)
{
    return v && v->kind == ND_CSS_V_KEYWORD && kw && strcmp(v->u.keyword, kw) == 0;
}

static gboolean
style_is_block(const nd_style *s)
{

    if (!s || !s->values[ND_CSS_DISPLAY]) return FALSE;
    const nd_css_value *v = s->values[ND_CSS_DISPLAY];
    if (v->kind != ND_CSS_V_KEYWORD || !v->u.keyword) return FALSE;
    const char *kw = v->u.keyword;
    return strcmp(kw, "block") == 0 ||
           strcmp(kw, "flex") == 0 ||
           strcmp(kw, "grid") == 0 ||
           strcmp(kw, "list-item") == 0 ||
           strcmp(kw, "flow-root") == 0;
}

static gboolean
style_is_absolute_or_fixed(const nd_style *s)
{
    if (!s || !s->values[ND_CSS_POSITION]) return FALSE;
    const nd_css_value *v = s->values[ND_CSS_POSITION];
    if (v->kind != ND_CSS_V_KEYWORD || !v->u.keyword) return FALSE;
    const char *kw = v->u.keyword;
    return strcmp(kw, "absolute") == 0 || strcmp(kw, "fixed") == 0;
}

static gboolean
style_is_none(const nd_style *s)
{
    return s && s->values[ND_CSS_DISPLAY] && is_keyword(s->values[ND_CSS_DISPLAY], "none");
}

static void
edges_from_style(const nd_style *s, double basis,
                 nd_edges *margin, nd_edges *padding, nd_edges *border)
{
    if (!s) {
        memset(margin, 0, sizeof(*margin));
        memset(padding, 0, sizeof(*padding));
        memset(border, 0, sizeof(*border));
        return;
    }
    margin->top    = length_resolve(s->values[ND_CSS_MARGIN_TOP],    basis, 0);
    margin->right  = length_resolve(s->values[ND_CSS_MARGIN_RIGHT],  basis, 0);
    margin->bottom = length_resolve(s->values[ND_CSS_MARGIN_BOTTOM], basis, 0);
    margin->left   = length_resolve(s->values[ND_CSS_MARGIN_LEFT],   basis, 0);
    padding->top    = length_resolve(s->values[ND_CSS_PADDING_TOP],    basis, 0);
    padding->right  = length_resolve(s->values[ND_CSS_PADDING_RIGHT],  basis, 0);
    padding->bottom = length_resolve(s->values[ND_CSS_PADDING_BOTTOM], basis, 0);
    padding->left   = length_resolve(s->values[ND_CSS_PADDING_LEFT],   basis, 0);
    border->top    = length_or(s->values[ND_CSS_BORDER_TOP_WIDTH],    0);
    border->right  = length_or(s->values[ND_CSS_BORDER_RIGHT_WIDTH],  0);
    border->bottom = length_or(s->values[ND_CSS_BORDER_BOTTOM_WIDTH], 0);
    border->left   = length_or(s->values[ND_CSS_BORDER_LEFT_WIDTH],   0);
}

static nd_box *
box_new(nd_box_kind kind)
{
    nd_box *b = g_new0(nd_box, 1);
    b->kind = kind;
    return b;
}

static void
line_clear(gpointer data)
{
    nd_line *ln = data;
    g_free(ln->text);
}

static void
link_clear(gpointer data)
{
    nd_link_range *r = data;
    g_free(r->href);
    g_free(r->target);
}

static nd_box *
box_new_inline(void)
{
    nd_box *b = box_new(ND_BOX_INLINE);
    b->lines = g_array_new(FALSE, FALSE, sizeof(nd_line));
    g_array_set_clear_func(b->lines, line_clear);
    b->links = g_array_new(FALSE, FALSE, sizeof(nd_link_range));
    g_array_set_clear_func(b->links, link_clear);
    b->attrs = g_array_new(FALSE, FALSE, sizeof(nd_inline_attr));
    return b;
}

static void
box_append_child(nd_box *parent, nd_box *child)
{
    child->parent = parent;
    if (!parent->first_child) parent->first_child = child;
    else                       parent->last_child->next_sibling = child;
    parent->last_child = child;
}

void
nd_box_free(nd_box *box)
{
    if (!box) return;
    nd_box *c = box->first_child;
    while (c) {
        nd_box *next = c->next_sibling;
        nd_box_free(c);
        c = next;
    }
    if (box->lines) g_array_free(box->lines, TRUE);
    if (box->links) g_array_free(box->links, TRUE);
    if (box->attrs) g_array_free(box->attrs, TRUE);
    g_free(box->text);
    g_free(box->image_src);
    g_free(box);
}

static gboolean
is_inline_dom(const nd_node *n, GHashTable *styles)
{
    if (!n) return FALSE;
    if (n->kind == ND_NODE_TEXT) return TRUE;
    if (n->kind != ND_NODE_ELEMENT) return FALSE;
    if (n->name && (strcmp(n->name, "img") == 0 ||
                    strcmp(n->name, "table") == 0)) return FALSE;
    const nd_style *s = g_hash_table_lookup(styles, n);
    if (!s) return FALSE;
    if (style_is_none(s)) return FALSE;
    if (style_is_absolute_or_fixed(s)) return FALSE;
    return !style_is_block(s);
}

static void
collect_rows_recurse(const nd_node *n, GPtrArray *out)
{
    if (!n) return;
    if (n->kind == ND_NODE_ELEMENT && n->name) {
        if (strcmp(n->name, "tr") == 0) {
            g_ptr_array_add(out, (gpointer)n);
            return;
        }
        if (strcmp(n->name, "table") == 0) return;
    }
    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        collect_rows_recurse(c, out);
}

static void
collect_rows(const nd_node *table, GPtrArray *out)
{
    if (!table) return;
    for (const nd_node *c = table->first_child; c; c = c->next_sibling)
        collect_rows_recurse(c, out);
}

static gboolean
is_cell_element(const nd_node *n)
{
    return n && n->kind == ND_NODE_ELEMENT && n->name &&
           (strcmp(n->name, "td") == 0 || strcmp(n->name, "th") == 0);
}

static nd_box *build_block_for(const nd_node *n, GHashTable *styles);
static nd_box *build_inline_run(const nd_node *first, const nd_node *last_excl, GHashTable *styles);
static const nd_node *g_focused_input_for_layout;
static nd_box *nd_layout_build_(const nd_node *doc, GHashTable *styles, double viewport_width);

static nd_box *
build_cell(const nd_node *n, GHashTable *styles)
{
    nd_box *cell = box_new(ND_BOX_TABLE_CELL);
    cell->dom = n;
    cell->style = g_hash_table_lookup(styles, n);
    const nd_node *c = n->first_child;
    while (c) {
        if (is_inline_dom(c, styles)) {
            const nd_node *start = c;
            while (c && is_inline_dom(c, styles)) c = c->next_sibling;
            nd_box *run = build_inline_run(start, c, styles);
            if (run->text && run->text[0] != '\0')
                box_append_child(cell, run);
            else
                nd_box_free(run);
        } else {
            nd_box *child = build_block_for(c, styles);
            if (child) box_append_child(cell, child);
            if (c) c = c->next_sibling;
        }
    }
    return cell;
}

static nd_box *
build_table(const nd_node *n, GHashTable *styles)
{
    nd_box *table = box_new(ND_BOX_TABLE);
    table->dom = n;
    table->style = g_hash_table_lookup(styles, n);
    GPtrArray *rows = g_ptr_array_new();
    collect_rows(n, rows);
    for (guint i = 0; i < rows->len; i++) {
        const nd_node *tr = g_ptr_array_index(rows, i);
        nd_box *row = box_new(ND_BOX_TABLE_ROW);
        row->dom = tr;
        row->style = g_hash_table_lookup(styles, tr);
        for (const nd_node *c = tr->first_child; c; c = c->next_sibling) {
            if (!is_cell_element(c)) continue;
            nd_box *cell = build_cell(c, styles);
            box_append_child(row, cell);
        }
        box_append_child(table, row);
    }
    g_ptr_array_free(rows, TRUE);
    return table;
}

typedef struct collector_ctx {
    GHashTable *styles;
    const char *active_href;
    const char *active_target;
    const nd_node *active_link_node;
    GString    *out;
    GArray     *links;
    GArray     *attrs;
    int  bold_depth;
    int  italic_depth;
    int  mono_depth;
    int  underline_depth;
    int  strike_depth;
    gsize bold_start;
    gsize italic_start;
    gsize mono_start;
    gsize underline_start;
    gsize strike_start;
} collector_ctx;

static gboolean
tag_is_bold(const char *name)
{
    return strcmp(name, "b") == 0 || strcmp(name, "strong") == 0;
}

static gboolean
tag_is_italic(const char *name)
{
    return strcmp(name, "i") == 0 || strcmp(name, "em") == 0 ||
           strcmp(name, "cite") == 0 || strcmp(name, "dfn") == 0;
}

static gboolean
tag_is_monospace(const char *name)
{
    return strcmp(name, "code") == 0 || strcmp(name, "tt") == 0 ||
           strcmp(name, "kbd") == 0 || strcmp(name, "samp") == 0 ||
           strcmp(name, "pre") == 0;
}

static void
emit_attr(GArray *attrs, nd_inline_attr_kind k, gsize start, gsize end)
{
    if (end <= start) return;
    nd_inline_attr a = { .kind = k, .start = start, .len = end - start };
    g_array_append_val(attrs, a);
}

static void
emit_font_size_attr(GArray *attrs, gsize start, gsize end, double font_size_px)
{
    if (end <= start) return;
    nd_inline_attr a = { .kind = ND_INLINE_FONT_SIZE, .start = start,
                         .len = end - start, .font_size_px = font_size_px };
    g_array_append_val(attrs, a);
}

static void
emit_color_attr(GArray *attrs, gsize start, gsize end,
                guint8 r, guint8 g, guint8 b, guint8 a8)
{
    if (end <= start) return;
    nd_inline_attr a = { .kind = ND_INLINE_COLOR, .start = start,
                         .len = end - start, .r = r, .g = g, .b = b, .a = a8 };
    g_array_append_val(attrs, a);
}

static void
emit_bg_color_attr(GArray *attrs, gsize start, gsize end,
                   guint8 r, guint8 g, guint8 b, guint8 a8)
{
    if (end <= start) return;
    nd_inline_attr a = { .kind = ND_INLINE_BG_COLOR, .start = start,
                         .len = end - start, .r = r, .g = g, .b = b, .a = a8 };
    g_array_append_val(attrs, a);
}

static void
emit_font_family_attr(GArray *attrs, gsize start, gsize end, const char *family)
{
    if (end <= start || !family) return;
    nd_inline_attr a = { .kind = ND_INLINE_FONT_FAMILY, .start = start,
                         .len = end - start, .family = family };
    g_array_append_val(attrs, a);
}

static void
collect_walk(const nd_node *n, collector_ctx *ctx)
{
    if (!n) return;
    if (n->kind == ND_NODE_TEXT) {
        if (!n->text) return;
        gsize start = ctx->out->len;
        g_string_append(ctx->out, n->text);
        if (ctx->active_href) {
            nd_link_range r = {
                .start = start,
                .len   = ctx->out->len - start,
                .href  = g_strdup(ctx->active_href),
                .target = ctx->active_target ? g_strdup(ctx->active_target) : NULL,
                .dom   = ctx->active_link_node,
            };
            g_array_append_val(ctx->links, r);
        }
        return;
    }
    if (n->kind != ND_NODE_ELEMENT) return;
    const nd_style *s = g_hash_table_lookup(ctx->styles, n);
    if (s && style_is_none(s)) return;

    if (strcmp(n->name, "br") == 0) {
        g_string_append(ctx->out, "\xe2\x80\xa8");
        return;
    }
    if (strcmp(n->name, "wbr") == 0) {
        g_string_append(ctx->out, "\xe2\x80\x8b");
        return;
    }
    if (strcmp(n->name, "input") == 0) {
        const char *type = nd_element_get_attr(n, "type");
        gboolean is_text = !type || !*type ||
                           g_ascii_strcasecmp(type, "text") == 0 ||
                           g_ascii_strcasecmp(type, "search") == 0 ||
                           g_ascii_strcasecmp(type, "email") == 0 ||
                           g_ascii_strcasecmp(type, "url") == 0 ||
                           g_ascii_strcasecmp(type, "tel") == 0 ||
                           g_ascii_strcasecmp(type, "number") == 0;
        if (is_text) {
            gsize start = ctx->out->len;
            g_string_append(ctx->out, "\xc2\xa0");
            const char *v = nd_element_get_attr(n, "value");
            if (!v || !*v) v = nd_element_get_attr(n, "placeholder");
            if (v && *v) {
                g_string_append(ctx->out, v);
            } else {
                const char *size_str = nd_element_get_attr(n, "size");
                int size = size_str ? atoi(size_str) : 20;
                if (size < 4)  size = 20;
                if (size > 80) size = 80;
                for (int i = 0; i < size; i++)
                    g_string_append(ctx->out, "\xc2\xa0");
            }
            g_string_append(ctx->out, "\xc2\xa0");
            nd_inline_attr_kind kind = (n == g_focused_input_for_layout)
                                       ? ND_INLINE_INPUT_FIELD_FOCUSED
                                       : ND_INLINE_INPUT_FIELD;
            emit_attr(ctx->attrs, kind, start, ctx->out->len);
        } else if (type && (g_ascii_strcasecmp(type, "submit") == 0 ||
                            g_ascii_strcasecmp(type, "button") == 0 ||
                            g_ascii_strcasecmp(type, "reset") == 0)) {
            const char *v = nd_element_get_attr(n, "value");
            if (!v || !*v) v = g_ascii_strcasecmp(type, "submit") == 0 ? "Submit"
                              : g_ascii_strcasecmp(type, "reset")  == 0 ? "Reset"
                                                                        : "Button";
            gsize start = ctx->out->len;
            g_string_append_printf(ctx->out, "\xc2\xa0%s\xc2\xa0", v);
            emit_attr(ctx->attrs, ND_INLINE_BUTTON, start, ctx->out->len);
        } else if (type && g_ascii_strcasecmp(type, "checkbox") == 0) {
            const char *checked = nd_element_get_attr(n, "checked");
            g_string_append(ctx->out, checked ? "\xe2\x98\x91" : "\xe2\x98\x90");
        } else if (type && g_ascii_strcasecmp(type, "radio") == 0) {
            const char *checked = nd_element_get_attr(n, "checked");
            g_string_append(ctx->out, checked ? "\xe2\x97\x89" : "\xe2\x97\x8b");
        } else if (type && g_ascii_strcasecmp(type, "file") == 0) {
            gsize start = ctx->out->len;
            g_string_append(ctx->out, "\xc2\xa0" "Choose File" "\xc2\xa0");
            emit_attr(ctx->attrs, ND_INLINE_BUTTON, start, ctx->out->len);
            g_string_append(ctx->out, " (file upload not supported)");
        } else if (type && g_ascii_strcasecmp(type, "color") == 0) {
            const char *v = nd_element_get_attr(n, "value");
            gsize start = ctx->out->len;
            g_string_append(ctx->out, "\xc2\xa0");
            g_string_append(ctx->out, v && *v ? v : "#000000");
            g_string_append(ctx->out, "\xc2\xa0");
            emit_attr(ctx->attrs, ND_INLINE_INPUT_FIELD, start, ctx->out->len);
        } else if (type && (g_ascii_strcasecmp(type, "range") == 0)) {
            const char *v = nd_element_get_attr(n, "value");
            const char *mn = nd_element_get_attr(n, "min");
            const char *mx = nd_element_get_attr(n, "max");
            gsize start = ctx->out->len;
            g_string_append_printf(ctx->out,
                "\xc2\xa0[%s\xc2\xa0%s/%s]" "\xc2\xa0",
                v && *v ? v : "50",
                mn && *mn ? mn : "0",
                mx && *mx ? mx : "100");
            emit_attr(ctx->attrs, ND_INLINE_INPUT_FIELD, start, ctx->out->len);
        } else if (type && (g_ascii_strcasecmp(type, "date") == 0 ||
                            g_ascii_strcasecmp(type, "datetime-local") == 0 ||
                            g_ascii_strcasecmp(type, "time") == 0 ||
                            g_ascii_strcasecmp(type, "month") == 0 ||
                            g_ascii_strcasecmp(type, "week") == 0)) {
            const char *v = nd_element_get_attr(n, "value");
            gsize start = ctx->out->len;
            g_string_append(ctx->out, "\xc2\xa0");
            if (v && *v) g_string_append(ctx->out, v);
            else         g_string_append(ctx->out, "____-__-__");
            g_string_append(ctx->out, "\xc2\xa0");
            emit_attr(ctx->attrs, ND_INLINE_INPUT_FIELD, start, ctx->out->len);
        }
        return;
    }
    if (strcmp(n->name, "button") == 0) {
        char *label = nd_node_collect_text(n);
        if (!label || !*label) {
            g_free(label);
            label = g_strdup("Button");
        }
        gsize start = ctx->out->len;
        g_string_append(ctx->out, "\xc2\xa0");
        g_string_append(ctx->out, label);
        g_string_append(ctx->out, "\xc2\xa0");
        emit_attr(ctx->attrs, ND_INLINE_BUTTON, start, ctx->out->len);
        g_free(label);
        return;
    }
    if (strcmp(n->name, "select") == 0) {
        const nd_node *chosen = NULL;
        const nd_node *first_opt = NULL;
        for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
            if (c->kind != ND_NODE_ELEMENT || !c->name) continue;
            if (strcmp(c->name, "optgroup") == 0) {
                for (const nd_node *cc = c->first_child; cc; cc = cc->next_sibling) {
                    if (cc->kind == ND_NODE_ELEMENT && cc->name &&
                        strcmp(cc->name, "option") == 0) {
                        if (!first_opt) first_opt = cc;
                        if (nd_element_get_attr(cc, "selected")) { chosen = cc; break; }
                    }
                }
            } else if (strcmp(c->name, "option") == 0) {
                if (!first_opt) first_opt = c;
                if (nd_element_get_attr(c, "selected")) { chosen = c; break; }
            }
        }
        if (!chosen) chosen = first_opt;
        char *label = chosen ? nd_node_collect_text(chosen) : g_strdup("");
        if (!label) label = g_strdup("");
        gsize start = ctx->out->len;
        g_string_append(ctx->out, "\xc2\xa0");
        if (*label) g_string_append(ctx->out, label);
        g_string_append(ctx->out, " \xe2\x96\xbe\xc2\xa0");
        emit_attr(ctx->attrs, ND_INLINE_INPUT_FIELD, start, ctx->out->len);
        g_free(label);
        return;
    }
    if (strcmp(n->name, "option") == 0 || strcmp(n->name, "optgroup") == 0)
        return;
    if (strcmp(n->name, "textarea") == 0) {
        gsize start = ctx->out->len;
        g_string_append(ctx->out, "\xc2\xa0");
        gboolean any = FALSE;
        for (const nd_node *c = n->first_child; c; c = c->next_sibling)
            if (c->kind == ND_NODE_TEXT && c->text && *c->text) {
                g_string_append(ctx->out, c->text);
                any = TRUE;
            }
        if (!any) {
            for (int i = 0; i < 40; i++) g_string_append(ctx->out, "\xc2\xa0");
        }
        g_string_append(ctx->out, "\xc2\xa0");
        nd_inline_attr_kind ta_kind = (n == g_focused_input_for_layout)
                                       ? ND_INLINE_INPUT_FIELD_FOCUSED
                                       : ND_INLINE_INPUT_FIELD;
        emit_attr(ctx->attrs, ta_kind, start, ctx->out->len);
        return;
    }

    const char *prev_href   = ctx->active_href;
    const char *prev_target = ctx->active_target;
    const nd_node *prev_link_node = ctx->active_link_node;
    if (strcmp(n->name, "a") == 0) {
        const char *h = nd_element_get_attr(n, "href");
        if (h && *h) {
            ctx->active_href   = h;
            ctx->active_target = nd_element_get_attr(n, "target");
            ctx->active_link_node = n;
        }
    }
    gboolean bold   = tag_is_bold(n->name);
    gboolean italic = tag_is_italic(n->name);
    gboolean mono   = tag_is_monospace(n->name);
    gboolean uline  = strcmp(n->name, "u") == 0 ||
                      strcmp(n->name, "ins") == 0;
    gboolean strike = strcmp(n->name, "s") == 0 ||
                      strcmp(n->name, "del") == 0 ||
                      strcmp(n->name, "strike") == 0;
    if (s && s->values[ND_CSS_FONT_WEIGHT] &&
        s->values[ND_CSS_FONT_WEIGHT]->kind == ND_CSS_V_KEYWORD) {
        const char *kw = s->values[ND_CSS_FONT_WEIGHT]->u.keyword;
        if (strcmp(kw, "bold") == 0 || strcmp(kw, "bolder") == 0) bold = TRUE;
        else if (g_ascii_isdigit(kw[0])) {
            int n_w = atoi(kw);
            if (n_w >= 600) bold = TRUE;
        }
    }
    if (s && s->values[ND_CSS_FONT_STYLE] &&
        s->values[ND_CSS_FONT_STYLE]->kind == ND_CSS_V_KEYWORD &&
        strcmp(s->values[ND_CSS_FONT_STYLE]->u.keyword, "italic") == 0)
        italic = TRUE;
    if (s && s->values[ND_CSS_TEXT_DECORATION] &&
        s->values[ND_CSS_TEXT_DECORATION]->kind == ND_CSS_V_KEYWORD) {
        const char *kw = s->values[ND_CSS_TEXT_DECORATION]->u.keyword;
        if (strstr(kw, "underline")) uline = TRUE;
        if (strstr(kw, "line-through")) strike = TRUE;
        if (strstr(kw, "none")) { uline = FALSE; strike = FALSE; }
    }
    if (bold && ctx->bold_depth++ == 0) ctx->bold_start = ctx->out->len;
    if (italic && ctx->italic_depth++ == 0) ctx->italic_start = ctx->out->len;
    if (mono && ctx->mono_depth++ == 0) ctx->mono_start = ctx->out->len;
    if (uline && ctx->underline_depth++ == 0) ctx->underline_start = ctx->out->len;
    if (strike && ctx->strike_depth++ == 0) ctx->strike_start = ctx->out->len;

    double font_size_self = 0;
    if (s && s->values[ND_CSS_FONT_SIZE]) {
        const nd_css_value *fv = s->values[ND_CSS_FONT_SIZE];
        if (fv->kind == ND_CSS_V_LENGTH && fv->u.length.unit == ND_CSS_UNIT_PX)
            font_size_self = fv->u.length.v;
    }
    gsize fs_start = ctx->out->len;
    gboolean fs_active = font_size_self > 0;

    gsize color_start = ctx->out->len;
    gboolean color_active = FALSE;
    guint8 cr = 0, cg = 0, cb = 0, ca = 0;
    if (s && s->values[ND_CSS_COLOR] &&
        s->values[ND_CSS_COLOR]->kind == ND_CSS_V_COLOR &&
        strcmp(n->name, "a") != 0) {
        cr = s->values[ND_CSS_COLOR]->u.color.r;
        cg = s->values[ND_CSS_COLOR]->u.color.g;
        cb = s->values[ND_CSS_COLOR]->u.color.b;
        ca = s->values[ND_CSS_COLOR]->u.color.a;
        color_active = TRUE;
    }

    gsize bg_start = ctx->out->len;
    gboolean bg_active = FALSE;
    guint8 bgr = 0, bgg = 0, bgb = 0, bga = 0;
    if (s && s->values[ND_CSS_BACKGROUND_COLOR] &&
        s->values[ND_CSS_BACKGROUND_COLOR]->kind == ND_CSS_V_COLOR) {
        bgr = s->values[ND_CSS_BACKGROUND_COLOR]->u.color.r;
        bgg = s->values[ND_CSS_BACKGROUND_COLOR]->u.color.g;
        bgb = s->values[ND_CSS_BACKGROUND_COLOR]->u.color.b;
        bga = s->values[ND_CSS_BACKGROUND_COLOR]->u.color.a;
        if (bga > 0) bg_active = TRUE;
    }

    gsize family_start = ctx->out->len;
    const char *family_str = NULL;
    if (s && s->values[ND_CSS_FONT_FAMILY] &&
        s->values[ND_CSS_FONT_FAMILY]->kind == ND_CSS_V_KEYWORD)
        family_str = s->values[ND_CSS_FONT_FAMILY]->u.keyword;

    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        collect_walk(c, ctx);

    if (fs_active && ctx->out->len > fs_start)
        emit_font_size_attr(ctx->attrs, fs_start, ctx->out->len, font_size_self);
    if (color_active && ctx->out->len > color_start)
        emit_color_attr(ctx->attrs, color_start, ctx->out->len, cr, cg, cb, ca);
    if (bg_active && ctx->out->len > bg_start)
        emit_bg_color_attr(ctx->attrs, bg_start, ctx->out->len, bgr, bgg, bgb, bga);
    if (family_str && ctx->out->len > family_start)
        emit_font_family_attr(ctx->attrs, family_start, ctx->out->len, family_str);

    if (bold && --ctx->bold_depth == 0)
        emit_attr(ctx->attrs, ND_INLINE_BOLD, ctx->bold_start, ctx->out->len);
    if (italic && --ctx->italic_depth == 0)
        emit_attr(ctx->attrs, ND_INLINE_ITALIC, ctx->italic_start, ctx->out->len);
    if (mono && --ctx->mono_depth == 0)
        emit_attr(ctx->attrs, ND_INLINE_MONOSPACE, ctx->mono_start, ctx->out->len);
    if (uline && --ctx->underline_depth == 0)
        emit_attr(ctx->attrs, ND_INLINE_UNDERLINE, ctx->underline_start, ctx->out->len);
    if (strike && --ctx->strike_depth == 0)
        emit_attr(ctx->attrs, ND_INLINE_STRIKETHROUGH, ctx->strike_start, ctx->out->len);
    ctx->active_href   = prev_href;
    ctx->active_target = prev_target;
    ctx->active_link_node = prev_link_node;
}


static nd_box *build_block(const nd_node *n, GHashTable *styles);

static gboolean
is_preformatted_parent(const nd_node *parent)
{
    for (const nd_node *p = parent; p; p = p->parent) {
        if (p->kind != ND_NODE_ELEMENT) continue;
        if (p->name && (strcmp(p->name, "pre") == 0 ||
                        strcmp(p->name, "textarea") == 0))
            return TRUE;
    }
    return FALSE;
}

static nd_box *
build_inline_run(const nd_node *first, const nd_node *last_excl, GHashTable *styles)
{
    GString *buf = g_string_new(NULL);
    GArray  *raw_links = g_array_new(FALSE, FALSE, sizeof(nd_link_range));
    GArray  *raw_attrs = g_array_new(FALSE, FALSE, sizeof(nd_inline_attr));
    g_array_set_clear_func(raw_links, link_clear);
    collector_ctx ctx = {
        .styles = styles, .out = buf, .links = raw_links, .attrs = raw_attrs,
    };
    for (const nd_node *n = first; n && n != last_excl; n = n->next_sibling)
        collect_walk(n, &ctx);

    gboolean preformatted = first && first->parent &&
                            is_preformatted_parent(first->parent);

    GString *collapsed = g_string_new(NULL);
    gsize   *map = g_new(gsize, buf->len + 1);
    gboolean prev_ws = !preformatted;
    for (gsize i = 0; i < buf->len; i++) {
        char c = buf->str[i];
        if (preformatted) {
            map[i] = collapsed->len;
            g_string_append_c(collapsed, c);
            continue;
        }
        gboolean ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f');
        if (ws) {
            if (!prev_ws) {
                map[i] = collapsed->len;
                g_string_append_c(collapsed, ' ');
            } else {
                map[i] = collapsed->len;
            }
            prev_ws = TRUE;
        } else {
            map[i] = collapsed->len;
            g_string_append_c(collapsed, c);
            prev_ws = FALSE;
        }
    }
    map[buf->len] = collapsed->len;

    nd_box *box = box_new_inline();
    if (!preformatted && collapsed->len > 0 &&
        collapsed->str[collapsed->len - 1] == ' ')
        g_string_set_size(collapsed, collapsed->len - 1);

    for (guint i = 0; i < raw_links->len; i++) {
        nd_link_range *r = &g_array_index(raw_links, nd_link_range, i);
        if (r->start > buf->len) r->start = buf->len;
        gsize end = r->start + r->len;
        if (end > buf->len) end = buf->len;
        gsize ns = map[r->start];
        gsize ne = map[end];
        if (ne > collapsed->len) ne = collapsed->len;
        if (ne <= ns) continue;
        nd_link_range out = {
            .start = ns,
            .len = ne - ns,
            .href = g_strdup(r->href),
            .target = r->target ? g_strdup(r->target) : NULL,
            .dom = r->dom,
        };
        g_array_append_val(box->links, out);
    }

    for (guint i = 0; i < raw_attrs->len; i++) {
        nd_inline_attr *a = &g_array_index(raw_attrs, nd_inline_attr, i);
        gsize end = a->start + a->len;
        if (a->start > buf->len) a->start = buf->len;
        if (end > buf->len) end = buf->len;
        gsize ns = map[a->start];
        gsize ne = map[end];
        if (ne > collapsed->len) ne = collapsed->len;
        if (ne <= ns) continue;
        nd_inline_attr out = *a;
        out.start = ns;
        out.len = ne - ns;
        g_array_append_val(box->attrs, out);
    }

    g_free(map);
    g_array_free(raw_links, TRUE);
    g_array_free(raw_attrs, TRUE);
    g_string_free(buf, TRUE);

    box->text = g_string_free(collapsed, FALSE);
    return box;
}

static nd_box *
build_image_box(const nd_node *n)
{
    const char *src = nd_element_get_attr(n, "src");
    if (!src || !*src) return NULL;
    nd_box *box = box_new(ND_BOX_IMAGE);
    box->dom = n;
    box->image_src = g_strdup(src);
    const char *ws = nd_element_get_attr(n, "width");
    const char *hs = nd_element_get_attr(n, "height");
    box->content_width  = ws ? g_ascii_strtod(ws, NULL) : 0;
    box->content_height = hs ? g_ascii_strtod(hs, NULL) : 0;
    return box;
}

static nd_box *
build_block_for(const nd_node *n, GHashTable *styles)
{
    extern nd_box *build_block(const nd_node *, GHashTable *);
    return build_block(n, styles);
}

static nd_box *
build_block(const nd_node *n, GHashTable *styles)
{
    if (!n) return NULL;
    if (n->kind == ND_NODE_DOCUMENT) {
        nd_box *root = box_new(ND_BOX_BLOCK);
        for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
            nd_box *child = build_block(c, styles);
            if (child) box_append_child(root, child);
        }
        return root;
    }
    if (n->kind != ND_NODE_ELEMENT) return NULL;

    const nd_style *s = g_hash_table_lookup(styles, n);
    if (s && style_is_none(s)) return NULL;
    if (s && style_is_absolute_or_fixed(s)) return NULL;

    if (n->name && strcmp(n->name, "img") == 0)
        return build_image_box(n);

    if (n->name && strcmp(n->name, "table") == 0)
        return build_table(n, styles);

    if (!style_is_block(s)) return NULL;

    nd_box *block = box_new(ND_BOX_BLOCK);
    block->dom = n;
    block->style = s;

    gboolean details_collapsed = FALSE;
    if (n->name && strcmp(n->name, "details") == 0 &&
        !nd_element_get_attr(n, "open"))
        details_collapsed = TRUE;

    const nd_node *c = n->first_child;
    while (c) {
        if (details_collapsed) {
            if (c->kind != ND_NODE_ELEMENT || !c->name ||
                strcmp(c->name, "summary") != 0) {
                c = c->next_sibling;
                continue;
            }
        }
        if (is_inline_dom(c, styles)) {
            const nd_node *start = c;
            while (c && is_inline_dom(c, styles)) {
                if (details_collapsed &&
                    (c->kind != ND_NODE_ELEMENT || !c->name ||
                     strcmp(c->name, "summary") != 0)) break;
                c = c->next_sibling;
            }
            nd_box *run = build_inline_run(start, c, styles);

            if (run->text && run->text[0] != '\0')
                box_append_child(block, run);
            else
                nd_box_free(run);
        } else {
            nd_box *child = build_block(c, styles);
            if (child) box_append_child(block, child);
            if (c) c = c->next_sibling;
        }
    }
    return block;
}

static gboolean
keyword_is(const nd_css_value *v, const char *kw)
{
    return v && v->kind == ND_CSS_V_KEYWORD && kw && strcmp(v->u.keyword, kw) == 0;
}

static PangoLayout *
make_pango_layout(const nd_style *parent_style)
{
    PangoFontMap *fm = pango_cairo_font_map_get_default();
    PangoContext *ctx = pango_font_map_create_context(fm);
    PangoLayout *layout = pango_layout_new(ctx);
    g_object_unref(ctx);

    PangoFontDescription *desc = pango_font_description_new();
    double font_size = length_or(parent_style ? parent_style->values[ND_CSS_FONT_SIZE] : NULL, 16);
    const char *family = "sans-serif";
    const nd_css_value *fam = parent_style ? parent_style->values[ND_CSS_FONT_FAMILY] : NULL;
    if (fam && fam->kind == ND_CSS_V_KEYWORD) family = fam->u.keyword;
    pango_font_description_set_family(desc, family);
    pango_font_description_set_absolute_size(desc, font_size * PANGO_SCALE);
    const nd_css_value *fw = parent_style ? parent_style->values[ND_CSS_FONT_WEIGHT] : NULL;
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
    if (keyword_is(parent_style ? parent_style->values[ND_CSS_FONT_STYLE] : NULL, "italic"))
        pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    return layout;
}

static double
inline_line_height(const nd_style *parent_style)
{
    double font_size = length_or(parent_style ? parent_style->values[ND_CSS_FONT_SIZE] : NULL, 16);
    const nd_css_value *lh = parent_style ? parent_style->values[ND_CSS_LINE_HEIGHT] : NULL;
    if (lh && lh->kind == ND_CSS_V_LENGTH) {
        if (lh->u.length.unit == ND_CSS_UNIT_PX)
            return lh->u.length.v;
        if (lh->u.length.unit == ND_CSS_UNIT_NUMBER)
            return lh->u.length.v * font_size;
        if (lh->u.length.unit == ND_CSS_UNIT_EM)
            return lh->u.length.v * font_size;
        if (lh->u.length.unit == ND_CSS_UNIT_PERCENT)
            return lh->u.length.v / 100.0 * font_size;
    }
    return font_size * 1.4;
}

static void
inline_layout(nd_box *box, double content_width, const nd_style *parent_style)
{
    g_assert(box->kind == ND_BOX_INLINE);
    g_array_set_size(box->lines, 0);
    if (!box->text || !*box->text) {
        box->content_width  = 0;
        box->content_height = 0;
        return;
    }

    PangoLayout *layout = make_pango_layout(parent_style);
    pango_layout_set_width(layout, (int)(content_width * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_text(layout, box->text, -1);

    int pw, ph;
    pango_layout_get_pixel_size(layout, &pw, &ph);
    double lh_default = inline_line_height(parent_style);
    int line_count = pango_layout_get_line_count(layout);
    if (line_count < 1) line_count = 1;
    double measured = ph;
    double expected = line_count * lh_default;
    box->content_width  = content_width;
    box->content_height = measured > expected ? measured : expected;
    g_object_unref(layout);
}

static void
layout_block(nd_box *box, double parent_content_width, const nd_style *inherited_style);
static void
layout_box(nd_box *box, double parent_content_width, const nd_style *inherited_style);

static void
layout_image(nd_box *box, double parent_content_width)
{
    double w = box->content_width;
    double h = box->content_height;
    if (w <= 0 && h <= 0) {
        w = 200;
        h = 150;
    } else if (w <= 0) {
        w = h;
    } else if (h <= 0) {
        h = w;
    }
    if (w > parent_content_width) {
        double ratio = h / w;
        w = parent_content_width;
        h = w * ratio;
    }
    box->content_width = w;
    box->content_height = h;
}

static void
layout_table(nd_box *box, double parent_content_width, const nd_style *inherited_style)
{
    edges_from_style(box->style, parent_content_width,
                     &box->margin, &box->padding, &box->border);
    double cw = parent_content_width - box->margin.left - box->margin.right -
                box->padding.left - box->padding.right -
                box->border.left - box->border.right;
    if (cw < 0) cw = 0;
    box->content_width = cw;

    guint max_cols = 0;
    for (nd_box *row = box->first_child; row; row = row->next_sibling) {
        guint c = 0;
        for (nd_box *cell = row->first_child; cell; cell = cell->next_sibling) c++;
        if (c > max_cols) max_cols = c;
    }
    if (max_cols == 0) { box->content_height = 0; return; }
    double col_w = cw / (double)max_cols;

    double inner_x = box->x + box->margin.left + box->border.left + box->padding.left;
    double inner_y = box->y + box->margin.top  + box->border.top  + box->padding.top;
    double cursor_y = inner_y;
    const nd_style *child_inherited = box->style ? box->style : inherited_style;

    for (nd_box *row = box->first_child; row; row = row->next_sibling) {
        row->x = inner_x;
        row->y = cursor_y;
        row->content_width = cw;
        double row_height = 0;
        double cell_x = inner_x;
        for (nd_box *cell = row->first_child; cell; cell = cell->next_sibling) {
            cell->x = cell_x;
            cell->y = cursor_y;
            const nd_style *cs = cell->style ? cell->style : child_inherited;
            edges_from_style(cell->style, col_w,
                             &cell->margin, &cell->padding, &cell->border);
            double cell_inner_w = col_w
                - cell->padding.left - cell->padding.right
                - cell->border.left - cell->border.right
                - cell->margin.left - cell->margin.right;
            if (cell_inner_w < 0) cell_inner_w = 0;
            cell->content_width = cell_inner_w;
            double ix = cell->x + cell->margin.left + cell->border.left + cell->padding.left;
            double iy = cell->y + cell->margin.top  + cell->border.top  + cell->padding.top;
            double sub_y = iy;
            for (nd_box *child = cell->first_child; child; child = child->next_sibling) {
                child->x = ix;
                child->y = sub_y;
                layout_box(child, cell_inner_w, cs);
                double dh = child->content_height;
                if (child->kind == ND_BOX_BLOCK)
                    dh += child->margin.top + child->margin.bottom +
                          child->padding.top + child->padding.bottom +
                          child->border.top + child->border.bottom;
                sub_y += dh;
            }
            double cell_h = sub_y - iy;
            cell->content_height = cell_h;
            double cell_outer_h = cell_h
                + cell->margin.top + cell->margin.bottom
                + cell->padding.top + cell->padding.bottom
                + cell->border.top + cell->border.bottom;
            if (cell_outer_h > row_height) row_height = cell_outer_h;
            cell_x += col_w;
        }
        for (nd_box *cell = row->first_child; cell; cell = cell->next_sibling) {
            double extra = row_height
                         - cell->content_height
                         - cell->margin.top - cell->margin.bottom
                         - cell->padding.top - cell->padding.bottom
                         - cell->border.top - cell->border.bottom;
            if (extra > 0) cell->content_height += extra;
        }
        row->content_height = row_height;
        cursor_y += row_height;
    }
    box->content_height = cursor_y - inner_y;
}

static void
layout_box(nd_box *box, double parent_content_width, const nd_style *inherited_style)
{
    if (box->kind == ND_BOX_BLOCK) {
        layout_block(box, parent_content_width, inherited_style);
    } else if (box->kind == ND_BOX_INLINE) {
        inline_layout(box, parent_content_width, inherited_style);
    } else if (box->kind == ND_BOX_IMAGE) {
        layout_image(box, parent_content_width);
    } else if (box->kind == ND_BOX_TABLE) {
        layout_table(box, parent_content_width, inherited_style);
    } else {
        box->content_width = parent_content_width;
        box->content_height = 0;
    }
}

static void
layout_block(nd_box *box, double parent_content_width, const nd_style *inherited_style)
{
    edges_from_style(box->style, parent_content_width,
                     &box->margin, &box->padding, &box->border);

    const nd_css_value *wv  = box->style ? box->style->values[ND_CSS_WIDTH]     : NULL;
    const nd_css_value *mxw = box->style ? box->style->values[ND_CSS_MAX_WIDTH] : NULL;
    double horiz_extras = box->padding.left + box->padding.right +
                          box->border.left + box->border.right;
    double horiz_total  = horiz_extras + box->margin.left + box->margin.right;
    double cw;
    gboolean explicit_width = FALSE;
    if (wv && wv->kind == ND_CSS_V_LENGTH) {
        cw = length_resolve(wv, parent_content_width, 0);
        explicit_width = TRUE;
    } else {
        cw = parent_content_width - horiz_total;
        if (cw < 0) cw = 0;
    }
    double max_cw = length_resolve(mxw, parent_content_width, -1);
    if (max_cw >= 0 && cw > max_cw) { cw = max_cw; explicit_width = TRUE; }
    box->content_width = cw;

    if (explicit_width) {
        gboolean ml_auto = length_is_auto(box->style ? box->style->values[ND_CSS_MARGIN_LEFT]  : NULL);
        gboolean mr_auto = length_is_auto(box->style ? box->style->values[ND_CSS_MARGIN_RIGHT] : NULL);
        double available = parent_content_width - cw - horiz_extras;
        if (available < 0) available = 0;
        if (ml_auto && mr_auto) {
            box->margin.left  = available / 2.0;
            box->margin.right = available / 2.0;
        } else if (ml_auto) {
            box->margin.left  = available - box->margin.right;
            if (box->margin.left < 0) box->margin.left = 0;
        } else if (mr_auto) {
            box->margin.right = available - box->margin.left;
            if (box->margin.right < 0) box->margin.right = 0;
        }
    }

    double inner_x = box->x + box->margin.left + box->border.left + box->padding.left;
    double inner_y = box->y + box->margin.top  + box->border.top  + box->padding.top;
    double cursor_y = inner_y;
    double prev_margin_bottom = 0;
    const nd_style *child_inherited = box->style ? box->style : inherited_style;

    for (nd_box *c = box->first_child; c; c = c->next_sibling) {
        c->x = inner_x;
        if (c->kind == ND_BOX_BLOCK || c->kind == ND_BOX_TABLE) {
            double mt = c->margin.top;
            double gap = mt > prev_margin_bottom ? mt : prev_margin_bottom;
            cursor_y += gap;
            c->y = cursor_y - mt;
            layout_box(c, cw, child_inherited);
            cursor_y += c->content_height +
                        c->padding.top + c->padding.bottom +
                        c->border.top + c->border.bottom;
            prev_margin_bottom = c->margin.bottom;
        } else {
            cursor_y += prev_margin_bottom;
            prev_margin_bottom = 0;
            c->y = cursor_y;
            layout_box(c, cw, child_inherited);
            cursor_y += c->content_height;
        }
    }
    cursor_y += prev_margin_bottom;

    const nd_css_value *hv  = box->style ? box->style->values[ND_CSS_HEIGHT]     : NULL;
    const nd_css_value *mxh = box->style ? box->style->values[ND_CSS_MAX_HEIGHT] : NULL;
    double measured = cursor_y - inner_y;
    if (hv && hv->kind == ND_CSS_V_LENGTH) {
        double explicit_h = length_resolve(hv, parent_content_width, measured);
        box->content_height = explicit_h > measured ? explicit_h : measured;
    } else {
        box->content_height = measured;
    }
    double max_h = length_resolve(mxh, parent_content_width, -1);
    if (max_h >= 0 && box->content_height > max_h)
        box->content_height = max_h;
}

nd_box *
nd_layout_build(const nd_node *doc, GHashTable *styles, double viewport_width,
                const nd_node *focused_input)
{
    g_focused_input_for_layout = focused_input;
    nd_box *root = nd_layout_build_(doc, styles, viewport_width);
    g_focused_input_for_layout = NULL;
    return root;
}

static gboolean
style_is_relative(const nd_style *s)
{
    if (!s || !s->values[ND_CSS_POSITION]) return FALSE;
    const nd_css_value *v = s->values[ND_CSS_POSITION];
    return v->kind == ND_CSS_V_KEYWORD &&
           (strcmp(v->u.keyword, "relative") == 0 ||
            strcmp(v->u.keyword, "sticky")   == 0);
}

static double
length_or_zero(const nd_css_value *v, double basis)
{
    if (!v || v->kind != ND_CSS_V_LENGTH) return 0;
    if (v->u.length.unit == ND_CSS_UNIT_PERCENT)
        return v->u.length.v * basis / 100.0;
    if (v->u.length.unit == ND_CSS_UNIT_EM)
        return v->u.length.v * 16.0;
    return v->u.length.v;
}

static void
apply_position_offsets(nd_box *box, double parent_w)
{
    if (!box) return;
    if (style_is_relative(box->style)) {
        double dx = length_or_zero(box->style->values[ND_CSS_LEFT], parent_w);
        if (dx == 0)
            dx = -length_or_zero(box->style->values[ND_CSS_RIGHT], parent_w);
        double dy = length_or_zero(box->style->values[ND_CSS_TOP], parent_w);
        if (dy == 0)
            dy = -length_or_zero(box->style->values[ND_CSS_BOTTOM], parent_w);
        if (dx != 0 || dy != 0) {
            box->x += dx;
            box->y += dy;
        }
    }
    for (nd_box *c = box->first_child; c; c = c->next_sibling)
        apply_position_offsets(c, box->content_width);
}

static nd_box *
nd_layout_build_(const nd_node *doc, GHashTable *styles, double viewport_width)
{
    nd_box *root = build_block(doc, styles);
    if (!root) return NULL;
    root->x = 0;
    root->y = 0;

    layout_block(root, viewport_width, NULL);
    apply_position_offsets(root, viewport_width);
    return root;
}

static const char *
box_kind_str(nd_box_kind k)
{
    switch (k) {
    case ND_BOX_BLOCK:      return "block";
    case ND_BOX_INLINE:     return "inline";
    case ND_BOX_TEXT:       return "text";
    case ND_BOX_IMAGE:      return "image";
    case ND_BOX_TABLE:      return "table";
    case ND_BOX_TABLE_ROW:  return "row";
    case ND_BOX_TABLE_CELL: return "cell";
    }
    return "?";
}

static void
collect_images_walk(const nd_box *b, GPtrArray *out)
{
    if (!b) return;
    if (b->kind == ND_BOX_IMAGE) g_ptr_array_add(out, (gpointer)b);
    for (const nd_box *c = b->first_child; c; c = c->next_sibling)
        collect_images_walk(c, out);
}

void
nd_layout_collect_images(const nd_box *root, GPtrArray *out_boxes)
{
    collect_images_walk(root, out_boxes);
}

static void
dump_box(GString *out, const nd_box *b, int depth)
{
    for (int i = 0; i < depth; i++) g_string_append(out, "  ");
    const char *tag = (b->dom && b->dom->kind == ND_NODE_ELEMENT) ? b->dom->name : "(anon)";
    g_string_append_printf(out,
        "[%s %s] x=%.0f y=%.0f w=%.0f h=%.0f m=%.0f/%.0f/%.0f/%.0f p=%.0f/%.0f/%.0f/%.0f",
        box_kind_str(b->kind), tag,
        b->x, b->y, b->content_width, b->content_height,
        b->margin.top, b->margin.right, b->margin.bottom, b->margin.left,
        b->padding.top, b->padding.right, b->padding.bottom, b->padding.left);
    if (b->kind == ND_BOX_INLINE && b->text) {
        gsize plen = strlen(b->text);
        gsize show = plen > 60 ? 60 : plen;
        g_string_append(out, " text=\"");
        for (gsize i = 0; i < show; i++) {
            char c = b->text[i];
            g_string_append_c(out, (c == '\n' || c == '\t') ? ' ' : c);
        }
        if (plen > show) g_string_append(out, "…");
        g_string_append_c(out, '"');
        if (b->lines && b->lines->len > 0)
            g_string_append_printf(out, " lines=%u", b->lines->len);
    }
    g_string_append_c(out, '\n');
    for (const nd_box *c = b->first_child; c; c = c->next_sibling)
        dump_box(out, c, depth + 1);
}

GString *
nd_box_dump(const nd_box *root)
{
    GString *out = g_string_new(NULL);
    if (root) dump_box(out, root, 0);
    return out;
}

static guint
count_matches_in_text(const char *text, const char *needle)
{
    if (!text || !needle || !*needle) return 0;
    gsize needle_len = strlen(needle);
    gsize text_len = strlen(text);
    guint hits = 0;
    for (gsize i = 0; i + needle_len <= text_len; ) {
        if (g_ascii_strncasecmp(text + i, needle, needle_len) == 0) {
            hits++;
            i += needle_len;
        } else {
            i++;
        }
    }
    return hits;
}

guint
nd_box_count_matches(const nd_box *root, const char *needle)
{
    if (!root || !needle || !*needle) return 0;
    guint sum = 0;
    if (root->kind == ND_BOX_INLINE && root->text)
        sum += count_matches_in_text(root->text, needle);
    for (const nd_box *c = root->first_child; c; c = c->next_sibling)
        sum += nd_box_count_matches(c, needle);
    return sum;
}

const nd_box *
nd_box_first_match_below(const nd_box *root, const char *needle, double y_threshold)
{
    if (!root || !needle || !*needle) return NULL;
    if (root->kind == ND_BOX_INLINE && root->text && root->y > y_threshold) {
        if (count_matches_in_text(root->text, needle) > 0)
            return root;
    }
    for (const nd_box *c = root->first_child; c; c = c->next_sibling) {
        const nd_box *m = nd_box_first_match_below(c, needle, y_threshold);
        if (m) return m;
    }
    return NULL;
}

const nd_box *
nd_box_hit_test(const nd_box *root, double x, double y)
{
    if (!root) return NULL;
    const nd_box *best = NULL;
    for (const nd_box *c = root->first_child; c; c = c->next_sibling) {
        const nd_box *m = nd_box_hit_test(c, x, y);
        if (m) best = m;
    }
    if (best) return best;
    double x0 = root->x;
    double y0 = root->y;
    double x1 = x0 + root->content_width
              + (root->kind == ND_BOX_BLOCK ? root->padding.left + root->padding.right +
                                              root->border.left + root->border.right +
                                              root->margin.left + root->margin.right : 0);
    double y1 = y0 + root->content_height
              + (root->kind == ND_BOX_BLOCK ? root->padding.top + root->padding.bottom +
                                              root->border.top + root->border.bottom +
                                              root->margin.top + root->margin.bottom : 0);
    if (x >= x0 && x <= x1 && y >= y0 && y <= y1 && root->dom)
        return root;
    return NULL;
}

const nd_box *
nd_box_find_by_id(const nd_box *root, const char *id)
{
    if (!root || !id) return NULL;
    if (root->dom && root->dom->kind == ND_NODE_ELEMENT) {
        const char *eid = nd_element_get_attr(root->dom, "id");
        if (eid && strcmp(eid, id) == 0) return root;
    }
    for (const nd_box *c = root->first_child; c; c = c->next_sibling) {
        const nd_box *m = nd_box_find_by_id(c, id);
        if (m) return m;
    }
    return NULL;
}

const nd_link_range *
nd_box_hit_link_range(const nd_box *root, double x, double y)
{
    if (!root) return NULL;
    if (root->kind == ND_BOX_INLINE && root->lines && root->links &&
        root->links->len > 0) {
        double box_x0 = root->x;
        double box_y0 = root->y;
        double box_y1 = box_y0 + root->content_height;
        if (x >= box_x0 && x <= box_x0 + root->content_width &&
            y >= box_y0 && y <= box_y1) {
            return &g_array_index(root->links, nd_link_range, 0);
        }
    }
    for (const nd_box *c = root->first_child; c; c = c->next_sibling) {
        const nd_link_range *r = nd_box_hit_link_range(c, x, y);
        if (r) return r;
    }
    return NULL;
}

const char *
nd_box_hit_link(const nd_box *root, double x, double y)
{
    const nd_link_range *r = nd_box_hit_link_range(root, x, y);
    return r ? r->href : NULL;
}
