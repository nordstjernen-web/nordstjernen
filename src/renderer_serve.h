/* Nordstjernen — renderer request handling shared by the out-of-process
   renderer executable and the in-process single-process-mode host. */

#ifndef NS_RENDERER_SERVE_H
#define NS_RENDERER_SERVE_H

#include <glib.h>

#include "ipc_http.h"
#include "print.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ns_renderer_session ns_renderer_session;

ns_renderer_session *ns_renderer_session_new(int ctrl_w, unsigned char *fb,
                                             int max_w, int max_h,
                                             int shm_mode);

/* Dispatch one already-read request. Returns 1 after /quit, else 0. */
int  ns_renderer_session_handle(ns_renderer_session *s, const http_head *head,
                                const char *body);

/* Closes the session's open page. Does not free or unmap the framebuffer. */
void ns_renderer_session_free(ns_renderer_session *s);

/* Nonzero while the session's page is pumping a nested main loop (blocking
   fetch); freeing or navigating it then would tear down live stack state. */
int ns_renderer_session_busy(const ns_renderer_session *s);

/* One cairo recording surface per sheet of paper. The caller destroys them. */
GPtrArray *ns_renderer_session_print(ns_renderer_session *s,
                                     ns_print_setup *out_setup);

#ifdef __cplusplus
}
#endif

#endif
