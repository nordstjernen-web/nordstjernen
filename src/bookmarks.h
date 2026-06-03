/* Nordstjernen — bookmarks storage API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_BOOKMARKS_H
#define ND_BOOKMARKS_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct nd_bookmark {
    char *url;
    char *title;
} nd_bookmark;

typedef struct nd_bookmarks nd_bookmarks;

nd_bookmarks *nd_bookmarks_load(void);
void          nd_bookmarks_free(nd_bookmarks *bm);

guint    nd_bookmarks_count(const nd_bookmarks *bm);
const nd_bookmark *nd_bookmarks_get(const nd_bookmarks *bm, guint i);

gboolean nd_bookmarks_contains(const nd_bookmarks *bm, const char *url);
void     nd_bookmarks_add(nd_bookmarks *bm, const char *url, const char *title);
void     nd_bookmarks_remove(nd_bookmarks *bm, const char *url);

G_END_DECLS

#endif
