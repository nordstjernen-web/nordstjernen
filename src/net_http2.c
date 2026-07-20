/* Nordstjernen — HTTP transport over libnghttp2 + OpenSSL (curl alternative). */

#include "net.h"
#include "net_backend.h"
#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <nghttp2/nghttp2.h>
#include <zlib.h>
#ifdef NS_HTTP_HAVE_BROTLI
#include <brotli/decode.h>
#endif

#ifndef _WIN32
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

typedef enum {
    NS_ENC_IDENTITY = 0,
    NS_ENC_GZIP,
    NS_ENC_DEFLATE,
    NS_ENC_BROTLI,
} ns_content_encoding;

typedef struct {
    const ns_hop_req *req;
    ns_write_ctx     *wctx;
    ns_header_ctx    *hctx;
    ns_hop_out       *out;
    GCancellable     *cancellable;
    gint64            start_us;
    gint64            deadline_us;

    SSL              *ssl;
    int               fd;

    const char       *url;
    long              status;
    ns_content_encoding encoding;
    GByteArray       *comp_buf;
    gboolean          status_line_fed;
    gboolean          got_first_byte;
    gboolean          sink_full;
    gboolean          stream_closed;
    gboolean          proto_error;

    const guint8     *body;
    size_t            body_len;
    size_t            body_off;
} ns_h2;

static gboolean
ns_h2_should_abort(ns_h2 *c)
{
    if (ns_net_aborting())
        return TRUE;
    if (c->cancellable && g_cancellable_is_cancelled(c->cancellable))
        return TRUE;
    if (g_get_monotonic_time() > c->deadline_us)
        return TRUE;
    return FALSE;
}

static double
ns_h2_ms_since(gint64 start_us)
{
    return (double)(g_get_monotonic_time() - start_us) / 1000.0;
}

#ifndef _WIN32

static int
ns_h2_default_port(gboolean https)
{
    return https ? 443 : 80;
}

static int
ns_h2_connect(const char *host, int port, gint64 deadline_us,
              GCancellable *cancellable, char **remote_ip_out,
              char **err_out, gboolean *timed_out)
{
    char portstr[16];
    g_snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0 || !res) {
        if (err_out)
            *err_out = g_strdup_printf("could not resolve host %s", host);
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ns_net_aborting() ||
            (cancellable && g_cancellable_is_cancelled(cancellable)))
            break;
        int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) continue;
        int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        int rc = connect(s, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            fd = s;
        } else if (errno == EINPROGRESS) {
            gboolean connected = FALSE;
            for (;;) {
                if (ns_net_aborting() ||
                    (cancellable && g_cancellable_is_cancelled(cancellable)))
                    break;
                gint64 remain = deadline_us - g_get_monotonic_time();
                if (remain <= 0) { if (timed_out) *timed_out = TRUE; break; }
                int slice = (int)(remain / 1000);
                if (slice > 500) slice = 500;
                struct pollfd pfd = { .fd = s, .events = POLLOUT };
                int pr = poll(&pfd, 1, slice);
                if (pr < 0) { if (errno == EINTR) continue; break; }
                if (pr == 0) continue;
                int soerr = 0;
                socklen_t slen = sizeof soerr;
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0)
                    break;
                if (soerr == 0) { connected = TRUE; break; }
                break;
            }
            if (connected) fd = s;
        }

        if (fd == s) {
            fcntl(s, F_SETFL, flags);
            if (remote_ip_out && !*remote_ip_out) {
                char ipbuf[INET6_ADDRSTRLEN] = {0};
                void *addr = NULL;
                if (ai->ai_family == AF_INET)
                    addr = &((struct sockaddr_in *)ai->ai_addr)->sin_addr;
                else if (ai->ai_family == AF_INET6)
                    addr = &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr;
                if (addr && inet_ntop(ai->ai_family, addr, ipbuf, sizeof ipbuf))
                    *remote_ip_out = g_strdup(ipbuf);
            }
            break;
        }
        close(s);
    }
    freeaddrinfo(res);
    if (fd < 0 && err_out && !*err_out)
        *err_out = g_strdup_printf("could not connect to %s:%d", host, port);
    return fd;
}

static void
ns_h2_apply_socket_timeout(int fd)
{
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

static const unsigned char ns_h2_alpn[] = { 2, 'h', '2', 8, 'h', 't', 't', 'p',
                                            '/', '1', '.', '1' };

static SSL_CTX *
ns_h2_ssl_ctx(gboolean verify)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_cipher_list(ctx,
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
        "ECDHE-RSA-AES128-SHA:ECDHE-RSA-AES256-SHA:"
        "AES128-GCM-SHA256:AES256-GCM-SHA384:AES128-SHA:AES256-SHA");
    SSL_CTX_set_ciphersuites(ctx,
        "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:"
        "TLS_CHACHA20_POLY1305_SHA256");
    const char *curves = ns_net_ec_curves();
    if (curves && *curves)
        SSL_CTX_set1_groups_list(ctx, curves);
    if (verify) {
        const char *ca = ns_net_ca_bundle_path();
        if (ca && *ca)
            SSL_CTX_load_verify_locations(ctx, ca, NULL);
        else
            SSL_CTX_set_default_verify_paths(ctx);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    }
    SSL_CTX_set_alpn_protos(ctx, ns_h2_alpn, sizeof ns_h2_alpn);
    return ctx;
}

