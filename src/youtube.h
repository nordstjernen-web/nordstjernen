/* Nordstjernen — YouTube watch page interception.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#ifndef ND_YOUTUBE_H
#define ND_YOUTUBE_H

#include <glib.h>

G_BEGIN_DECLS

gboolean nd_youtube_is_watch_url(const char *url);

gboolean nd_youtube_host_needs_browser_ua(const char *host);

const char *nd_youtube_browser_user_agent(void);

char *nd_youtube_render_watch_page(const char *url,
                                   const char *body,
                                   gsize body_len);

G_END_DECLS

#endif
