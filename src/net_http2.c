/* Nordstjernen — HTTP transport over libnghttp2 + OpenSSL (curl alternative). */

#include "net.h"
#include "net_backend.h"
#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#endif

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

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef gintptr ns_socket;

#ifdef _WIN32
typedef WSAPOLLFD ns_pollfd;
#define NS_NATIVE_SOCKET(fd) ((SOCKET)(fd))
#else
typedef struct pollfd ns_pollfd;
#define NS_NATIVE_SOCKET(fd) ((int)(fd))
#endif

static gboolean
ns_h2_socket_init(void)
{
#ifdef _WIN32
    static gsize state;
    if (g_once_init_enter(&state)) {
        WSADATA data;
        gsize ready = WSAStartup(MAKEWORD(2, 2), &data) == 0 ? 1 : 2;
        g_once_init_leave(&state, ready);
    }
    return state == 1;
#else
    return TRUE;
#endif
}

static int
ns_h2_socket_error(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static gboolean
ns_h2_connect_pending(int error)
{
#ifdef _WIN32
    return error == WSAEINPROGRESS || error == WSAEWOULDBLOCK ||
           error == WSAEALREADY;
#else
    return error == EINPROGRESS;
#endif
}

static gboolean
ns_h2_interrupted(int error)
{
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

static void
ns_h2_socket_close(ns_socket fd)
{
    if (fd < 0) return;
#ifdef _WIN32
    closesocket((SOCKET)fd);
#else
    close((int)fd);
#endif
}

static gboolean
ns_h2_set_nonblocking(ns_socket fd, gboolean on)
{
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    return ioctlsocket((SOCKET)fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl((int)fd, F_GETFL, 0);
    if (flags < 0) return FALSE;
    if (on) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    return fcntl((int)fd, F_SETFL, flags) == 0;
#endif
}

static int
ns_h2_socket_poll(ns_pollfd *fds, unsigned long count, int timeout_ms)
{
#ifdef _WIN32
    return WSAPoll(fds, count, timeout_ms);
#else
    return poll(fds, (nfds_t)count, timeout_ms);
#endif
}

static gssize
ns_h2_socket_recv(ns_socket fd, void *buffer, size_t length, int flags)
{
#ifdef _WIN32
    int amount = length > G_MAXINT ? G_MAXINT : (int)length;
    return recv((SOCKET)fd, (char *)buffer, amount, flags);
#else
    return recv((int)fd, buffer, length, flags);
#endif
}

static gssize
ns_h2_socket_send(ns_socket fd, const void *buffer, size_t length, int flags)
{
#ifdef _WIN32
    int amount = length > G_MAXINT ? G_MAXINT : (int)length;
    return send((SOCKET)fd, (const char *)buffer, amount, flags);
#else
    return send((int)fd, buffer, length, flags);
#endif
}

static void
ns_h2_set_nodelay(ns_socket fd)
{
    int one = 1;
#ifdef _WIN32
    setsockopt((SOCKET)fd, IPPROTO_TCP, TCP_NODELAY,
               (const char *)&one, sizeof one);
#else
    setsockopt((int)fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#endif
}

static gboolean
ns_h2_get_socket_error(ns_socket fd, int *error)
{
#ifdef _WIN32
    int len = sizeof *error;
    return getsockopt((SOCKET)fd, SOL_SOCKET, SO_ERROR, (char *)error,
                      &len) == 0;
#else
    socklen_t len = sizeof *error;
    return getsockopt((int)fd, SOL_SOCKET, SO_ERROR, error, &len) == 0;
#endif
}

static gboolean
ns_h2_wake_pair(ns_socket wake[2])
{
#ifdef _WIN32
    SOCKET reader = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (reader == INVALID_SOCKET) return FALSE;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(reader, (const struct sockaddr *)&address, sizeof address) != 0) {
        closesocket(reader);
        return FALSE;
    }
    int address_len = sizeof address;
    if (getsockname(reader, (struct sockaddr *)&address, &address_len) != 0) {
        closesocket(reader);
        return FALSE;
    }
    SOCKET writer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (writer == INVALID_SOCKET ||
        connect(writer, (const struct sockaddr *)&address,
                sizeof address) != 0) {
        if (writer != INVALID_SOCKET) closesocket(writer);
        closesocket(reader);
        return FALSE;
    }
    wake[0] = (ns_socket)reader;
    wake[1] = (ns_socket)writer;
#else
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) return FALSE;
    wake[0] = pipe_fd[0];
    wake[1] = pipe_fd[1];
#endif
    if (!ns_h2_set_nonblocking(wake[0], TRUE) ||
        !ns_h2_set_nonblocking(wake[1], TRUE)) {
        ns_h2_socket_close(wake[0]);
        ns_h2_socket_close(wake[1]);
        wake[0] = wake[1] = -1;
        return FALSE;
    }
    return TRUE;
}

static void
ns_h2_wake_signal(ns_socket fd)
{
    if (fd < 0) return;
#ifdef _WIN32
    send((SOCKET)fd, "x", 1, 0);
#else
    ssize_t written = write((int)fd, "x", 1);
    (void)written;
#endif
}

static void
ns_h2_wake_drain(ns_socket fd)
{
    char buffer[256];
#ifdef _WIN32
    while (recv((SOCKET)fd, buffer, sizeof buffer, 0) > 0)
        ;
#else
    while (read((int)fd, buffer, sizeof buffer) > 0)
        ;
#endif
}

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
    ns_socket         fd;
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
    const char       *proto;
} ns_h2;

struct ns_conn {
    char            *origin;
    char            *host;
    char            *remote_ip;
    int              port;
    ns_socket        fd;
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
    ns_socket        wake[2];
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

static void ns_h3_altsvc_note(const char *url, const char *value, size_t vlen);
#ifdef NS_HTTP_HAVE_HTTP3
static gboolean ns_h3_should_try(const char *origin);
static gboolean ns_h3_perform(const ns_hop_req *req, ns_write_ctx *wctx,
                              ns_header_ctx *hctx, ns_hop_out *out,
                              GCancellable *cancellable, const char *host,
                              int port, const char *authority,
                              const char *path);
#endif

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

static ns_socket
ns_h2_connect(const char *host, int port, gint64 deadline_us,
              GCancellable *cancellable, char **remote_ip_out,
              char **err_out, gboolean *timed_out)
{
    if (!ns_h2_socket_init()) {
        if (err_out) *err_out = g_strdup("socket initialization failed");
        return -1;
    }
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

    ns_socket fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ns_net_aborting() ||
            (cancellable && g_cancellable_is_cancelled(cancellable)))
            break;
        ns_socket s = (ns_socket)socket(ai->ai_family, ai->ai_socktype,
                                       ai->ai_protocol);
        if (s < 0) continue;
        if (!ns_h2_set_nonblocking(s, TRUE)) {
            ns_h2_socket_close(s);
            continue;
        }
        ns_h2_set_nodelay(s);

        int rc = connect(NS_NATIVE_SOCKET(s), ai->ai_addr,
                         (int)ai->ai_addrlen);
        if (rc == 0) {
            fd = s;
        } else if (ns_h2_connect_pending(ns_h2_socket_error())) {
            gboolean connected = FALSE;
            for (;;) {
                if (ns_net_aborting() ||
                    (cancellable && g_cancellable_is_cancelled(cancellable)))
                    break;
                gint64 remain = deadline_us - g_get_monotonic_time();
                if (remain <= 0) { if (timed_out) *timed_out = TRUE; break; }
                int slice = (int)(remain / 1000);
                if (slice > 500) slice = 500;
                ns_pollfd pfd = { .fd = s, .events = POLLOUT };
                int pr = ns_h2_socket_poll(&pfd, 1, slice);
                if (pr < 0) {
                    if (ns_h2_interrupted(ns_h2_socket_error())) continue;
                    break;
                }
                if (pr == 0) continue;
                int soerr = 0;
                if (!ns_h2_get_socket_error(s, &soerr))
                    break;
                if (soerr == 0) { connected = TRUE; break; }
                break;
            }
            if (connected) fd = s;
        }

        if (fd == s) {
            ns_h2_set_nonblocking(s, FALSE);
            if (remote_ip_out && !*remote_ip_out) {
                char ipbuf[INET6_ADDRSTRLEN] = {0};
                if (ai->ai_family == AF_INET &&
                    (size_t)ai->ai_addrlen >= sizeof(struct sockaddr_in)) {
                    struct sockaddr_in ipv4;
                    memcpy(&ipv4, ai->ai_addr, sizeof ipv4);
                    if (inet_ntop(AF_INET, &ipv4.sin_addr, ipbuf, sizeof ipbuf))
                        *remote_ip_out = g_strdup(ipbuf);
                } else if (ai->ai_family == AF_INET6 &&
                           (size_t)ai->ai_addrlen >= sizeof(struct sockaddr_in6)) {
                    struct sockaddr_in6 ipv6;
                    memcpy(&ipv6, ai->ai_addr, sizeof ipv6);
                    if (inet_ntop(AF_INET6, &ipv6.sin6_addr, ipbuf, sizeof ipbuf))
                        *remote_ip_out = g_strdup(ipbuf);
                }
            }
            break;
        }
        ns_h2_socket_close(s);
    }
    freeaddrinfo(res);
    if (fd < 0 && err_out && !*err_out)
        *err_out = g_strdup_printf("could not connect to %s:%d", host, port);
    return fd;
}

