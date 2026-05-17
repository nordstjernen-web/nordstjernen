/* Nordstjernen — CSS transitions and @keyframes animation engine.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "anim.h"

#include <math.h>
#include <string.h>

#define ND_ANIM_MAX_ACTIVE 64

typedef struct nd_anim_state {
    gboolean has_last_opacity;
    double   last_opacity;
    gboolean has_last_transform;
    nd_css_transform last_transform;

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
    nd_css_timing anim_timing;
    gboolean anim_has_opacity_value;
    double   anim_opacity_value;
    gboolean anim_has_transform_value;
    nd_css_transform anim_transform_value;
} nd_anim_state;

struct nd_anim {
    GHashTable *states;
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

nd_anim *
nd_anim_new(void)
{
    nd_anim *a = g_new0(nd_anim, 1);
    a->states    = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                         NULL, nd_anim_state_free);
    a->keyframes = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free, nd_anim_keyframes_free);
    return a;
}

void
nd_anim_free(nd_anim *a)
{
    if (!a) return;
    g_hash_table_destroy(a->states);
    g_hash_table_destroy(a->keyframes);
    g_free(a);
}

void
nd_anim_reset(nd_anim *a)
{
    if (!a) return;
    g_hash_table_remove_all(a->states);
    g_hash_table_remove_all(a->keyframes);
    a->active_count = 0;
}

void
nd_anim_drop_node(nd_anim *a, const nd_node *dom)
{
    if (!a || !dom) return;
    g_hash_table_remove(a->states, dom);
}

void
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
timing_apply(nd_css_timing t, double x)
{
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    switch (t) {
    case ND_CSS_TIMING_LINEAR:      return x;
    case ND_CSS_TIMING_EASE_IN:     return x * x;
    case ND_CSS_TIMING_EASE_OUT:    return 1 - (1 - x) * (1 - x);
    case ND_CSS_TIMING_EASE_IN_OUT:
        return x < 0.5 ? 2 * x * x : 1 - 2 * (1 - x) * (1 - x);
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
        out->ops[i].a_is_percent = from->ops[i].a_is_percent;
        out->ops[i].b_is_percent = from->ops[i].b_is_percent;
    }
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
observe_transition(nd_anim *a, nd_anim_state *s, const nd_style *style,
                   gint64 now_us)
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
        }
    }
    if (cur_has_tf) {
        s->last_transform = cur_tf;
        s->has_last_transform = TRUE;
    }
}

static void
observe_animation(nd_anim *a, nd_anim_state *s, const nd_style *style,
                  gint64 now_us)
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
    s->anim_timing = e->timing;
    s->anim_active = TRUE;
    s->anim_has_opacity_value = FALSE;
    s->anim_has_transform_value = FALSE;
}

void
nd_anim_observe(nd_anim *a, const nd_node *dom,
                const nd_style *style, gint64 now_us)
{
    if (!a || !dom || !style) return;
    nd_anim_state *s = state_for(a, dom);
    observe_transition(a, s, style, now_us);
    observe_animation(a, s, style, now_us);
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

static void
keyframe_sample(const nd_css_keyframes *kf, double pct,
                gboolean *out_has_opacity, double *out_opacity,
                gboolean *out_has_transform, nd_css_transform *out_transform)
{
    *out_has_opacity = FALSE;
    *out_has_transform = FALSE;
    if (!kf || kf->n_stops == 0) return;

    const nd_css_keyframe_stop *prev_op = NULL, *next_op = NULL;
    const nd_css_keyframe_stop *prev_tf = NULL, *next_tf = NULL;
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
}

static gboolean
advance_animation(nd_anim *a, nd_anim_state *s, gint64 now_us)
{
    if (!s->anim_name) return FALSE;
    nd_css_keyframes *kf = g_hash_table_lookup(a->keyframes, s->anim_name);
    if (!kf || kf->n_stops == 0) return FALSE;
    double elapsed = (now_us - s->anim_start_us) / 1000.0 - s->anim_delay_ms;
    if (elapsed < 0) elapsed = 0;
    double cycle_ms = s->anim_duration_ms;
    int finished_cycles = (int)(elapsed / cycle_ms);
    if (s->anim_iter_count > 0 && finished_cycles >= s->anim_iter_count) {
        if (s->anim_active) {
            s->anim_active = FALSE;
            if (a->active_count > 0) a->active_count--;
        }
        double last_t = 1.0;
        last_t = timing_apply(s->anim_timing, last_t);
        double pct = last_t * 100.0;
        keyframe_sample(kf, pct,
                        &s->anim_has_opacity_value, &s->anim_opacity_value,
                        &s->anim_has_transform_value, &s->anim_transform_value);
        return TRUE;
    }
    double phase = fmod(elapsed, cycle_ms) / cycle_ms;
    phase = timing_apply(s->anim_timing, phase);
    double pct = phase * 100.0;
    keyframe_sample(kf, pct,
                    &s->anim_has_opacity_value, &s->anim_opacity_value,
                    &s->anim_has_transform_value, &s->anim_transform_value);
    return TRUE;
}

gboolean
nd_anim_tick(nd_anim *a, gint64 now_us)
{
    if (!a) return FALSE;
    if (a->active_count == 0) return FALSE;
    gboolean any = FALSE;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, a->states);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        nd_anim_state *s = val;
        if (s->opacity_active && advance_opacity(a, s, now_us)) any = TRUE;
        if (s->transform_active && advance_transform(a, s, now_us)) any = TRUE;
        if (s->anim_active && advance_animation(a, s, now_us)) any = TRUE;
    }
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
