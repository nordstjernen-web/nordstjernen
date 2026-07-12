/* Nordstjernen — @font-face web font loader.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "font.h"

#include <gio/gio.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <string.h>

#include "net.h"
#include "paint.h"

#ifdef NS_HAVE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>
#endif
#ifdef NS_HAVE_PANGOFT2
#include <pango/pangofc-fontmap.h>
#define NS_HAVE_PANGOFC 1
#endif
#ifdef NS_HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H
#endif

typedef struct ns_font_entry {
    char *family;
    char *url;
    gboolean loaded;
    gboolean inflight;
    GCancellable *cancel;
} ns_font_entry;

typedef struct ns_font_pending {
    GPtrArray *families;
    GCancellable *cancel;
} ns_font_pending;

static GHashTable        *g_entries;
static GHashTable        *g_pending_by_url;
static char              *g_cache_dir;
static ns_font_loaded_cb  g_loaded_cb;
static gpointer           g_loaded_ud;

static void
ns_font_pending_free(gpointer data)
{
    ns_font_pending *p = data;
    if (!p) return;
    if (p->cancel) g_object_unref(p->cancel);
    if (p->families) g_ptr_array_free(p->families, TRUE);
    g_free(p);
}

static char *
ns_font_entry_key(const char *family, const char *url)
{
    return g_strdup_printf("%s\x1f%s", family, url ? url : "");
}

void
ns_font_init(void)
{
    if (g_entries) return;
    g_entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_pending_by_url = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, ns_font_pending_free);
    const char *xdg = g_getenv("XDG_CACHE_HOME");
    char *base = xdg && *xdg
        ? g_strdup(xdg)
        : g_build_filename(g_get_home_dir(), ".cache", NULL);
    g_cache_dir = g_build_filename(base, "nordstjernen", "webfonts", NULL);
    g_free(base);
    g_mkdir_with_parents(g_cache_dir, 0700);
    ns_paint_register_font_oracle();
}

void
ns_font_shutdown(void)
{
    if (!g_entries) return;
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_entries);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        ns_font_entry *e = v;
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
    if (g_pending_by_url) {
        g_hash_table_destroy(g_pending_by_url);
        g_pending_by_url = NULL;
    }
    g_free(g_cache_dir);
    g_cache_dir = NULL;
}

gboolean
ns_font_available(void)
{
#ifdef NS_HAVE_FONTCONFIG
    return TRUE;
#else
    return FALSE;
#endif
}

gboolean
ns_font_family_loaded(const char *family)
{
    if (!family || !g_entries) return FALSE;
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_entries);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        ns_font_entry *e = v;
        if (e->loaded && e->family && strcmp(e->family, family) == 0)
            return TRUE;
    }
    return FALSE;
}

static const char *
ns_font_extension_for(const char *url, char *buf, gsize buflen)
{
    if (!url) return ".bin";
    const char *q = strchr(url, '?');
    gsize end = q ? (gsize)(q - url) : strlen(url);
    const char *frag = memchr(url, '#', end);
    if (frag) end = (gsize)(frag - url);
    gsize i = end;
    while (i > 0 && url[i - 1] != '.' && url[i - 1] != '/') i--;
    if (i == 0 || url[i - 1] != '.') return ".bin";
    gsize len = end - i;
    if (len + 2 > buflen) return ".bin";
    buf[0] = '.';
    for (gsize j = 0; j < len; j++) buf[j + 1] = g_ascii_tolower(url[i + j]);
    buf[len + 1] = '\0';
    return buf;
}

static gboolean
ns_font_bytes_look_like_font(const guint8 *d, gsize len)
{
    if (!d || len < 4) return FALSE;
    static const guint8 sigs[][4] = {
        { 0x00, 0x01, 0x00, 0x00 },
        { 'O', 'T', 'T', 'O' },
        { 't', 'r', 'u', 'e' },
        { 't', 'y', 'p', '1' },
        { 't', 't', 'c', 'f' },
        { 'w', 'O', 'F', 'F' },
        { 'w', 'O', 'F', '2' },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(sigs); i++)
        if (memcmp(d, sigs[i], 4) == 0) return TRUE;
    return FALSE;
}

#ifdef NS_HAVE_FREETYPE
static gboolean
ns_font_bytes_are_woff(const guint8 *d, gsize len)
{
    if (!d || len < 4) return FALSE;
    return d[0] == 'w' && d[1] == 'O' && d[2] == 'F' &&
           (d[3] == 'F' || d[3] == '2');
}

static void
ns_put_be16(guint8 *p, guint16 v) { p[0] = v >> 8; p[1] = (guint8)v; }

static void
ns_put_be32(guint8 *p, guint32 v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (guint8)v;
}

static guint8 *
ns_font_woff_to_sfnt(const guint8 *data, gsize len, gsize *out_len,
                     gboolean *out_cff)
{
    FT_Library lib;
    if (FT_Init_FreeType(&lib)) return NULL;
    FT_Face face;
    if (FT_New_Memory_Face(lib, data, (FT_Long)len, 0, &face)) {
        FT_Done_FreeType(lib);
        return NULL;
    }

    typedef struct { FT_ULong tag, len, off; } ns_sfnt_tab;
    guint8 *buf = NULL;
    FT_ULong num = 0;
    if (FT_Sfnt_Table_Info(face, 0, NULL, &num) || num == 0 || num > 4096)
        goto out;

    ns_sfnt_tab *tabs = g_new0(ns_sfnt_tab, num);
    gboolean cff = FALSE, bad = FALSE;
    for (FT_UInt i = 0; i < num; i++) {
        FT_ULong tag = 0, tlen = 0;
        if (FT_Sfnt_Table_Info(face, i, &tag, &tlen)) { bad = TRUE; break; }
        tabs[i].tag = tag;
        tabs[i].len = tlen;
        if (tag == FT_MAKE_TAG('C', 'F', 'F', ' ')) cff = TRUE;
    }

    if (!bad) {
        guint16 es = 0;
        while ((1u << (es + 1)) <= num) es++;
        guint16 sr = (guint16)((1u << es) * 16);
        guint16 rs = (guint16)(num * 16 - sr);
        gsize off = 12 + (gsize)num * 16;
        const gsize max_sfnt = 64u * 1024u * 1024u;
        gboolean too_big = FALSE;
        for (FT_UInt i = 0; i < num; i++) {
            tabs[i].off = off;
            if (tabs[i].len > max_sfnt) {
                too_big = TRUE;
                break;
            }
            gsize padded = ((gsize)tabs[i].len + 3) & ~(gsize)3;
            if (padded > max_sfnt || off > max_sfnt - padded) {
                too_big = TRUE;
                break;
            }
            off += padded;
        }
        buf = too_big ? NULL : g_try_malloc0(off);
        if (buf) {
            ns_put_be32(buf, cff ? FT_MAKE_TAG('O', 'T', 'T', 'O') : 0x00010000u);
            ns_put_be16(buf + 4, (guint16)num);
            ns_put_be16(buf + 6, sr);
            ns_put_be16(buf + 8, es);
            ns_put_be16(buf + 10, rs);
            for (FT_UInt i = 0; i < num; i++) {
                FT_ULong tlen = tabs[i].len;
                if (FT_Load_Sfnt_Table(face, tabs[i].tag, 0,
                                       buf + tabs[i].off, &tlen)) {
                    g_clear_pointer(&buf, g_free);
                    break;
                }
                guint32 sum = 0;
                gsize padded = (tabs[i].len + 3) & ~(gsize)3;
                for (gsize k = 0; k < padded; k += 4) {
                    const guint8 *q = buf + tabs[i].off + k;
                    sum += ((guint32)q[0] << 24) | ((guint32)q[1] << 16) |
                           ((guint32)q[2] << 8) | q[3];
                }
                guint8 *dir = buf + 12 + (gsize)i * 16;
                ns_put_be32(dir, tabs[i].tag);
                ns_put_be32(dir + 4, sum);
                ns_put_be32(dir + 8, (guint32)tabs[i].off);
                ns_put_be32(dir + 12, (guint32)tabs[i].len);
            }
        }
        if (buf) {
            if (out_len) *out_len = off;
            if (out_cff) *out_cff = cff;
        }
    }
    g_free(tabs);

out:
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    return buf;
}
#endif /* NS_HAVE_FREETYPE */

