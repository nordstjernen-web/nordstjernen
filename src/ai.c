/* Nordstjernen — local on-CPU chat inference over llama.cpp.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "ai.h"
#include "config.h"
#include "net.h"

#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

#ifndef NS_HAVE_AI

void
ns_ai_select_download(const char *model_id)
{
    (void)model_id;
}

char *
ns_ai_status_json(void)
{
    return g_strdup("{\"state\":\"disabled\",\"models\":[]}");
}

char *
ns_ai_chat_start(const char *user_msg)
{
    (void)user_msg;
    return g_strdup("{\"job\":0,\"state\":\"error\",\"message\":"
                    "\"This build was compiled without the local AI "
                    "feature.\"}");
}

char *
ns_ai_chat_poll(void)
{
    return g_strdup("{\"job\":0,\"state\":\"error\",\"message\":"
                    "\"This build was compiled without the local AI "
                    "feature.\"}");
}

void
ns_ai_chat_reset(void)
{
}

void
ns_ai_shutdown(void)
{
}

#else

#include <stdio.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <gio/gio.h>
#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "llama.h"
#include "ggml-backend.h"

#define NS_AI_N_CTX           4096
#define NS_AI_MAX_REPLY       640
#define NS_AI_HISTORY_MAX     16
#define NS_AI_IDLE_UNLOAD_US  (5 * 60 * G_USEC_PER_SEC)
#define NS_AI_SYSTEM_PROMPT \
    "You are the assistant built into the Nordstjernen web browser. " \
    "By default, answer the user directly, concisely, and helpfully in the " \
    "language they write in. Greetings, small talk, opinions, and anything " \
    "you already know: just answer in plain prose \xe2\x80\x94 do NOT use a " \
    "tool. Only when the user clearly asks for something you cannot answer " \
    "without the live web, reply with ONLY one line and nothing else: " \
    "use 'GO: <url>' when the user asks to open, visit, or go to a specific " \
    "website; 'IMAGE: <search terms>' when they ask to see, show, or find a " \
    "picture or image; 'SEARCH: <search terms>' when answering needs current " \
    "facts, news, prices, or a web lookup. When in doubt, answer directly " \
    "instead of using a tool. Never describe or mention the tools."
#define NS_AI_ANSWER_PROMPT \
    "You are the assistant built into the Nordstjernen web browser. Answer " \
    "the user's request using the web search results provided, in the " \
    "language the user writes in. Be concise. Cite sources inline as " \
    "markdown links like [title](https://url). Do not mention tools and do " \
    "not emit IMAGE: or SEARCH: lines."

typedef struct {
    const char *id;
    const char *label;
    const char *file;
    const char *url;
    int         mb;
    const char *sha256;
} ns_ai_model;

static const ns_ai_model k_models[] = {
    {
        "fast", "Llama 3.2 1B", "Llama-3.2-1B-Instruct-Q4_K_M.gguf",
        "https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF/"
        "resolve/main/Llama-3.2-1B-Instruct-Q4_K_M.gguf?download=true",
        810,
        "6f85a640a97cf2bf5b8e764087b1e83da0fdb51d7c9fab7d0fece9385611df83",
    },
    {
        "balanced", "Gemma 3 4B (Google)", "gemma-3-4b-it-Q4_K_M.gguf",
        "https://huggingface.co/ggml-org/gemma-3-4b-it-GGUF/resolve/main/"
        "gemma-3-4b-it-Q4_K_M.gguf?download=true", 2490,
        "882e8d2db44dc554fb0ea5077cb7e4bc49e7342a1f0da57901c0802ea21a0863",
    },
    {
        "quality", "Qwen3 4B Instruct 2507",
        "Qwen3-4B-Instruct-2507-Q4_K_M.gguf",
        "https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF/"
        "resolve/main/Qwen3-4B-Instruct-2507-Q4_K_M.gguf?download=true", 2382,
        "3605803b982cb64aead44f6c1b2ae36e3acdb41d8e46c8a94c6533bc4c67e597",
    },
    {
        "large", "Qwen3.5 9B", "Qwen3.5-9B-Q4_K_M.gguf",
        "https://huggingface.co/unsloth/Qwen3.5-9B-GGUF/"
        "resolve/main/Qwen3.5-9B-Q4_K_M.gguf?download=true",
        5417,
        "03b74727a860a56338e042c4420bb3f04b2fec5734175f4cb9fa853daf52b7e8",
    },
};

static struct llama_model       *g_model;
static struct llama_context     *g_ctx;
static const struct llama_vocab *g_vocab;
static gboolean                  g_model_thinks;
static char                     *g_loaded_path;
static int                       g_gpu_layers_used;
static char                     *g_gpu_device;
static llama_token              *g_cached_toks;
static int                       g_cached_n;
static GMutex                    g_model_lock;

static char    *g_active_id;
static gboolean g_downloading;
static char    *g_dl_id;
static char    *g_dl_error;
static gint64   g_dl_now;
static gint64   g_dl_total;
static GThread *g_dl_thread;
static GMutex   g_dl_lock;
static volatile gboolean g_ai_quit;

typedef struct {
    char *role;
    char *content;
} ns_ai_turn;

static GMutex     g_job_lock;
static int        g_job_id;
static gboolean   g_job_running;
static gboolean   g_job_cancel;
static char      *g_job_phase;
static GString   *g_job_text;
static char      *g_job_error;
static GPtrArray *g_history;
static gint64     g_last_use_us;
static guint      g_idle_timer;
static GThread   *g_chat_thread;

static void
ns_ai_turn_free(gpointer data)
{
    ns_ai_turn *t = data;
    g_free(t->role);
    g_free(t->content);
    g_free(t);
}

static char *
ns_ai_history_compact(const char *content)
{
    GString *o = g_string_new(NULL);
    for (const char *p = content; *p; ) {
        if (g_str_has_prefix(p, "data:")) {
            g_string_append(o, "data:,");
            while (*p && *p != ')' && !g_ascii_isspace((guchar)*p))
                p++;
            continue;
        }
        g_string_append_c(o, *p++);
    }
    return g_string_free(o, FALSE);
}

static void
ns_ai_history_append_locked(const char *role, const char *content)
{
    if (!g_history)
        g_history = g_ptr_array_new_with_free_func(ns_ai_turn_free);
    ns_ai_turn *t = g_new0(ns_ai_turn, 1);
    t->role = g_strdup(role);
    t->content = ns_ai_history_compact(content);
    g_ptr_array_add(g_history, t);
    while (g_history->len > NS_AI_HISTORY_MAX)
        g_ptr_array_remove_index(g_history, 0);
}

static void
ns_ai_job_set_phase(const char *phase)
{
    g_mutex_lock(&g_job_lock);
    g_free(g_job_phase);
    g_job_phase = g_strdup(phase);
    g_mutex_unlock(&g_job_lock);
}

static gboolean
ns_ai_job_cancelled(void)
{
    g_mutex_lock(&g_job_lock);
    gboolean c = g_job_cancel;
    g_mutex_unlock(&g_job_lock);
    return c;
}

static void
ns_ai_job_stream(const char *piece, gsize len)
{
    g_mutex_lock(&g_job_lock);
    if (!g_job_text) g_job_text = g_string_new(NULL);
    g_string_append_len(g_job_text, piece, (gssize)len);
    g_mutex_unlock(&g_job_lock);
}

static const ns_ai_model *
ns_ai_model_by_id(const char *id)
{
    if (!id) return NULL;
    for (gsize i = 0; i < G_N_ELEMENTS(k_models); i++)
        if (g_str_equal(k_models[i].id, id))
            return &k_models[i];
    return NULL;
}

static char *
ns_ai_models_dir(void)
{
    return g_build_filename(g_get_user_data_dir(), NS_APP_DIR_NAME, "models",
                            NULL);
}

static char *
ns_ai_model_path(const ns_ai_model *m)
{
    char *dir = ns_ai_models_dir();
    char *path = g_build_filename(dir, m->file, NULL);
    g_free(dir);
    return path;
}

static gboolean
ns_ai_model_installed(const ns_ai_model *m)
{
    char *path = ns_ai_model_path(m);
    gboolean ok = g_file_test(path, G_FILE_TEST_IS_REGULAR);
    g_free(path);
    return ok;
}

static const ns_ai_model *
ns_ai_first_installed(void)
{
    for (gsize i = 0; i < G_N_ELEMENTS(k_models); i++)
        if (ns_ai_model_installed(&k_models[i]))
            return &k_models[i];
    return NULL;
}

static const ns_ai_model *
ns_ai_active_model_locked(void)
{
    const ns_ai_model *m = ns_ai_model_by_id(g_active_id);
    if (m && ns_ai_model_installed(m))
        return m;
    return ns_ai_first_installed();
}

static char *
ns_ai_active_path(void)
{
    const char *env = g_getenv("NORDSTJERNEN_AI_MODEL");
    if (env && *env && g_file_test(env, G_FILE_TEST_IS_REGULAR))
        return g_strdup(env);

    g_mutex_lock(&g_dl_lock);
    const ns_ai_model *m = ns_ai_active_model_locked();
    char *path = m ? ns_ai_model_path(m) : NULL;
    g_mutex_unlock(&g_dl_lock);
    return path;
}

static int
ns_ai_xfer_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow,
              curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal; (void)ulnow;
    if (g_ai_quit)
        return 1;
    gint64 resumed = ud ? *(const gint64 *)ud : 0;
    g_mutex_lock(&g_dl_lock);
    g_dl_now = resumed + (gint64)dlnow;
    g_dl_total = dltotal > 0 ? resumed + (gint64)dltotal : 0;
    g_mutex_unlock(&g_dl_lock);
    return 0;
}

typedef struct {
    char *url;
    char *path;
    char *sha256;
    int   mb;
} ns_ai_dl_job;

static char *
ns_ai_file_sha256(const char *path)
{
    FILE *f = g_fopen(path, "rb");
    if (!f) return NULL;
    GChecksum *ck = g_checksum_new(G_CHECKSUM_SHA256);
    guchar buf[256 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        g_checksum_update(ck, buf, (gssize)n);
    gboolean ok = !ferror(f);
    fclose(f);
    char *hex = ok ? g_strdup(g_checksum_get_string(ck)) : NULL;
    g_checksum_free(ck);
    return hex;
}

static gint64
ns_ai_free_disk_bytes(const char *dir)
{
    GFile *gf = g_file_new_for_path(dir);
    GFileInfo *info = g_file_query_filesystem_info(
        gf, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, NULL, NULL);
    gint64 freeb = -1;
    if (info) {
        freeb = (gint64)g_file_info_get_attribute_uint64(
            info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
        g_object_unref(info);
    }
    g_object_unref(gf);
    return freeb;
}

static char *
ns_ai_curl_fetch_part(const ns_ai_dl_job *job, const char *part,
                      gboolean *retry_fresh)
{
    *retry_fresh = FALSE;
    GStatBuf st;
    gint64 have = g_stat(part, &st) == 0 ? (gint64)st.st_size : 0;
    FILE *f = g_fopen(part, have > 0 ? "ab" : "wb");
    if (!f) return g_strdup("could not open destination file");

    char *err = NULL;
    CURL *c = curl_easy_init();
    if (!c) {
        err = g_strdup("could not initialise libcurl");
    } else {
        curl_easy_setopt(c, CURLOPT_URL, job->url);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, fwrite);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, NS_USER_AGENT);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
        ns_net_apply_curl_proxy(c, job->url);
        ns_net_apply_curl_tls(c);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, ns_ai_xfer_cb);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &have);
        curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
        if (have > 0)
            curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE,
                             (curl_off_t)have);
        CURLcode rc = curl_easy_perform(c);
        if (rc != CURLE_OK) {
            long http = 0;
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
            if (have > 0 && (http == 416 || rc == CURLE_RANGE_ERROR))
                *retry_fresh = TRUE;
            else
                err = g_strdup(curl_easy_strerror(rc));
        }
        curl_easy_cleanup(c);
    }
    fclose(f);
    return err;
}

static gpointer
ns_ai_download_thread(gpointer data)
{
    ns_ai_dl_job *job = data;
    char *dir = ns_ai_models_dir();
    g_mkdir_with_parents(dir, 0700);
    char *part = g_strconcat(job->path, ".part", NULL);

    char *err = NULL;
    GStatBuf st;
    gint64 have = g_stat(part, &st) == 0 ? (gint64)st.st_size : 0;
    gint64 need = (gint64)job->mb * 1024 * 1024 - have;
    gint64 freeb = ns_ai_free_disk_bytes(dir);
    if (freeb >= 0 && need > 0 && freeb < need + need / 10)
        err = g_strdup_printf("not enough disk space (%d MB needed)",
                              (int)(need / (1024 * 1024)));
    g_free(dir);

    if (!err) {
        gboolean retry_fresh = FALSE;
        err = ns_ai_curl_fetch_part(job, part, &retry_fresh);
        if (!err && retry_fresh) {
            g_remove(part);
            err = ns_ai_curl_fetch_part(job, part, &retry_fresh);
        }
    }

    if (!err) {
        char *hex = ns_ai_file_sha256(part);
        if (!hex) {
            err = g_strdup("could not read back the downloaded model");
        } else if (job->sha256 && g_ascii_strcasecmp(hex, job->sha256) != 0) {
            err = g_strdup("checksum mismatch — the downloaded model does "
                           "not match the pinned digest; it was discarded");
            g_remove(part);
        } else {
            if (!job->sha256)
                g_message("nordstjernen-ai: downloaded %s sha256=%s "
                          "(unpinned; add this digest to k_models to pin it)",
                          job->path, hex);
        }
        g_free(hex);
    }

    if (!err) {
        if (g_rename(part, job->path) != 0)
            err = g_strdup("could not move downloaded model into place");
    } else if (!g_str_has_prefix(err, "checksum mismatch") &&
               !g_str_has_prefix(err, "not enough disk space")) {
        GStatBuf pst;
        if (g_stat(part, &pst) != 0 || pst.st_size == 0)
            g_remove(part);
    }

    g_mutex_lock(&g_dl_lock);
    g_free(g_dl_error);
    g_dl_error = err;
    g_downloading = FALSE;
    g_free(g_dl_id);
    g_dl_id = NULL;
    g_mutex_unlock(&g_dl_lock);

    g_free(part);
    g_free(job->url);
    g_free(job->path);
    g_free(job->sha256);
    g_free(job);
    return NULL;
}

void
ns_ai_select_download(const char *model_id)
{
    const ns_ai_model *m = ns_ai_model_by_id(model_id);
    if (!m) return;

    ns_ai_dl_job *job = NULL;
    g_mutex_lock(&g_dl_lock);
    g_free(g_active_id);
    g_active_id = g_strdup(m->id);

    if (!ns_ai_model_installed(m) && !g_downloading) {
        const char *env = g_getenv("NORDSTJERNEN_AI_MODEL_URL");
        job = g_new0(ns_ai_dl_job, 1);
        if (env && *env) {
            job->url = g_strdup(env);
        } else {
            const ns_config *cfg = ns_config_get();
            const char *mirror =
                (cfg && cfg->ai_model_mirror && *cfg->ai_model_mirror)
                ? cfg->ai_model_mirror : NULL;
            const char *hf = "https://huggingface.co/";
            if (mirror && g_str_has_prefix(m->url, hf)) {
                gsize mirror_len = strlen(mirror);
                gboolean slash = mirror_len > 0 && mirror[mirror_len - 1] == '/';
                job->url = g_strconcat(mirror, slash ? "" : "/",
                                       m->url + strlen(hf), NULL);
            } else {
                job->url = g_strdup(m->url);
            }
        }
        job->path = ns_ai_model_path(m);
        job->sha256 = (!env || !*env) ? g_strdup(m->sha256) : NULL;
        job->mb = m->mb;
        g_downloading = TRUE;
        g_free(g_dl_id);
        g_dl_id = g_strdup(m->id);
        g_free(g_dl_error);
        g_dl_error = NULL;
        g_dl_now = 0;
        g_dl_total = 0;
    }
    GThread *old_dl = job ? g_dl_thread : NULL;
    if (job)
        g_dl_thread = NULL;
    g_mutex_unlock(&g_dl_lock);

    if (old_dl)
        g_thread_join(old_dl);
    if (job) {
        GThread *t = g_thread_new("ns-ai-download", ns_ai_download_thread, job);
        g_mutex_lock(&g_dl_lock);
        g_dl_thread = t;
        g_mutex_unlock(&g_dl_lock);
    }
}

static char *
ns_ai_json_escape(const char *s)
{
    GString *o = g_string_new(NULL);
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\') g_string_append_c(o, '\\');
        if ((guchar)*s < 0x20) { g_string_append_printf(o, "\\u%04x", *s); continue; }
        g_string_append_c(o, *s);
    }
    return g_string_free(o, FALSE);
}

typedef struct {
    char  *data;
    size_t len;
} ns_ai_http_buf;

static size_t
ns_ai_http_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
    ns_ai_http_buf *b = ud;
    size_t add = size * nmemb;
    if (b->len + add > 8u * 1024u * 1024u)
        return 0;
    b->data = g_realloc(b->data, b->len + add + 1);
    memcpy(b->data + b->len, ptr, add);
    b->len += add;
    b->data[b->len] = '\0';
    return add;
}

static char *
ns_ai_http_get(const char *url)
{
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    ns_ai_http_buf b = { 0 };
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept: text/html,application/json,*/*");
    const ns_config *cfg = ns_config_get();
    const char *accept_language =
        (cfg && cfg->accept_language && *cfg->accept_language)
        ? cfg->accept_language : ns_net_default_accept_language();
    char *al_hdr = g_strdup_printf("Accept-Language: %s", accept_language);
    hdrs = curl_slist_append(hdrs, al_hdr);
    g_free(al_hdr);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, ns_ai_http_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, NS_USER_AGENT);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    ns_net_apply_curl_proxy(c, url);
    ns_net_apply_curl_tls(c);
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        g_free(b.data);
        return NULL;
    }
    return b.data;
}

