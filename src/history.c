/* Nordstjernen — persistent visit history. */

#include "history.h"

#include <string.h>

struct nd_history {
    GArray *entries;
    char   *path;
};

static char *
history_path(void)
{
    const char *config = g_get_user_config_dir();
    char *dir = g_build_filename(config, "nordstjernen", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *path = g_build_filename(dir, "history.txt", NULL);
    g_free(dir);
    return path;
}

static void
entry_clear(gpointer data)
{
    nd_history_entry *e = data;
    g_free(e->url);
    g_free(e->title);
}

nd_history *
nd_history_load(void)
{
    nd_history *h = g_new0(nd_history, 1);
    h->entries = g_array_new(FALSE, FALSE, sizeof(nd_history_entry));
    g_array_set_clear_func(h->entries, entry_clear);
    h->path = history_path();

    char *contents = NULL;
    gsize len = 0;
    if (g_file_get_contents(h->path, &contents, &len, NULL) && contents) {
        char **lines = g_strsplit(contents, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *line = lines[i];
            if (*line == '\0' || *line == '#') continue;
            char **parts = g_strsplit(line, "\t", 3);
            if (parts[0]) {
                nd_history_entry e = {
                    .last_visit = g_ascii_strtoll(parts[0], NULL, 10),
                    .url   = g_strdup(parts[1] ? parts[1] : ""),
                    .title = g_strdup(parts[2] ? parts[2] : (parts[1] ? parts[1] : "")),
                };
                g_array_append_val(h->entries, e);
            }
            g_strfreev(parts);
        }
        g_strfreev(lines);
        g_free(contents);
    }
    return h;
}

void
nd_history_free(nd_history *h)
{
    if (!h) return;
    g_array_free(h->entries, TRUE);
    g_free(h->path);
    g_free(h);
}

static void
nd_history_save(nd_history *h)
{
    GString *out = g_string_new(NULL);
    guint start = 0;
    if (h->entries->len > ND_HISTORY_MAX)
        start = h->entries->len - ND_HISTORY_MAX;
    for (guint i = start; i < h->entries->len; i++) {
        nd_history_entry *e = &g_array_index(h->entries, nd_history_entry, i);
        g_string_append_printf(out, "%" G_GINT64_FORMAT "\t%s\t%s\n",
                               e->last_visit,
                               e->url   ? e->url   : "",
                               e->title ? e->title : "");
    }
    g_file_set_contents(h->path, out->str, (gssize)out->len, NULL);
    g_string_free(out, TRUE);
}

void
nd_history_visit(nd_history *h, const char *url, const char *title)
{
    if (!h || !url || !*url) return;
    if (g_str_has_prefix(url, "about:")) return;

    for (guint i = 0; i < h->entries->len; i++) {
        nd_history_entry *e = &g_array_index(h->entries, nd_history_entry, i);
        if (e->url && strcmp(e->url, url) == 0) {
            g_array_remove_index(h->entries, i);
            break;
        }
    }
    nd_history_entry e = {
        .url   = g_strdup(url),
        .title = g_strdup(title && *title ? title : url),
        .last_visit = g_get_real_time() / G_USEC_PER_SEC,
    };
    g_array_append_val(h->entries, e);
    if (h->entries->len > ND_HISTORY_MAX + 200) {
        guint excess = h->entries->len - ND_HISTORY_MAX;
        g_array_remove_range(h->entries, 0, excess);
    }
    nd_history_save(h);
}

guint
nd_history_count(const nd_history *h)
{
    return h ? h->entries->len : 0;
}

const nd_history_entry *
nd_history_get(const nd_history *h, guint i)
{
    if (!h || i >= h->entries->len) return NULL;
    return &g_array_index(h->entries, nd_history_entry, i);
}

GPtrArray *
nd_history_search(const nd_history *h, const char *needle, guint limit)
{
    GPtrArray *out = g_ptr_array_new();
    if (!h) return out;
    char *lower_needle = needle && *needle ? g_ascii_strdown(needle, -1) : NULL;
    for (gint i = (gint)h->entries->len - 1; i >= 0 && out->len < limit; i--) {
        nd_history_entry *e = &g_array_index(h->entries, nd_history_entry, i);
        if (!lower_needle) {
            g_ptr_array_add(out, e);
            continue;
        }
        gboolean hit = FALSE;
        if (e->url) {
            char *lu = g_ascii_strdown(e->url, -1);
            if (strstr(lu, lower_needle)) hit = TRUE;
            g_free(lu);
        }
        if (!hit && e->title) {
            char *lt = g_ascii_strdown(e->title, -1);
            if (strstr(lt, lower_needle)) hit = TRUE;
            g_free(lt);
        }
        if (hit) g_ptr_array_add(out, e);
    }
    g_free(lower_needle);
    return out;
}
