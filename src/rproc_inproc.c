/* Nordstjernen — single-process mode: serves every tab's renderer session on
   the shell's main-context thread instead of per-tab renderer processes. */

#include "rproc_inproc.h"

#include <glib.h>
#include <stdlib.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "ipc_http.h"
#include "libnordstjernen.h"
#include "renderer_serve.h"
#include "rproc_http.h"

typedef struct {
    int                  ctrl_r;
    int                  ctrl_w;
    unsigned char       *fb;
    ns_renderer_session *session;
} InprocConn;

typedef struct {
    InprocConn *conn;
    http_head   head;
    char       *body;
    int         close_conn;
} InprocItem;

static int      g_enabled;
static gboolean g_engine_inited;
static gboolean g_handling;
static GQueue   g_pending = G_QUEUE_INIT;

static void
conn_close(InprocConn *conn)
{
    ns_renderer_session_free(conn->session);
#ifdef _WIN32
    _close(conn->ctrl_r);
    _close(conn->ctrl_w);
#else
    close(conn->ctrl_r);
#endif
    free(conn->fb);
    g_free(conn);
}

static void
item_run(InprocItem *item)
{
    if (item->close_conn)
        conn_close(item->conn);
    else
        ns_renderer_session_handle(item->conn->session, &item->head,
                                   item->body);
    free(item->body);
    g_free(item);
}

static gboolean g_retry_scheduled;

static gboolean item_retry_tick(gpointer data);

static void
item_schedule_retry(void)
{
    if (g_retry_scheduled) return;
    g_retry_scheduled = TRUE;
    g_timeout_add(5, item_retry_tick, NULL);
}

static gboolean
item_busy(const InprocItem *item)
{
    return item->conn && ns_renderer_session_busy(item->conn->session);
}

static void
item_run_queue(void)
{
    g_handling = TRUE;
    if (!g_engine_inited) {
        g_engine_inited = TRUE;
        ns_browser_init();
    }
    InprocItem *item;
    while ((item = g_queue_peek_head(&g_pending)) && !item_busy(item)) {
        g_queue_pop_head(&g_pending);
        item_run(item);
    }
    g_handling = FALSE;
    if (!g_queue_is_empty(&g_pending))
        item_schedule_retry();
}

static gboolean
item_retry_tick(gpointer data)
{
    (void)data;
    if (g_handling) return G_SOURCE_CONTINUE;
    InprocItem *head = g_queue_peek_head(&g_pending);
    if (head && item_busy(head)) return G_SOURCE_CONTINUE;
    g_retry_scheduled = FALSE;
    if (head) item_run_queue();
    return G_SOURCE_REMOVE;
}

static gboolean
item_dispatch(gpointer data)
{
    InprocItem *item = data;
    g_queue_push_tail(&g_pending, item);
    if (g_handling)
        return G_SOURCE_REMOVE;
    if (item_busy(g_queue_peek_head(&g_pending))) {
        item_schedule_retry();
        return G_SOURCE_REMOVE;
    }
    item_run_queue();
    return G_SOURCE_REMOVE;
}

static void
item_post(InprocItem *item)
{
    GSource *src = g_idle_source_new();
    g_source_set_priority(src, G_PRIORITY_DEFAULT);
    g_source_set_callback(src, item_dispatch, item, NULL);
    g_source_attach(src, NULL);
    g_source_unref(src);
}

static gpointer
reader_thread_main(gpointer data)
{
    InprocConn *conn = data;
    http_conn c;
    http_conn_init(&c, conn->ctrl_r);
    for (;;) {
        InprocItem *item = g_new0(InprocItem, 1);
        item->conn = conn;
        if (http_read_head(&c, &item->head) != 0 ||
            item->head.content_length > NS_HTTP_MAX_BODY) {
            item->close_conn = 1;
            item_post(item);
            return NULL;
        }
        if (item->head.content_length > 0) {
            item->body = malloc((size_t)item->head.content_length + 1);
            if (!item->body ||
                http_read_body(&c, item->head.content_length,
                               item->body) != 0) {
                free(item->body);
                item->body = NULL;
                item->close_conn = 1;
                item_post(item);
                return NULL;
            }
            item->body[item->head.content_length] = '\0';
        }
        item_post(item);
    }
}

static int
inproc_attach(int ctrl_r, int ctrl_w, unsigned char *fb, int max_w, int max_h)
{
    InprocConn *conn = g_new0(InprocConn, 1);
    conn->ctrl_r = ctrl_r;
    conn->ctrl_w = ctrl_w;
    conn->fb = fb;
    conn->session = ns_renderer_session_new(ctrl_w, fb, max_w, max_h, 1);
    if (!conn->session) {
        g_free(conn);
        return -1;
    }
    g_thread_unref(g_thread_new("ns-inproc-read", reader_thread_main, conn));
    return 0;
}

void
ns_rproc_single_process_enable(void)
{
    g_enabled = 1;
    ns_rproc_http_set_inproc(inproc_attach);
}

int
ns_rproc_single_process_enabled(void)
{
    return g_enabled;
}
