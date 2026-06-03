/* Nordstjernen — developer console window (JS REPL, profiler, debug log). */

#include <gtk/gtk.h>
#include <string.h>

#include "console.h"
#include "debuglog.h"
#include "env.h"
#include "js.h"
#include "net.h"
#include "profiler.h"
#include "version.h"
#include "window.h"

void
nd_window_console_append(nd_window *w, const char *line)
{
    if (!w || !w->console.buffer || !line) return;
    GDateTime *now = g_date_time_new_now_local();
    char *ts = g_strdup_printf("%02d:%02d:%02d  ",
                               g_date_time_get_hour(now),
                               g_date_time_get_minute(now),
                               g_date_time_get_second(now));
    g_date_time_unref(now);
    const char *tag = NULL;
    if (g_str_has_prefix(line, "[error]")) tag = "error";
    else if (g_str_has_prefix(line, "[warn]")) tag = "warn";
    else if (g_str_has_prefix(line, "[alert]")) tag = "alert";
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(w->console.buffer, &end);
    GtkTextMark *start_mark = gtk_text_buffer_create_mark(w->console.buffer,
                                                          NULL, &end, TRUE);
    gtk_text_buffer_insert(w->console.buffer, &end, ts, -1);
    GtkTextIter ts_start;
    gtk_text_buffer_get_iter_at_mark(w->console.buffer, &ts_start, start_mark);
    GtkTextIter ts_end_iter;
    gtk_text_buffer_get_end_iter(w->console.buffer, &ts_end_iter);
    gtk_text_buffer_apply_tag_by_name(w->console.buffer, "timestamp",
                                      &ts_start, &ts_end_iter);
    gtk_text_buffer_delete_mark(w->console.buffer, start_mark);

    GtkTextIter body_start;
    gtk_text_buffer_get_end_iter(w->console.buffer, &body_start);
    GtkTextMark *body_mark = gtk_text_buffer_create_mark(w->console.buffer,
                                                         NULL, &body_start, TRUE);
    char *with_nl = g_strconcat(line, "\n", NULL);
    GtkTextIter ins_end;
    gtk_text_buffer_get_end_iter(w->console.buffer, &ins_end);
    gtk_text_buffer_insert(w->console.buffer, &ins_end, with_nl, -1);
    g_free(with_nl);
    if (tag) {
        GtkTextIter line_start, line_end;
        gtk_text_buffer_get_iter_at_mark(w->console.buffer, &line_start, body_mark);
        gtk_text_buffer_get_end_iter(w->console.buffer, &line_end);
        gtk_text_buffer_apply_tag_by_name(w->console.buffer, tag,
                                          &line_start, &line_end);
    }
    gtk_text_buffer_delete_mark(w->console.buffer, body_mark);
    g_free(ts);
}

static void
nd_console_entry_activate(GtkEntry *entry, gpointer user_data)
{
    nd_window *w = user_data;
    const char *src = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!src || !*src) return;
    char *echo = g_strdup_printf("> %s", src);
    nd_window_console_append(w, echo);
    g_free(echo);
    nd_window_ensure_js(w);
    if (w->js) {
        char *result = nd_js_eval_source(w->js, src, "console");
        if (nd_js_consume_mutated(w->js))
            nd_window_js_mutated(w);
        if (result) {
            nd_window_console_append(w, result);
            g_free(result);
        }
    } else {
        nd_window_console_append(w, "(no JS context)");
    }
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

static void
nd_console_emit_env_line(const char *label, const char *value, gpointer user_data)
{
    nd_window *w = user_data;
    char *line = g_strdup_printf("%-11s : %s", label, value);
    nd_window_console_append(w, line);
    g_free(line);
}

static void
nd_window_console_emit_banner(nd_window *w)
{
    char *line;

    line = g_strdup_printf("Nordstjernen %s — JavaScript console", ND_VERSION);
    nd_window_console_append(w, line); g_free(line);

    nd_env_each(nd_console_emit_env_line, w);

    if (w->last_render_us > 0) {
        double ms = (double)w->last_render_us / 1000.0;
        line = g_strdup_printf("%-11s : %.1f ms", "Last render", ms);
    } else {
        line = g_strdup_printf("%-11s : (not measured yet)", "Last render");
    }
    nd_window_console_append(w, line); g_free(line);

    const char *enc = nd_net_supported_encodings();
    line = g_strdup_printf("%-11s : %s", "Encodings",
                           (enc && *enc) ? enc : "(identity only)");
    nd_window_console_append(w, line); g_free(line);

    nd_window_console_append(w, "");
}

