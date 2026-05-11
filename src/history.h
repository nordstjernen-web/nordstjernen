/* Nordstjernen — persistent visit history API. */

#ifndef ND_HISTORY_H
#define ND_HISTORY_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct nd_history nd_history;
typedef struct nd_history_entry {
    char  *url;
    char  *title;
    gint64 last_visit;
} nd_history_entry;

#define ND_HISTORY_MAX 2000

nd_history *nd_history_load(void);
void        nd_history_free(nd_history *h);

void  nd_history_visit(nd_history *h, const char *url, const char *title);
guint nd_history_count(const nd_history *h);
const nd_history_entry *nd_history_get(const nd_history *h, guint i);

GPtrArray *nd_history_search(const nd_history *h, const char *needle, guint limit);

G_END_DECLS

#endif
