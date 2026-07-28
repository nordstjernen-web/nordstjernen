/* Nordstjernen — WebExtensions loader and content-script host.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_EXT_H
#define NS_EXT_H

#include <glib.h>
#include <quickjs.h>

G_BEGIN_DECLS

char    *ns_ext_content_scripts_for_url(JSContext *ctx, JSValueConst global,
                                        const char *url, gboolean at_start);
gboolean ns_ext_should_block(const char *url, const char *initiator);

G_END_DECLS

#endif
