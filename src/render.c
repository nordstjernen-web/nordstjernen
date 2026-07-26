/* Nordstjernen — shared style/layout pipeline used by GUI and headless.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "render.h"

#include <math.h>
#include <string.h>

#include "font.h"
#include "net.h"

static gboolean g_render_page_uses_hover = FALSE;

gboolean
ns_render_page_uses_hover(void)
{
    return g_render_page_uses_hover;
}

static gboolean g_render_page_uses_active = FALSE;

gboolean
ns_render_page_uses_active(void)
{
    return g_render_page_uses_active;
}

typedef struct render_font_usage {
    GArray *codepoints;
    GHashTable *seen;
} render_font_usage;

static void
render_font_usage_free(gpointer data)
{
    render_font_usage *usage = data;
    if (!usage) return;
    g_array_free(usage->codepoints, TRUE);
    g_hash_table_destroy(usage->seen);
    g_free(usage);
}

static gboolean
render_font_list_contains(const char *list, const char *family)
{
    if (!list || !family || !*family) return FALSE;
    const char *p = list;
    while (*p) {
        while (g_ascii_isspace(*p) || *p == ',') p++;
        GString *token = g_string_new(NULL);
        char quote = 0;
        while (*p) {
            char ch = *p++;
            if (quote) {
                if (ch == '\\' && *p) ch = *p++;
                else if (ch == quote) {
                    quote = 0;
                    continue;
                }
            } else {
                if (ch == '\'' || ch == '"') {
                    quote = ch;
                    continue;
                }
                if (ch == ',') break;
                if (ch == '\\' && *p) ch = *p++;
            }
            g_string_append_c(token, ch);
        }
        g_strstrip(token->str);
        gboolean match = g_ascii_strcasecmp(token->str, family) == 0;
        g_string_free(token, TRUE);
        if (match) return TRUE;
    }
    return FALSE;
}

static void
render_font_usage_add_text(render_font_usage *usage, const char *text)
{
    const char *p = text;
    while (p && *p) {
        gunichar cp = g_utf8_get_char_validated(p, -1);
        if (cp == (gunichar)-1 || cp == (gunichar)-2) {
            p++;
            continue;
        }
        gpointer key = GUINT_TO_POINTER(cp + 1u);
        if (g_hash_table_add(usage->seen, key))
            g_array_append_val(usage->codepoints, cp);
        p = g_utf8_next_char(p);
    }
}

static void
render_collect_font_usage(const ns_node *node, GHashTable *styles,
                          GHashTable *families)
{
    if (!node) return;
    if (node->kind == NS_NODE_ELEMENT) {
        const ns_style *style = g_hash_table_lookup(styles, node);
        if (ns_display_is_none(ns_css_display_of(style)))
            return;
    }
    if (node->kind == NS_NODE_TEXT && node->text && *node->text &&
        node->parent) {
        const ns_style *style = g_hash_table_lookup(styles, node->parent);
        const ns_css_value *value = style
            ? style->values[NS_CSS_FONT_FAMILY] : NULL;
        const char *list = value && value->kind == NS_CSS_V_KEYWORD
            ? value->u.keyword : NULL;
        if (list) {
            GHashTableIter iter;
            gpointer key, val;
            g_hash_table_iter_init(&iter, families);
            while (g_hash_table_iter_next(&iter, &key, &val))
                if (render_font_list_contains(list, key))
                    render_font_usage_add_text(val, node->text);
        }
    }
    for (const ns_node *child = node->first_child; child;
         child = child->next_sibling)
        render_collect_font_usage(child, styles, families);
}

static gboolean
render_unicode_range_parse(const char *start, const char *end,
                           gunichar *out_start, gunichar *out_end)
{
    while (start < end && g_ascii_isspace(*start)) start++;
    while (end > start && g_ascii_isspace(end[-1])) end--;
    if (end - start < 3 || g_ascii_tolower(start[0]) != 'u' ||
        start[1] != '+')
        return FALSE;
    start += 2;
    guint32 lo = 0;
    guint32 hi = 0;
    guint digits = 0;
    guint wildcards = 0;
    while (start < end && digits < 6) {
        int hex = g_ascii_xdigit_value(*start);
        if (hex >= 0) {
            if (wildcards) return FALSE;
            lo = (lo << 4) | (guint32)hex;
            hi = (hi << 4) | (guint32)hex;
        } else if (*start == '?') {
            wildcards++;
            lo <<= 4;
            hi = (hi << 4) | 0xFu;
        } else {
            break;
        }
        digits++;
        start++;
    }
    if (!digits) return FALSE;
    if (wildcards) {
        if (start != end) return FALSE;
    } else if (start < end && *start == '-') {
        start++;
        guint32 range_end = 0;
        guint end_digits = 0;
        while (start < end && end_digits < 6) {
            int hex = g_ascii_xdigit_value(*start);
            if (hex < 0) break;
            range_end = (range_end << 4) | (guint32)hex;
            end_digits++;
            start++;
        }
        if (!end_digits || start != end) return FALSE;
        hi = range_end;
    } else if (start != end) {
        return FALSE;
    }
    if (lo > hi || lo > 0x10FFFFu) return FALSE;
    if (hi > 0x10FFFFu) hi = 0x10FFFFu;
    *out_start = (gunichar)lo;
    *out_end = (gunichar)hi;
    return TRUE;
}

static gboolean
render_unicode_range_matches(const char *range,
                             const render_font_usage *usage)
{
    if (!usage || usage->codepoints->len == 0) return FALSE;
    if (!range || !*range) return TRUE;
    gboolean valid = FALSE;
    const char *part = range;
    while (*part) {
        const char *end = strchr(part, ',');
        if (!end) end = part + strlen(part);
        gunichar lo = 0, hi = 0;
        if (render_unicode_range_parse(part, end, &lo, &hi)) {
            valid = TRUE;
            for (guint i = 0; i < usage->codepoints->len; i++) {
                gunichar cp = g_array_index(usage->codepoints, gunichar, i);
                if (cp >= lo && cp <= hi) return TRUE;
            }
        }
        part = *end ? end + 1 : end;
    }
    return !valid;
}

static void
render_feed_animations(const ns_render_ctx *c, GHashTable *styles)
{
    if (!c->anim) return;
    for (guint i = 0; i < c->n_sheets; i++)
        if (c->sheets[i]) ns_anim_load_from_stylesheet(c->anim, c->sheets[i]);
    gint64 now_us = g_get_monotonic_time();
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, styles);
    while (g_hash_table_iter_next(&it, &key, &val))
        ns_anim_observe(c->anim, (const ns_node *)key,
                        (const ns_style *)val, now_us);
    ns_anim_prune(c->anim, styles);
}

static void
render_request_fonts(const ns_render_ctx *c, GHashTable *styles)
{
    if (!ns_font_available()) return;
    GHashTable *families = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, render_font_usage_free);
    for (guint i = 0; i < c->n_sheets; i++) {
        const ns_css_stylesheet *sh = c->sheets[i];
        if (!sh || !sh->font_faces) continue;
        for (guint j = 0; j < sh->font_faces->len; j++) {
            const ns_css_font_face *ff =
                &g_array_index(sh->font_faces, ns_css_font_face, j);
            if (!ff->family || !*ff->family ||
                g_hash_table_contains(families, ff->family))
                continue;
            render_font_usage *usage = g_new0(render_font_usage, 1);
            usage->codepoints = g_array_new(FALSE, FALSE, sizeof(gunichar));
            usage->seen = g_hash_table_new(g_direct_hash, g_direct_equal);
            g_hash_table_insert(families, g_strdup(ff->family), usage);
        }
    }
    render_collect_font_usage(c->doc, styles, families);
    for (guint i = 0; i < c->n_sheets; i++) {
        const ns_css_stylesheet *sh = c->sheets[i];
        if (!sh || !sh->font_faces) continue;
        for (guint j = 0; j < sh->font_faces->len; j++) {
            const ns_css_font_face *ff =
                &g_array_index(sh->font_faces, ns_css_font_face, j);
            if (!ff->family || !ff->src_url) continue;
            render_font_usage *usage = g_hash_table_lookup(families,
                                                           ff->family);
            if (!render_unicode_range_matches(ff->unicode_range, usage))
                continue;
            char *abs = c->resolve_url
                ? c->resolve_url(ff->src_url, c->cb_ud)
                : ns_url_resolve(c->base_url, ff->src_url);
            if (!abs) continue;
            if (c->font_allowed && !c->font_allowed(abs, c->cb_ud)) {
                g_free(abs);
                continue;
            }
            ns_font_request(ff->family, abs, c->base_url);
            g_free(abs);
        }
    }
    g_hash_table_destroy(families);
}

static void
render_apply_zoom(const ns_render_ctx *c, GHashTable *styles)
{
    double zoom = c->zoom > 0 ? c->zoom : 1.0;
    if (fabs(zoom - 1.0) <= 0.001) return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, styles);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_style *st = val;
        if (!st || !st->values[NS_CSS_FONT_SIZE]) continue;
        if (st->values[NS_CSS_FONT_SIZE]->kind != NS_CSS_V_LENGTH) continue;
        st->values[NS_CSS_FONT_SIZE]->u.length.v *= zoom;
    }
}

static void
render_collect_containers(const ns_box *b, GHashTable *map)
{
    if (!b) return;
    if (b->dom && b->style) {
        const ns_css_value *ct = b->style->values[NS_CSS_CONTAINER_TYPE];
        if (ct && ct->kind == NS_CSS_V_KEYWORD && ct->u.keyword &&
            g_ascii_strcasecmp(ct->u.keyword, "normal") != 0) {
            const ns_css_value *nm = b->style->values[NS_CSS_CONTAINER_NAME];
            const char *names = (nm && nm->kind == NS_CSS_V_KEYWORD)
                ? nm->u.keyword : NULL;
            ns_css_container_map_add(map, b->dom, ct->u.keyword, names,
                                     b->content_width, b->content_height);
        }
    }
    for (const ns_box *ch = b->first_child; ch; ch = ch->next_sibling)
        render_collect_containers(ch, map);
}

static void
render_style_pass(const ns_render_ctx *c, GHashTable *styles)
{
    render_feed_animations(c, styles);
    render_request_fonts(c, styles);
    render_apply_zoom(c, styles);
}

static const ns_node *
render_find_viewport_meta(const ns_node *n, int depth)
{
    if (!n || depth >= 512) return NULL;
    if (ns_node_is_element_named(n, "meta")) {
        const char *name = ns_element_get_attr(n, "name");
        if (name && g_ascii_strcasecmp(name, "viewport") == 0)
            return n;
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        const ns_node *found = render_find_viewport_meta(c, depth + 1);
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
render_effective_viewport_width(const ns_render_ctx *c)
{
    double width = c->viewport_width;
    const ns_node *meta = render_find_viewport_meta(c->doc, 0);
    const char *content = meta ? ns_element_get_attr(meta, "content") : NULL;
    double hint = render_parse_viewport_width(content);
    if (hint > width) width = hint;
    return width;
}

GHashTable *
ns_render_relayout_profile(const ns_render_ctx *c, ns_box **out_layout,
                           ns_render_profile *profile)
{
    if (out_layout) *out_layout = NULL;
    if (!c || !out_layout) return NULL;
    if (profile) memset(profile, 0, sizeof *profile);

    double viewport_width = render_effective_viewport_width(c);
    ns_css_set_viewport(viewport_width, c->viewport_height);
    ns_css_set_focus_node(c->focused_input);
    ns_css_set_hover_node(c->hover_node);

    gboolean uses_hover = FALSE;
    for (guint i = 0; i < c->n_sheets && !uses_hover; i++)
        uses_hover = ns_css_stylesheet_has_hover_rules(c->sheets[i]);
    g_render_page_uses_hover = uses_hover;

    gboolean uses_active = FALSE;
    for (guint i = 0; i < c->n_sheets && !uses_active; i++)
        uses_active = ns_css_stylesheet_has_active_rules(c->sheets[i]);
    g_render_page_uses_active = uses_active;

    gint64 t0 = profile ? g_get_monotonic_time() : 0;
    ns_css_set_render_zoom(c->zoom > 0 ? c->zoom : 1.0);
    GHashTable *styles = ns_css_compute(c->doc, c->sheets, c->n_sheets);
    gint64 t1 = profile ? g_get_monotonic_time() : 0;

    render_style_pass(c, styles);
    gint64 t2 = profile ? g_get_monotonic_time() : 0;

    ns_box *layout = ns_layout_build(c->doc, styles, viewport_width,
                                     c->focused_input, c->caret_byte,
                                     c->sel_anchor_byte,
                                     c->images, c->base_url);
    gint64 t3 = profile ? g_get_monotonic_time() : 0;
    if (profile) {
        profile->css1_us = t1 - t0;
        profile->style1_us = t2 - t1;
        profile->layout1_us = t3 - t2;
    }

    GHashTable *containers = ns_css_container_map_new();
    gint64 tc0 = profile ? g_get_monotonic_time() : 0;
    render_collect_containers(layout, containers);
    gint64 tc1 = profile ? g_get_monotonic_time() : 0;
    guint n_containers = g_hash_table_size(containers);
    if (profile) {
        profile->container_us = tc1 - tc0;
        profile->containers = n_containers;
    }
    if (n_containers > 0) {
        if (profile) profile->container_pass = TRUE;
        ns_css_set_container_map(containers);
        gint64 t4 = profile ? g_get_monotonic_time() : 0;
        GHashTable *styles2 = ns_css_compute(c->doc, c->sheets, c->n_sheets);
        gint64 t5 = profile ? g_get_monotonic_time() : 0;
        ns_css_set_container_map(NULL);
        render_style_pass(c, styles2);
        gint64 t6 = profile ? g_get_monotonic_time() : 0;
        ns_box *layout2 = ns_layout_build(c->doc, styles2, viewport_width,
                                          c->focused_input, c->caret_byte,
                                          c->sel_anchor_byte,
                                          c->images, c->base_url);
        gint64 t7 = profile ? g_get_monotonic_time() : 0;
        if (profile) {
            profile->css2_us = t5 - t4;
            profile->style2_us = t6 - t5;
            profile->layout2_us = t7 - t6;
        }
        ns_box_free(layout);
        g_hash_table_destroy(styles);
        layout = layout2;
        styles = styles2;
    }
    g_hash_table_destroy(containers);
    ns_css_set_focus_node(NULL);
    ns_css_set_hover_node(NULL);

    if (c->js) {
        ns_js_set_style_table(c->js, styles);
        ns_js_set_layout_root(c->js, layout);
    }
    *out_layout = layout;
    return styles;
}

GHashTable *
ns_render_relayout(const ns_render_ctx *c, ns_box **out_layout)
{
    return ns_render_relayout_profile(c, out_layout, NULL);
}
