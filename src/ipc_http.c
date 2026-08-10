/* Nordstjernen — minimal HTTP/1.1 + JSON framing for the IPC experiment. */

#include "ipc_http.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <fcntl.h>
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif
#endif

void
http_conn_init(http_conn *c, int fd)
{
    c->fd = fd;
    c->start = 0;
    c->len = 0;
}

#ifdef _WIN32

void
http_set_bufsize(int fd, int bytes)
{
    (void)fd;
    (void)bytes;
}

void
http_set_read_timeout(int fd, int seconds)
{
    (void)fd;
    (void)seconds;
}

#else

#include <sys/time.h>

void
http_set_bufsize(int fd, int bytes)
{
    int snd = SO_SNDBUF, rcv = SO_RCVBUF;
#ifdef SO_SNDBUFFORCE
    snd = SO_SNDBUFFORCE;
#endif
#ifdef SO_RCVBUFFORCE
    rcv = SO_RCVBUFFORCE;
#endif
    if (setsockopt(fd, SOL_SOCKET, snd, &bytes, sizeof bytes) != 0)
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof bytes);
    if (setsockopt(fd, SOL_SOCKET, rcv, &bytes, sizeof bytes) != 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof bytes);
}

void
http_set_read_timeout(int fd, int seconds)
{
    struct timeval tv = { seconds, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

int
http_send_fd(int sock, int fd)
{
    char dummy = 'F';
    struct iovec iov = { &dummy, 1 };
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } u;
    memset(&u, 0, sizeof u);
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof u.buf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    for (;;) {
        if (sendmsg(sock, &msg, 0) >= 0)
            return 0;
        if (errno != EINTR)
            return -1;
    }
}

int
http_recv_fd(int sock)
{
    char dummy;
    struct iovec iov = { &dummy, 1 };
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } u;
    memset(&u, 0, sizeof u);
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof u.buf;
    ssize_t r;
    while ((r = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC)) < 0 && errno == EINTR)
        ;
    if (r <= 0)
        return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
        return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(c), sizeof(int));
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

#endif

static int
conn_fill(http_conn *c)
{
    if (c->len > 0)
        return 0;
    c->start = 0;
    for (;;) {
        ssize_t r = read(c->fd, c->buf, sizeof c->buf);
        if (r > 0) {
            c->len = (size_t)r;
            return 0;
        }
        if (r == 0)
            return -1;
        if (errno == EINTR)
            continue;
        return -1;
    }
}

static int
conn_read_line(http_conn *c, char *out, size_t cap)
{
    size_t n = 0;
    for (;;) {
        if (c->len == 0 && conn_fill(c) != 0)
            return -1;
        unsigned char ch = c->buf[c->start];
        c->start++;
        c->len--;
        if (ch == '\n') {
            while (n > 0 && out[n - 1] == '\r')
                n--;
            out[n] = '\0';
            return (int)n;
        }
        if (n + 1 < cap)
            out[n++] = (char)ch;
    }
}

int
http_read_body(http_conn *c, long n, void *dst)
{
    if (n < 0)
        return -1;
    if (n > 0 && !dst)
        return -1;
    unsigned char *p = dst;
    long got = 0;
    while (got < n) {
        if (c->len == 0 && conn_fill(c) != 0)
            return -1;
        size_t take = c->len;
        if (take > (size_t)(n - got))
            take = (size_t)(n - got);
        memcpy(p + got, c->buf + c->start, take);
        c->start += take;
        c->len -= take;
        got += (long)take;
    }
    return 0;
}

int
http_skip_body(http_conn *c, long n)
{
    if (n < 0)
        return -1;
    long got = 0;
    while (got < n) {
        if (c->len == 0 && conn_fill(c) != 0)
            return -1;
        size_t take = c->len;
        if (take > (size_t)(n - got))
            take = (size_t)(n - got);
        c->start += take;
        c->len -= take;
        got += (long)take;
    }
    return 0;
}