static gboolean
ns_h2_retryable(int e)
{
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    if (e == EWOULDBLOCK) return TRUE;
#endif
    return e == EAGAIN || e == EINTR;
}

static gboolean
ns_h2_handshake(SSL *ssl, ns_h2 *c, long *verify_result_out)
{
    for (;;) {
        int r = SSL_connect(ssl);
        if (r == 1) {
            if (verify_result_out)
                *verify_result_out = SSL_get_verify_result(ssl);
            return TRUE;
        }
        int err = SSL_get_error(ssl, r);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            if (ns_h2_should_abort(c)) return FALSE;
            continue;
        }
        if ((err == SSL_ERROR_SYSCALL || err == SSL_ERROR_WANT_READ) &&
            ns_h2_retryable(errno)) {
            if (ns_h2_should_abort(c)) return FALSE;
            continue;
        }
        if (verify_result_out)
            *verify_result_out = SSL_get_verify_result(ssl);
        return FALSE;
    }
}

static int
ns_h2_ssl_read(ns_h2 *c, void *buf, int len)
{
    if (!c->ssl) {
        for (;;) {
            ssize_t r = recv(c->fd, buf, (size_t)len, 0);
            if (r > 0) return (int)r;
            if (r == 0) return 0;
            if (ns_h2_retryable(errno)) {
                if (ns_h2_should_abort(c)) return -1;
                continue;
            }
            return -1;
        }
    }
    for (;;) {
        int r = SSL_read(c->ssl, buf, len);
        if (r > 0) return r;
        int err = SSL_get_error(c->ssl, r);
        if (err == SSL_ERROR_ZERO_RETURN) return 0;
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            if (ns_h2_should_abort(c)) return -1;
            continue;
        }
        if (err == SSL_ERROR_SYSCALL &&
            ns_h2_retryable(errno)) {
            if (ns_h2_should_abort(c)) return -1;
            continue;
        }
        if (err == SSL_ERROR_SYSCALL && errno == 0) return 0;
        return -1;
    }
}

static gboolean
ns_h2_ssl_write_all(ns_h2 *c, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    size_t off = 0;
    if (!c->ssl) {
        while (off < len) {
            ssize_t r = send(c->fd, p + off, len - off, MSG_NOSIGNAL);
            if (r > 0) { off += (size_t)r; continue; }
            if (r < 0 && ns_h2_retryable(errno)) {
                if (ns_h2_should_abort(c)) return FALSE;
                continue;
            }
            return FALSE;
        }
        return TRUE;
    }
    while (off < len) {
        int r = SSL_write(c->ssl, p + off, (int)(len - off));
        if (r > 0) { off += (size_t)r; continue; }
        int err = SSL_get_error(c->ssl, r);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE ||
            (err == SSL_ERROR_SYSCALL &&
             ns_h2_retryable(errno))) {
            if (ns_h2_should_abort(c)) return FALSE;
            continue;
        }
        return FALSE;
    }
    return TRUE;
}

static gboolean
ns_h2_inflate_try(GByteArray *in, ns_write_ctx *wctx, int window_bits)
{
    z_stream zs = {0};
    if (inflateInit2(&zs, window_bits) != Z_OK)
        return FALSE;
    zs.next_in = in->data;
    zs.avail_in = in->len;
    unsigned char outbuf[16384];
    gboolean ok = TRUE;
    gboolean produced_any = FALSE;
    for (;;) {
        zs.next_out = outbuf;
        zs.avail_out = sizeof outbuf;
        int r = inflate(&zs, Z_NO_FLUSH);
        size_t produced = sizeof outbuf - zs.avail_out;
        if (produced) {
            produced_any = TRUE;
            if (!ns_body_sink_write(wctx, outbuf, produced)) { ok = FALSE; break; }
        }
        if (r == Z_STREAM_END) break;
        if (r == Z_OK || r == Z_BUF_ERROR) {
            if (zs.avail_in == 0 && produced == 0) break;
            continue;
        }
        ok = FALSE;
        break;
    }
    inflateEnd(&zs);
    if (!produced_any && in->len > 0)
        return FALSE;
    return ok;
}

