/* Nordstjernen — startup security: refuse-root + landlock sandbox. */

#ifndef ND_SECURITY_H
#define ND_SECURITY_H

#include <glib.h>

G_BEGIN_DECLS

gboolean nd_security_refuse_root(void);

void nd_security_sandbox_init(const char *self_exe);

G_END_DECLS

#endif
