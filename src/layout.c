/* Nordstjernen — block layout. */

#include "layout.h"

#include <math.h>
#include <string.h>

static double
length_or(const nd_css_value *v, double fallback)
{
    if (!v) return fallback;
    if (v->kind == ND_CSS_V_LENGTH) return v->u.length.v;
    return fallback;
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
    return is_keyword(s->values[ND_CSS_DISPLAY], "block");
}

static gboolean
style_is_none(const nd_style *s)
{
    return s && s->values[ND_CSS_DISPLAY] && is_keyword(s->values[ND_CSS_DISPLAY], "none");
}

static void
edges_from_style(const nd_style *s, nd_edges *margin, nd_edges *padding, nd_edges *border)
{
    if (!s) {
        memset(margin, 0, sizeof(*margin));
        memset(padding, 0, sizeof(*padding));
        memset(border, 0, sizeof(*border));
        return;
    }
    margin->top    = length_or(s->values[ND_CSS_MARGIN_TOP],    0);
    margin->right  = length_or(s->values[ND_CSS_MARGIN_RIGHT],  0);
    margin->bottom = length_or(s->values[ND_CSS_MARGIN_BOTTOM], 0);
    margin->left   = length_or(s->values[ND_CSS_MARGIN_LEFT],   0);
    padding->top    = length_or(s->values[ND_CSS_PADDING_TOP],    0);
    padding->right  = length_or(s->values[ND_CSS_PADDING_RIGHT],  0);
    padding->bottom = length_or(s->values[ND_CSS_PADDING_BOTTOM], 0);
    padding->left   = length_or(s->values[ND_CSS_PADDING_LEFT],   0);
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
    const nd_style *s = g_hash_table_lookup(styles, n);
    if (!s) return FALSE;
    if (style_is_none(s)) return FALSE;
    return !style_is_block(s);
}