#ifdef NS_HTTP_HAVE_BROTLI
static gboolean
ns_h2_brotli(GByteArray *in, ns_write_ctx *wctx)
{
    BrotliDecoderState *st = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (!st) return FALSE;
    const uint8_t *next_in = in->data;
    size_t avail_in = in->len;
    unsigned char outbuf[16384];
    gboolean ok = TRUE;
    for (;;) {
        uint8_t *next_out = outbuf;
        size_t avail_out = sizeof outbuf;
        BrotliDecoderResult r = BrotliDecoderDecompressStream(
            st, &avail_in, &next_in, &avail_out, &next_out, NULL);
        size_t produced = sizeof outbuf - avail_out;
        if (produced && !ns_body_sink_write(wctx, outbuf, produced)) {
            ok = FALSE;
            break;
        }
        if (r == BROTLI_DECODER_RESULT_SUCCESS) break;
        if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
            if (avail_in == 0) break;
            continue;
        }
        if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT)
            continue;
        ok = FALSE;
        break;
    }
    BrotliDecoderDestroyInstance(st);
    return ok;
}
#endif

static void
ns_h2_flush_body(ns_h2 *c)
{
    if (!c->comp_buf)
        return;
    gboolean ok = TRUE;
    switch (c->encoding) {
    case NS_ENC_GZIP:
        ok = ns_h2_inflate_try(c->comp_buf, c->wctx, 15 + 32);
        break;
    case NS_ENC_DEFLATE:
        ok = ns_h2_inflate_try(c->comp_buf, c->wctx, 15 + 32);
        if (!ok)
            ok = ns_h2_inflate_try(c->comp_buf, c->wctx, -15);
        break;
    case NS_ENC_BROTLI:
#ifdef NS_HTTP_HAVE_BROTLI
        ok = ns_h2_brotli(c->comp_buf, c->wctx);
#else
        ok = ns_body_sink_write(c->wctx, c->comp_buf->data, c->comp_buf->len);
#endif
        break;
    default:
        ok = ns_body_sink_write(c->wctx, c->comp_buf->data, c->comp_buf->len);
        break;
    }
    if (!ok)
        c->sink_full = TRUE;
    g_byte_array_free(c->comp_buf, TRUE);
    c->comp_buf = NULL;
}

static void
ns_h2_feed_status_line(ns_h2 *c, const char *proto)
{
    if (c->status_line_fed)
        return;
    c->status_line_fed = TRUE;
    char line[64];
    int n = g_snprintf(line, sizeof line, "%s %ld\r\n", proto, c->status);
    ns_header_sink_feed(c->hctx, line, (size_t)n);
}

static void
ns_h2_on_response_header(ns_h2 *c, const char *name, size_t namelen,
                         const char *value, size_t valuelen)
{
    if (namelen == 0)
        return;
    if (name[0] == ':') {
        if (namelen == 7 && memcmp(name, ":status", 7) == 0) {
            char *tmp = g_strndup(value, valuelen);
            c->status = g_ascii_strtoll(tmp, NULL, 10);
            g_free(tmp);
            ns_h2_feed_status_line(c, "HTTP/2");
        }
        return;
    }
    if (namelen == 16 && g_ascii_strncasecmp(name, "content-encoding", 16) == 0) {
        if (valuelen == 4 && g_ascii_strncasecmp(value, "gzip", 4) == 0)
            c->encoding = NS_ENC_GZIP;
        else if (valuelen == 7 && g_ascii_strncasecmp(value, "deflate", 7) == 0)
            c->encoding = NS_ENC_DEFLATE;
        else if (valuelen == 2 && g_ascii_strncasecmp(value, "br", 2) == 0)
            c->encoding = NS_ENC_BROTLI;
    }
    if (namelen == 10 && g_ascii_strncasecmp(name, "set-cookie", 10) == 0) {
        char *v = g_strndup(value, valuelen);
        ns_net_store_set_cookie(c->url, v);
        g_free(v);
    }
    char *line = g_strdup_printf("%.*s: %.*s\r\n", (int)namelen, name,
                                 (int)valuelen, value);
    ns_header_sink_feed(c->hctx, line, strlen(line));
    g_free(line);
}

static void
ns_h2_on_body(ns_h2 *c, const uint8_t *data, size_t len)
{
    if (len == 0)
        return;
    c->got_first_byte = TRUE;
    if (c->out->t_starttransfer_ms == 0.0)
        c->out->t_starttransfer_ms = ns_h2_ms_since(c->start_us);
    if (c->encoding == NS_ENC_IDENTITY) {
        if (!ns_body_sink_write(c->wctx, data, len))
            c->sink_full = TRUE;
    } else {
        if (!c->comp_buf)
            c->comp_buf = g_byte_array_new();
        if (c->comp_buf->len + len <= c->wctx->budget)
            g_byte_array_append(c->comp_buf, data, len);
        else
            c->sink_full = TRUE;
    }
}

