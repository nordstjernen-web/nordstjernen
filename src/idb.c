/* Nordstjernen — SQLite-backed IndexedDB storage.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "idb.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <string.h>

#include "config.h"
#include "js.h"

typedef struct ns_idb_db {
    sqlite3 *db;
    char    *path;
    guint64  last_used;
} ns_idb_db;

static JSValue ns_idb_throw(JSContext *ctx, const char *name, const char *message);

#define NS_IDB_MAX_OPEN 16

static GHashTable *g_idb_handles;
static guint64     g_idb_clock;

static void
ns_idb_db_free(ns_idb_db *h)
{
    if (!h) return;
    if (h->db) sqlite3_close(h->db);
    g_free(h->path);
    g_free(h);
}

static void
ns_idb_db_close(ns_idb_db *h)
{
    (void)h;
}

static void
ns_idb_cache_evict(const char *key)
{
    if (g_idb_handles && key)
        g_hash_table_remove(g_idb_handles, key);
}

static void
ns_idb_cache_trim(void)
{
    if (!g_idb_handles ||
        g_hash_table_size(g_idb_handles) <= NS_IDB_MAX_OPEN)
        return;
    GHashTableIter it;
    gpointer k, v;
    const char *lru_disk = NULL, *lru_mem = NULL;
    guint64 lru_disk_stamp = G_MAXUINT64, lru_mem_stamp = G_MAXUINT64;
    g_hash_table_iter_init(&it, g_idb_handles);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        ns_idb_db *h = v;
        if (h->path && strcmp(h->path, ":memory:") == 0) {
            if (h->last_used < lru_mem_stamp) {
                lru_mem_stamp = h->last_used;
                lru_mem = k;
            }
        } else if (h->last_used < lru_disk_stamp) {
            lru_disk_stamp = h->last_used;
            lru_disk = k;
        }
    }
    const char *victim = lru_disk ? lru_disk : lru_mem;
    if (victim)
        g_hash_table_remove(g_idb_handles, victim);
}

static JSValue
ns_idb_throw(JSContext *ctx, const char *name, const char *message)
{
    return JS_ThrowDOMException(ctx, name ? name : "UnknownError",
                                "%s", message ? message : "");
}

static JSValue
ns_idb_throw_sql(JSContext *ctx, sqlite3 *db)
{
    return ns_idb_throw(ctx, "UnknownError", db ? sqlite3_errmsg(db) : "SQLite error");
}

static void
ns_idb_free_cstrings(JSContext *ctx, int n, ...)
{
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        const char *s = va_arg(ap, const char *);
        if (s) JS_FreeCString(ctx, s);
    }
    va_end(ap);
}

static char *
ns_idb_hash_string(const char *input)
{
    return g_compute_checksum_for_string(G_CHECKSUM_SHA256, input, -1);
}

static char *
ns_idb_partition_dir(JSContext *ctx)
{
    const ns_config *cfg = ns_config_get();
    if (cfg && cfg->private_mode) return NULL;
    ns_js *js = JS_GetContextOpaque(ctx);
    const char *partition = ns_js_storage_partition(js);
    if (!partition || !*partition) return NULL;
    g_autofree char *hash = ns_idb_hash_string(partition);
    char *dir = g_build_filename(g_get_user_data_dir(), NS_APP_DIR_NAME,
                                 "indexeddb", hash, NULL);
    g_mkdir_with_parents(dir, 0700);
    g_chmod(dir, 0700);
    return dir;
}

static char *
ns_idb_path_for_name(JSContext *ctx, const char *name)
{
    if (!name || !*name) return NULL;
    const ns_config *cfg = ns_config_get();
    if (cfg && cfg->private_mode) return g_strdup(":memory:");
    g_autofree char *dir = ns_idb_partition_dir(ctx);
    if (!dir) return NULL;
    g_autofree char *hash = ns_idb_hash_string(name);
    g_autofree char *file = g_strdup_printf("%s.sqlite", hash);
    return g_build_filename(dir, file, NULL);
}

static gboolean
ns_idb_exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK)
        g_warning("idb: sqlite exec failed: %s", err ? err : sqlite3_errstr(rc));
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

#define NS_IDB_MAX_PAGES 65536
#define NS_IDB_MAX_ORIGIN_PAGES (256 * 1024)

static void
ns_idb_configure(sqlite3 *db)
{
#ifdef SQLITE_DBCONFIG_DEFENSIVE
    sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL);
#endif
    sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, NULL);
#ifdef SQLITE_DBCONFIG_TRUSTED_SCHEMA
    sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, NULL);
#endif
#ifdef SQLITE_DBCONFIG_DQS_DDL
    sqlite3_db_config(db, SQLITE_DBCONFIG_DQS_DDL, 0, NULL);
#endif
#ifdef SQLITE_DBCONFIG_DQS_DML
    sqlite3_db_config(db, SQLITE_DBCONFIG_DQS_DML, 0, NULL);
#endif
}

static gboolean
ns_idb_schema(sqlite3 *db)
{
    return ns_idb_exec(db, "PRAGMA foreign_keys=ON") &&
           ns_idb_exec(db, "PRAGMA journal_mode=WAL") &&
           ns_idb_exec(db, "PRAGMA synchronous=NORMAL") &&
           ns_idb_exec(db, "PRAGMA cache_size=-512") &&
           ns_idb_exec(db, "PRAGMA max_page_count=" G_STRINGIFY(NS_IDB_MAX_PAGES)) &&
           ns_idb_exec(db, "CREATE TABLE IF NOT EXISTS meta("
                           "key TEXT PRIMARY KEY,value TEXT NOT NULL)") &&
           ns_idb_exec(db, "CREATE TABLE IF NOT EXISTS stores("
                           "name TEXT PRIMARY KEY,"
                           "key_path TEXT NOT NULL,"
                           "auto_increment INTEGER NOT NULL,"
                           "key_gen INTEGER NOT NULL DEFAULT 1)") &&
           ns_idb_exec(db, "CREATE TABLE IF NOT EXISTS records("
                           "store TEXT NOT NULL,"
                           "key TEXT NOT NULL,"
                           "value BLOB NOT NULL,"
                           "PRIMARY KEY(store,key),"
                           "FOREIGN KEY(store) REFERENCES stores(name) "
                           "ON DELETE CASCADE ON UPDATE CASCADE)") &&
           ns_idb_exec(db, "CREATE TABLE IF NOT EXISTS indexes("
                           "store TEXT NOT NULL,"
                           "name TEXT NOT NULL,"
                           "key_path TEXT NOT NULL,"
                           "unique_index INTEGER NOT NULL,"
                           "multi_entry INTEGER NOT NULL,"
                           "PRIMARY KEY(store,name),"
                           "FOREIGN KEY(store) REFERENCES stores(name) "
                           "ON DELETE CASCADE ON UPDATE CASCADE)") &&
           ns_idb_exec(db, "CREATE TABLE IF NOT EXISTS index_records("
                           "store TEXT NOT NULL,"
                           "name TEXT NOT NULL,"
                           "index_key TEXT NOT NULL,"
                           "primary_key TEXT NOT NULL,"
                           "PRIMARY KEY(store,name,index_key,primary_key),"
                           "FOREIGN KEY(store,name) REFERENCES indexes(store,name) "
                           "ON DELETE CASCADE ON UPDATE CASCADE,"
                           "FOREIGN KEY(store,primary_key) REFERENCES records(store,key) "
                           "ON DELETE CASCADE ON UPDATE CASCADE)") &&
           ns_idb_exec(db, "CREATE INDEX IF NOT EXISTS idx_records_store "
                           "ON records(store)") &&
           ns_idb_exec(db, "CREATE INDEX IF NOT EXISTS idx_index_records_lookup "
                           "ON index_records(store,name,index_key)");
}

#ifdef SQLITE_OPEN_NOFOLLOW
#define NS_IDB_OPEN_FLAGS \
    (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW)
#else
#define NS_IDB_OPEN_FLAGS (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
#endif

static char *
ns_idb_cache_key(JSContext *ctx, const char *name)
{
    ns_js *js = JS_GetContextOpaque(ctx);
    const char *partition = ns_js_storage_partition(js);
    if (!partition || !*partition || !name || !*name) return NULL;
    return g_strdup_printf("%s\x1f%s", partition, name);
}

static ns_idb_db *
ns_idb_open_db(JSContext *ctx, const char *name)
{
    g_autofree char *key = ns_idb_cache_key(ctx, name);
    if (!key) return NULL;
    if (g_idb_handles) {
        ns_idb_db *cached = g_hash_table_lookup(g_idb_handles, key);
        if (cached) {
            cached->last_used = ++g_idb_clock;
            return cached;
        }
    }
    g_autofree char *path = ns_idb_path_for_name(ctx, name);
    if (!path) return NULL;
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, NS_IDB_OPEN_FLAGS, NULL);
    if (rc != SQLITE_OK) {
        g_warning("idb: could not open %s: %s", path,
                  db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db) sqlite3_close(db);
        return NULL;
    }
    ns_idb_configure(db);
    sqlite3_busy_timeout(db, 2500);
    if (!ns_idb_schema(db)) {
        sqlite3_close(db);
        return NULL;
    }
    ns_idb_db *h = g_new0(ns_idb_db, 1);
    h->db = db;
    h->path = g_strdup(path);
    h->last_used = ++g_idb_clock;
    if (!g_idb_handles)
        g_idb_handles = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, (GDestroyNotify)ns_idb_db_free);
    g_hash_table_insert(g_idb_handles, g_steal_pointer(&key), h);
    ns_idb_cache_trim();
    return h;
}

static gboolean
ns_idb_set_meta(sqlite3 *db, const char *key, const char *value)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO meta(key,value) VALUES(?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        -1, &st, NULL) != SQLITE_OK)
        return FALSE;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value ? value : "", -1, SQLITE_TRANSIENT);
    gboolean ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static char *
ns_idb_get_meta(sqlite3 *db, const char *key)
{
    sqlite3_stmt *st = NULL;
    char *out = NULL;
    if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key=?",
                           -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s = sqlite3_column_text(st, 0);
        if (s) out = g_strdup((const char *)s);
    }
    sqlite3_finalize(st);
    return out;
}

static int64_t
ns_idb_get_version(sqlite3 *db)
{
    g_autofree char *v = ns_idb_get_meta(db, "version");
    if (!v || !*v) return 0;
    return g_ascii_strtoll(v, NULL, 10);
}

static JSValue
ns_idb_read_value(JSContext *ctx, const void *blob, int len)
{
    if (!blob || len <= 0) return JS_UNDEFINED;
    JSValue v = JS_ReadObject(ctx, blob, (size_t)len, JS_READ_OBJ_REFERENCE);
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }
    return v;
}

static gboolean
ns_idb_bind_value(JSContext *ctx, sqlite3_stmt *st, int index, JSValueConst value)
{
    size_t len = 0;
    uint8_t *buf = JS_WriteObject(ctx, &len, value, JS_WRITE_OBJ_REFERENCE);
    if (!buf) return FALSE;
    if (len > (size_t)G_MAXINT) { js_free(ctx, buf); return FALSE; }
    int rc = sqlite3_bind_blob(st, index, buf, (int)len, SQLITE_TRANSIENT);
    js_free(ctx, buf);
    return rc == SQLITE_OK;
}

static JSValue
ns_idb_store_array(sqlite3 *db, JSContext *ctx)
{
    JSValue arr = JS_NewArray(ctx);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT name,key_path,auto_increment,key_gen FROM stores ORDER BY name",
        -1, &st, NULL) != SQLITE_OK)
        return arr;
    uint32_t i = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        const char *kp = (const char *)sqlite3_column_text(st, 1);
        JSValue store = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, store, "name", JS_NewString(ctx, name ? name : ""));
        JS_SetPropertyStr(ctx, store, "keyPath", JS_NewString(ctx, kp ? kp : "null"));
        JS_SetPropertyStr(ctx, store, "autoIncrement",
                          JS_NewBool(ctx, sqlite3_column_int(st, 2) != 0));
        JS_SetPropertyStr(ctx, store, "keyGenerator",
                          JS_NewInt64(ctx, sqlite3_column_int64(st, 3)));
        JSValue idxs = JS_NewArray(ctx);
        sqlite3_stmt *ist = NULL;
        if (sqlite3_prepare_v2(db,
            "SELECT name,key_path,unique_index,multi_entry FROM indexes "
            "WHERE store=? ORDER BY name",
            -1, &ist, NULL) == SQLITE_OK) {
            sqlite3_bind_text(ist, 1, name ? name : "", -1, SQLITE_TRANSIENT);
            uint32_t j = 0;
            while (sqlite3_step(ist) == SQLITE_ROW) {
                const char *in = (const char *)sqlite3_column_text(ist, 0);
                const char *ikp = (const char *)sqlite3_column_text(ist, 1);
                JSValue idx = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, idx, "name", JS_NewString(ctx, in ? in : ""));
                JS_SetPropertyStr(ctx, idx, "keyPath", JS_NewString(ctx, ikp ? ikp : "null"));
                JS_SetPropertyStr(ctx, idx, "unique",
                                  JS_NewBool(ctx, sqlite3_column_int(ist, 2) != 0));
                JS_SetPropertyStr(ctx, idx, "multiEntry",
                                  JS_NewBool(ctx, sqlite3_column_int(ist, 3) != 0));
                JS_SetPropertyUint32(ctx, idxs, j++, idx);
            }
            sqlite3_finalize(ist);
        }
        JS_SetPropertyStr(ctx, store, "indexes", idxs);
        JS_SetPropertyUint32(ctx, arr, i++, store);
    }
    sqlite3_finalize(st);
    return arr;
}

static JSValue
ns_idb_info_for(JSContext *ctx, ns_idb_db *h, const char *name)
{
    g_autofree char *stored_name = ns_idb_get_meta(h->db, "name");
    if (!stored_name) ns_idb_set_meta(h->db, "name", name);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, obj, "version", JS_NewInt64(ctx, ns_idb_get_version(h->db)));
    JS_SetPropertyStr(ctx, obj, "stores", ns_idb_store_array(h->db, ctx));
    return obj;
}

static const char *
ns_idb_arg_string(JSContext *ctx, JSValueConst v)
{
    return JS_ToCString(ctx, v);
}

static JSValue
ns_idb_backend_open(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return ns_idb_throw(ctx, "TypeError", "Database name is required");
    const char *name = ns_idb_arg_string(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    ns_idb_db *h = ns_idb_open_db(ctx, name);
    if (!h) {
        JS_FreeCString(ctx, name);
        return ns_idb_throw(ctx, "UnknownError", "Could not open IndexedDB database");
    }
    JSValue out = ns_idb_info_for(ctx, h, name);
    JS_FreeCString(ctx, name);
    ns_idb_db_close(h);
    return out;
}

static JSValue
ns_idb_backend_set_version(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_FALSE;
    const char *name = JS_ToCString(ctx, argv[0]);
    int64_t version = 0;
    if (!name || JS_ToInt64(ctx, &version, argv[1]) < 0) {
        if (name) JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    ns_idb_db *h = ns_idb_open_db(ctx, name);
    JS_FreeCString(ctx, name);
    if (!h) return ns_idb_throw(ctx, "UnknownError", "Could not open IndexedDB database");
    char buf[64];
    g_snprintf(buf, sizeof(buf), "%" G_GINT64_FORMAT, version);
    gboolean ok = ns_idb_set_meta(h->db, "version", buf);
    ns_idb_db_close(h);
    return JS_NewBool(ctx, ok);
}

static JSValue
ns_idb_backend_create_store(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 4) return JS_FALSE;
    const char *dbn = JS_ToCString(ctx, argv[0]);
    const char *store = JS_ToCString(ctx, argv[1]);
    const char *key_path = JS_ToCString(ctx, argv[2]);
    gboolean auto_inc = JS_ToBool(ctx, argv[3]) > 0;
    if (!dbn || !store || !key_path) {
        ns_idb_free_cstrings(ctx, 3, dbn, store, key_path);
        return JS_EXCEPTION;
    }
    ns_idb_db *h = ns_idb_open_db(ctx, dbn);
    JS_FreeCString(ctx, dbn);
    if (!h) {
        ns_idb_free_cstrings(ctx, 2, store, key_path);
        return ns_idb_throw(ctx, "UnknownError", "Could not open IndexedDB database");
    }
    sqlite3_stmt *st = NULL;
    gboolean ok = sqlite3_prepare_v2(h->db,
        "INSERT INTO stores(name,key_path,auto_increment,key_gen) VALUES(?,?,?,1)",
        -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(st, 1, store, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, key_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, auto_inc ? 1 : 0);
        ok = sqlite3_step(st) == SQLITE_DONE;
        if (!ok && sqlite3_errcode(h->db) == SQLITE_CONSTRAINT) {
            sqlite3_finalize(st);
            ns_idb_db_close(h);
            ns_idb_free_cstrings(ctx, 2, store, key_path);
            return ns_idb_throw(ctx, "ConstraintError", "Object store already exists");
        }
    }
    if (st) sqlite3_finalize(st);
    JSValue out = ok ? JS_TRUE : ns_idb_throw_sql(ctx, h->db);
    ns_idb_db_close(h);
    ns_idb_free_cstrings(ctx, 2, store, key_path);
    return out;
}

static JSValue
ns_idb_backend_delete_store(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_FALSE;
    const char *dbn = JS_ToCString(ctx, argv[0]);
    const char *store = JS_ToCString(ctx, argv[1]);
    if (!dbn || !store) {
        ns_idb_free_cstrings(ctx, 2, dbn, store);
        return JS_EXCEPTION;
    }
    ns_idb_db *h = ns_idb_open_db(ctx, dbn);
    JS_FreeCString(ctx, dbn);
    if (!h) {
        JS_FreeCString(ctx, store);
        return ns_idb_throw(ctx, "UnknownError", "Could not open IndexedDB database");
    }
    sqlite3_stmt *st = NULL;
    gboolean ok = sqlite3_prepare_v2(h->db, "DELETE FROM stores WHERE name=?",
                                     -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(st, 1, store, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    if (st) sqlite3_finalize(st);
    JSValue out = ok ? JS_TRUE : ns_idb_throw_sql(ctx, h->db);
    ns_idb_db_close(h);
    JS_FreeCString(ctx, store);
    return out;
}

static JSValue
ns_idb_backend_create_index(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 6) return JS_FALSE;
    const char *dbn = JS_ToCString(ctx, argv[0]);
    const char *store = JS_ToCString(ctx, argv[1]);
    const char *name = JS_ToCString(ctx, argv[2]);
    const char *key_path = JS_ToCString(ctx, argv[3]);
    gboolean unique = JS_ToBool(ctx, argv[4]) > 0;
    gboolean multi = JS_ToBool(ctx, argv[5]) > 0;
    if (!dbn || !store || !name || !key_path) {
        ns_idb_free_cstrings(ctx, 4, dbn, store, name, key_path);
        return JS_EXCEPTION;
    }
    ns_idb_db *h = ns_idb_open_db(ctx, dbn);
    JS_FreeCString(ctx, dbn);
    if (!h) {
        ns_idb_free_cstrings(ctx, 3, store, name, key_path);
        return ns_idb_throw(ctx, "UnknownError", "Could not open IndexedDB database");
    }
    sqlite3_stmt *st = NULL;
    gboolean ok = sqlite3_prepare_v2(h->db,
        "INSERT INTO indexes(store,name,key_path,unique_index,multi_entry) "
        "VALUES(?,?,?,?,?)",
        -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(st, 1, store, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, key_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 4, unique ? 1 : 0);
        sqlite3_bind_int(st, 5, multi ? 1 : 0);
        ok = sqlite3_step(st) == SQLITE_DONE;
        if (!ok && sqlite3_errcode(h->db) == SQLITE_CONSTRAINT) {
            sqlite3_finalize(st);
            ns_idb_db_close(h);
            ns_idb_free_cstrings(ctx, 3, store, name, key_path);
            return ns_idb_throw(ctx, "ConstraintError", "Index already exists");
        }
    }
    if (st) sqlite3_finalize(st);
    JSValue out = ok ? JS_TRUE : ns_idb_throw_sql(ctx, h->db);
    ns_idb_db_close(h);
    ns_idb_free_cstrings(ctx, 3, store, name, key_path);
    return out;
}

static JSValue
ns_idb_backend_delete_index(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3) return JS_FALSE;
    const char *dbn = JS_ToCString(ctx, argv[0]);
    const char *store = JS_ToCString(ctx, argv[1]);
    const char *name = JS_ToCString(ctx, argv[2]);
    if (!dbn || !store || !name) {
        ns_idb_free_cstrings(ctx, 3, dbn, store, name);
        return JS_EXCEPTION;
    }
    ns_idb_db *h = ns_idb_open_db(ctx, dbn);
    JS_FreeCString(ctx, dbn);
    if (!h) {
        ns_idb_free_cstrings(ctx, 2, store, name);
        return ns_idb_throw(ctx, "UnknownError", "Could not open IndexedDB database");
    }
    sqlite3_stmt *st = NULL;
    gboolean ok = sqlite3_prepare_v2(h->db,
        "DELETE FROM indexes WHERE store=? AND name=?",
        -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(st, 1, store, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    if (st) sqlite3_finalize(st);
    JSValue out = ok ? JS_TRUE : ns_idb_throw_sql(ctx, h->db);
    ns_idb_db_close(h);
    ns_idb_free_cstrings(ctx, 2, store, name);
    return out;
}

static JSValue
ns_idb_backend_next_key(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    const char *dbn = JS_ToCString(ctx, argv[0]);
    const char *store = JS_ToCString(ctx, argv[1]);
    if (!dbn || !store) {
        ns_idb_free_cstrings(ctx, 2, dbn, store);
        return JS_EXCEPTION;
    }
    ns_idb_db *h = ns_idb_open_db(ctx, dbn);
    JS_FreeCString(ctx, dbn);
    if (!h) {
        JS_FreeCString(ctx, store);
        return ns_idb_throw(ctx, "UnknownError", "Could not open IndexedDB database");
    }
    if (sqlite3_exec(h->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        JSValue e = ns_idb_throw_sql(ctx, h->db);
        ns_idb_db_close(h);
        JS_FreeCString(ctx, store);
        return e;
    }
    sqlite3_stmt *st = NULL;
    int64_t key = 1;
    gboolean ok = sqlite3_prepare_v2(h->db,
        "SELECT key_gen FROM stores WHERE name=?",
        -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(st, 1, store, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW)
            key = sqlite3_column_int64(st, 0);
        else
            ok = FALSE;
    }
    if (st) { sqlite3_finalize(st); st = NULL; }
    if (ok && sqlite3_prepare_v2(h->db,
        "UPDATE stores SET key_gen=? WHERE name=?",
        -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, key + 1);
        sqlite3_bind_text(st, 2, store, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(st) == SQLITE_DONE;
    } else {
        ok = FALSE;
    }
    if (st) sqlite3_finalize(st);
    sqlite3_exec(h->db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    JSValue out = ok ? JS_NewInt64(ctx, key) : ns_idb_throw_sql(ctx, h->db);
    ns_idb_db_close(h);
    JS_FreeCString(ctx, store);
    return out;
}

static gboolean
ns_idb_insert_index_entries(JSContext *ctx, sqlite3 *db,
                            const char *store, const char *primary_key,
                            JSValueConst entries)
{
    if (!JS_IsArray(entries)) return TRUE;
    uint32_t len = 0;
    JSValue lv = JS_GetPropertyStr(ctx, entries, "length");
    JS_ToUint32(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO index_records(store,name,index_key,primary_key) "
        "VALUES(?,?,?,?)",
        -1, &st, NULL) != SQLITE_OK)
        return FALSE;
    gboolean ok = TRUE;
    for (uint32_t i = 0; ok && i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, entries, i);
        JSValue nv = JS_GetPropertyStr(ctx, e, "name");
        JSValue kv = JS_GetPropertyStr(ctx, e, "key");
        const char *name = JS_ToCString(ctx, nv);
        const char *key = JS_ToCString(ctx, kv);
        if (name && key) {
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);
            sqlite3_bind_text(st, 1, store, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, key, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 4, primary_key, -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(st) == SQLITE_DONE;
        } else {
            ok = FALSE;
        }
        if (name) JS_FreeCString(ctx, name);
        if (key) JS_FreeCString(ctx, key);
        JS_FreeValue(ctx, kv);
        JS_FreeValue(ctx, nv);
        JS_FreeValue(ctx, e);
    }
    sqlite3_finalize(st);
    return ok;
}

static gint64
ns_idb_db_pages(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    gint64 pages = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA page_count", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        pages = sqlite3_column_int64(st, 0);
    if (st) sqlite3_finalize(st);
    return pages;
}

static gint64
ns_idb_max_origin_pages(void)
{
    static gint64 cached = -1;
    if (cached < 0) {
        cached = NS_IDB_MAX_ORIGIN_PAGES;
        const char *env = g_getenv("NS_IDB_MAX_ORIGIN_PAGES");
        if (env && *env) {
            gint64 v = g_ascii_strtoll(env, NULL, 10);
            if (v > 0) cached = v;
        }
    }
    return cached;
}

static gint64
ns_idb_origin_pages(JSContext *ctx, sqlite3 *current, const char *current_path)
{
    gint64 total = current ? ns_idb_db_pages(current) : 0;
    g_autofree char *dir = ns_idb_partition_dir(ctx);
    if (!dir) return total;
    GDir *gd = g_dir_open(dir, 0, NULL);
    if (!gd) return total;
    const char *entry = NULL;
    while ((entry = g_dir_read_name(gd)) != NULL) {
        if (!g_str_has_suffix(entry, ".sqlite")) continue;
        g_autofree char *path = g_build_filename(dir, entry, NULL);
        if (current_path && !g_strcmp0(path, current_path)) continue;
        sqlite3 *db = NULL;
        if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            continue;
        }
        total += ns_idb_db_pages(db);
        sqlite3_close(db);
    }
    g_dir_close(gd);
    return total;
}

static JSValue
ns_idb_backend_put(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 7) return JS_FALSE;
    const char *dbn = JS_ToCString(ctx, argv[0]);
    const char *store = JS_ToCString(ctx, argv[1]);
    const char *key = JS_ToCString(ctx, argv[2]);
    gboolean add_only = JS_ToBool(ctx, argv[4]) > 0;
    double numeric_key = 0.0;
    gboolean has_numeric = FALSE;
    if (!JS_IsUndefined(argv[6])) {
        if (JS_ToFloat64(ctx, &numeric_key, argv[6]) == 0)
            has_numeric = TRUE;
        else
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    if (!dbn || !store || !key) {
        ns_idb_free_cstrings(ctx, 3, dbn, store, key);
        return JS_EXCEPTION;
    }
    ns_idb_db *h = ns_idb_open_db(ctx, dbn);
    JS_FreeCString(ctx, dbn);
    if (!h) {
        ns_idb_free_cstrings(ctx, 2, store, key);
        return ns_idb_throw(ctx, "UnknownError", "Could not open IndexedDB database");
    }
    gboolean ok = TRUE;
    if (sqlite3_exec(h->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        JSValue e = ns_idb_throw_sql(ctx, h->db);
        ns_idb_db_close(h);
        ns_idb_free_cstrings(ctx, 2, store, key);
        return e;
    }
    if (add_only) {
        sqlite3_stmt *exists = NULL;
        ok = sqlite3_prepare_v2(h->db,
            "SELECT 1 FROM records WHERE store=? AND key=?",
            -1, &exists, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(exists, 1, store, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(exists, 2, key, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(exists) == SQLITE_ROW) {
                sqlite3_finalize(exists);
                sqlite3_exec(h->db, "ROLLBACK", NULL, NULL, NULL);
                ns_idb_db_close(h);
                ns_idb_free_cstrings(ctx, 2, store, key);
                return ns_idb_throw(ctx, "ConstraintError", "Key already exists");
            }
        }
        if (exists) sqlite3_finalize(exists);
    }
    sqlite3_stmt *st = NULL;
    if (ok) {
        ok = sqlite3_prepare_v2(h->db,
            "INSERT INTO records(store,key,value) VALUES(?,?,?) "
            "ON CONFLICT(store,key) DO UPDATE SET value=excluded.value",
            -1, &st, NULL) == SQLITE_OK;
    }
    if (ok) {
        sqlite3_bind_text(st, 1, store, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, key, -1, SQLITE_TRANSIENT);
        ok = ns_idb_bind_value(ctx, st, 3, argv[3]);
        if (ok) ok = sqlite3_step(st) == SQLITE_DONE;
    }
    if (st) { sqlite3_finalize(st); st = NULL; }
    if (ok && sqlite3_prepare_v2(h->db,
        "DELETE FROM index_records WHERE store=? AND primary_key=?",
        -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, store, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, key, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(st) == SQLITE_DONE;
    } else {
        ok = FALSE;
    }
    if (st) {
        sqlite3_finalize(st);
        st = NULL;
    }
    if (ok) ok = ns_idb_insert_index_entries(ctx, h->db, store, key, argv[5]);
    if (ok && has_numeric && numeric_key >= 1.0 &&
        sqlite3_prepare_v2(h->db,
            "UPDATE stores SET key_gen=max(key_gen, ?) WHERE name=?",
            -1, &st, NULL) == SQLITE_OK) {
        double next_gen = numeric_key + 1.0;
        sqlite3_int64 bound = next_gen >= 9223372036854775807.0
                                  ? G_MAXINT64
                                  : (sqlite3_int64)next_gen;
        sqlite3_bind_int64(st, 1, bound);
        sqlite3_bind_text(st, 2, store, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    if (st) sqlite3_finalize(st);
    if (ok && ns_idb_origin_pages(ctx, h->db, h->path) > ns_idb_max_origin_pages()) {
        ns_idb_throw(ctx, "QuotaExceededError",
                     "IndexedDB origin storage limit reached");
        ok = FALSE;
    }
    sqlite3_exec(h->db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    JSValue out = ok ? JS_TRUE
                     : (JS_HasException(ctx) ? JS_EXCEPTION
                                             : ns_idb_throw_sql(ctx, h->db));
    ns_idb_db_close(h);
    ns_idb_free_cstrings(ctx, 2, store, key);
    return out;
}

static JSValue
ns_idb_backend_get(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3) return JS_UNDEFINED;
    const char *dbn = JS_ToCString(ctx, argv[0]);
    const char *store = JS_ToCString(ctx, argv[1]);
    const char *key = JS_ToCString(ctx, argv[2]);
    if (!dbn || !store || !key) {
        ns_idb_free_cstrings(ctx, 3, dbn, store, key);
        return JS_EXCEPTION;
    }
    ns_idb_db *h = ns_idb_open_db(ctx, dbn);
    JS_FreeCString(ctx, dbn);
    if (!h) {
        ns_idb_free_cstrings(ctx, 2, store, key);
        return ns_idb_throw(ctx, "UnknownError", "Could not open IndexedDB database");
    }
    sqlite3_stmt *st = NULL;
    JSValue out = JS_UNDEFINED;
    if (sqlite3_prepare_v2(h->db,
        "SELECT value FROM records WHERE store=? AND key=?",
        -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, store, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW)
            out = ns_idb_read_value(ctx, sqlite3_column_blob(st, 0),
                                    sqlite3_column_bytes(st, 0));
    }
    if (st) sqlite3_finalize(st);
    ns_idb_db_close(h);
    ns_idb_free_cstrings(ctx, 2, store, key);
    return out;
}

static JSValue
ns_idb_backend_records(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_NewArray(ctx);
    const char *dbn = JS_ToCString(ctx, argv[0]);
    const char *store = JS_ToCString(ctx, argv[1]);
    if (!dbn || !store) {
        ns_idb_free_cstrings(ctx, 2, dbn, store);
        return JS_EXCEPTION;
    }
    ns_idb_db *h = ns_idb_open_db(ctx, dbn);
    JS_FreeCString(ctx, dbn);
    if (!h) {
        JS_FreeCString(ctx, store);
        return ns_idb_throw(ctx, "UnknownError", "Could not open IndexedDB database");
    }
    JSValue arr = JS_NewArray(ctx);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h->db,
        "SELECT key,value FROM records WHERE store=?",
        -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, store, -1, SQLITE_TRANSIENT);
        uint32_t i = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *key = (const char *)sqlite3_column_text(st, 0);
            JSValue rec = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, rec, "key", JS_NewString(ctx, key ? key : ""));
            JS_SetPropertyStr(ctx, rec, "value",
                ns_idb_read_value(ctx, sqlite3_column_blob(st, 1),
                                  sqlite3_column_bytes(st, 1)));
            JS_SetPropertyUint32(ctx, arr, i++, rec);
        }
    }
    if (st) sqlite3_finalize(st);
    ns_idb_db_close(h);
    JS_FreeCString(ctx, store);
    return arr;
}

static JSValue
ns_idb_backend_index_records(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3) return JS_NewArray(ctx);
    const char *dbn = JS_ToCString(ctx, argv[0]);
    const char *store = JS_ToCString(ctx, argv[1]);
    const char *index = JS_ToCString(ctx, argv[2]);
    if (!dbn || !store || !index) {
        ns_idb_free_cstrings(ctx, 3, dbn, store, index);
        return JS_EXCEPTION;
    }
    ns_idb_db *h = ns_idb_open_db(ctx, dbn);
    JS_FreeCString(ctx, dbn);
    if (!h) {
        ns_idb_free_cstrings(ctx, 2, store, index);
        return ns_idb_throw(ctx, "UnknownError", "Could not open IndexedDB database");
    }
    JSValue arr = JS_NewArray(ctx);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h->db,
        "SELECT ir.index_key,ir.primary_key,r.value "
        "FROM index_records ir JOIN records r "
        "ON r.store=ir.store AND r.key=ir.primary_key "
        "WHERE ir.store=? AND ir.name=?",
        -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, store, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, index, -1, SQLITE_TRANSIENT);
        uint32_t i = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *ik = (const char *)sqlite3_column_text(st, 0);
            const char *pk = (const char *)sqlite3_column_text(st, 1);
            JSValue rec = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, rec, "key",
                JS_NewString(ctx, ik ? ik : ""));
            JS_SetPropertyStr(ctx, rec, "primaryKey",
                JS_NewString(ctx, pk ? pk : ""));
            JS_SetPropertyStr(ctx, rec, "value",
                ns_idb_read_value(ctx, sqlite3_column_blob(st, 2),
                                  sqlite3_column_bytes(st, 2)));
            JS_SetPropertyUint32(ctx, arr, i++, rec);
        }
    }
    if (st) sqlite3_finalize(st);
    ns_idb_db_close(h);
    ns_idb_free_cstrings(ctx, 2, store, index);
    return arr;
}

static JSValue
ns_idb_backend_delete_record(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3) return JS_FALSE;
    const char *dbn = JS_ToCString(ctx, argv[0]);
    const char *store = JS_ToCString(ctx, argv[1]);
    const char *key = JS_ToCString(ctx, argv[2]);
    if (!dbn || !store || !key) {
        ns_idb_free_cstrings(ctx, 3, dbn, store, key);
        return JS_EXCEPTION;
    }
    ns_idb_db *h = ns_idb_open_db(ctx, dbn);
    JS_FreeCString(ctx, dbn);
    if (!h) {
        ns_idb_free_cstrings(ctx, 2, store, key);
        return ns_idb_throw(ctx, "UnknownError", "Could not open IndexedDB database");
    }
    sqlite3_stmt *st = NULL;
    gboolean ok = sqlite3_prepare_v2(h->db,
        "DELETE FROM records WHERE store=? AND key=?",
        -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(st, 1, store, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, key, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    if (st) sqlite3_finalize(st);
    JSValue out = ok ? JS_TRUE : ns_idb_throw_sql(ctx, h->db);
    ns_idb_db_close(h);
    ns_idb_free_cstrings(ctx, 2, store, key);
    return out;
}

static JSValue
ns_idb_backend_clear(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_FALSE;
    const char *dbn = JS_ToCString(ctx, argv[0]);
    const char *store = JS_ToCString(ctx, argv[1]);
    if (!dbn || !store) {
        ns_idb_free_cstrings(ctx, 2, dbn, store);
        return JS_EXCEPTION;
    }
    ns_idb_db *h = ns_idb_open_db(ctx, dbn);
    JS_FreeCString(ctx, dbn);
    if (!h) {
        JS_FreeCString(ctx, store);
        return ns_idb_throw(ctx, "UnknownError", "Could not open IndexedDB database");
    }
    sqlite3_stmt *st = NULL;
    gboolean ok = sqlite3_prepare_v2(h->db,
        "DELETE FROM records WHERE store=?",
        -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(st, 1, store, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    if (st) sqlite3_finalize(st);
    JSValue out = ok ? JS_TRUE : ns_idb_throw_sql(ctx, h->db);
    ns_idb_db_close(h);
    JS_FreeCString(ctx, store);
    return out;
}

static JSValue
ns_idb_backend_info(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    ns_idb_db *h = ns_idb_open_db(ctx, name);
    if (!h) {
        JS_FreeCString(ctx, name);
        return ns_idb_throw(ctx, "UnknownError", "Could not open IndexedDB database");
    }
    JSValue out = ns_idb_info_for(ctx, h, name);
    ns_idb_db_close(h);
    JS_FreeCString(ctx, name);
    return out;
}

static JSValue
ns_idb_backend_delete_database(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_FALSE;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    g_autofree char *path = ns_idb_path_for_name(ctx, name);
    g_autofree char *key = ns_idb_cache_key(ctx, name);
    JS_FreeCString(ctx, name);
    if (!path) return ns_idb_throw(ctx, "SecurityError", "Storage is unavailable");
    ns_idb_cache_evict(key);
    if (strcmp(path, ":memory:") == 0) return JS_TRUE;
    g_unlink(path);
    g_autofree char *wal = g_strconcat(path, "-wal", NULL);
    g_autofree char *shm = g_strconcat(path, "-shm", NULL);
    g_unlink(wal);
    g_unlink(shm);
    return JS_TRUE;
}

static JSValue
ns_idb_backend_databases(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    JSValue arr = JS_NewArray(ctx);
    g_autofree char *dir = ns_idb_partition_dir(ctx);
    if (!dir) return arr;
    GDir *gd = g_dir_open(dir, 0, NULL);
    if (!gd) return arr;
    uint32_t i = 0;
    const char *entry = NULL;
    while ((entry = g_dir_read_name(gd)) != NULL) {
        if (!g_str_has_suffix(entry, ".sqlite")) continue;
        g_autofree char *path = g_build_filename(dir, entry, NULL);
        sqlite3 *db = NULL;
        if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            continue;
        }
        ns_idb_configure(db);
        g_autofree char *name = ns_idb_get_meta(db, "name");
        int64_t version = ns_idb_get_version(db);
        sqlite3_close(db);
        if (!name) continue;
        JSValue info = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, info, "name", JS_NewString(ctx, name));
        JS_SetPropertyStr(ctx, info, "version", JS_NewInt64(ctx, version));
        JS_SetPropertyUint32(ctx, arr, i++, info);
    }
    g_dir_close(gd);
    return arr;
}

static void
ns_idb_bind(JSContext *ctx, JSValueConst obj, const char *name,
            JSCFunction *fn, int argc)
{
    JS_SetPropertyStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, argc));
}

void
ns_idb_install(JSContext *ctx, JSValueConst global)
{
    JSValue backend = JS_NewObject(ctx);
    ns_idb_bind(ctx, backend, "open", ns_idb_backend_open, 1);
    ns_idb_bind(ctx, backend, "info", ns_idb_backend_info, 1);
    ns_idb_bind(ctx, backend, "setVersion", ns_idb_backend_set_version, 2);
    ns_idb_bind(ctx, backend, "createStore", ns_idb_backend_create_store, 4);
    ns_idb_bind(ctx, backend, "deleteStore", ns_idb_backend_delete_store, 2);
    ns_idb_bind(ctx, backend, "createIndex", ns_idb_backend_create_index, 6);
    ns_idb_bind(ctx, backend, "deleteIndex", ns_idb_backend_delete_index, 3);
    ns_idb_bind(ctx, backend, "nextKey", ns_idb_backend_next_key, 2);
    ns_idb_bind(ctx, backend, "put", ns_idb_backend_put, 7);
    ns_idb_bind(ctx, backend, "get", ns_idb_backend_get, 3);
    ns_idb_bind(ctx, backend, "records", ns_idb_backend_records, 2);
    ns_idb_bind(ctx, backend, "indexRecords", ns_idb_backend_index_records, 3);
    ns_idb_bind(ctx, backend, "deleteRecord", ns_idb_backend_delete_record, 3);
    ns_idb_bind(ctx, backend, "clear", ns_idb_backend_clear, 2);
    ns_idb_bind(ctx, backend, "deleteDatabase", ns_idb_backend_delete_database, 1);
    ns_idb_bind(ctx, backend, "databases", ns_idb_backend_databases, 0);
    JS_DefinePropertyValueStr(ctx, global, "__nd_idb", backend,
                              JS_PROP_CONFIGURABLE);
}
