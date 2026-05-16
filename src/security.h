/* Nordstjernen — startup security: refuse-root + landlock + seccomp sandbox.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#ifndef ND_SECURITY_H
#define ND_SECURITY_H

#include <glib.h>

G_BEGIN_DECLS

gboolean nd_security_refuse_root(void);

void nd_security_sandbox_init(const char *self_exe);

void nd_security_seccomp_init(void);

gboolean nd_security_csprng_fill(void *buf, gsize len);

G_END_DECLS

#endif