static const char *
ns_ai_json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

static char *
ns_ai_json_string(const char *p, const char **endp)
{
    if (*p != '"') return NULL;
    p++;
    GString *o = g_string_new(NULL);
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n':  g_string_append_c(o, '\n'); break;
            case 't':  g_string_append_c(o, '\t'); break;
            case 'r':  g_string_append_c(o, '\r'); break;
            case 'b':  g_string_append_c(o, '\b'); break;
            case 'f':  g_string_append_c(o, '\f'); break;
            case 'u':
                if (p[1] && p[2] && p[3] && p[4]) {
                    char hex[5] = { p[1], p[2], p[3], p[4], 0 };
                    gunichar cp = (gunichar)strtol(hex, NULL, 16);
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        p[5] == '\\' && p[6] == 'u' &&
                        p[7] && p[8] && p[9] && p[10]) {
                        char hex2[5] = { p[7], p[8], p[9], p[10], 0 };
                        gunichar lo = (gunichar)strtol(hex2, NULL, 16);
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                    if (cp && !(cp >= 0xD800 && cp <= 0xDFFF)) {
                        char utf8[8];
                        int utf8_len = g_unichar_to_utf8(cp, utf8);
                        g_string_append_len(o, utf8, utf8_len);
                    }
                    p += 4;
                }
                break;
            default:   g_string_append_c(o, *p);
            }
            p++;
        } else {
            g_string_append_c(o, *p++);
        }
    }
    if (*p != '"') {
        g_string_free(o, TRUE);
        return NULL;
    }
    *endp = p + 1;
    return g_string_free(o, FALSE);
}

