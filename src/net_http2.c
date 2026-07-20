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
#ifdef NS_HTTP_HAVE_ZSTD
#include <zstd.h>
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
    NS_ENC_ZSTD,
} ns_content_encoding;

typedef struct ns_conn ns_conn;

typedef struct ns_h2 {
    const ns_hop_req *req;
    ns_write_ctx     *wctx;
    ns_header_ctx    *hctx;
    ns_hop_out       *out;
    GCancellable     *cancellable;
    gint64            start_us;
    gint64            deadline_us;

    SSL              *ssl;
    int               fd;
    ns_conn          *conn;

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

    char             *authority;
    char             *path;
    const char       *scheme;
    int32_t           stream_id;
    gboolean          submitted;
    gboolean          done;
    gboolean          stream_ok;
    gboolean          rst;
} ns_h2;

struct ns_conn {
    char            *origin;
    char            *host;
    char            *remote_ip;
    int              port;
    int              fd;
    SSL             *ssl;
    nghttp2_session *session;
    gboolean         is_http2;
    gboolean         goaway;
    gboolean         insecure;
    int              reuse_count;
    gint64           last_used_us;

    int              refs;
    GThread         *io_thread;
    GMutex           lock;
    GCond            cond;
    int              wake[2];
    GQueue          *pending;
    GHashTable      *streams;
    int              active;
    int              max_streams;
    gboolean         io_failed;
    gboolean         stopping;
};

#define NS_CONN_MAX_CONCURRENT 64

#define NS_CONN_MAX_REUSE 1000
#define NS_CONN_MAX_IDLE_US ((gint64)60 * G_USEC_PER_SEC)
#define NS_POOL_MAX_PER_ORIGIN 8

#ifndef _WIN32

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

static const char *
ns_h2_accept_encoding(void)
{
#if defined(NS_HTTP_HAVE_BROTLI) && defined(NS_HTTP_HAVE_ZSTD)
    return "gzip, deflate, br, zstd";
#elif defined(NS_HTTP_HAVE_BROTLI)
    return "gzip, deflate, br";
#elif defined(NS_HTTP_HAVE_ZSTD)
    return "gzip, deflate, zstd";
#else
    return "gzip, deflate";
#endif
}

static double
ns_h2_ms_since(gint64 start_us)
{
    return (double)(g_get_monotonic_time() - start_us) / 1000.0;
}

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

static GMutex   g_ctx_lock;
static SSL_CTX *g_ctx_cache[2];

static GMutex      g_sess_lock;
static GHashTable *g_sess_cache;

static void
ns_h2_sess_store(const char *host, int port, SSL *ssl)
{
    SSL_SESSION *sess = SSL_get1_session(ssl);
    if (!sess)
        return;
    char *key = g_strdup_printf("%s:%d", host, port);
    g_mutex_lock(&g_sess_lock);
    if (!g_sess_cache)
        g_sess_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify)SSL_SESSION_free);
    g_hash_table_replace(g_sess_cache, key, sess);
    g_mutex_unlock(&g_sess_lock);
}

static void
ns_h2_sess_apply(const char *host, int port, SSL *ssl)
{
    char *key = g_strdup_printf("%s:%d", host, port);
    g_mutex_lock(&g_sess_lock);
    SSL_SESSION *sess = g_sess_cache
        ? g_hash_table_lookup(g_sess_cache, key) : NULL;
    if (sess)
        SSL_set_session(ssl, sess);
    g_mutex_unlock(&g_sess_lock);
    g_free(key);
}

static SSL_CTX *
ns_h2_ssl_ctx(gboolean verify)
{
    g_mutex_lock(&g_ctx_lock);
    SSL_CTX **slot = &g_ctx_cache[verify ? 0 : 1];
    if (*slot) {
        SSL_CTX *cached = *slot;
        g_mutex_unlock(&g_ctx_lock);
        return cached;
    }
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        g_mutex_unlock(&g_ctx_lock);
        return NULL;
    }
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT |
                                        SSL_SESS_CACHE_NO_INTERNAL_STORE);
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                          SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                          SSL_MODE_AUTO_RETRY);
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
    *slot = ctx;
    g_mutex_unlock(&g_ctx_lock);
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

