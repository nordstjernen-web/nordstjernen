/* Nordstjernen — block layout.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "layout.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "net.h"
#include "paint.h"

#define length_or nd_css_length_or

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

#define is_keyword nd_css_keyword_is
#define keyword_is nd_css_keyword_is

static gboolean
style_is_block(const nd_style *s)
{
    const nd_css_value *v = s ? s->values[ND_CSS_DISPLAY] : NULL;
    return keyword_is(v, "block")     || keyword_is(v, "flex") ||
           keyword_is(v, "grid")      || keyword_is(v, "list-item") ||
           keyword_is(v, "flow-root");
}

static gboolean
style_is_flex_container(const nd_style *s)
{
    const nd_css_value *v = s ? s->values[ND_CSS_DISPLAY] : NULL;
    return keyword_is(v, "flex") || keyword_is(v, "inline-flex");
}

static gboolean
style_is_grid_container(const nd_style *s)
{
    const nd_css_value *v = s ? s->values[ND_CSS_DISPLAY] : NULL;
    return keyword_is(v, "grid") || keyword_is(v, "inline-grid");
}

static const char *
keyword_or(const nd_style *s, nd_css_prop p, const char *fallback)
{
    if (!s || !s->values[p]) return fallback;
    const nd_css_value *v = s->values[p];
    if (v->kind != ND_CSS_V_KEYWORD || !v->u.keyword) return fallback;
    return v->u.keyword;
}

static double
number_or(const nd_css_value *v, double fallback)
{
    if (!v) return fallback;
    if (v->kind == ND_CSS_V_LENGTH) return v->u.length.v;
    return fallback;
}

static gboolean
style_is_absolute_or_fixed(const nd_style *s)
{
    const nd_css_value *v = s ? s->values[ND_CSS_POSITION] : NULL;
    return keyword_is(v, "absolute") || keyword_is(v, "fixed");
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
    b->colspan = 1;
    b->rowspan = 1;
    return b;
}

static void
link_clear(gpointer data)
{
    nd_link_range *r = data;
    g_free(r->href);
    g_free(r->target);
}

static GArray *inline_links_ensure(nd_box *b);

nd_box_media *
nd_box_media_ensure(nd_box *b)
{
    if (!b->media) b->media = g_new0(nd_box_media, 1);
    return b->media;
}

static nd_box *
inline_merge_prefix(nd_box *prefix, nd_box *suffix)
{
    if (!prefix) return suffix;
    if (!suffix) return prefix;
    gsize plen = prefix->text ? strlen(prefix->text) : 0;
    gsize slen = suffix->text ? strlen(suffix->text) : 0;
    char *combined = g_malloc(plen + slen + 1);
    if (plen) memcpy(combined, prefix->text, plen);
    if (slen) memcpy(combined + plen, suffix->text, slen);
    combined[plen + slen] = '\0';
    g_free(suffix->text);
    suffix->text = combined;

    if (suffix->attrs) {
        for (guint i = 0; i < suffix->attrs->len; i++) {
            nd_inline_attr *a = &g_array_index(suffix->attrs, nd_inline_attr, i);
            a->start += plen;
        }
    }
    if (suffix->links) {
        for (guint i = 0; i < suffix->links->len; i++) {
            nd_link_range *l = &g_array_index(suffix->links, nd_link_range, i);
            l->start += plen;
        }
    }
    if (prefix->attrs) {
        for (guint i = 0; i < prefix->attrs->len; i++) {
            nd_inline_attr a = g_array_index(prefix->attrs, nd_inline_attr, i);
            g_array_append_val(suffix->attrs, a);
        }
    }
    if (prefix->links) {
        GArray *dst = inline_links_ensure(suffix);
        for (guint i = 0; i < prefix->links->len; i++) {
            nd_link_range src = g_array_index(prefix->links, nd_link_range, i);
            nd_link_range dup = src;
            dup.href   = src.href   ? g_strdup(src.href)   : NULL;
            dup.target = src.target ? g_strdup(src.target) : NULL;
            g_array_append_val(dst, dup);
        }
    }
    nd_box_free(prefix);
    return suffix;
}

static nd_box *
box_new_inline(void)
{
    nd_box *b = box_new(ND_BOX_INLINE);
    b->attrs = g_array_new(FALSE, FALSE, sizeof(nd_inline_attr));
    return b;
}

static GArray *
inline_links_ensure(nd_box *b)
{
    if (!b->links) {
        b->links = g_array_new(FALSE, FALSE, sizeof(nd_link_range));
        g_array_set_clear_func(b->links, link_clear);
    }
    return b->links;
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
    GPtrArray *stack = g_ptr_array_new();
    g_ptr_array_add(stack, box);
    while (stack->len > 0) {
        nd_box *cur = g_ptr_array_index(stack, stack->len - 1);
        g_ptr_array_set_size(stack, stack->len - 1);
        for (nd_box *c = cur->first_child; c; ) {
            nd_box *next = c->next_sibling;
            g_ptr_array_add(stack, c);
            c = next;
        }
        if (cur->links) g_array_free(cur->links, TRUE);
        if (cur->attrs) g_array_free(cur->attrs, TRUE);
        g_free(cur->text);
        if (cur->media) {
            g_free(cur->media->image_src);
            g_free(cur->media->bg_image_src);
            g_free(cur->media->video_src);
            g_free(cur->media->video_poster);
            g_free(cur->media->video_audio_src);
            g_free(cur->media);
        }
        g_free(cur);
    }
    g_ptr_array_free(stack, TRUE);
}

static gboolean
is_replaced_block_tag(const char *name)
{
    return name && (strcmp(name, "img") == 0 ||
                    strcmp(name, "picture") == 0 ||
                    strcmp(name, "video") == 0 ||
                    strcmp(name, "table") == 0);
}

#define ND_LAYOUT_MAX_DEPTH 512

static gboolean
contains_block_media_depth(const nd_node *n, int depth)
{
    if (!n || depth >= ND_LAYOUT_MAX_DEPTH || n->kind != ND_NODE_ELEMENT)
        return FALSE;
    for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind != ND_NODE_ELEMENT || !c->name) continue;
        if (is_replaced_block_tag(c->name) ||
            strcmp(c->name, "iframe") == 0)
            return TRUE;
        if (contains_block_media_depth(c, depth + 1)) return TRUE;
    }
    return FALSE;
}

static gboolean
contains_block_media(const nd_node *n)
{
    return contains_block_media_depth(n, 0);
}

static gboolean
is_inline_dom(const nd_node *n, GHashTable *styles)
{
    if (!n) return FALSE;
    if (n->kind == ND_NODE_TEXT) return TRUE;
    if (n->kind != ND_NODE_ELEMENT) return FALSE;
    if (is_replaced_block_tag(n->name)) return FALSE;
    const nd_style *s = g_hash_table_lookup(styles, n);
    if (!s) return FALSE;
    if (style_is_none(s)) return FALSE;
    if (style_is_absolute_or_fixed(s)) return FALSE;
    if (!style_is_block(s) && contains_block_media(n)) return FALSE;
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
    return nd_node_is_element_named(n, "td") ||
           nd_node_is_element_named(n, "th");
}

static nd_box *build_block(const nd_node *n, GHashTable *styles);
static nd_box *build_inline_run(const nd_node *first, const nd_node *last_excl, GHashTable *styles);
static nd_box *build_pseudo_inline(const nd_style *ps);
static const nd_node *g_focused_input_for_layout;
static gsize          g_focused_caret_byte_for_layout;
static struct nd_image_cache *g_image_cache_for_layout;
static const char    *g_base_url_for_layout;
static nd_box *nd_layout_build_(const nd_node *doc, GHashTable *styles, double viewport_width);

typedef struct nd_abs_entry {
    const nd_node *dom;
    gboolean       fixed;
} nd_abs_entry;

static GArray  *g_abs_pending;
static gboolean g_abs_force_build;

static nd_box *
build_cell(const nd_node *n, GHashTable *styles)
{
    nd_box *cell = box_new(ND_BOX_TABLE_CELL);
    cell->dom = n;
    cell->style = g_hash_table_lookup(styles, n);
    const char *cs_attr = nd_element_get_attr(n, "colspan");
    if (cs_attr) cell->colspan = nd_parse_int(cs_attr, 1, 1, 100);
    const char *rs_attr = nd_element_get_attr(n, "rowspan");
    if (rs_attr) cell->rowspan = nd_parse_int(rs_attr, 1, 1, 100);
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
            nd_box *child = build_block(c, styles);
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
    int  q_depth;
    gsize bold_start;
    gsize italic_start;
    gsize mono_start;
    gsize underline_start;
    gsize strike_start;
    const char *text_transform;
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
emit_form_attr(GArray *attrs, nd_inline_attr_kind k, gsize start, gsize end,
               const nd_node *dom)
{
    if (end <= start) return;
    nd_inline_attr a = {
        .kind = k, .start = start, .len = end - start, .dom = dom,
    };
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

static char *
apply_text_transform(const char *src, const char *tt)
{
    if (!src || !tt) return NULL;
    if (strcmp(tt, "uppercase") == 0)
        return g_utf8_strup(src, -1);
    if (strcmp(tt, "lowercase") == 0)
        return g_utf8_strdown(src, -1);
    if (strcmp(tt, "capitalize") == 0) {
        GString *out = g_string_new(NULL);
        gboolean at_word_start = TRUE;
        for (const char *p = src; p && *p; ) {
            gunichar c = g_utf8_get_char(p);
            const char *next = g_utf8_next_char(p);
            if (g_unichar_isspace(c) || c == '-' || c == '/') {
                g_string_append_len(out, p, next - p);
                at_word_start = TRUE;
            } else {
                if (at_word_start) {
                    gunichar uc = g_unichar_toupper(c);
                    char buf[8];
                    gint nb = g_unichar_to_utf8(uc, buf);
                    g_string_append_len(out, buf, nb);
                    at_word_start = FALSE;
                } else {
                    g_string_append_len(out, p, next - p);
                }
            }
            p = next;
        }
        return g_string_free(out, FALSE);
    }
    return NULL;
}

static void
collect_walk(const nd_node *n, collector_ctx *ctx)
{
    if (!n) return;
    if (n->kind == ND_NODE_TEXT) {
        if (!n->text) return;
        gsize start = ctx->out->len;
        char *xformed = ctx->text_transform
                        ? apply_text_transform(n->text, ctx->text_transform)
                        : NULL;
        g_string_append(ctx->out, xformed ? xformed : n->text);
        g_free(xformed);
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
    if (n->name && (strcmp(n->name, "style") == 0 ||
                    strcmp(n->name, "script") == 0 ||
                    strcmp(n->name, "head")   == 0 ||
                    strcmp(n->name, "title")  == 0 ||
                    strcmp(n->name, "noscript") == 0 ||
                    strcmp(n->name, "template") == 0))
        return;
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
    if (strcmp(n->name, "progress") == 0 || strcmp(n->name, "meter") == 0) {
        const char *vs = nd_element_get_attr(n, "value");
        const char *ms = nd_element_get_attr(n, "max");
        double v = vs ? g_ascii_strtod(vs, NULL) : 0;
        double m = ms ? g_ascii_strtod(ms, NULL) : (strcmp(n->name, "progress") == 0 ? 1 : 1);
        if (m <= 0) m = 1;
        int pct = (int)(100.0 * v / m + 0.5);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        char bar[12] = { '[' };
        int fill = pct / 10;
        for (int i = 0; i < 10; i++) bar[1 + i] = (i < fill) ? '#' : '-';
        bar[11] = ']';
        gsize start = ctx->out->len;
        g_string_append_len(ctx->out, bar, 12);
        g_string_append_printf(ctx->out, " %d%%", pct);
        emit_attr(ctx->attrs, ND_INLINE_MONOSPACE, start, ctx->out->len);
        return;
    }
    if (strcmp(n->name, "input") == 0) {
        const char *type = nd_element_get_attr(n, "type");
        gboolean is_password = type && g_ascii_strcasecmp(type, "password") == 0;
        gboolean is_text = !type || !*type ||
                           is_password ||
                           g_ascii_strcasecmp(type, "text") == 0 ||
                           g_ascii_strcasecmp(type, "search") == 0 ||
                           g_ascii_strcasecmp(type, "email") == 0 ||
                           g_ascii_strcasecmp(type, "url") == 0 ||
                           g_ascii_strcasecmp(type, "tel") == 0 ||
                           g_ascii_strcasecmp(type, "number") == 0;
        if (is_text) {
            gsize start = ctx->out->len;
            g_string_append(ctx->out, "\xc2\xa0");
            const char *real_value = nd_element_get_attr(n, "value");
            const char *v = real_value;
            if (!v || !*v) v = nd_element_get_attr(n, "placeholder");
            gboolean focused = (n == g_focused_input_for_layout);
            gsize val_start = ctx->out->len;
            gsize caret_pos = val_start;
            gsize caret_byte = g_focused_caret_byte_for_layout;
            if (real_value && caret_byte > strlen(real_value))
                caret_byte = strlen(real_value);
            const char *size_str = nd_element_get_attr(n, "size");
            int size = size_str ? nd_parse_int(size_str, 20, 4, 80) : 20;
            glong displayed_chars = 0;
            if (v && *v && is_password) {
                glong cps = g_utf8_strlen(v, -1);
                for (glong i = 0; i < cps; i++)
                    g_string_append(ctx->out, "\xe2\x80\xa2");
                displayed_chars = cps;
                if (focused) {
                    glong cp_before = real_value
                        ? g_utf8_pointer_to_offset(real_value, real_value + caret_byte)
                        : 0;
                    caret_pos = val_start + (gsize)cp_before * 3;
                }
            } else if (v && *v) {
                g_string_append(ctx->out, v);
                displayed_chars = g_utf8_strlen(v, -1);
                if (focused && real_value && *real_value)
                    caret_pos = val_start + caret_byte;
                else if (focused)
                    caret_pos = val_start;
            } else {
                if (focused) caret_pos = val_start;
            }
            for (glong i = displayed_chars; i < size; i++)
                g_string_append(ctx->out, "\xc2\xa0");
            g_string_append(ctx->out, "\xc2\xa0");
            nd_inline_attr_kind kind = focused
                                       ? ND_INLINE_INPUT_FIELD_FOCUSED
                                       : ND_INLINE_INPUT_FIELD;
            emit_form_attr(ctx->attrs, kind, start, ctx->out->len, n);
            if (focused)
                emit_attr(ctx->attrs, ND_INLINE_CARET, caret_pos, caret_pos + 1);
        } else if (type && (g_ascii_strcasecmp(type, "submit") == 0 ||
                            g_ascii_strcasecmp(type, "button") == 0 ||
                            g_ascii_strcasecmp(type, "reset") == 0)) {
            const char *v = nd_element_get_attr(n, "value");
            if (!v || !*v) v = g_ascii_strcasecmp(type, "submit") == 0 ? "Submit"
                              : g_ascii_strcasecmp(type, "reset")  == 0 ? "Reset"
                                                                        : "Button";
            gsize start = ctx->out->len;
            g_string_append_printf(ctx->out, "\xc2\xa0%s\xc2\xa0", v);
            emit_form_attr(ctx->attrs, ND_INLINE_BUTTON, start, ctx->out->len, n);
            g_string_append_c(ctx->out, ' ');
        } else if (type && g_ascii_strcasecmp(type, "checkbox") == 0) {
            const char *checked = nd_element_get_attr(n, "checked");
            g_string_append(ctx->out, checked ? "\xe2\x98\x91" : "\xe2\x98\x90");
        } else if (type && g_ascii_strcasecmp(type, "radio") == 0) {
            const char *checked = nd_element_get_attr(n, "checked");
            g_string_append(ctx->out, checked ? "\xe2\x97\x89" : "\xe2\x97\x8b");
        } else if (type && g_ascii_strcasecmp(type, "file") == 0) {
            gsize start = ctx->out->len;
            g_string_append(ctx->out, "\xc2\xa0" "Choose File" "\xc2\xa0");
            emit_form_attr(ctx->attrs, ND_INLINE_BUTTON, start, ctx->out->len, n);
            const char *fpath = nd_element_get_attr(n, "data-nd-file-path");
            if (fpath && *fpath) {
                const char *base = strrchr(fpath, '/');
#ifdef G_OS_WIN32
                const char *base_w = strrchr(fpath, '\\');
                if (!base || (base_w && base_w > base)) base = base_w;
#endif
                const char *show = base ? base + 1 : fpath;
                g_string_append_c(ctx->out, ' ');
                g_string_append(ctx->out, show);
            } else {
                g_string_append(ctx->out, " (no file chosen)");
            }
        } else if (type && g_ascii_strcasecmp(type, "color") == 0) {
            const char *v = nd_element_get_attr(n, "value");
            const char *hex = v && *v ? v : "#000000";
            gsize start = ctx->out->len;
            g_string_append(ctx->out, "\xc2\xa0");
            gsize swatch_start = ctx->out->len;
            g_string_append(ctx->out, "\xe2\x96\xa0");
            gsize swatch_end = ctx->out->len;
            g_string_append(ctx->out, "\xc2\xa0");
            g_string_append(ctx->out, hex);
            g_string_append(ctx->out, "\xc2\xa0");
            emit_form_attr(ctx->attrs, ND_INLINE_INPUT_FIELD, start, ctx->out->len, n);
            guint8 r8 = 0, g8 = 0, b8 = 0;
            if (hex[0] == '#' && strlen(hex) >= 7) {
                unsigned int rv, gv, bv;
                if (sscanf(hex + 1, "%2x%2x%2x", &rv, &gv, &bv) == 3) {
                    r8 = (guint8)rv; g8 = (guint8)gv; b8 = (guint8)bv;
                }
            }
            emit_color_attr(ctx->attrs, swatch_start, swatch_end, r8, g8, b8, 255);
        } else if (type && (g_ascii_strcasecmp(type, "range") == 0)) {
            const char *v = nd_element_get_attr(n, "value");
            const char *mn = nd_element_get_attr(n, "min");
            const char *mx = nd_element_get_attr(n, "max");
            double vv = v && *v ? g_ascii_strtod(v, NULL) : 50;
            double mnv = mn && *mn ? g_ascii_strtod(mn, NULL) : 0;
            double mxv = mx && *mx ? g_ascii_strtod(mx, NULL) : 100;
            if (mxv <= mnv) mxv = mnv + 1;
            double frac = (vv - mnv) / (mxv - mnv);
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            int knob_at = (int)(frac * 10 + 0.5);
            gsize start = ctx->out->len;
            g_string_append(ctx->out, "\xc2\xa0");
            for (int i = 0; i <= 10; i++) {
                if (i == knob_at)
                    g_string_append(ctx->out, "\xe2\x97\x8f");
                else
                    g_string_append(ctx->out, "\xe2\x94\x80");
            }
            g_string_append_printf(ctx->out, " %g\xc2\xa0", vv);
            emit_form_attr(ctx->attrs, ND_INLINE_INPUT_FIELD, start, ctx->out->len, n);
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
            emit_form_attr(ctx->attrs, ND_INLINE_INPUT_FIELD, start, ctx->out->len, n);
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
        emit_form_attr(ctx->attrs, ND_INLINE_BUTTON, start, ctx->out->len, n);
        g_free(label);
        return;
    }
    if (strcmp(n->name, "select") == 0) {
        gboolean multi = nd_element_get_attr(n, "multiple") != NULL;
        const char *size_attr = nd_element_get_attr(n, "size");
        int size_n = size_attr ? nd_parse_int(size_attr, 0, 0, 1000) : 0;
        gboolean listbox = multi || size_n > 1;
        if (listbox) {
            gsize start = ctx->out->len;
            g_string_append(ctx->out, "\xc2\xa0");
            int shown = 0;
            int cap = size_n > 0 ? size_n : 6;
            for (const nd_node *c = n->first_child; c && shown < cap; c = c->next_sibling) {
                if (c->kind != ND_NODE_ELEMENT || !c->name) continue;
                if (strcmp(c->name, "optgroup") == 0) {
                    const char *gl = nd_element_get_attr(c, "label");
                    if (gl && *gl) {
                        if (shown > 0) g_string_append(ctx->out, "\n");
                        g_string_append_printf(ctx->out, "  %s", gl);
                        shown++;
                    }
                    for (const nd_node *opt = c->first_child;
                         opt && shown < cap; opt = opt->next_sibling) {
                        if (opt->kind != ND_NODE_ELEMENT || !opt->name ||
                            strcmp(opt->name, "option") != 0) continue;
                        if (shown > 0) g_string_append(ctx->out, "\n");
                        gboolean sel = nd_element_get_attr(opt, "selected") != NULL;
                        char *t = nd_node_collect_text(opt);
                        g_string_append(ctx->out, sel ? "\xe2\x96\xb8 " : "    ");
                        g_string_append(ctx->out, t ? t : "");
                        g_free(t);
                        shown++;
                    }
                } else if (strcmp(c->name, "option") == 0) {
                    if (shown > 0) g_string_append(ctx->out, "\n");
                    gboolean sel = nd_element_get_attr(c, "selected") != NULL;
                    char *t = nd_node_collect_text(c);
                    g_string_append(ctx->out, sel ? "\xe2\x96\xb8 " : "  ");
                    g_string_append(ctx->out, t ? t : "");
                    g_free(t);
                    shown++;
                }
            }
            if (shown == 0) g_string_append(ctx->out, "  ");
            g_string_append(ctx->out, "\xc2\xa0");
            emit_attr(ctx->attrs, ND_INLINE_INPUT_FIELD, start, ctx->out->len);
            return;
        }
        const nd_node *chosen = nd_select_chosen_option(n);
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
        gboolean focused = (n == g_focused_input_for_layout);
        g_string_append(ctx->out, "\xc2\xa0");
        gsize val_start = ctx->out->len;
        gboolean any = FALSE;
        gsize value_byte_len = 0;
        for (const nd_node *c = n->first_child; c; c = c->next_sibling)
            if (c->kind == ND_NODE_TEXT && c->text && *c->text) {
                gsize tlen = strlen(c->text);
                g_string_append_len(ctx->out, c->text, (gssize)tlen);
                value_byte_len += tlen;
                any = TRUE;
            }
        gsize caret_byte = g_focused_caret_byte_for_layout;
        if (caret_byte > value_byte_len) caret_byte = value_byte_len;
        gsize caret_pos = val_start + caret_byte;
        if (!any) {
            for (int i = 0; i < 40; i++) g_string_append(ctx->out, "\xc2\xa0");
            if (focused) caret_pos = val_start;
            else         caret_pos = 0;
        }
        g_string_append(ctx->out, "\xc2\xa0");
        nd_inline_attr_kind ta_kind = focused
                                       ? ND_INLINE_INPUT_FIELD_FOCUSED
                                       : ND_INLINE_INPUT_FIELD;
        emit_attr(ctx->attrs, ta_kind, start, ctx->out->len);
        if (focused)
            emit_attr(ctx->attrs, ND_INLINE_CARET, caret_pos, caret_pos + 1);
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
    double ml = length_or(s ? s->values[ND_CSS_MARGIN_LEFT]  : NULL, 0);
    double mr = length_or(s ? s->values[ND_CSS_MARGIN_RIGHT] : NULL, 0);
    if (ml >= 3.0) g_string_append_c(ctx->out, ' ');
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
            int n_w = nd_parse_int(kw, 0, 0, 1000);
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

    gboolean is_q = strcmp(n->name, "q") == 0;
    if (is_q) {
        g_string_append(ctx->out,
            (ctx->q_depth % 2 == 0) ? "\xe2\x80\x9c" : "\xe2\x80\x98");
        ctx->q_depth++;
    }

    gboolean sup = strcmp(n->name, "sup") == 0;
    gboolean sub = strcmp(n->name, "sub") == 0;
    gsize rise_start = ctx->out->len;
    gboolean small_caps = s && keyword_is(s->values[ND_CSS_FONT_VARIANT],
                                          "small-caps");
    gsize sc_start = ctx->out->len;

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

    const char *prev_text_transform = ctx->text_transform;
    if (s && s->values[ND_CSS_TEXT_TRANSFORM] &&
        s->values[ND_CSS_TEXT_TRANSFORM]->kind == ND_CSS_V_KEYWORD) {
        const char *kw = s->values[ND_CSS_TEXT_TRANSFORM]->u.keyword;
        if (strcmp(kw, "none") == 0)
            ctx->text_transform = NULL;
        else if (strcmp(kw, "uppercase") == 0 ||
                 strcmp(kw, "lowercase") == 0 ||
                 strcmp(kw, "capitalize") == 0)
            ctx->text_transform = kw;
    }

    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        collect_walk(c, ctx);

    ctx->text_transform = prev_text_transform;

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
    if (sup && ctx->out->len > rise_start)
        emit_attr(ctx->attrs, ND_INLINE_SUPERSCRIPT, rise_start, ctx->out->len);
    if (sub && ctx->out->len > rise_start)
        emit_attr(ctx->attrs, ND_INLINE_SUBSCRIPT, rise_start, ctx->out->len);
    if (small_caps && ctx->out->len > sc_start)
        emit_attr(ctx->attrs, ND_INLINE_SMALL_CAPS, sc_start, ctx->out->len);
    if (is_q) {
        ctx->q_depth--;
        g_string_append(ctx->out,
            (ctx->q_depth % 2 == 0) ? "\xe2\x80\x9d" : "\xe2\x80\x99");
    }
    if (mr >= 3.0) g_string_append_c(ctx->out, ' ');
    ctx->active_href   = prev_href;
    ctx->active_target = prev_target;
    ctx->active_link_node = prev_link_node;
}

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

static gboolean
is_white_space_preserving(const nd_node *node, GHashTable *styles)
{
    if (is_preformatted_parent(node ? node->parent : NULL)) return TRUE;
    for (const nd_node *p = node; p; p = p->parent) {
        if (p->kind != ND_NODE_ELEMENT) continue;
        const nd_style *ps = g_hash_table_lookup(styles, p);
        if (!ps) continue;
        const nd_css_value *ws = ps->values[ND_CSS_WHITE_SPACE];
        if (ws && ws->kind == ND_CSS_V_KEYWORD && ws->u.keyword) {
            const char *kw = ws->u.keyword;
            if (strcmp(kw, "pre") == 0 ||
                strcmp(kw, "pre-wrap") == 0 ||
                strcmp(kw, "break-spaces") == 0 ||
                strcmp(kw, "pre-line") == 0)
                return TRUE;
            if (strcmp(kw, "normal") == 0 || strcmp(kw, "nowrap") == 0)
                return FALSE;
        }
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
    if (first && first->parent && first->parent->kind == ND_NODE_ELEMENT &&
        first->parent->name &&
        strcmp(first->parent->name, "summary") == 0 &&
        first->parent->parent &&
        first->parent->parent->kind == ND_NODE_ELEMENT &&
        first->parent->parent->name &&
        strcmp(first->parent->parent->name, "details") == 0 &&
        first == first->parent->first_child) {
        gboolean open = nd_element_get_attr(first->parent->parent, "open") != NULL;
        g_string_append(buf, open ? "\xe2\x96\xbe " : "\xe2\x96\xb8 ");
    }
    for (const nd_node *n = first; n && n != last_excl; n = n->next_sibling)
        collect_walk(n, &ctx);

    gboolean preformatted = first &&
                            is_white_space_preserving(first, styles);

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
        g_array_append_val(inline_links_ensure(box), out);
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

static char *
first_url_from_srcset(const char *srcset)
{
    if (!srcset) return NULL;
    while (*srcset && (g_ascii_isspace(*srcset) || *srcset == ',')) srcset++;
    if (!*srcset) return NULL;
    const char *end = srcset;
    while (*end && !g_ascii_isspace(*end)) end++;
    gsize len = (gsize)(end - srcset);
    while (len > 0 && srcset[len - 1] == ',') len--;
    if (len == 0) return NULL;
    return g_strndup(srcset, len);
}

static gboolean
nd_pixbuf_likely_supports(const char *mime)
{
    if (!mime || !*mime) return TRUE;
    return nd_image_pixbuf_supports_mime(mime);
}

static char *
pick_picture_source_url(const nd_node *picture)
{
    if (!picture) return NULL;
    char *data_fallback = NULL;
    for (const nd_node *c = picture->first_child; c; c = c->next_sibling) {
        if (c->kind != ND_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "source") != 0) continue;
        const char *type = nd_element_get_attr(c, "type");
        if (type && !nd_pixbuf_likely_supports(type)) continue;
        const char *dsset = nd_element_get_attr(c, "data-srcset");
        char *u = first_url_from_srcset(dsset);
        if (u && !g_str_has_prefix(u, "data:")) return u;
        if (u && !data_fallback) data_fallback = u;
        else g_free(u);
        const char *ss = nd_element_get_attr(c, "srcset");
        u = first_url_from_srcset(ss);
        if (u && !g_str_has_prefix(u, "data:")) return u;
        if (u && !data_fallback) data_fallback = u;
        else g_free(u);
        const char *s = nd_element_get_attr(c, "src");
        if (s && *s) {
            if (!g_str_has_prefix(s, "data:")) return g_strdup(s);
            if (!data_fallback) data_fallback = g_strdup(s);
        }
    }
    return data_fallback;
}

static char *
pick_img_url(const nd_node *n)
{
    if (!n) return NULL;
    const char *src    = nd_element_get_attr(n, "src");
    const char *srcset = nd_element_get_attr(n, "srcset");
    const char *dsrc   = nd_element_get_attr(n, "data-src");
    if (!dsrc || !*dsrc) dsrc = nd_element_get_attr(n, "data-original");
    if (!dsrc || !*dsrc) dsrc = nd_element_get_attr(n, "data-lazy-src");
    const char *dsset  = nd_element_get_attr(n, "data-srcset");
    if (!dsset || !*dsset) dsset = nd_element_get_attr(n, "data-lazy-srcset");

    if (dsrc && *dsrc) return g_strdup(dsrc);
    char *u = first_url_from_srcset(dsset);
    if (u) return u;

    gboolean placeholder = src && g_str_has_prefix(src, "data:");
    if (src && *src && !placeholder) return g_strdup(src);
    u = first_url_from_srcset(srcset);
    if (u) return u;
    if (src && *src) return g_strdup(src);
    return NULL;
}

static nd_box *
build_image_box(const nd_node *n)
{
    const nd_node *img = n;
    char *url = NULL;
    if (n->name && strcmp(n->name, "picture") == 0) {
        for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
            if (nd_node_is_element_named(c, "img")) {
                img = c;
                break;
            }
        }
        if (img != n) url = pick_img_url(img);
        if (!url || g_str_has_prefix(url, "data:")) {
            char *source_url = pick_picture_source_url(n);
            if (source_url && !g_str_has_prefix(source_url, "data:")) {
                g_free(url);
                url = source_url;
            } else if (source_url && !url) {
                url = source_url;
            } else {
                g_free(source_url);
            }
        }
    } else {
        url = pick_img_url(n);
    }
    if (!url) return NULL;

    nd_box *box = box_new(ND_BOX_IMAGE);
    box->dom = img;
    nd_box_media *m = nd_box_media_ensure(box);
    m->image_src = url;
    const char *ws = nd_element_get_attr(img, "width");
    const char *hs = nd_element_get_attr(img, "height");
    box->content_width  = ws ? g_ascii_strtod(ws, NULL) : 0;
    box->content_height = hs ? g_ascii_strtod(hs, NULL) : 0;
    if (g_image_cache_for_layout) {
        char *abs = g_base_url_for_layout
            ? nd_url_resolve(g_base_url_for_layout, url)
            : NULL;
        m->image = nd_image_cache_peek(g_image_cache_for_layout,
                                       abs ? abs : url);
        g_free(abs);
    }
    return box;
}

static const char *
video_source_url(const nd_node *n)
{
    const char *src = nd_element_get_attr(n, "src");
    if (src && *src) return src;
    for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind != ND_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "source") != 0) continue;
        const char *type = nd_element_get_attr(c, "type");
        const char *csrc = nd_element_get_attr(c, "src");
        if (!csrc || !*csrc) continue;
        if (!type || g_str_has_prefix(type, "video/webm") ||
            g_str_has_prefix(type, "video/mp4"))
            return csrc;
    }
    return NULL;
}

static nd_box *
build_video_box(const nd_node *n)
{
    const char *src = video_source_url(n);
    nd_box *box = box_new(ND_BOX_VIDEO);
    box->dom = n;
    nd_box_media *m = nd_box_media_ensure(box);
    if (src) m->video_src = g_strdup(src);
    const char *poster = nd_element_get_attr(n, "poster");
    if (poster && *poster) m->video_poster = g_strdup(poster);
    const char *ws = nd_element_get_attr(n, "width");
    const char *hs = nd_element_get_attr(n, "height");
    box->content_width  = ws ? g_ascii_strtod(ws, NULL) : 320;
    box->content_height = hs ? g_ascii_strtod(hs, NULL) : 180;
    m->video_loop = nd_element_get_attr(n, "loop") != NULL;
    const char *audio = nd_element_get_attr(n, "data-audio-src");
    if (audio && *audio) m->video_audio_src = g_strdup(audio);
    return box;
}

static nd_box *
build_pseudo_inline(const nd_style *ps)
{
    if (!ps) return NULL;
    const nd_css_value *cv = ps->values[ND_CSS_CONTENT];
    if (!cv || cv->kind != ND_CSS_V_KEYWORD || !cv->u.keyword) return NULL;
    const char *txt = cv->u.keyword;
    if (!*txt || strcmp(txt, "none") == 0 || strcmp(txt, "normal") == 0)
        return NULL;

    nd_box *box = box_new_inline();
    box->text = g_strdup(txt);
    box->style = ps;

    gsize tlen = strlen(box->text);
    if (ps->values[ND_CSS_COLOR] && ps->values[ND_CSS_COLOR]->kind == ND_CSS_V_COLOR) {
        nd_inline_attr a = {
            .kind = ND_INLINE_COLOR,
            .start = 0, .len = tlen,
            .r = ps->values[ND_CSS_COLOR]->u.color.r,
            .g = ps->values[ND_CSS_COLOR]->u.color.g,
            .b = ps->values[ND_CSS_COLOR]->u.color.b,
            .a = ps->values[ND_CSS_COLOR]->u.color.a,
        };
        g_array_append_val(box->attrs, a);
    }
    if (ps->values[ND_CSS_BACKGROUND_COLOR] &&
        ps->values[ND_CSS_BACKGROUND_COLOR]->kind == ND_CSS_V_COLOR) {
        nd_inline_attr a = {
            .kind = ND_INLINE_BG_COLOR,
            .start = 0, .len = tlen,
            .r = ps->values[ND_CSS_BACKGROUND_COLOR]->u.color.r,
            .g = ps->values[ND_CSS_BACKGROUND_COLOR]->u.color.g,
            .b = ps->values[ND_CSS_BACKGROUND_COLOR]->u.color.b,
            .a = ps->values[ND_CSS_BACKGROUND_COLOR]->u.color.a,
        };
        g_array_append_val(box->attrs, a);
    }
    if (ps->values[ND_CSS_FONT_SIZE] &&
        ps->values[ND_CSS_FONT_SIZE]->kind == ND_CSS_V_LENGTH &&
        ps->values[ND_CSS_FONT_SIZE]->u.length.unit == ND_CSS_UNIT_PX) {
        nd_inline_attr a = {
            .kind = ND_INLINE_FONT_SIZE,
            .start = 0, .len = tlen,
            .font_size_px = ps->values[ND_CSS_FONT_SIZE]->u.length.v,
        };
        g_array_append_val(box->attrs, a);
    }
    const nd_css_value *fw = ps->values[ND_CSS_FONT_WEIGHT];
    if (keyword_is(fw, "bold") || keyword_is(fw, "bolder")) {
        nd_inline_attr a = { .kind = ND_INLINE_BOLD, .start = 0, .len = tlen };
        g_array_append_val(box->attrs, a);
    }
    if (keyword_is(ps->values[ND_CSS_FONT_STYLE], "italic")) {
        nd_inline_attr a = { .kind = ND_INLINE_ITALIC, .start = 0, .len = tlen };
        g_array_append_val(box->attrs, a);
    }
    return box;
}

static int g_build_block_depth;

static nd_box *build_block_impl(const nd_node *n, GHashTable *styles);

static nd_box *
build_block(const nd_node *n, GHashTable *styles)
{
    if (!n || g_build_block_depth >= ND_LAYOUT_MAX_DEPTH) return NULL;
    g_build_block_depth++;
    nd_box *out = build_block_impl(n, styles);
    g_build_block_depth--;
    return out;
}

static nd_box *
build_block_impl(const nd_node *n, GHashTable *styles)
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
    if (s && style_is_absolute_or_fixed(s)) {
        if (!g_abs_force_build) {
            if (g_abs_pending) {
                nd_abs_entry e;
                e.dom = n;
                const nd_css_value *pv = s->values[ND_CSS_POSITION];
                e.fixed = pv && pv->kind == ND_CSS_V_KEYWORD && pv->u.keyword &&
                          strcmp(pv->u.keyword, "fixed") == 0;
                g_array_append_val(g_abs_pending, e);
            }
            return NULL;
        }
        g_abs_force_build = FALSE;
    }

    if (n->name && (strcmp(n->name, "img") == 0 ||
                    strcmp(n->name, "picture") == 0)) {
        nd_box *ib = build_image_box(n);
        if (ib) ib->style = s;
        return ib;
    }

    if (n->name && strcmp(n->name, "video") == 0) {
        nd_box *vb = build_video_box(n);
        if (vb) vb->style = s;
        return vb;
    }

    if (n->name && strcmp(n->name, "table") == 0)
        return build_table(n, styles);

    if (!style_is_block(s) && !contains_block_media(n) &&
        !style_is_absolute_or_fixed(s)) return NULL;

    nd_box *block = box_new(ND_BOX_BLOCK);
    block->dom = n;
    block->style = s;

    if (s && s->values[ND_CSS_BACKGROUND_IMAGE] &&
        s->values[ND_CSS_BACKGROUND_IMAGE]->kind == ND_CSS_V_URL &&
        s->values[ND_CSS_BACKGROUND_IMAGE]->u.url) {
        nd_box_media *m = nd_box_media_ensure(block);
        m->bg_image_src = g_strdup(s->values[ND_CSS_BACKGROUND_IMAGE]->u.url);
        if (g_image_cache_for_layout) {
            char *abs = g_base_url_for_layout
                ? nd_url_resolve(g_base_url_for_layout, m->bg_image_src)
                : NULL;
            m->bg_image = nd_image_cache_peek(g_image_cache_for_layout,
                                              abs ? abs : m->bg_image_src);
            g_free(abs);
        }
    }

    gboolean details_collapsed = FALSE;
    if (n->name && strcmp(n->name, "details") == 0 &&
        !nd_element_get_attr(n, "open"))
        details_collapsed = TRUE;

    nd_box *pending_before = (s && s->before)
        ? build_pseudo_inline(s->before) : NULL;

    gboolean is_flex = style_is_flex_container(s);

    const nd_node *c = n->first_child;
    while (c) {
        if (details_collapsed) {
            if (c->kind != ND_NODE_ELEMENT || !c->name ||
                strcmp(c->name, "summary") != 0) {
                c = c->next_sibling;
                continue;
            }
        }
        if (is_flex) {
            if (c->kind == ND_NODE_TEXT) {
                gboolean ws_only = TRUE;
                if (c->text) {
                    for (const char *p = c->text; *p; p++)
                        if (!g_ascii_isspace((unsigned char)*p)) { ws_only = FALSE; break; }
                }
                if (ws_only) { c = c->next_sibling; continue; }
                nd_box *item = box_new(ND_BOX_BLOCK);
                item->style = s;
                nd_box *run = build_inline_run(c, c->next_sibling, styles);
                if (pending_before) {
                    run = inline_merge_prefix(pending_before, run);
                    pending_before = NULL;
                }
                if (run && run->text && run->text[0]) {
                    box_append_child(item, run);
                    box_append_child(block, item);
                } else {
                    if (run) nd_box_free(run);
                    nd_box_free(item);
                }
                c = c->next_sibling;
                continue;
            }
            if (c->kind != ND_NODE_ELEMENT) { c = c->next_sibling; continue; }
            const nd_style *cs = g_hash_table_lookup(styles, c);
            if (cs && style_is_none(cs)) { c = c->next_sibling; continue; }
            if (cs && style_is_absolute_or_fixed(cs)) {
                nd_box *child = build_block(c, styles);
                if (child) box_append_child(block, child);
                c = c->next_sibling;
                continue;
            }
            if (style_is_block(cs) ||
                (c->name && (strcmp(c->name, "img") == 0 ||
                             strcmp(c->name, "picture") == 0 ||
                             strcmp(c->name, "video") == 0 ||
                             strcmp(c->name, "table") == 0))) {
                nd_box *child = build_block(c, styles);
                if (child) box_append_child(block, child);
                c = c->next_sibling;
                continue;
            }
            nd_box *item = box_new(ND_BOX_BLOCK);
            item->dom = c;
            item->style = cs;
            nd_box *run = build_inline_run(c, c->next_sibling, styles);
            if (pending_before) {
                run = inline_merge_prefix(pending_before, run);
                pending_before = NULL;
            }
            if (run && run->text && run->text[0]) {
                box_append_child(item, run);
                box_append_child(block, item);
            } else {
                if (run) nd_box_free(run);
                nd_box_free(item);
            }
            c = c->next_sibling;
            continue;
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
            if (pending_before) {
                run = inline_merge_prefix(pending_before, run);
                pending_before = NULL;
            }

            if (run->text && run->text[0] != '\0')
                box_append_child(block, run);
            else
                nd_box_free(run);
        } else {
            if (pending_before) {
                box_append_child(block, pending_before);
                pending_before = NULL;
            }
            nd_box *child = build_block(c, styles);
            if (child) box_append_child(block, child);
            if (c) c = c->next_sibling;
        }
    }

    if (pending_before) {
        box_append_child(block, pending_before);
        pending_before = NULL;
    }

    if (s && s->after) {
        nd_box *gen = build_pseudo_inline(s->after);
        if (gen) box_append_child(block, gen);
    }
    return block;
}

static PangoLayout *
make_pango_layout(const nd_style *parent_style)
{
    static PangoContext *cached_ctx;
    if (!cached_ctx) {
        PangoFontMap *fm = pango_cairo_font_map_get_default();
        cached_ctx = pango_font_map_create_context(fm);
    }
    PangoLayout *layout = pango_layout_new(cached_ctx);
    nd_paint_apply_inline_font(layout, parent_style);
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

static const nd_node *
inline_box_form_hit(const nd_box *box, double local_x, double local_y,
                    const nd_style *parent_style)
{
    if (!box || !box->attrs || box->attrs->len == 0) return NULL;
    if (!box->text || !*box->text) return NULL;
    PangoLayout *layout = make_pango_layout(parent_style);
    pango_layout_set_width(layout, (int)(box->content_width * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_text(layout, box->text, -1);
    const nd_css_value *ta_v =
        parent_style ? parent_style->values[ND_CSS_TEXT_ALIGN] : NULL;
    if (keyword_is(ta_v, "center"))
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    else if (keyword_is(ta_v, "right") || keyword_is(ta_v, "end"))
        pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
    else
        pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);

    int index = 0, trailing = 0;
    gboolean inside = pango_layout_xy_to_index(
        layout,
        (int)(local_x * PANGO_SCALE),
        (int)(local_y * PANGO_SCALE),
        &index, &trailing);
    g_object_unref(layout);
    if (!inside) return NULL;
    if (index < 0) return NULL;
    gsize idx = (gsize)index;
    const nd_node *button_hit = NULL;
    const nd_node *field_hit = NULL;
    for (guint i = 0; i < box->attrs->len; i++) {
        const nd_inline_attr *r =
            &g_array_index(box->attrs, nd_inline_attr, i);
        if (r->kind != ND_INLINE_INPUT_FIELD &&
            r->kind != ND_INLINE_INPUT_FIELD_FOCUSED &&
            r->kind != ND_INLINE_BUTTON)
            continue;
        if (!r->dom) continue;
        if (idx < r->start || idx >= r->start + r->len) continue;
        if (r->kind == ND_INLINE_BUTTON) button_hit = r->dom;
        else if (!field_hit)             field_hit = r->dom;
    }
    if (button_hit) return button_hit;
    return field_hit;
}

static const nd_node *
nd_form_hit_walk(const nd_box *box, double x, double y,
                 const nd_style *inherited)
{
    if (!box) return NULL;
    const nd_style *child_inherited = box->style ? box->style : inherited;
    if (box->kind == ND_BOX_INLINE) {
        const nd_node *m = inline_box_form_hit(
            box, x - box->x, y - box->y, child_inherited);
        if (m) return m;
    }
    for (const nd_box *c = box->first_child; c; c = c->next_sibling) {
        const nd_node *m = nd_form_hit_walk(c, x, y, child_inherited);
        if (m) return m;
    }
    return NULL;
}

static void
layout_block(nd_box *box, double parent_content_width, const nd_style *inherited_style);
static void
layout_box(nd_box *box, double parent_content_width, const nd_style *inherited_style);
static double
measure_natural_width(nd_box *box, const nd_style *parent_style);

static int
float_side_of(const nd_style *s)
{
    if (!s) return -1;
    const nd_css_value *v = s->values[ND_CSS_FLOAT];
    if (!v || v->kind != ND_CSS_V_KEYWORD || !v->u.keyword) return -1;
    if (strcmp(v->u.keyword, "left") == 0) return 0;
    if (strcmp(v->u.keyword, "right") == 0) return 1;
    return -1;
}

static int
clear_kind_of(const nd_style *s)
{
    if (!s) return 0;
    const nd_css_value *v = s->values[ND_CSS_CLEAR];
    if (!v || v->kind != ND_CSS_V_KEYWORD || !v->u.keyword) return 0;
    if (strcmp(v->u.keyword, "left") == 0) return 1;
    if (strcmp(v->u.keyword, "right") == 0) return 2;
    if (strcmp(v->u.keyword, "both") == 0) return 3;
    return 0;
}

typedef struct float_ref {
    nd_box *box;
    int side;
    double top, bottom;
    double outer_w;
} float_ref;

static void
floats_offsets_at(const GArray *floats, double y,
                  double *left_out, double *right_out)
{
    double l = 0, r = 0;
    if (floats) {
        for (guint i = 0; i < floats->len; i++) {
            const float_ref *f = &g_array_index(floats, float_ref, i);
            if (y < f->top || y >= f->bottom) continue;
            if (f->side == 0) l += f->outer_w;
            else              r += f->outer_w;
        }
    }
    *left_out = l;
    *right_out = r;
}

static double
floats_clear_y(const GArray *floats, double y, int clear)
{
    if (!floats || clear == 0) return y;
    double out = y;
    for (guint i = 0; i < floats->len; i++) {
        const float_ref *f = &g_array_index(floats, float_ref, i);
        if (clear == 1 && f->side != 0) continue;
        if (clear == 2 && f->side != 1) continue;
        if (f->bottom > out) out = f->bottom;
    }
    return out;
}

static double
floats_max_bottom(const GArray *floats)
{
    double y = 0;
    if (!floats) return y;
    for (guint i = 0; i < floats->len; i++) {
        const float_ref *f = &g_array_index(floats, float_ref, i);
        if (f->bottom > y) y = f->bottom;
    }
    return y;
}

static void
layout_image(nd_box *box, double parent_content_width)
{
    edges_from_style(box->style, parent_content_width,
                     &box->margin, &box->padding, &box->border);
    const nd_css_value *wv  = box->style ? box->style->values[ND_CSS_WIDTH]      : NULL;
    const nd_css_value *hv  = box->style ? box->style->values[ND_CSS_HEIGHT]     : NULL;
    const nd_css_value *mxw = box->style ? box->style->values[ND_CSS_MAX_WIDTH]  : NULL;
    const nd_css_value *mxh = box->style ? box->style->values[ND_CSS_MAX_HEIGHT] : NULL;
    const nd_css_value *mnw = box->style ? box->style->values[ND_CSS_MIN_WIDTH]  : NULL;
    const nd_css_value *mnh = box->style ? box->style->values[ND_CSS_MIN_HEIGHT] : NULL;

    double w = -1, h = -1;
    if (wv && (wv->kind == ND_CSS_V_LENGTH || wv->kind == ND_CSS_V_CALC))
        w = length_resolve(wv, parent_content_width, -1);
    if (hv && (hv->kind == ND_CSS_V_LENGTH || hv->kind == ND_CSS_V_CALC))
        h = length_resolve(hv, parent_content_width, -1);

    const nd_image *img = box->media ? (const nd_image *)box->media->image : NULL;
    double nat_w = (img && img->loaded && img->natural_width > 0)
                   ? (double)img->natural_width  : -1;
    double nat_h = (img && img->loaded && img->natural_height > 0)
                   ? (double)img->natural_height : -1;

    if (w < 0 && h < 0) {
        if (nat_w > 0 && nat_h > 0) { w = nat_w; h = nat_h; }
        else { w = 0; h = 0; }
    } else if (w < 0) {
        w = (nat_w > 0 && nat_h > 0) ? h * (nat_w / nat_h) : h;
    } else if (h < 0) {
        h = (nat_w > 0 && nat_h > 0) ? w * (nat_h / nat_w) : w;
    }

    double max_w = length_resolve(mxw, parent_content_width, -1);
    double max_h = length_resolve(mxh, parent_content_width, -1);
    double min_w = length_resolve(mnw, parent_content_width, -1);
    double min_h = length_resolve(mnh, parent_content_width, -1);

    if (max_w >= 0 && w > max_w) {
        if (h > 0 && w > 0) h *= max_w / w;
        w = max_w;
    }
    if (max_h >= 0 && h > max_h) {
        if (w > 0 && h > 0) w *= max_h / h;
        h = max_h;
    }
    if (min_w >= 0 && w < min_w) w = min_w;
    if (min_h >= 0 && h < min_h) h = min_h;

    if (w > parent_content_width && parent_content_width > 0) {
        double ratio = (w > 0) ? h / w : 0;
        w = parent_content_width;
        h = w * ratio;
    }

    box->content_width = w;
    box->content_height = h;
}

static double
measure_natural_width(nd_box *box, const nd_style *parent_style)
{
    if (!box) return 0;
    if (box->kind == ND_BOX_INLINE) {
        if (!box->text || !*box->text) return 0;
        PangoLayout *layout = make_pango_layout(parent_style);
        pango_layout_set_width(layout, -1);
        pango_layout_set_text(layout, box->text, -1);
        int pw, ph;
        pango_layout_get_pixel_size(layout, &pw, &ph);
        g_object_unref(layout);
        return pw;
    }
    if (box->kind == ND_BOX_IMAGE || box->kind == ND_BOX_VIDEO) {
        return box->content_width > 0 ? box->content_width : 200;
    }
    if (box->kind == ND_BOX_TEXT) {
        return box->content_width > 0 ? box->content_width : 0;
    }
    const nd_style *child_style = box->style ? box->style : parent_style;
    double max_child = 0;
    for (nd_box *c = box->first_child; c; c = c->next_sibling) {
        double w = measure_natural_width(c, child_style);
        if (box->kind == ND_BOX_BLOCK) {
            if (w > max_child) max_child = w;
        } else {
            max_child += w;
        }
    }
    return max_child;
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
        for (nd_box *cell = row->first_child; cell; cell = cell->next_sibling)
            c += cell->colspan > 0 ? (guint)cell->colspan : 1;
        if (c > max_cols) max_cols = c;
    }
    if (max_cols == 0) { box->content_height = 0; return; }

    const nd_style *measure_inherited = box->style ? box->style : inherited_style;
    double *col_widths = g_new0(double, max_cols);
    for (nd_box *row = box->first_child; row; row = row->next_sibling) {
        guint col = 0;
        for (nd_box *cell = row->first_child; cell; cell = cell->next_sibling) {
            int span = cell->colspan > 0 ? cell->colspan : 1;
            const nd_style *cs = cell->style ? cell->style : measure_inherited;
            double natural = measure_natural_width(cell, cs);
            edges_from_style(cell->style, cw > 0 ? cw : 1000.0,
                             &cell->margin, &cell->padding, &cell->border);
            double cell_outer = natural
                + cell->padding.left + cell->padding.right
                + cell->border.left + cell->border.right
                + cell->margin.left + cell->margin.right;
            double per_col = cell_outer / (double)span;
            for (int i = 0; i < span && col + (guint)i < max_cols; i++) {
                if (per_col > col_widths[col + i])
                    col_widths[col + i] = per_col;
            }
            col += (guint)span;
        }
    }
    double natural_sum = 0;
    for (guint i = 0; i < max_cols; i++) natural_sum += col_widths[i];
    if (natural_sum > 0 && natural_sum > cw && cw > 0) {
        double scale = cw / natural_sum;
        for (guint i = 0; i < max_cols; i++) col_widths[i] *= scale;
    } else if (natural_sum > 0 && natural_sum < cw) {
        guint widest = 0;
        for (guint i = 1; i < max_cols; i++)
            if (col_widths[i] > col_widths[widest]) widest = i;
        col_widths[widest] += (cw - natural_sum);
    } else if (natural_sum == 0) {
        double evenly = cw / (double)max_cols;
        for (guint i = 0; i < max_cols; i++) col_widths[i] = evenly;
    }
    double *col_x = g_new0(double, max_cols);
    {
        double cx = 0;
        for (guint i = 0; i < max_cols; i++) { col_x[i] = cx; cx += col_widths[i]; }
    }

    double inner_x = box->x + box->margin.left + box->border.left + box->padding.left;
    double inner_y = box->y + box->margin.top  + box->border.top  + box->padding.top;
    double cursor_y = inner_y;
    const nd_style *child_inherited = box->style ? box->style : inherited_style;

    for (nd_box *row = box->first_child; row; row = row->next_sibling) {
        row->x = inner_x;
        row->y = cursor_y;
        row->content_width = cw;
        double row_height = 0;
        guint col = 0;
        for (nd_box *cell = row->first_child; cell; cell = cell->next_sibling) {
            int span = cell->colspan > 0 ? cell->colspan : 1;
            double cell_outer_w = 0;
            for (int i = 0; i < span && col + (guint)i < max_cols; i++)
                cell_outer_w += col_widths[col + i];
            cell->x = inner_x + (col < max_cols ? col_x[col] : 0);
            cell->y = cursor_y;
            const nd_style *cs = cell->style ? cell->style : child_inherited;
            edges_from_style(cell->style, cell_outer_w,
                             &cell->margin, &cell->padding, &cell->border);
            double cell_inner_w = cell_outer_w
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
            col += (guint)span;
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
    g_free(col_widths);
    g_free(col_x);
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
    } else if (box->kind == ND_BOX_VIDEO) {
        layout_image(box, parent_content_width);
    } else if (box->kind == ND_BOX_TABLE) {
        layout_table(box, parent_content_width, inherited_style);
    } else {
        box->content_width = parent_content_width;
        box->content_height = 0;
    }
}

static gboolean
flex_main_basis_explicit(const nd_box *c, double cw, double *out)
{
    const nd_style *s = c->style;
    if (!s) return FALSE;
    const nd_css_value *b = s->values[ND_CSS_FLEX_BASIS];
    if (b && b->kind == ND_CSS_V_LENGTH) {
        *out = length_resolve(b, cw, 0);
        return TRUE;
    }
    const nd_css_value *w = s->values[ND_CSS_WIDTH];
    if (w && (w->kind == ND_CSS_V_LENGTH || w->kind == ND_CSS_V_CALC)) {
        *out = length_resolve(w, cw, 0);
        return TRUE;
    }
    return FALSE;
}

static double
estimate_natural_width(const nd_box *b, double cap)
{
    double font_size = 16;
    if (b->style && b->style->values[ND_CSS_FONT_SIZE]) {
        const nd_css_value *fs = b->style->values[ND_CSS_FONT_SIZE];
        if (fs->kind == ND_CSS_V_LENGTH && fs->u.length.unit == ND_CSS_UNIT_PX)
            font_size = fs->u.length.v;
    }
    double w = 0;
    if (b->kind == ND_BOX_INLINE && b->text) {
        double chars = (double)g_utf8_strlen(b->text, -1);
        w = chars * font_size * 0.65 + font_size * 0.5;
    } else {
        for (const nd_box *c = b->first_child; c; c = c->next_sibling)
            w += estimate_natural_width(c, cap);
    }
    w += b->padding.left + b->padding.right +
         b->border.left  + b->border.right;
    if (w > cap) w = cap;
    return w;
}

static double
flex_grow_of(const nd_box *c)
{
    if (!c->style) return 0;
    return number_or(c->style->values[ND_CSS_FLEX_GROW], 0);
}

static double
flex_gap_of(const nd_style *s)
{
    if (!s) return 0;
    double g = number_or(s->values[ND_CSS_COLUMN_GAP], -1);
    if (g >= 0) return g;
    return number_or(s->values[ND_CSS_GAP], 0);
}

static double
flex_gap_row_of(const nd_style *s)
{
    if (!s) return 0;
    double g = number_or(s->values[ND_CSS_ROW_GAP], -1);
    if (g >= 0) return g;
    return number_or(s->values[ND_CSS_GAP], 0);
}

static gboolean
flex_wraps(const nd_style *s)
{
    if (!s) return FALSE;
    const nd_css_value *w = s->values[ND_CSS_FLEX_WRAP];
    if (!w || w->kind != ND_CSS_V_KEYWORD || !w->u.keyword) return FALSE;
    return strcmp(w->u.keyword, "wrap") == 0 ||
           strcmp(w->u.keyword, "wrap-reverse") == 0;
}

static double
flex_basis_main_height(const nd_box *c, double cw, gboolean *out_explicit)
{
    *out_explicit = FALSE;
    const nd_style *s = c->style;
    if (!s) return 0;
    const nd_css_value *b = s->values[ND_CSS_FLEX_BASIS];
    if (b && b->kind == ND_CSS_V_LENGTH) {
        *out_explicit = TRUE;
        return length_resolve(b, cw, 0);
    }
    const nd_css_value *h = s->values[ND_CSS_HEIGHT];
    if (h && (h->kind == ND_CSS_V_LENGTH || h->kind == ND_CSS_V_CALC)) {
        *out_explicit = TRUE;
        return length_resolve(h, cw, 0);
    }
    return 0;
}

static void
layout_flex_row(nd_box *box, double cw,
                double inner_x, double inner_y,
                const nd_style *child_inherited,
                gboolean reverse,
                double parent_content_width,
                double *cursor_y_out)
{
    (void)parent_content_width;

    GPtrArray *items = g_ptr_array_new();
    for (nd_box *c = box->first_child; c; c = c->next_sibling)
        g_ptr_array_add(items, c);

    double gap = flex_gap_of(box->style);

    double total_extras = 0;
    double total_explicit = 0;
    double total_grow = 0;
    int    implicit_count = 0;
    GArray *basis = g_array_new(FALSE, FALSE, sizeof(double));
    GArray *explicit_flags = g_array_new(FALSE, FALSE, sizeof(gboolean));
    for (guint i = 0; i < items->len; i++) {
        nd_box *c = items->pdata[i];
        edges_from_style(c->style, cw,
                         &c->margin, &c->padding, &c->border);
        double extras = c->margin.left + c->margin.right +
                        c->padding.left + c->padding.right +
                        c->border.left + c->border.right;
        total_extras += extras;
        total_grow   += flex_grow_of(c);
        double b = 0;
        gboolean exp_flag = flex_main_basis_explicit(c, cw, &b);
        if (!exp_flag) b = estimate_natural_width(c, cw);
        g_array_append_val(basis, b);
        g_array_append_val(explicit_flags, exp_flag);
        if (exp_flag) total_explicit += b;
        else          implicit_count++;
    }

    if (items->len > 1) total_extras += gap * (items->len - 1);

    double total_basis = total_explicit;
    for (guint i = 0; i < items->len; i++) {
        gboolean exp_flag = g_array_index(explicit_flags, gboolean, i);
        if (!exp_flag) total_basis += g_array_index(basis, double, i);
    }
    double remaining_free = cw - total_extras - total_basis;
    if (remaining_free < 0) remaining_free = 0;
    double extra_per_grow = (total_grow > 0) ? (remaining_free / total_grow) : 0;
    (void)implicit_count;

    const char *justify = keyword_or(box->style, ND_CSS_JUSTIFY_CONTENT, "flex-start");
    double leading = 0;
    double between = 0;
    if (total_grow == 0 && remaining_free > 0) {
        if      (strcmp(justify, "flex-end") == 0 ||
                 strcmp(justify, "end") == 0)            leading = remaining_free;
        else if (strcmp(justify, "center") == 0)         leading = remaining_free / 2.0;
        else if (strcmp(justify, "space-between") == 0)  between = items->len > 1
                                                            ? remaining_free / (items->len - 1) : 0;
        else if (strcmp(justify, "space-around") == 0) {
            between = items->len > 0 ? remaining_free / items->len : 0;
            leading = between / 2.0;
        } else if (strcmp(justify, "space-evenly") == 0) {
            between = items->len > 0 ? remaining_free / (items->len + 1) : 0;
            leading = between;
        }
    }

    GArray *assigned_main = g_array_new(FALSE, FALSE, sizeof(double));
    GArray *measured_h    = g_array_new(FALSE, FALSE, sizeof(double));
    double max_cross = 0;
    for (guint i = 0; i < items->len; i++) {
        nd_box *c = items->pdata[i];
        double a = g_array_index(basis, double, i)
                 + extra_per_grow * flex_grow_of(c);
        if (a < 0) a = 0;
        g_array_append_val(assigned_main, a);
        c->x = inner_x;
        c->y = inner_y;
        layout_box(c, a + c->margin.left + c->margin.right, child_inherited);
        double item_h = c->content_height +
                        c->padding.top + c->padding.bottom +
                        c->border.top + c->border.bottom +
                        c->margin.top + c->margin.bottom;
        g_array_append_val(measured_h, item_h);
        if (item_h > max_cross) max_cross = item_h;
    }

    double cursor_x = inner_x + leading;
    const char *align = keyword_or(box->style, ND_CSS_ALIGN_ITEMS, "stretch");

    for (guint k = 0; k < items->len; k++) {
        guint i = reverse ? (items->len - 1 - k) : k;
        nd_box *c = items->pdata[i];
        double item_h_full = g_array_index(measured_h, double, i);
        double item_h = item_h_full - c->margin.top - c->margin.bottom;
        double cy = inner_y + c->margin.top;
        if (strcmp(align, "center") == 0)
            cy = inner_y + (max_cross - item_h_full) / 2.0 + c->margin.top;
        else if (strcmp(align, "flex-end") == 0 || strcmp(align, "end") == 0)
            cy = inner_y + max_cross - item_h - c->margin.bottom;
        c->x = cursor_x + c->margin.left;
        c->y = cy;
        double a = g_array_index(assigned_main, double, i);
        layout_box(c, a + c->margin.left + c->margin.right, child_inherited);
        cursor_x += a + c->margin.left + c->margin.right + gap + between;
    }
    g_array_free(measured_h, TRUE);

    *cursor_y_out = inner_y + max_cross;
    g_array_free(basis, TRUE);
    g_array_free(explicit_flags, TRUE);
    g_array_free(assigned_main, TRUE);
    g_ptr_array_free(items, TRUE);
}

static void
layout_flex_row_wrap(nd_box *box, double cw,
                     double inner_x, double inner_y,
                     const nd_style *child_inherited,
                     gboolean reverse,
                     double *cursor_y_out)
{
    GPtrArray *items = g_ptr_array_new();
    for (nd_box *c = box->first_child; c; c = c->next_sibling)
        g_ptr_array_add(items, c);
    double gap = flex_gap_of(box->style);
    double row_gap = flex_gap_row_of(box->style);
    const char *align = keyword_or(box->style, ND_CSS_ALIGN_ITEMS, "stretch");
    const char *justify = keyword_or(box->style, ND_CSS_JUSTIFY_CONTENT, "flex-start");

    double line_y = inner_y;
    guint i = 0;
    while (i < items->len) {
        guint line_start = i;
        double used = 0;
        double line_max_h = 0;
        guint line_count = 0;
        for (; i < items->len; i++) {
            nd_box *c = items->pdata[i];
            edges_from_style(c->style, cw,
                             &c->margin, &c->padding, &c->border);
            double extras = c->margin.left + c->margin.right +
                            c->padding.left + c->padding.right +
                            c->border.left + c->border.right;
            double basis = 0;
            gboolean exp = flex_main_basis_explicit(c, cw, &basis);
            if (!exp) basis = estimate_natural_width(c, cw);
            double item_outer = basis + extras;
            double try_used = used + (line_count > 0 ? gap : 0) + item_outer;
            if (try_used > cw && line_count > 0) break;
            used = try_used;
            line_count++;
            c->x = inner_x;
            c->y = line_y;
            layout_box(c, basis + c->margin.left + c->margin.right, child_inherited);
            double item_h = c->content_height +
                            c->padding.top + c->padding.bottom +
                            c->border.top + c->border.bottom +
                            c->margin.top + c->margin.bottom;
            if (item_h > line_max_h) line_max_h = item_h;
        }
        double remaining = cw - used;
        if (remaining < 0) remaining = 0;
        double leading = 0;
        double between = 0;
        if (line_count > 0) {
            if (strcmp(justify, "flex-end") == 0 || strcmp(justify, "end") == 0)
                leading = remaining;
            else if (strcmp(justify, "center") == 0)
                leading = remaining / 2.0;
            else if (strcmp(justify, "space-between") == 0)
                between = line_count > 1 ? remaining / (line_count - 1) : 0;
            else if (strcmp(justify, "space-around") == 0) {
                between = remaining / line_count;
                leading = between / 2.0;
            } else if (strcmp(justify, "space-evenly") == 0) {
                between = remaining / (line_count + 1);
                leading = between;
            }
        }
        double cursor_x = inner_x + leading;
        for (guint k = 0; k < line_count; k++) {
            guint idx = reverse ? (line_start + line_count - 1 - k) : (line_start + k);
            nd_box *c = items->pdata[idx];
            double item_h_full = c->content_height +
                                 c->padding.top + c->padding.bottom +
                                 c->border.top + c->border.bottom +
                                 c->margin.top + c->margin.bottom;
            double cy = line_y + c->margin.top;
            if (strcmp(align, "center") == 0)
                cy = line_y + (line_max_h - item_h_full) / 2.0 + c->margin.top;
            else if (strcmp(align, "flex-end") == 0 || strcmp(align, "end") == 0)
                cy = line_y + line_max_h - item_h_full + c->margin.top;
            c->x = cursor_x + c->margin.left;
            c->y = cy;
            double outer = c->content_width
                + c->padding.left + c->padding.right
                + c->border.left + c->border.right;
            layout_box(c, outer + c->margin.left + c->margin.right,
                       child_inherited);
            cursor_x += outer + c->margin.left + c->margin.right + gap + between;
        }
        line_y += line_max_h + row_gap;
    }
    *cursor_y_out = line_y - (items->len > 0 ? row_gap : 0);
    g_ptr_array_free(items, TRUE);
}

static void
layout_flex_column(nd_box *box, double cw,
                   double inner_x, double inner_y,
                   const nd_style *child_inherited,
                   gboolean reverse,
                   double parent_content_height,
                   double *cursor_y_out)
{
    (void)parent_content_height;
    GPtrArray *items = g_ptr_array_new();
    for (nd_box *c = box->first_child; c; c = c->next_sibling)
        g_ptr_array_add(items, c);

    double row_gap = flex_gap_row_of(box->style);
    const char *align = keyword_or(box->style, ND_CSS_ALIGN_ITEMS, "stretch");

    const nd_css_value *hv = box->style ? box->style->values[ND_CSS_HEIGHT] : NULL;
    double explicit_h = -1;
    if (hv && (hv->kind == ND_CSS_V_LENGTH || hv->kind == ND_CSS_V_CALC))
        explicit_h = length_resolve(hv, cw, -1);

    GArray *basis = g_array_new(FALSE, FALSE, sizeof(double));
    GArray *explicit_flags = g_array_new(FALSE, FALSE, sizeof(gboolean));
    double total_grow = 0;
    double total_basis = 0;
    double total_margins = 0;
    for (guint i = 0; i < items->len; i++) {
        nd_box *c = items->pdata[i];
        edges_from_style(c->style, cw,
                         &c->margin, &c->padding, &c->border);
        double w_for_layout = cw - c->margin.left - c->margin.right;
        if (w_for_layout < 0) w_for_layout = 0;
        c->x = inner_x;
        c->y = inner_y;
        layout_box(c, w_for_layout, child_inherited);
        gboolean exp = FALSE;
        double b = flex_basis_main_height(c, cw, &exp);
        if (!exp) {
            b = c->content_height +
                c->padding.top + c->padding.bottom +
                c->border.top + c->border.bottom;
        }
        g_array_append_val(basis, b);
        g_array_append_val(explicit_flags, exp);
        total_basis += b;
        total_margins += c->margin.top + c->margin.bottom;
        total_grow += flex_grow_of(c);
    }
    double gaps = items->len > 1 ? row_gap * (items->len - 1) : 0;

    double extra_per_grow = 0;
    if (explicit_h > 0 && total_grow > 0) {
        double avail = explicit_h - total_basis - total_margins - gaps;
        if (avail > 0) extra_per_grow = avail / total_grow;
    }
    double remaining_free = 0;
    if (explicit_h > 0)
        remaining_free = explicit_h - total_basis - total_margins - gaps;
    if (remaining_free < 0) remaining_free = 0;

    const char *justify = keyword_or(box->style, ND_CSS_JUSTIFY_CONTENT, "flex-start");
    double leading = 0;
    double between = 0;
    if (total_grow == 0 && remaining_free > 0) {
        if (strcmp(justify, "flex-end") == 0 || strcmp(justify, "end") == 0)
            leading = remaining_free;
        else if (strcmp(justify, "center") == 0)
            leading = remaining_free / 2.0;
        else if (strcmp(justify, "space-between") == 0)
            between = items->len > 1 ? remaining_free / (items->len - 1) : 0;
        else if (strcmp(justify, "space-around") == 0) {
            between = items->len > 0 ? remaining_free / items->len : 0;
            leading = between / 2.0;
        } else if (strcmp(justify, "space-evenly") == 0) {
            between = items->len > 0 ? remaining_free / (items->len + 1) : 0;
            leading = between;
        }
    }

    double cursor_y = inner_y + leading;
    for (guint k = 0; k < items->len; k++) {
        guint i = reverse ? (items->len - 1 - k) : k;
        nd_box *c = items->pdata[i];
        double main_size = g_array_index(basis, double, i)
                         + extra_per_grow * flex_grow_of(c);
        if (main_size < 0) main_size = 0;

        double item_outer_w = c->content_width
            + c->padding.left + c->padding.right
            + c->border.left + c->border.right
            + c->margin.left + c->margin.right;
        double cx = inner_x + c->margin.left;
        if (strcmp(align, "center") == 0)
            cx = inner_x + (cw - item_outer_w) / 2.0 + c->margin.left;
        else if (strcmp(align, "flex-end") == 0 || strcmp(align, "end") == 0)
            cx = inner_x + cw - item_outer_w + c->margin.left;
        else if (strcmp(align, "stretch") == 0) {
            double stretched = cw - c->margin.left - c->margin.right;
            if (stretched > 0) {
                c->x = inner_x + c->margin.left;
                c->y = cursor_y + c->margin.top;
                layout_box(c, stretched, child_inherited);
            }
            cx = inner_x + c->margin.left;
        }
        c->x = cx;
        c->y = cursor_y + c->margin.top;

        if (extra_per_grow > 0 && flex_grow_of(c) > 0) {
            double target_h = main_size -
                              c->padding.top - c->padding.bottom -
                              c->border.top - c->border.bottom;
            if (target_h > c->content_height) c->content_height = target_h;
        }

        cursor_y += main_size + c->margin.top + c->margin.bottom + row_gap + between;
    }
    if (items->len > 0) cursor_y -= row_gap;

    *cursor_y_out = cursor_y;
    g_array_free(basis, TRUE);
    g_array_free(explicit_flags, TRUE);
    g_ptr_array_free(items, TRUE);
}

static void
resolve_track_sizes(const nd_css_tracks *tr, double available_main,
                    double *out_sizes)
{
    double total_fixed = 0;
    double total_fr    = 0;
    int    n_auto      = 0;
    for (int i = 0; i < tr->n; i++) {
        const nd_css_track *t = &tr->tracks[i];
        switch (t->kind) {
        case ND_CSS_TRACK_PX:      total_fixed += t->v; break;
        case ND_CSS_TRACK_PERCENT: total_fixed += t->v * available_main / 100.0; break;
        case ND_CSS_TRACK_FR:      total_fr += t->v > 0 ? t->v : 0; break;
        case ND_CSS_TRACK_AUTO:    n_auto++; break;
        }
    }
    double remaining = available_main - total_fixed;
    if (remaining < 0) remaining = 0;
    double per_fr   = total_fr > 0 ? remaining / total_fr : 0;
    double per_auto = 0;
    if (total_fr == 0 && n_auto > 0) per_auto = remaining / n_auto;

    for (int i = 0; i < tr->n; i++) {
        const nd_css_track *t = &tr->tracks[i];
        switch (t->kind) {
        case ND_CSS_TRACK_PX:      out_sizes[i] = t->v; break;
        case ND_CSS_TRACK_PERCENT: out_sizes[i] = t->v * available_main / 100.0; break;
        case ND_CSS_TRACK_FR:      out_sizes[i] = per_fr * (t->v > 0 ? t->v : 0); break;
        case ND_CSS_TRACK_AUTO:    out_sizes[i] = per_auto; break;
        }
        if (out_sizes[i] < 0) out_sizes[i] = 0;
    }
}

static int
grid_pos_span(const nd_css_value *v, int *out_start, int *out_span)
{
    *out_start = 0;
    *out_span  = 1;
    if (!v || v->kind != ND_CSS_V_KEYWORD || !v->u.keyword) return 0;
    const char *s = v->u.keyword;
    if (g_str_has_prefix(s, "span ")) {
        *out_span = nd_parse_int(s + 5, 1, 1, ND_CSS_TRACKS_MAX);
        return 0;
    }
    const char *slash = strchr(s, '/');
    if (slash) {
        char *a = g_strndup(s, slash - s);
        const char *b = slash + 1;
        while (*b == ' ') b++;
        int n = nd_parse_int(a, 0, 0, ND_CSS_TRACKS_MAX);
        *out_start = n > 0 ? n - 1 : 0;
        g_free(a);
        if (g_str_has_prefix(b, "span ")) {
            *out_span = nd_parse_int(b + 5, 1, 1, ND_CSS_TRACKS_MAX);
        } else {
            int e = nd_parse_int(b, 0, 0, ND_CSS_TRACKS_MAX);
            if (e > *out_start + 1) *out_span = e - 1 - *out_start;
        }
        return 1;
    }
    int n = nd_parse_int(s, 0, 0, ND_CSS_TRACKS_MAX);
    if (n > 0) { *out_start = n - 1; return 1; }
    return 0;
}

static void
layout_grid(nd_box *box, double cw,
            double inner_x, double inner_y,
            const nd_style *child_inherited,
            double *cursor_y_out)
{
    const nd_css_value *cols_v = box->style ? box->style->values[ND_CSS_GRID_TEMPLATE_COLUMNS] : NULL;
    const nd_css_value *rows_v = box->style ? box->style->values[ND_CSS_GRID_TEMPLATE_ROWS]    : NULL;
    nd_css_tracks default_cols = { .n = 1, .tracks = { { ND_CSS_TRACK_FR, 1 } } };
    const nd_css_tracks *cols = (cols_v && cols_v->kind == ND_CSS_V_TRACKS) ?
                                &cols_v->u.tracks : &default_cols;
    int n_cols = cols->n > 0 ? cols->n : 1;

    double col_gap = number_or(box->style ? box->style->values[ND_CSS_COLUMN_GAP] : NULL, -1);
    if (col_gap < 0) col_gap = number_or(box->style ? box->style->values[ND_CSS_GAP] : NULL, 0);
    double row_gap = number_or(box->style ? box->style->values[ND_CSS_ROW_GAP] : NULL, -1);
    if (row_gap < 0) row_gap = number_or(box->style ? box->style->values[ND_CSS_GAP] : NULL, 0);

    double col_sizes[ND_CSS_TRACKS_MAX];
    double avail = cw - (n_cols > 1 ? col_gap * (n_cols - 1) : 0);
    if (avail < 0) avail = 0;
    resolve_track_sizes(cols, avail, col_sizes);

    double col_x[ND_CSS_TRACKS_MAX + 1];
    col_x[0] = inner_x;
    for (int i = 0; i < n_cols; i++)
        col_x[i + 1] = col_x[i] + col_sizes[i] + col_gap;

    GPtrArray *items = g_ptr_array_new();
    GArray *starts = g_array_new(FALSE, FALSE, sizeof(int));
    GArray *spans  = g_array_new(FALSE, FALSE, sizeof(int));
    for (nd_box *c = box->first_child; c; c = c->next_sibling) {
        int s = -1, sp = 1;
        if (c->style) {
            int got = grid_pos_span(c->style->values[ND_CSS_GRID_COLUMN], &s, &sp);
            if (!got) s = -1;
        }
        g_ptr_array_add(items, c);
        g_array_append_val(starts, s);
        g_array_append_val(spans, sp);
    }

    int cursor_col = 0;
    double cursor_y = inner_y;
    guint i = 0;
    int row_idx = 0;
    while (i < items->len) {
        double row_height = 0;
        int row_filled[ND_CSS_TRACKS_MAX] = {0};
        int row_count = 0;
        while (i < items->len) {
            nd_box *c = items->pdata[i];
            int s = g_array_index(starts, int, i);
            int sp = g_array_index(spans, int, i);
            if (sp < 1) sp = 1;
            if (sp > n_cols) sp = n_cols;

            int chosen = s;
            if (chosen < 0 || chosen + sp > n_cols ||
                row_filled[chosen]) {
                chosen = cursor_col;
                while (chosen + sp <= n_cols) {
                    gboolean ok = TRUE;
                    for (int k = 0; k < sp; k++)
                        if (row_filled[chosen + k]) { ok = FALSE; break; }
                    if (ok) break;
                    chosen++;
                }
                if (chosen + sp > n_cols) break;
            }
            for (int k = 0; k < sp; k++) row_filled[chosen + k] = 1;
            cursor_col = chosen + sp;

            double w = 0;
            for (int k = 0; k < sp; k++)
                w += col_sizes[chosen + k] + (k > 0 ? col_gap : 0);
            edges_from_style(c->style, w, &c->margin, &c->padding, &c->border);
            double cw_for_item = w - c->margin.left - c->margin.right;
            if (cw_for_item < 0) cw_for_item = 0;
            c->x = col_x[chosen] + c->margin.left;
            c->y = cursor_y + c->margin.top;
            layout_box(c, cw_for_item, child_inherited);
            double item_outer = c->content_height +
                                c->padding.top + c->padding.bottom +
                                c->border.top + c->border.bottom +
                                c->margin.top + c->margin.bottom;
            if (item_outer > row_height) row_height = item_outer;
            row_count++;
            i++;
            if (cursor_col >= n_cols) break;
        }
        if (rows_v && rows_v->kind == ND_CSS_V_TRACKS &&
            row_idx < rows_v->u.tracks.n) {
            const nd_css_track *t = &rows_v->u.tracks.tracks[row_idx];
            double fixed = 0;
            if (t->kind == ND_CSS_TRACK_PX) fixed = t->v;
            else if (t->kind == ND_CSS_TRACK_PERCENT) fixed = t->v * cw / 100.0;
            if (fixed > row_height) row_height = fixed;
        }
        cursor_y += row_height + row_gap;
        cursor_col = 0;
        row_idx++;
        (void)row_count;
    }
    if (items->len > 0) cursor_y -= row_gap;

    *cursor_y_out = cursor_y;
    g_ptr_array_free(items, TRUE);
    g_array_free(starts, TRUE);
    g_array_free(spans, TRUE);
}

static void
layout_block(nd_box *box, double parent_content_width, const nd_style *inherited_style)
{
    edges_from_style(box->style, parent_content_width,
                     &box->margin, &box->padding, &box->border);

    const nd_css_value *wv  = box->style ? box->style->values[ND_CSS_WIDTH]     : NULL;
    const nd_css_value *mxw = box->style ? box->style->values[ND_CSS_MAX_WIDTH] : NULL;
    const nd_css_value *mnw = box->style ? box->style->values[ND_CSS_MIN_WIDTH] : NULL;
    double horiz_extras = box->padding.left + box->padding.right +
                          box->border.left + box->border.right;
    double horiz_total  = horiz_extras + box->margin.left + box->margin.right;
    double cw;
    gboolean explicit_width = FALSE;
    if (wv && wv->kind == ND_CSS_V_LENGTH) {
        cw = length_resolve(wv, parent_content_width, 0);
        explicit_width = TRUE;
    } else if (wv && wv->kind == ND_CSS_V_CALC) {
        cw = length_resolve(wv, parent_content_width, 0);
        explicit_width = TRUE;
    } else {
        cw = parent_content_width - horiz_total;
        if (cw < 0) cw = 0;
    }
    gboolean border_box = FALSE;
    if (box->style && box->style->values[ND_CSS_BOX_SIZING] &&
        box->style->values[ND_CSS_BOX_SIZING]->kind == ND_CSS_V_KEYWORD &&
        strcmp(box->style->values[ND_CSS_BOX_SIZING]->u.keyword, "border-box") == 0)
        border_box = TRUE;
    if (border_box && explicit_width) {
        cw -= horiz_extras;
        if (cw < 0) cw = 0;
    }
    double max_cw = length_resolve(mxw, parent_content_width, -1);
    if (max_cw >= 0) {
        if (border_box) max_cw -= horiz_extras;
        if (max_cw >= 0 && cw > max_cw) { cw = max_cw; explicit_width = TRUE; }
    }
    double min_cw = length_resolve(mnw, parent_content_width, -1);
    if (min_cw >= 0) {
        if (border_box) min_cw -= horiz_extras;
        if (min_cw >= 0 && cw < min_cw) { cw = min_cw; explicit_width = TRUE; }
    }
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

    if (style_is_flex_container(box->style)) {
        const char *dir = keyword_or(box->style, ND_CSS_FLEX_DIRECTION, "row");
        gboolean is_row = strcmp(dir, "row") == 0 || strcmp(dir, "row-reverse") == 0;
        gboolean is_col = strcmp(dir, "column") == 0 || strcmp(dir, "column-reverse") == 0;
        if (is_row) {
            if (flex_wraps(box->style))
                layout_flex_row_wrap(box, cw, inner_x, inner_y, child_inherited,
                                     strcmp(dir, "row-reverse") == 0, &cursor_y);
            else
                layout_flex_row(box, cw, inner_x, inner_y, child_inherited,
                                strcmp(dir, "row-reverse") == 0,
                                parent_content_width, &cursor_y);
            goto flex_done;
        }
        if (is_col) {
            layout_flex_column(box, cw, inner_x, inner_y, child_inherited,
                               strcmp(dir, "column-reverse") == 0,
                               parent_content_width, &cursor_y);
            goto flex_done;
        }
    }

    if (style_is_grid_container(box->style)) {
        layout_grid(box, cw, inner_x, inner_y, child_inherited, &cursor_y);
        goto flex_done;
    }

    GArray *floats = g_array_new(FALSE, FALSE, sizeof(float_ref));

    for (nd_box *c = box->first_child; c; c = c->next_sibling) {
        c->x = inner_x;
        int fside = float_side_of(c->style);
        int clr = clear_kind_of(c->style);
        if (fside >= 0 && (c->kind == ND_BOX_BLOCK || c->kind == ND_BOX_TABLE ||
                           c->kind == ND_BOX_IMAGE || c->kind == ND_BOX_VIDEO)) {
            edges_from_style(c->style, cw,
                             &c->margin, &c->padding, &c->border);
            double float_max_w = cw;
            double cw_for_float;
            const nd_css_value *wv2 = c->style ? c->style->values[ND_CSS_WIDTH] : NULL;
            if (wv2 && (wv2->kind == ND_CSS_V_LENGTH || wv2->kind == ND_CSS_V_CALC)) {
                cw_for_float = length_resolve(wv2, cw, 0);
            } else {
                cw_for_float = measure_natural_width(c, child_inherited);
                if (cw_for_float > float_max_w * 0.6) cw_for_float = float_max_w * 0.6;
                if (cw_for_float < 60) cw_for_float = 60;
            }
            double avail = cw_for_float
                + c->padding.left + c->padding.right
                + c->border.left + c->border.right
                + c->margin.left + c->margin.right;
            double float_y = cursor_y;
            if (clr) {
                double y_after_clear = floats_clear_y(floats, float_y, clr);
                if (y_after_clear > float_y) float_y = y_after_clear;
            }
            double left_off = 0, right_off = 0;
            floats_offsets_at(floats, float_y, &left_off, &right_off);
            while ((avail > cw - left_off - right_off) && floats->len > 0) {
                double next_y = float_y;
                gboolean advanced = FALSE;
                for (guint i = 0; i < floats->len; i++) {
                    const float_ref *f = &g_array_index(floats, float_ref, i);
                    if (f->bottom > float_y &&
                        (!advanced || f->bottom < next_y)) {
                        next_y = f->bottom;
                        advanced = TRUE;
                    }
                }
                if (!advanced) break;
                float_y = next_y;
                floats_offsets_at(floats, float_y, &left_off, &right_off);
            }
            double cw_capped = cw - left_off - right_off
                - c->margin.left - c->margin.right
                - c->padding.left - c->padding.right
                - c->border.left - c->border.right;
            if (cw_for_float > cw_capped && cw_capped > 0) cw_for_float = cw_capped;
            if (fside == 0)
                c->x = inner_x + left_off;
            else
                c->x = inner_x + cw - right_off
                       - cw_for_float
                       - c->padding.left - c->padding.right
                       - c->border.left - c->border.right
                       - c->margin.right;
            c->y = float_y + c->margin.top;
            double saved_cw = c->content_width;
            c->content_width = cw_for_float;
            layout_box(c, cw_for_float
                       + c->padding.left + c->padding.right
                       + c->border.left + c->border.right
                       + c->margin.left + c->margin.right,
                       child_inherited);
            (void)saved_cw;
            float_ref fr = {
                .box = c, .side = fside,
                .top = c->y - c->margin.top,
                .bottom = c->y + c->content_height
                    + c->padding.top + c->padding.bottom
                    + c->border.top + c->border.bottom
                    + c->margin.bottom,
                .outer_w = cw_for_float
                    + c->padding.left + c->padding.right
                    + c->border.left + c->border.right
                    + c->margin.left + c->margin.right,
            };
            g_array_append_val(floats, fr);
            continue;
        }
        if (c->kind == ND_BOX_BLOCK || c->kind == ND_BOX_TABLE) {
            edges_from_style(c->style, cw,
                             &c->margin, &c->padding, &c->border);
            double mt = c->margin.top;
            double gap = mt > prev_margin_bottom ? mt : prev_margin_bottom;
            cursor_y += gap;
            if (clr) {
                double y_after_clear = floats_clear_y(floats, cursor_y, clr);
                if (y_after_clear > cursor_y) cursor_y = y_after_clear;
            }
            double left_off = 0, right_off = 0;
            floats_offsets_at(floats, cursor_y, &left_off, &right_off);
            double cw_avail = cw - left_off - right_off;
            if (cw_avail < 0) cw_avail = 0;
            c->x = inner_x + left_off;
            c->y = cursor_y - mt;
            layout_box(c, cw_avail, child_inherited);
            cursor_y += c->content_height +
                        c->padding.top + c->padding.bottom +
                        c->border.top + c->border.bottom;
            prev_margin_bottom = c->margin.bottom;
        } else {
            cursor_y += prev_margin_bottom;
            prev_margin_bottom = 0;
            double left_off = 0, right_off = 0;
            floats_offsets_at(floats, cursor_y, &left_off, &right_off);
            double cw_avail = cw - left_off - right_off;
            if (cw_avail < 0) cw_avail = 0;
            c->x = inner_x + left_off;
            c->y = cursor_y;
            layout_box(c, cw_avail, child_inherited);
            cursor_y += c->content_height;
        }
        if ((c->kind == ND_BOX_IMAGE || c->kind == ND_BOX_VIDEO) &&
            c->content_width < cw) {
            const nd_css_value *ta = child_inherited
                ? child_inherited->values[ND_CSS_TEXT_ALIGN] : NULL;
            if (keyword_is(ta, "center"))
                c->x = inner_x + (cw - c->content_width) / 2.0;
            else if (keyword_is(ta, "right") || keyword_is(ta, "end"))
                c->x = inner_x + (cw - c->content_width);
        }
    }
    cursor_y += prev_margin_bottom;

    {
        double fb = floats_max_bottom(floats);
        if (fb > cursor_y) cursor_y = fb;
    }
    g_array_free(floats, TRUE);

flex_done: ;
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
                const nd_node *focused_input, gsize focused_caret_byte,
                struct nd_image_cache *image_cache, const char *base_url)
{
    g_focused_input_for_layout = focused_input;
    g_focused_caret_byte_for_layout = focused_caret_byte;
    g_image_cache_for_layout = image_cache;
    g_base_url_for_layout = base_url;
    nd_box *root = nd_layout_build_(doc, styles, viewport_width);
    g_focused_input_for_layout = NULL;
    g_focused_caret_byte_for_layout = 0;
    g_image_cache_for_layout = NULL;
    g_base_url_for_layout = NULL;
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
translate_subtree(nd_box *box, double dx, double dy)
{
    if (!box || (dx == 0 && dy == 0)) return;
    GQueue q = G_QUEUE_INIT;
    g_queue_push_tail(&q, box);
    while (!g_queue_is_empty(&q)) {
        nd_box *b = g_queue_pop_head(&q);
        b->x += dx;
        b->y += dy;
        for (nd_box *c = b->first_child; c; c = c->next_sibling)
            g_queue_push_tail(&q, c);
    }
    g_queue_clear(&q);
}

static void
apply_position_offsets(nd_box *box, double parent_w, double parent_h)
{
    if (!box) return;
    double child_w = box->content_width;
    double child_h = box->content_height;
    if (style_is_relative(box->style)) {
        const nd_css_value *lv = box->style->values[ND_CSS_LEFT];
        const nd_css_value *rv = box->style->values[ND_CSS_RIGHT];
        const nd_css_value *tv = box->style->values[ND_CSS_TOP];
        const nd_css_value *bv = box->style->values[ND_CSS_BOTTOM];
        gboolean l_auto = !lv || length_is_auto(lv);
        gboolean t_auto = !tv || length_is_auto(tv);
        double dx = 0, dy = 0;
        if (!l_auto)
            dx = length_or_zero(lv, parent_w);
        else if (rv && !length_is_auto(rv))
            dx = -length_or_zero(rv, parent_w);
        if (!t_auto)
            dy = length_or_zero(tv, parent_h);
        else if (bv && !length_is_auto(bv))
            dy = -length_or_zero(bv, parent_h);
        translate_subtree(box, dx, dy);
    }
    for (nd_box *c = box->first_child; c; c = c->next_sibling)
        apply_position_offsets(c, child_w, child_h);
}

static nd_box *
find_box_by_dom(nd_box *root, const nd_node *dom)
{
    if (!root || !dom) return NULL;
    if (root->dom == dom) return root;
    for (nd_box *c = root->first_child; c; c = c->next_sibling) {
        nd_box *m = find_box_by_dom(c, dom);
        if (m) return m;
    }
    return NULL;
}

static gboolean
style_creates_abs_cb(const nd_style *s)
{
    if (!s) return FALSE;
    const nd_css_value *v = s->values[ND_CSS_POSITION];
    if (!v || v->kind != ND_CSS_V_KEYWORD || !v->u.keyword) return FALSE;
    const char *kw = v->u.keyword;
    return strcmp(kw, "relative") == 0 || strcmp(kw, "absolute") == 0 ||
           strcmp(kw, "fixed") == 0    || strcmp(kw, "sticky") == 0;
}

static const nd_node *
find_abs_containing_block_dom(const nd_node *n, GHashTable *styles)
{
    for (const nd_node *p = n ? n->parent : NULL; p; p = p->parent) {
        if (p->kind != ND_NODE_ELEMENT) continue;
        const nd_style *ps = g_hash_table_lookup(styles, p);
        if (style_creates_abs_cb(ps)) return p;
    }
    return NULL;
}

static void
position_absolute_box(nd_box *abox, nd_box *cb)
{
    if (!abox || !cb) return;
    const nd_style *s = abox->style;
    double cb_w = cb->content_width;
    double cb_h = cb->content_height;
    double cb_inner_x = cb->x + cb->margin.left + cb->border.left + cb->padding.left;
    double cb_inner_y = cb->y + cb->margin.top  + cb->border.top  + cb->padding.top;

    const nd_css_value *lv = s ? s->values[ND_CSS_LEFT]   : NULL;
    const nd_css_value *rv = s ? s->values[ND_CSS_RIGHT]  : NULL;
    const nd_css_value *tv = s ? s->values[ND_CSS_TOP]    : NULL;
    const nd_css_value *bv = s ? s->values[ND_CSS_BOTTOM] : NULL;

    gboolean l_auto = !lv || length_is_auto(lv);
    gboolean r_auto = !rv || length_is_auto(rv);
    gboolean t_auto = !tv || length_is_auto(tv);
    gboolean b_auto = !bv || length_is_auto(bv);

    double left   = l_auto ? 0 : length_resolve(lv, cb_w, 0);
    double right  = r_auto ? 0 : length_resolve(rv, cb_w, 0);
    double top    = t_auto ? 0 : length_resolve(tv, cb_h, 0);
    double bottom = b_auto ? 0 : length_resolve(bv, cb_h, 0);

    double box_outer_w = abox->content_width
                       + abox->padding.left + abox->padding.right
                       + abox->border.left  + abox->border.right
                       + abox->margin.left  + abox->margin.right;
    double box_outer_h = abox->content_height
                       + abox->padding.top + abox->padding.bottom
                       + abox->border.top  + abox->border.bottom
                       + abox->margin.top  + abox->margin.bottom;

    double final_x, final_y;
    if (!l_auto) {
        final_x = cb_inner_x + left;
    } else if (!r_auto) {
        final_x = cb_inner_x + cb_w - right - box_outer_w;
    } else {
        final_x = cb_inner_x;
    }
    if (!t_auto) {
        final_y = cb_inner_y + top;
    } else if (!b_auto) {
        final_y = cb_inner_y + cb_h - bottom - box_outer_h;
    } else {
        final_y = cb_inner_y;
    }

    double dx = final_x - abox->x;
    double dy = final_y - abox->y;
    if (dx == 0 && dy == 0) return;
    GQueue q = G_QUEUE_INIT;
    g_queue_push_tail(&q, abox);
    while (!g_queue_is_empty(&q)) {
        nd_box *b = g_queue_pop_head(&q);
        b->x += dx;
        b->y += dy;
        for (nd_box *c = b->first_child; c; c = c->next_sibling)
            g_queue_push_tail(&q, c);
    }
    g_queue_clear(&q);
}

static void
process_absolute_boxes(nd_box *root, GHashTable *styles, double viewport_width)
{
    if (!g_abs_pending || g_abs_pending->len == 0) return;
    for (guint i = 0; i < g_abs_pending->len; i++) {
        nd_abs_entry e = g_array_index(g_abs_pending, nd_abs_entry, i);
        const nd_node *cb_dom = e.fixed
            ? NULL
            : find_abs_containing_block_dom(e.dom, styles);
        nd_box *cb = cb_dom ? find_box_by_dom(root, cb_dom) : root;
        if (!cb) cb = root;

        g_abs_force_build = TRUE;
        nd_box *abox = build_block(e.dom, styles);
        g_abs_force_build = FALSE;
        if (!abox) continue;

        box_append_child(cb, abox);
        double avail = cb->content_width > 0 ? cb->content_width : viewport_width;
        const nd_style *cs = cb->style;
        abox->x = cb->x + cb->margin.left + cb->border.left + cb->padding.left;
        abox->y = cb->y + cb->margin.top  + cb->border.top  + cb->padding.top;
        const nd_css_value *awv = abox->style
            ? abox->style->values[ND_CSS_WIDTH] : NULL;
        gboolean has_explicit_width = awv &&
            (awv->kind == ND_CSS_V_LENGTH || awv->kind == ND_CSS_V_CALC);
        layout_box(abox, avail, cs);
        if (!has_explicit_width && abox->kind == ND_BOX_BLOCK) {
            double fit = estimate_natural_width(abox, avail);
            if (fit > 0 && fit < avail) layout_box(abox, fit, cs);
        }
        apply_position_offsets(abox, avail, cb->content_height);
        position_absolute_box(abox, cb);
    }
    g_array_set_size(g_abs_pending, 0);
}

static nd_box *
nd_layout_build_(const nd_node *doc, GHashTable *styles, double viewport_width)
{
    g_abs_pending = g_array_new(FALSE, FALSE, sizeof(nd_abs_entry));
    nd_box *root = build_block(doc, styles);
    if (!root) {
        g_array_free(g_abs_pending, TRUE);
        g_abs_pending = NULL;
        return NULL;
    }
    root->x = 0;
    root->y = 0;

    layout_block(root, viewport_width, NULL);
    apply_position_offsets(root, viewport_width, root->content_height);
    process_absolute_boxes(root, styles, viewport_width);

    g_array_free(g_abs_pending, TRUE);
    g_abs_pending = NULL;
    return root;
}

const char *
nd_box_kind_name(nd_box_kind k)
{
    switch (k) {
    case ND_BOX_BLOCK:      return "block";
    case ND_BOX_INLINE:     return "inline";
    case ND_BOX_TEXT:       return "text";
    case ND_BOX_IMAGE:      return "image";
    case ND_BOX_TABLE:      return "table";
    case ND_BOX_TABLE_ROW:  return "row";
    case ND_BOX_TABLE_CELL: return "cell";
    case ND_BOX_VIDEO:      return "video";
    }
    return "?";
}

static void
collect_images_walk(const nd_box *b, GPtrArray *out)
{
    if (!b) return;
    if (b->kind == ND_BOX_IMAGE) g_ptr_array_add(out, (gpointer)b);
    if (b->media && b->media->bg_image_src) g_ptr_array_add(out, (gpointer)b);
    for (const nd_box *c = b->first_child; c; c = c->next_sibling)
        collect_images_walk(c, out);
}

static void
collect_videos_walk(const nd_box *b, GPtrArray *out)
{
    if (!b) return;
    if (b->kind == ND_BOX_VIDEO) g_ptr_array_add(out, (gpointer)b);
    for (const nd_box *c = b->first_child; c; c = c->next_sibling)
        collect_videos_walk(c, out);
}

void
nd_layout_collect_videos(const nd_box *root, GPtrArray *out_boxes)
{
    collect_videos_walk(root, out_boxes);
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
        nd_box_kind_name(b->kind), tag,
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
    }
    g_string_append_c(out, '\n');
    for (const nd_box *c = b->first_child; c; c = c->next_sibling)
        dump_box(out, c, depth + 1);
}

static void
extent_walk(const nd_box *b, double *out_w, double *out_h)
{
    if (!b) return;
    double right = b->x + b->margin.left + b->border.left + b->padding.left +
                   b->content_width + b->padding.right + b->border.right +
                   b->margin.right;
    double bottom = b->y + b->margin.top + b->border.top + b->padding.top +
                    b->content_height + b->padding.bottom + b->border.bottom +
                    b->margin.bottom;
    if (right  > *out_w) *out_w = right;
    if (bottom > *out_h) *out_h = bottom;
    for (const nd_box *c = b->first_child; c; c = c->next_sibling)
        extent_walk(c, out_w, out_h);
}

void
nd_box_content_extent(const nd_box *root, double *out_w, double *out_h)
{
    double w = 0, h = 0;
    extent_walk(root, &w, &h);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
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

const nd_node *
nd_box_hit_form_dom(const nd_box *root, double x, double y)
{
    return nd_form_hit_walk(root, x, y, NULL);
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
    if (root->kind == ND_BOX_INLINE && root->links &&
        root->links->len > 0) {
        double box_x0 = root->x;
        double box_y0 = root->y;
        double box_y1 = box_y0 + root->content_height;
        if (x >= box_x0 && x <= box_x0 + root->content_width &&
            y >= box_y0 && y <= box_y1) {
            gsize byte = 0;
            if (nd_paint_inline_xy_to_byte(root, x - box_x0, y - box_y0, &byte)) {
                for (guint i = 0; i < root->links->len; i++) {
                    const nd_link_range *r = &g_array_index(root->links, nd_link_range, i);
                    if (byte >= r->start && byte < r->start + r->len)
                        return r;
                }
            }
            return NULL;
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