static ssize_t
ns_h2_send_cb(nghttp2_session *session, const uint8_t *data, size_t length,
              int flags, void *user_data)
{
    (void)session; (void)flags;
    ns_h2 *c = user_data;
    if (!ns_h2_ssl_write_all(c, data, length)) {
        c->proto_error = TRUE;
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return (ssize_t)length;
}

static int
ns_h2_header_cb(nghttp2_session *session, const nghttp2_frame *frame,
                const uint8_t *name, size_t namelen,
                const uint8_t *value, size_t valuelen,
                uint8_t flags, void *user_data)
{
    (void)session; (void)flags;
    ns_h2 *c = user_data;
    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_RESPONSE)
        ns_h2_on_response_header(c, (const char *)name, namelen,
                                 (const char *)value, valuelen);
    return 0;
}

static int
ns_h2_data_chunk_cb(nghttp2_session *session, uint8_t flags,
                    int32_t stream_id, const uint8_t *data, size_t len,
                    void *user_data)
{
    (void)session; (void)flags; (void)stream_id;
    ns_h2 *c = user_data;
    ns_h2_on_body(c, data, len);
    return 0;
}

static int
ns_h2_stream_close_cb(nghttp2_session *session, int32_t stream_id,
                      uint32_t error_code, void *user_data)
{
    (void)stream_id; (void)error_code;
    ns_h2 *c = user_data;
    c->stream_closed = TRUE;
    nghttp2_session_terminate_session(session, NGHTTP2_NO_ERROR);
    return 0;
}

static ssize_t
ns_h2_req_body_cb(nghttp2_session *session, int32_t stream_id, uint8_t *buf,
                  size_t length, uint32_t *data_flags,
                  nghttp2_data_source *source, void *user_data)
{
    (void)session; (void)stream_id; (void)source;
    ns_h2 *c = user_data;
    size_t remain = c->body_len - c->body_off;
    size_t n = remain < length ? remain : length;
    if (n) {
        memcpy(buf, c->body + c->body_off, n);
        c->body_off += n;
    }
    if (c->body_off >= c->body_len)
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return (ssize_t)n;
}

static void
ns_h2_add_nv(GArray *nva, const char *name, const char *value)
{
    nghttp2_nv nv = {
        .name = (uint8_t *)name,
        .value = (uint8_t *)value,
        .namelen = strlen(name),
        .valuelen = strlen(value),
        .flags = NGHTTP2_NV_FLAG_NONE,
    };
    g_array_append_val(nva, nv);
}

static gboolean
ns_h2_hdr_is_reserved(const char *lname, size_t len)
{
    static const char *const skip[] = {
        "host", "connection", "keep-alive", "proxy-connection",
        "transfer-encoding", "upgrade", "accept-encoding", "cookie",
        "user-agent", "referer", "content-length", NULL,
    };
    for (int i = 0; skip[i]; i++)
        if (strlen(skip[i]) == len && g_ascii_strncasecmp(lname, skip[i], len) == 0)
            return TRUE;
    return FALSE;
}