static void
ns_h2_apply_socket_timeout(ns_socket fd)
{
#ifdef _WIN32
    DWORD timeout_ms = 1000;
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO,
               (const char *)&timeout_ms, sizeof timeout_ms);
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_SNDTIMEO,
               (const char *)&timeout_ms, sizeof timeout_ms);
#else
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt((int)fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
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
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINTR || e == WSAEINPROGRESS;
#else
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    if (e == EWOULDBLOCK) return TRUE;
#endif
    return e == EAGAIN || e == EINTR;
#endif
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
            ns_h2_retryable(ns_h2_socket_error())) {
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
            gssize r = ns_h2_socket_recv(c->fd, buf, (size_t)len, 0);
            if (r > 0) return (int)r;
            if (r == 0) return 0;
            if (ns_h2_retryable(ns_h2_socket_error())) {
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
            ns_h2_retryable(ns_h2_socket_error())) {
            if (ns_h2_should_abort(c)) return -1;
            continue;
        }
        if (err == SSL_ERROR_SYSCALL && ns_h2_socket_error() == 0) return 0;
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
            gssize r = ns_h2_socket_send(c->fd, p + off, len - off,
                                         MSG_NOSIGNAL);
            if (r > 0) { off += (size_t)r; continue; }
            if (r < 0 && ns_h2_retryable(ns_h2_socket_error())) {
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
             ns_h2_retryable(ns_h2_socket_error()))) {
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
            ns_h2_feed_status_line(c, c->proto ? c->proto : "HTTP/2");
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
    if (namelen == 7 && g_ascii_strncasecmp(name, "alt-svc", 7) == 0)
        ns_h3_altsvc_note(c->url, value, valuelen);
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

    nghttp2_nv *headers = g_new(nghttp2_nv, nva->len);
    memcpy(headers, nva->data, nva->len * sizeof *headers);
    int32_t sid = nghttp2_submit_request(conn->session, NULL, headers, nva->len,
                                         provp, c);
    g_free(headers);
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

        ns_pollfd pfd[2];
        pfd[0].fd = conn->wake[0];
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = conn->fd;
        pfd[1].events = POLLIN | (want_write ? POLLOUT : 0);
        pfd[1].revents = 0;
        int pr = ns_h2_socket_poll(pfd, 2, 250);
        if (pr < 0 && !ns_h2_interrupted(ns_h2_socket_error())) {
            g_mutex_lock(&conn->lock);
            conn->io_failed = TRUE;
            g_mutex_unlock(&conn->lock);
            continue;
        }
        if (pfd[0].revents & POLLIN) {
            ns_h2_wake_drain(conn->wake[0]);
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
                    have_clen = clen >= 0;
                    g_free(cl);
                } else if (nlen == 10 &&
                    g_ascii_strncasecmp(line, "set-cookie", 10) == 0) {
                    char *cv = g_strndup(val, vlen);
                    ns_net_store_set_cookie(c->url, cv);
                    g_free(cv);
                } else if (nlen == 7 &&
                    g_ascii_strncasecmp(line, "alt-svc", 7) == 0) {
                    ns_h3_altsvc_note(c->url, val, vlen);
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

static GMutex      g_altsvc_lock;
static GHashTable *g_altsvc_h3;

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
        ns_h2_wake_signal(conn->wake[1]);
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
    ns_h2_socket_close(conn->fd);
    ns_h2_socket_close(conn->wake[0]);
    ns_h2_socket_close(conn->wake[1]);
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
            ns_h2_wake_signal(conn->wake[1]);
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
    g_mutex_lock(&g_altsvc_lock);
    if (g_altsvc_h3) {
        g_hash_table_destroy(g_altsvc_h3);
        g_altsvc_h3 = NULL;
    }
    g_mutex_unlock(&g_altsvc_lock);
}

static ns_conn *
ns_h2_conn_open(const char *origin, const char *host, int port, gboolean https,
                const ns_hop_req *req, ns_h2 *c, ns_hop_out *out,
                gint64 start, gint64 connect_deadline,
                GCancellable *cancellable)
{
    char *conn_err = NULL;
    char *remote_ip = NULL;
    ns_socket fd = ns_h2_connect(host, port, connect_deadline, cancellable,
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
                ns_h2_socket_close(fd); g_free(remote_ip);
                return NULL;
            }
            ssl = SSL_new(ctx);
            SSL_set_fd(ssl, (int)fd);
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
                    SSL_free(ssl); ns_h2_socket_close(fd); g_free(remote_ip);
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
                        ns_h2_socket_close(fd);
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
                SSL_free(ssl); ns_h2_socket_close(fd); g_free(remote_ip);
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
        if (!ns_h2_wake_pair(conn->wake)) {
            conn->wake[0] = conn->wake[1] = -1;
            ns_h2_conn_destroy(conn);
            out->error_message = g_strdup("wake socket initialization failed");
            return NULL;
        }
        ns_h2_set_nonblocking(fd, TRUE);
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

#ifdef NS_HTTP_HAVE_HTTP3
    if (https && ns_h3_should_try(origin) &&
        ns_h3_perform(req, wctx, hctx, out, cancellable, host, port,
                      authority, path)) {
        g_free(origin); g_free(authority); g_free(path);
        return TRUE;
    }
#endif

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
            ns_h2_wake_signal(conn->wake[1]);
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

static void
ns_h3_altsvc_note(const char *url, const char *value, size_t vlen)
{
    gboolean h3 = FALSE;
    for (size_t i = 0; i + 2 < vlen; i++)
        if ((value[i] == 'h' || value[i] == 'H') && value[i + 1] == '3' &&
            (value[i + 2] == '=' || value[i + 2] == '-' || value[i + 2] == ',' ||
             value[i + 2] == ' ' || value[i + 2] == ';')) {
            h3 = TRUE;
            break;
        }
    if (!h3)
        return;
    char *origin = ns_url_origin_from(url);
    if (!origin)
        return;
    g_mutex_lock(&g_altsvc_lock);
    if (!g_altsvc_h3)
        g_altsvc_h3 = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (!g_hash_table_contains(g_altsvc_h3, origin))
        g_hash_table_add(g_altsvc_h3, origin);
    else
        g_free(origin);
    g_mutex_unlock(&g_altsvc_lock);
}

#ifdef NS_HTTP_HAVE_HTTP3

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_gnutls.h>
#include <nghttp3/nghttp3.h>
#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>
#include <time.h>

typedef struct {
    ns_h2 *c;
    int fd;
    ngtcp2_conn *conn;
    gnutls_session_t tls;
    gnutls_certificate_credentials_t cred;
    nghttp3_conn *h3;
    ngtcp2_crypto_conn_ref conn_ref;
    int64_t stream_id;
    gboolean h3_setup;
    gboolean done;
    gboolean failed;
    gboolean got_response;
    const guint8 *body;
    size_t body_len;
    size_t body_off;
} ns_h3;

static ngtcp2_tstamp
ns_h3_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ngtcp2_tstamp)ts.tv_sec * NGTCP2_SECONDS + (ngtcp2_tstamp)ts.tv_nsec;
}

