/* Nordstjernen — plain-file HTTP cache. */

#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

#include "cache.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <utime.h>

#define ND_CACHE_CAP_BYTES (256LL * 1024 * 1024)

static char    *g_cache_dir;
static gboolean g_cache_disabled;

static char *
key_for_url(const char *url)
{
    GChecksum *c = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(c, (const guchar *)url, (gssize)strlen(url));
    char *digest = g_strdup(g_checksum_get_string(c));
    g_checksum_free(c);
    return digest;
}

static char *
meta_path_for_key(const char *key)
{
    char prefix[3] = { key[0], key[1], '\0' };
    char *sub = g_build_filename(g_cache_dir, prefix, NULL);
    g_mkdir_with_parents(sub, 0700);
    char *out = g_strdup_printf("%s/%s.meta", sub, key + 2);
    g_free(sub);
    return out;
}

static char *
body_path_for_key(const char *key)
{
    char prefix[3] = { key[0], key[1], '\0' };
    char *out = g_strdup_printf("%s/%s/%s.body",
                                g_cache_dir, prefix, key + 2);
    return out;
}

void
nd_cache_init(void)
{
    if (g_getenv("ND_NO_CACHE")) {
        g_cache_disabled = TRUE;
        return;
    }
    const char *base = g_get_user_cache_dir();
    g_cache_dir = g_build_filename(base, "nordstjernen", "cache", NULL);
    g_mkdir_with_parents(g_cache_dir, 0700);
}

void
nd_cache_shutdown(void)
{
    g_clear_pointer(&g_cache_dir, g_free);
}

gboolean
nd_cache_enabled(void)
{
    return !g_cache_disabled && g_cache_dir != NULL;
}

void
nd_cache_entry_free(nd_cache_entry *e)
{
    if (!e) return;
    g_free(e->final_url);
    g_free(e->content_type);
    g_free(e->etag);
    g_free(e->last_modified);
    if (e->body) g_byte_array_unref(e->body);
    g_free(e);
}

static gint64
parse_http_date(const char *s)
{
    if (!s || !*s) return 0;
    GDateTime *dt = g_date_time_new_from_iso8601(s, NULL);
    if (!dt) {
        struct tm tm = {0};
        char *end = strptime(s, "%a, %d %b %Y %H:%M:%S", &tm);
        if (end) return (gint64)timegm(&tm);
        return 0;
    }
    gint64 r = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return r;
}

static gint64
freshness_from_headers(const char *cache_control, const char *expires_header)
{
    if (cache_control) {
        if (strstr(cache_control, "no-store")) return -1;
        if (strstr(cache_control, "no-cache")) return 0;
        const char *p = strstr(cache_control, "max-age");
        if (p) {
            p = strchr(p, '=');
            if (p) {
                gint64 ma = g_ascii_strtoll(p + 1, NULL, 10);
                if (ma > 0) return g_get_real_time() / G_USEC_PER_SEC + ma;
            }
        }
        if (strstr(cache_control, "immutable"))
            return g_get_real_time() / G_USEC_PER_SEC + 86400 * 30;
    }
    if (expires_header) {
        gint64 t = parse_http_date(expires_header);
        if (t > 0) return t;
    }
    return 0;
}

static gboolean
is_cacheable_status(long s)
{
    return s == 200 || s == 203 || s == 301 || s == 410;
}

