/* Nordstjernen — JavaScript engine binding (QuickJS). */

#ifndef ND_JS_H
#define ND_JS_H

#include <glib.h>

#include "dom.h"

G_BEGIN_DECLS

typedef struct nd_js nd_js;

typedef void (*nd_js_log_cb)(const char *line, gpointer user_data);
typedef void (*nd_js_mutated_cb)(gpointer user_data);

nd_js *nd_js_new(nd_js_log_cb     log_cb,    gpointer log_user_data,
                 nd_js_mutated_cb mut_cb,    gpointer mut_user_data);
void   nd_js_free(nd_js *js);

void     nd_js_run_scripts_in_doc(nd_js *js, const nd_node *doc, const char *base_url);
gboolean nd_js_consume_mutated(nd_js *js);

char  *nd_js_eval_source(nd_js *js, const char *src, const char *origin);

G_END_DECLS

#endif