static void nd_window_console_build_profile_tab(nd_window *w, GtkWidget *notebook);
static void nd_window_console_build_dlog_tab(nd_window *w, GtkWidget *notebook);
static void nd_console_on_window_destroy(GtkWidget *widget, gpointer user_data);

void
nd_window_open_console(nd_window *w)
{
    if (w->console.window) {
        gtk_window_present(GTK_WINDOW(w->console.window));
        if (w->console.entry) gtk_widget_grab_focus(w->console.entry);
        return;
    }
    w->console.window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(w->console.window), "JavaScript Console — Nordstjernen");
    gtk_window_set_icon_name(GTK_WINDOW(w->console.window), "nordstjernen");
    gtk_window_set_default_size(GTK_WINDOW(w->console.window), 760, 520);
    gtk_window_set_transient_for(GTK_WINDOW(w->console.window), GTK_WINDOW(w->window));
    g_object_add_weak_pointer(G_OBJECT(w->console.window), (gpointer *)&w->console.window);
    g_signal_connect(w->console.window, "destroy",
                     G_CALLBACK(nd_console_on_window_destroy), w);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_window_set_child(GTK_WINDOW(w->console.window), notebook);
    w->console.notebook = notebook;

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    GtkWidget *text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(text_view), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(text_view), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text_view), 6);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(text_view), 6);
    w->console.buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    gtk_text_buffer_create_tag(w->console.buffer, "warn",
                               "foreground", "#b25400", NULL);
    gtk_text_buffer_create_tag(w->console.buffer, "error",
                               "foreground", "#c00",
                               "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(w->console.buffer, "alert",
                               "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(w->console.buffer, "timestamp",
                               "foreground", "#888", NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), text_view);
    gtk_box_append(GTK_BOX(vbox), scrolled);

    GtkWidget *input_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(input_row, 4);
    gtk_widget_set_margin_end(input_row, 4);
    gtk_widget_set_margin_top(input_row, 4);
    gtk_widget_set_margin_bottom(input_row, 4);
    GtkWidget *prompt = gtk_label_new(">");
    w->console.entry = gtk_entry_new();
    gtk_widget_set_hexpand(w->console.entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->console.entry),
                                   "Evaluate JavaScript in this page");
    gtk_box_append(GTK_BOX(input_row), prompt);
    gtk_box_append(GTK_BOX(input_row), w->console.entry);
    gtk_box_append(GTK_BOX(vbox), input_row);

    g_signal_connect(w->console.entry, "activate",
                     G_CALLBACK(nd_console_entry_activate), w);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vbox,
                             gtk_label_new("Console"));

    nd_window_console_build_profile_tab(w, notebook);
    nd_window_console_build_dlog_tab(w, notebook);

    nd_window_console_emit_banner(w);

    gtk_window_present(GTK_WINDOW(w->console.window));
    gtk_widget_grab_focus(w->console.entry);
}

static void
nd_console_profile_append(nd_window *w, const char *line)
{
    if (!w || !w->console.profile_buffer || !line) return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(w->console.profile_buffer, &end);
    gtk_text_buffer_insert(w->console.profile_buffer, &end, line, -1);
    gtk_text_buffer_get_end_iter(w->console.profile_buffer, &end);
    gtk_text_buffer_insert(w->console.profile_buffer, &end, "\n", 1);
}

typedef struct {
    nd_window *w;
} nd_console_profile_ctx;

static void
nd_console_profile_progress(guint done, guint total, gpointer user_data)
{
    nd_console_profile_ctx *ctx = user_data;
    if (!ctx || !ctx->w || !ctx->w->console.profile_progress_label) return;
    char *msg = g_strdup_printf("Sampling %u / %u…", done, total);
    gtk_label_set_text(GTK_LABEL(ctx->w->console.profile_progress_label), msg);
    g_free(msg);
}

