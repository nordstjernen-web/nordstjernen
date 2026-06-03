/* Nordstjernen — headless engine driver.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "headless.h"

#include <stdio.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#include "anim.h"
#include "cache.h"
#include "css.h"
#include "debuglog.h"
#include "dom.h"
#include "engine.h"
#include "html.h"
#include "image.h"
#include "js.h"
#include "layout.h"
#include "net.h"
#include "paint.h"
#include "video.h"

static gboolean
settle_quit_cb(gpointer user_data)
{
    GMainLoop *loop = user_data;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

unsigned
nd_headless_debug_mask(const char *spec)
{
    if (!spec || !*spec) return 0;
    if (g_ascii_strcasecmp(spec, "all") == 0)
        return 0xFFFFFFFFu;
    unsigned mask = 0;
    char **toks = g_strsplit(spec, ",", -1);
    for (int i = 0; toks[i]; i++) {
        char *t = g_strstrip(toks[i]);
        for (int lvl = ND_DLOG_INFO; lvl <= ND_DLOG_JS; lvl++)
            if (g_ascii_strcasecmp(t, nd_dlog_level_name(lvl)) == 0)
                mask |= (1u << lvl);
    }
    g_strfreev(toks);
    return mask;
}

static void
headless_dlog_listener(const nd_dlog_entry *e, gpointer user_data)
{
    unsigned mask = GPOINTER_TO_UINT(user_data);
    if (!e || !(mask & (1u << e->level))) return;
    fprintf(stderr, "[%s %s] %s\n", nd_dlog_level_name(e->level),
            e->category ? e->category : "", e->message ? e->message : "");
}

static void
fetch_videos_into_layout(nd_box *root, const char *base_url)
{
    if (!root || !base_url) return;
    GPtrArray *vids = g_ptr_array_new();
    nd_layout_collect_videos(root, vids);
    for (guint i = 0; i < vids->len; i++) {
        nd_box *box = g_ptr_array_index(vids, i);
        if (!box->media || box->media->video) continue;
        if (box->media->video_poster) {
            char *poster = nd_url_resolve(base_url, box->media->video_poster);
            if (poster) {
                nd_response *resp = nd_engine_fetch_blocking(poster, base_url, NULL);
                if (resp && !resp->error && resp->body && resp->body->len > 0) {
                    int w = 0, h = 0;
                    nd_texture *tex = nd_image_decode_bytes(resp->body->data,
                                                            resp->body->len,
                                                            &w, &h);
                    if (tex) {
                        nd_video *v = g_new0(nd_video, 1);
                        v->url = g_strdup(poster);
                        v->poster_texture = tex;
                        v->natural_width = w;
                        v->natural_height = h;
                        box->media->video = v;
                    }
                }
                if (resp) nd_response_free(resp);
                g_free(poster);
            }
        }
    }
    g_ptr_array_free(vids, TRUE);
}

static int
write_capture(const nd_box *root, const char *path, nd_headless_dump kind)
{
    if (kind == ND_DUMP_PDF) return nd_engine_write_pdf(root, path);
    return nd_engine_write_png(root, path);
}

static void
headless_js_log(const char *line, gpointer user_data)
{
    (void)user_data;
    fprintf(stderr, "[js] %s\n", line);
}

static void
headless_js_mutated(gpointer user_data) { (void)user_data; }

typedef struct headless_nav_capture {
    char *pending_url;
} headless_nav_capture;

static void
headless_js_navigate(const char *url, gboolean reload, gpointer user_data)
{
    (void)reload;
    headless_nav_capture *cap = user_data;
    if (cap && url && *url) {
        g_free(cap->pending_url);
        cap->pending_url = g_strdup(url);
    }
}

static void
headless_form_collect_select(const nd_node *select, GString *q,
                             gboolean *first, const char *name)
{
    if (!nd_element_get_attr(select, "multiple")) {
        const nd_node *opt = nd_select_chosen_option(select);
        char *text = nd_option_value_dup(opt);
        nd_form_urlencoded_append_pair(q, first, name, text ? text : "");
        g_free(text);
        return;
    }
    for (const nd_node *c = select->first_child; c; c = c->next_sibling) {
        if (nd_node_is_element_named(c, "optgroup")) {
            if (nd_element_effectively_disabled(c) ||
                nd_element_get_attr(c, "disabled"))
                continue;
            for (const nd_node *cc = c->first_child; cc; cc = cc->next_sibling) {
                if (nd_node_is_element_named(cc, "option") &&
                    nd_element_get_attr(cc, "selected") &&
                    !nd_element_effectively_disabled(cc)) {
                    char *text = nd_option_value_dup(cc);
                    nd_form_urlencoded_append_pair(q, first, name, text ? text : "");
                    g_free(text);
                }
            }
        } else if (nd_node_is_element_named(c, "option") &&
                   nd_element_get_attr(c, "selected") &&
                   !nd_element_effectively_disabled(c)) {
            char *text = nd_option_value_dup(c);
            nd_form_urlencoded_append_pair(q, first, name, text ? text : "");
            g_free(text);
        }
    }
}

static void
headless_form_collect(const nd_node *form, const nd_node *n, const nd_node *doc,
                      GString *q, gboolean *first)
{
    if (!n) return;
    if (n->kind == ND_NODE_ELEMENT && n->name) {
        if (strcmp(n->name, "input") == 0) {
            const char *name = nd_element_get_attr(n, "name");
            const char *type = nd_element_get_attr(n, "type");
            const char *value = nd_element_get_attr(n, "value");
            if (name && *name && nd_form_owner(n, doc) == form &&
                !nd_element_effectively_disabled(n)) {
                gboolean skip = FALSE;
                if (type && (g_ascii_strcasecmp(type, "submit") == 0 ||
                             g_ascii_strcasecmp(type, "image")  == 0 ||
                             g_ascii_strcasecmp(type, "button") == 0 ||
                             g_ascii_strcasecmp(type, "reset")  == 0 ||
                             g_ascii_strcasecmp(type, "file")   == 0))
                    skip = TRUE;
                if (type && (g_ascii_strcasecmp(type, "checkbox") == 0 ||
                             g_ascii_strcasecmp(type, "radio")    == 0)) {
                    if (!nd_element_get_attr(n, "checked")) skip = TRUE;
                    else if (!value) value = "on";
                }
                if (!skip)
                    nd_form_urlencoded_append_pair(q, first, name, value ? value : "");
            }
        } else if (strcmp(n->name, "textarea") == 0) {
            const char *name = nd_element_get_attr(n, "name");
            if (name && *name && nd_form_owner(n, doc) == form &&
                !nd_element_effectively_disabled(n)) {
                char *text = nd_node_collect_text(n);
                nd_form_urlencoded_append_pair(q, first, name, text ? text : "");
                g_free(text);
            }
        } else if (strcmp(n->name, "select") == 0) {
            const char *name = nd_element_get_attr(n, "name");
            if (name && *name && nd_form_owner(n, doc) == form &&
                !nd_element_effectively_disabled(n))
                headless_form_collect_select(n, q, first, name);
        }
    }
    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        headless_form_collect(form, c, doc, q, first);
}

static void
headless_js_form_submit(const nd_node *form, const nd_node *submitter,
                        gpointer user_data)
{
    (void)submitter;
    headless_nav_capture *cap = user_data;
    if (!cap || !form) return;
    const char *method = nd_element_get_attr(form, "method");
    if (method && g_ascii_strcasecmp(method, "post") == 0) return;
    const char *action = nd_element_get_attr(form, "action");
    if (!action) action = "";
    GString *q = g_string_new(NULL);
    gboolean first = TRUE;
    const nd_node *doc = nd_node_root(form);
    headless_form_collect(form, doc ? doc : form, doc ? doc : form, q, &first);
    char *url;
    if (q->len > 0) {
        const char *sep = strchr(action, '?') ? "&" : "?";
        url = g_strdup_printf("%s%s%s", action, sep, q->str);
    } else {
        url = g_strdup(action);
    }
    g_string_free(q, TRUE);
    g_free(cap->pending_url);
    cap->pending_url = url;
}

static int nd_headless_run_one(const nd_headless_opts *opts,
                               const char *fetch_url, int hop);

int
nd_headless_run(const nd_headless_opts *opts)
{
    if (!opts || !opts->url || !*opts->url) {
        fprintf(stderr, "headless: --url is required\n");
        return 2;
    }
#ifdef G_OS_WIN32
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    guint dlog_sub = 0;
    if (opts->debug_levels)
        dlog_sub = nd_debug_log_subscribe(headless_dlog_listener,
                                          GUINT_TO_POINTER(opts->debug_levels));
    int rc = nd_headless_run_one(opts, opts->url, 0);
    if (dlog_sub) nd_debug_log_unsubscribe(dlog_sub);
    return rc;
}

typedef struct headless_flush_ctx {
    nd_node           *doc;
    nd_js             *js;
    const char        *base;
    int                vw;
    double             vh;
    nd_image_cache    *image_cache;
    nd_anim           *anim;
    GHashTable        *css_cache;
    GHashTable       **styles;
    nd_box           **layout;
    const nd_node     *focused;
    gsize              caret;
    gsize              anchor;
} headless_flush_ctx;

static void
headless_relayout(headless_flush_ctx *c)
{
    if (!c) return;
    if (g_getenv("ND_ANIM_DEBUG")) g_printerr("[anim] headless_relayout\n");
    if (c->js && *c->layout) nd_js_set_layout_root(c->js, NULL);
    if (*c->layout) { nd_box_free(*c->layout); *c->layout = NULL; }
    if (c->js && *c->styles) nd_js_set_style_table(c->js, NULL);
    if (*c->styles) { g_hash_table_destroy(*c->styles); *c->styles = NULL; }

    *c->styles = nd_engine_relayout(c->doc, c->base, c->vw, c->vh,
                                    c->image_cache, c->anim, c->js,
                                    c->css_cache, c->focused, c->caret,
                                    c->anchor, c->layout);
}

static void
headless_flush_layout(gpointer ud)
{
    headless_flush_ctx *c = ud;
    if (!c || !c->js) return;
    gboolean mutated = nd_js_consume_mutated(c->js);
    gboolean dirty = !c->layout || !*c->layout || mutated;
    if (!dirty) return;
    headless_relayout(c);
}

typedef struct {
    headless_flush_ctx *fc;
    gint64              last_flush_us;
} settle_state;

static gboolean
settle_raf_tick(gpointer user_data)
{
    settle_state *s = user_data;
    headless_flush_ctx *fc = s->fc;
    gint64 now = g_get_monotonic_time();
    if (fc->image_cache) nd_image_cache_tick(fc->image_cache, now);
    if (fc->anim) nd_anim_tick(fc->anim, now);
    if (fc->js && nd_js_run_animation_frame(fc->js)) {
        if (nd_js_consume_mutated(fc->js) &&
            now - s->last_flush_us >= 2000000) {
            headless_relayout(fc);
            s->last_flush_us = g_get_monotonic_time();
        }
    }
    return G_SOURCE_CONTINUE;
}

static void
settle_main_loop(int ms, headless_flush_ctx *fc)
{
    if (ms <= 0 || !fc) return;
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add(ms, settle_quit_cb, loop);
    settle_state st = { .fc = fc, .last_flush_us = g_get_monotonic_time() };
    guint raf_id = g_timeout_add(16, settle_raf_tick, &st);
    g_main_loop_run(loop);
    g_source_remove(raf_id);
    g_main_loop_unref(loop);
}

static void
headless_edit_replace(headless_flush_ctx *fc, gsize lo, gsize hi, const char *ins)
{
    if (!fc->focused) return;
    nd_node *t = (nd_node *)fc->focused;
    const char *cur = nd_node_editable_value(t);
    gsize clen = strlen(cur);
    if (lo > clen) lo = clen;
    if (hi > clen) hi = clen;
    if (hi < lo) hi = lo;
    gsize ins_len = ins ? strlen(ins) : 0;
    char *numeric_filtered = NULL;
    if (ins_len && nd_node_is_numeric_input(t)) {
        gsize fl = 0;
        numeric_filtered = nd_numeric_filter_insert(ins, ins_len, &fl);
        ins = numeric_filtered;
        ins_len = fl;
        if (ins_len == 0) { g_free(numeric_filtered); return; }
    }
    if (ins_len && nd_form_control_length_limits_apply(t)) {
        const char *ml = nd_element_get_attr(t, "maxlength");
        if (ml && *ml) {
            long maxl = atol(ml);
            if (maxl >= 0) {
                glong kept = g_utf8_strlen(cur, (gssize)lo) +
                             g_utf8_strlen(cur + hi, (gssize)(clen - hi));
                glong room = maxl - kept;
                if (room < 0) room = 0;
                if (g_utf8_strlen(ins, (gssize)ins_len) > room) {
                    const char *p = ins;
                    for (glong i = 0; i < room; i++) p = g_utf8_next_char(p);
                    ins_len = (gsize)(p - ins);
                    if (ins_len == 0) { g_free(numeric_filtered); return; }
                }
            }
        }
    }
    GString *s = g_string_new(NULL);
    g_string_append_len(s, cur, (gssize)lo);
    if (ins_len) g_string_append_len(s, ins, (gssize)ins_len);
    g_string_append_len(s, cur + hi, (gssize)(clen - hi));
    g_free(numeric_filtered);
    if (fc->js) {
        gboolean prevented = FALSE;
        nd_js_dispatch_event(fc->js, t, "beforeinput", &prevented);
        if (prevented) { g_string_free(s, TRUE); return; }
    }
    nd_node_set_editable_value(t, s->str);
    fc->caret = lo + ins_len;
    fc->anchor = fc->caret;
    g_string_free(s, TRUE);
    if (fc->js) {
        nd_js_dispatch_event(fc->js, t, "input", NULL);
        nd_js_consume_mutated(fc->js);
    }
}

static void
headless_click(headless_flush_ctx *fc, headless_nav_capture *nav,
               double x, double y)
{
    nd_box *layout = *fc->layout;
    if (!layout) return;
    const nd_link_range *link = nd_box_hit_link_range(layout, x, y);
    const nd_node *form_target = nd_box_hit_form_dom(layout, x, y);
    const nd_box *hit = nd_box_hit_test(layout, x, y);
    const nd_node *dom = form_target ? form_target
                       : link ? link->dom
                       : hit ? hit->dom : NULL;
    if (!dom) { fc->focused = NULL; return; }
    if (!form_target) {
        for (const nd_node *lc = dom; lc; lc = lc->parent) {
            if (!nd_node_is_element_named(lc, "label")) continue;
            const nd_node *tgt = NULL;
            const char *for_id = nd_element_get_attr(lc, "for");
            if (for_id && *for_id && fc->doc)
                tgt = nd_node_find_by_id(fc->doc, for_id);
            if (!tgt) {
                GQueue q = G_QUEUE_INIT;
                for (const nd_node *d = lc->first_child; d; d = d->next_sibling)
                    g_queue_push_tail(&q, (gpointer)d);
                while (!g_queue_is_empty(&q) && !tgt) {
                    const nd_node *d = g_queue_pop_head(&q);
                    if (d->kind == ND_NODE_ELEMENT && d->name &&
                        (strcmp(d->name, "input") == 0 ||
                         strcmp(d->name, "select") == 0 ||
                         strcmp(d->name, "textarea") == 0 ||
                         strcmp(d->name, "button") == 0))
                        tgt = d;
                    else
                        for (const nd_node *e = d->first_child; e; e = e->next_sibling)
                            g_queue_push_tail(&q, (gpointer)e);
                }
                g_queue_clear(&q);
            }
            if (tgt && tgt != dom) dom = tgt;
            break;
        }
    }
    fprintf(stderr, "[headless] click hit <%s>\n",
            dom->name ? dom->name : "(text)");
    const nd_node *editable = NULL;
    for (const nd_node *cur = dom; cur; cur = cur->parent)
        if (nd_node_is_editable(cur)) { editable = cur; break; }
    gboolean prevented = FALSE;
    if (fc->js) {
        nd_js_dispatch_event(fc->js, dom, "click", &prevented);
        nd_js_consume_mutated(fc->js);
    }
    if (editable) {
        nd_node_flatten_editable((nd_node *)editable);
        if (fc->js && fc->focused && fc->focused != editable)
            nd_js_dispatch_event(fc->js, fc->focused, "blur", NULL);
        fc->focused = editable;
        const char *v = nd_node_editable_value(editable);
        fc->caret = v ? strlen(v) : 0;
        fc->anchor = fc->caret;
        if (fc->js) {
            nd_js_set_focused_node(fc->js, editable);
            nd_js_dispatch_event(fc->js, editable, "focus",   NULL);
            nd_js_dispatch_event(fc->js, editable, "focusin", NULL);
        }
        return;
    }
    if (prevented) return;
    if (hit && hit->dom && nd_node_is_element_named(hit->dom, "img") && nav) {
        const char *usemap = nd_element_get_attr(hit->dom, "usemap");
        if (usemap && *usemap && fc->doc) {
            double cx0 = hit->x + hit->margin.left +
                         hit->border.left + hit->padding.left;
            double cy0 = hit->y + hit->margin.top +
                         hit->border.top + hit->padding.top;
            char *ahref = nd_image_map_resolve(fc->doc, usemap, x - cx0, y - cy0,
                                               hit->content_width,
                                               hit->content_height, NULL);
            if (ahref) {
                g_free(nav->pending_url);
                nav->pending_url = ahref;
                return;
            }
        }
    }
    if (link && link->href && *link->href && nav) {
        g_free(nav->pending_url);
        nav->pending_url = g_strdup(link->href);
        return;
    }
    for (const nd_node *cur = dom; cur; cur = cur->parent) {
        if (cur->kind == ND_NODE_ELEMENT && cur->name &&
            strcmp(cur->name, "summary") == 0 && cur->parent &&
            nd_node_is_element_named(cur->parent, "details")) {
            nd_node *details = (nd_node *)cur->parent;
            gboolean now_open;
            if (nd_element_get_attr(details, "open")) {
                nd_element_remove_attr(details, "open"); now_open = FALSE;
            } else {
                nd_element_set_attr(details, "open", ""); now_open = TRUE;
            }
            if (fc->js) {
                nd_js_details_toggle_open(fc->js, details, now_open);
                nd_js_consume_mutated(fc->js);
            }
            return;
        }
        if (cur->kind == ND_NODE_ELEMENT && cur->name &&
            strcmp(cur->name, "input") == 0) {
            const char *type = nd_element_get_attr(cur, "type");
            if (type && g_ascii_strcasecmp(type, "checkbox") == 0) {
                if (nd_element_get_attr(cur, "checked"))
                    nd_element_remove_attr((nd_node *)cur, "checked");
                else
                    nd_element_set_attr((nd_node *)cur, "checked", "");
            } else if (type && g_ascii_strcasecmp(type, "radio") == 0) {
                nd_element_set_attr((nd_node *)cur, "checked", "");
            } else {
                continue;
            }
            if (fc->js) {
                nd_js_dispatch_event(fc->js, cur, "input",  NULL);
                nd_js_dispatch_event(fc->js, cur, "change", NULL);
                nd_js_consume_mutated(fc->js);
            }
            return;
        }
    }
    fc->focused = NULL;
}

static void
headless_key(headless_flush_ctx *fc, const char *name)
{
    if (!fc->focused || !name || !*name) return;
    nd_node *t = (nd_node *)fc->focused;
    const char *cur = nd_node_editable_value(t);
    gsize clen = strlen(cur);
    if (fc->caret > clen) fc->caret = clen;
    if (fc->anchor > clen) fc->anchor = clen;
    gsize lo = MIN(fc->caret, fc->anchor);
    gsize hi = MAX(fc->caret, fc->anchor);
    gboolean has_sel = lo != hi;
    gboolean multiline = (t->name && strcmp(t->name, "textarea") == 0) ||
                         nd_node_is_contenteditable_host(t);
    if (fc->js) {
        int key_code = 0;
        const char *jskey = name;
        struct { const char *n; int c; const char *k; } map[] = {
            {"Enter",13,"Enter"}, {"Return",13,"Enter"},
            {"Backspace",8,"Backspace"}, {"Delete",46,"Delete"},
            {"Tab",9,"Tab"}, {"Escape",27,"Escape"},
            {"Left",37,"ArrowLeft"}, {"Right",39,"ArrowRight"},
            {"Up",38,"ArrowUp"}, {"Down",40,"ArrowDown"},
            {"Home",36,"Home"}, {"End",35,"End"},
        };
        for (gsize i = 0; i < G_N_ELEMENTS(map); i++)
            if (g_ascii_strcasecmp(name, map[i].n) == 0) {
                key_code = map[i].c; jskey = map[i].k; break;
            }
        if (key_code) {
            nd_js_dispatch_key_event(fc->js, t, "keydown", jskey, jskey,
                                     key_code, FALSE, FALSE, FALSE, FALSE, NULL);
            nd_js_dispatch_key_event(fc->js, t, "keyup", jskey, jskey,
                                     key_code, FALSE, FALSE, FALSE, FALSE, NULL);
            nd_js_consume_mutated(fc->js);
        }
    }
    if (g_ascii_strcasecmp(name, "Enter") == 0 ||
        g_ascii_strcasecmp(name, "Return") == 0) {
        if (multiline) headless_edit_replace(fc, lo, hi, "\n");
        return;
    }
    if (g_ascii_strcasecmp(name, "Backspace") == 0) {
        if (has_sel) headless_edit_replace(fc, lo, hi, NULL);
        else if (fc->caret > 0) {
            const char *prev = g_utf8_prev_char(cur + fc->caret);
            headless_edit_replace(fc, (gsize)(prev - cur), fc->caret, NULL);
        }
        return;
    }
    if (g_ascii_strcasecmp(name, "Delete") == 0) {
        if (has_sel) headless_edit_replace(fc, lo, hi, NULL);
        else if (fc->caret < clen) {
            const char *nxt = g_utf8_next_char(cur + fc->caret);
            headless_edit_replace(fc, fc->caret, (gsize)(nxt - cur), NULL);
        }
        return;
    }
    if (g_ascii_strcasecmp(name, "Left") == 0) {
        if (fc->caret > 0) {
            const char *p = g_utf8_prev_char(cur + fc->caret);
            fc->caret = (gsize)(p - cur);
        }
        fc->anchor = fc->caret;
        return;
    }
    if (g_ascii_strcasecmp(name, "Right") == 0) {
        if (fc->caret < clen) {
            const char *p = g_utf8_next_char(cur + fc->caret);
            fc->caret = (gsize)(p - cur);
        }
        fc->anchor = fc->caret;
        return;
    }
    if (g_ascii_strcasecmp(name, "Home") == 0) { fc->caret = 0; fc->anchor = 0; return; }
    if (g_ascii_strcasecmp(name, "End") == 0)  { fc->caret = clen; fc->anchor = clen; return; }
    if (g_ascii_strcasecmp(name, "Up") == 0 || g_ascii_strcasecmp(name, "Down") == 0) {
        const char *itype = t->name && strcmp(t->name, "input") == 0
            ? nd_element_get_attr(t, "type") : NULL;
        if (itype && g_ascii_strcasecmp(itype, "number") == 0) {
            const char *sv = nd_element_get_attr(t, "step");
            double step = sv && *sv ? g_ascii_strtod(sv, NULL) : 1.0;
            if (!(step > 0)) step = 1.0;
            double val = *cur ? g_ascii_strtod(cur, NULL) : 0.0;
            val += (g_ascii_strcasecmp(name, "Up") == 0) ? step : -step;
            const char *mn = nd_element_get_attr(t, "min");
            const char *mx = nd_element_get_attr(t, "max");
            if (mn && *mn) { double m = g_ascii_strtod(mn, NULL); if (val < m) val = m; }
            if (mx && *mx) { double m = g_ascii_strtod(mx, NULL); if (val > m) val = m; }
            char buf[32];
            g_snprintf(buf, sizeof buf, "%g", val);
            nd_node_set_editable_value(t, buf);
            fc->caret = strlen(buf); fc->anchor = fc->caret;
            if (fc->js) {
                nd_js_dispatch_event(fc->js, t, "input",  NULL);
                nd_js_dispatch_event(fc->js, t, "change", NULL);
                nd_js_consume_mutated(fc->js);
            }
        }
        return;
    }
}

static void
headless_run_actions(headless_flush_ctx *fc, headless_nav_capture *nav,
                     const char *spec)
{
    if (!fc || !spec || !*spec) return;
    headless_relayout(fc);
    char **acts = g_strsplit(spec, ";", -1);
    for (int i = 0; acts[i]; i++) {
        char *a = g_strstrip(acts[i]);
        if (!*a) continue;
        if (g_str_has_prefix(a, "click ")) {
            double x = 0, y = 0;
            if (sscanf(a + 6, "%lf , %lf", &x, &y) == 2) {
                fprintf(stderr, "[headless] click %g,%g\n", x, y);
                headless_click(fc, nav, x, y);
            }
        } else if (g_str_has_prefix(a, "type ")) {
            fprintf(stderr, "[headless] type \"%s\"\n", a + 5);
            headless_edit_replace(fc, MIN(fc->caret, fc->anchor),
                                  MAX(fc->caret, fc->anchor), a + 5);
        } else if (g_str_has_prefix(a, "key ")) {
            fprintf(stderr, "[headless] key %s\n", a + 4);
            headless_key(fc, g_strstrip(a + 4));
        } else if (g_str_has_prefix(a, "wait ")) {
            gint64 ms = g_ascii_strtoll(a + 5, NULL, 10);
            if (ms < 0) ms = 0;
            if (ms > 600000) ms = 600000;
            fprintf(stderr, "[headless] wait %" G_GINT64_FORMAT "ms\n", ms);
            settle_main_loop((int)ms, fc);
        } else {
            fprintf(stderr, "[headless] unknown action: %s\n", a);
        }
        headless_relayout(fc);
        if (nav && nav->pending_url) break;
    }
    g_strfreev(acts);
}

static const char *
inspect_unit_name(nd_css_unit u)
{
    switch (u) {
    case ND_CSS_UNIT_PX:      return "px";
    case ND_CSS_UNIT_EM:      return "em";
    case ND_CSS_UNIT_REM:     return "rem";
    case ND_CSS_UNIT_PERCENT: return "%";
    case ND_CSS_UNIT_NUMBER:  return "";
    case ND_CSS_UNIT_VW:      return "vw";
    case ND_CSS_UNIT_VH:      return "vh";
    case ND_CSS_UNIT_VMIN:    return "vmin";
    case ND_CSS_UNIT_VMAX:    return "vmax";
    default:                  return "";
    }
}

static void
inspect_value_str(const nd_css_value *v, char *buf, size_t cap)
{
    if (!v) { g_strlcpy(buf, "(unset)", cap); return; }
    switch (v->kind) {
    case ND_CSS_V_KEYWORD:
        g_snprintf(buf, cap, "%s", v->u.keyword ? v->u.keyword : "?");
        break;
    case ND_CSS_V_LENGTH:
        g_snprintf(buf, cap, "%g%s", v->u.length.v,
                   inspect_unit_name(v->u.length.unit));
        break;
    case ND_CSS_V_CALC:
        g_snprintf(buf, cap, "calc(%g%% + %gpx)", v->u.calc.pct, v->u.calc.px);
        break;
    case ND_CSS_V_COLOR:
        g_snprintf(buf, cap, "rgba(%u, %u, %u, %u)", v->u.color.r,
                   v->u.color.g, v->u.color.b, v->u.color.a);
        break;
    case ND_CSS_V_URL:
        g_snprintf(buf, cap, "url(%s)", v->u.url ? v->u.url : "");
        break;
    default:
        g_strlcpy(buf, "(set)", cap);
        break;
    }
}

static void
inspect_node_label(const nd_node *n, char *buf, size_t cap)
{
    if (!n || n->kind != ND_NODE_ELEMENT || !n->name) {
        g_strlcpy(buf, n && n->kind == ND_NODE_TEXT ? "#text" : "(anonymous)",
                  cap);
        return;
    }
    GString *s = g_string_new(n->name);
    const char *id = nd_element_get_attr(n, "id");
    if (id && *id) g_string_append_printf(s, "#%s", id);
    const char *cls = nd_element_get_attr(n, "class");
    if (cls && *cls) {
        char **parts = g_strsplit_set(cls, " \t\r\n", -1);
        for (int i = 0; parts[i]; i++)
            if (*parts[i]) g_string_append_printf(s, ".%s", parts[i]);
        g_strfreev(parts);
    }
    g_strlcpy(buf, s->str, cap);
    g_string_free(s, TRUE);
}

static const nd_box *
inspect_find_box(const nd_box *root, const nd_node *dom)
{
    if (!root) return NULL;
    if (root->dom == dom) return root;
    for (const nd_box *c = root->first_child; c; c = c->next_sibling) {
        const nd_box *m = inspect_find_box(c, dom);
        if (m) return m;
    }
    return NULL;
}

static void
inspect_prop_line(GString *out, const nd_style *s, nd_css_prop p,
                  const char *label)
{
    if (!s || !s->values[p]) return;
    char b[160];
    inspect_value_str(s->values[p], b, sizeof b);
    g_string_append_printf(out, "    %-15s %s\n", label, b);
}

static void
inspect_print_box(const nd_box *box, GString *out)
{
    char label[320];
    inspect_node_label(box->dom, label, sizeof label);
    g_string_append_printf(out, "  <%s>\n", label);

    double bx = box->x + box->margin.left;
    double by = box->y + box->margin.top;
    double bw = box->content_width + box->padding.left + box->padding.right +
                box->border.left + box->border.right;
    double bh = box->content_height + box->padding.top + box->padding.bottom +
                box->border.top + box->border.bottom;
    g_string_append_printf(out, "    content         %g x %g  (at %g,%g)\n",
                           box->content_width, box->content_height,
                           box->x, box->y);
    g_string_append_printf(out, "    border-box      %g x %g  (at %g,%g)\n",
                           bw, bh, bx, by);
    g_string_append_printf(out, "    margin          T%g R%g B%g L%g\n",
                           box->margin.top, box->margin.right,
                           box->margin.bottom, box->margin.left);
    g_string_append_printf(out, "    border          T%g R%g B%g L%g\n",
                           box->border.top, box->border.right,
                           box->border.bottom, box->border.left);
    g_string_append_printf(out, "    padding         T%g R%g B%g L%g\n",
                           box->padding.top, box->padding.right,
                           box->padding.bottom, box->padding.left);

    const nd_style *s = box->style;
    inspect_prop_line(out, s, ND_CSS_DISPLAY,    "display");
    inspect_prop_line(out, s, ND_CSS_POSITION,   "position");
    inspect_prop_line(out, s, ND_CSS_BOX_SIZING, "box-sizing");
    inspect_prop_line(out, s, ND_CSS_WIDTH,      "width");
    inspect_prop_line(out, s, ND_CSS_HEIGHT,     "height");
    inspect_prop_line(out, s, ND_CSS_MIN_WIDTH,  "min-width");
    inspect_prop_line(out, s, ND_CSS_MAX_WIDTH,  "max-width");
    inspect_prop_line(out, s, ND_CSS_MIN_HEIGHT, "min-height");
    inspect_prop_line(out, s, ND_CSS_MAX_HEIGHT, "max-height");
    inspect_prop_line(out, s, ND_CSS_FLEX_DIRECTION,  "flex-direction");
    inspect_prop_line(out, s, ND_CSS_JUSTIFY_CONTENT, "justify-content");
    inspect_prop_line(out, s, ND_CSS_ALIGN_ITEMS,     "align-items");
    inspect_prop_line(out, s, ND_CSS_ALIGN_SELF,      "align-self");
    inspect_prop_line(out, s, ND_CSS_FLEX_GROW,       "flex-grow");
    inspect_prop_line(out, s, ND_CSS_FLEX_SHRINK,     "flex-shrink");
    inspect_prop_line(out, s, ND_CSS_FLEX_BASIS,      "flex-basis");
    inspect_prop_line(out, s, ND_CSS_GAP,             "gap");
    inspect_prop_line(out, s, ND_CSS_FONT_SIZE,       "font-size");
    inspect_prop_line(out, s, ND_CSS_COLOR,           "color");
    inspect_prop_line(out, s, ND_CSS_BACKGROUND_COLOR, "background-color");

    gboolean has_children = box->first_child != NULL;
    if (has_children) {
        g_string_append(out, "    children:\n");
        int idx = 0;
        for (const nd_box *c = box->first_child; c; c = c->next_sibling, idx++) {
            char cl[320];
            inspect_node_label(c->dom, cl, sizeof cl);
            char grow[32] = "";
            if (c->style && c->style->values[ND_CSS_FLEX_GROW]) {
                char gb[32];
                inspect_value_str(c->style->values[ND_CSS_FLEX_GROW], gb, sizeof gb);
                g_snprintf(grow, sizeof grow, "  grow=%s", gb);
            }
            g_string_append_printf(out,
                "      [%d] at %g,%g  %g x %g  <%s>%s\n",
                idx, c->x, c->y, c->content_width, c->content_height, cl, grow);
        }
    }

    GPtrArray *chain = g_ptr_array_new();
    for (const nd_box *p = box; p; p = p->parent)
        g_ptr_array_add(chain, (gpointer)p);
    g_string_append(out, "    path            ");
    for (int i = (int)chain->len - 1; i >= 0; i--) {
        const nd_box *p = g_ptr_array_index(chain, i);
        char pl[320];
        inspect_node_label(p->dom, pl, sizeof pl);
        g_string_append(out, pl);
        if (i > 0) g_string_append(out, " > ");
    }
    g_string_append_c(out, '\n');
    g_ptr_array_free(chain, TRUE);
}

static void
headless_collect_matches(const nd_node *n, GPtrArray *sels, GPtrArray *out)
{
    if (!n) return;
    if (n->kind == ND_NODE_ELEMENT && n->name) {
        for (guint i = 0; i < sels->len; i++) {
            if (nd_css_selector_matches(g_ptr_array_index(sels, i), n)) {
                g_ptr_array_add(out, (gpointer)n);
                break;
            }
        }
    }
    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        headless_collect_matches(c, sels, out);
}

static void
headless_inspect(const nd_box *layout, const nd_node *doc, const char *selector,
                 GString *out)
{
    GPtrArray *sels = nd_css_parse_selector_list(selector);
    if (!sels || sels->len == 0) {
        g_string_append_printf(out, "inspect: could not parse selector '%s'\n",
                               selector);
        if (sels) g_ptr_array_free(sels, TRUE);
        return;
    }
    GPtrArray *matches = g_ptr_array_new();
    headless_collect_matches(doc, sels, matches);
    g_string_append_printf(out, "inspect: '%s' matched %u element(s)\n",
                           selector, matches->len);
    for (guint i = 0; i < matches->len; i++) {
        const nd_node *el = g_ptr_array_index(matches, i);
        const nd_box *box = inspect_find_box(layout, el);
        g_string_append_printf(out, "\n--- match %u ---\n", i + 1);
        if (box) {
            inspect_print_box(box, out);
        } else {
            char label[320];
            inspect_node_label(el, label, sizeof label);
            g_string_append_printf(out,
                "  <%s>\n    (not in layout: display:none or non-rendered)\n",
                label);
        }
    }
    g_ptr_array_free(matches, TRUE);
    g_ptr_array_free(sels, TRUE);
}

static void
headless_inspect_at(const nd_box *layout, double x, double y, GString *out)
{
    const nd_box *hit = nd_box_hit_test(layout, x, y);
    g_string_append_printf(out, "inspect-at: %g,%g\n", x, y);
    if (!hit || !hit->dom) {
        g_string_append(out, "  (no element at point)\n");
        return;
    }
    GPtrArray *chain = g_ptr_array_new();
    for (const nd_box *p = hit; p; p = p->parent)
        g_ptr_array_add(chain, (gpointer)p);
    g_string_append(out, "  stack (innermost first):\n");
    for (guint i = 0; i < chain->len; i++) {
        const nd_box *p = g_ptr_array_index(chain, i);
        char label[320];
        inspect_node_label(p->dom, label, sizeof label);
        g_string_append_printf(out, "    %*s<%s>  at %g,%g  %g x %g\n",
                               (int)(i * 2), "", label, p->x, p->y,
                               p->content_width, p->content_height);
    }
    g_ptr_array_free(chain, TRUE);
    g_string_append(out, "\n");
    inspect_print_box(hit, out);
}

static int
nd_headless_run_one(const nd_headless_opts *opts, const char *fetch_url, int hop)
{
    GError *err = NULL;
    nd_response *resp = nd_engine_fetch_blocking(fetch_url, NULL, &err);
    if (!resp) {
        const char *emsg = err ? err->message : "unknown error";
        fprintf(stderr, "headless: fetch failed: %s\n", emsg);
        if (opts->dump == ND_DUMP_PNG || opts->dump == ND_DUMP_PDF) {
            resp = g_new0(nd_response, 1);
            resp->body = g_byte_array_new();
            resp->final_url = g_strdup(opts->url ? opts->url : "");
            resp->content_type = g_strdup("text/html; charset=utf-8");
            char *html = nd_build_error_page(opts->url, 0, emsg);
            g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
            g_free(html);
            g_clear_error(&err);
        } else {
            g_clear_error(&err);
            return 1;
        }
    } else if (resp->error) {
        fprintf(stderr, "headless: fetch error: %s\n", resp->error);
        if (opts->dump == ND_DUMP_PNG || opts->dump == ND_DUMP_PDF) {
            char *html = nd_build_error_page(
                resp->final_url ? resp->final_url : opts->url,
                resp->status, resp->error);
            if (resp->body) g_byte_array_set_size(resp->body, 0);
            else            resp->body = g_byte_array_new();
            g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
            g_free(html);
            g_free(resp->content_type);
            resp->content_type = g_strdup("text/html; charset=utf-8");
        } else {
            nd_response_free(resp);
            return 1;
        }
    } else if (resp->status >= 400) {
        gboolean body_is_html =
            resp->content_type &&
            (g_ascii_strncasecmp(resp->content_type, "text/html", 9) == 0 ||
             g_ascii_strncasecmp(resp->content_type, "application/xhtml", 17) == 0);
        gboolean body_useful = resp->body && resp->body->len > 64 && body_is_html;
        if (!body_useful &&
            (opts->dump == ND_DUMP_PNG || opts->dump == ND_DUMP_PDF)) {
            char *html = nd_build_error_page(
                resp->final_url ? resp->final_url : opts->url,
                resp->status, NULL);
            if (resp->body) g_byte_array_set_size(resp->body, 0);
            else            resp->body = g_byte_array_new();
            g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
            g_free(html);
            g_free(resp->content_type);
            resp->content_type = g_strdup("text/html; charset=utf-8");
        }
    }

    const char *raw = resp->body ? (const char *)resp->body->data : "";
    gsize raw_len = resp->body ? resp->body->len : 0;
    char *decoded = nd_html_decode_body(raw, raw_len);
    nd_node *doc = nd_html_parse(decoded ? decoded : "",
                                 decoded ? (gssize)strlen(decoded) : 0);
    const char *page_url = resp->final_url ? resp->final_url : opts->url;

    int vw = opts->viewport_width > 0 ? opts->viewport_width : 1000;
    double vh = opts->viewport_height > 0 ? (double)opts->viewport_height
                                          : (double)vw * 0.75;
    nd_css_set_viewport((double)vw, vh);
    const char *frag = opts->url ? strchr(opts->url, '#') : NULL;
    nd_css_set_target_fragment(frag && *(frag + 1) ? frag + 1 : NULL);
    GHashTable *css_cache =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                              (GDestroyNotify)g_bytes_unref);
    GHashTable *styles = nd_engine_compute_cascade(doc, page_url, css_cache);

    nd_anim *anim = nd_anim_new();
    nd_engine_load_keyframes(anim, doc, page_url, css_cache);
    nd_engine_anim_observe(anim, styles, g_get_monotonic_time());

    headless_nav_capture nav_cap = {0};
    nd_js *js = nd_js_new(headless_js_log, NULL,
                          headless_js_mutated, NULL,
                          headless_js_navigate, &nav_cap);
    if (js) nd_js_set_form_submit_cb(js, headless_js_form_submit, &nav_cap);
    nd_image_cache *image_cache = NULL;
    if (opts->dump == ND_DUMP_PNG || opts->dump == ND_DUMP_PDF)
        image_cache = nd_image_cache_new();
    nd_box *layout = NULL;
    const char *flush_base = resp->final_url ? resp->final_url : opts->url;
    headless_flush_ctx flush_ctx = {
        .doc = doc, .js = js, .base = flush_base, .vw = vw, .vh = vh,
        .image_cache = image_cache, .anim = anim,
        .css_cache = css_cache, .styles = &styles, .layout = &layout,
    };
    if (js) {
        nd_js_set_style_table(js, styles);
        nd_js_set_image_cache(js, image_cache);
        nd_js_set_layout_flush_cb(js, headless_flush_layout, &flush_ctx);
        nd_js_run_scripts_in_doc(js, doc, resp->final_url);
    }

    if (opts->settle_ms > 0) settle_main_loop(opts->settle_ms, &flush_ctx);

    if (opts->actions && *opts->actions)
        headless_run_actions(&flush_ctx, &nav_cap, opts->actions);

    if (nav_cap.pending_url && hop < 4) {
        char *next = NULL;
        if (strstr(nav_cap.pending_url, "://")) {
            next = g_strdup(nav_cap.pending_url);
        } else {
            const char *base = resp->final_url ? resp->final_url : fetch_url;
            next = nd_url_resolve(base, nav_cap.pending_url);
            if (!next) next = g_strdup(nav_cap.pending_url);
        }
        fprintf(stderr, "[headless follow %s]\n", next);
        g_free(nav_cap.pending_url);
        nav_cap.pending_url = NULL;
        if (js)            nd_js_set_layout_flush_cb(js, NULL, NULL);
        if (js)            nd_js_set_layout_root(js, NULL);
        if (js)            nd_js_set_style_table(js, NULL);
        if (anim)          nd_anim_free(anim);
        if (layout)        nd_box_free(layout);
        if (styles)        g_hash_table_destroy(styles);
        if (css_cache)     g_hash_table_destroy(css_cache);
        if (js)            nd_js_free(js);
        if (doc)           nd_node_free(doc);
        if (image_cache)   nd_image_cache_free(image_cache);
        g_free(decoded);
        nd_response_free(resp);
        int rc2 = nd_headless_run_one(opts, next, hop + 1);
        g_free(next);
        return rc2;
    }

    headless_relayout(&flush_ctx);
    if (js && opts->settle_ms > 0) {
        settle_main_loop(opts->settle_ms, &flush_ctx);
        headless_relayout(&flush_ctx);
    }

    int rc = 0;
    GString *out = g_string_new(NULL);

    switch (opts->dump) {
    case ND_DUMP_NONE:
        break;
    case ND_DUMP_TEXT:
        nd_engine_dump_text(layout, out);
        fwrite(out->str, 1, out->len, stdout);
        break;
    case ND_DUMP_DOM: {
        GString *dom = nd_node_dump(doc);
        fwrite(dom->str, 1, dom->len, stdout);
        g_string_free(dom, TRUE);
        break;
    }
    case ND_DUMP_LAYOUT:
        nd_engine_dump_layout(layout, 0, out);
        fwrite(out->str, 1, out->len, stdout);
        break;
    case ND_DUMP_PNG:
    case ND_DUMP_PDF: {
        const char *base = resp->final_url ? resp->final_url : opts->url;
        if (!image_cache) image_cache = nd_image_cache_new();
        nd_engine_fetch_images(layout, base, image_cache);
        headless_relayout(&flush_ctx);
        nd_paint_set_js(js);
        fetch_videos_into_layout(layout, base);

        int time_ms = opts->time_ms >= 0 ? opts->time_ms : 1000;

        nd_anim_rebase(anim, 0);
        nd_anim_tick(anim, 0);
        nd_paint_set_anim(anim);
        char *initial_path = nd_engine_suffix_before_ext(opts->out_path, "-initial");
        rc = write_capture(layout, initial_path, opts->dump);
        fprintf(stderr, "[headless] initial render -> %s\n", initial_path);
        g_free(initial_path);

        if (js) nd_js_fire_media_load_events(js, layout);
        settle_main_loop(time_ms, &flush_ctx);
        headless_relayout(&flush_ctx);
        fetch_videos_into_layout(layout, base);
        nd_anim_rebase(anim, 0);
        for (gint64 t = 0; t <= (gint64)time_ms * 1000; t += 16000)
            nd_anim_tick(anim, t);
        nd_anim_tick(anim, (gint64)time_ms * 1000);
        nd_paint_set_anim(anim);
        int rc2 = write_capture(layout, opts->out_path, opts->dump);
        fprintf(stderr, "[headless] after %dms -> %s\n", time_ms, opts->out_path);
        if (rc == 0) rc = rc2;
        break;
    }
    }
    g_string_free(out, TRUE);

    if ((opts->inspect && *opts->inspect) ||
        (opts->inspect_at && *opts->inspect_at)) {
        GString *report = g_string_new(NULL);
        if (opts->inspect && *opts->inspect)
            headless_inspect(layout, doc, opts->inspect, report);
        if (opts->inspect_at && *opts->inspect_at) {
            double ix = 0, iy = 0;
            if (sscanf(opts->inspect_at, "%lf , %lf", &ix, &iy) == 2)
                headless_inspect_at(layout, ix, iy, report);
            else
                g_string_append_printf(report,
                    "inspect-at: bad coordinate '%s' (expected X,Y)\n",
                    opts->inspect_at);
        }
        fwrite(report->str, 1, report->len, stdout);
        g_string_free(report, TRUE);
    }

    g_free(decoded);
    g_free(nav_cap.pending_url);
    nd_paint_set_anim(NULL);
    if (anim)          nd_anim_free(anim);
    if (js)            nd_js_set_layout_root(js, NULL);
    if (js)            nd_js_set_style_table(js, NULL);
    if (layout)        nd_box_free(layout);
    if (styles)        g_hash_table_destroy(styles);
    if (css_cache)     g_hash_table_destroy(css_cache);
    if (js)            nd_js_free(js);
    if (doc)           nd_node_free(doc);
    if (image_cache)   nd_image_cache_free(image_cache);
    nd_response_free(resp);
    return rc;
}
