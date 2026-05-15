/* Nordstjernen — @font-face web font loader.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "font.h"

#include <gio/gio.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <string.h>

#include "net.h"

#ifdef ND_HAVE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif
#ifdef ND_HAVE_PANGOFT2
#include <pango/pangofc-fontmap.h>
#define ND_HAVE_PANGOFC 1
#endif

typedef struct nd_font_entry {
    char *family;
    char *url;
    gboolean loaded;
    gboolean inflight;
    GCancellable *cancel;
} nd_font_entry;

static GHashTable        *g_entries;
static char              *g_cache_dir;
static nd_font_loaded_cb  g_loaded_cb;
static gpointer           g_loaded_ud;

void
nd_font_init(void)
{
    if (g_entries) return;
    g_entries = g_hash_table_new(g_str_hash, g_str_equal);
    const char *xdg = g_getenv("XDG_CACHE_HOME");
    char *base = xdg && *xdg
        ? g_strdup(xdg)
        : g_build_filename(g_get_home_dir(), ".cache", NULL);
    g_cache_dir = g_build_filename(base, "nordstjernen", "webfonts", NULL);
    g_free(base);
    g_mkdir_with_parents(g_cache_dir, 0700);
}

void
nd_font_shutdown(void)
{
    if (!g_entries) return;
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_entries);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        nd_font_entry *e = v;
        if (e->cancel) {
            g_cancellable_cancel(e->cancel);
            g_object_unref(e->cancel);
        }
        g_free(e->family);
        g_free(e->url);
        g_free(e);
    }
    g_hash_table_destroy(g_entries);
    g_entries = NULL;
    g_free(g_cache_dir);
    g_cache_dir = NULL;
}

gboolean
nd_font_available(void)
{
#ifdef ND_HAVE_FONTCONFIG
    return TRUE;
#else
    return FALSE;
#endif
}

void
nd_font_set_loaded_cb(nd_font_loaded_cb cb, gpointer user_data)
{
    g_loaded_cb = cb;
    g_loaded_ud = user_data;
}

gboolean
nd_font_has_loaded(const char *family)
{
    if (!g_entries || !family) return FALSE;
    nd_font_entry *e = g_hash_table_lookup(g_entries, family);
    return e && e->loaded;
}

static const char *
nd_font_extension_for(const char *url)
{
    if (!url) return ".bin";
    const char *q = strchr(url, '?');
    gsize end = q ? (gsize)(q - url) : strlen(url);
    const char *frag = memchr(url, '#', end);
    if (frag) end = (gsize)(frag - url);
    gsize i = end;
    while (i > 0 && url[i - 1] != '.' && url[i - 1] != '/') i--;
    if (i == 0 || url[i - 1] != '.') return ".bin";
    static char buf[12];
    gsize len = end - i;
    if (len > 7) return ".bin";
    buf[0] = '.';
    for (gsize j = 0; j < len; j++) buf[j + 1] = g_ascii_tolower(url[i + j]);
    buf[len + 1] = '\0';
    return buf;
}

static char *
nd_font_cache_path_for(const char *family, const char *url)
{
    if (!g_cache_dir || !family) return NULL;
    char *sanitized = g_strdup(family);
    for (char *p = sanitized; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_') *p = '_';
    const char *ext = nd_font_extension_for(url);
    char *name = g_strdup_printf("%s%s", sanitized, ext);
    g_free(sanitized);
    char *full = g_build_filename(g_cache_dir, name, NULL);
    g_free(name);
    return full;
}

typedef struct nd_font_fetch_ctx {
    char *family;
} nd_font_fetch_ctx;

#ifdef ND_HAVE_FONTCONFIG
static void
nd_font_install_file(const char *path)
{
    if (!path) return;
    FcConfigAppFontAddFile(NULL, (const FcChar8 *)path);
#ifdef ND_HAVE_PANGOFC
    PangoFontMap *fm = pango_cairo_font_map_get_default();
    if (fm && PANGO_IS_FC_FONT_MAP(fm))
        pango_fc_font_map_config_changed(PANGO_FC_FONT_MAP(fm));
#endif
}
#endif

static void
nd_font_on_fetched(GObject *src, GAsyncResult *res, gpointer user_data)
{
    (void)src;
    nd_font_fetch_ctx *ctx = user_data;
    GError *err = NULL;
    nd_response *resp = nd_net_fetch_finish(res, &err);
    nd_font_entry *e = g_entries ? g_hash_table_lookup(g_entries, ctx->family)
                                 : NULL;
    if (e) {
        if (e->cancel) { g_object_unref(e->cancel); e->cancel = NULL; }
        e->inflight = FALSE;
    }
    if (resp && !resp->error && resp->status < 400 &&
        resp->body && resp->body->len > 0) {
        char *path = nd_font_cache_path_for(ctx->family,
                                            resp->final_url ? resp->final_url
                                                            : (e ? e->url : NULL));
        if (path) {
            GError *werr = NULL;
            if (g_file_set_contents(path, (const char *)resp->body->data,
                                    (gssize)resp->body->len, &werr)) {
#ifdef ND_HAVE_FONTCONFIG
                nd_font_install_file(path);
#endif
                if (e) e->loaded = TRUE;
                if (g_loaded_cb) g_loaded_cb(ctx->family, g_loaded_ud);
            }
            g_clear_error(&werr);
            g_free(path);
        }
    }
    nd_response_free(resp);
    g_clear_error(&err);
    g_free(ctx->family);
    g_free(ctx);
}

void
nd_font_request(const char *family, const char *src_url, const char *base_url)
{
    if (!nd_font_available()) return;
    if (!g_entries) nd_font_init();
    if (!family || !*family || !src_url || !*src_url) return;

    char *abs = base_url ? nd_url_resolve(base_url, src_url) : g_strdup(src_url);
    if (!abs) return;

    nd_font_entry *existing = g_hash_table_lookup(g_entries, family);
    if (existing) {
        if (existing->loaded || existing->inflight) { g_free(abs); return; }
        if (existing->url && strcmp(existing->url, abs) == 0) {
            g_free(abs);
            return;
        }
        g_free(existing->url);
        existing->url = abs;
    } else {
        existing = g_new0(nd_font_entry, 1);
        existing->family = g_strdup(family);
        existing->url = abs;
        g_hash_table_insert(g_entries, existing->family, existing);
    }

    existing->inflight = TRUE;
    existing->cancel = g_cancellable_new();

    nd_font_fetch_ctx *ctx = g_new0(nd_font_fetch_ctx, 1);
    ctx->family = g_strdup(family);
    nd_net_fetch_async(existing->url, base_url, existing->cancel,
                       nd_font_on_fetched, ctx);
}