int
http_read_head(http_conn *c, http_head *out)
{
    memset(out, 0, sizeof *out);
    out->content_length = 0;
    out->x_w = out->x_h = out->x_stride = out->x_anim = -1;
    out->x_page_w = out->x_page_h = -1;
    out->x_scroll_y = out->x_scroll_x = -1;
    out->x_unchanged = 0;

    char line[20480];
    int len = conn_read_line(c, line, sizeof line);
    if (len <= 0)
        return -1;

    if (strncmp(line, "HTTP/", 5) == 0) {
        char *sp = strchr(line, ' ');
        out->status = sp ? atoi(sp + 1) : 0;
    } else {
        char *sp = strchr(line, ' ');
        if (!sp)
            return -1;
        size_t mlen = (size_t)(sp - line);
        if (mlen >= sizeof out->method)
            mlen = sizeof out->method - 1;
        memcpy(out->method, line, mlen);
        out->method[mlen] = '\0';
        char *p2 = sp + 1;
        char *sp2 = strchr(p2, ' ');
        size_t plen = sp2 ? (size_t)(sp2 - p2) : strlen(p2);
        if (plen >= sizeof out->path)
            plen = sizeof out->path - 1;
        memcpy(out->path, p2, plen);
        out->path[plen] = '\0';
    }

    int header_count = 0;
    for (;;) {
        len = conn_read_line(c, line, sizeof line);
        if (len < 0)
            return -1;
        if (len == 0)
            break;
        if (++header_count > NS_HTTP_MAX_HEADERS)
            return -1;
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = '\0';
        const char *val = colon + 1;
        while (*val == ' ')
            val++;
        if (strcasecmp(line, "Content-Length") == 0)
            out->content_length = atol(val);
        else if (strcasecmp(line, "X-W") == 0)
            out->x_w = atol(val);
        else if (strcasecmp(line, "X-H") == 0)
            out->x_h = atol(val);
        else if (strcasecmp(line, "X-Stride") == 0)
            out->x_stride = atol(val);
        else if (strcasecmp(line, "X-Anim") == 0)
            out->x_anim = atol(val);
        else if (strcasecmp(line, "X-PageW") == 0)
            out->x_page_w = atol(val);
        else if (strcasecmp(line, "X-PageH") == 0)
            out->x_page_h = atol(val);
        else if (strcasecmp(line, "X-ScrollY") == 0)
            out->x_scroll_y = atol(val);
        else if (strcasecmp(line, "X-ScrollX") == 0)
            out->x_scroll_x = atol(val);
        else if (strcasecmp(line, "X-Unchanged") == 0)
            out->x_unchanged = atol(val);
        else if (strcasecmp(line, "X-Render-RC") == 0)
            out->x_render_rc = atol(val);
        else if (strcasecmp(line, "X-Clipboard") == 0)
            out->x_clipboard = atol(val);
        else if (strcasecmp(line, "X-Nav") == 0) {
            size_t vlen = strlen(val);
            if (vlen >= sizeof out->x_nav)
                vlen = sizeof out->x_nav - 1;
            memcpy(out->x_nav, val, vlen);
            out->x_nav[vlen] = '\0';
        }
        else if (strcasecmp(line, "X-WebGL") == 0) {
            size_t vlen = strlen(val);
            if (vlen >= sizeof out->x_webgl)
                vlen = sizeof out->x_webgl - 1;
            memcpy(out->x_webgl, val, vlen);
            out->x_webgl[vlen] = '\0';
        }
        else if (strcasecmp(line, "X-Camera") == 0) {
            size_t vlen = strlen(val);
            if (vlen >= sizeof out->x_camera)
                vlen = sizeof out->x_camera - 1;
            memcpy(out->x_camera, val, vlen);
            out->x_camera[vlen] = '\0';
        }
        else if (strcasecmp(line, "X-Download") == 0) {
            size_t vlen = strlen(val);
            if (vlen >= sizeof out->x_download)
                vlen = sizeof out->x_download - 1;
            memcpy(out->x_download, val, vlen);
            out->x_download[vlen] = '\0';
        }
        else if (strcasecmp(line, "X-Audio") == 0) {
            size_t vlen = strlen(val);
            if (vlen >= sizeof out->x_audio)
                vlen = sizeof out->x_audio - 1;
            memcpy(out->x_audio, val, vlen);
            out->x_audio[vlen] = '\0';
        }
        else if (strcasecmp(line, "X-Window-Action") == 0) {
            size_t vlen = strlen(val);
            if (vlen >= sizeof out->x_window_action)
                vlen = sizeof out->x_window_action - 1;
            memcpy(out->x_window_action, val, vlen);
            out->x_window_action[vlen] = '\0';
        }
    }
    if (out->content_length < 0 || out->content_length > NS_HTTP_MAX_BODY)
        return -1;
    return 0;
}

