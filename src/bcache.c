/* Nordstjernen — JavaScript bytecode cache. */

#include "bcache.h"

#include "config.h"
#include "sha3.h"

#include <glib/gstdio.h>
#include <string.h>

#define ND_BCACHE_MEM_CAP_BYTES   (16u * 1024u * 1024u)
#define ND_BCACHE_VALUE_CAP_BYTES (4u  * 1024u * 1024u)
#define ND_BCACHE_FORMAT_VERSION  2026052201u

typedef struct nd_bcache_entry {
    guint8 *bytes;
    gsize   len;
    gint64  used_us;
} nd_bcache_entry;

static GHashTable *g_mem;
static GMutex      g_lock;
static guint64     g_mem_bytes;
static char       *g_dir;

static void
nd_bcache_entry_free(gpointer data)
{
    nd_bcache_entry *e = data;
    if (!e) return;
    g_free(e->bytes);
    g_free(e);
}

void
nd_bcache_init(void)
{
    g_mutex_lock(&g_lock);
    if (!g_mem)
        g_mem = g_hash_table_new_full(g_str_hash, g_str_equal,
                                      g_free, nd_bcache_entry_free);
    if (!g_dir) {
        const nd_config *c = nd_config_get();
        if (!c || c->cache_enabled) {
            const char *base = g_get_user_cache_dir();
            g_dir = g_build_filename(base, ND_APP_DIR_NAME, "jsbc", NULL);
            g_mkdir_with_parents(g_dir, 0700);
        }
    }
    g_mutex_unlock(&g_lock);
}

void
nd_bcache_shutdown(void)
{
    g_mutex_lock(&g_lock);
    if (g_mem) {
        g_hash_table_destroy(g_mem);
        g_mem = NULL;
    }
    g_mem_bytes = 0;
    g_clear_pointer(&g_dir, g_free);
    g_mutex_unlock(&g_lock);
}

static void
hash_source(const char *src, gsize len, char out[65])
{
    uint8_t digest[64];
    nd_sha3_512((const uint8_t *)(src ? src : ""), len, digest);
    static const char hex[] = "0123456789abcdef";
    int n = 32;
    for (int i = 0; i < n; i++) {
        out[i * 2]     = hex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    out[n * 2] = '\0';
}

static char *
disk_path_for(const char *key)
{
    if (!g_dir || !key || !*key) return NULL;
    char sub[3] = { key[0], key[1], '\0' };
    char *dir = g_build_filename(g_dir, sub, NULL);
    g_mkdir_with_parents(dir, 0700);
    char *file = g_build_filename(dir, key + 2, NULL);
    g_free(dir);
    return file;
}

static guint8 *
read_disk(const char *key, gsize *out_len)
{
    char *path = disk_path_for(key);
    if (!path) return NULL;
    gchar *contents = NULL;
    gsize length = 0;
    gboolean ok = g_file_get_contents(path, &contents, &length, NULL);
    g_free(path);
    if (!ok) return NULL;
    if (length < 4 + 4) { g_free(contents); return NULL; }
    guint32 magic = 0, fmt = 0;
    memcpy(&magic, contents,     4);
    memcpy(&fmt,   contents + 4, 4);
    if (magic != GUINT32_FROM_LE(0x4E4A4243u) ||
        fmt   != GUINT32_FROM_LE(ND_BCACHE_FORMAT_VERSION)) {
        g_free(contents);
        return NULL;
    }
    gsize bc_len = length - 8;
    if (bc_len == 0 || bc_len > ND_BCACHE_VALUE_CAP_BYTES) {
        g_free(contents);
        return NULL;
    }
    guint8 *out = g_malloc(bc_len);
    memcpy(out, contents + 8, bc_len);
    g_free(contents);
    if (out_len) *out_len = bc_len;
    return out;
}

static void
write_disk(const char *key, const guint8 *bc, gsize bc_len)
{
    char *path = disk_path_for(key);
    if (!path) return;
    guint32 magic = GUINT32_TO_LE(0x4E4A4243u);
    guint32 fmt   = GUINT32_TO_LE(ND_BCACHE_FORMAT_VERSION);
    char *tmp = g_strdup_printf("%s.tmp", path);
    FILE *f = g_fopen(tmp, "wb");
    if (!f) { g_free(tmp); g_free(path); return; }
    gboolean wrote = fwrite(&magic, 1, 4, f) == 4 &&
                     fwrite(&fmt,   1, 4, f) == 4 &&
                     fwrite(bc,     1, bc_len, f) == bc_len &&
                     ferror(f) == 0;
    if (fclose(f) != 0 || !wrote) {
        g_unlink(tmp);
        g_free(tmp); g_free(path);
        return;
    }
    if (g_rename(tmp, path) != 0)
        g_unlink(tmp);
    g_free(tmp);
    g_free(path);
}

static void
evict_until_fits(gsize want_bytes)
{
    if (!g_mem) return;
    while (g_mem_bytes + want_bytes > ND_BCACHE_MEM_CAP_BYTES) {
        gint64   oldest_us = G_MAXINT64;
        gpointer oldest_key = NULL;
        nd_bcache_entry *oldest_entry = NULL;
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, g_mem);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            nd_bcache_entry *e = v;
            if (e->used_us < oldest_us) {
                oldest_us = e->used_us;
                oldest_key = k;
                oldest_entry = e;
            }
        }
        if (!oldest_entry) break;
        g_mem_bytes -= oldest_entry->len;
        g_hash_table_remove(g_mem, oldest_key);
    }
}

