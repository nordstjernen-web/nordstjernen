/* Nordstjernen — headless engine driver for scripting / regression testing.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_HEADLESS_H
#define ND_HEADLESS_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum nd_headless_dump {
    ND_DUMP_TEXT,
    ND_DUMP_DOM,
    ND_DUMP_LAYOUT,
    ND_DUMP_PNG,
    ND_DUMP_PDF,
    ND_DUMP_NONE,
} nd_headless_dump;

typedef struct nd_headless_opts {
    const char       *url;
    nd_headless_dump  dump;
    const char       *out_path;
    int               viewport_width;
    int               viewport_height;
    int               settle_ms;
    int               time_ms;
    unsigned          debug_levels;
    const char       *actions;
    const char       *eval;
    const char       *inspect;
    const char       *inspect_at;
} nd_headless_opts;

unsigned nd_headless_debug_mask(const char *spec);

int nd_headless_run(const nd_headless_opts *opts);

G_END_DECLS

#endif