#ifdef NS_HTTP_HAVE_ZSTD
static gboolean
ns_h2_zstd(GByteArray *in, ns_write_ctx *wctx)
{
    ZSTD_DStream *ds = ZSTD_createDStream();
    if (!ds) return FALSE;
    ZSTD_initDStream(ds);
    unsigned char outbuf[16384];
    ZSTD_inBuffer input = { in->data, in->len, 0 };
    gboolean ok = TRUE;
    while (input.pos < input.size) {
        ZSTD_outBuffer output = { outbuf, sizeof outbuf, 0 };
        size_t r = ZSTD_decompressStream(ds, &output, &input);
        if (output.pos && !ns_body_sink_write(wctx, outbuf, output.pos)) {
            ok = FALSE;
            break;
        }
        if (ZSTD_isError(r)) { ok = FALSE; break; }
        if (r == 0 && input.pos >= input.size)
            break;
        if (output.pos == 0 && input.pos >= input.size)
            break;
    }
    ZSTD_freeDStream(ds);
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
    case NS_ENC_ZSTD:
#ifdef NS_HTTP_HAVE_ZSTD
        ok = ns_h2_zstd(c->comp_buf, c->wctx);
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
        else if (valuelen == 4 && g_ascii_strncasecmp(value, "zstd", 4) == 0)
            c->encoding = NS_ENC_ZSTD;
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
    ns_conn *conn = user_data;
    int r = SSL_write(conn->ssl, data, (int)length);
    if (r > 0)
        return (ssize_t)r;
    int err = SSL_get_error(conn->ssl, r);
    if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ)
        return NGHTTP2_ERR_WOULDBLOCK;
    conn->io_failed = TRUE;
    return NGHTTP2_ERR_CALLBACK_FAILURE;
}

static int
ns_h2_frame_recv_cb(nghttp2_session *session, const nghttp2_frame *frame,
                    void *user_data)
{
    (void)session;
    ns_conn *conn = user_data;
    if (frame->hd.type == NGHTTP2_GOAWAY)
        conn->goaway = TRUE;
    return 0;
}

static int
ns_h2_header_cb(nghttp2_session *session, const nghttp2_frame *frame,
                const uint8_t *name, size_t namelen,
                const uint8_t *value, size_t valuelen,
                uint8_t flags, void *user_data)
{
    (void)user_data; (void)flags;
    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_RESPONSE)
        return 0;
    ns_h2 *c = nghttp2_session_get_stream_user_data(session,
                                                    frame->hd.stream_id);
    if (c)
        ns_h2_on_response_header(c, (const char *)name, namelen,
                                 (const char *)value, valuelen);
    return 0;
}

static int
ns_h2_data_chunk_cb(nghttp2_session *session, uint8_t flags,
                    int32_t stream_id, const uint8_t *data, size_t len,
                    void *user_data)
{
    (void)flags; (void)user_data;
    ns_h2 *c = nghttp2_session_get_stream_user_data(session, stream_id);
    if (c)
        ns_h2_on_body(c, data, len);
    return 0;
}

static int
ns_h2_stream_close_cb(nghttp2_session *session, int32_t stream_id,
                      uint32_t error_code, void *user_data)
{
    ns_conn *conn = user_data;
    ns_h2 *c = nghttp2_session_get_stream_user_data(session, stream_id);
    if (c) {
        c->stream_closed = TRUE;
        c->stream_ok = (error_code == NGHTTP2_NO_ERROR);
        c->done = TRUE;
        g_hash_table_remove(conn->streams, GINT_TO_POINTER(stream_id));
        if (conn->active > 0)
            conn->active--;
    }
    return 0;
}

