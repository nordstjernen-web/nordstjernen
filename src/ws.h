/* Nordstjernen — minimal WebSocket client (libcurl-backed).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#ifndef ND_WS_H
#define ND_WS_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct nd_ws nd_ws;

typedef enum {
    ND_WS_STATE_CONNECTING = 0,
    ND_WS_STATE_OPEN       = 1,
    ND_WS_STATE_CLOSING    = 2,
    ND_WS_STATE_CLOSED     = 3,
} nd_ws_state;

typedef struct nd_ws_callbacks {
    void (*on_open)   (gpointer user_data);
    void (*on_text)   (const char *text, gsize len, gpointer user_data);
    void (*on_binary) (const guint8 *data, gsize len, gpointer user_data);
    void (*on_close)  (int code, const char *reason, gboolean clean, gpointer user_data);
    void (*on_error)  (const char *message, gpointer user_data);
} nd_ws_callbacks;

gboolean nd_ws_available(void);

nd_ws *nd_ws_new(const char        *url,
                 const char        *origin,
                 const char *const *protocols,
                 const nd_ws_callbacks *cbs,
                 gpointer           user_data);

gboolean nd_ws_send_text(nd_ws *ws, const char *text, gsize len);
gboolean nd_ws_send_binary(nd_ws *ws, const guint8 *data, gsize len);

void     nd_ws_close(nd_ws *ws, int code, const char *reason);

int      nd_ws_state_get(nd_ws *ws);

void     nd_ws_free(nd_ws *ws);

G_END_DECLS

#endif