static void
nd_console_profile_done(const nd_profile_result *r, gpointer user_data)
{
    nd_console_profile_ctx *ctx = user_data;
    nd_window *w = ctx ? ctx->w : NULL;
    g_free(ctx);
    if (!w) return;
    w->console.profile_running = FALSE;
    if (w->console.profile_start_btn)
        gtk_button_set_label(GTK_BUTTON(w->console.profile_start_btn),
                             "Start sampling");
    if (w->console.profile_progress_label) {
        if (r->ok) {
            char *msg = g_strdup_printf("Captured %u samples in %.2fs",
                                        r->samples_taken,
                                        r->wall_us / 1.0e6);
            gtk_label_set_text(GTK_LABEL(w->console.profile_progress_label), msg);
            g_free(msg);
        } else {
            gtk_label_set_text(GTK_LABEL(w->console.profile_progress_label),
                               r->error_message ? r->error_message : "Failed");
        }
    }
    if (!w->console.profile_buffer) return;

    guint denom = r->thread_snapshots > 0 ? r->thread_snapshots : r->samples_taken;
    char *hdr = g_strdup_printf(
        "\n=== Profile run %s — samples=%u/%u threads=%u interval=%ums wall=%.2fs ===\n",
        r->ok ? "ok" : "failed",
        r->samples_taken, r->samples_requested,
        r->thread_snapshots,
        r->interval_ms, r->wall_us / 1.0e6);
    nd_console_profile_append(w, hdr);
    g_free(hdr);
    if (!r->ok && r->error_message) {
        char *line = g_strdup_printf("error: %s", r->error_message);
        nd_console_profile_append(w, line);
        g_free(line);
    }
    if (denom == 0) return;

    nd_console_profile_append(w,
        "Hot C functions (leaf, ignoring poll/main-loop):");
    nd_console_profile_append(w,
        "  hits   pct  function");
    guint shown = 0;
    for (guint i = 0; i < r->leaf_rows->len && shown < 20; i++) {
        nd_profile_row row = g_array_index(r->leaf_rows, nd_profile_row, i);
        double pct = 100.0 * (double)row.hits / (double)denom;
        char *line = g_strdup_printf("  %4u  %5.1f%%  %s",
                                     row.hits, pct, row.function);
        nd_console_profile_append(w, line);
        g_free(line);
        shown++;
    }
    nd_console_profile_append(w, "");
    nd_console_profile_append(w, "Top-of-stack frames (raw frame #0):");
    nd_console_profile_append(w,
        "  hits   pct  function");
    shown = 0;
    for (guint i = 0; i < r->top_rows->len && shown < 10; i++) {
        nd_profile_row row = g_array_index(r->top_rows, nd_profile_row, i);
        double pct = 100.0 * (double)row.hits / (double)denom;
        char *line = g_strdup_printf("  %4u  %5.1f%%  %s",
                                     row.hits, pct, row.function);
        nd_console_profile_append(w, line);
        g_free(line);
        shown++;
    }
}

static void
nd_console_profile_start_clicked(GtkButton *btn, gpointer user_data)
{
    nd_window *w = user_data;
    if (!w || w->console.profile_running) return;
    int samples = 30, interval = 50;
    if (w->console.profile_samples_spin)
        samples = (int)gtk_spin_button_get_value(
            GTK_SPIN_BUTTON(w->console.profile_samples_spin));
    if (w->console.profile_interval_spin)
        interval = (int)gtk_spin_button_get_value(
            GTK_SPIN_BUTTON(w->console.profile_interval_spin));
    w->console.profile_running = TRUE;
    gtk_button_set_label(btn, "Sampling…");
    if (w->console.profile_progress_label)
        gtk_label_set_text(GTK_LABEL(w->console.profile_progress_label),
                           "Starting gdb…");
    nd_console_profile_ctx *ctx = g_new0(nd_console_profile_ctx, 1);
    ctx->w = w;
    if (!nd_profiler_run_async((guint)samples, (guint)interval,
                               nd_console_profile_progress,
                               nd_console_profile_done, ctx)) {
        w->console.profile_running = FALSE;
        gtk_button_set_label(btn, "Start sampling");
    }
}

static void
nd_console_profile_clear_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    nd_window *w = user_data;
    if (!w || !w->console.profile_buffer) return;
    GtkTextIter a, b;
    gtk_text_buffer_get_start_iter(w->console.profile_buffer, &a);
    gtk_text_buffer_get_end_iter(w->console.profile_buffer, &b);
    gtk_text_buffer_delete(w->console.profile_buffer, &a, &b);
}

