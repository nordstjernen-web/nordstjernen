/* Nordstjernen — page export: screenshot (PNG), save (PDF/HTML), and printing. */

#include <gtk/gtk.h>
#include <cairo-pdf.h>

#include "export.h"
#include "dom.h"
#include "layout.h"
#include "paint.h"
#include "window.h"

gboolean
nd_window_screenshot_to(nd_window *w, const char *path)
{
    if (!w || !path) return FALSE;
    double vw = w->last_viewport_w > 0 ? w->last_viewport_w : nd_layout_viewport();
    nd_window_ensure_layout(w, vw);
    if (!w->layout_tree) return FALSE;
    double pw = vw;
    double ph = w->layout_tree->content_height + 1;
    int iw = (int)(pw + 0.5);
    int ih = (int)(ph + 0.5);
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
    if (ih > 32767) ih = 32767;
    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return FALSE;
    }
    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    nd_paint_set_js(w->js);
    nd_paint_set_anim(w->anim);
    nd_paint_with_selection(cr, w->layout_tree, NULL, &w->selection);
    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(surf, path);
    cairo_surface_destroy(surf);
    return st == CAIRO_STATUS_SUCCESS;
}

static void
nd_save_screenshot_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    nd_window *w = nd_window_for_id(GPOINTER_TO_UINT(user_data));
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!file || !w) { if (file) g_object_unref(file); return; }
    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path) return;
    if (nd_window_screenshot_to(w, path))
        nd_window_set_status(w, "Saved screenshot: %s", path);
    else
        nd_window_set_status(w, "Cannot write %s", path);
    g_free(path);
}

void
on_win_screenshot(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    if (!w->layout_tree) return;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save screenshot as PNG");
    char *title_text = nd_window_current_title(w);
    char *suggested  = g_strdup_printf("%s.png",
        title_text && *title_text ? title_text : "page");
    g_free(title_text);
    gtk_file_dialog_set_initial_name(dialog, suggested);
    g_free(suggested);
    gtk_file_dialog_save(dialog, GTK_WINDOW(w->window), NULL,
                         nd_save_screenshot_done, GUINT_TO_POINTER(w->id));
    g_object_unref(dialog);
}

static void
nd_save_pdf_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    nd_window *w = nd_window_for_id(GPOINTER_TO_UINT(user_data));
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!file || !w) { if (file) g_object_unref(file); return; }
    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path) return;

    nd_window_ensure_layout(w, nd_layout_viewport());
    if (!w->layout_tree) { g_free(path); return; }
    double pw = nd_layout_viewport();
    double ph = w->layout_tree->content_height + 32;
    cairo_surface_t *surf = cairo_pdf_surface_create(path, pw, ph);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        nd_window_set_status(w, "Cannot write %s", path);
        g_free(path);
        return;
    }
    cairo_t *cr = cairo_create(surf);
    nd_paint(cr, w->layout_tree, NULL);
    cairo_destroy(cr);
    cairo_surface_finish(surf);
    cairo_surface_destroy(surf);
    nd_window_set_status(w, "Saved PDF: %s", path);
    g_free(path);
}

void
on_win_save_pdf(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    if (!w->layout_tree) return;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save page as PDF");
    char *title_text = nd_window_current_title(w);
    char *suggested  = g_strdup_printf("%s.pdf",
        title_text && *title_text ? title_text : "page");
    g_free(title_text);
    gtk_file_dialog_set_initial_name(dialog, suggested);
    g_free(suggested);
    gtk_file_dialog_save(dialog, GTK_WINDOW(w->window), NULL,
                         nd_save_pdf_done, GUINT_TO_POINTER(w->id));
    g_object_unref(dialog);
}

static void
nd_save_html_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    nd_window *w = nd_window_for_id(GPOINTER_TO_UINT(user_data));
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!file || !w) { if (file) g_object_unref(file); return; }
    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path) return;

    char *body = NULL;
    gsize body_len = 0;
    if (!w->dom_mutated && w->last_body && w->last_body_len > 0) {
        body = g_memdup2(w->last_body, w->last_body_len);
        body_len = w->last_body_len;
    } else if (w->parsed_doc) {
        body = nd_node_outer_html(w->parsed_doc);
        body_len = body ? strlen(body) : 0;
    }
    if (!body || body_len == 0) {
        g_free(body);
        nd_window_set_status(w, "Nothing to save");
        g_free(path);
        return;
    }

    GError *err = NULL;
    if (!g_file_set_contents(path, body, (gssize)body_len, &err)) {
        nd_window_set_status(w, "Cannot write %s: %s",
                             path, err ? err->message : "(unknown)");
        g_clear_error(&err);
    } else {
        nd_window_set_status(w, "Saved HTML: %s", path);
    }
    g_free(body);
    g_free(path);
}