static ngtcp2_conn *
ns_h3_get_conn(ngtcp2_crypto_conn_ref *ref)
{
    ns_h3 *h = ref->user_data;
    return h->conn;
}

static void
ns_h3_rand_cb(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *ctx)
{
    (void)ctx;
    gnutls_rnd(GNUTLS_RND_RANDOM, dest, destlen);
}

static int
ns_h3_get_new_cid_cb(ngtcp2_conn *conn, ngtcp2_cid *cid, uint8_t *token,
                     size_t cidlen, void *user_data)
{
    (void)conn; (void)user_data;
    if (gnutls_rnd(GNUTLS_RND_RANDOM, cid->data, cidlen) != 0)
        return NGTCP2_ERR_CALLBACK_FAILURE;
    cid->datalen = cidlen;
    if (gnutls_rnd(GNUTLS_RND_RANDOM, token, NGTCP2_STATELESS_RESET_TOKENLEN) != 0)
        return NGTCP2_ERR_CALLBACK_FAILURE;
    return 0;
}

static int
ns_h3_recv_stream_data_cb(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
                          uint64_t offset, const uint8_t *data, size_t datalen,
                          void *user_data, void *stream_user_data)
{
    (void)offset; (void)stream_user_data;
    ns_h3 *h = user_data;
    if (!h->h3)
        return 0;
    nghttp3_ssize n = nghttp3_conn_read_stream(
        h->h3, stream_id, data, datalen,
        (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0);
    if (n < 0) {
        h->failed = TRUE;
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    ngtcp2_conn_extend_max_stream_offset(conn, stream_id, (uint64_t)n);
    ngtcp2_conn_extend_max_offset(conn, (uint64_t)n);
    return 0;
}

static int
ns_h3_acked_cb(ngtcp2_conn *conn, int64_t stream_id, uint64_t offset,
               uint64_t datalen, void *user_data, void *stream_user_data)
{
    (void)conn; (void)offset; (void)stream_user_data;
    ns_h3 *h = user_data;
    if (h->h3)
        nghttp3_conn_add_ack_offset(h->h3, stream_id, datalen);
    return 0;
}

static int
ns_h3_stream_close_cb(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
                      uint64_t app_error_code, void *user_data,
                      void *stream_user_data)
{
    (void)conn; (void)stream_user_data;
    ns_h3 *h = user_data;
    if (!(flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET))
        app_error_code = NGHTTP3_H3_NO_ERROR;
    if (h->h3) {
        int rv = nghttp3_conn_close_stream(h->h3, stream_id, app_error_code);
        if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND)
            h->failed = TRUE;
    }
    if (stream_id == h->stream_id)
        h->done = TRUE;
    return 0;
}

static int
ns_h3_extend_max_streams_cb(ngtcp2_conn *conn, uint64_t max_streams,
                            void *user_data)
{
    (void)conn; (void)max_streams; (void)user_data;
    return 0;
}

static int
ns_h3_recv_header_cb(nghttp3_conn *h3conn, int64_t stream_id, int32_t token,
                     nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t flags,
                     void *conn_user_data, void *stream_user_data)
{
    (void)h3conn; (void)stream_id; (void)token; (void)flags;
    (void)stream_user_data;
    ns_h3 *h = conn_user_data;
    nghttp3_vec nv = nghttp3_rcbuf_get_buf(name);
    nghttp3_vec vv = nghttp3_rcbuf_get_buf(value);
    ns_h2_on_response_header(h->c, (const char *)nv.base, nv.len,
                             (const char *)vv.base, vv.len);
    h->got_response = TRUE;
    return 0;
}

static int
ns_h3_recv_data_cb(nghttp3_conn *h3conn, int64_t stream_id, const uint8_t *data,
                   size_t datalen, void *conn_user_data, void *stream_user_data)
{
    (void)h3conn; (void)stream_id; (void)stream_user_data;
    ns_h3 *h = conn_user_data;
    ns_h2_on_body(h->c, data, datalen);
    return 0;
}

static int
ns_h3_end_stream_cb(nghttp3_conn *h3conn, int64_t stream_id,
                    void *conn_user_data, void *stream_user_data)
{
    (void)h3conn; (void)stream_id; (void)stream_user_data;
    ns_h3 *h = conn_user_data;
    h->done = TRUE;
    return 0;
}

static int
ns_h3_h3_stream_close_cb(nghttp3_conn *h3conn, int64_t stream_id,
                         uint64_t app_error_code, void *conn_user_data,
                         void *stream_user_data)
{
    (void)h3conn; (void)app_error_code; (void)stream_user_data;
    ns_h3 *h = conn_user_data;
    if (stream_id == h->stream_id)
        h->done = TRUE;
    return 0;
}

static nghttp3_ssize
ns_h3_body_read_cb(nghttp3_conn *h3conn, int64_t stream_id, nghttp3_vec *vec,
                   size_t veccnt, uint32_t *pflags, void *conn_user_data,
                   void *stream_user_data)
{
    (void)h3conn; (void)stream_id; (void)veccnt; (void)stream_user_data;
    ns_h3 *h = conn_user_data;
    size_t remain = h->body_len - h->body_off;
    if (remain == 0) {
        *pflags |= NGHTTP3_DATA_FLAG_EOF;
        return 0;
    }
    vec[0].base = (uint8_t *)(h->body + h->body_off);
    vec[0].len = remain;
    h->body_off = h->body_len;
    *pflags |= NGHTTP3_DATA_FLAG_EOF;
    return 1;
}

static const char ns_h3_priority[] =
    "%DISABLE_TLS13_COMPAT_MODE:NORMAL:-VERS-ALL:+VERS-TLS1.3:"
    "-CIPHER-ALL:+AES-128-GCM:+AES-256-GCM:+CHACHA20-POLY1305:+AES-128-CCM:"
    "-GROUP-ALL:+GROUP-SECP256R1:+GROUP-X25519:+GROUP-SECP384R1:"
    "+GROUP-SECP521R1";

static gboolean
ns_h3_gnutls_init(ns_h3 *h, const char *host, gboolean insecure)
{
    if (gnutls_certificate_allocate_credentials(&h->cred) != 0)
        return FALSE;
    if (insecure) {
        gnutls_certificate_set_verify_flags(h->cred, 0);
    } else {
        const char *ca = ns_net_ca_bundle_path();
        if (ca && *ca)
            gnutls_certificate_set_x509_trust_file(h->cred, ca,
                                                   GNUTLS_X509_FMT_PEM);
        else
            gnutls_certificate_set_x509_system_trust(h->cred);
    }
    if (gnutls_init(&h->tls, GNUTLS_CLIENT) != 0)
        return FALSE;
    if (gnutls_priority_set_direct(h->tls, ns_h3_priority, NULL) != 0)
        return FALSE;
    if (gnutls_credentials_set(h->tls, GNUTLS_CRD_CERTIFICATE, h->cred) != 0)
        return FALSE;
    if (ngtcp2_crypto_gnutls_configure_client_session(h->tls) != 0)
        return FALSE;
    h->conn_ref.get_conn = ns_h3_get_conn;
    h->conn_ref.user_data = h;
    gnutls_session_set_ptr(h->tls, &h->conn_ref);
    if (!insecure)
        gnutls_session_set_verify_cert(h->tls, host, 0);
    gnutls_server_name_set(h->tls, GNUTLS_NAME_DNS, host, strlen(host));
    gnutls_datum_t alpn = { (unsigned char *)"h3", 2 };
    gnutls_alpn_set_protocols(h->tls, &alpn, 1, 0);
    return TRUE;
}

static gboolean
ns_h3_setup(ns_h3 *h, const char *authority, const char *path,
            const char *method)
{
    nghttp3_settings settings;
    nghttp3_settings_default(&settings);
    nghttp3_callbacks cbs = {
        .stream_close = ns_h3_h3_stream_close_cb,
        .recv_data = ns_h3_recv_data_cb,
        .recv_header = ns_h3_recv_header_cb,
        .end_stream = ns_h3_end_stream_cb,
    };
    if (nghttp3_conn_client_new(&h->h3, &cbs, &settings, NULL, h) != 0)
        return FALSE;

    int64_t ctrl = -1, qpe = -1, qpd = -1;
    if (ngtcp2_conn_open_uni_stream(h->conn, &ctrl, NULL) != 0 ||
        ngtcp2_conn_open_uni_stream(h->conn, &qpe, NULL) != 0 ||
        ngtcp2_conn_open_uni_stream(h->conn, &qpd, NULL) != 0)
        return FALSE;
    if (nghttp3_conn_bind_control_stream(h->h3, ctrl) != 0 ||
        nghttp3_conn_bind_qpack_streams(h->h3, qpe, qpd) != 0)
        return FALSE;

    if (ngtcp2_conn_open_bidi_stream(h->conn, &h->stream_id, h) != 0)
        return FALSE;

    GPtrArray *owned = g_ptr_array_new_with_free_func(g_free);
    GArray *nva = g_array_new(FALSE, FALSE, sizeof(nghttp3_nv));
    ns_h2 *c = h->c;
#define NS_H3_ADD(N, V) do {                                              \
        nghttp3_nv _nv = { (uint8_t *)(N), (uint8_t *)(V),                \
                           strlen(N), strlen(V), NGHTTP3_NV_FLAG_NONE };  \
        g_array_append_val(nva, _nv);                                     \
    } while (0)
    NS_H3_ADD(":method", method);
    NS_H3_ADD(":scheme", "https");
    NS_H3_ADD(":authority", authority);
    NS_H3_ADD(":path", path);
    if (c->req->user_agent)
        NS_H3_ADD("user-agent", c->req->user_agent);
    NS_H3_ADD("accept-encoding", ns_h2_accept_encoding());
    if (c->req->referer && *c->req->referer)
        NS_H3_ADD("referer", c->req->referer);
    char *cookie = ns_net_cookies_for_request(c->url);
    if (cookie) {
        g_ptr_array_add(owned, cookie);
        NS_H3_ADD("cookie", cookie);
    }
    for (struct curl_slist *sh = c->req->headers; sh; sh = sh->next) {
        const char *line = sh->data;
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
        nghttp3_nv nv = { (uint8_t *)lname, (uint8_t *)lval,
                          strlen(lname), strlen(lval), NGHTTP3_NV_FLAG_NONE };
        g_array_append_val(nva, nv);
    }
#undef NS_H3_ADD

    nghttp3_data_reader dr = { .read_data = ns_h3_body_read_cb };
    int rv = nghttp3_conn_submit_request(h->h3, h->stream_id,
                                         (nghttp3_nv *)nva->data, nva->len,
                                         (h->body && h->body_len) ? &dr : NULL,
                                         h);
    g_array_free(nva, TRUE);
    g_ptr_array_free(owned, TRUE);
    return rv == 0;
}

