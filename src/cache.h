/* Nordstjernen — on-disk HTTP cache API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_CACHE_H
#define NS_CACHE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct ns_cache_entry {
    char       *final_url;
    long        status;
    char       *content_type;
    char       *cors_allow_origin;
    char       *etag;
    char       *last_modified;
    gint64      expires_at;
    gint64      fetched_at;
    GByteArray *body;
} ns_cache_entry;

void   ns_cache_init(void);
void   ns_cache_shutdown(void);
void   ns_cache_clear(void);

ns_cache_entry *ns_cache_get(const char *url, const char *partition,
                             const char *const *request_headers);
gboolean        ns_cache_is_fresh(const ns_cache_entry *e);
void   ns_cache_entry_free(ns_cache_entry *e);

void   ns_cache_put(const char *url,
                    const char *partition,
                    const char *final_url,
                    long status,
                    const char *content_type,
                    const char *cors_allow_origin,
                    const char *etag,
                    const char *last_modified,
                    const char *cache_control,
                    const char *expires_header,
                    const char *vary,
                    const char *const *request_headers,
                    const void *body, gsize body_len);

void   ns_cache_promote_304(const char *url,
                            const char *partition,
                            const char *const *request_headers,
                            const char *cache_control,
                            const char *expires_header);


G_END_DECLS

#endif
