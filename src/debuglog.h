/* Nordstjernen — in-process debug event log shared with the JS console.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_DEBUGLOG_H
#define ND_DEBUGLOG_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum nd_dlog_level {
    ND_DLOG_INFO,
    ND_DLOG_WARN,
    ND_DLOG_ERROR,
    ND_DLOG_RENDER,
    ND_DLOG_NET,
    ND_DLOG_JS,
} nd_dlog_level;

typedef struct nd_dlog_entry {
    gint64        monotonic_us;
    nd_dlog_level level;
    char         *category;
    char         *message;
} nd_dlog_entry;

typedef void (*nd_dlog_listener)(const nd_dlog_entry *entry, gpointer user_data);

void nd_debug_log_init(void);

void nd_debug_log_emit(nd_dlog_level level, const char *category,
                       const char *fmt, ...) G_GNUC_PRINTF(3, 4);

guint nd_debug_log_subscribe(nd_dlog_listener cb, gpointer user_data);
void  nd_debug_log_unsubscribe(guint id);

void  nd_debug_log_snapshot(GFunc visit, gpointer user_data);

const char *nd_dlog_level_name(nd_dlog_level lvl);

G_END_DECLS

#endif
