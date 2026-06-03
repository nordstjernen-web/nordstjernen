/* Nordstjernen — in-process debug event log shared with the JS console.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "debuglog.h"

#include <stdio.h>
#include <string.h>

#define ND_DLOG_CAPACITY 1024

typedef struct nd_dlog_sub {
    guint              id;
    nd_dlog_listener   cb;
    gpointer           user_data;
} nd_dlog_sub;

static GMutex   g_dlog_mutex;
static GQueue  *g_dlog_entries;
static GArray  *g_dlog_subs;
static guint    g_dlog_next_id = 1;
static gboolean g_dlog_inited;

static void
nd_dlog_entry_free(gpointer p)
{
    nd_dlog_entry *e = p;
    if (!e) return;
    g_free(e->category);
    g_free(e->message);
    g_free(e);
}

void
nd_debug_log_init(void)
{
    g_mutex_lock(&g_dlog_mutex);
    if (!g_dlog_inited) {
        g_dlog_entries = g_queue_new();
        g_dlog_subs = g_array_new(FALSE, FALSE, sizeof(nd_dlog_sub));
        g_dlog_inited = TRUE;
    }
    g_mutex_unlock(&g_dlog_mutex);
}

const char *
nd_dlog_level_name(nd_dlog_level lvl)
{
    switch (lvl) {
    case ND_DLOG_INFO:   return "info";
    case ND_DLOG_WARN:   return "warn";
    case ND_DLOG_ERROR:  return "error";
    case ND_DLOG_RENDER: return "render";
    case ND_DLOG_NET:    return "net";
    case ND_DLOG_JS:     return "js";
    }
    return "?";
}

static void
nd_dlog_dispatch(const nd_dlog_entry *snap)
{
    GArray *subs_copy = NULL;
    g_mutex_lock(&g_dlog_mutex);
    if (g_dlog_subs && g_dlog_subs->len > 0) {
        subs_copy = g_array_sized_new(FALSE, FALSE, sizeof(nd_dlog_sub),
                                      g_dlog_subs->len);
        g_array_append_vals(subs_copy, g_dlog_subs->data, g_dlog_subs->len);
    }
    g_mutex_unlock(&g_dlog_mutex);
    if (!subs_copy) return;
    for (guint i = 0; i < subs_copy->len; i++) {
        nd_dlog_sub s = g_array_index(subs_copy, nd_dlog_sub, i);
        if (s.cb) s.cb(snap, s.user_data);
    }
    g_array_free(subs_copy, TRUE);
}

G_GNUC_PRINTF(3, 0)
static void
nd_debug_log_emit_v(nd_dlog_level level, const char *category,
                    const char *fmt, va_list ap)
{
    if (!g_dlog_inited) nd_debug_log_init();
    nd_dlog_entry *e = g_new0(nd_dlog_entry, 1);
    e->monotonic_us = g_get_monotonic_time();
    e->level = level;
    e->category = g_strdup(category ? category : "");
    e->message = fmt ? g_strdup_vprintf(fmt, ap) : g_strdup("");

    nd_dlog_dispatch(e);

    g_mutex_lock(&g_dlog_mutex);
    g_queue_push_tail(g_dlog_entries, e);
    while (g_queue_get_length(g_dlog_entries) > ND_DLOG_CAPACITY) {
        nd_dlog_entry *drop = g_queue_pop_head(g_dlog_entries);
        nd_dlog_entry_free(drop);
    }
    g_mutex_unlock(&g_dlog_mutex);
}

void
nd_debug_log_emit(nd_dlog_level level, const char *category,
                  const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nd_debug_log_emit_v(level, category, fmt, ap);
    va_end(ap);
}

guint
nd_debug_log_subscribe(nd_dlog_listener cb, gpointer user_data)
{
    if (!cb) return 0;
    if (!g_dlog_inited) nd_debug_log_init();
    g_mutex_lock(&g_dlog_mutex);
    nd_dlog_sub s = { g_dlog_next_id++, cb, user_data };
    g_array_append_val(g_dlog_subs, s);
    guint id = s.id;
    g_mutex_unlock(&g_dlog_mutex);
    return id;
}

void
nd_debug_log_unsubscribe(guint id)
{
    if (id == 0 || !g_dlog_inited) return;
    g_mutex_lock(&g_dlog_mutex);
    for (guint i = 0; i < g_dlog_subs->len; i++) {
        if (g_array_index(g_dlog_subs, nd_dlog_sub, i).id == id) {
            g_array_remove_index(g_dlog_subs, i);
            break;
        }
    }
    g_mutex_unlock(&g_dlog_mutex);
}

void
nd_debug_log_snapshot(GFunc visit, gpointer user_data)
{
    if (!visit || !g_dlog_inited) return;
    GPtrArray *copy = g_ptr_array_new();
    g_mutex_lock(&g_dlog_mutex);
    for (GList *l = g_dlog_entries->head; l; l = l->next) {
        nd_dlog_entry *src = l->data;
        nd_dlog_entry *e = g_new0(nd_dlog_entry, 1);
        e->monotonic_us = src->monotonic_us;
        e->level        = src->level;
        e->category     = g_strdup(src->category ? src->category : "");
        e->message      = g_strdup(src->message  ? src->message  : "");
        g_ptr_array_add(copy, e);
    }
    g_mutex_unlock(&g_dlog_mutex);
    for (guint i = 0; i < copy->len; i++)
        visit(copy->pdata[i], user_data);
    for (guint i = 0; i < copy->len; i++)
        nd_dlog_entry_free(copy->pdata[i]);
    g_ptr_array_free(copy, TRUE);
}