static char *
ns_ai_json_first_string(const char *json, const char *key)
{
    for (const char *p = json; p && *p; ) {
        if (*p != '"') { p++; continue; }
        const char *end = NULL;
        char *tok = ns_ai_json_string(p, &end);
        if (!tok) { p++; continue; }
        const char *q = ns_ai_json_skip_ws(end);
        if (*q != ':') {
            g_free(tok);
            p = end;
            continue;
        }
        gboolean want = g_strcmp0(tok, key) == 0;
        g_free(tok);
        const char *v = ns_ai_json_skip_ws(q + 1);
        if (want && *v == '"') {
            char *val = ns_ai_json_string(v, &end);
            if (val) return val;
        }
        p = q + 1;
    }
    return NULL;
}

static char *
ns_ai_ddg_decode_url(const char *href)
{
    const char *u = strstr(href, "uddg=");
    if (u) {
        u += 5;
        const char *end = strchr(u, '&');
        char *enc = end ? g_strndup(u, (gsize)(end - u)) : g_strdup(u);
        char *dec = g_uri_unescape_string(enc, NULL);
        g_free(enc);
        return dec ? dec : g_strdup(href);
    }
    if (g_str_has_prefix(href, "//"))
        return g_strconcat("https:", href, NULL);
    return g_strdup(href);
}

static const char *
ns_ai_wiki_lang(void)
{
    static char lang[4];
    static gboolean tried;
    if (tried)
        return lang;
    tried = TRUE;
    const char *const *names = g_get_language_names();
    for (int i = 0; names && names[i]; i++) {
        const char *n = names[i];
        gsize l = 0;
        while (n[l] && g_ascii_isalpha((guchar)n[l]))
            l++;
        if ((l != 2 && l != 3) ||
            (n[l] && n[l] != '_' && n[l] != '-' && n[l] != '.' && n[l] != '@'))
            continue;
        for (gsize k = 0; k < l; k++)
            lang[k] = g_ascii_tolower(n[k]);
        lang[l] = '\0';
        break;
    }
    if (!lang[0])
        g_strlcpy(lang, "en", sizeof lang);
    return lang;
}

static char *
ns_ai_image_search_lang(const char *query, const char *lang, char **page_out)
{
    if (page_out) *page_out = NULL;
    char *eq = g_uri_escape_string(query, NULL, TRUE);
    char *url = g_strdup_printf(
        "https://%s.wikipedia.org/w/api.php?action=query&format=json"
        "&generator=search&gsrsearch=%s&gsrlimit=1&prop=pageimages"
        "&piprop=thumbnail&pithumbsize=640", lang, eq);
    char *json = ns_ai_http_get(url);
    g_free(url);
    g_free(eq);
    if (!json) return NULL;

    char *img = ns_ai_json_first_string(json, "source");
    char *title = ns_ai_json_first_string(json, "title");
    g_free(json);

    if (img && page_out && title) {
        char *t = g_uri_escape_string(title, NULL, TRUE);
        *page_out = g_strdup_printf("https://%s.wikipedia.org/wiki/%s",
                                    lang, t);
        g_free(t);
    }
    g_free(title);
    return img;
}

static char *
ns_ai_image_search(const char *query, char **page_out)
{
    const char *lang = ns_ai_wiki_lang();
    char *img = ns_ai_image_search_lang(query, lang, page_out);
    if (!img && !g_str_equal(lang, "en"))
        img = ns_ai_image_search_lang(query, "en", page_out);
    return img;
}

