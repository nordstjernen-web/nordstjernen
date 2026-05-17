/* Nordstjernen — CSS transitions and @keyframes animation engine.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#ifndef ND_ANIM_H
#define ND_ANIM_H

#include <glib.h>

#include "css.h"
#include "dom.h"

G_BEGIN_DECLS

typedef struct nd_anim nd_anim;

nd_anim *nd_anim_new(void);
void     nd_anim_free(nd_anim *a);
void     nd_anim_reset(nd_anim *a);

void     nd_anim_register_keyframes(nd_anim *a, const nd_css_keyframes *kf);
void     nd_anim_load_from_stylesheet(nd_anim *a, const nd_css_stylesheet *sh);

void     nd_anim_observe(nd_anim *a, const nd_node *dom,
                         const nd_style *style, gint64 now_us);

gboolean nd_anim_tick(nd_anim *a, gint64 now_us);

gboolean                 nd_anim_get_opacity   (nd_anim *a,
                                                const nd_node *dom,
                                                double *out_opacity);
const nd_css_transform  *nd_anim_get_transform (nd_anim *a,
                                                const nd_node *dom);

void     nd_anim_drop_node(nd_anim *a, const nd_node *dom);

G_END_DECLS

#endif