static int
write_all(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w > 0) {
            p += (size_t)w;
            n -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

int
http_write_request(int fd, const char *method, const char *path,
                   const char *content_type, const void *body, size_t n)
{
    char head[512];
    int hlen = snprintf(head, sizeof head,
                        "%s %s HTTP/1.1\r\nContent-Type: %s\r\n"
                        "Content-Length: %zu\r\n\r\n",
                        method, path, content_type ? content_type : "text/plain",
                        n);
    if (hlen < 0 || (size_t)hlen >= sizeof head)
        return -1;
    if (write_all(fd, head, (size_t)hlen) != 0)
        return -1;
    if (n && write_all(fd, body, n) != 0)
        return -1;
    return 0;
}

int
http_write_response(int fd, int status, const char *content_type,
                    const char *extra_headers, const void *body, size_t n)
{
    const char *ct = content_type ? content_type : "text/plain";
    const char *eh = extra_headers ? extra_headers : "";
    int hlen = snprintf(NULL, 0,
                        "HTTP/1.1 %d OK\r\nContent-Type: %s\r\n%s"
                        "Content-Length: %zu\r\n\r\n",
                        status, ct, eh, n);
    if (hlen < 0)
        return -1;
    char *head = malloc((size_t)hlen + 1);
    if (!head)
        return -1;
    if (snprintf(head, (size_t)hlen + 1,
                 "HTTP/1.1 %d OK\r\nContent-Type: %s\r\n%s"
                 "Content-Length: %zu\r\n\r\n",
                 status, ct, eh, n) != hlen ||
        write_all(fd, head, (size_t)hlen) != 0) {
        free(head);
        return -1;
    }
    free(head);
    if (n && write_all(fd, body, n) != 0)
        return -1;
    return 0;
}

char *
json_escape(const char *s)
{
    if (!s)
        s = "";
    size_t len = strlen(s);
    if (len > (SIZE_MAX - 1) / 6)
        return NULL;
    size_t cap = len * 6 + 1;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    char *w = out;
    for (const char *p = s; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '"' || ch == '\\') {
            *w++ = '\\';
            *w++ = (char)ch;
        } else if (ch == '\n') {
            *w++ = '\\';
            *w++ = 'n';
        } else if (ch == '\r') {
            *w++ = '\\';
            *w++ = 'r';
        } else if (ch == '\t') {
            *w++ = '\\';
            *w++ = 't';
        } else if (ch < 0x20) {
            w += snprintf(w, 7, "\\u%04x", ch);
        } else {
            *w++ = (char)ch;
        }
    }
    *w = '\0';
    return out;
}

static const char *
json_find_value(const char *body, const char *key)
{
    if (!body)
        return NULL;
    size_t klen = strlen(key);
    for (const char *p = body; (p = strchr(p, '"')) != NULL; p++) {
        if (strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char *q = p + 1 + klen + 1;
            while (*q == ' ' || *q == ':')
                q++;
            return q;
        }
    }
    return NULL;
}

int
json_get_long(const char *body, const char *key, long *out)
{
    const char *v = json_find_value(body, key);
    if (!v)
        return -1;
    *out = atol(v);
    return 0;
}

int
json_get_double(const char *body, const char *key, double *out)
{
    const char *v = json_find_value(body, key);
    if (!v)
        return -1;
    int sign = 1;
    if (*v == '-') { sign = -1; v++; }
    else if (*v == '+') { v++; }
    double val = 0.0;
    while (*v >= '0' && *v <= '9') { val = val * 10.0 + (*v - '0'); v++; }
    if (*v == '.') {
        v++;
        double frac = 0.1;
        while (*v >= '0' && *v <= '9') {
            val += (*v - '0') * frac;
            frac *= 0.1;
            v++;
        }
    }
    if (*v == 'e' || *v == 'E') {
        v++;
        int esign = 1;
        if (*v == '-') { esign = -1; v++; }
        else if (*v == '+') { v++; }
        int e = 0;
        while (*v >= '0' && *v <= '9') {
            if (e < 1000) e = e * 10 + (*v - '0');
            v++;
        }
        if (e > 308) e = 308;
        double p = 1.0;
        while (e-- > 0) p *= 10.0;
        val = esign > 0 ? val * p : val / p;
    }
    *out = sign * val;
    return 0;
}

static int
json_hex4(const char *s)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int c = (unsigned char)s[i];
        if (c >= '0' && c <= '9') c -= '0';
        else if (c >= 'a' && c <= 'f') c -= 'a' - 10;
        else if (c >= 'A' && c <= 'F') c -= 'A' - 10;
        else return -1;
        v = (v << 4) | c;
    }
    return v;
}

static int
json_utf8_encode(unsigned int cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

char *
json_get_str(const char *body, const char *key)
{
    const char *v = json_find_value(body, key);
    if (!v || *v != '"')
        return NULL;
    v++;
    size_t cap = strlen(v) + 1;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    char *w = out;
    while (*v && *v != '"') {
        if (*v == '\\' && v[1]) {
            v++;
            switch (*v) {
            case 'n': *w++ = '\n'; break;
            case 'r': *w++ = '\r'; break;
            case 't': *w++ = '\t'; break;
            case 'u': {
                int hi = json_hex4(v + 1);
                if (hi < 0)
                    break;
                v += 4;
                unsigned int cp = (unsigned int)hi;
                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    v[1] == '\\' && v[2] == 'u') {
                    int lo = json_hex4(v + 3);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) +
                             ((unsigned int)lo - 0xDC00u);
                        v += 6;
                    }
                }
                w += json_utf8_encode(cp, w);
                break;
            }
            default: *w++ = *v; break;
            }
            v++;
        } else {
            *w++ = *v++;
        }
    }
    *w = '\0';
    return out;
}