void
on_win_save_html(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    if (!w->parsed_doc && (!w->last_body || w->last_body_len == 0)) return;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save page as HTML");
    char *title_text = nd_window_current_title(w);
    char *suggested  = g_strdup_printf("%s.html",
        title_text && *title_text ? title_text : "page");
    g_free(title_text);
    gtk_file_dialog_set_initial_name(dialog, suggested);
    g_free(suggested);
    gtk_file_dialog_save(dialog, GTK_WINDOW(w->window), NULL,
                         nd_save_html_done, GUINT_TO_POINTER(w->id));
    g_object_unref(dialog);
}

typedef struct nd_print_ctx {
    guint      w_id;
    double     scale;
    double     page_content_h;
    int        n_pages;
    double    *page_offsets;
    char      *header_title;
    char      *header_url;
} nd_print_ctx;

static void
nd_print_ctx_free(nd_print_ctx *pc)
{
    if (!pc) return;
    g_free(pc->page_offsets);
    g_free(pc->header_title);
    g_free(pc->header_url);
    g_free(pc);
}

static void
nd_print_collect_breakpoints(const nd_box *root, GArray *ys)
{
    if (!root) return;
    for (const nd_box *c = root->first_child; c; c = c->next_sibling) {
        if (c->kind != ND_BOX_BLOCK && c->kind != ND_BOX_TABLE &&
            c->kind != ND_BOX_TABLE_CAPTION &&
            c->kind != ND_BOX_TABLE_ROW && c->kind != ND_BOX_IMAGE &&
            c->kind != ND_BOX_VIDEO)
            continue;
        double top    = c->y + c->margin.top;
        double height = c->content_height + c->padding.top + c->padding.bottom +
                        c->border.top + c->border.bottom;
        double bottom = top + height;
        g_array_append_val(ys, top);
        g_array_append_val(ys, bottom);
        if (height > 0)
            nd_print_collect_breakpoints(c, ys);
    }
}

static int
nd_print_compare_double(gconstpointer a, gconstpointer b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return  1;
    return 0;
}

static void
nd_on_print_begin(GtkPrintOperation *op, GtkPrintContext *ctx, gpointer user_data)
{
    nd_print_ctx *pc = user_data;
    nd_window *w = nd_window_for_id(pc->w_id);
    if (!w) { gtk_print_operation_set_n_pages(op, 1); pc->n_pages = 1; return; }
    nd_window_ensure_layout(w, nd_layout_viewport());
    if (!w->layout_tree) {
        gtk_print_operation_set_n_pages(op, 1);
        pc->n_pages = 1;
        return;
    }
    double page_w = gtk_print_context_get_width(ctx);
    double page_h = gtk_print_context_get_height(ctx);
    double doc_w  = nd_layout_viewport();
    double doc_h  = w->layout_tree->content_height + 16;
    if (doc_w <= 0 || doc_h <= 0) {
        gtk_print_operation_set_n_pages(op, 1);
        pc->n_pages = 1;
        return;
    }
    pc->scale = page_w / doc_w;
    double header_h = 18.0;
    double footer_h = 18.0;
    pc->page_content_h = (page_h - header_h - footer_h) / pc->scale;
    if (pc->page_content_h < 100) pc->page_content_h = 100;

    GArray *breaks = g_array_new(FALSE, FALSE, sizeof(double));
    nd_print_collect_breakpoints(w->layout_tree, breaks);
    g_array_sort(breaks, nd_print_compare_double);

    GArray *offsets = g_array_new(FALSE, FALSE, sizeof(double));
    double zero = 0.0;
    g_array_append_val(offsets, zero);
    double tolerance = pc->page_content_h * 0.20;
    while (TRUE) {
        double cur = g_array_index(offsets, double, offsets->len - 1);
        if (cur + pc->page_content_h >= doc_h) break;
        double hard = cur + pc->page_content_h;
        double soft = cur + pc->page_content_h - tolerance;
        double next = hard;
        for (guint i = 0; i < breaks->len; i++) {
            double y = g_array_index(breaks, double, i);
            if (y > soft && y <= hard && y > cur + 16) {
                if (y > next - tolerance) next = y;
            }
        }
        if (next <= cur + 16) next = cur + pc->page_content_h;
        g_array_append_val(offsets, next);
    }

    pc->n_pages = (int)offsets->len;
    pc->page_offsets = g_new(double, pc->n_pages);
    for (int i = 0; i < pc->n_pages; i++)
        pc->page_offsets[i] = g_array_index(offsets, double, i);

    g_array_free(breaks, TRUE);
    g_array_free(offsets, TRUE);

    gtk_print_operation_set_n_pages(op, pc->n_pages);
}