static gboolean
ns_h3_write(ns_h3 *h)
{
    uint8_t buf[1452];
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi;
    for (;;) {
        int64_t stream_id = -1;
        int fin = 0;
        nghttp3_vec vec[16];
        nghttp3_ssize sveccnt = 0;
        if (h->h3 && ngtcp2_conn_get_max_data_left(h->conn)) {
            sveccnt = nghttp3_conn_writev_stream(h->h3, &stream_id, &fin,
                                                 vec, 16);
            if (sveccnt < 0)
                return FALSE;
        }
        ngtcp2_tstamp ts = ns_h3_now();
        ngtcp2_ssize ndatalen;
        uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
        if (fin)
            flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
        ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(
            h->conn, &ps.path, &pi, buf, sizeof buf, &ndatalen, flags,
            stream_id, (ngtcp2_vec *)vec, (size_t)sveccnt, ts);
        if (nwrite < 0) {
            if (nwrite == NGTCP2_ERR_WRITE_MORE) {
                if (ndatalen >= 0)
                    nghttp3_conn_add_write_offset(h->h3, stream_id,
                                                  (size_t)ndatalen);
                continue;
            }
            if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED ||
                nwrite == NGTCP2_ERR_STREAM_SHUT_WR) {
                if (h->h3)
                    nghttp3_conn_block_stream(h->h3, stream_id);
                continue;
            }
            return FALSE;
        }
        if (ndatalen >= 0 && h->h3 && stream_id >= 0)
            nghttp3_conn_add_write_offset(h->h3, stream_id, (size_t)ndatalen);
        if (nwrite == 0)
            return TRUE;
        for (ssize_t off = 0; off < nwrite; ) {
            gssize s = ns_h2_socket_send(h->fd, buf + off,
                                         (size_t)(nwrite - off), 0);
            if (s < 0) {
                if (errno == EINTR)
                    continue;
                return FALSE;
            }
            off += s;
        }
    }
}