typedef struct collector_ctx {
    GHashTable *styles;
    const char *active_href;
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
            };
            g_array_append_val(ctx->links, r);
        }
        return;
    }
    if (n->kind != ND_NODE_ELEMENT) return;
    const nd_style *s = g_hash_table_lookup(ctx->styles, n);
    if (s && style_is_none(s)) return;

    const char *prev_href = ctx->active_href;
    if (strcmp(n->name, "a") == 0) {
        const char *h = nd_element_get_attr(n, "href");
        if (h && *h) ctx->active_href = h;
    }
    gboolean bold   = tag_is_bold(n->name);
    gboolean italic = tag_is_italic(n->name);
    gboolean mono   = tag_is_monospace(n->name);
    gboolean uline  = strcmp(n->name, "u") == 0;
    gboolean strike = strcmp(n->name, "s") == 0 ||
                      strcmp(n->name, "del") == 0 ||
                      strcmp(n->name, "strike") == 0;
    if (bold && ctx->bold_depth++ == 0) ctx->bold_start = ctx->out->len;
    if (italic && ctx->italic_depth++ == 0) ctx->italic_start = ctx->out->len;
    if (mono && ctx->mono_depth++ == 0) ctx->mono_start = ctx->out->len;
    if (uline && ctx->underline_depth++ == 0) ctx->underline_start = ctx->out->len;
    if (strike && ctx->strike_depth++ == 0) ctx->strike_start = ctx->out->len;

    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        collect_walk(c, ctx);

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
    ctx->active_href = prev_href;
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
        gboolean ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f');
        if (preformatted) {
            map[i] = collapsed->len;
            g_string_append_c(collapsed, c);
            continue;
        }
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
        nd_inline_attr out = { .kind = a->kind, .start = ns, .len = ne - ns };
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

    if (n->name && strcmp(n->name, "img") == 0)
        return build_image_box(n);

    if (!style_is_block(s)) return NULL;

    nd_box *block = box_new(ND_BOX_BLOCK);
    block->dom = n;
    block->style = s;

    const nd_node *c = n->first_child;
    while (c) {
        if (is_inline_dom(c, styles)) {
            const nd_node *start = c;
            while (c && is_inline_dom(c, styles)) c = c->next_sibling;
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

static double
inline_avg_char_px(const nd_style *parent_style)
{
    double font_size = length_or(parent_style ? parent_style->values[ND_CSS_FONT_SIZE] : NULL, 16);
    return font_size * 0.55;
}

static double
inline_line_height(const nd_style *parent_style)
{
    double font_size = length_or(parent_style ? parent_style->values[ND_CSS_FONT_SIZE] : NULL, 16);
    return font_size * 1.2;
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
    double cw = inline_avg_char_px(parent_style);
    double lh = inline_line_height(parent_style);
    if (cw <= 0) cw = 8;
    if (content_width < cw) content_width = cw;

    int chars_per_line = (int)floor(content_width / cw);
    if (chars_per_line < 1) chars_per_line = 1;

    const char *p = box->text;
    gsize len = strlen(box->text);
    gsize i = 0;
    double y = 0;
    while (i < len) {
        gsize remaining = len - i;
        gsize take = (gsize)chars_per_line < remaining ? (gsize)chars_per_line : remaining;

        if (take < remaining) {
            gsize back = take;
            while (back > 0 && p[i + back] != ' ' && p[i + back - 1] != ' ')
                back--;
            if (back > 0) take = back;
        }
        nd_line ln = {
            .y = y, .height = lh,
            .char_count = (int)take,
            .text = g_strndup(p + i, take),
        };
        g_array_append_val(box->lines, ln);
        i += take;

        while (i < len && p[i] == ' ') i++;
        y += lh;
    }
    box->content_width  = content_width;
    box->content_height = y;
}

static void
layout_block(nd_box *box, double parent_content_width, const nd_style *inherited_style);

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
layout_box(nd_box *box, double parent_content_width, const nd_style *inherited_style)
{
    if (box->kind == ND_BOX_BLOCK) {
        layout_block(box, parent_content_width, inherited_style);
    } else if (box->kind == ND_BOX_INLINE) {
        inline_layout(box, parent_content_width, inherited_style);
    } else if (box->kind == ND_BOX_IMAGE) {
        layout_image(box, parent_content_width);
    } else {
        box->content_width = parent_content_width;
        box->content_height = 0;
    }
}

static void
layout_block(nd_box *box, double parent_content_width, const nd_style *inherited_style)
{
    edges_from_style(box->style, &box->margin, &box->padding, &box->border);

    const nd_css_value *wv = box->style ? box->style->values[ND_CSS_WIDTH] : NULL;
    double horiz_extras = box->margin.left + box->margin.right +
                          box->padding.left + box->padding.right +
                          box->border.left + box->border.right;
    double cw;
    if (wv && wv->kind == ND_CSS_V_LENGTH) {
        cw = wv->u.length.v;
    } else {
        cw = parent_content_width - horiz_extras;
        if (cw < 0) cw = 0;
    }
    box->content_width = cw;

    double inner_x = box->x + box->margin.left + box->border.left + box->padding.left;
    double inner_y = box->y + box->margin.top  + box->border.top  + box->padding.top;
    double cursor_y = inner_y;
    const nd_style *child_inherited = box->style ? box->style : inherited_style;

    for (nd_box *c = box->first_child; c; c = c->next_sibling) {
        c->x = inner_x;
        c->y = cursor_y;
        layout_box(c, cw, child_inherited);
        double child_outer_h = c->content_height +
                               c->margin.top  + c->margin.bottom +
                               c->padding.top + c->padding.bottom +
                               c->border.top  + c->border.bottom;
        if (c->kind != ND_BOX_BLOCK) {

            child_outer_h = c->content_height;
        }
        cursor_y += child_outer_h;
    }

    const nd_css_value *hv = box->style ? box->style->values[ND_CSS_HEIGHT] : NULL;
    if (hv && hv->kind == ND_CSS_V_LENGTH)
        box->content_height = hv->u.length.v;
    else
        box->content_height = cursor_y - inner_y;
}

nd_box *
nd_layout_build(const nd_node *doc, GHashTable *styles, double viewport_width)
{
    nd_box *root = build_block(doc, styles);
    if (!root) return NULL;
    root->x = 0;
    root->y = 0;

    layout_block(root, viewport_width, NULL);
    return root;
}

static const char *
box_kind_str(nd_box_kind k)
{
    switch (k) {
    case ND_BOX_BLOCK:  return "block";
    case ND_BOX_INLINE: return "inline";
    case ND_BOX_TEXT:   return "text";
    case ND_BOX_IMAGE:  return "image";
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

const char *
nd_box_hit_link(const nd_box *root, double x, double y)
{
    if (!root) return NULL;
    if (root->kind == ND_BOX_INLINE && root->lines && root->links &&
        root->links->len > 0) {
        double box_x0 = root->x;
        double box_y0 = root->y;
        double box_y1 = box_y0 + root->content_height;
        if (x >= box_x0 && x <= box_x0 + root->content_width &&
            y >= box_y0 && y <= box_y1) {
            return g_array_index(root->links, nd_link_range, 0).href;
        }
    }
    for (const nd_box *c = root->first_child; c; c = c->next_sibling) {
        const char *h = nd_box_hit_link(c, x, y);
        if (h) return h;
    }
    return NULL;
}