static char *
ns_ai_fetch_data_uri(const char *url)
{
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    ns_ai_http_buf b = { 0 };
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept: image/*,*/*");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, ns_ai_http_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, NS_USER_AGENT);
    ns_net_apply_curl_proxy(c, url);
    ns_net_apply_curl_tls(c);
    CURLcode rc = curl_easy_perform(c);
    char *ctype = NULL;
    if (rc == CURLE_OK) {
        char *info = NULL;
        curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &info);
        if (info) ctype = g_strdup(info);
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK || b.len == 0) {
        g_free(b.data);
        g_free(ctype);
        return NULL;
    }
    if (!ctype || !g_str_has_prefix(ctype, "image/")) {
        g_free(ctype);
        ctype = g_strdup("image/jpeg");
    } else {
        char *semi = strchr(ctype, ';');
        if (semi) *semi = '\0';
        g_strstrip(ctype);
    }
    char *b64 = g_base64_encode((const guchar *)b.data, b.len);
    g_free(b.data);
    char *uri = g_strdup_printf("data:%s;base64,%s", ctype, b64);
    g_free(b64);
    g_free(ctype);
    return uri;
}

static const char *
ns_ai_search_region(void)
{
    static const struct { const char *tag; const char *kl; } map[] = {
        { "en_us", "us-en" }, { "en_gb", "uk-en" }, { "en_au", "au-en" },
        { "en_ca", "ca-en" }, { "en_in", "in-en" }, { "de_at", "at-de" },
        { "de_ch", "ch-de" }, { "de", "de-de" },    { "fr_ca", "ca-fr" },
        { "fr_be", "be-fr" }, { "fr_ch", "ch-fr" }, { "fr", "fr-fr" },
        { "es_mx", "mx-es" }, { "es_ar", "ar-es" }, { "es_cl", "cl-es" },
        { "es_co", "co-es" }, { "es", "es-es" },    { "pt_pt", "pt-pt" },
        { "pt", "br-pt" },    { "it", "it-it" },    { "nl_be", "be-nl" },
        { "nl", "nl-nl" },    { "pl", "pl-pl" },    { "ru", "ru-ru" },
        { "ja", "jp-jp" },    { "ko", "kr-kr" },    { "zh_tw", "tw-tzh" },
        { "zh_hk", "hk-tzh" },{ "zh", "cn-zh" },    { "tr", "tr-tr" },
        { "sv", "se-sv" },    { "nb", "no-no" },    { "nn", "no-no" },
        { "no", "no-no" },    { "da", "dk-da" },    { "fi", "fi-fi" },
        { "el", "gr-el" },    { "cs", "cz-cs" },    { "hu", "hu-hu" },
        { "ro", "ro-ro" },    { "bg", "bg-bg" },    { "uk", "ua-uk" },
        { "he", "il-he" },    { "ar", "xa-ar" },    { "th", "th-th" },
        { "vi", "vn-vi" },    { "id", "id-id" },    { "ms", "my-ms" },
        { "et", "ee-et" },    { "lv", "lv-lv" },    { "lt", "lt-lt" },
        { "sk", "sk-sk" },    { "sl", "sl-sl" },    { "hr", "hr-hr" },
        { "ca", "ct-ca" },
    };
    static const char *cached;
    static gboolean tried;
    if (tried)
        return cached;
    tried = TRUE;
    const char *const *names = g_get_language_names();
    const char *raw = names && names[0] ? names[0] : "";
    char tag[16] = { 0 };
    gsize n = 0;
    for (const char *p = raw; *p && n < sizeof tag - 1; p++) {
        if (*p == '.' || *p == '@')
            break;
        tag[n++] = (*p == '-') ? '_' : g_ascii_tolower(*p);
    }
    for (gsize i = 0; i < G_N_ELEMENTS(map); i++)
        if (g_str_equal(tag, map[i].tag)) {
            cached = map[i].kl;
            return cached;
        }
    char *us = strchr(tag, '_');
    if (us) {
        *us = '\0';
        for (gsize i = 0; i < G_N_ELEMENTS(map); i++)
            if (g_str_equal(tag, map[i].tag)) {
                cached = map[i].kl;
                return cached;
            }
    }
    return cached;
}

static gboolean
ns_ai_el_class_has(lxb_dom_element_t *el, const char *cls)
{
    size_t vlen = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(
        el, (const lxb_char_t *)"class", 5, &vlen);
    if (!v) return FALSE;
    char *s = g_strndup((const char *)v, vlen);
    gboolean has = strstr(s, cls) != NULL;
    g_free(s);
    return has;
}

static char *
ns_ai_el_text(lxb_dom_node_t *node)
{
    size_t tlen = 0;
    lxb_char_t *t = lxb_dom_node_text_content(node, &tlen);
    if (!t) return NULL;
    return g_strstrip(g_strndup((const char *)t, tlen));
}

static char *
ns_ai_el_href_url(lxb_dom_element_t *el)
{
    size_t hlen = 0;
    const lxb_char_t *href = lxb_dom_element_get_attribute(
        el, (const lxb_char_t *)"href", 4, &hlen);
    if (!href) return NULL;
    char *raw = g_strndup((const char *)href, hlen);
    char *url = ns_ai_ddg_decode_url(raw);
    g_free(raw);
    return url;
}

static void
ns_ai_search_emit(GString *ctx, GString *src, GString *disp, int n,
                  const char *title, const char *url, const char *snippet)
{
    const char *tt = (title && *title) ? title : (url ? url : "");
    g_string_append_printf(ctx, "[%d] %s\n%s\n%.280s\n\n",
        n, title ? title : "", url ? url : "", snippet ? snippet : "");
    g_string_append_printf(src, "[%d] [%s](%s)\n", n, tt, url ? url : "");
    g_string_append_printf(disp, "[%s](%s)\n%.240s\n\n",
        tt, url ? url : "", snippet ? snippet : "");
}

static char *
ns_ai_web_search(const char *query, char **sources_out, char **display_out)
{
    if (sources_out) *sources_out = NULL;
    if (display_out) *display_out = NULL;
    char *eq = g_uri_escape_string(query, NULL, TRUE);
    const char *kl = ns_ai_search_region();
    char *u = kl
        ? g_strdup_printf("https://html.duckduckgo.com/html/?q=%s&kl=%s",
                          eq, kl)
        : g_strdup_printf("https://html.duckduckgo.com/html/?q=%s", eq);
    g_free(eq);
    char *html = ns_ai_http_get(u);
    g_free(u);
    if (!html) return NULL;

    lxb_html_parser_t *parser = lxb_html_parser_create();
    if (!parser || lxb_html_parser_init(parser) != LXB_STATUS_OK) {
        if (parser) lxb_html_parser_destroy(parser);
        g_free(html);
        return NULL;
    }
    lxb_html_parser_dom_opt_set(parser, LXB_DOM_DOCUMENT_OPT_WO_EVENTS);
    lxb_html_document_t *doc = lxb_html_parse(parser,
        (const lxb_char_t *)html, strlen(html));
    lxb_html_parser_destroy(parser);
    g_free(html);
    if (!doc) return NULL;

    GString *ctx = g_string_new(NULL);
    GString *src = g_string_new(NULL);
    GString *disp = g_string_new(NULL);
    int n = 0;
    char *pending_url = NULL, *pending_title = NULL;

    lxb_dom_node_t *root = lxb_dom_interface_node(doc);
    lxb_dom_node_t *node = root->first_child;
    while (node && n < 4) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT &&
            node->local_name == LXB_TAG_A) {
            lxb_dom_element_t *el = lxb_dom_interface_element(node);
            if (ns_ai_el_class_has(el, "result__a")) {
                if (pending_url)
                    ns_ai_search_emit(ctx, src, disp, ++n,
                                      pending_title, pending_url, NULL);
                g_free(pending_url);
                g_free(pending_title);
                pending_url = ns_ai_el_href_url(el);
                pending_title = ns_ai_el_text(node);
            } else if (pending_url &&
                       ns_ai_el_class_has(el, "result__snippet")) {
                char *snippet = ns_ai_el_text(node);
                ns_ai_search_emit(ctx, src, disp, ++n,
                                  pending_title, pending_url, snippet);
                g_free(snippet);
                g_free(pending_url);
                g_free(pending_title);
                pending_url = NULL;
                pending_title = NULL;
            }
        }
        if (node->first_child) {
            node = node->first_child;
        } else {
            while (node && node != root && !node->next)
                node = node->parent;
            if (!node || node == root) break;
            node = node->next;
        }
    }
    if (pending_url && n < 4)
        ns_ai_search_emit(ctx, src, disp, ++n, pending_title, pending_url, NULL);
    g_free(pending_url);
    g_free(pending_title);
    lxb_html_document_destroy(doc);

    if (n == 0) {
        g_string_free(ctx, TRUE);
        g_string_free(src, TRUE);
        g_string_free(disp, TRUE);
        return NULL;
    }
    if (sources_out) *sources_out = g_string_free(src, FALSE);
    else g_string_free(src, TRUE);
    if (display_out) *display_out = g_string_free(disp, FALSE);
    else g_string_free(disp, TRUE);
    return g_string_free(ctx, FALSE);
}