static void
nd_window_console_build_profile_tab(nd_window *w, GtkWidget *notebook)
{
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 6);
    gtk_widget_set_margin_end(vbox, 6);
    gtk_widget_set_margin_top(vbox, 6);
    gtk_widget_set_margin_bottom(vbox, 6);

    GtkWidget *intro = gtk_label_new(
        "Attaches gdb to this process, sampling stacks to find C functions "
        "that take time. Requires gdb in $PATH. Interact with the page in the "
        "main window while sampling — the listing shows where time is spent.");
    gtk_label_set_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_xalign(GTK_LABEL(intro), 0.0);
    gtk_box_append(GTK_BOX(vbox), intro);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(row), gtk_label_new("Samples:"));
    GtkWidget *samples = gtk_spin_button_new_with_range(5, 500, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(samples), 30);
    w->console.profile_samples_spin = samples;
    gtk_box_append(GTK_BOX(row), samples);

    gtk_box_append(GTK_BOX(row), gtk_label_new("Interval (ms):"));
    GtkWidget *interval = gtk_spin_button_new_with_range(5, 1000, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(interval), 50);
    w->console.profile_interval_spin = interval;
    gtk_box_append(GTK_BOX(row), interval);

    GtkWidget *start = gtk_button_new_with_label("Start sampling");
    g_signal_connect(start, "clicked",
                     G_CALLBACK(nd_console_profile_start_clicked), w);
    w->console.profile_start_btn = start;
    gtk_box_append(GTK_BOX(row), start);

    GtkWidget *clear = gtk_button_new_with_label("Clear");
    g_signal_connect(clear, "clicked",
                     G_CALLBACK(nd_console_profile_clear_clicked), w);
    gtk_box_append(GTK_BOX(row), clear);

    gtk_box_append(GTK_BOX(vbox), row);

    GtkWidget *progress = gtk_label_new(
        nd_profiler_supported()
            ? "Ready. Click \"Start sampling\" to capture a profile."
            : "gdb not found in $PATH — profiler unavailable.");
    gtk_label_set_xalign(GTK_LABEL(progress), 0.0);
    w->console.profile_progress_label = progress;
    gtk_box_append(GTK_BOX(vbox), progress);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 6);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 6);
    w->console.profile_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), view);
    gtk_box_append(GTK_BOX(vbox), scrolled);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vbox,
                             gtk_label_new("Profiler"));
}

static void
nd_console_dlog_format(const nd_dlog_entry *e, char **out_line)
{
    gint64 wall_us = g_get_real_time() -
        (g_get_monotonic_time() - e->monotonic_us);
    GDateTime *t = g_date_time_new_from_unix_local(wall_us / G_USEC_PER_SEC);
    char *ts = t ? g_strdup_printf("%02d:%02d:%02d.%03d",
                                   g_date_time_get_hour(t),
                                   g_date_time_get_minute(t),
                                   g_date_time_get_second(t),
                                   (int)((wall_us % G_USEC_PER_SEC) / 1000))
                 : g_strdup("--:--:--.---");
    if (t) g_date_time_unref(t);
    *out_line = g_strdup_printf("%s  [%-6s] %s: %s",
                                ts,
                                nd_dlog_level_name(e->level),
                                e->category ? e->category : "",
                                e->message  ? e->message  : "");
    g_free(ts);
}

static void
nd_console_dlog_append_entry(nd_window *w, const nd_dlog_entry *e)
{
    if (!w || !w->console.dlog_buffer || !e) return;
    char *line = NULL;
    nd_console_dlog_format(e, &line);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(w->console.dlog_buffer, &end);
    GtkTextMark *mk = gtk_text_buffer_create_mark(w->console.dlog_buffer,
                                                  NULL, &end, TRUE);
    gtk_text_buffer_insert(w->console.dlog_buffer, &end, line, -1);
    gtk_text_buffer_get_end_iter(w->console.dlog_buffer, &end);
    gtk_text_buffer_insert(w->console.dlog_buffer, &end, "\n", 1);
    const char *tag = NULL;
    switch (e->level) {
    case ND_DLOG_ERROR:  tag = "dlog-error";  break;
    case ND_DLOG_WARN:   tag = "dlog-warn";   break;
    case ND_DLOG_RENDER: tag = "dlog-render"; break;
    case ND_DLOG_NET:    tag = "dlog-net";    break;
    case ND_DLOG_JS:     tag = "dlog-js";     break;
    default: break;
    }
    if (tag) {
        GtkTextIter ls, le;
        gtk_text_buffer_get_iter_at_mark(w->console.dlog_buffer, &ls, mk);
        gtk_text_buffer_get_end_iter(w->console.dlog_buffer, &le);
        gtk_text_buffer_apply_tag_by_name(w->console.dlog_buffer, tag, &ls, &le);
    }
    gtk_text_buffer_delete_mark(w->console.dlog_buffer, mk);
    g_free(line);
}

typedef struct {
    guint w_id;
    nd_dlog_entry e;
} nd_dlog_idle_payload;

