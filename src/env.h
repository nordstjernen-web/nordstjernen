/* Nordstjernen — runtime environment info shared by the JS console and about: page.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_ENV_H
#define ND_ENV_H

#include <glib.h>

#include "html.h"

G_BEGIN_DECLS

typedef void (*nd_env_emit_fn)(const char *label, const char *value,
                               gpointer user_data);

void nd_env_each(nd_env_emit_fn emit, gpointer user_data);

G_END_DECLS

#endif
