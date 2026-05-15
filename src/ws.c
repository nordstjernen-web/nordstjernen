/* Nordstjernen — minimal WebSocket client (libcurl-backed).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "ws.h"

#include <curl/curl.h>
#include <curl/websockets.h>

#include <string.h>

#include "net.h"

#define ND_WS_RECV_BUF       8192
#define ND_WS_MAX_MESSAGE    (8 * 1024 * 1024)
#define ND_WS_POLL_USEC      20000

typedef enum {
    ND_WS_OUT_TEXT,
    ND_WS_OUT_BINARY,
    ND_WS_OUT_CLOSE,
} nd_ws_out_kind;

typedef struct {
    nd_ws_out_kind kind;
    guint8        *data;
    gsize          len;
    int            close_code;
} nd_ws_out_msg;

struct nd_ws {
    volatile gint     refcount;

    char             *url;
    char             *origin;
    GPtrArray        *protocols;

    nd_ws_callbacks   cbs;
    gpointer          user_data;

    GMutex            lock;
    GQueue            out_queue;
    volatile gint     state;
    volatile gint     exit_requested;
    volatile gint     detached;

    GThread          *thread;

    GByteArray       *recv_assembly;
    int               recv_assembly_kind;
};

static nd_ws *
nd_ws_ref(nd_ws *ws)
{
    if (ws) g_atomic_int_inc(&ws->refcount);
    return ws;
}

static void
nd_ws_destroy(nd_ws *ws)
{
    if (!ws) return;
    g_free(ws->url);
    g_free(ws->origin);
    if (ws->protocols) g_ptr_array_free(ws->protocols, TRUE);
    while (!g_queue_is_empty(&ws->out_queue)) {
        nd_ws_out_msg *m = g_queue_pop_head(&ws->out_queue);
        g_free(m->data);
        g_free(m);
    }
    if (ws->recv_assembly) g_byte_array_free(ws->recv_assembly, TRUE);
    g_mutex_clear(&ws->lock);
    g_free(ws);
}

static void
nd_ws_unref(nd_ws *ws)
{
    if (!ws) return;
    if (g_atomic_int_dec_and_test(&ws->refcount))
        nd_ws_destroy(ws);
}

typedef struct {
    nd_ws  *ws;
    void  (*invoke)(struct nd_ws *ws, gpointer payload);
    gpointer payload;
    void   (*payload_free)(gpointer);
} nd_ws_dispatch;

static gboolean
nd_ws_dispatch_run(gpointer data)
{
    nd_ws_dispatch *d = data;
    nd_ws *ws = d->ws;
    if (!g_atomic_int_get(&ws->detached) && d->invoke)
        d->invoke(ws, d->payload);
    if (d->payload && d->payload_free) d->payload_free(d->payload);
    nd_ws_unref(ws);
    g_free(d);
    return G_SOURCE_REMOVE;
}

static void
nd_ws_post(nd_ws *ws,
           void (*invoke)(nd_ws *, gpointer),
           gpointer payload,
           void   (*payload_free)(gpointer))
{
    nd_ws_dispatch *d = g_new0(nd_ws_dispatch, 1);
    d->ws = nd_ws_ref(ws);
    d->invoke = invoke;
    d->payload = payload;
    d->payload_free = payload_free;
    g_idle_add(nd_ws_dispatch_run, d);
}

typedef struct {
    GByteArray *data;
    gboolean    is_text;
} nd_ws_msg_payload;

typedef struct {
    int   code;
    char *reason;
    gboolean clean;
} nd_ws_close_payload;

static void
nd_ws_msg_payload_free(gpointer p)
{
    nd_ws_msg_payload *m = p;
    if (m->data) g_byte_array_free(m->data, TRUE);
    g_free(m);
}

static void
nd_ws_close_payload_free(gpointer p)
{
    nd_ws_close_payload *c = p;
    g_free(c->reason);
    g_free(c);
}

static void
nd_ws_invoke_open(nd_ws *ws, gpointer payload)
{
    (void)payload;
    if (ws->cbs.on_open) ws->cbs.on_open(ws->user_data);
}

static void
nd_ws_invoke_msg(nd_ws *ws, gpointer payload)
{
    nd_ws_msg_payload *m = payload;
    if (m->is_text) {
        if (ws->cbs.on_text)
            ws->cbs.on_text((const char *)m->data->data, m->data->len, ws->user_data);
    } else {
        if (ws->cbs.on_binary)
            ws->cbs.on_binary(m->data->data, m->data->len, ws->user_data);
    }
}

static void
nd_ws_invoke_close(nd_ws *ws, gpointer payload)
{
    nd_ws_close_payload *c = payload;
    if (ws->cbs.on_close)
        ws->cbs.on_close(c->code, c->reason ? c->reason : "", c->clean, ws->user_data);
}

static void
nd_ws_invoke_error(nd_ws *ws, gpointer payload)
{
    const char *msg = payload;
    if (ws->cbs.on_error) ws->cbs.on_error(msg, ws->user_data);
}

static void
nd_ws_dispatch_open(nd_ws *ws)
{
    g_atomic_int_set(&ws->state, ND_WS_STATE_OPEN);
    nd_ws_post(ws, nd_ws_invoke_open, NULL, NULL);
}

static void
nd_ws_dispatch_message(nd_ws *ws, gboolean is_text,
                       const guint8 *data, gsize len)
{
    nd_ws_msg_payload *m = g_new0(nd_ws_msg_payload, 1);
    m->is_text = is_text;
    m->data = g_byte_array_sized_new(len);
    if (len) g_byte_array_append(m->data, data, len);
    nd_ws_post(ws, nd_ws_invoke_msg, m, nd_ws_msg_payload_free);
}

static void
nd_ws_dispatch_close(nd_ws *ws, int code, const char *reason, gboolean clean)
{
    g_atomic_int_set(&ws->state, ND_WS_STATE_CLOSED);
    nd_ws_close_payload *c = g_new0(nd_ws_close_payload, 1);
    c->code = code;
    c->reason = reason ? g_strdup(reason) : NULL;
    c->clean = clean;
    nd_ws_post(ws, nd_ws_invoke_close, c, nd_ws_close_payload_free);
}

static void
nd_ws_dispatch_error(nd_ws *ws, const char *msg)
{
    nd_ws_post(ws, nd_ws_invoke_error, g_strdup(msg ? msg : ""), g_free);
}

static char *
nd_ws_to_http_url(const char *ws_url)
{
    if (!ws_url) return NULL;
    if (g_ascii_strncasecmp(ws_url, "wss://", 6) == 0)
        return g_strconcat("https://", ws_url + 6, NULL);
    if (g_ascii_strncasecmp(ws_url, "ws://", 5) == 0)
        return g_strconcat("http://",  ws_url + 5, NULL);
    return g_strdup(ws_url);
}

static gboolean
nd_ws_send_curl(CURL *curl, const guint8 *data, gsize len, unsigned int flags)
{
    for (;;) {
        size_t sent = 0;
        CURLcode rc = curl_ws_send(curl, len ? (const void *)data : "",
                                   len, &sent, 0, flags);
        if (rc == CURLE_AGAIN) {
            g_usleep(2000);
            continue;
        }
        return rc == CURLE_OK;
    }
}

static gboolean
nd_ws_send_close_frame(CURL *curl, int code, const char *reason)
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
nd_ws_drain_outgoing(nd_ws *ws, CURL *curl, gboolean *want_close,
                     int *close_code, char **close_reason)
{
    for (;;) {
        g_mutex_lock(&ws->lock);
        nd_ws_out_msg *m = g_queue_pop_head(&ws->out_queue);
        g_mutex_unlock(&ws->lock);
        if (!m) return;
        switch (m->kind) {
        case ND_WS_OUT_TEXT:
            nd_ws_send_curl(curl, m->data, m->len, CURLWS_TEXT);
            break;
        case ND_WS_OUT_BINARY:
            nd_ws_send_curl(curl, m->data, m->len, CURLWS_BINARY);
            break;
        case ND_WS_OUT_CLOSE:
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
nd_ws_handle_frame(nd_ws *ws, const guint8 *data, gsize len,
                   const struct curl_ws_frame *meta,
                   CURL *curl,
                   gboolean *peer_closed,
                   int *peer_code, char **peer_reason)
{
    int flags = meta->flags;

    if (flags & CURLWS_CLOSE) {
        int code = 1005;
        char *reason = NULL;
        if (len >= 2) {
            code = (data[0] << 8) | data[1];
            if (len > 2)
                reason = g_strndup((const char *)data + 2, len - 2);
        }
        *peer_closed = TRUE;
        *peer_code = code;
        *peer_reason = reason;
        return;
    }

    if (flags & CURLWS_PING) {
        size_t sent = 0;
        curl_ws_send(curl, data, len, &sent, 0, CURLWS_PONG);
        return;
    }

    if (!(flags & (CURLWS_TEXT | CURLWS_BINARY | CURLWS_CONT))) return;

    if (!ws->recv_assembly) ws->recv_assembly = g_byte_array_new();
    if (meta->offset == 0 && !(flags & CURLWS_CONT)) {
        g_byte_array_set_size(ws->recv_assembly, 0);
        ws->recv_assembly_kind = (flags & CURLWS_BINARY)
            ? CURLWS_BINARY : CURLWS_TEXT;
    }
    if (len && ws->recv_assembly->len + len <= ND_WS_MAX_MESSAGE)
        g_byte_array_append(ws->recv_assembly, data, len);

    if (meta->bytesleft == 0) {
        gboolean is_text = (ws->recv_assembly_kind != CURLWS_BINARY);
        nd_ws_dispatch_message(ws, is_text,
                               ws->recv_assembly->data,
                               ws->recv_assembly->len);
        g_byte_array_set_size(ws->recv_assembly, 0);
    }
}

static gpointer
nd_ws_worker(gpointer data)
{
    nd_ws *ws = data;
    CURL *curl = curl_easy_init();
    if (!curl) {
        nd_ws_dispatch_error(ws, "curl init failed");
        nd_ws_dispatch_close(ws, 1006, "init failed", FALSE);
        nd_ws_unref(ws);
        return NULL;
    }

    char *http_url = nd_ws_to_http_url(ws->url);
    curl_easy_setopt(curl, CURLOPT_URL, http_url);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ND_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);

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
    g_free(http_url);

    if (rc != CURLE_OK) {
        const char *msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        nd_ws_dispatch_error(ws, msg);
        nd_ws_dispatch_close(ws, 1006, msg, FALSE);
        if (headers) curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        nd_ws_unref(ws);
        return NULL;
    }

    nd_ws_dispatch_open(ws);

    gboolean clean_close = FALSE;
    int close_code = 1006;
    char *close_reason = NULL;
    gboolean peer_closed = FALSE;
    int peer_code = 1005;
    char *peer_reason = NULL;
    gboolean want_close = FALSE;

    guint8 buf[ND_WS_RECV_BUF];

    while (!g_atomic_int_get(&ws->exit_requested)) {
        nd_ws_drain_outgoing(ws, curl, &want_close, &close_code, &close_reason);
        if (want_close) {
            g_atomic_int_set(&ws->state, ND_WS_STATE_CLOSING);
            nd_ws_send_close_frame(curl, close_code, close_reason);
            clean_close = TRUE;
            break;
        }

        size_t got = 0;
        const struct curl_ws_frame *meta = NULL;
        rc = curl_ws_recv(curl, buf, sizeof buf, &got, &meta);
        if (rc == CURLE_AGAIN) {
            g_usleep(ND_WS_POLL_USEC);
            continue;
        }
        if (rc != CURLE_OK) {
            const char *msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
            nd_ws_dispatch_error(ws, msg);
            close_code = 1006;
            g_free(close_reason);
            close_reason = g_strdup(msg);
            break;
        }
        if (meta) {
            nd_ws_handle_frame(ws, buf, got, meta, curl,
                               &peer_closed, &peer_code, &peer_reason);
            if (peer_closed) {
                g_atomic_int_set(&ws->state, ND_WS_STATE_CLOSING);
                nd_ws_send_close_frame(curl, peer_code, NULL);
                clean_close = TRUE;
                close_code = peer_code;
                g_free(close_reason);
                close_reason = peer_reason ? g_strdup(peer_reason) : NULL;
                break;
            }
        }
    }

    nd_ws_dispatch_close(ws, close_code,
                         close_reason ? close_reason
                                      : (peer_reason ? peer_reason : ""),
                         clean_close);

    g_free(close_reason);
    g_free(peer_reason);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    nd_ws_unref(ws);
    return NULL;
}

gboolean
nd_ws_available(void)
{
    const curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    if (!v || !v->protocols) return FALSE;
    for (const char *const *p = v->protocols; *p; p++) {
        if (g_ascii_strcasecmp(*p, "ws") == 0) return TRUE;
        if (g_ascii_strcasecmp(*p, "wss") == 0) return TRUE;
    }
    return FALSE;
}

nd_ws *
nd_ws_new(const char        *url,
          const char        *origin,
          const char *const *protocols,
          const nd_ws_callbacks *cbs,
          gpointer           user_data)
{
    g_return_val_if_fail(url != NULL, NULL);
    if (!nd_ws_available()) return NULL;

    nd_ws *ws = g_new0(nd_ws, 1);
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
    g_queue_init(&ws->out_queue);
    g_atomic_int_set(&ws->state, ND_WS_STATE_CONNECTING);

    nd_ws_ref(ws);
    ws->thread = g_thread_new("nd-ws", nd_ws_worker, ws);
    return ws;
}

static gboolean
nd_ws_enqueue(nd_ws *ws, nd_ws_out_kind kind,
              const guint8 *data, gsize len, int close_code)
{
    if (!ws) return FALSE;
    int s = g_atomic_int_get(&ws->state);
    if (s == ND_WS_STATE_CLOSED) return FALSE;
    if (kind != ND_WS_OUT_CLOSE &&
        (s == ND_WS_STATE_CLOSING || s == ND_WS_STATE_CONNECTING)) {
        if (s == ND_WS_STATE_CLOSING) return FALSE;
    }
    nd_ws_out_msg *m = g_new0(nd_ws_out_msg, 1);
    m->kind = kind;
    m->close_code = close_code;
    if (len > 0 && data) {
        m->data = g_memdup2(data, len);
        m->len = len;
    } else if (kind == ND_WS_OUT_CLOSE && data && len > 0) {
        m->data = g_memdup2(data, len);
        m->len = len;
    }
    g_mutex_lock(&ws->lock);
    g_queue_push_tail(&ws->out_queue, m);
    g_mutex_unlock(&ws->lock);
    return TRUE;
}

gboolean
nd_ws_send_text(nd_ws *ws, const char *text, gsize len)
{
    return nd_ws_enqueue(ws, ND_WS_OUT_TEXT,
                         (const guint8 *)text, len, 0);
}

gboolean
nd_ws_send_binary(nd_ws *ws, const guint8 *data, gsize len)
{
    return nd_ws_enqueue(ws, ND_WS_OUT_BINARY, data, len, 0);
}

void
nd_ws_close(nd_ws *ws, int code, const char *reason)
{
    if (!ws) return;
    int s = g_atomic_int_get(&ws->state);
    if (s == ND_WS_STATE_CLOSING || s == ND_WS_STATE_CLOSED) return;
    if (code <= 0) code = 1000;
    gsize rlen = reason ? strlen(reason) : 0;
    nd_ws_enqueue(ws, ND_WS_OUT_CLOSE, (const guint8 *)reason, rlen, code);
    g_atomic_int_set(&ws->state, ND_WS_STATE_CLOSING);
}

int
nd_ws_state_get(nd_ws *ws)
{
    if (!ws) return ND_WS_STATE_CLOSED;
    return g_atomic_int_get(&ws->state);
}

void
nd_ws_free(nd_ws *ws)
{
    if (!ws) return;
    g_atomic_int_set(&ws->detached, 1);
    g_atomic_int_set(&ws->exit_requested, 1);
    if (ws->thread) {
        g_thread_join(ws->thread);
        ws->thread = NULL;
    }
    nd_ws_unref(ws);
}
