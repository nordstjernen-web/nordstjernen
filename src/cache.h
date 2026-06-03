/* Nordstjernen — on-disk HTTP cache API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_CACHE_H
#define ND_CACHE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct nd_cache_entry {
    char       *final_url;
    long        status;
    char       *content_type;
    char       *cors_allow_origin;
    char       *etag;
    char       *last_modified;
    gint64      expires_at;
    gint64      fetched_at;
    GByteArray *body;
} nd_cache_entry;

void   nd_cache_init(void);
void   nd_cache_shutdown(void);

nd_cache_entry *nd_cache_get(const char *url, const char *partition);
gboolean        nd_cache_is_fresh(const nd_cache_entry *e);
void   nd_cache_entry_free(nd_cache_entry *e);

void   nd_cache_put(const char *url,
                    const char *partition,
                    const char *final_url,
                    long status,
                    const char *content_type,
                    const char *cors_allow_origin,
                    const char *etag,
                    const char *last_modified,
                    const char *cache_control,
                    const char *expires_header,
                    const void *body, gsize body_len);

void   nd_cache_promote_304(const char *url,
                            const char *partition,
                            const char *cache_control,
                            const char *expires_header);

void   nd_cache_clear(void);

G_END_DECLS

#endif
