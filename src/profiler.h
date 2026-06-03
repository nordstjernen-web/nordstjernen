/* Nordstjernen — gdb-driven sampling profiler for the JS console.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_PROFILER_H
#define ND_PROFILER_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct nd_profile_row {
    char *function;
    guint hits;
} nd_profile_row;

typedef struct nd_profile_result {
    guint           samples_taken;
    guint           samples_requested;
    guint           thread_snapshots;
    guint           interval_ms;
    gint64          wall_us;
    gboolean        ok;
    char           *error_message;
    GArray         *top_rows;
    GArray         *leaf_rows;
} nd_profile_result;

typedef void (*nd_profile_progress_fn)(guint done, guint total, gpointer user_data);
typedef void (*nd_profile_done_fn)    (const nd_profile_result *result,
                                       gpointer user_data);

gboolean nd_profiler_supported(void);

gboolean nd_profiler_run_async(guint samples, guint interval_ms,
                               nd_profile_progress_fn progress,
                               nd_profile_done_fn done,
                               gpointer user_data);

G_END_DECLS

#endif