static char *
ns_font_cache_path_for(const char *family, const char *url,
                       const char *forced_ext)
{
    if (!g_cache_dir || !family) return NULL;
    char *sanitized = g_strdup(family);
    for (char *p = sanitized; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_') *p = '_';
    char extbuf[12];
    const char *ext = forced_ext ? forced_ext
                                 : ns_font_extension_for(url, extbuf, sizeof extbuf);
    char *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256,
                                                 url ? url : "", -1);
    char tag[65];
    g_strlcpy(tag, digest ? digest : "0", sizeof tag);
    g_free(digest);
    char *name = g_strdup_printf("%s-%s%s", sanitized, tag, ext);
    g_free(sanitized);
    char *full = g_build_filename(g_cache_dir, name, NULL);
    g_free(name);
    return full;
}

typedef struct ns_font_fetch_ctx {
    char *url;
} ns_font_fetch_ctx;

#ifdef NS_HAVE_FONTCONFIG
static void
ns_font_install_file(const char *path, const char *css_family)
{
    if (!path) return;
    FcConfigAppFontAddFile(NULL, (const FcChar8 *)path);
    if (css_family && *css_family) {
        int count = 0;
        FcPattern *pat = FcFreeTypeQuery((const FcChar8 *)path, 0, NULL, &count);
        if (pat) {
            FcChar8 *internal = NULL;
            if (FcPatternGetString(pat, FC_FAMILY, 0, &internal) == FcResultMatch &&
                internal &&
                g_ascii_strcasecmp((const char *)internal, css_family) != 0) {
                char *fam = g_markup_escape_text(css_family, -1);
                char *intl = g_markup_escape_text((const char *)internal, -1);
                char *xml = g_strdup_printf(
                    "<?xml version=\"1.0\"?>\n"
                    "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
                    "<fontconfig><alias binding=\"strong\"><family>%s</family>"
                    "<prefer><family>%s</family></prefer></alias></fontconfig>",
                    fam, intl);
                FcConfigParseAndLoadFromMemory(FcConfigGetCurrent(),
                                               (const FcChar8 *)xml, FcTrue);
                g_free(xml);
                g_free(fam);
                g_free(intl);
            }
            FcPatternDestroy(pat);
        }
    }
#ifdef NS_HAVE_PANGOFC
    PangoFontMap *fm = pango_cairo_font_map_get_default();
    if (fm && PANGO_IS_FC_FONT_MAP(fm))
        pango_fc_font_map_config_changed(PANGO_FC_FONT_MAP(fm));
#endif
}
#endif

