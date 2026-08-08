/* Nordstjernen — headless engine driver for scripting / regression testing.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_HEADLESS_H
#define NS_HEADLESS_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum ns_headless_dump {
    NS_DUMP_TEXT,
    NS_DUMP_DOM,
    NS_DUMP_LAYOUT,
    NS_DUMP_PNG,
    NS_DUMP_PDF,
    NS_DUMP_PRINT,
    NS_DUMP_NONE,
} ns_headless_dump;

typedef struct ns_headless_opts {
    const char       *url;
    ns_headless_dump  dump;
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
    gboolean          wpt;
    int               wpt_timeout_ms;
} ns_headless_opts;

unsigned ns_headless_debug_mask(const char *spec);

int ns_headless_run(const ns_headless_opts *opts);

G_END_DECLS

#endif
