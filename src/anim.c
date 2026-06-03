/* Nordstjernen — CSS transitions and @keyframes animation engine.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "anim.h"

#include <math.h>
#include <string.h>

#define ND_ANIM_MAX_ACTIVE 64

typedef struct nd_anim_color_chan {
    gboolean has_last;
    guint8   last[4];
    gboolean active;
    guint8   from[4], to[4];
    gint64   start_us;
    double   duration_ms, delay_ms;
    nd_css_timing timing;
    guint8   current[4];
} nd_anim_color_chan;

typedef struct nd_anim_state {
    gboolean has_last_opacity;
    double   last_opacity;
    gboolean has_last_transform;
    nd_css_transform last_transform;

    nd_anim_color_chan color, bg;

    gboolean opacity_active;
    double   opacity_from, opacity_to;
    gint64   opacity_start_us;
    double   opacity_duration_ms;
    double   opacity_delay_ms;
    nd_css_timing opacity_timing;
    double   opacity_current;

    gboolean transform_active;
    nd_css_transform transform_from, transform_to;
    gint64   transform_start_us;
    double   transform_duration_ms;
    double   transform_delay_ms;
    nd_css_timing transform_timing;
    nd_css_transform transform_current;

    gboolean anim_active;
    char    *anim_name;
    gint64   anim_start_us;
    double   anim_duration_ms;
    double   anim_delay_ms;
    int      anim_iter_count;
    nd_css_anim_direction anim_direction;
    nd_css_anim_fill      anim_fill;
    nd_css_timing anim_timing;
    gboolean anim_has_opacity_value;
    double   anim_opacity_value;
    gboolean anim_has_transform_value;
    nd_css_transform anim_transform_value;
    gboolean anim_has_color_value;
    guint8   anim_color_value[4];
    gboolean anim_has_bg_value;
    guint8   anim_bg_value[4];
} nd_anim_state;

struct nd_anim {
    GHashTable *states;
    GHashTable *active;
    GHashTable *keyframes;
    int         active_count;
};

static void
nd_anim_state_free(gpointer data)
{
    nd_anim_state *s = data;
    if (!s) return;
    g_free(s->anim_name);
    g_free(s);
}

static void
nd_anim_keyframes_free(gpointer data)
{
    nd_css_keyframes *kf = data;
    if (!kf) return;
    g_free(kf->name);
    g_free(kf->stops);
    g_free(kf);
}

static gboolean
state_is_active(const nd_anim_state *s)
{
    return s->opacity_active || s->transform_active ||
           s->color.active || s->bg.active || s->anim_active;
}

static void
anim_track(nd_anim *a, nd_anim_state *s)
{
    if (state_is_active(s)) g_hash_table_add(a->active, s);
    else                    g_hash_table_remove(a->active, s);
}

nd_anim *
nd_anim_new(void)
{
    nd_anim *a = g_new0(nd_anim, 1);
    a->states    = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                         NULL, nd_anim_state_free);
    a->active    = g_hash_table_new(g_direct_hash, g_direct_equal);
    a->keyframes = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free, nd_anim_keyframes_free);
    return a;
}

void
nd_anim_free(nd_anim *a)
{
    if (!a) return;
    g_hash_table_destroy(a->active);
    g_hash_table_destroy(a->states);
    g_hash_table_destroy(a->keyframes);
    g_free(a);
}

void
nd_anim_prune(nd_anim *a, GHashTable *live)
{
    if (!a || !live) return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, a->states);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        if (g_hash_table_contains(live, key)) continue;
        nd_anim_state *s = val;
        if (s->opacity_active   && a->active_count > 0) a->active_count--;
        if (s->transform_active && a->active_count > 0) a->active_count--;
        if (s->color.active     && a->active_count > 0) a->active_count--;
        if (s->bg.active        && a->active_count > 0) a->active_count--;
        if (s->anim_active      && a->active_count > 0) a->active_count--;
        g_hash_table_remove(a->active, s);
        g_hash_table_iter_remove(&it);
    }
}

void
nd_anim_rebase(nd_anim *a, gint64 base_us)
{
    if (!a) return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, a->states);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        nd_anim_state *s = val;
        s->opacity_start_us   = base_us;
        s->transform_start_us = base_us;
        s->color.start_us     = base_us;
        s->bg.start_us        = base_us;
        s->anim_start_us      = base_us;
    }
}

static void
nd_anim_register_keyframes(nd_anim *a, const nd_css_keyframes *src)
{
    if (!a || !src || !src->name) return;
    nd_css_keyframes *copy = g_new0(nd_css_keyframes, 1);
    copy->name = g_strdup(src->name);
    copy->n_stops = src->n_stops;
    if (src->n_stops > 0) {
        copy->stops = g_new(nd_css_keyframe_stop, src->n_stops);
        memcpy(copy->stops, src->stops,
               src->n_stops * sizeof(nd_css_keyframe_stop));
    }
    g_hash_table_replace(a->keyframes, g_strdup(src->name), copy);
}

void
nd_anim_load_from_stylesheet(nd_anim *a, const nd_css_stylesheet *sh)
{
    if (!a || !sh || !sh->keyframes) return;
    for (guint i = 0; i < sh->keyframes->len; i++) {
        const nd_css_keyframes *kf =
            &g_array_index(sh->keyframes, nd_css_keyframes, i);
        nd_anim_register_keyframes(a, kf);
    }
}

static double
steps_apply(int n, nd_css_step_pos pos, double x)
{
    if (n < 1) n = 1;
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    int step = (int)floor(x * n);
    if (pos == ND_CSS_STEP_JUMP_START || pos == ND_CSS_STEP_JUMP_BOTH)
        step += 1;
    int jumps = n;
    if (pos == ND_CSS_STEP_JUMP_NONE) jumps = n > 1 ? n - 1 : 1;
    else if (pos == ND_CSS_STEP_JUMP_BOTH) jumps = n + 1;
    if (step < 0) step = 0;
    if (step > jumps) step = jumps;
    return (double)step / jumps;
}

static double
cubic_bezier_axis(double t, double p1, double p2)
{
    double mt = 1.0 - t;
    return 3.0 * mt * mt * t * p1 + 3.0 * mt * t * t * p2 + t * t * t;
}

static double
cubic_bezier_apply(const double cb[4], double x)
{
    double t = x;
    for (int i = 0; i < 8; i++) {
        double xt = cubic_bezier_axis(t, cb[0], cb[2]) - x;
        if (fabs(xt) < 1e-6) break;
        double mt = 1.0 - t;
        double d = 3.0 * mt * mt * cb[0]
                 + 6.0 * mt * t * (cb[2] - cb[0])
                 + 3.0 * t * t * (1.0 - cb[2]);
        if (fabs(d) < 1e-6) break;
        t -= xt / d;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
    }
    return cubic_bezier_axis(t, cb[1], cb[3]);
}

static double
timing_apply(nd_css_timing t, double x)
{
    if (t.kind == ND_CSS_TIMING_STEPS)
        return steps_apply(t.steps, t.step_pos, x);
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    switch (t.kind) {
    case ND_CSS_TIMING_LINEAR:      return x;
    case ND_CSS_TIMING_EASE_IN:     return x * x;
    case ND_CSS_TIMING_EASE_OUT:    return 1 - (1 - x) * (1 - x);
    case ND_CSS_TIMING_EASE_IN_OUT:
        return x < 0.5 ? 2 * x * x : 1 - 2 * (1 - x) * (1 - x);
    case ND_CSS_TIMING_CUBIC:       return cubic_bezier_apply(t.cb, x);
    case ND_CSS_TIMING_EASE:
    default:
        return x < 0.5 ? 4 * x * x * x
                       : 1 - pow(-2 * x + 2, 3) / 2;
    }
}

static gboolean
transforms_compatible(const nd_css_transform *a, const nd_css_transform *b)
{
    if (a->n_ops != b->n_ops) return FALSE;
    for (int i = 0; i < a->n_ops; i++)
        if (a->ops[i].kind != b->ops[i].kind) return FALSE;
    return TRUE;
}

static void
transform_lerp(const nd_css_transform *from, const nd_css_transform *to,
               double t, nd_css_transform *out)
{
    out->n_ops = from->n_ops;
    for (int i = 0; i < from->n_ops; i++) {
        out->ops[i].kind = from->ops[i].kind;
        out->ops[i].a = from->ops[i].a + (to->ops[i].a - from->ops[i].a) * t;
        out->ops[i].b = from->ops[i].b + (to->ops[i].b - from->ops[i].b) * t;
        out->ops[i].c = from->ops[i].c + (to->ops[i].c - from->ops[i].c) * t;
        out->ops[i].d = from->ops[i].d + (to->ops[i].d - from->ops[i].d) * t;
        out->ops[i].e = from->ops[i].e + (to->ops[i].e - from->ops[i].e) * t;
        out->ops[i].f = from->ops[i].f + (to->ops[i].f - from->ops[i].f) * t;
        out->ops[i].a_is_percent = from->ops[i].a_is_percent;
        out->ops[i].b_is_percent = from->ops[i].b_is_percent;
        out->ops[i].e_is_percent = from->ops[i].e_is_percent;
        out->ops[i].f_is_percent = from->ops[i].f_is_percent;
    }
}

static void
color_lerp(const guint8 from[4], const guint8 to[4], double t, guint8 out[4])
{
    for (int i = 0; i < 4; i++) {
        double v = from[i] + ((double)to[i] - from[i]) * t;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        out[i] = (guint8)(v + 0.5);
    }
}

static gboolean
colors_differ(const guint8 a[4], const guint8 b[4])
{
    return a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || a[3] != b[3];
}

static const nd_css_anim_entry *
find_entry(const nd_css_anim_list *list, nd_css_anim_target target)
{
    if (!list) return NULL;
    const nd_css_anim_entry *fallback = NULL;
    for (int i = 0; i < list->n; i++) {
        if (list->entries[i].target == target) return &list->entries[i];
        if (list->entries[i].target == ND_CSS_ANIM_TARGET_ALL && !fallback)
            fallback = &list->entries[i];
    }
    return fallback;
}

static nd_anim_state *
state_for(nd_anim *a, const nd_node *dom)
{
    nd_anim_state *s = g_hash_table_lookup(a->states, dom);
    if (s) return s;
    s = g_new0(nd_anim_state, 1);
    g_hash_table_insert(a->states, (gpointer)dom, s);
    return s;
}

static gboolean
should_skip_motion(void)
{
    return nd_css_get_reduced_motion() == ND_CSS_REDUCED_MOTION_REDUCE;
}

static void
observe_color_chan(nd_anim *a, nd_anim_color_chan *ch, gboolean cur_has,
                   const guint8 cur[4], const nd_css_anim_entry *e,
                   gint64 now_us)
{
    if (!cur_has) return;
    if (e && e->duration_ms > 0 && ch->has_last &&
        colors_differ(ch->last, cur)) {
        if (ch->active || a->active_count < ND_ANIM_MAX_ACTIVE) {
            const guint8 *from = ch->active ? ch->current : ch->last;
            if (!ch->active) a->active_count++;
            ch->active = TRUE;
            memcpy(ch->from, from, 4);
            memcpy(ch->to, cur, 4);
            memcpy(ch->current, from, 4);
            ch->start_us = now_us;
            ch->duration_ms = e->duration_ms;
            ch->delay_ms    = e->delay_ms;
            ch->timing      = e->timing;
        }
    }
    memcpy(ch->last, cur, 4);
    ch->has_last = TRUE;
}

static gboolean
value_to_rgba(const nd_css_value *v, guint8 out[4])
{
    if (!v || v->kind != ND_CSS_V_COLOR) return FALSE;
    out[0] = v->u.color.r;
    out[1] = v->u.color.g;
    out[2] = v->u.color.b;
    out[3] = v->u.color.a;
    return TRUE;
}

static void
observe_transition(nd_anim *a, nd_anim_state *s, const nd_style *style,
                   gint64 now_us, const nd_node *dom)
{
    const nd_css_value *tv = style ? style->values[ND_CSS_TRANSITION] : NULL;
    if (!tv || tv->kind != ND_CSS_V_ANIM) return;
    if (should_skip_motion()) return;

    const nd_css_value *ov = style->values[ND_CSS_OPACITY];
    double cur_opacity = 1.0;
    if (ov && ov->kind == ND_CSS_V_LENGTH) cur_opacity = ov->u.length.v;
    if (cur_opacity < 0) cur_opacity = 0;
    if (cur_opacity > 1) cur_opacity = 1;

    const nd_css_anim_entry *op_e =
        find_entry(&tv->u.anim, ND_CSS_ANIM_TARGET_OPACITY);
    if (op_e && op_e->duration_ms > 0 && s->has_last_opacity &&
        fabs(cur_opacity - s->last_opacity) > 0.001) {
        if (s->opacity_active || a->active_count < ND_ANIM_MAX_ACTIVE) {
            double from = s->opacity_active ? s->opacity_current
                                            : s->last_opacity;
            if (!s->opacity_active) a->active_count++;
            s->opacity_active = TRUE;
            s->opacity_from = from;
            s->opacity_to   = cur_opacity;
            s->opacity_start_us = now_us;
            s->opacity_duration_ms = op_e->duration_ms;
            s->opacity_delay_ms    = op_e->delay_ms;
            s->opacity_timing      = op_e->timing;
            s->opacity_current     = from;
            if (dom && g_getenv("ND_ANIM_DEBUG")) {
                const char *id = nd_element_get_attr(dom, "id");
                const char *cl = nd_element_get_attr(dom, "class");
                g_printerr("[anim] opacity transition <%s id=%s class=%s>"
                           " %.2f->%.2f dur=%.0fms\n",
                           dom->name ? dom->name : "?", id ? id : "",
                           cl ? cl : "", from, cur_opacity,
                           (double)op_e->duration_ms);
            }
        }
    }
    s->last_opacity = cur_opacity;
    s->has_last_opacity = TRUE;

    const nd_css_value *cv = style->values[ND_CSS_TRANSFORM];
    nd_css_transform cur_tf = { 0 };
    gboolean cur_has_tf = FALSE;
    if (cv && cv->kind == ND_CSS_V_TRANSFORM) {
        cur_tf = cv->u.transform;
        cur_has_tf = TRUE;
    }
    const nd_css_anim_entry *tf_e =
        find_entry(&tv->u.anim, ND_CSS_ANIM_TARGET_TRANSFORM);
    if (tf_e && tf_e->duration_ms > 0 && s->has_last_transform && cur_has_tf &&
        transforms_compatible(&s->last_transform, &cur_tf)) {
        gboolean differs = FALSE;
        for (int i = 0; i < cur_tf.n_ops; i++) {
            if (fabs(cur_tf.ops[i].a - s->last_transform.ops[i].a) > 0.001 ||
                fabs(cur_tf.ops[i].b - s->last_transform.ops[i].b) > 0.001) {
                differs = TRUE; break;
            }
        }
        if (differs && (s->transform_active ||
                        a->active_count < ND_ANIM_MAX_ACTIVE)) {
            nd_css_transform from = s->transform_active
                ? s->transform_current : s->last_transform;
            if (!s->transform_active) a->active_count++;
            s->transform_active = TRUE;
            s->transform_from = from;
            s->transform_to   = cur_tf;
            s->transform_start_us = now_us;
            s->transform_duration_ms = tf_e->duration_ms;
            s->transform_delay_ms    = tf_e->delay_ms;
            s->transform_timing      = tf_e->timing;
            s->transform_current     = from;
            if (dom && g_getenv("ND_ANIM_DEBUG")) {
                const char *id = nd_element_get_attr(dom, "id");
                const char *cl = nd_element_get_attr(dom, "class");
                g_printerr("[anim] transform transition <%s id=%s class=%s>"
                           " from(%d ops) to(%d ops) dur=%.0fms\n",
                           dom->name ? dom->name : "?", id ? id : "",
                           cl ? cl : "", from.n_ops, cur_tf.n_ops,
                           (double)tf_e->duration_ms);
            }
        }
    }
    if (cur_has_tf) {
        s->last_transform = cur_tf;
        s->has_last_transform = TRUE;
    }

    guint8 cur_col[4];
    gboolean has_col = value_to_rgba(style->values[ND_CSS_COLOR], cur_col);
    observe_color_chan(a, &s->color, has_col, cur_col,
                       find_entry(&tv->u.anim, ND_CSS_ANIM_TARGET_COLOR),
                       now_us);

    guint8 cur_bg[4];
    gboolean has_bg = value_to_rgba(style->values[ND_CSS_BACKGROUND_COLOR],
                                    cur_bg);
    observe_color_chan(a, &s->bg, has_bg, cur_bg,
                       find_entry(&tv->u.anim, ND_CSS_ANIM_TARGET_BG_COLOR),
                       now_us);
}

static void
observe_animation(nd_anim *a, nd_anim_state *s, const nd_style *style,
                  gint64 now_us, const nd_node *dom)
{
    const nd_css_value *av = style ? style->values[ND_CSS_ANIMATION] : NULL;
    if (!av || av->kind != ND_CSS_V_ANIM || av->u.anim.n == 0) {
        if (s->anim_active) {
            s->anim_active = FALSE;
            if (a->active_count > 0) a->active_count--;
            g_free(s->anim_name);
            s->anim_name = NULL;
        }
        return;
    }
    if (should_skip_motion()) return;
    const nd_css_anim_entry *e = &av->u.anim.entries[0];
    if (!e->name || e->duration_ms <= 0) return;
    if (s->anim_active && s->anim_name &&
        strcmp(s->anim_name, e->name) == 0 &&
        s->anim_duration_ms == e->duration_ms) {
        return;
    }
    if (!s->anim_active) {
        if (a->active_count >= ND_ANIM_MAX_ACTIVE) return;
        a->active_count++;
    }
    g_free(s->anim_name);
    s->anim_name = g_strdup(e->name);
    s->anim_start_us = now_us;
    s->anim_duration_ms = e->duration_ms;
    s->anim_delay_ms = e->delay_ms;
    s->anim_iter_count = e->iter_count;
    s->anim_direction = e->direction;
    s->anim_fill = e->fill;
    s->anim_timing = e->timing;
    s->anim_active = TRUE;
    s->anim_has_opacity_value = FALSE;
    s->anim_has_transform_value = FALSE;
    if (dom && g_getenv("ND_ANIM_DEBUG")) {
        const char *id = nd_element_get_attr(dom, "id");
        const char *cl = nd_element_get_attr(dom, "class");
        g_printerr("[anim] keyframe animation '%s' <%s id=%s class=%s>"
                   " dur=%.0fms\n", e->name, dom->name ? dom->name : "?",
                   id ? id : "", cl ? cl : "", (double)e->duration_ms);
    }
}

void
nd_anim_observe(nd_anim *a, const nd_node *dom,
                const nd_style *style, gint64 now_us)
{
    if (!a || !dom || !style) return;
    nd_anim_state *s = state_for(a, dom);
    observe_transition(a, s, style, now_us, dom);
    observe_animation(a, s, style, now_us, dom);
    anim_track(a, s);
}

static gboolean
advance_opacity(nd_anim *a, nd_anim_state *s, gint64 now_us)
{
    double elapsed = (now_us - s->opacity_start_us) / 1000.0 - s->opacity_delay_ms;
    if (elapsed < 0) {
        s->opacity_current = s->opacity_from;
        return TRUE;
    }
    if (elapsed >= s->opacity_duration_ms) {
        s->opacity_current = s->opacity_to;
        s->opacity_active = FALSE;
        if (a->active_count > 0) a->active_count--;
        return TRUE;
    }
    double t = elapsed / s->opacity_duration_ms;
    t = timing_apply(s->opacity_timing, t);
    s->opacity_current = s->opacity_from + (s->opacity_to - s->opacity_from) * t;
    return TRUE;
}

static gboolean
advance_transform(nd_anim *a, nd_anim_state *s, gint64 now_us)
{
    double elapsed = (now_us - s->transform_start_us) / 1000.0
                     - s->transform_delay_ms;
    if (elapsed < 0) {
        s->transform_current = s->transform_from;
        return TRUE;
    }
    if (elapsed >= s->transform_duration_ms) {
        s->transform_current = s->transform_to;
        s->transform_active = FALSE;
        if (a->active_count > 0) a->active_count--;
        return TRUE;
    }
    double t = elapsed / s->transform_duration_ms;
    t = timing_apply(s->transform_timing, t);
    transform_lerp(&s->transform_from, &s->transform_to, t,
                   &s->transform_current);
    return TRUE;
}

static gboolean
advance_color_chan(nd_anim *a, nd_anim_color_chan *ch, gint64 now_us)
{
    double elapsed = (now_us - ch->start_us) / 1000.0 - ch->delay_ms;
    if (elapsed < 0) {
        memcpy(ch->current, ch->from, 4);
        return TRUE;
    }
    if (elapsed >= ch->duration_ms) {
        memcpy(ch->current, ch->to, 4);
        ch->active = FALSE;
        if (a->active_count > 0) a->active_count--;
        return TRUE;
    }
    double t = timing_apply(ch->timing, elapsed / ch->duration_ms);
    color_lerp(ch->from, ch->to, t, ch->current);
    return TRUE;
}

static void
keyframe_sample(const nd_css_keyframes *kf, double pct,
                gboolean *out_has_opacity, double *out_opacity,
                gboolean *out_has_transform, nd_css_transform *out_transform,
                gboolean *out_has_color, guint8 out_color[4],
                gboolean *out_has_bg, guint8 out_bg[4])
{
    *out_has_opacity = FALSE;
    *out_has_transform = FALSE;
    *out_has_color = FALSE;
    *out_has_bg = FALSE;
    if (!kf || kf->n_stops == 0) return;

    const nd_css_keyframe_stop *prev_op = NULL, *next_op = NULL;
    const nd_css_keyframe_stop *prev_tf = NULL, *next_tf = NULL;
    const nd_css_keyframe_stop *prev_col = NULL, *next_col = NULL;
    const nd_css_keyframe_stop *prev_bg = NULL, *next_bg = NULL;
    for (int i = 0; i < kf->n_stops; i++) {
        const nd_css_keyframe_stop *s = &kf->stops[i];
        if (s->has_opacity) {
            if (s->pct <= pct) prev_op = s;
            if (s->pct >= pct && !next_op) next_op = s;
        }
        if (s->has_transform) {
            if (s->pct <= pct) prev_tf = s;
            if (s->pct >= pct && !next_tf) next_tf = s;
        }
        if (s->has_color) {
            if (s->pct <= pct) prev_col = s;
            if (s->pct >= pct && !next_col) next_col = s;
        }
        if (s->has_bg_color) {
            if (s->pct <= pct) prev_bg = s;
            if (s->pct >= pct && !next_bg) next_bg = s;
        }
    }
    if (prev_op && next_op) {
        double range = next_op->pct - prev_op->pct;
        double t = range > 0 ? (pct - prev_op->pct) / range : 0;
        *out_opacity = prev_op->opacity + (next_op->opacity - prev_op->opacity) * t;
        *out_has_opacity = TRUE;
    } else if (prev_op) {
        *out_opacity = prev_op->opacity;
        *out_has_opacity = TRUE;
    } else if (next_op) {
        *out_opacity = next_op->opacity;
        *out_has_opacity = TRUE;
    }
    if (prev_tf && next_tf &&
        transforms_compatible(&prev_tf->transform, &next_tf->transform)) {
        double range = next_tf->pct - prev_tf->pct;
        double t = range > 0 ? (pct - prev_tf->pct) / range : 0;
        transform_lerp(&prev_tf->transform, &next_tf->transform, t, out_transform);
        *out_has_transform = TRUE;
    } else if (prev_tf) {
        *out_transform = prev_tf->transform;
        *out_has_transform = TRUE;
    } else if (next_tf) {
        *out_transform = next_tf->transform;
        *out_has_transform = TRUE;
    }
    if (prev_col && next_col) {
        double range = next_col->pct - prev_col->pct;
        double t = range > 0 ? (pct - prev_col->pct) / range : 0;
        color_lerp(prev_col->color, next_col->color, t, out_color);
        *out_has_color = TRUE;
    } else if (prev_col) {
        memcpy(out_color, prev_col->color, 4);
        *out_has_color = TRUE;
    } else if (next_col) {
        memcpy(out_color, next_col->color, 4);
        *out_has_color = TRUE;
    }
    if (prev_bg && next_bg) {
        double range = next_bg->pct - prev_bg->pct;
        double t = range > 0 ? (pct - prev_bg->pct) / range : 0;
        color_lerp(prev_bg->bg_color, next_bg->bg_color, t, out_bg);
        *out_has_bg = TRUE;
    } else if (prev_bg) {
        memcpy(out_bg, prev_bg->bg_color, 4);
        *out_has_bg = TRUE;
    } else if (next_bg) {
        memcpy(out_bg, next_bg->bg_color, 4);
        *out_has_bg = TRUE;
    }
}

static double
directed_progress(int iter, double raw, nd_css_anim_direction dir)
{
    gboolean rev;
    switch (dir) {
    case ND_CSS_ANIM_DIR_REVERSE:           rev = TRUE; break;
    case ND_CSS_ANIM_DIR_ALTERNATE:         rev = (iter & 1); break;
    case ND_CSS_ANIM_DIR_ALTERNATE_REVERSE: rev = !(iter & 1); break;
    case ND_CSS_ANIM_DIR_NORMAL:
    default:                                rev = FALSE; break;
    }
    return rev ? 1.0 - raw : raw;
}

static void
anim_clear_values(nd_anim_state *s)
{
    s->anim_has_opacity_value = FALSE;
    s->anim_has_transform_value = FALSE;
    s->anim_has_color_value = FALSE;
    s->anim_has_bg_value = FALSE;
}

static void
anim_sample_at(nd_anim_state *s, nd_css_keyframes *kf, double progress)
{
    double pct = timing_apply(s->anim_timing, progress) * 100.0;
    keyframe_sample(kf, pct,
                    &s->anim_has_opacity_value, &s->anim_opacity_value,
                    &s->anim_has_transform_value, &s->anim_transform_value,
                    &s->anim_has_color_value, s->anim_color_value,
                    &s->anim_has_bg_value, s->anim_bg_value);
}

static gboolean
advance_animation(nd_anim *a, nd_anim_state *s, gint64 now_us)
{
    if (!s->anim_name) return FALSE;
    nd_css_keyframes *kf = g_hash_table_lookup(a->keyframes, s->anim_name);
    if (!kf || kf->n_stops == 0) return FALSE;
    double cycle_ms = s->anim_duration_ms > 0 ? s->anim_duration_ms : 1.0;
    double elapsed = (now_us - s->anim_start_us) / 1000.0 - s->anim_delay_ms;

    if (elapsed < 0) {
        gboolean fill_back = s->anim_fill == ND_CSS_ANIM_FILL_BACKWARDS ||
                             s->anim_fill == ND_CSS_ANIM_FILL_BOTH;
        if (!fill_back) { anim_clear_values(s); return FALSE; }
        anim_sample_at(s, kf, directed_progress(0, 0.0, s->anim_direction));
        return TRUE;
    }

    int iter = (int)(elapsed / cycle_ms);
    if (s->anim_iter_count > 0 && iter >= s->anim_iter_count) {
        if (s->anim_active) {
            s->anim_active = FALSE;
            if (a->active_count > 0) a->active_count--;
        }
        gboolean fill_fwd = s->anim_fill == ND_CSS_ANIM_FILL_FORWARDS ||
                            s->anim_fill == ND_CSS_ANIM_FILL_BOTH;
        if (!fill_fwd) { anim_clear_values(s); return TRUE; }
        int last = s->anim_iter_count - 1;
        anim_sample_at(s, kf, directed_progress(last, 1.0, s->anim_direction));
        return TRUE;
    }

    double raw = fmod(elapsed, cycle_ms) / cycle_ms;
    anim_sample_at(s, kf, directed_progress(iter, raw, s->anim_direction));
    return TRUE;
}

gboolean
nd_anim_tick(nd_anim *a, gint64 now_us)
{
    if (!a) return FALSE;
    if (g_hash_table_size(a->active) == 0) return FALSE;
    gboolean any = FALSE;
    GHashTableIter it;
    gpointer key, val;
    nd_anim_state *settled[64];
    int n_settled = 0;
    g_hash_table_iter_init(&it, a->active);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        nd_anim_state *s = key;
        if (s->opacity_active && advance_opacity(a, s, now_us)) any = TRUE;
        if (s->transform_active && advance_transform(a, s, now_us)) any = TRUE;
        if (s->color.active && advance_color_chan(a, &s->color, now_us)) any = TRUE;
        if (s->bg.active && advance_color_chan(a, &s->bg, now_us)) any = TRUE;
        if (s->anim_active && advance_animation(a, s, now_us)) any = TRUE;
        if (!state_is_active(s) && n_settled < 64) settled[n_settled++] = s;
    }
    for (int i = 0; i < n_settled; i++)
        g_hash_table_remove(a->active, settled[i]);
    return any;
}

gboolean
nd_anim_get_opacity(nd_anim *a, const nd_node *dom, double *out_opacity)
{
    if (!a || !dom || !out_opacity) return FALSE;
    nd_anim_state *s = g_hash_table_lookup(a->states, dom);
    if (!s) return FALSE;
    if (s->anim_active && s->anim_has_opacity_value) {
        *out_opacity = s->anim_opacity_value;
        return TRUE;
    }
    if (s->opacity_active) {
        *out_opacity = s->opacity_current;
        return TRUE;
    }
    return FALSE;
}

const nd_css_transform *
nd_anim_get_transform(nd_anim *a, const nd_node *dom)
{
    if (!a || !dom) return NULL;
    nd_anim_state *s = g_hash_table_lookup(a->states, dom);
    if (!s) return NULL;
    if (s->anim_active && s->anim_has_transform_value)
        return &s->anim_transform_value;
    if (s->transform_active && s->transform_current.n_ops > 0)
        return &s->transform_current;
    return NULL;
}

gboolean
nd_anim_get_color(nd_anim *a, const nd_node *dom,
                  nd_css_anim_target which, guint8 out_rgba[4])
{
    if (!a || !dom || !out_rgba) return FALSE;
    nd_anim_state *s = g_hash_table_lookup(a->states, dom);
    if (!s) return FALSE;
    if (which == ND_CSS_ANIM_TARGET_COLOR) {
        if (s->anim_active && s->anim_has_color_value) {
            memcpy(out_rgba, s->anim_color_value, 4);
            return TRUE;
        }
        if (s->color.active) {
            memcpy(out_rgba, s->color.current, 4);
            return TRUE;
        }
    } else if (which == ND_CSS_ANIM_TARGET_BG_COLOR) {
        if (s->anim_active && s->anim_has_bg_value) {
            memcpy(out_rgba, s->anim_bg_value, 4);
            return TRUE;
        }
        if (s->bg.active) {
            memcpy(out_rgba, s->bg.current, 4);
            return TRUE;
        }
    }
    return FALSE;
}
