/* Nordstjernen — shared style/layout pipeline used by GUI and headless.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "render.h"

#include <math.h>

#include "font.h"
#include "net.h"

static void
render_feed_animations(const nd_render_ctx *c, GHashTable *styles)
{
    if (!c->anim) return;
    for (guint i = 0; i < c->n_sheets; i++)
        if (c->sheets[i]) nd_anim_load_from_stylesheet(c->anim, c->sheets[i]);
    gint64 now_us = g_get_monotonic_time();
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, styles);
    while (g_hash_table_iter_next(&it, &key, &val))
        nd_anim_observe(c->anim, (const nd_node *)key,
                        (const nd_style *)val, now_us);
    nd_anim_prune(c->anim, styles);
}

static void
render_request_fonts(const nd_render_ctx *c)
{
    if (!nd_font_available()) return;
    for (guint i = 0; i < c->n_sheets; i++) {
        const nd_css_stylesheet *sh = c->sheets[i];
        if (!sh || !sh->font_faces) continue;
        for (guint j = 0; j < sh->font_faces->len; j++) {
            const nd_css_font_face *ff =
                &g_array_index(sh->font_faces, nd_css_font_face, j);
            if (!ff->family || !ff->src_url) continue;
            char *abs = c->resolve_url
                ? c->resolve_url(ff->src_url, c->cb_ud)
                : nd_url_resolve(c->base_url, ff->src_url);
            if (!abs) continue;
            if (c->font_allowed && !c->font_allowed(abs, c->cb_ud)) {
                g_free(abs);
                continue;
            }
            nd_font_request(ff->family, abs, c->base_url);
            g_free(abs);
        }
    }
}

static void
render_apply_zoom(const nd_render_ctx *c, GHashTable *styles)
{
    double zoom = c->zoom > 0 ? c->zoom : 1.0;
    if (fabs(zoom - 1.0) <= 0.001) return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, styles);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        nd_style *st = val;
        if (!st || !st->values[ND_CSS_FONT_SIZE]) continue;
        if (st->values[ND_CSS_FONT_SIZE]->kind != ND_CSS_V_LENGTH) continue;
        st->values[ND_CSS_FONT_SIZE]->u.length.v *= zoom;
    }
}

static void
render_collect_containers(const nd_box *b, GHashTable *map)
{
    if (!b) return;
    if (b->dom && b->style) {
        const nd_css_value *ct = b->style->values[ND_CSS_CONTAINER_TYPE];
        if (ct && ct->kind == ND_CSS_V_KEYWORD && ct->u.keyword &&
            g_ascii_strcasecmp(ct->u.keyword, "normal") != 0) {
            const nd_css_value *nm = b->style->values[ND_CSS_CONTAINER_NAME];
            const char *names = (nm && nm->kind == ND_CSS_V_KEYWORD)
                ? nm->u.keyword : NULL;
            nd_css_container_map_add(map, b->dom, ct->u.keyword, names,
                                     b->content_width, b->content_height);
        }
    }
    for (const nd_box *ch = b->first_child; ch; ch = ch->next_sibling)
        render_collect_containers(ch, map);
}

static void
render_style_pass(const nd_render_ctx *c, GHashTable *styles)
{
    render_feed_animations(c, styles);
    render_request_fonts(c);
    render_apply_zoom(c, styles);
}

static const nd_node *
render_find_viewport_meta(const nd_node *n)
{
    if (!n) return NULL;
    if (nd_node_is_element_named(n, "meta")) {
        const char *name = nd_element_get_attr(n, "name");
        if (name && g_ascii_strcasecmp(name, "viewport") == 0)
            return n;
    }
    for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
        const nd_node *found = render_find_viewport_meta(c);
        if (found) return found;
    }
    return NULL;
}

static double
render_parse_viewport_width(const char *content)
{
    if (!content || !*content) return 0;
    double out = 0;
    char **parts = g_strsplit_set(content, ",;", -1);
    for (int i = 0; parts && parts[i]; i++) {
        char *part = g_strstrip(parts[i]);
        char *eq = strchr(part, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = g_strstrip(part);
        char *value = g_strstrip(eq + 1);
        if (g_ascii_strcasecmp(key, "width") != 0) continue;
        if (g_ascii_strcasecmp(value, "device-width") == 0) break;
        char *end = NULL;
        double n = g_ascii_strtod(value, &end);
        if (end != value && n >= 320 && n <= 4096) {
            out = n;
            break;
        }
    }
    g_strfreev(parts);
    return out;
}

static double
render_effective_viewport_width(const nd_render_ctx *c)
{
    double width = c->viewport_width;
    const nd_node *meta = render_find_viewport_meta(c->doc);
    const char *content = meta ? nd_element_get_attr(meta, "content") : NULL;
    double hint = render_parse_viewport_width(content);
    if (hint > width) width = hint;
    return width;
}

GHashTable *
nd_render_relayout(const nd_render_ctx *c, nd_box **out_layout)
{
    if (out_layout) *out_layout = NULL;
    if (!c || !out_layout) return NULL;

    double viewport_width = render_effective_viewport_width(c);
    nd_css_set_viewport(viewport_width, c->viewport_height);
    nd_css_set_focus_node(c->focused_input);
    GHashTable *styles = nd_css_compute(c->doc, c->sheets, c->n_sheets);

    render_style_pass(c, styles);

    nd_box *layout = nd_layout_build(c->doc, styles, viewport_width,
                                     c->focused_input, c->caret_byte,
                                     c->sel_anchor_byte,
                                     c->images, c->base_url);

    gboolean want_cq = FALSE;
    for (guint i = 0; i < c->n_sheets && !want_cq; i++)
        want_cq = nd_css_stylesheet_has_container_rules(c->sheets[i]);

    GHashTable *containers = nd_css_container_map_new();
    if (want_cq) render_collect_containers(layout, containers);
    if (g_hash_table_size(containers) > 0) {
        nd_css_set_container_map(containers);
        GHashTable *styles2 = nd_css_compute(c->doc, c->sheets, c->n_sheets);
        nd_css_set_container_map(NULL);
        render_style_pass(c, styles2);
        nd_box *layout2 = nd_layout_build(c->doc, styles2, viewport_width,
                                          c->focused_input, c->caret_byte,
                                          c->sel_anchor_byte,
                                          c->images, c->base_url);
        nd_box_free(layout);
        g_hash_table_destroy(styles);
        layout = layout2;
        styles = styles2;
    }
    g_hash_table_destroy(containers);
    nd_css_set_focus_node(NULL);

    if (c->js) {
        nd_js_set_style_table(c->js, styles);
        nd_js_set_layout_root(c->js, layout);
    }
    *out_layout = layout;
    return styles;
}
