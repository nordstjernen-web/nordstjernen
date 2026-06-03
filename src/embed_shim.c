/* Nordstjernen — app-level hooks the engine expects, stubbed for embedding.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include <glib.h>

const char *nd_app_self_exe(void);

const char *
nd_app_self_exe(void)
{
    return NULL;
}