static gboolean
ns_h2_run_http2(ns_h2 *c, const char *authority, const char *path,
                const char *scheme)
{
    GPtrArray *owned = g_ptr_array_new_with_free_func(g_free);
    nghttp2_session_callbacks *cbs = NULL;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_send_callback(cbs, ns_h2_send_cb);
    nghttp2_session_callbacks_set_on_header_callback(cbs, ns_h2_header_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        cbs, ns_h2_data_chunk_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(
        cbs, ns_h2_stream_close_cb);

    nghttp2_session *session = NULL;
    nghttp2_session_client_new(&session, cbs, c);
    nghttp2_session_callbacks_del(cbs);
    if (!session) {
        g_ptr_array_free(owned, TRUE);
        return FALSE;
    }

    nghttp2_settings_entry iv[] = {
        { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100 },
        { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 8 * 1024 * 1024 },
    };
    nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv, 2);

    GArray *nva = g_array_new(FALSE, FALSE, sizeof(nghttp2_nv));
    const char *method = (c->req->method && *c->req->method) ? c->req->method
                                                             : "GET";
    ns_h2_add_nv(nva, ":method", method);
    ns_h2_add_nv(nva, ":scheme", scheme);
    ns_h2_add_nv(nva, ":authority", authority);
    ns_h2_add_nv(nva, ":path", path);
    if (c->req->user_agent)
        ns_h2_add_nv(nva, "user-agent", c->req->user_agent);
#ifdef NS_HTTP_HAVE_BROTLI
    ns_h2_add_nv(nva, "accept-encoding", "gzip, deflate, br");
#else
    ns_h2_add_nv(nva, "accept-encoding", "gzip, deflate");
#endif
    if (c->req->referer && *c->req->referer)
        ns_h2_add_nv(nva, "referer", c->req->referer);

    char *cookie = ns_net_cookies_for_request(c->url);
    if (cookie) {
        g_ptr_array_add(owned, cookie);
        ns_h2_add_nv(nva, "cookie", cookie);
    }

    for (struct curl_slist *h = c->req->headers; h; h = h->next) {
        const char *line = h->data;
        const char *colon = line ? strchr(line, ':') : NULL;
        if (!colon)
            continue;
        size_t nlen = (size_t)(colon - line);
        const char *val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;
        if (ns_h2_hdr_is_reserved(line, nlen))
            continue;
        char *lname = g_ascii_strdown(line, nlen);
        char *lval = g_strdup(val);
        g_ptr_array_add(owned, lname);
        g_ptr_array_add(owned, lval);
        ns_h2_add_nv(nva, lname, lval);
    }

    nghttp2_data_provider prov;
    nghttp2_data_provider *provp = NULL;
    if (c->body && c->body_len > 0) {
        prov.source.ptr = NULL;
        prov.read_callback = ns_h2_req_body_cb;
        provp = &prov;
    }

    int32_t sid = nghttp2_submit_request(session, NULL,
                                         (nghttp2_nv *)nva->data, nva->len,
                                         provp, c);
    g_array_free(nva, TRUE);
    g_ptr_array_free(owned, TRUE);
    if (sid < 0) {
        nghttp2_session_del(session);
        return FALSE;
    }

    gboolean ok = TRUE;
    unsigned char rbuf[65536];
    while (!c->stream_closed && !c->proto_error) {
        if (nghttp2_session_send(session) != 0) { ok = FALSE; break; }
        if (c->proto_error) { ok = FALSE; break; }
        if (!nghttp2_session_want_read(session) &&
            !nghttp2_session_want_write(session))
            break;
        if (ns_h2_should_abort(c)) { ok = FALSE; break; }
        int n = ns_h2_ssl_read(c, rbuf, sizeof rbuf);
        if (n < 0) { ok = FALSE; break; }
        if (n == 0) break;
        if (c->out->t_starttransfer_ms == 0.0)
            c->out->t_starttransfer_ms = ns_h2_ms_since(c->start_us);
        ssize_t rv = nghttp2_session_mem_recv(session, rbuf, (size_t)n);
        if (rv < 0) { ok = FALSE; break; }
    }
    if (c->proto_error)
        ok = FALSE;
    nghttp2_session_del(session);
    return ok && c->stream_closed;
}

static gboolean
ns_h2_read_line(ns_h2 *c, GByteArray *buf, size_t *pos, char **line_out,
                size_t *line_len)
{
    size_t search = *pos;
    for (;;) {
        for (size_t i = search; i + 1 < buf->len; i++) {
            if (buf->data[i] == '\r' && buf->data[i + 1] == '\n') {
                *line_out = (char *)buf->data + *pos;
                *line_len = i - *pos;
                *pos = i + 2;
                return TRUE;
            }
        }
        search = buf->len >= 1 ? buf->len - 1 : 0;
        unsigned char tmp[16384];
        int n = ns_h2_ssl_read(c, tmp, sizeof tmp);
        if (n <= 0)
            return FALSE;
        g_byte_array_append(buf, tmp, (guint)n);
        if (buf->len > 1024 * 1024)
            return FALSE;
    }
}