static gint64
now_seconds(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

static void
touch_paths(const char *meta, const char *body)
{
    struct utimbuf t = { .actime = now_seconds(), .modtime = now_seconds() };
    utime(meta, &t);
    utime(body, &t);
}

nd_cache_entry *
nd_cache_get(const char *url)
{
    if (!nd_cache_enabled() || !url) return NULL;
    char *key  = key_for_url(url);
    char *meta = meta_path_for_key(key);
    char *body = body_path_for_key(key);
    char *meta_text = NULL;
    gsize meta_len = 0;
    if (!g_file_get_contents(meta, &meta_text, &meta_len, NULL)) {
        g_free(key); g_free(meta); g_free(body);
        return NULL;
    }
    nd_cache_entry *e = g_new0(nd_cache_entry, 1);
    char **lines = g_strsplit(meta_text, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        const char *line = lines[i];
        const char *colon = strchr(line, ':');
        if (!colon) continue;
        gsize klen = (gsize)(colon - line);
        const char *v = colon + 1;
        while (*v == ' ') v++;
        if      (klen == 9  && strncmp(line, "final_url",   9)  == 0) e->final_url    = g_strdup(v);
        else if (klen == 6  && strncmp(line, "status",      6)  == 0) e->status       = atol(v);
        else if (klen == 12 && strncmp(line, "content_type",12) == 0) e->content_type = g_strdup(v);
        else if (klen == 4  && strncmp(line, "etag",        4)  == 0) e->etag         = g_strdup(v);
        else if (klen == 13 && strncmp(line, "last_modified",13)== 0) e->last_modified= g_strdup(v);
        else if (klen == 10 && strncmp(line, "expires_at",  10) == 0) e->expires_at   = g_ascii_strtoll(v, NULL, 10);
        else if (klen == 10 && strncmp(line, "fetched_at",  10) == 0) e->fetched_at   = g_ascii_strtoll(v, NULL, 10);
    }
    g_strfreev(lines);
    g_free(meta_text);
    if (!e->final_url) e->final_url = g_strdup(url);
    char *body_text = NULL;
    gsize body_len = 0;
    if (!g_file_get_contents(body, &body_text, &body_len, NULL)) {
        nd_cache_entry_free(e);
        g_free(key); g_free(meta); g_free(body);
        return NULL;
    }
    e->body = g_byte_array_new();
    g_byte_array_append(e->body, (const guint8 *)body_text, (guint)body_len);
    g_free(body_text);
    touch_paths(meta, body);
    g_free(key); g_free(meta); g_free(body);
    return e;
}

gboolean
nd_cache_is_fresh(const nd_cache_entry *e)
{
    if (!e) return FALSE;
    return e->expires_at > now_seconds();
}

static void
write_meta(const char *meta_path,
           const char *url,
           const char *final_url,
           long status,
           const char *content_type,
           const char *etag,
           const char *last_modified,
           gint64 expires_at,
           gint64 fetched_at)
{
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "url: %s\n", url ? url : "");
    g_string_append_printf(s, "final_url: %s\n", final_url ? final_url : (url ? url : ""));
    g_string_append_printf(s, "status: %ld\n", status);
    if (content_type)   g_string_append_printf(s, "content_type: %s\n",  content_type);
    if (etag)           g_string_append_printf(s, "etag: %s\n",          etag);
    if (last_modified)  g_string_append_printf(s, "last_modified: %s\n", last_modified);
    g_string_append_printf(s, "expires_at: %" G_GINT64_FORMAT "\n", expires_at);
    g_string_append_printf(s, "fetched_at: %" G_GINT64_FORMAT "\n", fetched_at);
    g_file_set_contents(meta_path, s->str, (gssize)s->len, NULL);
    g_chmod(meta_path, 0600);
    g_string_free(s, TRUE);
}

static guint64
scan_total_size(GFile *dir)
{
    guint64 total = 0;
    GFileEnumerator *en = g_file_enumerate_children(dir,
        G_FILE_ATTRIBUTE_STANDARD_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_SIZE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    if (!en) return 0;
    GFileInfo *info;
    while ((info = g_file_enumerator_next_file(en, NULL, NULL))) {
        if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
            GFile *sub = g_file_get_child(dir, g_file_info_get_name(info));
            total += scan_total_size(sub);
            g_object_unref(sub);
        } else {
            total += (guint64)g_file_info_get_size(info);
        }
        g_object_unref(info);
    }
    g_object_unref(en);
    return total;
}

typedef struct cache_file {
    char *path;
    gint64 mtime;
    guint64 size;
} cache_file;

static gint
cmp_file_mtime(gconstpointer a, gconstpointer b)
{
    const cache_file *fa = a, *fb = b;
    if (fa->mtime < fb->mtime) return -1;
    if (fa->mtime > fb->mtime) return 1;
    return 0;
}

