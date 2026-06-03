/* Nordstjernen — bookmarks storage.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "bookmarks.h"
#include "config.h"

#include <glib/gstdio.h>
#include <string.h>

struct nd_bookmarks {
    GArray *items;
    char   *path;
};

static char *
bookmarks_path(void)
{
    const char *config = g_get_user_config_dir();
    g_autofree char *dir = g_build_filename(config, ND_APP_DIR_NAME, NULL);
    g_mkdir_with_parents(dir, 0700);
    return g_build_filename(dir, "bookmarks.txt", NULL);
}

static void
bookmark_clear(gpointer data)
{
    nd_bookmark *b = data;
    g_free(b->url);
    g_free(b->title);
}

nd_bookmarks *
nd_bookmarks_load(void)
{
    nd_bookmarks *bm = g_new0(nd_bookmarks, 1);
    bm->items = g_array_new(FALSE, FALSE, sizeof(nd_bookmark));
    g_array_set_clear_func(bm->items, bookmark_clear);
    bm->path = bookmarks_path();

    char *contents = NULL;
    gsize len = 0;
    if (g_file_get_contents(bm->path, &contents, &len, NULL) && contents) {
        char **lines = g_strsplit(contents, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *line = g_strstrip(lines[i]);
            if (*line == '\0' || *line == '#') continue;
            char *tab = strchr(line, '\t');
            nd_bookmark b;
            if (tab) {
                *tab = '\0';
                b.url   = g_strdup(line);
                b.title = g_strdup(tab + 1);
            } else {
                b.url   = g_strdup(line);
                b.title = g_strdup(line);
            }
            g_array_append_val(bm->items, b);
        }
        g_strfreev(lines);
        g_free(contents);
    }
    return bm;
}

void
nd_bookmarks_free(nd_bookmarks *bm)
{
    if (!bm) return;
    g_array_free(bm->items, TRUE);
    g_free(bm->path);
    g_free(bm);
}

static void
bookmark_append_sanitized(GString *out, const char *s)
{
    if (!s) return;
    for (const char *p = s; *p; p++) {
        if (*p == '\t' || *p == '\n' || *p == '\r') g_string_append_c(out, ' ');
        else g_string_append_c(out, *p);
    }
}

static void
nd_bookmarks_save(nd_bookmarks *bm)
{
    if (!bm) return;
    GString *out = g_string_new(NULL);
    for (guint i = 0; i < bm->items->len; i++) {
        nd_bookmark *b = &g_array_index(bm->items, nd_bookmark, i);
        bookmark_append_sanitized(out, b->url);
        g_string_append_c(out, '\t');
        bookmark_append_sanitized(out, b->title);
        g_string_append_c(out, '\n');
    }
    GError *err = NULL;
    if (!g_file_set_contents_full(bm->path, out->str, (gssize)out->len,
                                  G_FILE_SET_CONTENTS_CONSISTENT,
                                  0600, &err)) {
        g_warning("bookmarks: failed to write %s: %s", bm->path, err->message);
        g_clear_error(&err);
    }
    g_string_free(out, TRUE);
}

guint
nd_bookmarks_count(const nd_bookmarks *bm)
{
    return bm ? bm->items->len : 0;
}

const nd_bookmark *
nd_bookmarks_get(const nd_bookmarks *bm, guint i)
{
    if (!bm || i >= bm->items->len) return NULL;
    return &g_array_index(bm->items, nd_bookmark, i);
}

gboolean
nd_bookmarks_contains(const nd_bookmarks *bm, const char *url)
{
    if (!bm || !url) return FALSE;
    for (guint i = 0; i < bm->items->len; i++) {
        nd_bookmark *b = &g_array_index(bm->items, nd_bookmark, i);
        if (b->url && strcmp(b->url, url) == 0) return TRUE;
    }
    return FALSE;
}

void
nd_bookmarks_add(nd_bookmarks *bm, const char *url, const char *title)
{
    if (!bm || !url || nd_bookmarks_contains(bm, url)) return;
    nd_bookmark b = { .url = g_strdup(url), .title = g_strdup(title ? title : url) };
    g_array_append_val(bm->items, b);
    nd_bookmarks_save(bm);
}

void
nd_bookmarks_remove(nd_bookmarks *bm, const char *url)
{
    if (!bm || !url) return;
    for (guint i = 0; i < bm->items->len; i++) {
        nd_bookmark *b = &g_array_index(bm->items, nd_bookmark, i);
        if (b->url && strcmp(b->url, url) == 0) {
            g_array_remove_index(bm->items, i);
            nd_bookmarks_save(bm);
            return;
        }
    }
}