static gboolean
ns_ai_parse_tool(const char *reply, char **kind, char **arg)
{
    for (const char *p = reply; p && *p; ) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        const char *s = p;
        size_t l = len;
        while (l && (*s == ' ' || *s == '\t' || *s == '*' || *s == '`')) { s++; l--; }
        if (l == 0) {
            if (!eol) break;
            p = eol + 1;
            continue;
        }
        if (l >= 6 && g_ascii_strncasecmp(s, "IMAGE:", 6) == 0) {
            *kind = g_strdup("image");
            *arg = g_strstrip(g_strndup(s + 6, l - 6));
            return TRUE;
        }
        if (l >= 7 && g_ascii_strncasecmp(s, "SEARCH:", 7) == 0) {
            *kind = g_strdup("search");
            *arg = g_strstrip(g_strndup(s + 7, l - 7));
            return TRUE;
        }
        if ((l >= 3 && g_ascii_strncasecmp(s, "GO:", 3) == 0)) {
            *kind = g_strdup("go");
            *arg = g_strstrip(g_strndup(s + 3, l - 3));
            return TRUE;
        }
        if (l >= 9 && g_ascii_strncasecmp(s, "NAVIGATE:", 9) == 0) {
            *kind = g_strdup("go");
            *arg = g_strstrip(g_strndup(s + 9, l - 9));
            return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

static char *
ns_ai_strip_nav_markers(char *s)
{
    char *p = s;
    while (p && (p = strstr(p, "@@NAVIGATE@@")))
        memmove(p, p + 12, strlen(p + 12) + 1);
    return s;
}

static char *
ns_ai_normalize_url(const char *raw)
{
    if (!raw || !*raw) return NULL;
    char *s = g_strstrip(g_strdup(raw));
    if (!*s || strpbrk(s, " \t\r\n")) { g_free(s); return NULL; }
    if (strstr(s, "://")) {
        if (g_ascii_strncasecmp(s, "http://", 7) == 0 ||
            g_ascii_strncasecmp(s, "https://", 8) == 0)
            return s;
        g_free(s);
        return NULL;
    }
    char *full = g_strconcat("https://", s, NULL);
    g_free(s);
    return full;
}

static char *
ns_ai_image_reply(const char *query)
{
    char *page = NULL;
    char *img = ns_ai_image_search(query, &page);
    char *reply;
    if (img) {
        char *alt = g_strdup(query);
        g_strdelimit(alt, "[]()", ' ');
        char *data_uri = ns_ai_fetch_data_uri(img);
        const char *src = data_uri ? data_uri : img;
        reply = g_strdup_printf(
            "Here's an image of %s:\n\n![%s](%s)\n\n[Image source](%s)",
            query, alt, src, page ? page : img);
        g_free(data_uri);
        g_free(alt);
    } else {
        reply = g_strdup_printf("I couldn't find an image for \"%s\".", query);
    }
    g_free(img);
    g_free(page);
    return ns_ai_strip_nav_markers(reply);
}

char *
ns_ai_status_json(void)
{
    g_mutex_lock(&g_model_lock);
    int gpu_layers = g_gpu_layers_used;
    char *gpu_device = g_strdup(g_gpu_device);
    g_mutex_unlock(&g_model_lock);

    g_mutex_lock(&g_dl_lock);
    gboolean downloading = g_downloading;
    char *dl_id = g_strdup(g_dl_id);
    char *err = g_strdup(g_dl_error);
    gint64 now = g_dl_now, total = g_dl_total;
    const ns_ai_model *active = ns_ai_active_model_locked();
    char *active_id = g_strdup(active ? active->id : NULL);
    g_mutex_unlock(&g_dl_lock);

    GString *models = g_string_new("[");
    for (gsize i = 0; i < G_N_ELEMENTS(k_models); i++) {
        const ns_ai_model *m = &k_models[i];
        g_string_append_printf(models,
            "%s{\"id\":\"%s\",\"label\":\"%s\",\"mb\":%d,\"installed\":%s}",
            i ? "," : "", m->id, m->label, m->mb,
            ns_ai_model_installed(m) ? "true" : "false");
    }
    g_string_append_c(models, ']');

    const char *state = downloading ? "downloading"
                      : active_id   ? "ready"
                      : err         ? "error" : "idle";

    GString *out = g_string_new(NULL);
    g_string_append_printf(out, "{\"state\":\"%s\",\"models\":%s",
                           state, models->str);
    if (active_id)
        g_string_append_printf(out, ",\"active\":\"%s\"", active_id);
    if (gpu_device && gpu_layers != 0) {
        char *esc = ns_ai_json_escape(gpu_device);
        g_string_append_printf(out,
            ",\"gpu\":\"%s\",\"gpu_layers\":%d", esc, gpu_layers);
        g_free(esc);
    }
    if (downloading) {
        int pct = total > 0 ? (int)((now * 100) / total) : 0;
        g_string_append_printf(out,
            ",\"downloading\":\"%s\",\"percent\":%d,"
            "\"received\":%" G_GINT64_FORMAT ",\"total\":%" G_GINT64_FORMAT,
            dl_id ? dl_id : "", pct, now, total);
    }
    if (err && !downloading) {
        char *esc = ns_ai_json_escape(err);
        g_string_append_printf(out, ",\"message\":\"%s\"", esc);
        g_free(esc);
    }
    g_string_append_c(out, '}');

    g_string_free(models, TRUE);
    g_free(dl_id);
    g_free(err);
    g_free(active_id);
    g_free(gpu_device);
    return g_string_free(out, FALSE);
}

static void
ns_ai_log_sink(enum ggml_log_level level, const char *text, void *ud)
{
    (void)ud;
    if (level >= GGML_LOG_LEVEL_ERROR && text)
        g_printerr("%s", text);
}

static void
ns_ai_unload_locked(void)
{
    if (g_ctx) { llama_free(g_ctx); g_ctx = NULL; }
    if (g_model) { llama_model_free(g_model); g_model = NULL; }
    g_vocab = NULL;
    g_model_thinks = FALSE;
    g_free(g_loaded_path);
    g_loaded_path = NULL;
    g_gpu_layers_used = 0;
    g_free(g_gpu_device);
    g_gpu_device = NULL;
    g_free(g_cached_toks);
    g_cached_toks = NULL;
    g_cached_n = 0;
}

static gboolean
ns_ai_device_is_software(const char *name)
{
    if (!name) return FALSE;
    char *lower = g_ascii_strdown(name, -1);
    gboolean soft = strstr(lower, "llvmpipe") || strstr(lower, "lavapipe") ||
                    strstr(lower, "swiftshader") || strstr(lower, "software") ||
                    strstr(lower, "cpu");
    g_free(lower);
    return soft;
}

static ggml_backend_dev_t
ns_ai_pick_gpu(void)
{
    size_t n = ggml_backend_dev_count();
    ggml_backend_dev_t best = NULL;
    int best_rank = 0;
    size_t best_free = 0;
    for (size_t i = 0; i < n; i++) {
        ggml_backend_dev_t d = ggml_backend_dev_get(i);
        enum ggml_backend_dev_type t = ggml_backend_dev_type(d);
        int rank = t == GGML_BACKEND_DEVICE_TYPE_GPU  ? 2
                 : t == GGML_BACKEND_DEVICE_TYPE_IGPU ? 1 : 0;
        if (rank == 0)
            continue;
        if (ns_ai_device_is_software(ggml_backend_dev_name(d)) ||
            ns_ai_device_is_software(ggml_backend_dev_description(d)))
            continue;
        size_t freem = 0, totalm = 0;
        ggml_backend_dev_memory(d, &freem, &totalm);
        if (rank > best_rank || (rank == best_rank && freem > best_free)) {
            best = d;
            best_rank = rank;
            best_free = freem;
        }
    }
    return best;
}

static int
ns_ai_plan_gpu_offload(const char *path, int n_layers, char **device_out)
{
    *device_out = NULL;

    const char *env = g_getenv("NORDSTJERNEN_AI_GPU_LAYERS");
    gboolean forced = env && *env;
    int want = forced ? atoi(env) : 0;
    if (forced && want == 0)
        return 0;

    if (!llama_supports_gpu_offload())
        return 0;

    ggml_backend_dev_t dev = ns_ai_pick_gpu();
    if (!dev)
        return 0;

    if (!forced) {
        size_t freem = 0, totalm = 0;
        ggml_backend_dev_memory(dev, &freem, &totalm);
        GStatBuf st;
        if (freem == 0 || n_layers <= 0 || g_stat(path, &st) != 0)
            return 0;
        double per_layer = (double)st.st_size / (double)n_layers;
        double budget = (double)freem * 0.85;
        want = per_layer > 0 ? (int)(budget / per_layer) : 0;
        if (want < 1)
            return 0;
        if (want > n_layers)
            want = n_layers;
    }

    const char *desc = ggml_backend_dev_description(dev);
    *device_out = g_strdup(desc ? desc : ggml_backend_dev_name(dev));
    return want;
}

static struct llama_model *
ns_ai_load_model(const char *path, int n_gpu_layers)
{
    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = n_gpu_layers;
    return llama_model_load_from_file(path, mparams);
}

static gboolean
ns_ai_ensure_loaded_locked(void)
{
    char *path = ns_ai_active_path();
    if (!path) return FALSE;

    if (g_loaded_path && g_str_equal(g_loaded_path, path)) {
        g_free(path);
        return TRUE;
    }
    ns_ai_unload_locked();

    llama_log_set(ns_ai_log_sink, NULL);
    llama_backend_init();

    g_model = ns_ai_load_model(path, 0);
    if (!g_model) { g_free(path); return FALSE; }

    char *device = NULL;
    int want = ns_ai_plan_gpu_offload(path, llama_model_n_layer(g_model),
                                      &device);
    if (want != 0) {
        llama_model_free(g_model);
        g_model = ns_ai_load_model(path, want);
        if (g_model) {
            g_gpu_layers_used = want;
            g_gpu_device = device;
        } else {
            g_free(device);
            g_model = ns_ai_load_model(path, 0);
        }
    }
    if (!g_model) { g_free(path); return FALSE; }

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx           = NS_AI_N_CTX;
    cparams.n_batch         = NS_AI_N_CTX;
    int32_t n_threads = (int32_t)g_get_num_processors() - 2;
    if (n_threads < 1) n_threads = 1;
    cparams.n_threads       = n_threads;
    cparams.n_threads_batch = n_threads;
    g_ctx = llama_init_from_model(g_model, cparams);
    if (!g_ctx && g_gpu_layers_used != 0) {
        llama_model_free(g_model);
        g_gpu_layers_used = 0;
        g_free(g_gpu_device);
        g_gpu_device = NULL;
        g_model = ns_ai_load_model(path, 0);
        if (g_model)
            g_ctx = llama_init_from_model(g_model, cparams);
    }
    if (!g_ctx) {
        if (g_model) { llama_model_free(g_model); g_model = NULL; }
        g_free(path);
        return FALSE;
    }

    g_vocab = llama_model_get_vocab(g_model);
    char arch[64] = {0};
    g_model_thinks =
        llama_model_meta_val_str(g_model, "general.architecture",
                                 arch, sizeof arch) > 0 &&
        g_str_has_prefix(arch, "qwen3");
    g_loaded_path = path;
    return TRUE;
}

static char *
ns_ai_apply_template(const char *tmpl, const struct llama_chat_message *msgs,
                     int n_msgs)
{
    int need = llama_chat_apply_template(tmpl, msgs, (size_t)n_msgs,
                                         true, NULL, 0);
    if (need <= 0) return NULL;
    char *buf = g_malloc((gsize)need + 1);
    int n = llama_chat_apply_template(tmpl, msgs, (size_t)n_msgs,
                                      true, buf, need + 1);
    if (n <= 0) {
        g_free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

static char *
ns_ai_build_prompt(const struct llama_chat_message *msgs, int n_msgs)
{
    const char *tmpl = llama_model_chat_template(g_model, NULL);

    if (tmpl) {
        char *p = ns_ai_apply_template(tmpl, msgs, n_msgs);
        if (p) return p;
        if (n_msgs >= 2 && g_str_equal(msgs[0].role, "system") &&
            g_str_equal(msgs[1].role, "user")) {
            struct llama_chat_message *fused =
                g_new0(struct llama_chat_message, (gsize)n_msgs - 1);
            char *merged = g_strdup_printf("%s\n\n%s", msgs[0].content,
                                           msgs[1].content);
            fused[0].role = "user";
            fused[0].content = merged;
            for (int i = 2; i < n_msgs; i++)
                fused[i - 1] = msgs[i];
            p = ns_ai_apply_template(tmpl, fused, n_msgs - 1);
            g_free(merged);
            g_free(fused);
            if (p) return p;
        }
    }

    GString *out = g_string_new(NULL);
    for (int i = 0; i < n_msgs; i++)
        g_string_append_printf(out, "<|im_start|>%s\n%s<|im_end|>\n",
                               msgs[i].role, msgs[i].content);
    g_string_append(out, "<|im_start|>assistant\n");
    return g_string_free(out, FALSE);
}

static int
ns_ai_tokenize_prompt(const char *prompt, llama_token **toks_out)
{
    int n_max = (int)strlen(prompt) + 8;
    llama_token *toks = g_malloc(sizeof(llama_token) * (gsize)n_max);
    int n = llama_tokenize(g_vocab, prompt, (int)strlen(prompt),
                           toks, n_max, true, true);
    if (n <= 0) {
        g_free(toks);
        *toks_out = NULL;
        return 0;
    }
    *toks_out = toks;
    return n;
}

static gsize
ns_ai_partial_tag_holdback(const char *s, gsize len)
{
    static const char *const tags[] = { "<think>", "</think>" };
    gsize best = 0;
    for (gsize t = 0; t < G_N_ELEMENTS(tags); t++) {
        gsize max_partial = strlen(tags[t]) - 1;
        for (gsize k = max_partial < len ? max_partial : len; k >= 1; k--) {
            if (memcmp(s + len - k, tags[t], k) == 0) {
                if (k > best) best = k;
                break;
            }
        }
    }
    return best;
}

static char *
ns_ai_strip_think(const char *s, gsize len, gboolean streaming)
{
    GString *o = g_string_new(NULL);
    gsize i = 0;
    while (i < len) {
        if (len - i >= 7 && memcmp(s + i, "<think>", 7) == 0) {
            const char *close = g_strstr_len(s + i + 7,
                                             (gssize)(len - i - 7),
                                             "</think>");
            if (!close) break;
            i = (gsize)(close - s) + 8;
            continue;
        }
        g_string_append_c(o, s[i]);
        i++;
    }
    if (streaming) {
        gsize holdback_bytes = ns_ai_partial_tag_holdback(o->str, o->len);
        g_string_truncate(o, o->len - holdback_bytes);
    }
    return g_string_free(o, FALSE);
}

static gboolean
ns_ai_reply_is_tool_prefix(const char *s, gsize len)
{
    while (len && (*s == ' ' || *s == '\t' || *s == '*' || *s == '`' ||
                   *s == '\n')) { s++; len--; }
    static const char *const kws[] = { "IMAGE:", "SEARCH:", "GO:",
                                       "NAVIGATE:" };
    for (gsize i = 0; i < G_N_ELEMENTS(kws); i++) {
        gsize kl = strlen(kws[i]);
        gsize cmp = len < kl ? len : kl;
        if (g_ascii_strncasecmp(s, kws[i], cmp) == 0)
            return TRUE;
    }
    return FALSE;
}

static char *
ns_ai_run_locked(const char *system_prompt, const GPtrArray *history,
                 const char *user_msg, gboolean stream)
{
    int n_hist = history ? (int)history->len : 0;
    struct llama_chat_message *msgs =
        g_new0(struct llama_chat_message, (gsize)n_hist + 2);
    char *system_full = g_model_thinks
        ? g_strconcat(system_prompt, " /no_think", NULL)
        : g_strdup(system_prompt);

    llama_token *toks = NULL;
    int n_prompt = 0;
    int drop = 0;
    for (;;) {
        int n_msgs = 0;
        msgs[n_msgs].role = "system";
        msgs[n_msgs].content = system_full;
        n_msgs++;
        for (int i = drop; i < n_hist; i++) {
            const ns_ai_turn *t = g_ptr_array_index(history, (guint)i);
            msgs[n_msgs].role = t->role;
            msgs[n_msgs].content = t->content;
            n_msgs++;
        }
        msgs[n_msgs].role = "user";
        msgs[n_msgs].content = user_msg;
        n_msgs++;

        char *prompt = ns_ai_build_prompt(msgs, n_msgs);
        g_free(toks);
        n_prompt = ns_ai_tokenize_prompt(prompt, &toks);
        g_free(prompt);
        if (n_prompt <= 0) break;
        if (n_prompt <= NS_AI_N_CTX - NS_AI_MAX_REPLY - 16) break;
        if (drop >= n_hist) {
            g_free(toks);
            toks = NULL;
            break;
        }
        drop += 2;
        if (drop > n_hist) drop = n_hist;
    }
    g_free(msgs);
    g_free(system_full);
    if (!toks || n_prompt <= 0) {
        g_free(toks);
        return NULL;
    }

    llama_memory_t mem = llama_get_memory(g_ctx);
    int n_match = 0;
    while (n_match < g_cached_n && n_match < n_prompt &&
           g_cached_toks[n_match] == toks[n_match])
        n_match++;
    if (n_match >= n_prompt)
        n_match = n_prompt - 1;
    if (n_match < g_cached_n)
        llama_memory_seq_rm(mem, 0, n_match, -1);

    struct llama_sampler_chain_params sp = llama_sampler_chain_default_params();
    struct llama_sampler *smpl = llama_sampler_chain_init(sp);
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.95f, 1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.7f));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    int cap = n_prompt + NS_AI_MAX_REPLY;
    llama_token *kept = g_malloc(sizeof(llama_token) * (gsize)cap);
    memcpy(kept, toks, sizeof(llama_token) * (gsize)n_prompt);
    int n_in_kv = n_prompt;

    GString *out = g_string_new(NULL);
    gboolean failed = FALSE;
    gboolean cancelled = FALSE;
    gsize streamed = 0;
    gboolean stream_decided = FALSE, stream_suppressed = FALSE;

    if (llama_decode(g_ctx, llama_batch_get_one(toks + n_match,
                                                n_prompt - n_match)) != 0)
        failed = TRUE;

    for (int step = 0; !failed && step < NS_AI_MAX_REPLY; step++) {
        if (ns_ai_job_cancelled()) {
            cancelled = TRUE;
            break;
        }
        llama_token id = llama_sampler_sample(smpl, g_ctx, -1);
        if (llama_vocab_is_eog(g_vocab, id))
            break;

        char piece[256];
        int pn = llama_token_to_piece(g_vocab, id, piece, sizeof piece, 0, false);
        if (pn > 0)
            g_string_append_len(out, piece, pn);

        if (stream && (!stream_decided || !stream_suppressed)) {
            char *stripped = g_model_thinks
                ? ns_ai_strip_think(out->str, out->len, TRUE) : NULL;
            const char *vis = stripped ? stripped : out->str;
            gsize vlen = stripped ? strlen(stripped) : out->len;
            if (!stream_decided &&
                (vlen >= 12 || memchr(vis, '\n', vlen))) {
                stream_decided = TRUE;
                stream_suppressed = ns_ai_reply_is_tool_prefix(vis, vlen);
            }
            if (stream_decided && !stream_suppressed && vlen > streamed) {
                ns_ai_job_stream(vis + streamed, vlen - streamed);
                streamed = vlen;
            }
            g_free(stripped);
        }

        if (llama_decode(g_ctx, llama_batch_get_one(&id, 1)) != 0)
            break;
        if (n_in_kv < cap)
            kept[n_in_kv++] = id;
    }

    llama_sampler_free(smpl);
    g_free(toks);
    g_last_use_us = g_get_monotonic_time();

    if (failed) {
        llama_memory_clear(mem, true);
        g_free(g_cached_toks);
        g_cached_toks = NULL;
        g_cached_n = 0;
        g_free(kept);
    } else {
        g_free(g_cached_toks);
        g_cached_toks = kept;
        g_cached_n = n_in_kv;
    }

    if (failed || cancelled) {
        g_string_free(out, TRUE);
        return NULL;
    }

    char *final = ns_ai_strip_think(out->str, out->len, FALSE);
    g_string_free(out, TRUE);
    gsize flen = strlen(final);
    if (stream && !stream_decided)
        stream_suppressed = ns_ai_reply_is_tool_prefix(final, flen);
    if (stream && !stream_suppressed && flen > streamed)
        ns_ai_job_stream(final + streamed, flen - streamed);
    return final;
}

static char *
ns_ai_run_tools_locked(const GPtrArray *history, const char *user_msg)
{
    char *first = ns_ai_run_locked(NS_AI_SYSTEM_PROMPT, history, user_msg,
                                   TRUE);
    if (!first) return NULL;

    char *kind = NULL, *arg = NULL;
    if (!ns_ai_parse_tool(first, &kind, &arg))
        return ns_ai_strip_nav_markers(first);
    g_free(first);

    const char *query = (arg && *arg) ? arg : user_msg;
    char *reply = NULL;

    if (g_str_equal(kind, "go")) {
        char *url = ns_ai_normalize_url(query);
        reply = url ? g_strdup_printf("@@NAVIGATE@@%s", url)
                    : g_strdup("I couldn't work out which page to open.");
        g_free(url);
    } else if (g_str_equal(kind, "image")) {
        ns_ai_job_set_phase("searching");
        reply = ns_ai_image_reply(query);
    } else {
        ns_ai_job_set_phase("searching");
        char *sources = NULL;
        char *ctx = ns_ai_web_search(query, &sources, NULL);
        if (ns_ai_job_cancelled()) {
            g_free(ctx);
            g_free(sources);
            g_free(kind);
            g_free(arg);
            return NULL;
        }
        if (ctx) {
            ns_ai_job_set_phase("thinking");
            char *augmented = g_strdup_printf(
                "Web search results for \"%s\":\n\n%s"
                "Using only these results, answer this request: %s",
                query, ctx, user_msg);
            char *answer = ns_ai_run_locked(NS_AI_ANSWER_PROMPT, NULL,
                                            augmented, TRUE);
            g_free(augmented);
            if (answer && *g_strstrip(answer))
                reply = sources
                      ? g_strdup_printf("%s\n\nSources:\n%s", answer, sources)
                      : g_strdup(answer);
            else
                reply = g_strdup_printf("Here's what I found:\n\n%s",
                                        sources ? sources : ctx);
            g_free(answer);
        } else {
            reply = g_strdup_printf(
                "I couldn't search the web for \"%s\" right now.", query);
        }
        g_free(ctx);
        g_free(sources);
    }

    if (reply && !g_str_equal(kind, "go"))
        ns_ai_strip_nav_markers(reply);
    g_free(kind);
    g_free(arg);
    return reply;
}

static gboolean
ns_ai_idle_unload_cb(gpointer ud)
{
    (void)ud;
    if (!g_mutex_trylock(&g_model_lock))
        return G_SOURCE_CONTINUE;
    gboolean loaded = g_model != NULL;
    if (loaded &&
        g_get_monotonic_time() - g_last_use_us > NS_AI_IDLE_UNLOAD_US) {
        ns_ai_unload_locked();
        loaded = FALSE;
        g_message("nordstjernen-ai: unloaded idle model");
    }
    if (!loaded)
        g_idle_timer = 0;
    g_mutex_unlock(&g_model_lock);
    return loaded ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void
ns_ai_job_finish(int job, const char *user_msg, char *reply, char *error)
{
    g_mutex_lock(&g_job_lock);
    if (job == g_job_id) {
        if (reply) {
            if (g_job_text) g_string_truncate(g_job_text, 0);
            else g_job_text = g_string_new(NULL);
            g_string_append(g_job_text, reply);
        }
        g_free(g_job_error);
        g_job_error = error;
        error = NULL;
        g_job_running = FALSE;
        g_free(g_job_phase);
        g_job_phase = NULL;
        if (reply && !g_job_cancel &&
            !g_str_has_prefix(reply, "@@NAVIGATE@@")) {
            ns_ai_history_append_locked("user", user_msg);
            ns_ai_history_append_locked("assistant", reply);
        }
    }
    g_mutex_unlock(&g_job_lock);
    g_free(reply);
    g_free(error);
}

typedef struct {
    int   job;
    char *msg;
} ns_ai_chat_job;

static gpointer
ns_ai_chat_thread(gpointer data)
{
    ns_ai_chat_job *cj = data;

    char *reply = NULL;
    if (ns_ai_job_cancelled()) {
        ns_ai_job_finish(cj->job, cj->msg, NULL, NULL);
        g_free(cj->msg);
        g_free(cj);
        return NULL;
    }

    ns_ai_job_set_phase("loading model");
    g_mutex_lock(&g_model_lock);
    char *error = NULL;
    if (ns_ai_ensure_loaded_locked()) {
        ns_ai_job_set_phase("thinking");
        g_mutex_lock(&g_job_lock);
        GPtrArray *hist = g_ptr_array_new_with_free_func(ns_ai_turn_free);
        for (guint i = 0; g_history && i < g_history->len; i++) {
            const ns_ai_turn *t = g_ptr_array_index(g_history, i);
            ns_ai_turn *c = g_new0(ns_ai_turn, 1);
            c->role = g_strdup(t->role);
            c->content = g_strdup(t->content);
            g_ptr_array_add(hist, c);
        }
        g_mutex_unlock(&g_job_lock);
        reply = ns_ai_run_tools_locked(hist, cj->msg);
        g_ptr_array_free(hist, TRUE);
        if (!reply && !ns_ai_job_cancelled())
            error = g_strdup("The model could not produce a reply.");
        if (!g_idle_timer)
            g_idle_timer = g_timeout_add_seconds(60, ns_ai_idle_unload_cb,
                                                 NULL);
    } else {
        error = g_strdup("The local AI model is not ready yet. Pick and "
                         "download a model from the start page first.");
    }
    g_last_use_us = g_get_monotonic_time();
    g_mutex_unlock(&g_model_lock);

    ns_ai_job_finish(cj->job, cj->msg, reply, error);
    g_free(cj->msg);
    g_free(cj);
    return NULL;
}

char *
ns_ai_chat_start(const char *user_msg)
{
    if (!user_msg || !*user_msg)
        return g_strdup("{\"job\":0,\"state\":\"error\","
                        "\"message\":\"Please type a message.\"}");

    g_mutex_lock(&g_job_lock);
    if (g_job_running) {
        char *out = g_strdup_printf("{\"job\":%d,\"busy\":true}", g_job_id);
        g_mutex_unlock(&g_job_lock);
        return out;
    }
    int job = ++g_job_id;
    g_job_running = TRUE;
    g_job_cancel = FALSE;
    g_free(g_job_phase);
    g_job_phase = g_strdup("starting");
    g_free(g_job_error);
    g_job_error = NULL;
    if (g_job_text) g_string_truncate(g_job_text, 0);
    GThread *old_chat = g_chat_thread;
    g_chat_thread = NULL;
    g_mutex_unlock(&g_job_lock);

    if (old_chat)
        g_thread_join(old_chat);

    ns_ai_chat_job *cj = g_new0(ns_ai_chat_job, 1);
    cj->job = job;
    cj->msg = g_strdup(user_msg);
    GThread *t = g_thread_new("ns-ai-chat", ns_ai_chat_thread, cj);
    g_mutex_lock(&g_job_lock);
    g_chat_thread = t;
    g_mutex_unlock(&g_job_lock);

    return g_strdup_printf("{\"job\":%d}", job);
}

static char *
ns_ai_job_text_utf8_locked(gboolean running)
{
    const char *raw = g_job_text ? g_job_text->str : "";
    gsize len = g_job_text ? g_job_text->len : 0;
    const char *valid_end = NULL;
    if (g_utf8_validate(raw, (gssize)len, &valid_end))
        return g_strndup(raw, len);
    if (running)
        return g_strndup(raw, (gsize)(valid_end - raw));
    return g_utf8_make_valid(raw, (gssize)len);
}

char *
ns_ai_chat_poll(void)
{
    g_mutex_lock(&g_job_lock);
    const char *state = g_job_running ? "running"
                      : g_job_error   ? "error" : "done";
    char *text_utf8 = ns_ai_job_text_utf8_locked(g_job_running);
    char *text = ns_ai_json_escape(text_utf8);
    g_free(text_utf8);
    char *phase = ns_ai_json_escape(g_job_phase ? g_job_phase : "");
    char *err = g_job_error ? ns_ai_json_escape(g_job_error) : NULL;
    int job = g_job_id;
    g_mutex_unlock(&g_job_lock);

    GString *out = g_string_new(NULL);
    g_string_append_printf(out,
        "{\"job\":%d,\"state\":\"%s\",\"phase\":\"%s\",\"text\":\"%s\"",
        job, state, phase, text);
    if (err)
        g_string_append_printf(out, ",\"message\":\"%s\"", err);
    g_string_append_c(out, '}');
    g_free(text);
    g_free(phase);
    g_free(err);
    return g_string_free(out, FALSE);
}

void
ns_ai_chat_reset(void)
{
    g_mutex_lock(&g_job_lock);
    g_job_cancel = TRUE;
    if (g_history) g_ptr_array_set_size(g_history, 0);
    if (g_job_text && !g_job_running) g_string_truncate(g_job_text, 0);
    g_free(g_job_error);
    g_job_error = NULL;
    g_mutex_unlock(&g_job_lock);
}

void
ns_ai_shutdown(void)
{
    g_mutex_lock(&g_job_lock);
    g_job_cancel = TRUE;
    GThread *chat = g_chat_thread;
    g_chat_thread = NULL;
    g_mutex_unlock(&g_job_lock);

    g_mutex_lock(&g_dl_lock);
    g_ai_quit = TRUE;
    GThread *dl = g_dl_thread;
    g_dl_thread = NULL;
    g_mutex_unlock(&g_dl_lock);

    if (chat) g_thread_join(chat);
    if (dl) g_thread_join(dl);

    g_mutex_lock(&g_model_lock);
    if (g_idle_timer) {
        g_source_remove(g_idle_timer);
        g_idle_timer = 0;
    }
    ns_ai_unload_locked();
    g_mutex_unlock(&g_model_lock);
}

#endif
