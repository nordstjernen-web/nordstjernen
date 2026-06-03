/* Nordstjernen — @font-face web font loader.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_FONT_H
#define ND_FONT_H

#include <glib.h>

G_BEGIN_DECLS

typedef void (*nd_font_loaded_cb)(const char *family, gpointer user_data);

void     nd_font_init(void);
void     nd_font_shutdown(void);

gboolean nd_font_available(void);

void     nd_font_set_loaded_cb(nd_font_loaded_cb cb, gpointer user_data);

void     nd_font_request(const char *family, const char *src_url,
                         const char *base_url);

G_END_DECLS

#endif