static void
ns_font_on_fetched(GObject *src, GAsyncResult *res, gpointer user_data)
{
    (void)src;
    ns_font_fetch_ctx *ctx = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(res, &err);
    ns_font_pending *pending = g_pending_by_url
        ? g_hash_table_lookup(g_pending_by_url, ctx->url) : NULL;
    GPtrArray *families = pending ? pending->families : NULL;
    if (families) {
        for (guint i = 0; i < families->len; i++) {
            const char *family = g_ptr_array_index(families, i);
            char *key = ns_font_entry_key(family, ctx->url);
            ns_font_entry *e = g_entries ? g_hash_table_lookup(g_entries, key)
                                         : NULL;
            g_free(key);
            if (!e) continue;
            g_clear_object(&e->cancel);
            e->inflight = FALSE;
        }
    }
    if (resp && !resp->error && resp->status < 400 &&
        resp->body && resp->body->len > 0 &&
        ns_font_bytes_look_like_font(resp->body->data, resp->body->len)) {
        const guint8 *write_data = resp->body->data;
        gsize write_len = resp->body->len;
        const char *forced_ext = NULL;
        guint8 *converted = NULL;
#ifdef NS_HAVE_FREETYPE
        if (ns_font_bytes_are_woff(resp->body->data, resp->body->len)) {
            gsize clen = 0;
            gboolean cff = FALSE;
            converted = ns_font_woff_to_sfnt(resp->body->data,
                                             resp->body->len, &clen, &cff);
            if (converted) {
                write_data = converted;
                write_len = clen;
                forced_ext = cff ? ".otf" : ".ttf";
            }
        }
#endif
        if (families) {
            for (guint i = 0; i < families->len; i++) {
                const char *family = g_ptr_array_index(families, i);
                char *ekey = ns_font_entry_key(family, ctx->url);
                ns_font_entry *e = g_entries
                    ? g_hash_table_lookup(g_entries, ekey) : NULL;
                g_free(ekey);
                char *path = ns_font_cache_path_for(family,
                                                    e ? e->url
                                                      : (resp->final_url ? resp->final_url
                                                                         : ctx->url),
                                                    forced_ext);
                if (!path) continue;
                GError *werr = NULL;
                if (g_file_set_contents(path, (const char *)write_data,
                                        (gssize)write_len, &werr)) {
#ifdef NS_HAVE_FONTCONFIG
                    ns_font_install_file(path, family);
#endif
                    if (e) e->loaded = TRUE;
                    if (g_loaded_cb) g_loaded_cb(family, g_loaded_ud);
                }
                g_clear_error(&werr);
                g_free(path);
            }
        }
        g_free(converted);
    }
    if (g_pending_by_url) g_hash_table_remove(g_pending_by_url, ctx->url);
    ns_response_free(resp);
    g_clear_error(&err);
    g_free(ctx->url);
    g_free(ctx);
}

