/* Nordstjernen — startup security: refuse-root + landlock + seccomp sandbox.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_SECURITY_H
#define ND_SECURITY_H

#include <glib.h>

G_BEGIN_DECLS

gboolean nd_security_refuse_root(void);

void nd_security_sandbox_init(const char *self_exe);

void nd_security_add_writable_dir(const char *dir);

void nd_security_seccomp_init(void);

void nd_security_win32_mitigations_init(void);

gboolean nd_security_csprng_fill(void *buf, gsize len);

gboolean nd_security_sri_check(const char *integrity_attr,
                               const void *body,
                               gsize       body_len);

G_END_DECLS

#endif