static ssize_t
ns_h2_req_body_cb(nghttp2_session *session, int32_t stream_id, uint8_t *buf,
                  size_t length, uint32_t *data_flags,
                  nghttp2_data_source *source, void *user_data)
{
    (void)source; (void)user_data;
    ns_h2 *c = nghttp2_session_get_stream_user_data(session, stream_id);
    if (!c) { *data_flags |= NGHTTP2_DATA_FLAG_EOF; return 0; }
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
ns_h2_conn_make_session(ns_conn *conn)
{
    nghttp2_session_callbacks *cbs = NULL;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_send_callback(cbs, ns_h2_send_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs,
                                                         ns_h2_frame_recv_cb);
    nghttp2_session_callbacks_set_on_header_callback(cbs, ns_h2_header_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        cbs, ns_h2_data_chunk_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(
        cbs, ns_h2_stream_close_cb);

    nghttp2_session_client_new(&conn->session, cbs, conn);
    nghttp2_session_callbacks_del(cbs);
    if (!conn->session)
        return FALSE;

    nghttp2_settings_entry iv[] = {
        { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100 },
        { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 8 * 1024 * 1024 },
        { NGHTTP2_SETTINGS_ENABLE_PUSH, 0 },
    };
    nghttp2_submit_settings(conn->session, NGHTTP2_FLAG_NONE, iv, 3);
    return TRUE;
}

static gboolean
ns_h2_submit_locked(ns_conn *conn, ns_h2 *c)
{
    GPtrArray *owned = g_ptr_array_new_with_free_func(g_free);
    GArray *nva = g_array_new(FALSE, FALSE, sizeof(nghttp2_nv));
    const char *method = (c->req->method && *c->req->method) ? c->req->method
                                                             : "GET";
    ns_h2_add_nv(nva, ":method", method);
    ns_h2_add_nv(nva, ":scheme", c->scheme);
    ns_h2_add_nv(nva, ":authority", c->authority);
    ns_h2_add_nv(nva, ":path", c->path);
    if (c->req->user_agent)
        ns_h2_add_nv(nva, "user-agent", c->req->user_agent);
    ns_h2_add_nv(nva, "accept-encoding", ns_h2_accept_encoding());
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

    int32_t sid = nghttp2_submit_request(conn->session, NULL,
                                         (nghttp2_nv *)nva->data, nva->len,
                                         provp, c);
    g_array_free(nva, TRUE);
    g_ptr_array_free(owned, TRUE);
    if (sid < 0)
        return FALSE;
    c->stream_id = sid;
    c->submitted = TRUE;
    g_hash_table_insert(conn->streams, GINT_TO_POINTER(sid), c);
    conn->active++;
    return TRUE;
}

static void
ns_h2_io_fail_all(ns_conn *conn)
{
    for (ns_h2 *c; (c = g_queue_pop_head(conn->pending)); ) {
        c->stream_ok = FALSE;
        c->done = TRUE;
    }
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, conn->streams);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_h2 *c = val;
        c->stream_ok = FALSE;
        c->done = TRUE;
    }
    g_hash_table_remove_all(conn->streams);
    conn->active = 0;
    g_cond_broadcast(&conn->cond);
}

static void
ns_h2_io_scan_timeouts(ns_conn *conn)
{
    if (conn->active == 0)
        return;
    GPtrArray *dead = NULL;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, conn->streams);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_h2 *c = val;
        if (ns_h2_should_abort(c)) {
            if (!dead) dead = g_ptr_array_new();
            g_ptr_array_add(dead, c);
        }
    }
    if (!dead)
        return;
    for (guint i = 0; i < dead->len; i++) {
        ns_h2 *c = g_ptr_array_index(dead, i);
        nghttp2_submit_rst_stream(conn->session, NGHTTP2_FLAG_NONE,
                                  c->stream_id, NGHTTP2_CANCEL);
        c->rst = TRUE;
        c->stream_ok = FALSE;
        c->done = TRUE;
        g_hash_table_remove(conn->streams, GINT_TO_POINTER(c->stream_id));
        if (conn->active > 0) conn->active--;
    }
    g_ptr_array_free(dead, TRUE);
    g_cond_broadcast(&conn->cond);
}