guint8 *
nd_bcache_get(const char *src, gsize src_len, gsize *out_len)
{
    if (!src || !src_len) return NULL;
    char key[65];
    hash_source(src, src_len, key);

    g_mutex_lock(&g_lock);
    if (!g_mem) {
        g_mutex_unlock(&g_lock);
        return NULL;
    }
    nd_bcache_entry *e = g_hash_table_lookup(g_mem, key);
    if (e) {
        e->used_us = g_get_monotonic_time();
        guint8 *copy = g_memdup2(e->bytes, e->len);
        if (out_len) *out_len = e->len;
        g_mutex_unlock(&g_lock);
        return copy;
    }
    g_mutex_unlock(&g_lock);

    gsize disk_len = 0;
    guint8 *disk = read_disk(key, &disk_len);
    if (!disk) return NULL;

    g_mutex_lock(&g_lock);
    if (g_mem && disk_len <= ND_BCACHE_VALUE_CAP_BYTES) {
        if (!g_hash_table_lookup(g_mem, key)) {
            evict_until_fits(disk_len);
            nd_bcache_entry *ne = g_new0(nd_bcache_entry, 1);
            ne->bytes = g_memdup2(disk, disk_len);
            ne->len = disk_len;
            ne->used_us = g_get_monotonic_time();
            g_hash_table_insert(g_mem, g_strdup(key), ne);
            g_mem_bytes += disk_len;
        }
    }
    g_mutex_unlock(&g_lock);
    if (out_len) *out_len = disk_len;
    return disk;
}

void
nd_bcache_put(const char *src, gsize src_len,
              const guint8 *bc, gsize bc_len)
{
    if (!src || !src_len || !bc || !bc_len) return;
    if (bc_len > ND_BCACHE_VALUE_CAP_BYTES) return;
    char key[65];
    hash_source(src, src_len, key);

    g_mutex_lock(&g_lock);
    if (!g_mem) { g_mutex_unlock(&g_lock); return; }
    nd_bcache_entry *existing = g_hash_table_lookup(g_mem, key);
    if (existing) {
        existing->used_us = g_get_monotonic_time();
        g_mutex_unlock(&g_lock);
        return;
    }
    evict_until_fits(bc_len);
    nd_bcache_entry *ne = g_new0(nd_bcache_entry, 1);
    ne->bytes = g_memdup2(bc, bc_len);
    ne->len = bc_len;
    ne->used_us = g_get_monotonic_time();
    g_hash_table_insert(g_mem, g_strdup(key), ne);
    g_mem_bytes += bc_len;
    g_mutex_unlock(&g_lock);

    write_disk(key, bc, bc_len);
}