static gboolean
ns_h2_run_http1(ns_h2 *c, const char *authority, const char *path)
{
    GString *reqs = g_string_new(NULL);
    const char *method = (c->req->method && *c->req->method) ? c->req->method
                                                             : "GET";
    g_string_append_printf(reqs, "%s %s HTTP/1.1\r\n", method, path);
    g_string_append_printf(reqs, "Host: %s\r\n", authority);
    if (c->req->user_agent)
        g_string_append_printf(reqs, "User-Agent: %s\r\n", c->req->user_agent);
#ifdef NS_HTTP_HAVE_BROTLI
    g_string_append(reqs, "Accept-Encoding: gzip, deflate, br\r\n");
#else
    g_string_append(reqs, "Accept-Encoding: gzip, deflate\r\n");
#endif
    if (c->req->referer && *c->req->referer)
        g_string_append_printf(reqs, "Referer: %s\r\n", c->req->referer);
    char *cookie = ns_net_cookies_for_request(c->url);
    if (cookie) {
        g_string_append_printf(reqs, "Cookie: %s\r\n", cookie);
        g_free(cookie);
    }
    for (struct curl_slist *h = c->req->headers; h; h = h->next) {
        const char *line = h->data;
        const char *colon = line ? strchr(line, ':') : NULL;
        if (!colon) continue;
        size_t nlen = (size_t)(colon - line);
        if (ns_h2_hdr_is_reserved(line, nlen))
            continue;
        g_string_append(reqs, line);
        g_string_append(reqs, "\r\n");
    }
    if (c->body && c->body_len > 0)
        g_string_append_printf(reqs, "Content-Length: %zu\r\n", c->body_len);
    g_string_append(reqs, "Connection: close\r\n\r\n");

    gboolean ok = ns_h2_ssl_write_all(c, reqs->str, reqs->len);
    g_string_free(reqs, TRUE);
    if (ok && c->body && c->body_len > 0)
        ok = ns_h2_ssl_write_all(c, c->body, c->body_len);
    if (!ok)
        return FALSE;

    GByteArray *buf = g_byte_array_new();
    size_t pos = 0;
    char *line = NULL;
    size_t line_len = 0;
    gboolean status_parsed = FALSE;
    gboolean headers_complete = FALSE;
    gboolean chunked = FALSE;
    gboolean have_clen = FALSE;
    gint64 clen = 0;

    for (;;) {
        if (!ns_h2_read_line(c, buf, &pos, &line, &line_len)) {
            if (!status_parsed) { g_byte_array_free(buf, TRUE); return FALSE; }
            break;
        }
        if (line_len == 0) {
            headers_complete = TRUE;
            break;
        }
        if (!status_parsed) {
            char *sl = g_strndup(line, line_len);
            const char *sp = strchr(sl, ' ');
            if (sp) c->status = g_ascii_strtoll(sp + 1, NULL, 10);
            char *feed = g_strdup_printf("%s\r\n", sl);
            ns_header_sink_feed(c->hctx, feed, strlen(feed));
            c->status_line_fed = TRUE;
            g_free(feed);
            g_free(sl);
            status_parsed = TRUE;
        } else {
            const char *colon = memchr(line, ':', line_len);
            if (colon) {
                size_t nlen = (size_t)(colon - line);
                const char *val = colon + 1;
                size_t vlen = line_len - nlen - 1;
                while (vlen && (*val == ' ' || *val == '\t')) { val++; vlen--; }
                if (nlen == 16 &&
                    g_ascii_strncasecmp(line, "content-encoding", 16) == 0) {
                    if (vlen == 4 && g_ascii_strncasecmp(val, "gzip", 4) == 0)
                        c->encoding = NS_ENC_GZIP;
                    else if (vlen == 7 && g_ascii_strncasecmp(val, "deflate", 7) == 0)
                        c->encoding = NS_ENC_DEFLATE;
                    else if (vlen == 2 && g_ascii_strncasecmp(val, "br", 2) == 0)
                        c->encoding = NS_ENC_BROTLI;
                } else if (nlen == 17 &&
                    g_ascii_strncasecmp(line, "transfer-encoding", 17) == 0) {
                    if (vlen >= 7 && g_ascii_strncasecmp(val, "chunked", 7) == 0)
                        chunked = TRUE;
                } else if (nlen == 14 &&
                    g_ascii_strncasecmp(line, "content-length", 14) == 0) {
                    char *cl = g_strndup(val, vlen);
                    clen = g_ascii_strtoll(cl, NULL, 10);
                    have_clen = TRUE;
                    g_free(cl);
                } else if (nlen == 10 &&
                    g_ascii_strncasecmp(line, "set-cookie", 10) == 0) {
                    char *cv = g_strndup(val, vlen);
                    ns_net_store_set_cookie(c->url, cv);
                    g_free(cv);
                }
            }
            char *feed = g_strndup(line, line_len);
            char *feed2 = g_strdup_printf("%s\r\n", feed);
            ns_header_sink_feed(c->hctx, feed2, strlen(feed2));
            g_free(feed);
            g_free(feed2);
        }
    }

    c->out->t_starttransfer_ms = ns_h2_ms_since(c->start_us);

    GByteArray *raw = g_byte_array_new();
    if (headers_complete && pos < buf->len)
        g_byte_array_append(raw, buf->data + pos, buf->len - pos);
    g_byte_array_free(buf, TRUE);

    for (;;) {
        if (have_clen && !chunked && (gint64)raw->len >= clen)
            break;
        if (ns_h2_should_abort(c)) break;
        unsigned char tmp[65536];
        int n = ns_h2_ssl_read(c, tmp, sizeof tmp);
        if (n <= 0) break;
        g_byte_array_append(raw, tmp, (guint)n);
        if (raw->len > c->wctx->budget * 2) break;
    }

    GByteArray *body = g_byte_array_new();
    if (chunked) {
        size_t i = 0;
        while (i < raw->len) {
            size_t j = i;
            while (j + 1 < raw->len &&
                   !(raw->data[j] == '\r' && raw->data[j + 1] == '\n')) j++;
            if (j + 1 >= raw->len) break;
            char *sz = g_strndup((char *)raw->data + i, j - i);
            long chunk = strtol(sz, NULL, 16);
            g_free(sz);
            i = j + 2;
            if (chunk <= 0) break;
            if (i + (size_t)chunk > raw->len) chunk = (long)(raw->len - i);
            g_byte_array_append(body, raw->data + i, (guint)chunk);
            i += (size_t)chunk + 2;
        }
    } else if (have_clen) {
        guint take = (guint)((gint64)raw->len < clen ? raw->len : clen);
        g_byte_array_append(body, raw->data, take);
    } else {
        g_byte_array_append(body, raw->data, raw->len);
    }
    g_byte_array_free(raw, TRUE);

    if (body->len) {
        c->got_first_byte = TRUE;
        if (c->encoding == NS_ENC_IDENTITY) {
            if (!ns_body_sink_write(c->wctx, body->data, body->len))
                c->sink_full = TRUE;
        } else {
            c->comp_buf = body;
            body = NULL;
            ns_h2_flush_body(c);
        }
    }
    if (body)
        g_byte_array_free(body, TRUE);
    return status_parsed;
}