void
ns_font_request(const char *family, const char *src_url, const char *base_url)
{
    if (!ns_font_available()) return;
    if (!g_entries) ns_font_init();
    if (!family || !*family || !src_url || !*src_url) return;

    char *abs = base_url ? ns_url_resolve(base_url, src_url) : g_strdup(src_url);
    if (!abs) return;

    char *key = ns_font_entry_key(family, abs);
    ns_font_entry *existing = g_hash_table_lookup(g_entries, key);
    if (existing) {
        g_free(key);
        if (existing->loaded || existing->inflight) { g_free(abs); return; }
        g_free(abs);
    } else {
        existing = g_new0(ns_font_entry, 1);
        existing->family = g_strdup(family);
        existing->url = abs;
        g_hash_table_insert(g_entries, key, existing);
    }

    ns_font_pending *pending = g_pending_by_url
        ? g_hash_table_lookup(g_pending_by_url, existing->url) : NULL;
    if (pending) {
        gboolean seen = FALSE;
        for (guint i = 0; i < pending->families->len; i++) {
            const char *f = g_ptr_array_index(pending->families, i);
            if (strcmp(f, family) == 0) { seen = TRUE; break; }
        }
        if (!seen) g_ptr_array_add(pending->families, g_strdup(family));
        existing->inflight = TRUE;
        g_set_object(&existing->cancel, pending->cancel);
        return;
    }

    pending = g_new0(ns_font_pending, 1);
    pending->families = g_ptr_array_new_with_free_func(g_free);
    pending->cancel = g_cancellable_new();
    g_ptr_array_add(pending->families, g_strdup(family));
    if (g_pending_by_url)
        g_hash_table_insert(g_pending_by_url, g_strdup(existing->url), pending);

    existing->inflight = TRUE;
    g_set_object(&existing->cancel, pending->cancel);

    ns_font_fetch_ctx *ctx = g_new0(ns_font_fetch_ctx, 1);
    ctx->url = g_strdup(existing->url);
    ns_net_fetch_async(existing->url, base_url, existing->cancel,
                       ns_font_on_fetched, ctx);
}
