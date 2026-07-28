/* Nordstjernen — set the macOS Dock icon when running unbundled.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */
#import <AppKit/AppKit.h>

#include <cairo.h>
#include <glib.h>

#include "macos_dock.h"
#include "svg.h"

#define NS_DOCK_ICON_DIM 512

void
ns_macos_set_dock_icon(void)
{
    GBytes *bytes = g_resources_lookup_data(
        "/org/nordstjernen/WebBrowser/icons/scalable/apps/nordstjernen.svg",
        G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
    if (!bytes)
        return;

    cairo_surface_t *surf = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, NS_DOCK_ICON_DIM, NS_DOCK_ICON_DIM);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        g_bytes_unref(bytes);
        return;
    }

    gsize len = 0;
    const guint8 *data = g_bytes_get_data(bytes, &len);
    cairo_t *cr = cairo_create(surf);
    gboolean rendered = ns_svg_render_bytes(cr, data, len,
                                            NS_DOCK_ICON_DIM, NS_DOCK_ICON_DIM);
    cairo_destroy(cr);
    g_bytes_unref(bytes);
    if (!rendered) {
        cairo_surface_destroy(surf);
        return;
    }
    cairo_surface_flush(surf);

    char *png = g_build_filename(g_get_tmp_dir(), "nordstjernen-dock.png", NULL);
    cairo_status_t st = cairo_surface_write_to_png(surf, png);
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) {
        g_free(png);
        return;
    }

    @autoreleasepool {
        NSString *path = [NSString stringWithUTF8String:png];
        NSImage *image = [[NSImage alloc] initWithContentsOfFile:path];
        if (image)
            [NSApplication sharedApplication].applicationIconImage = image;
    }
    g_free(png);
}
