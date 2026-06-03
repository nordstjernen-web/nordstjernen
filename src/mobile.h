/* Nordstjernen - force the mobile variant of select sites. */

#ifndef ND_MOBILE_H
#define ND_MOBILE_H

#include <glib.h>

G_BEGIN_DECLS

const char *nd_mobile_user_agent(void);

gboolean nd_mobile_force_host(const char *host);

char *nd_mobile_rewrite_url(const char *url);

G_END_DECLS

#endif
