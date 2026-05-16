/* Nordstjernen — encrypted HTTP cache.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "cache.h"
#include "config.h"
#include "net.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef G_OS_WIN32
#  include <sys/utime.h>
#  include <windows.h>
#  include <bcrypt.h>
#else
#  include <utime.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif
#if defined(__linux__)
#  include <sys/random.h>
#endif

static char    *g_cache_dir;
static gboolean g_cache_disabled;

enum {
    ND_CACHE_KEY_LEN   = 32,
    ND_CACHE_NONCE_LEN = 16,
    ND_CACHE_MAC_LEN   = 32,
    ND_CACHE_HDR_LEN   = 16,
    ND_CACHE_BLOCK_LEN = 32,
    ND_CACHE_PREAMBLE  = ND_CACHE_HDR_LEN + ND_CACHE_NONCE_LEN + ND_CACHE_MAC_LEN,
};

static const guchar ND_CACHE_HEADER[ND_CACHE_HDR_LEN] = {
    'N','D','C','1', 0,0,0,0, 0,0,0,0, 0,0,0,0
};

#define ND_CACHE_MAX_AGE_SECONDS (30 * 24 * 60 * 60)

static guchar  g_cache_master[ND_CACHE_KEY_LEN];
static gboolean g_cache_key_ok;

static gboolean
csprng_fill(void *buf, gsize len)
{
    if (!buf || len == 0) return TRUE;
#if defined(G_OS_WIN32)
    if (BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
        return TRUE;
#elif defined(__linux__)
    size_t off = 0;
    while (off < len) {
        ssize_t n = getrandom((char *)buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += (size_t)n;
    }
    if (off == len) return TRUE;
#endif
#ifndef G_OS_WIN32
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return FALSE;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        got += (size_t)n;
    }
    close(fd);
    return got == len;
#else
    return FALSE;
#endif
}

static void
hmac_sha256(const guchar *key, gsize kn,
            const guchar *data, gsize dn,
            guchar out[ND_CACHE_MAC_LEN])
{
    GHmac *h = g_hmac_new(G_CHECKSUM_SHA256, key, kn);
    g_hmac_update(h, data, (gssize)dn);
    gsize outlen = ND_CACHE_MAC_LEN;
    g_hmac_get_digest(h, out, &outlen);
    g_hmac_unref(h);
}

static void
derive_subkey(const char *label, guchar out[ND_CACHE_KEY_LEN])
{
    hmac_sha256(g_cache_master, ND_CACHE_KEY_LEN,
                (const guchar *)label, strlen(label), out);
}

static void
crypt_stream(const guchar enc_key[ND_CACHE_KEY_LEN],
             const guchar nonce[ND_CACHE_NONCE_LEN],
             const void *in, void *out, gsize n)
{
    guchar block_in[ND_CACHE_NONCE_LEN + 8];
    memcpy(block_in, nonce, ND_CACHE_NONCE_LEN);
    guchar ks[ND_CACHE_BLOCK_LEN];
    for (gsize off = 0; off < n; off += ND_CACHE_BLOCK_LEN) {
        guint64 ctr = GUINT64_TO_BE((guint64)(off / ND_CACHE_BLOCK_LEN));
        memcpy(block_in + ND_CACHE_NONCE_LEN, &ctr, 8);
        hmac_sha256(enc_key, ND_CACHE_KEY_LEN, block_in, sizeof block_in, ks);
        gsize take = (n - off) < ND_CACHE_BLOCK_LEN
                     ? (n - off) : ND_CACHE_BLOCK_LEN;
        for (gsize i = 0; i < take; i++)
            ((guchar *)out)[off + i] =
                ((const guchar *)in)[off + i] ^ ks[i];
    }
}

static GByteArray *
cache_encrypt(const void *plain, gsize n)
{
    if (!g_cache_key_ok) return NULL;
    guchar nonce[ND_CACHE_NONCE_LEN];
    if (!csprng_fill(nonce, sizeof nonce)) return NULL;

    guchar enc_key[ND_CACHE_KEY_LEN], mac_key[ND_CACHE_KEY_LEN];
    derive_subkey("nordstjernen-cache-v1-enc", enc_key);
    derive_subkey("nordstjernen-cache-v1-mac", mac_key);

    GByteArray *out = g_byte_array_sized_new((guint)(ND_CACHE_PREAMBLE + n));
    g_byte_array_append(out, ND_CACHE_HEADER, ND_CACHE_HDR_LEN);
    g_byte_array_append(out, nonce, ND_CACHE_NONCE_LEN);
    gsize mac_off = out->len;
    g_byte_array_set_size(out, (guint)(out->len + ND_CACHE_MAC_LEN));
    gsize cipher_off = out->len;
    g_byte_array_set_size(out, (guint)(out->len + n));

    crypt_stream(enc_key, nonce, plain, out->data + cipher_off, n);

    GHmac *mac = g_hmac_new(G_CHECKSUM_SHA256, mac_key, ND_CACHE_KEY_LEN);
    g_hmac_update(mac, ND_CACHE_HEADER, ND_CACHE_HDR_LEN);
    g_hmac_update(mac, nonce, ND_CACHE_NONCE_LEN);
    g_hmac_update(mac, out->data + cipher_off, (gssize)n);
    gsize maclen = ND_CACHE_MAC_LEN;
    g_hmac_get_digest(mac, out->data + mac_off, &maclen);
    g_hmac_unref(mac);

    memset(enc_key, 0, sizeof enc_key);
    memset(mac_key, 0, sizeof mac_key);
    return out;
}

static gboolean
cache_decrypt(const void *file, gsize n, guchar **out_plain, gsize *out_n)
{
    if (!g_cache_key_ok) return FALSE;
    if (n < ND_CACHE_PREAMBLE) return FALSE;
    const guchar *p = file;
    if (memcmp(p, ND_CACHE_HEADER, ND_CACHE_HDR_LEN) != 0) return FALSE;
    const guchar *nonce   = p + ND_CACHE_HDR_LEN;
    const guchar *mac_got = nonce + ND_CACHE_NONCE_LEN;
    const guchar *cipher  = mac_got + ND_CACHE_MAC_LEN;
    gsize cipher_n = n - ND_CACHE_PREAMBLE;

    guchar enc_key[ND_CACHE_KEY_LEN], mac_key[ND_CACHE_KEY_LEN];
    derive_subkey("nordstjernen-cache-v1-enc", enc_key);
    derive_subkey("nordstjernen-cache-v1-mac", mac_key);

    GHmac *mac = g_hmac_new(G_CHECKSUM_SHA256, mac_key, ND_CACHE_KEY_LEN);
    g_hmac_update(mac, ND_CACHE_HEADER, ND_CACHE_HDR_LEN);
    g_hmac_update(mac, nonce, ND_CACHE_NONCE_LEN);
    g_hmac_update(mac, cipher, (gssize)cipher_n);
    guchar mac_calc[ND_CACHE_MAC_LEN];
    gsize maclen = ND_CACHE_MAC_LEN;
    g_hmac_get_digest(mac, mac_calc, &maclen);
    g_hmac_unref(mac);

    guint32 diff = 0;
    for (gsize i = 0; i < ND_CACHE_MAC_LEN; i++)
        diff |= (guint32)(mac_got[i] ^ mac_calc[i]);
    if (diff != 0) {
        memset(enc_key, 0, sizeof enc_key);
        memset(mac_key, 0, sizeof mac_key);
        return FALSE;
    }

    guchar *plain = g_malloc(cipher_n ? cipher_n : 1);
    crypt_stream(enc_key, nonce, cipher, plain, cipher_n);
    memset(enc_key, 0, sizeof enc_key);
    memset(mac_key, 0, sizeof mac_key);
    *out_plain = plain;
    *out_n = cipher_n;
    return TRUE;
}

static gboolean
load_or_create_master_key(void)
{
    char *path = g_build_filename(g_cache_dir, ".key", NULL);
    gchar *data = NULL;
    gsize len = 0;
    if (g_file_get_contents(path, &data, &len, NULL) &&
        len == ND_CACHE_KEY_LEN) {
        memcpy(g_cache_master, data, ND_CACHE_KEY_LEN);
        memset(data, 0, len);
        g_free(data);
        g_free(path);
        g_cache_key_ok = TRUE;
        return TRUE;
    }
    g_free(data);
    if (!csprng_fill(g_cache_master, ND_CACHE_KEY_LEN)) {
        g_free(path);
        return FALSE;
    }
    GError *err = NULL;
    if (!g_file_set_contents(path, (const char *)g_cache_master,
                             ND_CACHE_KEY_LEN, &err)) {
        g_warning("cache: failed to write key %s: %s", path, err->message);
        g_clear_error(&err);
        g_free(path);
        return FALSE;
    }
    g_chmod(path, 0600);
    g_free(path);
    g_cache_key_ok = TRUE;
    return TRUE;
}

static guint64
cache_cap_bytes(void)
{
    const nd_config *c = nd_config_get();
    int mb = c ? c->cache_cap_mb : 256;
    if (mb <= 0) mb = 256;
    return (guint64)mb * 1024ULL * 1024ULL;
}

static char *
key_for_url(const char *url, const char *partition)
{
    GChecksum *c = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(c, (const guchar *)url, (gssize)strlen(url));
    if (partition && *partition) {
        g_checksum_update(c, (const guchar *)"\x1f", 1);
        g_checksum_update(c, (const guchar *)partition, (gssize)strlen(partition));
    }
    char *digest = g_strdup(g_checksum_get_string(c));
    g_checksum_free(c);
    return digest;
}

static char *
path_for_key(const char *key, const char *suffix, gboolean ensure_dir)
{
    char prefix[3] = { key[0], key[1], '\0' };
    char *sub = g_build_filename(g_cache_dir, prefix, NULL);
    if (ensure_dir) g_mkdir_with_parents(sub, 0700);
    char *leaf = g_strdup_printf("%s%s", key + 2, suffix);
    char *out = g_build_filename(sub, leaf, NULL);
    g_free(leaf);
    g_free(sub);
    return out;
}

static char *
meta_path_for_key(const char *key) { return path_for_key(key, ".meta", TRUE); }

static char *
body_path_for_key(const char *key) { return path_for_key(key, ".body", FALSE); }

static void evict_aged_out(void);

void
nd_cache_init(void)
{
    const nd_config *c = nd_config_get();
    if (c && !c->cache_enabled) {
        g_cache_disabled = TRUE;
        return;
    }
    const char *base = g_get_user_cache_dir();
    g_cache_dir = g_build_filename(base, ND_APP_DIR_NAME, "cache", NULL);
    g_mkdir_with_parents(g_cache_dir, 0700);
    g_chmod(g_cache_dir, 0700);
    if (!load_or_create_master_key()) {
        g_warning("cache: disabling — could not establish encryption key");
        g_cache_disabled = TRUE;
        g_clear_pointer(&g_cache_dir, g_free);
        return;
    }
    evict_aged_out();
}

void
nd_cache_shutdown(void)
{
    memset(g_cache_master, 0, sizeof g_cache_master);
    g_cache_key_ok = FALSE;
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

static int
month_from_name(const char *m)
{
    static const char *const months[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    for (int i = 0; i < 12; i++)
        if (g_ascii_strncasecmp(m, months[i], 3) == 0) return i + 1;
    return 0;
}

static gint64
parse_http_date(const char *s)
{
    if (!s || !*s) return 0;
    GDateTime *dt = g_date_time_new_from_iso8601(s, NULL);
    if (dt) {
        gint64 r = g_date_time_to_unix(dt);
        g_date_time_unref(dt);
        return r;
    }
    const char *comma = strchr(s, ',');
    const char *p = comma ? comma + 1 : s;
    while (*p == ' ') p++;
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    char mon[4] = {0};
    char sep = 0;
    if (sscanf(p, "%d %3s %d %d:%d:%d",
               &day, mon, &year, &hh, &mm, &ss) == 6) {
    } else if (sscanf(p, "%d%c%3s%c%d %d:%d:%d",
                      &day, &sep, mon, &sep, &year, &hh, &mm, &ss) == 8) {
        if (year < 100) year += (year < 70 ? 2000 : 1900);
    } else if (sscanf(p, "%3s %d %d:%d:%d %d",
                      mon, &day, &hh, &mm, &ss, &year) == 6) {
    } else {
        return 0;
    }
    int month = month_from_name(mon);
    if (!month) return 0;
    GTimeZone *utc = g_time_zone_new_utc();
    dt = g_date_time_new(utc, year, month, day, hh, mm, (double)ss);
    g_time_zone_unref(utc);
    if (!dt) return 0;
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
    g_utime(meta, NULL);
    g_utime(body, NULL);
}

static gboolean
read_file_decrypted(const char *path, guchar **out, gsize *out_n)
{
    gchar *raw = NULL;
    gsize raw_len = 0;
    if (!g_file_get_contents(path, &raw, &raw_len, NULL)) return FALSE;
    gboolean ok = cache_decrypt(raw, raw_len, out, out_n);
    memset(raw, 0, raw_len);
    g_free(raw);
    return ok;
}

static gboolean
write_file_encrypted(const char *path, const void *plain, gsize n)
{
    GByteArray *enc = cache_encrypt(plain, n);
    if (!enc) return FALSE;
    GError *err = NULL;
    gboolean ok = g_file_set_contents(path, (const char *)enc->data,
                                      (gssize)enc->len, &err);
    if (!ok) {
        g_warning("cache: failed to write %s: %s", path, err->message);
        g_clear_error(&err);
    } else {
        g_chmod(path, 0600);
    }
    g_byte_array_unref(enc);
    return ok;
}

static nd_cache_entry *
read_meta(const char *url, const char *meta_path)
{
    guchar *meta_buf = NULL;
    gsize meta_len = 0;
    if (!read_file_decrypted(meta_path, &meta_buf, &meta_len)) {
        g_unlink(meta_path);
        return NULL;
    }
    char *meta_text = g_strndup((const char *)meta_buf, meta_len);
    memset(meta_buf, 0, meta_len);
    g_free(meta_buf);
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
        else if (klen == 6  && strncmp(line, "status",      6)  == 0) e->status       = (long)g_ascii_strtoll(v, NULL, 10);
        else if (klen == 12 && strncmp(line, "content_type",12) == 0) e->content_type = g_strdup(v);
        else if (klen == 4  && strncmp(line, "etag",        4)  == 0) e->etag         = g_strdup(v);
        else if (klen == 13 && strncmp(line, "last_modified",13)== 0) e->last_modified= g_strdup(v);
        else if (klen == 10 && strncmp(line, "expires_at",  10) == 0) e->expires_at   = g_ascii_strtoll(v, NULL, 10);
        else if (klen == 10 && strncmp(line, "fetched_at",  10) == 0) e->fetched_at   = g_ascii_strtoll(v, NULL, 10);
    }
    g_strfreev(lines);
    g_free(meta_text);
    if (!e->final_url) e->final_url = g_strdup(url);
    return e;
}

nd_cache_entry *
nd_cache_get(const char *url, const char *partition)
{
    if (!nd_cache_enabled() || !url) return NULL;
    char *key  = key_for_url(url, partition);
    char *meta = meta_path_for_key(key);
    char *body = body_path_for_key(key);
    nd_cache_entry *e = read_meta(url, meta);
    if (!e) {
        g_free(key); g_free(meta); g_free(body);
        return NULL;
    }
    guchar *body_plain = NULL;
    gsize body_len = 0;
    if (!read_file_decrypted(body, &body_plain, &body_len)) {
        nd_cache_entry_free(e);
        g_unlink(body);
        g_unlink(meta);
        g_free(key); g_free(meta); g_free(body);
        return NULL;
    }
    e->body = g_byte_array_new();
    g_byte_array_append(e->body, body_plain, (guint)body_len);
    memset(body_plain, 0, body_len);
    g_free(body_plain);
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
append_meta_value(GString *s, const char *key, const char *value)
{
    g_string_append(s, key);
    g_string_append(s, ": ");
    if (value) {
        for (const char *p = value; *p; p++) {
            if (*p == '\n' || *p == '\r') g_string_append_c(s, ' ');
            else g_string_append_c(s, *p);
        }
    }
    g_string_append_c(s, '\n');
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
    append_meta_value(s, "url",       url);
    append_meta_value(s, "final_url", final_url ? final_url : url);
    g_string_append_printf(s, "status: %ld\n", status);
    if (content_type)  append_meta_value(s, "content_type",  content_type);
    if (etag)          append_meta_value(s, "etag",          etag);
    if (last_modified) append_meta_value(s, "last_modified", last_modified);
    g_string_append_printf(s, "expires_at: %" G_GINT64_FORMAT "\n", expires_at);
    g_string_append_printf(s, "fetched_at: %" G_GINT64_FORMAT "\n", fetched_at);
    write_file_encrypted(meta_path, s->str, s->len);
    memset(s->str, 0, s->len);
    g_string_free(s, TRUE);
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
collect_meta_files(GFile *dir, GArray *out_metas, guint64 *total)
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
            collect_meta_files(sub, out_metas, total);
            g_object_unref(sub);
        } else {
            guint64 size = (guint64)g_file_info_get_size(info);
            *total += size;
            if (g_str_has_suffix(name, ".meta")) {
                cache_file f = {
                    .path  = g_build_filename(g_file_peek_path(dir), name, NULL),
                    .mtime = (gint64)g_file_info_get_attribute_uint64(
                                  info, G_FILE_ATTRIBUTE_TIME_MODIFIED),
                    .size  = size,
                };
                g_array_append_val(out_metas, f);
            }
        }
        g_object_unref(info);
    }
    g_object_unref(en);
}

static void
unlink_entry_pair(const char *meta_path, guint64 *running_total)
{
    gsize stem_len = strlen(meta_path) - strlen(".meta");
    char *stem = g_strndup(meta_path, stem_len);
    char *body = g_strconcat(stem, ".body", NULL);
    g_free(stem);
    GStatBuf st;
    if (running_total && g_stat(body, &st) == 0)
        *running_total -= (guint64)st.st_size;
    g_unlink(body);
    if (running_total && g_stat(meta_path, &st) == 0)
        *running_total -= (guint64)st.st_size;
    g_unlink(meta_path);
    g_free(body);
}

static void
evict_aged_out(void)
{
    if (!nd_cache_enabled()) return;
    GFile *root = g_file_new_for_path(g_cache_dir);
    GArray *metas = g_array_new(FALSE, FALSE, sizeof(cache_file));
    guint64 total = 0;
    collect_meta_files(root, metas, &total);
    g_object_unref(root);
    gint64 cutoff = now_seconds() - ND_CACHE_MAX_AGE_SECONDS;
    for (guint i = 0; i < metas->len; i++) {
        cache_file *f = &g_array_index(metas, cache_file, i);
        if (f->mtime < cutoff)
            unlink_entry_pair(f->path, NULL);
        g_free(f->path);
    }
    g_array_free(metas, TRUE);
}

static void
evict_to_cap(void)
{
    if (!nd_cache_enabled()) return;
    evict_aged_out();
    GFile *root = g_file_new_for_path(g_cache_dir);
    GArray *metas = g_array_new(FALSE, FALSE, sizeof(cache_file));
    guint64 total = 0;
    collect_meta_files(root, metas, &total);
    g_object_unref(root);
    if (total <= cache_cap_bytes()) {
        for (guint i = 0; i < metas->len; i++)
            g_free(g_array_index(metas, cache_file, i).path);
        g_array_free(metas, TRUE);
        return;
    }
    g_array_sort(metas, cmp_file_mtime);
    for (guint i = 0; i < metas->len && total > cache_cap_bytes(); i++) {
        cache_file *f = &g_array_index(metas, cache_file, i);
        if (!g_str_has_suffix(f->path, ".meta")) continue;
        unlink_entry_pair(f->path, &total);
    }
    for (guint i = 0; i < metas->len; i++)
        g_free(g_array_index(metas, cache_file, i).path);
    g_array_free(metas, TRUE);
}

static gboolean
url_should_cache(const char *url)
{
    if (!url) return FALSE;
    if (!nd_url_is_http_or_https(url)) return FALSE;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p < 0x20 || *p == 0x7F) return FALSE;
    return TRUE;
}

void
nd_cache_put(const char *url,
             const char *partition,
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
    if (cache_control && (strstr(cache_control, "no-store") ||
                          strstr(cache_control, "private"))) return;
    if (!is_cacheable_status(status)) return;
    gint64 expires_at = freshness_from_headers(cache_control, expires_header);
    if (expires_at < 0) return;
    char *key       = key_for_url(url, partition);
    char *meta_path = meta_path_for_key(key);
    char *body_path = body_path_for_key(key);
    write_meta(meta_path, url, final_url, status, content_type,
               etag, last_modified, expires_at, now_seconds());
    write_file_encrypted(body_path, body ? body : "", body_len);
    g_free(key); g_free(meta_path); g_free(body_path);
    evict_to_cap();
}

void
nd_cache_promote_304(const char *url,
                     const char *partition,
                     const char *cache_control,
                     const char *expires_header)
{
    if (!nd_cache_enabled() || !url_should_cache(url)) return;
    char *key       = key_for_url(url, partition);
    char *meta_path = meta_path_for_key(key);
    char *body_path = body_path_for_key(key);
    nd_cache_entry *e = read_meta(url, meta_path);
    if (!e) {
        g_free(key); g_free(meta_path); g_free(body_path);
        return;
    }
    gint64 expires_at = freshness_from_headers(cache_control, expires_header);
    if (expires_at < 0) expires_at = 0;
    write_meta(meta_path, url, e->final_url, e->status, e->content_type,
               e->etag, e->last_modified, expires_at, now_seconds());
    touch_paths(meta_path, body_path);
    g_free(key); g_free(meta_path); g_free(body_path);
    nd_cache_entry_free(e);
}