static gpointer
ns_h2_io_thread(gpointer data)
{
    ns_conn *conn = data;
    unsigned char rbuf[65536];
    for (;;) {
        g_mutex_lock(&conn->lock);
        for (ns_h2 *c; (c = g_queue_pop_head(conn->pending)); ) {
            if (!ns_h2_submit_locked(conn, c)) {
                conn->goaway = TRUE;
                c->stream_ok = FALSE;
                c->done = TRUE;
                g_cond_broadcast(&conn->cond);
            }
        }
        if (nghttp2_session_send(conn->session) != 0)
            conn->io_failed = TRUE;
        ns_h2_io_scan_timeouts(conn);
        gboolean stop = conn->io_failed ||
                        (conn->stopping && conn->active == 0 &&
                         g_queue_is_empty(conn->pending)) ||
                        (conn->goaway && conn->active == 0);
        int want_write = nghttp2_session_want_write(conn->session);
        g_mutex_unlock(&conn->lock);
        if (stop)
            break;

        struct pollfd pfd[2];
        pfd[0].fd = conn->wake[0];
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = conn->fd;
        pfd[1].events = POLLIN | (want_write ? POLLOUT : 0);
        pfd[1].revents = 0;
        int pr = poll(pfd, 2, 250);
        if (pr < 0 && errno != EINTR) {
            g_mutex_lock(&conn->lock);
            conn->io_failed = TRUE;
            g_mutex_unlock(&conn->lock);
            continue;
        }
        if (pfd[0].revents & POLLIN) {
            char drain[256];
            while (read(conn->wake[0], drain, sizeof drain) > 0)
                ;
        }
        if (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            for (;;) {
                int n = SSL_read(conn->ssl, rbuf, sizeof rbuf);
                if (n > 0) {
                    g_mutex_lock(&conn->lock);
                    ssize_t rv = nghttp2_session_mem_recv(conn->session, rbuf,
                                                          (size_t)n);
                    if (rv < 0)
                        conn->io_failed = TRUE;
                    g_cond_broadcast(&conn->cond);
                    g_mutex_unlock(&conn->lock);
                    if (rv < 0)
                        break;
                    continue;
                }
                if (n == 0) {
                    g_mutex_lock(&conn->lock);
                    conn->io_failed = TRUE;
                    g_mutex_unlock(&conn->lock);
                    break;
                }
                int err = SSL_get_error(conn->ssl, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                    break;
                g_mutex_lock(&conn->lock);
                conn->io_failed = TRUE;
                g_mutex_unlock(&conn->lock);
                break;
            }
        }
    }
    g_mutex_lock(&conn->lock);
    ns_h2_io_fail_all(conn);
    g_mutex_unlock(&conn->lock);
    return NULL;
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
    g_string_append_printf(reqs, "Accept-Encoding: %s\r\n",
                           ns_h2_accept_encoding());
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
                    else if (vlen == 4 && g_ascii_strncasecmp(val, "zstd", 4) == 0)
                        c->encoding = NS_ENC_ZSTD;
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

typedef struct {
    GQueue  *conns;
    gboolean connecting;
} ns_pool_entry;

static GMutex      g_pool_lock;
static GCond       g_pool_cond;
static GHashTable *g_pool;

static void ns_h2_pool_entry_free(gpointer p);

static ns_pool_entry *
ns_h2_pool_entry(const char *origin)
{
    if (!g_pool)
        g_pool = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       ns_h2_pool_entry_free);
    ns_pool_entry *e = g_hash_table_lookup(g_pool, origin);
    if (!e) {
        e = g_new0(ns_pool_entry, 1);
        e->conns = g_queue_new();
        g_hash_table_insert(g_pool, g_strdup(origin), e);
    }
    return e;
}

static void
ns_h2_conn_destroy(ns_conn *conn)
{
    if (!conn)
        return;
    if (conn->io_thread) {
        g_mutex_lock(&conn->lock);
        conn->stopping = TRUE;
        g_mutex_unlock(&conn->lock);
        if (conn->wake[1] >= 0) {
            ssize_t wr = write(conn->wake[1], "x", 1);
            (void)wr;
        }
        g_thread_join(conn->io_thread);
    }
    if (conn->ssl) {
        if (!conn->insecure)
            ns_h2_sess_store(conn->host, conn->port, conn->ssl);
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
    }
    if (conn->session)
        nghttp2_session_del(conn->session);
    if (conn->fd >= 0)
        close(conn->fd);
    if (conn->wake[0] >= 0) close(conn->wake[0]);
    if (conn->wake[1] >= 0) close(conn->wake[1]);
    if (conn->pending) g_queue_free(conn->pending);
    if (conn->streams) g_hash_table_destroy(conn->streams);
    if (conn->io_thread) {
        g_mutex_clear(&conn->lock);
        g_cond_clear(&conn->cond);
    }
    g_free(conn->origin);
    g_free(conn->host);
    g_free(conn->remote_ip);
    g_free(conn);
}

static void
ns_h2_conn_unref(ns_conn *conn)
{
    if (conn && g_atomic_int_dec_and_test(&conn->refs))
        ns_h2_conn_destroy(conn);
}

static ns_conn *
ns_h2_entry_find(ns_pool_entry *e, ns_h2 *c)
{
    GList *l = e->conns->head;
    while (l) {
        ns_conn *conn = l->data;
        GList *next = l->next;
        g_mutex_lock(&conn->lock);
        gboolean dead = conn->io_failed || conn->stopping || conn->goaway;
        gboolean full = conn->active + (int)g_queue_get_length(conn->pending)
                        >= NS_CONN_MAX_CONCURRENT;
        if (dead) {
            g_queue_delete_link(e->conns, l);
            g_mutex_unlock(&conn->lock);
            ns_h2_conn_unref(conn);
        } else if (!full) {
            g_queue_push_tail(conn->pending, c);
            if (conn->wake[1] >= 0) {
                ssize_t wr = write(conn->wake[1], "x", 1);
                (void)wr;
            }
            conn->last_used_us = g_get_monotonic_time();
            conn->reuse_count++;
            g_atomic_int_inc(&conn->refs);
            g_mutex_unlock(&conn->lock);
            return conn;
        } else {
            g_mutex_unlock(&conn->lock);
        }
        l = next;
    }
    return NULL;
}

static ns_conn *
ns_h2_pool_attach(const char *origin, ns_h2 *c, gboolean *become_connector)
{
    *become_connector = FALSE;
    g_mutex_lock(&g_pool_lock);
    for (;;) {
        ns_pool_entry *e = ns_h2_pool_entry(origin);
        ns_conn *conn = ns_h2_entry_find(e, c);
        if (conn) {
            g_mutex_unlock(&g_pool_lock);
            return conn;
        }
        if (!e->connecting) {
            e->connecting = TRUE;
            *become_connector = TRUE;
            g_mutex_unlock(&g_pool_lock);
            return NULL;
        }
        gint64 slice = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
        if (!g_cond_wait_until(&g_pool_cond, &g_pool_lock, slice) &&
            ns_h2_should_abort(c)) {
            g_mutex_unlock(&g_pool_lock);
            return NULL;
        }
    }
}

static void
ns_h2_pool_connect_done(const char *origin, ns_conn *conn)
{
    g_mutex_lock(&g_pool_lock);
    ns_pool_entry *e = ns_h2_pool_entry(origin);
    e->connecting = FALSE;
    if (conn) {
        g_atomic_int_inc(&conn->refs);
        g_queue_push_head(e->conns, conn);
    }
    g_cond_broadcast(&g_pool_cond);
    g_mutex_unlock(&g_pool_lock);
}

static void
ns_h2_pool_entry_free(gpointer p)
{
    ns_pool_entry *e = p;
    for (ns_conn *conn; (conn = g_queue_pop_head(e->conns)); )
        ns_h2_conn_unref(conn);
    g_queue_free(e->conns);
    g_free(e);
}

void
ns_net_backend_shutdown(void)
{
    g_mutex_lock(&g_pool_lock);
    if (g_pool) {
        g_hash_table_destroy(g_pool);
        g_pool = NULL;
    }
    g_mutex_unlock(&g_pool_lock);
    g_mutex_lock(&g_sess_lock);
    if (g_sess_cache) {
        g_hash_table_destroy(g_sess_cache);
        g_sess_cache = NULL;
    }
    g_mutex_unlock(&g_sess_lock);
}

static ns_conn *
ns_h2_conn_open(const char *origin, const char *host, int port, gboolean https,
                const ns_hop_req *req, ns_h2 *c, ns_hop_out *out,
                gint64 start, gint64 connect_deadline,
                GCancellable *cancellable)
{
    char *conn_err = NULL;
    char *remote_ip = NULL;
    int fd = ns_h2_connect(host, port, connect_deadline, cancellable,
                           &remote_ip, &conn_err, NULL);
    out->t_namelookup_ms = ns_h2_ms_since(start);
    if (fd < 0) {
        if (ns_net_aborting() ||
            (cancellable && g_cancellable_is_cancelled(cancellable)))
            out->cancelled = TRUE;
        else {
            out->connect_failed = TRUE;
            out->error_message = conn_err ? conn_err
                                          : g_strdup("connection failed");
            conn_err = NULL;
        }
        g_free(conn_err);
        g_free(remote_ip);
        return NULL;
    }
    g_free(conn_err);
    out->t_connect_ms = ns_h2_ms_since(start);
    ns_h2_apply_socket_timeout(fd);
    g_free(out->remote_ip);
    out->remote_ip = remote_ip ? g_strdup(remote_ip) : NULL;

    SSL *ssl = NULL;
    gboolean use_http2 = FALSE;
    gboolean insecure = FALSE;
    if (https) {
        const ns_config *cfg = ns_config_get();
        gboolean verify = TRUE;
        gboolean tried_insecure = FALSE;
        c->fd = fd;
    retry_tls:
        {
            SSL_CTX *ctx = ns_h2_ssl_ctx(verify);
            if (!ctx) {
                out->error_message = g_strdup("TLS context init failed");
                close(fd); g_free(remote_ip);
                return NULL;
            }
            ssl = SSL_new(ctx);
            SSL_set_fd(ssl, fd);
            SSL_set_tlsext_host_name(ssl, host);
            ns_h2_sess_apply(host, port, ssl);
            if (verify) {
                SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
                SSL_set1_host(ssl, host);
            }
            c->ssl = ssl;
            long vr = X509_V_OK;
            gboolean hs = ns_h2_handshake(ssl, c, &vr);
            if (!hs) {
                if (ns_net_aborting() ||
                    (cancellable && g_cancellable_is_cancelled(cancellable))) {
                    out->cancelled = TRUE;
                    SSL_free(ssl); close(fd); g_free(remote_ip);
                    return NULL;
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
                        SSL_free(ssl); ssl = NULL;
                        close(fd);
                        fd = ns_h2_connect(host, port, connect_deadline,
                                           cancellable, NULL, NULL, NULL);
                        if (fd < 0) {
                            out->connect_failed = TRUE;
                            out->error_message = g_strdup("reconnect failed");
                            g_free(remote_ip);
                            return NULL;
                        }
                        ns_h2_apply_socket_timeout(fd);
                        c->fd = fd;
                        verify = FALSE;
                        tried_insecure = TRUE;
                        insecure = TRUE;
                        goto retry_tls;
                    }
                }
                out->tls_verify_failed = is_verify_fail;
                out->error_message = is_verify_fail
                    ? g_strdup_printf("TLS certificate problem: %s",
                                      X509_verify_cert_error_string(vr))
                    : g_strdup("TLS handshake failed");
                SSL_free(ssl); close(fd); g_free(remote_ip);
                return NULL;
            }
        }
        out->t_appconnect_ms = ns_h2_ms_since(start);
        const unsigned char *alpn = NULL;
        unsigned int alpnlen = 0;
        SSL_get0_alpn_selected(ssl, &alpn, &alpnlen);
        use_http2 = (alpnlen == 2 && memcmp(alpn, "h2", 2) == 0);
    }

    ns_conn *conn = g_new0(ns_conn, 1);
    conn->origin = g_strdup(origin);
    conn->host = g_strdup(host);
    conn->remote_ip = remote_ip;
    conn->port = port;
    conn->fd = fd;
    conn->ssl = ssl;
    conn->is_http2 = use_http2;
    conn->insecure = insecure;
    conn->refs = 1;
    conn->wake[0] = conn->wake[1] = -1;
    conn->last_used_us = g_get_monotonic_time();
    if (use_http2) {
        g_mutex_init(&conn->lock);
        g_cond_init(&conn->cond);
        conn->pending = g_queue_new();
        conn->streams = g_hash_table_new(g_direct_hash, g_direct_equal);
        conn->max_streams = NS_CONN_MAX_CONCURRENT;
        if (pipe(conn->wake) != 0) {
            conn->wake[0] = conn->wake[1] = -1;
            ns_h2_conn_destroy(conn);
            out->error_message = g_strdup("pipe() failed");
            return NULL;
        }
        fcntl(conn->wake[0], F_SETFL, O_NONBLOCK);
        fcntl(conn->wake[1], F_SETFL, O_NONBLOCK);
        int cfl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, cfl | O_NONBLOCK);
        if (!ns_h2_conn_make_session(conn)) {
            ns_h2_conn_destroy(conn);
            out->error_message = g_strdup("HTTP/2 session init failed");
            return NULL;
        }
        conn->io_thread = g_thread_new("ns-h2-io", ns_h2_io_thread, conn);
    }
    return conn;
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
    const char *scheme = https ? "https" : "http";
    char *origin = g_strdup_printf("%s://%s:%d", scheme, host, port);
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
    c.authority = authority;
    c.path = path;
    c.scheme = scheme;
    out->effective_url = g_strdup(req->url);
    c.start_us = g_get_monotonic_time();

    gint64 start = c.start_us;
    long connect_timeout = req->connect_timeout_s > 0 ? req->connect_timeout_s : 10;
    c.deadline_us = start + (gint64)(req->timeout_s > 0 ? req->timeout_s : 30)
                    * G_USEC_PER_SEC;
    gint64 connect_deadline = start + connect_timeout * G_USEC_PER_SEC;
    if (connect_deadline > c.deadline_us) connect_deadline = c.deadline_us;

    gboolean ok = FALSE;
    gboolean reused = FALSE;
    gboolean via_h2 = FALSE;
    ns_conn *conn = NULL;

    for (int attempt = 0; attempt < 2; attempt++) {
        gboolean connector = FALSE;
        conn = ns_h2_pool_attach(origin, &c, &connector);
        if (conn) {
            reused = TRUE;
            via_h2 = TRUE;
            c.conn = conn;
            if (out->remote_ip == NULL && conn->remote_ip)
                out->remote_ip = g_strdup(conn->remote_ip);
        } else if (connector) {
            conn = ns_h2_conn_open(origin, host, port, https, req, &c, out,
                                   start, connect_deadline, cancellable);
            if (!conn) {
                ns_h2_pool_connect_done(origin, NULL);
                g_free(origin); g_free(authority); g_free(path);
                return out->cancelled ? FALSE : TRUE;
            }
            c.conn = conn;
            out->t_pretransfer_ms = ns_h2_ms_since(start);
            if (!conn->is_http2) {
                ns_h2_pool_connect_done(origin, NULL);
                c.ssl = conn->ssl;
                c.fd = conn->fd;
                ok = ns_h2_run_http1(&c, authority, path);
                break;
            }
            via_h2 = TRUE;
            g_mutex_lock(&conn->lock);
            g_queue_push_tail(conn->pending, &c);
            if (conn->wake[1] >= 0) {
                ssize_t wr = write(conn->wake[1], "x", 1);
                (void)wr;
            }
            g_mutex_unlock(&conn->lock);
            ns_h2_pool_connect_done(origin, conn);
        } else {
            gboolean cx = ns_net_aborting() ||
                (cancellable && g_cancellable_is_cancelled(cancellable));
            out->cancelled = cx;
            if (!cx)
                out->error_message = g_strdup("request timed out");
            g_free(origin); g_free(authority); g_free(path);
            return cx ? FALSE : TRUE;
        }

        g_mutex_lock(&conn->lock);
        while (!c.done) {
            gint64 slice = g_get_monotonic_time() + 200 * G_TIME_SPAN_MILLISECOND;
            g_cond_wait_until(&conn->cond, &conn->lock, slice);
        }
        g_mutex_unlock(&conn->lock);
        ok = c.stream_ok;

        if (reused && !c.submitted && !c.got_first_byte && attempt == 0) {
            ns_h2_conn_unref(conn);
            conn = NULL;
            reused = FALSE;
            via_h2 = FALSE;
            c.done = c.stream_ok = c.submitted = FALSE;
            c.stream_closed = c.rst = c.got_first_byte = c.status_line_fed = FALSE;
            c.status = 0;
            c.encoding = NS_ENC_IDENTITY;
            c.body_off = 0;
            if (c.comp_buf) { g_byte_array_free(c.comp_buf, TRUE); c.comp_buf = NULL; }
            continue;
        }
        break;
    }

    if (c.comp_buf)
        ns_h2_flush_body(&c);

    out->t_total_ms = ns_h2_ms_since(start);
    out->http_version = via_h2 ? CURL_HTTP_VERSION_2_0 : CURL_HTTP_VERSION_1_1;
    out->num_connects = reused ? 0 : 1;

    gboolean cancelled = ns_net_aborting() ||
        (cancellable && g_cancellable_is_cancelled(cancellable));
    if (cancelled) {
        out->cancelled = TRUE;
        ns_h2_conn_unref(conn);
        g_free(origin); g_free(authority); g_free(path);
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
            out->error_message = g_strdup(c.rst ? "request timed out"
                : (via_h2 ? "HTTP/2 transfer failed" : "HTTP transfer failed"));
    }

    ns_h2_conn_unref(conn);
    g_free(origin);
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
