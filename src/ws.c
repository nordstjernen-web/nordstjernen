/* Nordstjernen — WebSocket client over libcurl's native WebSocket API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "ws.h"

#include <curl/curl.h>
#include <curl/websockets.h>

#include <string.h>

#include "net.h"

#define NS_WS_RECV_BUF       8192
#define NS_WS_MAX_MESSAGE    (8 * 1024 * 1024)
#define NS_WS_POLL_MSEC      10
#define NS_WS_MAX_PENDING    256

typedef enum {
    NS_WS_OUT_TEXT,
    NS_WS_OUT_BINARY,
    NS_WS_OUT_CLOSE,
} ns_ws_out_kind;

typedef struct {
    ns_ws_out_kind kind;
    guint8        *data;
    gsize          len;
    int            close_code;
} ns_ws_out_msg;

struct ns_ws {
    volatile gint     refcount;

    char             *url;
    char             *origin;
    GPtrArray        *protocols;

    ns_ws_callbacks   cbs;
    gpointer          user_data;

    GMutex            lock;
    GCond             cond;
    GQueue            out_queue;
    volatile gint     state;
    volatile gint     exit_requested;
    volatile gint     detached;
    volatile gint     pending;

    GThread          *thread;

    GByteArray       *recv_assembly;
    int               recv_assembly_kind;
    gboolean          recv_in_message;
};

static ns_ws *
ns_ws_ref(ns_ws *ws)
{
    if (ws) g_atomic_int_inc(&ws->refcount);
    return ws;
}

static void
ns_ws_destroy(ns_ws *ws)
{
    if (!ws) return;
    g_free(ws->url);
    g_free(ws->origin);
    if (ws->protocols) g_ptr_array_free(ws->protocols, TRUE);
    while (!g_queue_is_empty(&ws->out_queue)) {
        ns_ws_out_msg *m = g_queue_pop_head(&ws->out_queue);
        g_free(m->data);
        g_free(m);
    }
    if (ws->recv_assembly) g_byte_array_free(ws->recv_assembly, TRUE);
    g_mutex_clear(&ws->lock);
    g_cond_clear(&ws->cond);
    g_free(ws);
}

static void
ns_ws_unref(ns_ws *ws)
{
    if (!ws) return;
    if (g_atomic_int_dec_and_test(&ws->refcount))
        ns_ws_destroy(ws);
}

typedef struct {
    ns_ws  *ws;
    void  (*invoke)(struct ns_ws *ws, gpointer payload);
    gpointer payload;
    void   (*payload_free)(gpointer);
} ns_ws_dispatch;

static gboolean
ns_ws_dispatch_run(gpointer data)
{
    ns_ws_dispatch *d = data;
    ns_ws *ws = d->ws;
    if (!g_atomic_int_get(&ws->detached) && ws->cbs.busy &&
        ws->cbs.busy(ws->user_data)) {
        g_timeout_add(4, ns_ws_dispatch_run, d);
        return G_SOURCE_REMOVE;
    }
    if (!g_atomic_int_get(&ws->detached) && d->invoke)
        d->invoke(ws, d->payload);
    if (d->payload && d->payload_free) d->payload_free(d->payload);
    g_atomic_int_add(&ws->pending, -1);
    ns_ws_unref(ws);
    g_free(d);
    return G_SOURCE_REMOVE;
}

static void
ns_ws_post(ns_ws *ws,
           void (*invoke)(ns_ws *, gpointer),
           gpointer payload,
           void   (*payload_free)(gpointer),
           gboolean droppable)
{
    if (g_atomic_int_get(&ws->detached)) {
        if (payload && payload_free) payload_free(payload);
        return;
    }
    if (droppable && g_atomic_int_get(&ws->pending) >= NS_WS_MAX_PENDING) {
        if (payload && payload_free) payload_free(payload);
        return;
    }
    g_atomic_int_inc(&ws->pending);
    ns_ws_dispatch *d = g_new0(ns_ws_dispatch, 1);
    d->ws = ns_ws_ref(ws);
    d->invoke = invoke;
    d->payload = payload;
    d->payload_free = payload_free;
    g_idle_add(ns_ws_dispatch_run, d);
}

typedef struct {
    GByteArray *data;
    gboolean    is_text;
} ns_ws_msg_payload;

typedef struct {
    int   code;
    char *reason;
    gboolean clean;
} ns_ws_close_payload;

static void
ns_ws_msg_payload_free(gpointer p)
{
    ns_ws_msg_payload *m = p;
    if (m->data) g_byte_array_free(m->data, TRUE);
    g_free(m);
}

static void
ns_ws_close_payload_free(gpointer p)
{
    ns_ws_close_payload *c = p;
    g_free(c->reason);
    g_free(c);
}

static void
ns_ws_invoke_open(ns_ws *ws, gpointer payload)
{
    (void)payload;
    if (ws->cbs.on_open) ws->cbs.on_open(ws->user_data);
}

static void
ns_ws_invoke_msg(ns_ws *ws, gpointer payload)
{
    ns_ws_msg_payload *m = payload;
    if (m->is_text) {
        if (ws->cbs.on_text) {
            const char *text = m->data->len ? (const char *)m->data->data : "";
            ws->cbs.on_text(text, m->data->len, ws->user_data);
        }
    } else {
        if (ws->cbs.on_binary) {
            const guint8 *bytes = m->data->len
                ? m->data->data : (const guint8 *)"";
            ws->cbs.on_binary(bytes, m->data->len, ws->user_data);
        }
    }
}

static void
ns_ws_invoke_close(ns_ws *ws, gpointer payload)
{
    ns_ws_close_payload *c = payload;
    if (ws->cbs.on_close)
        ws->cbs.on_close(c->code, c->reason ? c->reason : "", c->clean, ws->user_data);
}

static void
ns_ws_invoke_error(ns_ws *ws, gpointer payload)
{
    const char *msg = payload;
    if (ws->cbs.on_error) ws->cbs.on_error(msg, ws->user_data);
}

static void
ns_ws_dispatch_open(ns_ws *ws)
{
    g_atomic_int_set(&ws->state, NS_WS_STATE_OPEN);
    ns_ws_post(ws, ns_ws_invoke_open, NULL, NULL, FALSE);
}

static void
ns_ws_dispatch_message(ns_ws *ws, gboolean is_text,
                       const guint8 *data, gsize len)
{
    ns_ws_msg_payload *m = g_new0(ns_ws_msg_payload, 1);
    m->is_text = is_text;
    m->data = g_byte_array_sized_new(len);
    if (len) g_byte_array_append(m->data, data, len);
    ns_ws_post(ws, ns_ws_invoke_msg, m, ns_ws_msg_payload_free, TRUE);
}

static void
ns_ws_dispatch_close(ns_ws *ws, int code, const char *reason, gboolean clean)
{
    g_atomic_int_set(&ws->state, NS_WS_STATE_CLOSED);
    ns_ws_close_payload *c = g_new0(ns_ws_close_payload, 1);
    c->code = code;
    c->reason = reason ? g_strdup(reason) : NULL;
    c->clean = clean;
    ns_ws_post(ws, ns_ws_invoke_close, c, ns_ws_close_payload_free, FALSE);
}

static void
ns_ws_dispatch_error(ns_ws *ws, const char *msg)
{
    ns_ws_post(ws, ns_ws_invoke_error, g_strdup(msg ? msg : ""), g_free, FALSE);
}

static gboolean
ns_ws_send_curl(ns_ws *ws, CURL *curl, const guint8 *data, gsize len,
                unsigned int flags)
{
    gsize off = 0;
    int stalls = 0;
    for (;;) {
        if (g_atomic_int_get(&ws->exit_requested)) return FALSE;
        gsize remain = len - off;
        size_t sent = 0;
        CURLcode rc = curl_ws_send(curl,
                                   remain ? (const void *)(data + off) : "",
                                   remain, &sent, 0, flags);
        if (rc == CURLE_AGAIN) {
            if (++stalls > 5000) return FALSE;
            g_usleep(2000);
            continue;
        }
        if (rc != CURLE_OK) return FALSE;
        off += sent;
        if (off >= len) return TRUE;
        if (sent == 0) {
            if (++stalls > 5000) return FALSE;
            g_usleep(2000);
        } else {
            stalls = 0;
        }
    }
}

static int
ns_ws_echo_close_code(int code)
{
    if (code == 1000 || code == 1001 || code == 1002 || code == 1003 ||
        (code >= 1007 && code <= 1014) || (code >= 3000 && code <= 4999))
        return code;
    return 0;
}

static gboolean
ns_ws_send_close_frame(CURL *curl, int code, const char *reason)
{
    guint8 buf[125];
    gsize  len = 0;
    if (code > 0) {
        buf[0] = (guint8)((code >> 8) & 0xff);
        buf[1] = (guint8)(code & 0xff);
        len = 2;
        if (reason && *reason) {
            gsize rlen = strlen(reason);
            if (rlen > sizeof buf - 2) rlen = sizeof buf - 2;
            memcpy(buf + 2, reason, rlen);
            len += rlen;
        }
    }
    size_t sent = 0;
    CURLcode rc = curl_ws_send(curl, len ? buf : (const void *)"", len,
                               &sent, 0, CURLWS_CLOSE);
    return rc == CURLE_OK;
}

static void
ns_ws_drain_outgoing(ns_ws *ws, CURL *curl, gboolean *want_close,
                     int *close_code, char **close_reason)
{
    for (;;) {
        g_mutex_lock(&ws->lock);
        ns_ws_out_msg *m = g_queue_pop_head(&ws->out_queue);
        g_mutex_unlock(&ws->lock);
        if (!m) return;
        switch (m->kind) {
        case NS_WS_OUT_TEXT:
            ns_ws_send_curl(ws, curl, m->data, m->len, CURLWS_TEXT);
            break;
        case NS_WS_OUT_BINARY:
            ns_ws_send_curl(ws, curl, m->data, m->len, CURLWS_BINARY);
            break;
        case NS_WS_OUT_CLOSE:
            *want_close = TRUE;
            *close_code = m->close_code;
            g_free(*close_reason);
            *close_reason = m->data ? g_strndup((const char *)m->data, m->len) : NULL;
            break;
        }
        g_free(m->data);
        g_free(m);
        if (*want_close) return;
    }
}

static void
ns_ws_handle_frame(ns_ws *ws, const guint8 *data, gsize len,
                   const struct curl_ws_frame *meta,
                   CURL *curl,
                   gboolean *peer_closed,
                   int *peer_code, char **peer_reason,
                   gboolean *bad_utf8, gboolean *too_big)
{
    int flags = meta->flags;

    if (flags & CURLWS_CLOSE) {
        int code = 1005;
        char *reason = NULL;
        if (data && len >= 2 && len <= 125) {
            code = (data[0] << 8) | data[1];
            if (len > 2)
                reason = g_strndup((const char *)data + 2, len - 2);
        } else if (data && len == 1) {
            code = 1002;
        }
        *peer_closed = TRUE;
        *peer_code = code;
        *peer_reason = reason;
        return;
    }

    if (flags & CURLWS_PING) {
        size_t sent = 0;
        size_t pong_len = (data && len > 0) ? (len > 125 ? 125 : len) : 0;
        curl_ws_send(curl, pong_len ? data : NULL, pong_len, &sent, 0,
                     CURLWS_PONG);
        return;
    }

    if (!(flags & (CURLWS_TEXT | CURLWS_BINARY | CURLWS_CONT))) return;

    if (!ws->recv_assembly) ws->recv_assembly = g_byte_array_new();
    if (meta->offset == 0 && !ws->recv_in_message &&
        (flags & (CURLWS_TEXT | CURLWS_BINARY))) {
        g_byte_array_set_size(ws->recv_assembly, 0);
        ws->recv_assembly_kind = (flags & CURLWS_BINARY)
            ? CURLWS_BINARY : CURLWS_TEXT;
        ws->recv_in_message = TRUE;
    } else if (!ws->recv_in_message) {
        return;
    }
    if (len) {
        if (ws->recv_assembly->len + len > NS_WS_MAX_MESSAGE) {
            *too_big = TRUE;
            g_byte_array_set_size(ws->recv_assembly, 0);
            ws->recv_in_message = FALSE;
            return;
        }
        g_byte_array_append(ws->recv_assembly, data, len);
    }

    if (meta->bytesleft == 0 && !(flags & CURLWS_CONT)) {
        gboolean is_text = (ws->recv_assembly_kind != CURLWS_BINARY);
        if (is_text && ws->recv_assembly->len > 0 &&
            !g_utf8_validate((const char *)ws->recv_assembly->data,
                             ws->recv_assembly->len, NULL)) {
            *bad_utf8 = TRUE;
        } else {
            ns_ws_dispatch_message(ws, is_text,
                                   ws->recv_assembly->data,
                                   ws->recv_assembly->len);
        }
        g_byte_array_set_size(ws->recv_assembly, 0);
        ws->recv_in_message = FALSE;
    }
}

static void
ns_ws_worker_wait(ns_ws *ws)
{
    g_mutex_lock(&ws->lock);
    if (g_queue_is_empty(&ws->out_queue) &&
        !g_atomic_int_get(&ws->exit_requested)) {
        gint64 deadline = g_get_monotonic_time() +
                          (gint64)NS_WS_POLL_MSEC * G_TIME_SPAN_MILLISECOND;
        g_cond_wait_until(&ws->cond, &ws->lock, deadline);
    }
    g_mutex_unlock(&ws->lock);
}

static int
ns_ws_handshake_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                         curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    const ns_ws *ws = clientp;
    return g_atomic_int_get(&ws->exit_requested) ? 1 : 0;
}

static gpointer
ns_ws_worker_curl(gpointer data)
{
    ns_ws *ws = data;
    CURL *curl = curl_easy_init();
    if (!curl) {
        ns_ws_dispatch_error(ws, "curl init failed");
        ns_ws_dispatch_close(ws, 1006, "init failed", FALSE);
        ns_ws_unref(ws);
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, ws->url);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                     (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, NS_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    ns_net_apply_curl_tls(curl);
    ns_net_apply_curl_proxy(curl, ws->url);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ns_ws_handshake_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, ws);

    struct curl_slist *headers = NULL;
    if (ws->origin && *ws->origin) {
        char *h = g_strconcat("Origin: ", ws->origin, NULL);
        headers = curl_slist_append(headers, h);
        g_free(h);
    }
    if (ws->protocols && ws->protocols->len > 0) {
        GString *s = g_string_new("Sec-WebSocket-Protocol: ");
        for (guint i = 0; i < ws->protocols->len; i++) {
            if (i > 0) g_string_append(s, ", ");
            g_string_append(s, g_ptr_array_index(ws->protocols, i));
        }
        headers = curl_slist_append(headers, s->str);
        g_string_free(s, TRUE);
    }
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode rc = curl_easy_perform(curl);

    if (rc != CURLE_OK || g_atomic_int_get(&ws->exit_requested)) {
        const char *msg = rc != CURLE_OK
            ? (errbuf[0] ? errbuf : curl_easy_strerror(rc))
            : "aborted";
        if (rc != CURLE_OK) ns_ws_dispatch_error(ws, msg);
        ns_ws_dispatch_close(ws, 1006, msg, FALSE);
        if (headers) curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        ns_ws_unref(ws);
        return NULL;
    }

    {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (code != 0 && code != 101) {
            char *msg = g_strdup_printf(
                "WebSocket handshake failed (HTTP %ld)", code);
            ns_ws_dispatch_error(ws, msg);
            ns_ws_dispatch_close(ws, 1006, msg, FALSE);
            g_free(msg);
            if (headers) curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            ns_ws_unref(ws);
            return NULL;
        }
    }
    ns_ws_dispatch_open(ws);

    gboolean clean_close = FALSE;
    int close_code = 1006;
    char *close_reason = NULL;
    gboolean peer_closed = FALSE;
    int peer_code = 1005;
    char *peer_reason = NULL;
    gboolean want_close = FALSE;
    gboolean bad_utf8 = FALSE;
    gboolean too_big = FALSE;

    guint8 buf[NS_WS_RECV_BUF];

    while (!g_atomic_int_get(&ws->exit_requested)) {
        ns_ws_drain_outgoing(ws, curl, &want_close, &close_code, &close_reason);
        if (want_close) {
            g_atomic_int_set(&ws->state, NS_WS_STATE_CLOSING);
            ns_ws_send_close_frame(curl, close_code, close_reason);
            clean_close = TRUE;
            break;
        }

        size_t got = 0;
        const struct curl_ws_frame *meta = NULL;
        rc = curl_ws_recv(curl, buf, sizeof buf, &got, &meta);
        if (rc == CURLE_AGAIN) {
            ns_ws_worker_wait(ws);
            continue;
        }
        if (rc != CURLE_OK) {
            const char *msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
            ns_ws_dispatch_error(ws, msg);
            close_code = 1006;
            g_free(close_reason);
            close_reason = g_strdup(msg);
            break;
        }
        if (meta) {
            ns_ws_handle_frame(ws, buf, got, meta, curl,
                               &peer_closed, &peer_code, &peer_reason,
                               &bad_utf8, &too_big);
            if (too_big) {
                g_atomic_int_set(&ws->state, NS_WS_STATE_CLOSING);
                ns_ws_send_close_frame(curl, 1009, "message too big");
                clean_close = FALSE;
                close_code = 1009;
                g_free(close_reason);
                close_reason = g_strdup("message too big");
                break;
            }
            if (bad_utf8) {
                g_atomic_int_set(&ws->state, NS_WS_STATE_CLOSING);
                ns_ws_send_close_frame(curl, 1007, "invalid utf-8");
                clean_close = FALSE;
                close_code = 1007;
                g_free(close_reason);
                close_reason = g_strdup("invalid utf-8");
                break;
            }
            if (peer_closed) {
                g_atomic_int_set(&ws->state, NS_WS_STATE_CLOSING);
                ns_ws_send_close_frame(curl, ns_ws_echo_close_code(peer_code),
                                       NULL);
                clean_close = TRUE;
                close_code = peer_code;
                g_free(close_reason);
                close_reason = peer_reason ? g_strdup(peer_reason) : NULL;
                break;
            }
        }
    }

    ns_ws_dispatch_close(ws, close_code,
                         close_reason ? close_reason
                                      : (peer_reason ? peer_reason : ""),
                         clean_close);

    g_free(close_reason);
    g_free(peer_reason);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    ns_ws_unref(ws);
    return NULL;
}

static gboolean
ns_ws_curl_native(void)
{
    const curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    if (!v || !v->protocols) return FALSE;
    for (const char *const *p = v->protocols; *p; p++) {
        if (g_ascii_strcasecmp(*p, "ws") == 0) return TRUE;
        if (g_ascii_strcasecmp(*p, "wss") == 0) return TRUE;
    }
    return FALSE;
}

gboolean
ns_ws_available(void)
{
    return ns_ws_curl_native();
}

ns_ws *
ns_ws_new(const char        *url,
          const char        *origin,
          const char *const *protocols,
          const ns_ws_callbacks *cbs,
          gpointer           user_data)
{
    g_return_val_if_fail(url != NULL, NULL);
    if (!ns_ws_available()) return NULL;

    ns_ws *ws = g_new0(ns_ws, 1);
    ws->refcount = 1;
    ws->url    = g_strdup(url);
    ws->origin = origin ? g_strdup(origin) : NULL;
    if (protocols && protocols[0]) {
        ws->protocols = g_ptr_array_new_with_free_func(g_free);
        for (int i = 0; protocols[i]; i++)
            g_ptr_array_add(ws->protocols, g_strdup(protocols[i]));
    }
    if (cbs) ws->cbs = *cbs;
    ws->user_data = user_data;
    g_mutex_init(&ws->lock);
    g_cond_init(&ws->cond);
    g_queue_init(&ws->out_queue);
    g_atomic_int_set(&ws->state, NS_WS_STATE_CONNECTING);

    ns_ws_ref(ws);
    ws->thread = g_thread_new("nd-ws", ns_ws_worker_curl, ws);
    return ws;
}

static gboolean
ns_ws_enqueue(ns_ws *ws, ns_ws_out_kind kind,
              const guint8 *data, gsize len, int close_code)
{
    if (!ws) return FALSE;
    int s = g_atomic_int_get(&ws->state);
    if (s == NS_WS_STATE_CLOSED) return FALSE;
    if (kind != NS_WS_OUT_CLOSE && s == NS_WS_STATE_CLOSING) return FALSE;
    if (kind != NS_WS_OUT_CLOSE && len > NS_WS_MAX_MESSAGE) return FALSE;

    ns_ws_out_msg *m = g_new0(ns_ws_out_msg, 1);
    m->kind = kind;
    m->close_code = close_code;
    if (data && len > 0) {
        m->data = g_memdup2(data, len);
        m->len = len;
    }
    g_mutex_lock(&ws->lock);
    g_queue_push_tail(&ws->out_queue, m);
    g_cond_signal(&ws->cond);
    g_mutex_unlock(&ws->lock);
    return TRUE;
}

gboolean
ns_ws_send_text(ns_ws *ws, const char *text, gsize len)
{
    return ns_ws_enqueue(ws, NS_WS_OUT_TEXT,
                         (const guint8 *)text, len, 0);
}

gboolean
ns_ws_send_binary(ns_ws *ws, const guint8 *data, gsize len)
{
    return ns_ws_enqueue(ws, NS_WS_OUT_BINARY, data, len, 0);
}

void
ns_ws_close(ns_ws *ws, int code, const char *reason)
{
    if (!ws) return;
    int s = g_atomic_int_get(&ws->state);
    if (s == NS_WS_STATE_CLOSING || s == NS_WS_STATE_CLOSED) return;
    if (code <= 0) code = 1000;
    gsize rlen = reason ? strlen(reason) : 0;
    ns_ws_enqueue(ws, NS_WS_OUT_CLOSE, (const guint8 *)reason, rlen, code);
    g_atomic_int_set(&ws->state, NS_WS_STATE_CLOSING);
}

int
ns_ws_state_get(ns_ws *ws)
{
    if (!ws) return NS_WS_STATE_CLOSED;
    return g_atomic_int_get(&ws->state);
}

void
ns_ws_free(ns_ws *ws)
{
    if (!ws) return;
    g_atomic_int_set(&ws->detached, 1);
    g_atomic_int_set(&ws->exit_requested, 1);
    g_mutex_lock(&ws->lock);
    g_cond_signal(&ws->cond);
    g_mutex_unlock(&ws->lock);
    if (ws->thread) {
        g_thread_join(ws->thread);
        ws->thread = NULL;
    }
    ns_ws_unref(ws);
}
