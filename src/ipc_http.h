/* Nordstjernen — minimal HTTP/1.1 + JSON framing for the IPC experiment. */

#ifndef NS_IPC_HTTP_H
#define NS_IPC_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NS_HTTP_MAX_BODY    (64 * 1024 * 1024)
#define NS_HTTP_MAX_REPLY   (16 * 1024 * 1024)
#define NS_HTTP_MAX_HEADERS 64

typedef struct {
    int           fd;
    unsigned char buf[16384];
    size_t        start;
    size_t        len;
} http_conn;

typedef struct {
    char  method[8];
    char  path[128];
    int   status;
    long  content_length;
    long  x_w, x_h, x_stride, x_anim, x_unchanged, x_render_rc;
    long  x_page_w, x_page_h, x_scroll_y, x_scroll_x;
    char  x_nav[2048];
    char  x_webgl[2048];
    char  x_camera[2048];
    char  x_download[3072];
    char  x_audio[16384];
    char  x_window_action[32];
} http_head;

void http_conn_init(http_conn *c, int fd);
void http_set_bufsize(int fd, int bytes);
void http_set_read_timeout(int fd, int seconds);
int  http_send_fd(int sock, int fd);
int  http_recv_fd(int sock);

int  http_read_head(http_conn *c, http_head *out);
int  http_read_body(http_conn *c, long n, void *dst);
int  http_skip_body(http_conn *c, long n);

int  http_write_request(int fd, const char *method, const char *path,
                        const char *content_type, const void *body, size_t n);
int  http_write_response(int fd, int status, const char *content_type,
                         const char *extra_headers, const void *body, size_t n);

char *json_escape(const char *s);
int   json_get_long(const char *body, const char *key, long *out);
int   json_get_double(const char *body, const char *key, double *out);
char *json_get_str(const char *body, const char *key);

#ifdef __cplusplus
}
#endif

#endif
