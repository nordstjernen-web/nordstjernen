/* Nordstjernen — CSS transitions and @keyframes animation engine.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "anim.h"

#include <math.h>
#include <string.h>

#define NS_ANIM_MAX_ACTIVE 64

typedef struct ns_anim_color_chan {
    gboolean has_last;
    guint8   last[4];
    gboolean active;
    guint8   from[4], to[4];
    gint64   start_us;
    double   duration_ms, delay_ms;
    ns_css_timing timing;
    guint8   current[4];
} ns_anim_color_chan;

typedef struct ns_anim_state {
    const ns_node *node;
    gboolean has_last_opacity;
    double   last_opacity;
    gboolean has_last_transform;
    ns_css_transform last_transform;

    ns_anim_color_chan color, bg;

    gboolean opacity_active;
    double   opacity_from, opacity_to;
    gint64   opacity_start_us;
    double   opacity_duration_ms;
    double   opacity_delay_ms;
    ns_css_timing opacity_timing;
    double   opacity_current;

    gboolean transform_active;
    ns_css_transform transform_from, transform_to;
    gint64   transform_start_us;
    double   transform_duration_ms;
    double   transform_delay_ms;
    ns_css_timing transform_timing;
    ns_css_transform transform_current;

    gboolean anim_active;
    gboolean anim_paused;
    gboolean anim_started;
    int      anim_iters_emitted;
    double   anim_elapsed_base_ms;
    ns_css_keyframes *anim_kf;
    char    *anim_name;
    gint64   anim_start_us;
    double   anim_duration_ms;
    double   anim_delay_ms;
    int      anim_iter_count;
    ns_css_anim_direction anim_direction;
    ns_css_anim_fill      anim_fill;
    ns_css_timing anim_timing;
    gboolean anim_has_opacity_value;
    double   anim_opacity_value;
    gboolean anim_has_transform_value;
    ns_css_transform anim_transform_value;
    gboolean anim_has_color_value;
    guint8   anim_color_value[4];
    gboolean anim_has_bg_value;
    guint8   anim_bg_value[4];

    GPtrArray *generic;
} ns_anim_state;

typedef struct ns_anim_generic_chan {
    char    *prop;
    char    *last;
    gboolean has_last;
    gboolean active;
    gint64   start_us;
    double   duration_ms;
    double   delay_ms;
} ns_anim_generic_chan;

typedef struct {
    const ns_node *node;
    const char    *type;
    char          *name;
    double         elapsed_ms;
} ns_anim_event;

struct ns_anim {
    GHashTable *states;
    GHashTable *active;
    GHashTable *keyframes;
    int         active_count;
    GArray     *events;
};

static void
anim_emit(ns_anim *a, const ns_node *node, const char *type,
          const char *name, double elapsed_ms)
{
    if (!a || !node) return;
    if (!a->events)
        a->events = g_array_new(FALSE, FALSE, sizeof(ns_anim_event));
    ns_anim_event e = { node, type, g_strdup(name ? name : ""), elapsed_ms };
    g_array_append_val(a->events, e);
}

void
ns_anim_drain_events(ns_anim *a, ns_anim_event_cb cb, gpointer user)
{
    if (!a || !a->events || a->events->len == 0) return;
    GArray *evs = a->events;
    a->events = NULL;
    for (guint i = 0; i < evs->len; i++) {
        ns_anim_event *e = &g_array_index(evs, ns_anim_event, i);
        if (cb) cb(e->node, e->type, e->name, e->elapsed_ms, user);
        g_free(e->name);
    }
    g_array_free(evs, TRUE);
}

static void
ns_anim_state_free(gpointer data)
{
    ns_anim_state *s = data;
    if (!s) return;
    ns_css_keyframes_resolved_free(s->anim_kf);
    g_free(s->anim_name);
    if (s->generic) {
        for (guint i = 0; i < s->generic->len; i++) {
            ns_anim_generic_chan *ch = s->generic->pdata[i];
            g_free(ch->prop);
            g_free(ch->last);
            g_free(ch);
        }
        g_ptr_array_free(s->generic, TRUE);
    }
    g_free(s);
}

static void
ns_anim_keyframes_free(gpointer data)
{
    ns_css_keyframes *kf = data;
    if (!kf) return;
    g_free(kf->name);
    for (int i = 0; i < kf->n_stops; i++)
        g_free(kf->stops[i].raw_props);
    g_free(kf->stops);
    g_free(kf);
}

static gboolean advance_animation(ns_anim *a, ns_anim_state *s, gint64 now_us);

static gboolean
state_is_active(const ns_anim_state *s)
{
    if (s->opacity_active || s->transform_active ||
        s->color.active || s->bg.active ||
        (s->anim_active && !s->anim_paused))
        return TRUE;
    if (s->generic)
        for (guint i = 0; i < s->generic->len; i++)
            if (((ns_anim_generic_chan *)s->generic->pdata[i])->active)
                return TRUE;
    return FALSE;
}

static void
anim_track(ns_anim *a, ns_anim_state *s)
{
    if (state_is_active(s)) g_hash_table_add(a->active, s);
    else                    g_hash_table_remove(a->active, s);
}

ns_anim *
ns_anim_new(void)
{
    ns_anim *a = g_new0(ns_anim, 1);
    a->states    = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                         NULL, ns_anim_state_free);
    a->active    = g_hash_table_new(g_direct_hash, g_direct_equal);
    a->keyframes = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free, ns_anim_keyframes_free);
    return a;
}

void
ns_anim_free(ns_anim *a)
{
    if (!a) return;
    g_hash_table_destroy(a->active);
    g_hash_table_destroy(a->states);
    g_hash_table_destroy(a->keyframes);
    if (a->events) {
        for (guint i = 0; i < a->events->len; i++)
            g_free(g_array_index(a->events, ns_anim_event, i).name);
        g_array_free(a->events, TRUE);
    }
    g_free(a);
}

void
ns_anim_prune(ns_anim *a, GHashTable *live)
{
    if (!a || !live) return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, a->states);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        if (g_hash_table_contains(live, key)) continue;
        ns_anim_state *s = val;
        if (s->opacity_active   && a->active_count > 0) a->active_count--;
        if (s->transform_active && a->active_count > 0) a->active_count--;
        if (s->color.active     && a->active_count > 0) a->active_count--;
        if (s->bg.active        && a->active_count > 0) a->active_count--;
        if (s->anim_active      && a->active_count > 0) a->active_count--;
        if (s->generic)
            for (guint i = 0; i < s->generic->len; i++)
                if (((ns_anim_generic_chan *)s->generic->pdata[i])->active &&
                    a->active_count > 0)
                    a->active_count--;
        g_hash_table_remove(a->active, s);
        g_hash_table_iter_remove(&it);
    }
}

void
ns_anim_rebase(ns_anim *a, gint64 base_us)
{
    if (!a) return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, a->states);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_anim_state *s = val;
        s->opacity_start_us   = base_us;
        s->transform_start_us = base_us;
        s->color.start_us     = base_us;
        s->bg.start_us        = base_us;
        s->anim_start_us      = base_us;
    }
}

static void
ns_anim_register_keyframes(ns_anim *a, const ns_css_keyframes *src)
{
    if (!a || !src || !src->name) return;
    ns_css_keyframes *copy = g_new0(ns_css_keyframes, 1);
    copy->name = g_strdup(src->name);
    copy->n_stops = src->n_stops;
    if (src->n_stops > 0) {
        copy->stops = g_new(ns_css_keyframe_stop, src->n_stops);
        memcpy(copy->stops, src->stops,
               src->n_stops * sizeof(ns_css_keyframe_stop));
        for (int i = 0; i < copy->n_stops; i++)
            copy->stops[i].raw_props = g_strdup(src->stops[i].raw_props);
    }
    g_hash_table_replace(a->keyframes, g_strdup(src->name), copy);
}

void
ns_anim_load_from_stylesheet(ns_anim *a, const ns_css_stylesheet *sh)
{
    if (!a || !sh || !sh->keyframes) return;
    for (guint i = 0; i < sh->keyframes->len; i++) {
        const ns_css_keyframes *kf =
            &g_array_index(sh->keyframes, ns_css_keyframes, i);
        ns_anim_register_keyframes(a, kf);
    }
}

static double
steps_apply(int n, ns_css_step_pos pos, double x)
{
    if (n < 1) n = 1;
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    int step = (int)floor(x * n);
    if (pos == NS_CSS_STEP_JUMP_START || pos == NS_CSS_STEP_JUMP_BOTH)
        step += 1;
    int jumps = n;
    if (pos == NS_CSS_STEP_JUMP_NONE) jumps = n > 1 ? n - 1 : 1;
    else if (pos == NS_CSS_STEP_JUMP_BOTH) jumps = n + 1;
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
timing_apply(ns_css_timing t, double x)
{
    if (t.kind == NS_CSS_TIMING_STEPS)
        return steps_apply(t.steps, t.step_pos, x);
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    switch (t.kind) {
    case NS_CSS_TIMING_LINEAR:      return x;
    case NS_CSS_TIMING_EASE_IN: {
        static const double cb[4] = { 0.42, 0.0, 1.0, 1.0 };
        return cubic_bezier_apply(cb, x);
    }
    case NS_CSS_TIMING_EASE_OUT: {
        static const double cb[4] = { 0.0, 0.0, 0.58, 1.0 };
        return cubic_bezier_apply(cb, x);
    }
    case NS_CSS_TIMING_EASE_IN_OUT: {
        static const double cb[4] = { 0.42, 0.0, 0.58, 1.0 };
        return cubic_bezier_apply(cb, x);
    }
    case NS_CSS_TIMING_CUBIC:       return cubic_bezier_apply(t.cb, x);
    case NS_CSS_TIMING_EASE:
    default: {
        static const double cb[4] = { 0.25, 0.1, 0.25, 1.0 };
        return cubic_bezier_apply(cb, x);
    }
    }
}

static gboolean
transforms_compatible(const ns_css_transform *a, const ns_css_transform *b)
{
    if (a->n_ops != b->n_ops) return FALSE;
    for (int i = 0; i < a->n_ops; i++)
        if (a->ops[i].kind != b->ops[i].kind) return FALSE;
    return TRUE;
}

static void
transform_lerp(const ns_css_transform *from, const ns_css_transform *to,
               double t, ns_css_transform *out)
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
        for (int k = 0; k < 16; k++)
            out->ops[i].m3d[k] = from->ops[i].m3d[k] +
                (to->ops[i].m3d[k] - from->ops[i].m3d[k]) * t;
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

static const ns_css_anim_entry *
find_entry(const ns_css_anim_list *list, ns_css_anim_target target)
{
    if (!list) return NULL;
    const ns_css_anim_entry *fallback = NULL;
    for (int i = 0; i < list->n; i++) {
        if (list->entries[i].target == target) return &list->entries[i];
        if (list->entries[i].target == NS_CSS_ANIM_TARGET_ALL && !fallback)
            fallback = &list->entries[i];
    }
    return fallback;
}

static ns_anim_state *
state_for(ns_anim *a, const ns_node *dom)
{
    ns_anim_state *s = g_hash_table_lookup(a->states, dom);
    if (s) return s;
    s = g_new0(ns_anim_state, 1);
    s->node = dom;
    g_hash_table_insert(a->states, (gpointer)dom, s);
    return s;
}

static gboolean
should_skip_motion(void)
{
    return ns_css_get_reduced_motion() == NS_CSS_REDUCED_MOTION_REDUCE;
}

static void
observe_color_chan(ns_anim *a, ns_anim_color_chan *ch, gboolean cur_has,
                   const guint8 cur[4], const ns_css_anim_entry *e,
                   gint64 now_us)
{
    if (!cur_has) return;
    if (e && e->duration_ms > 0 && ch->has_last &&
        colors_differ(ch->last, cur)) {
        if (ch->active || a->active_count < NS_ANIM_MAX_ACTIVE) {
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
value_to_rgba(const ns_css_value *v, guint8 out[4])
{
    if (!v || v->kind != NS_CSS_V_COLOR) return FALSE;
    out[0] = v->u.color.r;
    out[1] = v->u.color.g;
    out[2] = v->u.color.b;
    out[3] = v->u.color.a;
    return TRUE;
}

static void
observe_generic_chan(ns_anim *a, ns_anim_state *s, const ns_style *style,
                     const ns_css_anim_entry *e, gint64 now_us)
{
    int pid = ns_css_prop_id(e->name);
    if (pid < 0) return;
    char *cur = ns_css_value_serialize(style->values[pid]);
    if (!cur) cur = g_strdup("");
    if (!s->generic) s->generic = g_ptr_array_new();
    ns_anim_generic_chan *ch = NULL;
    for (guint i = 0; i < s->generic->len; i++) {
        ns_anim_generic_chan *c = s->generic->pdata[i];
        if (strcmp(c->prop, e->name) == 0) { ch = c; break; }
    }
    if (!ch) {
        ch = g_new0(ns_anim_generic_chan, 1);
        ch->prop = g_strdup(e->name);
        g_ptr_array_add(s->generic, ch);
    }
    if (e->duration_ms > 0 && ch->has_last && strcmp(ch->last, cur) != 0 &&
        (ch->active || a->active_count < NS_ANIM_MAX_ACTIVE)) {
        if (!ch->active) a->active_count++;
        ch->active = TRUE;
        ch->start_us = now_us;
        ch->duration_ms = e->duration_ms;
        ch->delay_ms = e->delay_ms;
    }
    g_free(ch->last);
    ch->last = cur;
    ch->has_last = TRUE;
}

static void
observe_transition(ns_anim *a, ns_anim_state *s, const ns_style *style,
                   gint64 now_us, const ns_node *dom)
{
    const ns_css_value *tv = style ? style->values[NS_CSS_TRANSITION] : NULL;
    if (!tv || tv->kind != NS_CSS_V_ANIM) return;
    if (should_skip_motion()) return;

    const ns_css_value *ov = style->values[NS_CSS_OPACITY];
    double cur_opacity = 1.0;
    if (ov && ov->kind == NS_CSS_V_LENGTH) cur_opacity = ov->u.length.v;
    if (cur_opacity < 0) cur_opacity = 0;
    if (cur_opacity > 1) cur_opacity = 1;

    const ns_css_anim_entry *op_e =
        find_entry(&tv->u.anim, NS_CSS_ANIM_TARGET_OPACITY);
    if (op_e && op_e->duration_ms > 0 && s->has_last_opacity &&
        fabs(cur_opacity - s->last_opacity) > 0.001) {
        if (s->opacity_active || a->active_count < NS_ANIM_MAX_ACTIVE) {
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
            if (dom && g_getenv("NS_ANIM_DEBUG")) {
                const char *id = ns_element_get_attr(dom, "id");
                const char *cl = ns_element_get_attr(dom, "class");
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

    const ns_css_value *cv = style->values[NS_CSS_TRANSFORM];
    ns_css_transform cur_tf = { 0 };
    gboolean cur_has_tf = FALSE;
    if (cv && cv->kind == NS_CSS_V_TRANSFORM) {
        cur_tf = cv->u.transform;
        cur_has_tf = TRUE;
    }
    const ns_css_anim_entry *tf_e =
        find_entry(&tv->u.anim, NS_CSS_ANIM_TARGET_TRANSFORM);
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
                        a->active_count < NS_ANIM_MAX_ACTIVE)) {
            ns_css_transform from = s->transform_active
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
            if (dom && g_getenv("NS_ANIM_DEBUG")) {
                const char *id = ns_element_get_attr(dom, "id");
                const char *cl = ns_element_get_attr(dom, "class");
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
    gboolean has_col = value_to_rgba(style->values[NS_CSS_COLOR], cur_col);
    observe_color_chan(a, &s->color, has_col, cur_col,
                       find_entry(&tv->u.anim, NS_CSS_ANIM_TARGET_COLOR),
                       now_us);

    guint8 cur_bg[4];
    gboolean has_bg = value_to_rgba(style->values[NS_CSS_BACKGROUND_COLOR],
                                    cur_bg);
    observe_color_chan(a, &s->bg, has_bg, cur_bg,
                       find_entry(&tv->u.anim, NS_CSS_ANIM_TARGET_BG_COLOR),
                       now_us);

    for (int i = 0; i < tv->u.anim.n; i++) {
        const ns_css_anim_entry *e = &tv->u.anim.entries[i];
        if (e->target == NS_CSS_ANIM_TARGET_OTHER && e->name)
            observe_generic_chan(a, s, style, e, now_us);
    }
}

static void
observe_animation(ns_anim *a, ns_anim_state *s, const ns_style *style,
                  gint64 now_us, const ns_node *dom)
{
    const ns_css_value *av = style ? style->values[NS_CSS_ANIMATION] : NULL;
    if (!av || av->kind != NS_CSS_V_ANIM || av->u.anim.n == 0) {
        if (s->anim_active) {
            s->anim_active = FALSE;
            if (a->active_count > 0) a->active_count--;
            g_free(s->anim_name);
            s->anim_name = NULL;
        }
        return;
    }
    if (should_skip_motion()) return;
    const ns_css_anim_entry *e = &av->u.anim.entries[0];
    if (!e->name || e->duration_ms <= 0) return;
    gboolean paused = e->paused;
    const ns_css_value *ps =
        style ? style->values[NS_CSS_ANIMATION_PLAY_STATE] : NULL;
    if (ps && ps->kind == NS_CSS_V_KEYWORD && ps->u.keyword) {
        if (strcmp(ps->u.keyword, "paused") == 0)       paused = TRUE;
        else if (strcmp(ps->u.keyword, "running") == 0) paused = FALSE;
    }
    if (s->anim_active && s->anim_name &&
        strcmp(s->anim_name, e->name) == 0 &&
        s->anim_duration_ms == e->duration_ms) {
        if (paused && !s->anim_paused) {
            double el = (now_us - s->anim_start_us) / 1000.0 - s->anim_delay_ms;
            s->anim_elapsed_base_ms = el > 0 ? el : 0;
            s->anim_paused = TRUE;
        } else if (!paused && s->anim_paused) {
            s->anim_start_us = now_us -
                (gint64)((s->anim_elapsed_base_ms + s->anim_delay_ms) * 1000.0);
            s->anim_paused = FALSE;
        }
        return;
    }
    if (!s->anim_active) {
        if (a->active_count >= NS_ANIM_MAX_ACTIVE) return;
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
    s->anim_paused = paused;
    s->anim_elapsed_base_ms = 0;
    ns_css_keyframes_resolved_free(s->anim_kf);
    const ns_css_keyframes *gkf = g_hash_table_lookup(a->keyframes, e->name);
    s->anim_kf = gkf ? ns_css_keyframes_resolve(gkf, style->vars) : NULL;
    s->anim_has_opacity_value = FALSE;
    s->anim_has_transform_value = FALSE;
    if (paused) advance_animation(a, s, now_us);
    if (dom && g_getenv("NS_ANIM_DEBUG")) {
        const char *id = ns_element_get_attr(dom, "id");
        const char *cl = ns_element_get_attr(dom, "class");
        g_printerr("[anim] keyframe animation '%s' <%s id=%s class=%s>"
                   " dur=%.0fms\n", e->name, dom->name ? dom->name : "?",
                   id ? id : "", cl ? cl : "", (double)e->duration_ms);
    }
}

void
ns_anim_observe(ns_anim *a, const ns_node *dom,
                const ns_style *style, gint64 now_us)
{
    if (!a || !dom || !style) return;
    ns_anim_state *s = g_hash_table_lookup(a->states, dom);
    if (!s) {
        const ns_css_value *tv = style->values[NS_CSS_TRANSITION];
        const ns_css_value *av = style->values[NS_CSS_ANIMATION];
        gboolean animatable =
            (tv && tv->kind == NS_CSS_V_ANIM) ||
            (av && av->kind == NS_CSS_V_ANIM && av->u.anim.n > 0);
        if (!animatable)
            return;
        s = state_for(a, dom);
    }
    observe_transition(a, s, style, now_us, dom);
    observe_animation(a, s, style, now_us, dom);
    anim_track(a, s);
}

static gboolean
advance_opacity(ns_anim *a, ns_anim_state *s, gint64 now_us)
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
advance_transform(ns_anim *a, ns_anim_state *s, gint64 now_us)
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
advance_color_chan(ns_anim *a, ns_anim_color_chan *ch, gint64 now_us)
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
keyframe_sample(const ns_css_keyframes *kf, double pct,
                gboolean *out_has_opacity, double *out_opacity,
                gboolean *out_has_transform, ns_css_transform *out_transform,
                gboolean *out_has_color, guint8 out_color[4],
                gboolean *out_has_bg, guint8 out_bg[4])
{
    *out_has_opacity = FALSE;
    *out_has_transform = FALSE;
    *out_has_color = FALSE;
    *out_has_bg = FALSE;
    if (!kf || kf->n_stops == 0) return;

    const ns_css_keyframe_stop *prev_op = NULL, *next_op = NULL;
    const ns_css_keyframe_stop *prev_tf = NULL, *next_tf = NULL;
    const ns_css_keyframe_stop *prev_col = NULL, *next_col = NULL;
    const ns_css_keyframe_stop *prev_bg = NULL, *next_bg = NULL;
    for (int i = 0; i < kf->n_stops; i++) {
        const ns_css_keyframe_stop *s = &kf->stops[i];
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
directed_progress(int iter, double raw, ns_css_anim_direction dir)
{
    gboolean rev;
    switch (dir) {
    case NS_CSS_ANIM_DIR_REVERSE:           rev = TRUE; break;
    case NS_CSS_ANIM_DIR_ALTERNATE:         rev = (iter & 1); break;
    case NS_CSS_ANIM_DIR_ALTERNATE_REVERSE: rev = !(iter & 1); break;
    case NS_CSS_ANIM_DIR_NORMAL:
    default:                                rev = FALSE; break;
    }
    return rev ? 1.0 - raw : raw;
}

static void
anim_clear_values(ns_anim_state *s)
{
    s->anim_has_opacity_value = FALSE;
    s->anim_has_transform_value = FALSE;
    s->anim_has_color_value = FALSE;
    s->anim_has_bg_value = FALSE;
}

static void
anim_sample_at(ns_anim_state *s, ns_css_keyframes *kf, double progress)
{
    double pct = timing_apply(s->anim_timing, progress) * 100.0;
    keyframe_sample(kf, pct,
                    &s->anim_has_opacity_value, &s->anim_opacity_value,
                    &s->anim_has_transform_value, &s->anim_transform_value,
                    &s->anim_has_color_value, s->anim_color_value,
                    &s->anim_has_bg_value, s->anim_bg_value);
}

static gboolean
advance_animation(ns_anim *a, ns_anim_state *s, gint64 now_us)
{
    if (!s->anim_name) return FALSE;
    ns_css_keyframes *kf = s->anim_kf
        ? s->anim_kf : g_hash_table_lookup(a->keyframes, s->anim_name);
    gboolean have_kf = kf && kf->n_stops > 0;
    double cycle_ms = s->anim_duration_ms > 0 ? s->anim_duration_ms : 1.0;
    double elapsed = s->anim_paused
        ? s->anim_elapsed_base_ms
        : (now_us - s->anim_start_us) / 1000.0 - s->anim_delay_ms;

    if (elapsed < 0) {
        gboolean fill_back = s->anim_fill == NS_CSS_ANIM_FILL_BACKWARDS ||
                             s->anim_fill == NS_CSS_ANIM_FILL_BOTH;
        if (!fill_back) { anim_clear_values(s); return FALSE; }
        if (have_kf)
            anim_sample_at(s, kf, directed_progress(0, 0.0, s->anim_direction));
        return TRUE;
    }

    double iter_d = elapsed / cycle_ms;
    if (iter_d > 1e9) iter_d = 1e9;
    int iter = (int)iter_d;
    if (s->anim_iter_count > 0 && iter >= s->anim_iter_count) {
        if (s->anim_active) {
            s->anim_active = FALSE;
            if (a->active_count > 0) a->active_count--;
        }
        gboolean fill_fwd = s->anim_fill == NS_CSS_ANIM_FILL_FORWARDS ||
                            s->anim_fill == NS_CSS_ANIM_FILL_BOTH;
        if (!fill_fwd) { anim_clear_values(s); return TRUE; }
        int last = s->anim_iter_count - 1;
        if (have_kf)
            anim_sample_at(s, kf, directed_progress(last, 1.0, s->anim_direction));
        return TRUE;
    }

    double raw = fmod(elapsed, cycle_ms) / cycle_ms;
    if (have_kf)
        anim_sample_at(s, kf, directed_progress(iter, raw, s->anim_direction));
    return TRUE;
}

gboolean
ns_anim_tick(ns_anim *a, gint64 now_us)
{
    if (!a) return FALSE;
    if (g_hash_table_size(a->active) == 0) return FALSE;
    gboolean any = FALSE;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, a->active);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_anim_state *s = key;
        gboolean op0 = s->opacity_active, tr0 = s->transform_active;
        gboolean co0 = s->color.active, bg0 = s->bg.active, an0 = s->anim_active;
        if (s->opacity_active && advance_opacity(a, s, now_us)) any = TRUE;
        if (s->transform_active && advance_transform(a, s, now_us)) any = TRUE;
        if (s->color.active && advance_color_chan(a, &s->color, now_us)) any = TRUE;
        if (s->bg.active && advance_color_chan(a, &s->bg, now_us)) any = TRUE;
        if (s->anim_active && advance_animation(a, s, now_us)) any = TRUE;
        if (op0 && !s->opacity_active)
            anim_emit(a, s->node, "transitionend", "opacity",
                      s->opacity_duration_ms);
        if (tr0 && !s->transform_active)
            anim_emit(a, s->node, "transitionend", "transform",
                      s->transform_duration_ms);
        if (co0 && !s->color.active)
            anim_emit(a, s->node, "transitionend", "color",
                      s->color.duration_ms);
        if (bg0 && !s->bg.active)
            anim_emit(a, s->node, "transitionend", "background-color",
                      s->bg.duration_ms);
        if (an0) {
            double el = s->anim_paused
                ? s->anim_elapsed_base_ms
                : (now_us - s->anim_start_us) / 1000.0 - s->anim_delay_ms;
            if (!s->anim_started && el >= 0) {
                s->anim_started = TRUE;
                s->anim_iters_emitted = 0;
                anim_emit(a, s->node, "animationstart", s->anim_name, 0.0);
            }
            if (s->anim_started && s->anim_duration_ms > 0) {
                int reached = (int)(el / s->anim_duration_ms);
                if (s->anim_iter_count > 0 && reached > s->anim_iter_count - 1)
                    reached = s->anim_iter_count - 1;
                while (s->anim_iters_emitted < reached) {
                    s->anim_iters_emitted++;
                    anim_emit(a, s->node, "animationiteration", s->anim_name,
                              s->anim_iters_emitted * s->anim_duration_ms);
                }
            }
            if (!s->anim_active) {
                anim_emit(a, s->node, "animationend", s->anim_name,
                          s->anim_duration_ms *
                          (s->anim_iter_count > 0 ? s->anim_iter_count : 1));
                s->anim_started = FALSE;
                s->anim_iters_emitted = 0;
            }
        }
        if (s->generic) {
            for (guint gi = 0; gi < s->generic->len; gi++) {
                ns_anim_generic_chan *ch = s->generic->pdata[gi];
                if (!ch->active) continue;
                double el = (now_us - ch->start_us) / 1000.0 - ch->delay_ms;
                if (el >= ch->duration_ms) {
                    ch->active = FALSE;
                    if (a->active_count > 0) a->active_count--;
                    anim_emit(a, s->node, "transitionend", ch->prop,
                              ch->duration_ms);
                    any = TRUE;
                }
            }
        }
        if (!state_is_active(s)) g_hash_table_iter_remove(&it);
    }
    return any;
}

gboolean
ns_anim_has_active(const ns_anim *a)
{
    return a && a->active && g_hash_table_size(a->active) > 0;
}

gboolean
ns_anim_get_opacity(ns_anim *a, const ns_node *dom, double *out_opacity)
{
    if (!a || !dom || !out_opacity) return FALSE;
    ns_anim_state *s = g_hash_table_lookup(a->states, dom);
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

const ns_css_transform *
ns_anim_get_transform(ns_anim *a, const ns_node *dom)
{
    if (!a || !dom) return NULL;
    ns_anim_state *s = g_hash_table_lookup(a->states, dom);
    if (!s) return NULL;
    if (s->anim_active && s->anim_has_transform_value)
        return &s->anim_transform_value;
    if (s->transform_active && s->transform_current.n_ops > 0)
        return &s->transform_current;
    return NULL;
}

gboolean
ns_anim_get_color(ns_anim *a, const ns_node *dom,
                  ns_css_anim_target which, guint8 out_rgba[4])
{
    if (!a || !dom || !out_rgba) return FALSE;
    ns_anim_state *s = g_hash_table_lookup(a->states, dom);
    if (!s) return FALSE;
    if (which == NS_CSS_ANIM_TARGET_COLOR) {
        if (s->anim_active && s->anim_has_color_value) {
            memcpy(out_rgba, s->anim_color_value, 4);
            return TRUE;
        }
        if (s->color.active) {
            memcpy(out_rgba, s->color.current, 4);
            return TRUE;
        }
    } else if (which == NS_CSS_ANIM_TARGET_BG_COLOR) {
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
