/* Nordstjernen — decoded-image texture abstraction.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_TEXTURE_H
#define ND_TEXTURE_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    ND_TEXTURE_BGRA_PREMULTIPLIED = 0,
    ND_TEXTURE_DEFAULT = 1,
} nd_texture_format;

#ifdef __ANDROID__
typedef struct nd_texture nd_texture;
#else
#include <gdk/gdk.h>
typedef GdkTexture nd_texture;
#endif

nd_texture *nd_texture_new(int width, int height, nd_texture_format format,
                           GBytes *bytes, gsize stride);

nd_texture *nd_texture_ref(nd_texture *texture);
void        nd_texture_unref(nd_texture *texture);
void        nd_texture_clear(nd_texture **texture);

int  nd_texture_get_width(nd_texture *texture);
int  nd_texture_get_height(nd_texture *texture);

void nd_texture_download(nd_texture *texture, guchar *dst, gsize dst_stride);

G_END_DECLS

#endif