static void
collect_meta_files(GFile *dir, GArray *out)
{
    GFileEnumerator *en = g_file_enumerate_children(dir,
        G_FILE_ATTRIBUTE_STANDARD_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_SIZE ","
        G_FILE_ATTRIBUTE_TIME_MODIFIED,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    if (!en) return;
    GFileInfo *info;
    while ((info = g_file_enumerator_next_file(en, NULL, NULL))) {
        const char *name = g_file_info_get_name(info);
        if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
            GFile *sub = g_file_get_child(dir, name);
            collect_meta_files(sub, out);
            g_object_unref(sub);
        } else if (g_str_has_suffix(name, ".meta")) {
            cache_file f = {
                .path  = g_build_filename(g_file_peek_path(dir), name, NULL),
                .mtime = (gint64)g_file_info_get_attribute_uint64(
                              info, G_FILE_ATTRIBUTE_TIME_MODIFIED),
                .size  = (guint64)g_file_info_get_size(info),
            };
            g_array_append_val(out, f);
        }
        g_object_unref(info);
    }
    g_object_unref(en);
}

static void
evict_to_cap(void)
{
    if (!nd_cache_enabled()) return;
    GFile *root = g_file_new_for_path(g_cache_dir);
    guint64 total = scan_total_size(root);
    if (total <= ND_CACHE_CAP_BYTES) {
        g_object_unref(root);
        return;
    }
    GArray *metas = g_array_new(FALSE, FALSE, sizeof(cache_file));
    collect_meta_files(root, metas);
    g_object_unref(root);
    g_array_sort(metas, cmp_file_mtime);
    for (guint i = 0; i < metas->len && total > ND_CACHE_CAP_BYTES; i++) {
        cache_file *f = &g_array_index(metas, cache_file, i);
        char *body = g_strdup(f->path);
        gsize plen = strlen(body);
        if (plen > 5) memcpy(body + plen - 5, ".body", 5);
        struct stat st;
        if (g_stat(body, &st) == 0) total -= (guint64)st.st_size;
        g_unlink(body);
        if (g_stat(f->path, &st) == 0) total -= (guint64)st.st_size;
        g_unlink(f->path);
        g_free(body);
    }
    for (guint i = 0; i < metas->len; i++)
        g_free(g_array_index(metas, cache_file, i).path);
    g_array_free(metas, TRUE);
}

static gboolean
url_should_cache(const char *url)
{
    if (!url) return FALSE;
    if (g_str_has_prefix(url, "about:")) return FALSE;
    if (g_str_has_prefix(url, "file:"))  return FALSE;
    return g_str_has_prefix(url, "http:") || g_str_has_prefix(url, "https:");
}

void
nd_cache_put(const char *url,
             const char *final_url,
             long status,
             const char *content_type,
             const char *etag,
             const char *last_modified,
             const char *cache_control,
             const char *expires_header,
             const void *body, gsize body_len)
{
    if (!nd_cache_enabled() || !url_should_cache(url)) return;
    if (!is_cacheable_status(status)) return;
    gint64 expires_at = freshness_from_headers(cache_control, expires_header);
    if (expires_at < 0) return;
    char *key       = key_for_url(url);
    char *meta_path = meta_path_for_key(key);
    char *body_path = body_path_for_key(key);
    write_meta(meta_path, url, final_url, status, content_type,
               etag, last_modified, expires_at, now_seconds());
    g_file_set_contents(body_path, body ? body : "", (gssize)body_len, NULL);
    g_chmod(body_path, 0600);
    g_free(key); g_free(meta_path); g_free(body_path);
    evict_to_cap();
}

void
nd_cache_promote_304(const char *url, const char *cache_control,
                     const char *expires_header)
{
    if (!nd_cache_enabled() || !url_should_cache(url)) return;
    nd_cache_entry *e = nd_cache_get(url);
    if (!e) return;
    char *key       = key_for_url(url);
    char *meta_path = meta_path_for_key(key);
    char *body_path = body_path_for_key(key);
    gint64 expires_at = freshness_from_headers(cache_control, expires_header);
    if (expires_at < 0) expires_at = 0;
    write_meta(meta_path, url, e->final_url, e->status, e->content_type,
               e->etag, e->last_modified, expires_at, now_seconds());
    touch_paths(meta_path, body_path);
    g_free(key); g_free(meta_path); g_free(body_path);
    nd_cache_entry_free(e);
}