static gboolean
nd_console_dlog_idle_cb(gpointer user_data)
{
    nd_dlog_idle_payload *p = user_data;
    nd_window *w = nd_window_for_id(p->w_id);
    if (w && w->console.dlog_buffer)
        nd_console_dlog_append_entry(w, &p->e);
    g_free(p->e.category);
    g_free(p->e.message);
    g_free(p);
    return G_SOURCE_REMOVE;
}

static void
nd_console_dlog_listener(const nd_dlog_entry *e, gpointer user_data)
{
    nd_window *w = user_data;
    if (!w || !e) return;
    nd_dlog_idle_payload *p = g_new0(nd_dlog_idle_payload, 1);
    p->w_id = w->id;
    p->e.monotonic_us = e->monotonic_us;
    p->e.level        = e->level;
    p->e.category     = g_strdup(e->category ? e->category : "");
    p->e.message      = g_strdup(e->message  ? e->message  : "");
    g_idle_add(nd_console_dlog_idle_cb, p);
}

static void
nd_console_dlog_visit_initial(gpointer item, gpointer user_data)
{
    nd_window *w = user_data;
    nd_dlog_entry *e = item;
    nd_console_dlog_append_entry(w, e);
}

static void
nd_console_dlog_clear_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    nd_window *w = user_data;
    if (!w || !w->console.dlog_buffer) return;
    GtkTextIter a, b;
    gtk_text_buffer_get_start_iter(w->console.dlog_buffer, &a);
    gtk_text_buffer_get_end_iter(w->console.dlog_buffer, &b);
    gtk_text_buffer_delete(w->console.dlog_buffer, &a, &b);
}

static void
nd_window_console_build_dlog_tab(nd_window *w, GtkWidget *notebook)
{
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 6);
    gtk_widget_set_margin_end(vbox, 6);
    gtk_widget_set_margin_top(vbox, 6);
    gtk_widget_set_margin_bottom(vbox, 6);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *info = gtk_label_new(
        "Live log of navigation, fetch, layout, paint, and JS events.");
    gtk_label_set_xalign(GTK_LABEL(info), 0.0);
    gtk_widget_set_hexpand(info, TRUE);
    gtk_box_append(GTK_BOX(row), info);

    GtkWidget *clear = gtk_button_new_with_label("Clear");
    g_signal_connect(clear, "clicked",
                     G_CALLBACK(nd_console_dlog_clear_clicked), w);
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(vbox), row);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 6);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 6);
    w->console.dlog_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_create_tag(w->console.dlog_buffer, "dlog-error",
                               "foreground", "#c00",
                               "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(w->console.dlog_buffer, "dlog-warn",
                               "foreground", "#b25400", NULL);
    gtk_text_buffer_create_tag(w->console.dlog_buffer, "dlog-render",
                               "foreground", "#055", NULL);
    gtk_text_buffer_create_tag(w->console.dlog_buffer, "dlog-net",
                               "foreground", "#226", NULL);
    gtk_text_buffer_create_tag(w->console.dlog_buffer, "dlog-js",
                               "foreground", "#553", NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), view);
    gtk_box_append(GTK_BOX(vbox), scrolled);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vbox,
                             gtk_label_new("Debug Log"));

    nd_debug_log_snapshot(nd_console_dlog_visit_initial, w);
    w->console.dlog_sub_id = nd_debug_log_subscribe(nd_console_dlog_listener, w);
}

static void
nd_console_on_window_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    nd_window *w = user_data;
    if (!w) return;
    if (w->console.dlog_sub_id) {
        nd_debug_log_unsubscribe(w->console.dlog_sub_id);
        w->console.dlog_sub_id = 0;
    }
    w->console.notebook              = NULL;
    w->console.buffer                = NULL;
    w->console.entry                 = NULL;
    w->console.profile_buffer        = NULL;
    w->console.profile_start_btn     = NULL;
    w->console.profile_samples_spin  = NULL;
    w->console.profile_interval_spin = NULL;
    w->console.profile_progress_label= NULL;
    w->console.dlog_buffer           = NULL;
    w->console.profile_running       = FALSE;
}

void
on_win_open_console(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    nd_window_open_console(w);
}

void
nd_window_console_close(nd_window *w)
{
    if (!w) return;
    if (w->console.dlog_sub_id) {
        nd_debug_log_unsubscribe(w->console.dlog_sub_id);
        w->console.dlog_sub_id = 0;
    }
    if (w->console.window) {
        g_signal_handlers_disconnect_by_func(
            w->console.window, G_CALLBACK(nd_console_on_window_destroy), w);
        gtk_window_destroy(GTK_WINDOW(w->console.window));
    }
}