static gboolean
ns_h2_perform(const ns_hop_req *req, ns_write_ctx *wctx, ns_header_ctx *hctx,
              ns_hop_out *out, GCancellable *cancellable)
{
    g_autoptr(ns_url_parts) parts = ns_url_parts_new(req->url);
    if (!parts || !parts->hostname || !*parts->hostname) {
        out->error_message = g_strdup("invalid URL");
        out->connect_failed = TRUE;
        out->effective_url = g_strdup(req->url);
        return TRUE;
    }
    gboolean https = parts->protocol &&
                     g_ascii_strcasecmp(parts->protocol, "https:") == 0;
    int port = ns_h2_default_port(https);
    if (parts->port && *parts->port) {
        long p = g_ascii_strtoll(parts->port, NULL, 10);
        if (p > 0 && p < 65536) port = (int)p;
    }
    const char *host = parts->hostname;
    char *authority = (parts->port && *parts->port)
        ? g_strdup_printf("%s:%s", host, parts->port) : g_strdup(host);
    const char *pathname = (parts->pathname && *parts->pathname)
                           ? parts->pathname : "/";
    char *path = (parts->search && *parts->search)
        ? g_strconcat(pathname, parts->search, NULL) : g_strdup(pathname);

    ns_h2 c = {0};
    c.req = req;
    c.wctx = wctx;
    c.hctx = hctx;
    c.out = out;
    c.cancellable = cancellable;
    c.url = req->url;
    c.fd = -1;
    c.body = req->body;
    c.body_len = req->body_len;
    out->effective_url = g_strdup(req->url);
    c.start_us = g_get_monotonic_time();

    gint64 start = c.start_us;
    long connect_timeout = req->connect_timeout_s > 0 ? req->connect_timeout_s : 10;
    c.deadline_us = start + (gint64)(req->timeout_s > 0 ? req->timeout_s : 30)
                    * G_USEC_PER_SEC;
    gint64 connect_deadline = start + connect_timeout * G_USEC_PER_SEC;
    if (connect_deadline > c.deadline_us) connect_deadline = c.deadline_us;

    char *conn_err = NULL;
    gboolean timed_out = FALSE;
    c.fd = ns_h2_connect(host, port, connect_deadline, cancellable,
                         &out->remote_ip, &conn_err, &timed_out);
    out->t_namelookup_ms = ns_h2_ms_since(start);
    if (c.fd < 0) {
        if (ns_net_aborting() ||
            (cancellable && g_cancellable_is_cancelled(cancellable))) {
            out->cancelled = TRUE;
            g_free(conn_err);
            g_free(authority);
            g_free(path);
            return FALSE;
        }
        out->connect_failed = TRUE;
        out->error_message = conn_err ? conn_err
                                      : g_strdup("connection failed");
        g_free(authority);
        g_free(path);
        return TRUE;
    }
    g_free(conn_err);
    out->t_connect_ms = ns_h2_ms_since(start);
    ns_h2_apply_socket_timeout(c.fd);

    gboolean use_http2 = FALSE;
    SSL_CTX *ctx = NULL;
    if (https) {
        const ns_config *cfg = ns_config_get();
        gboolean verify = TRUE;
        gboolean tried_insecure = FALSE;
    retry_tls:
        ctx = ns_h2_ssl_ctx(verify);
        if (!ctx) {
            out->error_message = g_strdup("TLS context init failed");
            close(c.fd);
            g_free(authority);
            g_free(path);
            return TRUE;
        }
        c.ssl = SSL_new(ctx);
        SSL_set_fd(c.ssl, c.fd);
        SSL_set_tlsext_host_name(c.ssl, host);
        if (verify) {
            SSL_set_hostflags(c.ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            SSL_set1_host(c.ssl, host);
        }
        long vr = X509_V_OK;
        gboolean hs = ns_h2_handshake(c.ssl, &c, &vr);
        if (!hs) {
            if (ns_h2_should_abort(&c) &&
                (ns_net_aborting() ||
                 (cancellable && g_cancellable_is_cancelled(cancellable)))) {
                out->cancelled = TRUE;
                SSL_free(c.ssl); c.ssl = NULL;
                SSL_CTX_free(ctx);
                close(c.fd);
                g_free(authority); g_free(path);
                return FALSE;
            }
            gboolean is_verify_fail = (vr != X509_V_OK);
            if (is_verify_fail && !tried_insecure) {
                gboolean opt_in = cfg && cfg->tls_allow_insecure_override;
                char *fbhost = ns_url_host_from(req->url);
                gboolean pinned = ns_net_hsts_should_upgrade(fbhost);
                g_free(fbhost);
                if (opt_in && !pinned) {
                    out->tls_warning = g_strdup_printf(
                        "Insecure: TLS certificate not trusted (%s)",
                        X509_verify_cert_error_string(vr));
                    SSL_free(c.ssl); c.ssl = NULL;
                    SSL_CTX_free(ctx); ctx = NULL;
                    close(c.fd);
                    c.fd = ns_h2_connect(host, port, connect_deadline,
                                         cancellable, NULL, NULL, NULL);
                    if (c.fd < 0) {
                        out->connect_failed = TRUE;
                        out->error_message = g_strdup("reconnect failed");
                        g_free(authority); g_free(path);
                        return TRUE;
                    }
                    ns_h2_apply_socket_timeout(c.fd);
                    verify = FALSE;
                    tried_insecure = TRUE;
                    goto retry_tls;
                }
            }
            out->tls_verify_failed = is_verify_fail;
            out->error_message = is_verify_fail
                ? g_strdup_printf("TLS certificate problem: %s",
                                  X509_verify_cert_error_string(vr))
                : g_strdup("TLS handshake failed");
            SSL_free(c.ssl); c.ssl = NULL;
            SSL_CTX_free(ctx);
            close(c.fd);
            g_free(authority); g_free(path);
            return TRUE;
        }
        out->t_appconnect_ms = ns_h2_ms_since(start);
        const unsigned char *alpn = NULL;
        unsigned int alpnlen = 0;
        SSL_get0_alpn_selected(c.ssl, &alpn, &alpnlen);
        use_http2 = (alpnlen == 2 && memcmp(alpn, "h2", 2) == 0);
    }

    out->t_pretransfer_ms = ns_h2_ms_since(start);

    const char *scheme = https ? "https" : "http";
    gboolean ok;
    if (use_http2)
        ok = ns_h2_run_http2(&c, authority, path, scheme);
    else
        ok = ns_h2_run_http1(&c, authority, path);

    if (c.comp_buf)
        ns_h2_flush_body(&c);

    out->t_total_ms = ns_h2_ms_since(start);
    out->http_version = use_http2 ? CURL_HTTP_VERSION_2_0
                                  : CURL_HTTP_VERSION_1_1;
    out->num_connects = 1;

    if (ns_net_aborting() ||
        (cancellable && g_cancellable_is_cancelled(cancellable))) {
        out->cancelled = TRUE;
        if (c.ssl) { SSL_shutdown(c.ssl); SSL_free(c.ssl); }
        if (ctx) SSL_CTX_free(ctx);
        if (c.fd >= 0) close(c.fd);
        g_free(authority); g_free(path);
        return FALSE;
    }

    if (c.sink_full) {
        wctx->exceeded = TRUE;
        out->ok = FALSE;
    } else if (ok && c.status > 0) {
        out->status = c.status;
        out->ok = TRUE;
    } else {
        out->ok = FALSE;
        if (!out->error_message)
            out->error_message = g_strdup(use_http2
                ? "HTTP/2 transfer failed" : "HTTP transfer failed");
    }

    if (c.ssl) { SSL_shutdown(c.ssl); SSL_free(c.ssl); }
    if (ctx) SSL_CTX_free(ctx);
    if (c.fd >= 0) close(c.fd);
    g_free(authority);
    g_free(path);
    return TRUE;
}

#endif /* !_WIN32 */

gboolean
ns_hop_transport(const ns_hop_req *req, ns_write_ctx *wctx,
                 ns_header_ctx *hctx, ns_hop_out *out, GCancellable *cancellable)
{
#ifdef _WIN32
    return ns_hop_transport_curl(req, wctx, hctx, out, cancellable);
#else
    if ((req->proxy && *req->proxy) || req->request_ftp ||
        !ns_url_is_http_or_https(req->url))
        return ns_hop_transport_curl(req, wctx, hctx, out, cancellable);

    return ns_h2_perform(req, wctx, hctx, out, cancellable);
#endif
}