static void
nd_on_print_draw_page(GtkPrintOperation *op, GtkPrintContext *ctx,
                      int page_nr, gpointer user_data)
{
    (void)op;
    nd_print_ctx *pc = user_data;
    nd_window *w = nd_window_for_id(pc->w_id);
    if (!w) return;
    cairo_t *cr = gtk_print_context_get_cairo_context(ctx);
    double page_w = gtk_print_context_get_width(ctx);
    double page_h = gtk_print_context_get_height(ctx);

    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    PangoLayout *header = gtk_print_context_create_pango_layout(ctx);
    pango_layout_set_text(header,
                          pc->header_title && *pc->header_title
                          ? pc->header_title : "Nordstjernen", -1);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 9");
    pango_layout_set_font_description(header, fd);
    pango_layout_set_width(header, (int)(page_w * PANGO_SCALE));
    pango_layout_set_ellipsize(header, PANGO_ELLIPSIZE_END);
    cairo_move_to(cr, 0, 2);
    pango_cairo_show_layout(cr, header);
    g_object_unref(header);

    PangoLayout *footer = gtk_print_context_create_pango_layout(ctx);
    char *footer_text = g_strdup_printf("%s   —   Page %d of %d",
        pc->header_url ? pc->header_url : "",
        page_nr + 1, pc->n_pages);
    pango_layout_set_text(footer, footer_text, -1);
    g_free(footer_text);
    pango_layout_set_font_description(footer, fd);
    pango_layout_set_width(footer, (int)(page_w * PANGO_SCALE));
    pango_layout_set_ellipsize(footer, PANGO_ELLIPSIZE_END);
    int fw, fh;
    pango_layout_get_pixel_size(footer, &fw, &fh);
    cairo_move_to(cr, 0, page_h - fh - 2);
    pango_cairo_show_layout(cr, footer);
    g_object_unref(footer);
    pango_font_description_free(fd);
    cairo_set_line_width(cr, 0.3);
    cairo_move_to(cr, 0, 16);
    cairo_line_to(cr, page_w, 16);
    cairo_move_to(cr, 0, page_h - 16);
    cairo_line_to(cr, page_w, page_h - 16);
    cairo_stroke(cr);
    cairo_restore(cr);

    if (!w->layout_tree) return;

    double offset = (pc->page_offsets && page_nr >= 0 && page_nr < pc->n_pages)
        ? pc->page_offsets[page_nr]
        : (double)page_nr * pc->page_content_h;

    cairo_save(cr);
    cairo_translate(cr, 0, 18.0);
    cairo_rectangle(cr, 0, 0, page_w, page_h - 36.0);
    cairo_clip(cr);
    cairo_scale(cr, pc->scale, pc->scale);
    cairo_translate(cr, 0, -offset);
    nd_paint(cr, w->layout_tree, NULL);
    cairo_restore(cr);
}

static void
nd_on_print_done(GtkPrintOperation *op, GtkPrintOperationResult result,
                 gpointer user_data)
{
    (void)op;
    nd_print_ctx *pc = user_data;
    nd_window *w = nd_window_for_id(pc->w_id);
    if (w) {
        if (result == GTK_PRINT_OPERATION_RESULT_ERROR) {
            GError *err = NULL;
            gtk_print_operation_get_error(op, &err);
            nd_window_set_status(w, "Print error: %s",
                                 err ? err->message : "unknown");
            g_clear_error(&err);
        } else if (result == GTK_PRINT_OPERATION_RESULT_APPLY) {
            nd_window_set_status(w, "Sent %d page%s to printer",
                                 pc->n_pages, pc->n_pages == 1 ? "" : "s");
        }
    }
    nd_print_ctx_free(pc);
}

void
on_win_print(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    nd_window_ensure_layout(w, nd_layout_viewport());
    if (!w->layout_tree) {
        nd_window_set_status(w, "Nothing to print");
        return;
    }
    nd_print_ctx *pc = g_new0(nd_print_ctx, 1);
    pc->w_id = w->id;
    pc->header_title = nd_window_current_title(w);
    pc->header_url   = g_strdup(nd_window_current_url(w));

    GtkPrintOperation *op = gtk_print_operation_new();
    gtk_print_operation_set_unit(op, GTK_UNIT_POINTS);
    gtk_print_operation_set_use_full_page(op, FALSE);
    gtk_print_operation_set_embed_page_setup(op, TRUE);
    gtk_print_operation_set_show_progress(op, TRUE);
    if (pc->header_title && *pc->header_title)
        gtk_print_operation_set_job_name(op, pc->header_title);
    else
        gtk_print_operation_set_job_name(op, "Nordstjernen page");

    g_signal_connect(op, "begin-print", G_CALLBACK(nd_on_print_begin),     pc);
    g_signal_connect(op, "draw-page",   G_CALLBACK(nd_on_print_draw_page), pc);
    g_signal_connect(op, "done",        G_CALLBACK(nd_on_print_done),      pc);

    GError *err = NULL;
    gtk_print_operation_run(op,
                            GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                            GTK_WINDOW(w->window), &err);
    if (err) {
        nd_window_set_status(w, "Print failed: %s", err->message);
        g_clear_error(&err);
    }
    g_object_unref(op);
}
