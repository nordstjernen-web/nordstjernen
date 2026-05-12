/* Nordstjernen — google.com compatibility helpers. */

#ifndef ND_GOOGLE_H
#define ND_GOOGLE_H

#include <glib.h>

#include "dom.h"

G_BEGIN_DECLS

gboolean nd_google_host_is_google(const char *host);

char *nd_google_unwrap_consent_url(const char *url);

char *nd_google_unwrap_redirect_href(const char *href);

void nd_google_rewrite_doc(nd_node *root);

void nd_google_rewrite_if_google_host(nd_node *root, const char *page_url);

G_END_DECLS

#endif