static gboolean
ns_h3_perform(const ns_hop_req *req, ns_write_ctx *wctx, ns_header_ctx *hctx,
              ns_hop_out *out, GCancellable *cancellable, const char *host,
              int port, const char *authority, const char *path)
{
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char portstr[16];
    g_snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return FALSE;

    int fd = socket(res->ai_family, SOCK_DGRAM, 0);
    if (fd < 0) { freeaddrinfo(res); return FALSE; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd); freeaddrinfo(res);
        return FALSE;
    }
    struct sockaddr_storage la;
    socklen_t lalen = sizeof la;
    getsockname(fd, (struct sockaddr *)&la, &lalen);
    struct sockaddr_storage ra;
    socklen_t ralen = res->ai_addrlen;
    memcpy(&ra, res->ai_addr, ralen);
    freeaddrinfo(res);
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    ns_h2 c = {0};
    c.req = req; c.wctx = wctx; c.hctx = hctx; c.out = out;
    c.cancellable = cancellable; c.url = req->url; c.proto = "HTTP/3";
    c.start_us = g_get_monotonic_time();
    c.deadline_us = c.start_us +
        (gint64)(req->timeout_s > 0 ? req->timeout_s : 30) * G_USEC_PER_SEC;

    ns_h3 h = {0};
    h.c = &c; h.fd = fd; h.stream_id = -1;
    h.body = req->body; h.body_len = req->body_len;

    gboolean insecure = getenv("NS_HTTP3_INSECURE") != NULL;
    if (!ns_h3_gnutls_init(&h, host, insecure)) {
        if (h.tls) gnutls_deinit(h.tls);
        if (h.cred) gnutls_certificate_free_credentials(h.cred);
        close(fd);
        return FALSE;
    }

    ngtcp2_path path_s = {
        .local = { (ngtcp2_sockaddr *)&la, lalen },
        .remote = { (ngtcp2_sockaddr *)&ra, ralen },
        .user_data = NULL,
    };
    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = ns_h3_now();
    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_streams_uni = 3;
    params.initial_max_stream_data_bidi_local = 1024 * 1024;
    params.initial_max_data = 8 * 1024 * 1024;
    params.initial_max_stream_data_uni = 256 * 1024;
    params.max_idle_timeout = 30 * NGTCP2_SECONDS;

    ngtcp2_cid dcid, scid;
    uint8_t cidbuf[18];
    gnutls_rnd(GNUTLS_RND_RANDOM, cidbuf, sizeof cidbuf);
    ngtcp2_cid_init(&dcid, cidbuf, 16);
    gnutls_rnd(GNUTLS_RND_RANDOM, cidbuf, sizeof cidbuf);
    ngtcp2_cid_init(&scid, cidbuf, 16);

    ngtcp2_callbacks callbacks = {
        .client_initial = ngtcp2_crypto_client_initial_cb,
        .recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb,
        .encrypt = ngtcp2_crypto_encrypt_cb,
        .decrypt = ngtcp2_crypto_decrypt_cb,
        .hp_mask = ngtcp2_crypto_hp_mask_cb,
        .recv_stream_data = ns_h3_recv_stream_data_cb,
        .acked_stream_data_offset = ns_h3_acked_cb,
        .stream_close = ns_h3_stream_close_cb,
        .recv_retry = ngtcp2_crypto_recv_retry_cb,
        .extend_max_local_streams_bidi = ns_h3_extend_max_streams_cb,
        .rand = ns_h3_rand_cb,
        .get_new_connection_id = ns_h3_get_new_cid_cb,
        .update_key = ngtcp2_crypto_update_key_cb,
        .delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
        .delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
        .get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb,
        .version_negotiation = ngtcp2_crypto_version_negotiation_cb,
    };

    if (ngtcp2_conn_client_new(&h.conn, &dcid, &scid, &path_s,
                               NGTCP2_PROTO_VER_V1, &callbacks, &settings,
                               &params, NULL, &h) != 0) {
        gnutls_deinit(h.tls);
        gnutls_certificate_free_credentials(h.cred);
        close(fd);
        return FALSE;
    }
    ngtcp2_conn_set_tls_native_handle(h.conn, h.tls);

    gboolean produced = FALSE;
    for (;;) {
        gint64 nowus = g_get_monotonic_time();
        if (ns_net_aborting() ||
            (cancellable && g_cancellable_is_cancelled(cancellable)) ||
            nowus > c.deadline_us) {
            h.failed = TRUE;
            break;
        }
        if (!h.h3_setup && ngtcp2_conn_get_handshake_completed(h.conn)) {
            const char *method = (req->method && *req->method) ? req->method
                                                               : "GET";
            if (!ns_h3_setup(&h, authority, path, method)) {
                h.failed = TRUE;
                break;
            }
            h.h3_setup = TRUE;
            out->t_appconnect_ms = ns_h2_ms_since(c.start_us);
        }
        if (!ns_h3_write(&h)) {
            h.failed = TRUE;
            break;
        }
        if (h.done || h.failed)
            break;

        ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(h.conn);
        ngtcp2_tstamp tnow = ns_h3_now();
        int timeout_ms = 250;
        if (expiry != UINT64_MAX) {
            if (expiry <= tnow)
                timeout_ms = 0;
            else {
                uint64_t d = (expiry - tnow) / 1000000ULL;
                timeout_ms = d > 250 ? 250 : (int)d;
            }
        }
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            h.failed = TRUE;
            break;
        }
        tnow = ns_h3_now();
        if (pr > 0 && (pfd.revents & POLLIN)) {
            for (;;) {
                uint8_t rbuf[65536];
                gssize n = ns_h2_socket_recv(fd, rbuf, sizeof rbuf, 0);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    if (ns_h2_retryable(errno)) break;
                    h.failed = TRUE;
                    break;
                }
                if (n == 0) break;
                ngtcp2_path rpath = {
                    .local = { (ngtcp2_sockaddr *)&la, lalen },
                    .remote = { (ngtcp2_sockaddr *)&ra, ralen },
                    .user_data = NULL,
                };
                ngtcp2_pkt_info pi = {0};
                int rv = ngtcp2_conn_read_pkt(h.conn, &rpath, &pi, rbuf,
                                              (size_t)n, tnow);
                if (rv != 0) {
                    h.failed = TRUE;
                    break;
                }
            }
        } else {
            if (ngtcp2_conn_handle_expiry(h.conn, tnow) != 0) {
                h.failed = TRUE;
                break;
            }
        }
        if (h.done || h.failed)
            break;
    }

    if (h.got_response) {
        if (c.comp_buf)
            ns_h2_flush_body(&c);
        out->status = c.status;
        out->http_version = CURL_HTTP_VERSION_3;
        out->t_total_ms = ns_h2_ms_since(c.start_us);
        out->num_connects = 1;
        {
            char ipbuf[INET6_ADDRSTRLEN] = {0};
            void *ap = ra.ss_family == AF_INET
                ? (void *)&((struct sockaddr_in *)&ra)->sin_addr
                : (void *)&((struct sockaddr_in6 *)&ra)->sin6_addr;
            if (inet_ntop(ra.ss_family, ap, ipbuf, sizeof ipbuf)) {
                g_free(out->remote_ip);
                out->remote_ip = g_strdup(ipbuf);
            }
        }
        if (c.sink_full) {
            wctx->exceeded = TRUE;
            out->ok = FALSE;
        } else if (h.done && c.status > 0) {
            out->ok = TRUE;
        } else {
            out->ok = FALSE;
            if (!out->error_message)
                out->error_message = g_strdup("HTTP/3 transfer incomplete");
        }
        produced = TRUE;
    } else if (c.comp_buf) {
        g_byte_array_free(c.comp_buf, TRUE);
        c.comp_buf = NULL;
    }

    ngtcp2_conn_del(h.conn);
    gnutls_deinit(h.tls);
    gnutls_certificate_free_credentials(h.cred);
    close(fd);
    return produced;
}

static gboolean
ns_h3_should_try(const char *origin)
{
    if (getenv("NS_FORCE_HTTP3"))
        return TRUE;
    gboolean yes = FALSE;
    g_mutex_lock(&g_altsvc_lock);
    if (g_altsvc_h3)
        yes = g_hash_table_contains(g_altsvc_h3, origin);
    g_mutex_unlock(&g_altsvc_lock);
    return yes;
}

#endif /* NS_HTTP_HAVE_HTTP3 */

gboolean
ns_hop_transport(const ns_hop_req *req, ns_write_ctx *wctx,
                 ns_header_ctx *hctx, ns_hop_out *out, GCancellable *cancellable)
{
    if ((req->proxy && *req->proxy) || req->request_ftp ||
        !ns_url_is_http_or_https(req->url))
        return ns_hop_transport_curl(req, wctx, hctx, out, cancellable);

    return ns_h2_perform(req, wctx, hctx, out, cancellable);
}
